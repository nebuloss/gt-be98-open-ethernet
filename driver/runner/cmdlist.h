/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cmdlist.h - open XPE (XRDP Packet Engine) cmdlist builder for BCM4916.
 *
 * The XPE runs a compact per-flow byte-code ("cmdlist") on egress. This is a
 * CLEAN-ROOM re-implementation of the emitter, derived from the RE'd opcode
 * encoding (no proprietary code copied):
 *
 *   - re-notes/xrdp-offload-abi.md sec 2.1 : command word = opcode in bits[31:26]
 *     (command_word >> 26), words are 16-bit BIG-ENDIAN, laid out in a .text
 *     (command words) + .data (inline operand data) region, 4-byte aligned.
 *   - sec 2.2 : opcode table (CMP_JMP=1 .. REPLACE=0x18 .. NOP=0x3f).
 *   - sec 2.3 : emitter API (replace_bits_16, insert_16, delete_16, ...).
 *   - sec 2.4b : the L2-bridge compiled profile (VLAN edit + sop push/pull).
 *   - re-notes/live-flow-dump.md sec 3 : a REAL cmdlist byte stream from the
 *     stock stack (16-bit BE words, opcodes 0x18 / 0x3f observed live), plus the
 *     TWO length fields the context carries: cmd_list_data_length (bytes of
 *     command data) and cmd_list_length (the larger, aligned figure).
 *
 * SCOPE: Phase 1 = L2 + VLAN; Phase 2 = L3 routing + NAT/NAPT. The Phase-2
 * emitter primitives (decrement_8 for TTL, replace_32 for IP SA/DA, replace_16
 * for L4 port, apply_icsum_16 for IP+L4 checksum) implement the RE'd
 * addIpv4Commands sequence (xrdp-offload-abi.md sec 2.4a).
 */
#ifndef _BCM4916_CMDLIST_H_
#define _BCM4916_CMDLIST_H_

#include <linux/types.h>

/* XPE opcode values, bits[31:26] of the (host-order) 32-bit command word.
 * Pinned in xrdp-offload-abi.md sec 2.2. */
#define XPE_OP_CMP_JMP	0x01
#define XPE_OP_JMPCOND	0x02
#define XPE_OP_JMPREG	0x03
#define XPE_OP_MCOPY	0x13
#define XPE_OP_REPLACE	0x18
#define XPE_OP_GDMA	0x1c
#define XPE_OP_ADD	0x1a	/* ADD immediate to a field; decrement_8 = ADD(-1).
				 * INFER (ABI sec 2.2 "ADD ~0x18 group"); the open
				 * driver + QEMU model agree on this value. */
#define XPE_OP_MOVE	0x2c
#define XPE_OP_ICSUM	0x36
/*
 * 0x3f is the opcode-switch "default" (csel fallback) in xpe_api_opcode_name -
 * it is NOT used as a list terminator. The stock list is length-delimited by
 * cmd_list_data_length (xpe_cmd_end emits no NOP word; see cmdlist.c). Kept for
 * completeness / defensive decoding only.
 */
#define XPE_OP_NOP	0x3f

/*
 * Max cmdlist body in bytes. The live stock entry used cmd_list_data_length=28
 * with the context-entry command_list[] field being larger (cmd_list_length=40,
 * RDP-impl2 template reserves command_list[80]). We cap generously.
 */
#define XPE_CMDLIST_MAX_BYTES	80

/*
 * cmdlist builder state. Command words and inline operand data are emitted as
 * 16-bit BIG-ENDIAN values into a single byte buffer (the live dump shows the
 * .text and .data regions packed contiguously in the context's command_list[]).
 *
 * We track two lengths to match the context entry's TWO length fields
 * (live-flow-dump.md "CORRECTS"):
 *   data_len  -> cmd_list_data_length (bytes actually emitted)
 *   len_field -> cmd_list_length (data_len rounded up / aligned; the unit the
 *                Runner uses to walk the program)
 */
struct xpe_cmdlist {
	u8	buf[XPE_CMDLIST_MAX_BYTES];
	u16	len;		/* bytes emitted so far (incl. trailing 0xfc pad) */
	u16	data_len;	/* executable length (excl. pad); set by xpe_cmd_end.
				 * The stock list is length-delimited by this value -
				 * there is NO NOP terminator (xpe_api.o xpe_cmd_end). */
	bool	overflow;	/* set if an emit would exceed XPE_CMDLIST_MAX_BYTES */
};

/* --- low-level: emit one 16-bit big-endian word --- */
void xpe_emit16(struct xpe_cmdlist *cl, u16 word);

/* --- Phase-1 L2/VLAN emitter primitives (xrdp-offload-abi.md sec 2.3) --- */

/*
 * REPLACE bits: overwrite a bit-field at byte 'offset' in the packet, bit
 * 'position', 'width' bits wide, with 'data16'. Used for VLAN VID/PCP edit and
 * ToS mangle. Opcode XPE_OP_REPLACE. Emits a command word + an inline data word.
 */
void xpe_cmd_replace_bits_16(struct xpe_cmdlist *cl, u8 offset, u8 position,
			     u8 width, u16 data16);

/*
 * INSERT bytes at SOP-relative 'offset' (header expand). Used for VLAN_PUSH:
 * make room for the 4-byte 802.1Q tag, then replace_bits to write TPID+TCI.
 * Modelled as a sop_push_replace (xrdp-offload-abi.md sec 2.3/2.4b).
 */
void xpe_cmd_insert_16(struct xpe_cmdlist *cl, u8 offset, u8 nbytes);

/*
 * DELETE bytes at SOP-relative 'offset' (header shrink). Used for VLAN_POP:
 * strip the 4-byte 802.1Q tag (sop_pull_replace).
 */
void xpe_cmd_delete_16(struct xpe_cmdlist *cl, u8 offset, u8 nbytes);

/*
 * Finalize the program: append the terminator/NOP (xpe_cmd_end). After this the
 * cmdlist is ready to embed in the context entry.
 */
void xpe_cmd_end(struct xpe_cmdlist *cl);

/* --- Phase-2 L3/NAT emitter primitives (xrdp-offload-abi.md sec 2.4a) --- */

/* ADD(-1) on an 8-bit field: IPv4 TTL / IPv6 hop-limit decrement. */
void xpe_cmd_decrement_8(struct xpe_cmdlist *cl, u8 offset);

/* REPLACE a full 32-bit field: IPv4 SA/DA rewrite (SNAT/DNAT). Two data words. */
void xpe_cmd_replace_32(struct xpe_cmdlist *cl, u8 offset, u32 data32);

/* REPLACE a full 16-bit field: L4 source/dest port rewrite (NAPT). One data word. */
void xpe_cmd_replace_16(struct xpe_cmdlist *cl, u8 offset, u16 data16);

/* Incremental ones-complement checksum fixup after a replace/decrement. */
void xpe_cmd_apply_icsum_16(struct xpe_cmdlist *cl, u8 offset);

/* --- builders --- */

/* init an empty cmdlist */
void xpe_cmdlist_init(struct xpe_cmdlist *cl);

/*
 * cmd_list_data_length: executable bytes of the program (matches the live field
 * of the same name). The stock list is length-delimited by this value (no NOP
 * terminator). Valid only after xpe_cmd_end(); before that it falls back to the
 * raw emitted length.
 */
static inline u16 xpe_cmdlist_data_len(const struct xpe_cmdlist *cl)
{
	return cl->data_len ? cl->data_len : cl->len;
}

/*
 * cmd_list_length: the (4-byte-aligned) length the Runner walks. xpe_api.o
 * asserts the total cmdlist size is a multiple of 4 (xrdp-offload-abi.md
 * sec 2.1 "padded to a multiple of 4").
 */
static inline u16 xpe_cmdlist_len(const struct xpe_cmdlist *cl)
{
	return round_up(cl->len, 4);
}

#endif /* _BCM4916_CMDLIST_H_ */
