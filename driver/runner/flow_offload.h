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

/* NAT-C engine block base, relative to the rdpa window (NATC@0x82950000). */
#define XRDP_OFF_NATC			0x00950000UL

/*
 * Flow-install staging. RE-PINNED sequence (re-notes/re-firmware/01-natc-abi.md
 * §1): mask key -> hash-derived slot -> stage key+result -> command 3. On real
 * silicon the command register is at NAT-C engine +0x10 (busy=bit4, table_id in
 * bits[14:12]) and the key+result is a single 72-byte BE window, with the full
 * 124-byte context in the DDR result table; those ABSOLUTE offsets are UNRESOLVED
 * (§Unresolved: need a live devmem/FDT read). The PSRAM staging windows below are
 * therefore a driver<->QEMU-model CONTRACT that the pinned corrections (hash slot
 * in bcm4916_runner.c, cmd-3 delete) sit on top of.
 */
#define NATC_STAGE_KEY			0x0100	/* PSRAM: 16-byte masked BE key */
#define NATC_STAGE_CTX			0x0120	/* PSRAM: 124-byte context entry */
#define NATC_INDIR_INDEX		0x0200	/* PSRAM: table index (u32) */
#define NATC_INDIR_CMD			0x0204	/* PSRAM: command reg (u32) */

#define NATC_CMD_ADD			3	/* engine command 3 = write/add (§6) */
/* There is NO command 4 (RE 01 §7): delete = a cmd-3 write of an invalidated
 * entry; a whole-table wipe uses the flush modifier below. */
#define NATC_CMD_FLUSH			0x10001	/* cmd 1 | bit16 = flush a table (§6) */

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
 * FC_UCAST_FLOW_CONTEXT_ENTRY (the NAT-C result) — REAL packed-BE layout.
 *
 * RE-PINNED (re-notes/re-firmware/03-fc-ucast-abi.md): the result entry is a
 * 124-byte packed BITFIELD struct. The field positions below are the
 * compiler-emitted bfi operands from the stock rdpa.ko builders
 * (ucast_prepare_rdd_ip_flow_result @0x36d30 routed;
 * l2_ucast_prepare_rdd_ip_flow_result.part.0 @0x32370 L2). The RE names a field
 * "WORD@W bit b" = host byte (W + b/8), bit (b%8) of the native LE word; encoded
 * here as absolute (byte, mask) into the driver's flat buf[], which the QEMU
 * model decodes by the SAME positions.
 *
 *   command_list body      @ struct byte 24 (0x18), rev32 per 32-bit word
 *   command_list_length_32 (byte 7) = (24 + command_list_bytes) / 4   [WORD units,
 *                          counting the 24-byte header]
 *   valid                  = WORD@4 bit23  (byte 6 bit7)  set unconditionally
 *   egress vport           = byte@5 (8-bit)
 *   service_queue_id       = WORD@12 [28:24] (byte 15 [4:0]); enable = byte@21 bit4
 *
 * is_routed vs is_l2_accel is encoded STRUCTURALLY on silicon (which builder runs
 * — RE 03 §3 Unresolved), not a single discriminator bit. For the driver<->model
 * contract we set is_l2_accel in the L2-only flag candidate WORD@4 bit20 (byte 6
 * bit4) so the model can still tell the paths apart. The many low-confidence
 * 1-bit flags (RE 03 §3) are left 0: their positions are certain but their
 * names/semantics are not, and they are not needed for the datapath.
 * ------------------------------------------------------------------------- */
#define XPE_CTX_CMDLIST_OFF	24	/* command_list @ struct byte 24 (RE 03 §1) */
#define XPE_CTX_ENTRY_MAX	124	/* FC_UCAST_FLOW_CONTEXT_ENTRY = 124 B (RE 03 §1) */

struct fc_ucast_ctx {
	u8	buf[XPE_CTX_ENTRY_MAX];
	u16	len;		/* total context bytes (= XPE_CTX_ENTRY_MAX) */
};

/* real packed-BE field positions (RE 03 §1-4), as (byte, mask) into buf[]. */
#define FCU_VALID_BYTE		6	/* WORD@4 bit23 */
#define FCU_VALID_BIT		BIT(7)
#define FCU_L2ACCEL_BYTE	6	/* WORD@4 bit20 (contract is_l2_accel discriminator) */
#define FCU_L2ACCEL_BIT		BIT(4)
#define FCU_CLLEN32_BYTE	7	/* WORD@4 [31:24] = (24 + clen)/4 in 32-bit words */
#define FCU_VPORT_BYTE		5	/* byte@5 = egress vport (8-bit) */
#define FCU_SQ_BYTE		15	/* WORD@12 [28:24] = service_queue_id (5-bit) */
#define FCU_SQ_MASK		0x1f
#define FCU_SQ_EN_BYTE		21	/* byte@21 bit4 = service_queue enable */
#define FCU_SQ_EN_BIT		BIT(4)
#define FCU_HWCSO_BYTE		14	/* byte@14 bit2 = is_hw_cso candidate (RE 03 §3 low-conf) */
#define FCU_HWCSO_BIT		BIT(2)

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
