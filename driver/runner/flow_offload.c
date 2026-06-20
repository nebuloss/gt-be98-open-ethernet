// SPDX-License-Identifier: GPL-2.0
/*
 * flow_offload.c - open BCM4916 XRDP L2/VLAN HW flow-offload (Phase 1).
 *
 * Translates a mainline nf_flow_table / TC_SETUP_FT flow into an XPE cmdlist +
 * FC_UCAST_FLOW_CONTEXT_ENTRY and programs a NAT-C connection-table entry, so
 * that the first packet of an L2/bridge+VLAN flow misses NAT-C (traps to the
 * CPU, where conntrack/flowtable resolves it), and every subsequent packet HITs
 * NAT-C in HW and is forwarded by the Runner without touching the A53.
 *
 * Architecture modelled on drivers/net/ethernet/mediatek/mtk_ppe_offload.c
 * (the mainline flowtable HW-offload precedent): a flow_block_cb registered on
 * TC_SETUP_FT, FLOW_CLS_REPLACE/DESTROY/STATS, the FLOW_DISSECTOR_KEY_* parse
 * and the FLOW_ACTION_VLAN_* loop. The HW-specific layer (cmdlist + context +
 * NAT-C key/add) is the open analog of the closed cmdlist.ko / rdpa.ko ucast
 * path (re-notes/xrdp-offload-abi.md sec 1-4).
 *
 * Phase 1 scope: L2 bridge + VLAN (is_l2_accel). L3/NAT is explicitly rejected
 * here (-EOPNOTSUPP) and slots into Phase 2 via the cmdlist.c stubs.
 */

#define pr_fmt(fmt) "bcm4916-offload: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rhashtable.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "bcm4916_runner.h"
#include "flow_offload.h"

/* ======================= cmdlist + context + key builders ================ */

/*
 * Build the L2 cmdlist for a resolved flow. Phase-1 ops only (VLAN edit +
 * push/pop), terminated by NOP. Matches the L2 profile in xrdp-offload-abi.md
 * sec 2.4b (replace_bits_16 + sop push/pull dominate the L2 program).
 *
 * Packet byte offsets (from SOP): the 802.1Q tag sits at offset 12 (after the
 * 12-byte MAC DA+SA); the TCI (PCP[15:13]|DEI[12]|VID[11:0]) is at offset 14.
 */
void xrdp_build_l2_cmdlist(const struct xrdp_flow *f, struct xpe_cmdlist *cl)
{
	xpe_cmdlist_init(cl);

	if (f->vlan_push) {
		u16 tci = ((f->vlan_push_pcp & 0x7) << 13) |
			  (f->vlan_push_vid & 0xfff);

		/* make room for the 4-byte tag at offset 12 (sop push) */
		xpe_cmd_insert_16(cl, 12, VLAN_HLEN);
		/* write TPID (0x8100) at offset 12 */
		xpe_cmd_replace_bits_16(cl, 12, 0, 16, ETH_P_8021Q);
		/* write TCI at offset 14 */
		xpe_cmd_replace_bits_16(cl, 14, 0, 16, tci);
	} else if (f->vlan_pop) {
		/* strip the 4-byte tag at offset 12 (sop pull) */
		xpe_cmd_delete_16(cl, 12, VLAN_HLEN);
	} else if (f->vlan_mangle) {
		/* rewrite only the VID bits [11:0] of the TCI at offset 14 */
		xpe_cmd_replace_bits_16(cl, 14, 0, 12,
					f->vlan_mangle_vid & 0xfff);
	}

	xpe_cmd_end(cl);
}

/* Build the FC_UCAST_FLOW_CONTEXT_ENTRY embedding the cmdlist (is_l2_accel=1). */
void xrdp_build_ctx(const struct xrdp_flow *f, const struct xpe_cmdlist *cl,
		    struct fc_ucast_ctx *ctx)
{
	u16 dlen = xpe_cmdlist_data_len(cl);
	u16 clen = xpe_cmdlist_len(cl);

	memset(ctx, 0, sizeof(*ctx));

	/* flags: pure L2 accelerate, not routed */
	ctx->buf[CTX_OFF_FLAGS] = CTX_FLAG_IS_L2_ACCEL;

	ctx->buf[CTX_OFF_VPORT]     = f->egress_vport & 0xff;
	ctx->buf[CTX_OFF_SERVICE_Q] = f->service_queue_id & 0xff;
	if (f->is_hw_cso)
		ctx->buf[CTX_OFF_IS_HW_CSO] = BIT(0);

	/* embed the cmdlist inline at +16 */
	if (clen > XPE_CMDLIST_MAX_BYTES)
		clen = XPE_CMDLIST_MAX_BYTES;
	memcpy(&ctx->buf[XPE_CTX_CMDLIST_OFF], cl->buf, dlen);

	/* the TWO length fields (live-flow-dump.md "CORRECTS") */
	ctx->buf[CTX_OFF_CMDLIST_DLEN] = dlen;
	ctx->buf[CTX_OFF_CMDLIST_LEN]  = clen;
	ctx->buf[CTX_OFF_VALID]        = 1;

	ctx->len = CTX_OFF_VALID + 1;
}

/*
 * Build the 16-byte masked big-endian NAT-C key from the L2 tuple.
 *
 * Packing (open L2 key class):
 *   w[0] = MAC DA [0..3]
 *   w[1] = MAC DA [4..5] | MAC SA [0..1]
 *   w[2] = MAC SA [2..5]
 *   w[3] = ethertype<<16 | ingress_vport<<4 | (vlan_in present flag)
 *
 * Stored big-endian (ABI sec 1.1: key is rev32 / big-endian). The per-table key
 * mask (all-significant here) would AND-zero unused bytes; for the open L2 table
 * every byte is significant so no masking is applied.
 */
void xrdp_build_key(const struct xrdp_flow *f, struct natc_key *key)
{
	u32 w0, w1, w2, w3;

	w0 = (f->mac_da[0] << 24) | (f->mac_da[1] << 16) |
	     (f->mac_da[2] << 8)  |  f->mac_da[3];
	w1 = (f->mac_da[4] << 24) | (f->mac_da[5] << 16) |
	     (f->mac_sa[0] << 8)  |  f->mac_sa[1];
	w2 = (f->mac_sa[2] << 24) | (f->mac_sa[3] << 16) |
	     (f->mac_sa[4] << 8)  |  f->mac_sa[5];
	w3 = ((u32)be16_to_cpu(f->ethertype) << 16) |
	     ((f->ingress_vport & 0xfff) << 4) |
	     ((f->vlan_in & 0xfff) ? 1 : 0);

	key->w[0] = cpu_to_be32(w0);
	key->w[1] = cpu_to_be32(w1);
	key->w[2] = cpu_to_be32(w2);
	key->w[3] = cpu_to_be32(w3);
}

/* ======================= NAT-C add/del (driver-provided I/O) ============= */
/*
 * The actual PSRAM staging + indirect "add" command is issued by the conduit
 * driver, which owns the ioremapped window. flow_offload.c builds the bytes and
 * calls these driver-exported helpers (declared in bcm4916_runner.h).
 */

/* ======================= flowtable offload glue ========================== */

struct xrdp_flow_entry {
	struct rhash_head	node;
	unsigned long		cookie;
	u32			natc_idx;	/* slot programmed in NAT-C */
	struct natc_key		key;
};

static const struct rhashtable_params xrdp_flow_ht_params = {
	.head_offset	= offsetof(struct xrdp_flow_entry, node),
	.key_offset	= offsetof(struct xrdp_flow_entry, cookie),
	.key_len	= sizeof(unsigned long),
	.automatic_shrinking = true,
};

/*
 * Resolve a TC_SETUP_FT FLOW_CLS_REPLACE into an xrdp_flow. Phase 1 accepts
 * only the L2/bridge case (addr_type 0 with ETH_ADDRS); IPv4/IPv6 5-tuple is
 * rejected (-EOPNOTSUPP) - that is Phase 2. Mirrors mtk_flow_offload_replace's
 * bridge branch (mtk_ppe_offload.c:328-349, FLOW_DISSECTOR_KEY_ETH_ADDRS +
 * KEY_VLAN) and FLOW_ACTION_VLAN_* loop (mtk_ppe_offload.c:374-385).
 */
static int xrdp_parse_flow(struct flow_cls_offload *f, struct xrdp_flow *out,
			   u16 ingress_vport, u16 egress_vport)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct flow_action_entry *act;
	u16 addr_type = 0;
	int i;

	memset(out, 0, sizeof(*out));
	out->ingress_vport = ingress_vport;
	out->egress_vport = egress_vport;
	out->ethertype = htons(ETH_P_IP);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;
	}

	/* Phase 1: only the pure-L2 bridge case (addr_type 0). */
	if (addr_type != 0)
		return -EOPNOTSUPP;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS))
		return -EOPNOTSUPP;
	else {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);
		ether_addr_copy(out->mac_da, match.key->dst);
		ether_addr_copy(out->mac_sa, match.key->src);
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		if (match.key->vlan_tpid != htons(ETH_P_8021Q))
			return -EOPNOTSUPP;
		out->vlan_in = match.key->vlan_id;
	}

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_REDIRECT:
		case FLOW_ACTION_MIRRED:
			/* egress device resolved by the caller -> egress_vport */
			break;
		case FLOW_ACTION_CSUM:
			out->is_hw_cso = true;
			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (act->vlan.proto != htons(ETH_P_8021Q))
				return -EOPNOTSUPP;
			out->vlan_push = true;
			out->vlan_push_vid = act->vlan.vid;
			out->vlan_push_pcp = act->vlan.prio;
			break;
		case FLOW_ACTION_VLAN_POP:
			out->vlan_pop = true;
			break;
		case FLOW_ACTION_VLAN_MANGLE:
			out->vlan_mangle = true;
			out->vlan_mangle_vid = act->vlan.vid;
			break;
		case FLOW_ACTION_MANGLE:
			/* L3/L4 mangle => routed/NAT => Phase 2 */
			return -EOPNOTSUPP;
		default:
			return -EOPNOTSUPP;
		}
	}

	if (!is_valid_ether_addr(out->mac_da) ||
	    !is_valid_ether_addr(out->mac_sa))
		return -EINVAL;

	return 0;
}

static int xrdp_flow_replace(struct xrdp_offload *o, struct flow_cls_offload *f)
{
	struct xrdp_flow flow;
	struct xpe_cmdlist cl;
	struct fc_ucast_ctx ctx;
	struct xrdp_flow_entry *entry;
	int err;

	if (rhashtable_lookup_fast(&o->flow_table, &f->cookie,
				   xrdp_flow_ht_params))
		return -EEXIST;

	/*
	 * ingress/egress vport: for the Phase-1 self-test both default to the
	 * conduit. A full DSA integration resolves these from the flow's
	 * ingress/egress netdevs (the FLOW_ACTION_REDIRECT dev).
	 */
	err = xrdp_parse_flow(f, &flow, o->default_vport, o->default_vport);
	if (err)
		return err;

	/* compile the L2 cmdlist + context + key (the open ucast path) */
	xrdp_build_l2_cmdlist(&flow, &cl);
	if (cl.overflow)
		return -E2BIG;
	xrdp_build_ctx(&flow, &cl, &ctx);

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->cookie = f->cookie;
	xrdp_build_key(&flow, &entry->key);

	/* program the NAT-C entry (driver owns the MMIO window) */
	err = xrdp_natc_add(o, &entry->key, &ctx, &entry->natc_idx);
	if (err)
		goto free;

	err = rhashtable_insert_fast(&o->flow_table, &entry->node,
				     xrdp_flow_ht_params);
	if (err)
		goto del;

	pr_info("flow %lx -> NAT-C idx %u: cmdlist %u/%u bytes, ctx %u bytes (is_l2_accel)\n",
		entry->cookie, entry->natc_idx,
		xpe_cmdlist_data_len(&cl), xpe_cmdlist_len(&cl), ctx.len);
	return 0;

del:
	xrdp_natc_del(o, &entry->key, entry->natc_idx);
free:
	kfree(entry);
	return err;
}

static int xrdp_flow_destroy(struct xrdp_offload *o, struct flow_cls_offload *f)
{
	struct xrdp_flow_entry *entry;

	entry = rhashtable_lookup_fast(&o->flow_table, &f->cookie,
				       xrdp_flow_ht_params);
	if (!entry)
		return -ENOENT;

	xrdp_natc_del(o, &entry->key, entry->natc_idx);
	rhashtable_remove_fast(&o->flow_table, &entry->node,
			       xrdp_flow_ht_params);
	kfree(entry);
	return 0;
}

static int xrdp_flow_stats(struct xrdp_offload *o, struct flow_cls_offload *f)
{
	struct xrdp_flow_entry *entry;
	u64 pkts = 0, bytes = 0;

	entry = rhashtable_lookup_fast(&o->flow_table, &f->cookie,
				       xrdp_flow_ht_params);
	if (!entry)
		return -ENOENT;

	/*
	 * Per-flow stats are CNPL counters (ABI sec 3.2). The CNPL counter
	 * read-back is not yet pinned; stub to 0 for now (the flow still ages
	 * out via the flowtable GC timeout). Phase 1.x: wire CNPL read.
	 */
	xrdp_natc_stats(o, entry->natc_idx, &pkts, &bytes);
	flow_stats_update(&f->stats, bytes, pkts, 0, 0,
			  FLOW_ACTION_HW_STATS_DELAYED);
	return 0;
}

static DEFINE_MUTEX(xrdp_flow_mutex);

static int xrdp_flow_cmd(struct xrdp_offload *o, struct flow_cls_offload *cls)
{
	int err;

	mutex_lock(&xrdp_flow_mutex);
	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		err = xrdp_flow_replace(o, cls);
		break;
	case FLOW_CLS_DESTROY:
		err = xrdp_flow_destroy(o, cls);
		break;
	case FLOW_CLS_STATS:
		err = xrdp_flow_stats(o, cls);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&xrdp_flow_mutex);
	return err;
}

static int xrdp_setup_tc_block_cb(enum tc_setup_type type, void *type_data,
				  void *cb_priv)
{
	struct xrdp_offload *o = cb_priv;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	return xrdp_flow_cmd(o, type_data);
}

static LIST_HEAD(xrdp_block_cb_list);

static int xrdp_setup_tc_block(struct xrdp_offload *o, struct net_device *dev,
			       struct flow_block_offload *f)
{
	struct flow_block_cb *block_cb;
	flow_setup_cb_t *cb = xrdp_setup_tc_block_cb;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &xrdp_block_cb_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, cb, o);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}
		block_cb = flow_block_cb_alloc(cb, o, o, NULL);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);
		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &xrdp_block_cb_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, cb, o);
		if (!block_cb)
			return -ENOENT;
		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* ndo_setup_tc entry: TC_SETUP_BLOCK + TC_SETUP_FT (mtk_eth_setup_tc model). */
int xrdp_offload_setup_tc(struct xrdp_offload *o, struct net_device *dev,
			  enum tc_setup_type type, void *type_data)
{
	switch (type) {
	case TC_SETUP_BLOCK:
	case TC_SETUP_FT:
		return xrdp_setup_tc_block(o, dev, type_data);
	default:
		return -EOPNOTSUPP;
	}
}

int xrdp_offload_init(struct xrdp_offload *o)
{
	return rhashtable_init(&o->flow_table, &xrdp_flow_ht_params);
}

static void xrdp_flow_free_one(void *ptr, void *arg)
{
	kfree(ptr);
}

void xrdp_offload_deinit(struct xrdp_offload *o)
{
	rhashtable_free_and_destroy(&o->flow_table, xrdp_flow_free_one, NULL);
}

/*
 * Self-test entry: program one L2 (optionally VLAN) flow into NAT-C directly,
 * exercising the REAL builders (cmdlist + context + key) and the NAT-C add MMIO
 * path - the same code FLOW_CLS_REPLACE drives, minus the TC dissector parse.
 * Used by the QEMU offload proof (debugfs trigger). Returns the cmdlist data
 * length so the caller can log it.
 *
 * vlan_op: 0=plain L2 forward, 1=VLAN push (vid), 2=VLAN pop, 3=VLAN mangle(vid)
 */
int xrdp_offload_selftest(struct xrdp_offload *o,
			  const u8 *mac_da, const u8 *mac_sa,
			  int vlan_op, u16 vid)
{
	struct xrdp_flow flow = {};
	struct xpe_cmdlist cl;
	struct fc_ucast_ctx ctx;
	struct xrdp_flow_entry *entry;
	int err;

	ether_addr_copy(flow.mac_da, mac_da);
	ether_addr_copy(flow.mac_sa, mac_sa);
	flow.ethertype = htons(ETH_P_IP);
	flow.ingress_vport = o->default_vport;
	flow.egress_vport = o->default_vport;

	switch (vlan_op) {
	case 1: flow.vlan_push = true; flow.vlan_push_vid = vid; break;
	case 2: flow.vlan_pop = true; flow.vlan_in = vid; break;
	case 3: flow.vlan_mangle = true; flow.vlan_mangle_vid = vid;
		flow.vlan_in = vid; break;
	default: break;
	}

	xrdp_build_l2_cmdlist(&flow, &cl);
	if (cl.overflow)
		return -E2BIG;
	xrdp_build_ctx(&flow, &cl, &ctx);

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	entry->cookie = (unsigned long)entry;	/* synthetic cookie */
	xrdp_build_key(&flow, &entry->key);

	err = xrdp_natc_add(o, &entry->key, &ctx, &entry->natc_idx);
	if (err) {
		kfree(entry);
		return err;
	}
	err = rhashtable_insert_fast(&o->flow_table, &entry->node,
				     xrdp_flow_ht_params);
	if (err) {
		xrdp_natc_del(o, &entry->key, entry->natc_idx);
		kfree(entry);
		return err;
	}

	pr_info("SELFTEST: programmed NAT-C idx %u (vlan_op=%d vid=%u): cmdlist %u/%u bytes\n",
		entry->natc_idx, vlan_op, vid,
		xpe_cmdlist_data_len(&cl), xpe_cmdlist_len(&cl));
	return xpe_cmdlist_data_len(&cl);
}
