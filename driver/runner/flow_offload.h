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
 * FC_UCAST_FLOW_CONTEXT_ENTRY (the NAT-C result). ABI sec 1.3 + live dump.
 *
 * Layout follows the RDP-impl2 template (xrdp-offload-abi.md sec 1.3) with the
 * live-confirmed XRDP fields. Multi-byte fields are stored BIG-ENDIAN (Runner
 * is BE) by the builder. The cmdlist is embedded inline at +16.
 * ------------------------------------------------------------------------- */
#define XPE_CTX_CMDLIST_OFF	16
#define XPE_CTX_ENTRY_MAX	(XPE_CTX_CMDLIST_OFF + XPE_CMDLIST_MAX_BYTES + 16)

struct fc_ucast_ctx {
	u8	buf[XPE_CTX_ENTRY_MAX];
	u16	len;		/* total context bytes */
};

/* context byte offsets (template; ABI UNKNOWN #1 for real silicon) */
#define CTX_OFF_FLAGS		8	/* byte: bit7 mcast,b5 is_routed,b4 is_l2_accel */
#define  CTX_FLAG_MCAST		BIT(7)
#define  CTX_FLAG_IS_ROUTED	BIT(5)
#define  CTX_FLAG_IS_L2_ACCEL	BIT(4)
#define CTX_OFF_VPORT		12	/* byte: egress vport */
#define CTX_OFF_SERVICE_Q	13	/* byte: service_queue_id */
#define CTX_OFF_IS_HW_CSO	14	/* byte: bit0 is_hw_cso */
#define CTX_OFF_CMDLIST_DLEN	96	/* u8 : cmd_list_data_length (bytes) */
#define CTX_OFF_CMDLIST_LEN	97	/* u8 : cmd_list_length (aligned) */
#define CTX_OFF_VALID		98	/* u8 : valid flag */

/* resolved Phase-1 flow description handed to the context/cmdlist builder */
struct xrdp_flow {
	/* L2 key */
	u8	mac_da[ETH_ALEN];
	u8	mac_sa[ETH_ALEN];
	__be16	ethertype;
	u16	ingress_vport;
	u16	vlan_in;	/* match VID (0 = untagged) */

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

/* Build the FC_UCAST_FLOW_CONTEXT_ENTRY (is_l2_accel=1) embedding the cmdlist. */
void xrdp_build_ctx(const struct xrdp_flow *f, const struct xpe_cmdlist *cl,
		    struct fc_ucast_ctx *ctx);

/* Build the 16-byte masked big-endian NAT-C key from the L2 tuple. */
void xrdp_build_key(const struct xrdp_flow *f, struct natc_key *key);

#endif /* _BCM4916_FLOW_OFFLOAD_H_ */
