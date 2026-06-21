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

/*
 * XPE opcode values = bits[31:26] of the 32-bit command word (= byte0 >> 2).
 *
 * NOW PINNED BYTE-FOR-BYTE from the stock emitters in xpe_api.armb53_6813.o
 * (objdump). The per-op byte0 (and thus the emitted opcode) is:
 *   replace_bits_16   byte0 0x50  -> opcode 0x14   (@0x1e20)
 *   __cmd_move_packet byte0 0x4c  -> opcode 0x13   (@0xd30; VLAN push/pop primitive)
 *   __cmd_replace     byte0 0x60  -> opcode 0x18   (@0x1290; replace_16/_32, LIVE-confirmed)
 *   decrement_8       byte0 0x6a  -> opcode 0x1a (ADD), byte3 0xff   (@0x2c50)
 *   apply_icsum_16    byte0 0x70  -> opcode 0x1c, 16-bit imm in low half (@0x2d60)
 * These supersede the generic >>26 table values below (kept for defensive
 * decoding only). The ACTUAL byte structure is built directly in cmdlist.c.
 *
 * !! QEMU MODEL CONTRACT: qemu/ runner model must be updated to decode this
 *    real byte layout (byte0=op<<2|flags, byte1=(off>>1)+1, op-specific
 *    bytes 2/3) instead of the old uniform offset8<<18|position<<13 split. !!
 */
#define XPE_OP_CMP_JMP	0x01
#define XPE_OP_JMPCOND	0x02
#define XPE_OP_JMPREG	0x03
#define XPE_OP_MCOPY	0x13	/* move-packet (VLAN push/pop), byte0 0x4c */
#define XPE_OP_REPLACE_BITS 0x14 /* replace_bits_16 (VLAN/ToS edit), byte0 0x50 */
#define XPE_OP_REPLACE	0x18	/* replace_16/_32 (full field), byte0 0x60 */
#define XPE_OP_ICSUM	0x1c	/* apply_icsum_16, byte0 0x70 (emitted value) */
#define XPE_OP_ADD	0x1a	/* decrement_8 = ADD(-1), byte0 0x6a, byte3 0xff */
#define XPE_OP_GDMA	0x1c
#define XPE_OP_MOVE	0x2c
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
 * MOVE-packet primitive (opcode 0x13, byte0 0x4c): shift 'nbytes' bytes from
 * 'from' to 'to' within the packet. The building block of VLAN push/pop
 * (pinned from __cmd_move_packet @0xd30). offsets are RAW byte offsets.
 */
void xpe_cmd_move_packet(struct xpe_cmdlist *cl, u8 from, u8 to, u8 nbytes);

/*
 * INSERT bytes at SOP-relative 'offset' (header expand). Used for VLAN_PUSH:
 * make room for the 4-byte 802.1Q tag, then replace_bits to write TPID+TCI.
 * Modelled as a move_packet (stock uses sop_push_replace; the in-place move is
 * the equivalent primitive — xrdp-offload-abi.md sec 2.3/2.4b).
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
