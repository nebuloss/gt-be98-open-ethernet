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
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
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
/*
 * STOCK L2-FORWARD op sequence — DERIVED from the cmdlist_l2_ucast.armb53_6813.o
 * compiler (cmdlist_l2_ucast_create_bin, ordered CALL26 trace) + the per-op
 * encoding pinned from xpe_api.o:
 *
 *   - plain L2 forward, no VLAN change: the cmdlist is (nearly) EMPTY. The
 *     forwarding decision is the CONTEXT (vport / service_queue_id / is_l2_accel),
 *     not the cmdlist. The stock compiler still wraps optional save_16/
 *     compute_csum_16 for a CSO flow, but no header edit is emitted. So our
 *     empty-body + end() is correct for the no-op forward case.
 *   - VLAN PUSH:  move_packet(open 4B hole @12) ; replace the 4-byte tag.
 *                 Stock uses sop_push_replace; the equivalent primitive pair is
 *                 move_packet + a single 32-bit replace of [TPID|TCI].
 *   - VLAN POP:   move_packet(remove 4B @12)  (stock sop_pull_replace).
 *   - VLAN MANGLE (VID/PCP remark, no length change): replace_bits_16 on the TCI
 *     at offset 14 — exactly what the stock compiler emits (replace_bits_16/
 *     copy_bits_16 #9/#17/#18 in the trace).
 *
 * NB the two LIVE captured samples (untagged/VLAN-50) are is_l2_accel=0 CPU/gdx
 * delivery flows, so they pin the ENCODING (byte0=op<<2, byte1=(off>>1)+1, the
 * tag-length operand 8d->91 = +4, tx_adjust -10 vs -6) but NOT this forward op
 * shape; the shape above comes from the L2 compiler's call sequence.
 */
void xrdp_build_l2_cmdlist(const struct xrdp_flow *f, struct xpe_cmdlist *cl)
{
	xpe_cmdlist_init(cl);

	if (f->vlan_push) {
		u16 tci = ((f->vlan_push_pcp & 0x7) << 13) |
			  (f->vlan_push_vid & 0xfff);
		u32 tag = ((u32)ETH_P_8021Q << 16) | tci;

		/* open a 4-byte hole for the tag at offset 12 (move_packet) */
		xpe_cmd_insert_16(cl, 12, VLAN_HLEN);
		/* write the full 4-byte 802.1Q tag (TPID|TCI) at offset 12 with a
		 * single 32-bit replace (byte0 0x60), matching the replace family
		 * the stock emits for a fixed multi-byte header write */
		xpe_cmd_replace_32(cl, 12, tag);
	} else if (f->vlan_pop) {
		/* strip the 4-byte tag at offset 12 (move_packet) */
		xpe_cmd_delete_16(cl, 12, VLAN_HLEN);
	} else if (f->vlan_mangle) {
		/* rewrite only the VID bits [11:0] of the TCI at offset 14
		 * (replace_bits_16: position 0, width 12) */
		xpe_cmd_replace_bits_16(cl, 14, 0, 12,
					f->vlan_mangle_vid & 0xfff);
	}

	xpe_cmd_end(cl);
}

/*
 * Build the L3/NAT cmdlist for a routed flow (Phase 2).
 *
 * Emits the RE'd addIpv4Commands program in the EXACT stock order
 * (xrdp-offload-abi.md sec 2.4a):
 *   decrement_8(TTL)
 *   replace_32(IP SA)  -> if SNAT      ] each followed by the IP-header
 *   replace_32(IP DA)  -> if DNAT      ] incremental-checksum fixup
 *   apply_icsum_16(IP csum)            (once, after the IP addr replaces)
 *   replace_16(L4 sport) -> if SNAPT
 *   replace_16(L4 dport) -> if DNAPT
 *   apply_icsum_16(L4 csum)            (once, after the L4 port replaces)
 *   end (NOP)
 *
 * Stock addIpv4Commands interleaves replace + icsum per field; the open program
 * coalesces the IP-csum and L4-csum fixups to one each (incremental checksum is
 * commutative over the deltas), which is functionally equivalent and shorter.
 * The opcode ORDER (dec-TTL, then IP replace+icsum, then L4 replace+icsum)
 * matches the ABI sequence.
 */
void xrdp_build_nat_cmdlist(const struct xrdp_flow *f, struct xpe_cmdlist *cl)
{
	u8 l4_csum_off;

	xpe_cmdlist_init(cl);

	/* 1. decrement IPv4 TTL (routed) */
	xpe_cmd_decrement_8(cl, IP4_OFF_TTL);

	/* 2. IP SA/DA NAT rewrite (replace_32) */
	if (f->nat_sip)
		xpe_cmd_replace_32(cl, IP4_OFF_SADDR, be32_to_cpu(f->nat_sip_val));
	if (f->nat_dip)
		xpe_cmd_replace_32(cl, IP4_OFF_DADDR, be32_to_cpu(f->nat_dip_val));

	/* 3. IP header checksum fixup (covers TTL dec + IP addr replaces) */
	xpe_cmd_apply_icsum_16(cl, IP4_OFF_CSUM);

	/* 4. L4 source/dest port NAPT rewrite (replace_16) */
	if (f->nat_sport)
		xpe_cmd_replace_16(cl, L4_OFF_SPORT, be16_to_cpu(f->nat_sport_val));
	if (f->nat_dport)
		xpe_cmd_replace_16(cl, L4_OFF_DPORT, be16_to_cpu(f->nat_dport_val));

	/* 5. L4 (TCP/UDP) checksum fixup. The L4 csum covers the pseudo-header
	 *    (IP SA/DA) AND the ports, so it must follow ALL the replaces above.
	 *    Only emitted if a port or IP was rewritten (the pseudo-header
	 *    depends on the IP addrs too). */
	if (f->nat_sport || f->nat_dport || f->nat_sip || f->nat_dip) {
		l4_csum_off = (f->ip_proto == IPPROTO_UDP) ? UDP_OFF_CSUM
							   : TCP_OFF_CSUM;
		xpe_cmd_apply_icsum_16(cl, l4_csum_off);
	}

	xpe_cmd_end(cl);
}

/* Build the FC_UCAST_FLOW_CONTEXT_ENTRY embedding the cmdlist. */
void xrdp_build_ctx(const struct xrdp_flow *f, const struct xpe_cmdlist *cl,
		    struct fc_ucast_ctx *ctx)
{
	u16 dlen = xpe_cmdlist_data_len(cl);
	u16 clen = xpe_cmdlist_len(cl);

	memset(ctx, 0, sizeof(*ctx));

	/* flags: routed (L3/NAT) vs pure L2 accelerate (live-flow-dump.md: a
	 * routed/NAT flow sets is_routed=1; a bridge flow sets is_l2_accel=1). */
	if (f->is_routed) {
		ctx->buf[CTX_OFF_FLAGS] = CTX_FLAG_IS_ROUTED;
		if (f->nat_sip || f->nat_dip || f->nat_sport || f->nat_dport)
			ctx->buf[CTX_OFF_FLAGS] |= CTX_FLAG_IS_NAT;
	} else {
		ctx->buf[CTX_OFF_FLAGS] = CTX_FLAG_IS_L2_ACCEL;
	}

	ctx->buf[CTX_OFF_VPORT]     = f->egress_vport & 0xff;
	ctx->buf[CTX_OFF_SERVICE_Q] = f->service_queue_id & 0xff;
	if (f->is_hw_cso)
		ctx->buf[CTX_OFF_IS_HW_CSO] = BIT(0);

	/* Embed the cmdlist inline at struct byte +24 (XPE_CTX_CMDLIST_OFF, pinned
	 * from the live FC_UCAST capture). Copy the full padded buffer (clen bytes =
	 * executable dlen + the trailing 0xfc slot pad emitted by xpe_cmd_end), so
	 * the context's command_list[] slack matches the stock 0xfc fill. The Runner
	 * executes only the first dlen bytes (length-delimited; 0xfc never decoded). */
	if (clen > XPE_CMDLIST_MAX_BYTES)
		clen = XPE_CMDLIST_MAX_BYTES;
	memcpy(&ctx->buf[XPE_CTX_CMDLIST_OFF], cl->buf, clen);

	/* the TWO length fields (live-flow-dump.md "CORRECTS").
	 * REAL SILICON: the length is carried as command_list_length_32 in 32-bit
	 * WORD units (stock-watch-capture.md sec 2: cmd_list_length=40 -> 0x0a). Our
	 * driver<->model contract stores raw byte counts; a real-HW context builder
	 * must instead write round_up(clen,4)/4 into the command_list_length_32
	 * bitfield of WORD 1. */
	ctx->buf[CTX_OFF_CMDLIST_DLEN] = dlen;
	ctx->buf[CTX_OFF_CMDLIST_LEN]  = clen;	/* model contract: byte count.
						 * real HW: clen/4 in length_32. */
	ctx->buf[CTX_OFF_VALID]        = 1;

	ctx->len = CTX_OFF_VALID + 1;
}

/*
 * Build the 16-byte masked big-endian NAT-C key.
 *
 * Two key classes (the stock stack uses per-table key masks for this; ABI sec
 * 1.1 "key composition is configurable"):
 *
 * L2 (bridge, is_routed=0) - open L2 key class:
 *   w[0] = MAC DA [0..3]
 *   w[1] = MAC DA [4..5] | MAC SA [0..1]
 *   w[2] = MAC SA [2..5]
 *   w[3] = ethertype<<16 | ingress_vport<<4 | (vlan_in present flag)
 *
 * L3/NAT (routed, is_routed=1) - open IPv4 5-tuple key class. The key is the
 * ORIGINAL (pre-NAT) tuple, since the lookup happens on ingress before the
 * cmdlist rewrites the packet (matches the stock ucast key = ingress 5-tuple).
 *
 * Byte layout PINNED from live silicon (re-notes/stock-watch-capture.md sec 1,
 * captured via tools/stock-watch on real stock flows):
 *   key[0..3]   = ORIGINAL source IP (be)         -> w[0]
 *   key[4..7]   = ORIGINAL dest   IP (be)         -> w[1]
 *   key[8..9]   = ORIGINAL src port (be)          -> w[2] high half
 *   key[10..11] = ORIGINAL dst port (be)          -> w[2] low  half
 *   key[12]     = ToS                             -> w[3] byte 0 (MSB)
 *   key[13]     = proto/key-class byte (0x28 obs) -> w[3] byte 1
 *   key[14]     = dir(bit7) | tcp_pure_ack(bit6)  -> w[3] byte 2
 *   key[15]     = ingress-vport / trailer (0x68)  -> w[3] byte 3 (LSB)
 *
 * NB earlier the driver packed ip_proto in the MSB of w[3]; the live capture
 * shows the MSB is ToS, not proto. proto + key-class live in byte 13. The exact
 * proto/vport encoding of bytes 13/15 is per-table (stock used a fixed key-class
 * for the eth0 upstream TCP table); we reproduce the observed constants and pass
 * ToS + the pure-ack/direction flags through, which is what splits the HW flows.
 *
 * Stored big-endian (ABI sec 1.1: key is rev32 / big-endian).
 */
#define NATC_L3_KEY_CLASS_BYTE	0x28	/* key[13]: proto(TCP)+key-class (live) */
#define NATC_L3_KEY_TRAILER	0x68	/* key[15]: ingress-vport/valid trailer (live) */
#define NATC_L3_KEY_DIR_US	BIT(7)	/* key[14] bit7: direction = upstream */
#define NATC_L3_KEY_PURE_ACK	BIT(6)	/* key[14] bit6: tcp_pure_ack */
void xrdp_build_key(const struct xrdp_flow *f, struct natc_key *key)
{
	u32 w0, w1, w2, w3;

	if (f->is_routed) {
		/* L3 IPv4 5-tuple key (original/ingress tuple). Byte layout pinned
		 * from live silicon (re-notes/stock-watch-capture.md sec 1):
		 *   w[3] = ToS<<24 | key-class<<16 | flags<<8 | trailer  */
		u8 k14 = NATC_L3_KEY_DIR_US |
			 (f->tcp_pure_ack ? NATC_L3_KEY_PURE_ACK : 0);

		w0 = be32_to_cpu(f->ip_sa);
		w1 = be32_to_cpu(f->ip_da);
		w2 = ((u32)be16_to_cpu(f->l4_sport) << 16) |
		     be16_to_cpu(f->l4_dport);
		w3 = ((u32)f->ip_tos << 24) |
		     ((u32)NATC_L3_KEY_CLASS_BYTE << 16) |
		     ((u32)k14 << 8) |
		     NATC_L3_KEY_TRAILER;
	} else {
		w0 = (f->mac_da[0] << 24) | (f->mac_da[1] << 16) |
		     (f->mac_da[2] << 8)  |  f->mac_da[3];
		w1 = (f->mac_da[4] << 24) | (f->mac_da[5] << 16) |
		     (f->mac_sa[0] << 8)  |  f->mac_sa[1];
		w2 = (f->mac_sa[2] << 24) | (f->mac_sa[3] << 16) |
		     (f->mac_sa[4] << 8)  |  f->mac_sa[5];
		w3 = ((u32)be16_to_cpu(f->ethertype) << 16) |
		     ((f->ingress_vport & 0xfff) << 4) |
		     ((f->vlan_in & 0xfff) ? 1 : 0);
	}

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
 * Parse a FLOW_ACTION_MANGLE entry into the NAT rewrite fields. Modelled on
 * mtk_flow_mangle_ipv4 (mtk_ppe_offload.c:147-167) and mtk_flow_mangle_ports
 * (mtk_ppe_offload.c:124-145): the conntrack flowtable hands us the rewritten
 * IP addr / L4 port as MANGLE actions, keyed by mangle.htype + mangle.offset.
 */
static int xrdp_parse_mangle(const struct flow_action_entry *act,
			     struct xrdp_flow *out)
{
	u32 val;

	switch (act->mangle.htype) {
	case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
		/* IPv4 address rewrite (mask ignored: full 32-bit replace,
		 * matches mtk_flow_mangle_ipv4 line 164). */
		switch (act->mangle.offset) {
		case offsetof(struct iphdr, saddr):
			out->nat_sip = true;
			memcpy(&out->nat_sip_val, &act->mangle.val, sizeof(__be32));
			break;
		case offsetof(struct iphdr, daddr):
			out->nat_dip = true;
			memcpy(&out->nat_dip_val, &act->mangle.val, sizeof(__be32));
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
	case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
		/* L4 port rewrite. The netfilter mangle word packs src in the
		 * high 16 bits, dst in the low 16 (mtk_flow_mangle_ports
		 * lines 124-145). */
		val = ntohl(act->mangle.val);
		switch (act->mangle.offset) {
		case 0:
			if (act->mangle.mask == ~htonl(0xffff)) {
				out->nat_dport = true;
				out->nat_dport_val = cpu_to_be16(val);
			} else {
				out->nat_sport = true;
				out->nat_sport_val = cpu_to_be16(val >> 16);
			}
			break;
		case 2:
			out->nat_dport = true;
			out->nat_dport_val = cpu_to_be16(val);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

/*
 * Resolve a TC_SETUP_FT FLOW_CLS_REPLACE into an xrdp_flow.
 *
 * Two paths:
 *   - addr_type 0 (no L3): pure-L2 bridge + VLAN (Phase 1).
 *   - FLOW_DISSECTOR_KEY_IPV4_ADDRS: routed L3 + NAT/NAPT (Phase 2). The 5-tuple
 *     is the ORIGINAL/ingress tuple; FLOW_ACTION_MANGLE carries the rewrite.
 *
 * Mirrors mtk_flow_offload_replace: KEY_CONTROL->addr_type (mtk:305-316),
 * KEY_BASIC->l4proto (318-325), KEY_PORTS (407-418), KEY_IPV4_ADDRS (420-429),
 * and the FLOW_ACTION_MANGLE loop (442-466). IPv6 NAT not offloaded (as in mtk).
 */
static int xrdp_parse_flow(struct flow_cls_offload *f, struct xrdp_flow *out,
			   u16 ingress_vport, u16 egress_vport)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct flow_action_entry *act;
	u16 addr_type = 0;
	int i, err;

	memset(out, 0, sizeof(*out));
	out->ingress_vport = ingress_vport;
	out->egress_vport = egress_vport;
	out->ethertype = htons(ETH_P_IP);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		/* ---- Phase 2: routed L3 + NAT/NAPT ---- */
		struct flow_match_ipv4_addrs addrs;
		struct flow_match_ports ports;
		struct flow_match_basic basic;

		out->is_routed = true;

		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC))
			return -EOPNOTSUPP;
		flow_rule_match_basic(rule, &basic);
		out->ip_proto = basic.key->ip_proto;
		if (out->ip_proto != IPPROTO_TCP &&
		    out->ip_proto != IPPROTO_UDP)
			return -EOPNOTSUPP;

		flow_rule_match_ipv4_addrs(rule, &addrs);
		out->ip_sa = addrs.key->src;
		out->ip_da = addrs.key->dst;

		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS))
			return -EOPNOTSUPP;
		flow_rule_match_ports(rule, &ports);
		out->l4_sport = ports.key->src;
		out->l4_dport = ports.key->dst;

		flow_action_for_each(i, act, &rule->action) {
			switch (act->id) {
			case FLOW_ACTION_REDIRECT:
			case FLOW_ACTION_MIRRED:
				break;
			case FLOW_ACTION_CSUM:
				out->is_hw_cso = true;
				break;
			case FLOW_ACTION_MANGLE:
				err = xrdp_parse_mangle(act, out);
				if (err)
					return err;
				break;
			default:
				return -EOPNOTSUPP;
			}
		}
		return 0;
	}

	if (addr_type != 0)
		return -EOPNOTSUPP;	/* IPv6 etc. not offloaded */

	/* ---- Phase 1: pure-L2 bridge + VLAN ---- */
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

	/* compile the cmdlist (routed L3/NAT or L2 bridge) + context + key */
	if (flow.is_routed)
		xrdp_build_nat_cmdlist(&flow, &cl);
	else
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

/*
 * Routed/NAT self-test entry (Phase 2): program one routed IPv4 SNAT+NAPT flow
 * into NAT-C directly, driving the REAL Phase-2 builders (xrdp_build_nat_cmdlist
 * + the is_routed context + the L3 5-tuple key) and the NAT-C add MMIO path -
 * the same code FLOW_CLS_REPLACE drives for a routed flow, minus the TC
 * dissector. Used by the QEMU NAT-offload proof (debugfs trigger), since a true
 * conntrack flowtable trigger needs a 2-port forwarding topology + NF_FLOW_TABLE
 * the single-pipe emulation does not host (see offload-phase2-status.md).
 *
 * The flow spec mirrors a LAN->WAN SNAT masquerade: the ORIGINAL tuple is the
 * match key; nat_sip_val/nat_sport_val are the post-NAT (WAN-side) source.
 * Returns the cmdlist data length.
 */
int xrdp_offload_nat_selftest(struct xrdp_offload *o,
			      __be32 ip_sa, __be32 ip_da,
			      __be16 l4_sport, __be16 l4_dport, u8 ip_proto,
			      __be32 nat_sip, __be16 nat_sport)
{
	struct xrdp_flow flow = {};
	struct xpe_cmdlist cl;
	struct fc_ucast_ctx ctx;
	struct xrdp_flow_entry *entry;
	int err;

	flow.is_routed = true;
	flow.ethertype = htons(ETH_P_IP);
	flow.ingress_vport = o->default_vport;
	flow.egress_vport = o->default_vport;

	/* original/ingress 5-tuple (the NAT-C key) */
	flow.ip_proto = ip_proto;
	flow.ip_sa = ip_sa;
	flow.ip_da = ip_da;
	flow.l4_sport = l4_sport;
	flow.l4_dport = l4_dport;

	/* SNAT + source-NAPT rewrite (the cmdlist) */
	if (nat_sip) {
		flow.nat_sip = true;
		flow.nat_sip_val = nat_sip;
	}
	if (nat_sport) {
		flow.nat_sport = true;
		flow.nat_sport_val = nat_sport;
	}

	xrdp_build_nat_cmdlist(&flow, &cl);
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

	pr_info("NAT-SELFTEST: programmed NAT-C idx %u routed SNAT: %pI4:%u -> [nat %pI4:%u] dst %pI4:%u proto %u, cmdlist %u/%u bytes\n",
		entry->natc_idx, &ip_sa, ntohs(l4_sport),
		&nat_sip, ntohs(nat_sport), &ip_da, ntohs(l4_dport), ip_proto,
		xpe_cmdlist_data_len(&cl), xpe_cmdlist_len(&cl));
	return xpe_cmdlist_data_len(&cl);
}
