/* SPDX-License-Identifier: GPL-2.0 */
/*
 * flow_offload.h - open BCM4916 XRDP L2/VLAN HW flow-offload (Phase 1).
 *
 * Implements the host side of the offload fast path:
 *   - the FC_UCAST_FLOW_CONTEXT_ENTRY builder (the NAT-C "result"),
 *   - the 16-byte masked big-endian NAT-C key builder + add-path,
 *   - the nf_flow_table / TC_SETUP_FT flowtable offload glue.
 *
 * SOURCES
 *   - re-notes/xrdp-offload-abi.md sec 1.1 (NAT-C add path:
 *       drv_natc_key_result_entry_var_size_ctx_add -> mask key -> key_idx_get ->
 *       eng_key_result_write -> eng_command_write cmd=3),
 *     sec 1.3 (FC_UCAST_FLOW_CONTEXT_ENTRY field set), sec 4.2 (FLOW_ACTION_*
 *     -> cmdlist-op + context-field map).
 *   - re-notes/live-flow-dump.md sec 3 (live context fields: vport/egress,
 *       service_queue_id, is_l2_accel, is_routed, is_hw_cso, cmd_list inline,
 *       cmd_list_data_length + cmd_list_length).
 *   - mtk_ppe_offload.c (mainline precedent for the flowtable offload block):
 *       FLOW_BLOCK_BIND / flow_block_cb / FLOW_CLS_REPLACE|DESTROY|STATS,
 *       FLOW_DISSECTOR_KEY_* parse, FLOW_ACTION_VLAN_* loop.
 *
 * The exact 6813 context-entry BYTE offsets are not pinned (ABI UNKNOWN #1);
 * Phase 1 lays out a structurally-faithful entry (RDP-impl2 template +
 * live-confirmed XRDP fields) that the QEMU model decodes by the SAME contract.
 * Real silicon needs the offsets re-derived from rdpa.ko _ucast_prepare_rdd_*.
 */
#ifndef _BCM4916_FLOW_OFFLOAD_H_
#define _BCM4916_FLOW_OFFLOAD_H_

#include <linux/types.h>
#include <linux/in6.h>
#include "cmdlist.h"

/* ------------------------------------------------------------------------- *
 * NAT-C connection table - shared driver<->QEMU-model CONTRACT.
 *
 * The driver stages the {key, context} into PSRAM at the offsets below and then
 * issues the "add" command via an indirect command register; the model watches
 * those writes and populates its own modelled NAT-C table. These are CONTRACT
 * placeholders (the real RDD/NAT-C indirect-register offsets are ABI UNKNOWN #5
 * / sec 1.1) - they only have to agree between driver and model.
 * ------------------------------------------------------------------------- */

/* NAT-C engine block base, relative to the rdpa window (ABI 1.1: NATC@0x82950000). */
#define XRDP_OFF_NATC			0x00950000UL

/*
 * Indirect add interface (open analog of ag_drv_natc_indir_*). The driver
 * stages the 16-byte key, then the variable-length context, into these PSRAM
 * staging windows, writes the table index, and issues NATC_CMD_ADD.
 *   key staging   : NATC_STAGE_KEY  (16 bytes)
 *   ctx staging   : NATC_STAGE_CTX  (XPE_CTX_ENTRY_MAX bytes)
 *   index + command via NATC_INDIR_*
 */
#define NATC_STAGE_KEY			0x0100	/* PSRAM: 16-byte masked BE key */
#define NATC_STAGE_CTX			0x0120	/* PSRAM: context entry */
#define NATC_INDIR_INDEX		0x0200	/* PSRAM: table index (u32) */
#define NATC_INDIR_CMD			0x0204	/* PSRAM: command reg (u32) */

#define NATC_CMD_ADD			3	/* eng_command_write(...,3) = add */
#define NATC_CMD_DEL			4	/* delete (open extension) */

/* ------------------------------------------------------------------------- *
 * 16-byte NAT-C key (4x u32, masked, big-endian). ABI sec 1.1.
 *
 * Phase-1 L2 key composition (the natc_l2_vlan_key class): {MAC DA, MAC SA,
 * ethertype, ingress vport, VLAN}. The per-table key MASK zeroes the unused
 * bytes; for the open L2 table we use a fixed mask (all key bytes significant).
 *
 * We pack the key from the flow's L2 tuple into 16 bytes. The exact stock byte
 * placement is configurable per table (ABI sec 1.1); the model uses the same
 * packing so lookups match.
 * ------------------------------------------------------------------------- */
struct natc_key {
	__be32	w[4];
};

/* ------------------------------------------------------------------------- *
 * FC_UCAST_FLOW_CONTEXT_ENTRY (the NAT-C result).
 *
 * REAL SILICON LAYOUT (pinned): the GPL FC_UCAST_FLOW_CONTEXT_ENTRY_STRUCT
 * (rdd_data_structures_auto.h, BCM6813, big-endian) is a 124-byte packed
 * BITFIELD struct with command_list[100] at struct byte 24, and the cmdlist
 * length carried in the WORD-1 bitfield `command_list_length_32` (32-bit-word
 * units), NOT a flat cmd_list_data_length byte. The live capture
 * (re-notes/stock-watch-capture.md sec 2) confirms it byte-for-byte
 * (cmdlist body @ +24..+51 of the FC_UCAST struct, gdx_ctx_data @ struct +14).
 *
 * EMULATION CONTRACT (below): the open driver and the QEMU Runner model share a
 * simplified FLAT-BYTE view of the entry so the model can decode what the driver
 * stages without reimplementing the BE bitfield packer. The ONE field that must
 * match real silicon for a real-HW bring-up is the cmdlist offset, which is now
 * 24 (was 16). The remaining CTX_OFF_* are a driver<->model private contract;
 * a real-silicon builder must instead emit the GPL bitfield struct.
 * ------------------------------------------------------------------------- */
#define XPE_CTX_CMDLIST_OFF	24	/* real: command_list @ struct byte 24 */
#define XPE_CTX_ENTRY_MAX	124	/* real: FC_UCAST_FLOW_CONTEXT_ENTRY = 124 B */

struct fc_ucast_ctx {
	u8	buf[XPE_CTX_ENTRY_MAX];
	u16	len;		/* total context bytes */
};

/* context byte offsets (driver<->QEMU-model contract; real layout = GPL
 * bitfield struct, see stock-watch-capture.md sec 2). */
#define CTX_OFF_FLAGS		8	/* byte: bit7 mcast,b5 is_routed,b4 is_l2_accel */
#define  CTX_FLAG_MCAST		BIT(7)
#define  CTX_FLAG_IS_ROUTED	BIT(5)
#define  CTX_FLAG_IS_L2_ACCEL	BIT(4)
#define  CTX_FLAG_IS_NAT	BIT(3)	/* open: NAT/NAPT rewrite present (model hint) */
#define CTX_OFF_VPORT		12	/* byte: egress vport */
#define CTX_OFF_SERVICE_Q	13	/* byte: service_queue_id */
#define CTX_OFF_IS_HW_CSO	14	/* byte: bit0 is_hw_cso */
/* the two cmdlist length fields live AFTER the 100-byte command_list region
 * (contract bytes; real silicon uses command_list_length_32 in WORD 1). */
#define CTX_OFF_CMDLIST_DLEN	(XPE_CTX_CMDLIST_OFF + XPE_CMDLIST_MAX_BYTES + 0)
#define CTX_OFF_CMDLIST_LEN	(XPE_CTX_CMDLIST_OFF + XPE_CMDLIST_MAX_BYTES + 1)
#define CTX_OFF_VALID		(XPE_CTX_CMDLIST_OFF + XPE_CMDLIST_MAX_BYTES + 2)

/* ------------------------------------------------------------------------- *
 * Packet byte offsets (from SOP) for an UNTAGGED IPv4 frame, used by the NAT
 * cmdlist builder + the QEMU interpreter. ETH header = 14 bytes; IPv4 header
 * follows. (Tagged frames shift by VLAN_HLEN; Phase 2 self-test uses untagged.)
 * ------------------------------------------------------------------------- */
#define L2_HLEN			14	/* DA(6)+SA(6)+ethertype(2) */
#define IP4_OFF_TTL		(L2_HLEN + 8)	/* 22: IPv4 TTL byte */
#define IP4_OFF_CSUM		(L2_HLEN + 10)	/* 24: IPv4 header checksum */
#define IP4_OFF_SADDR		(L2_HLEN + 12)	/* 26: IPv4 source address */
#define IP4_OFF_DADDR		(L2_HLEN + 16)	/* 30: IPv4 dest address */
#define IP4_HLEN		20	/* assume no IP options (offload fast path) */
#define L4_OFF_SPORT		(L2_HLEN + IP4_HLEN + 0)	/* 34 */
#define L4_OFF_DPORT		(L2_HLEN + IP4_HLEN + 2)	/* 36 */
#define TCP_OFF_CSUM		(L2_HLEN + IP4_HLEN + 16)	/* 50 */
#define UDP_OFF_CSUM		(L2_HLEN + IP4_HLEN + 6)	/* 40 */

/* resolved flow description handed to the context/cmdlist builder */
struct xrdp_flow {
	/* --- flow class --- */
	bool	is_routed;	/* L3 route + NAT/NAPT (Phase 2) vs pure L2 */

	/* L2 key (Phase 1 bridge path) */
	u8	mac_da[ETH_ALEN];
	u8	mac_sa[ETH_ALEN];
	__be16	ethertype;
	u16	ingress_vport;
	u16	vlan_in;	/* match VID (0 = untagged) */

	/* --- L3/L4 5-tuple key (Phase 2 routed/NAT path) --- */
	u8	ip_proto;	/* IPPROTO_TCP / IPPROTO_UDP */
	u8	ip_tos;		/* match ToS/DSCP (live key byte 12; splits HW flows) */
	bool	tcp_pure_ack;	/* live key byte 14 bit6 (TCP ack-prioritisation flow) */
	__be32	ip_sa;		/* match (original) source IP */
	__be32	ip_da;		/* match (original) dest   IP */
	__be16	l4_sport;	/* match (original) source port */
	__be16	l4_dport;	/* match (original) dest   port */

	/* --- NAT rewrite (post-translation tuple; from FLOW_ACTION_MANGLE) --- */
	bool	nat_sip;	/* rewrite source IP   (SNAT) */
	__be32	nat_sip_val;
	bool	nat_dip;	/* rewrite dest   IP   (DNAT) */
	__be32	nat_dip_val;
	bool	nat_sport;	/* rewrite source port (SNAPT) */
	__be16	nat_sport_val;
	bool	nat_dport;	/* rewrite dest   port (DNAPT) */
	__be16	nat_dport_val;

	/* egress */
	u16	egress_vport;
	u16	service_queue_id;

	/* VLAN actions (Phase 1) */
	bool	vlan_push;
	u16	vlan_push_vid;
	u8	vlan_push_pcp;
	bool	vlan_pop;
	bool	vlan_mangle;
	u16	vlan_mangle_vid;

	bool	is_hw_cso;
};

/* Build the L2 cmdlist for a resolved flow (VLAN push/pop/mangle + end). */
void xrdp_build_l2_cmdlist(const struct xrdp_flow *f, struct xpe_cmdlist *cl);

/*
 * Build the L3/NAT cmdlist for a routed flow (Phase 2): the RE'd addIpv4Commands
 * order (xrdp-offload-abi.md sec 2.4a) - dec-TTL, IP SA/DA replace + IP icsum,
 * L4 sport/dport replace + L4 icsum.
 */
void xrdp_build_nat_cmdlist(const struct xrdp_flow *f, struct xpe_cmdlist *cl);

/* Build the FC_UCAST_FLOW_CONTEXT_ENTRY embedding the cmdlist (is_l2_accel or
 * is_routed per f->is_routed). */
void xrdp_build_ctx(const struct xrdp_flow *f, const struct xpe_cmdlist *cl,
		    struct fc_ucast_ctx *ctx);

/* Build the 16-byte masked big-endian NAT-C key (L2 tuple or L3 5-tuple). */
void xrdp_build_key(const struct xrdp_flow *f, struct natc_key *key);

#endif /* _BCM4916_FLOW_OFFLOAD_H_ */
