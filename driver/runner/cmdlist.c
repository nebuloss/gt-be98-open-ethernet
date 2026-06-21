// SPDX-License-Identifier: GPL-2.0
/*
 * cmdlist.c - open XPE cmdlist (byte-code) builder for the BCM4916 XRDP Runner.
 *
 * CLEAN-ROOM emitter: emits 16-bit BIG-ENDIAN command words per the RE'd opcode
 * encoding (opcode = word>>26, see re-notes/xrdp-offload-abi.md sec 2.1/2.2). No
 * proprietary cmdlist.ko code is copied; only the documented bit layout is used.
 *
 * Phase 1 implements the L2/VLAN primitives (REPLACE-bits for VLAN VID/PCP edit,
 * INSERT/DELETE for VLAN push/pop, and the NOP terminator).
 *
 * Phase 2 adds the L3/NAT primitives per the RE'd addIpv4Commands sequence
 * (xrdp-offload-abi.md sec 2.4a): decrement_8 (TTL) -> replace_32 (IP SA/DA NAT)
 * -> apply_icsum_16 (IP csum) -> replace_16 (L4 sport/dport NAPT) ->
 * apply_icsum_16 (L4 csum). The opcodes used are the pinned ADD/REPLACE/ICSUM
 * (sec 2.2). decrement_8 is encoded as ADD(-1) on an 8-bit field; the open
 * driver's ADD opcode value is XPE_OP_ADD (see cmdlist.h) - the exact stock ADD
 * numeric opcode is INFER in the ABI doc (sec 2.2 "ADD ~0x18 group"); the QEMU
 * model decodes the SAME value, so the driver<->model pair is self-consistent.
 *
 * COMMAND WORD ENCODING (the bits below bit 26 are the open driver's own packing
 * of the documented operand fields - offset8 / position / width; the exact stock
 * sub-opcode bit math is UNKNOWN #3 in the ABI doc and is re-derivable from
 * xpe_api.o. The QEMU model decodes the SAME packing, so the driver<->model pair
 * is self-consistent; real silicon needs the pinned packing.)
 *
 *   word0 (command): [31:26]=opcode  [25:18]=offset8  [17:13]=position
 *                    [12:8]=width    [7:0]=nbytes (for insert/delete)
 *   word1 (data, when present): the 16-bit immediate (BE); replace_32 emits TWO
 *                    data words (high half then low half).
 *
 * ENCODING PINNED FROM THE STOCK EMITTER (xpe_api.armb53_6813.o disasm,
 * re-notes/xrdp-offload-abi.md sec 2.5):
 *   - Command words are 32-bit, stored BIG-ENDIAN (sym.__command_add @0x740:
 *     `rev w5, w22 ; str w5, [.text]`). Opcode = word>>26 (bits[31:26]), confirmed
 *     by the live REPLACE word 0x6014eb98 (byte0 0x60 -> 0x60>>2... i.e. top 6
 *     bits = 0x18 = REPLACE) and offset8 in bits[25:18] (live offset 5).
 *   - Byte structure of the 32-bit word (from xpe_cmd_decrement_8 @0x2c90,
 *     xpe_cmd_apply_icsum_16 @0x2da0, xpe_cmd_replace_bits_16 @0x1e60):
 *       byte0 = opcode<<2 | sub-flags   (decrement_8 base 0x6a -> op 0x1a = ADD)
 *       byte1 = "to"   word-count/offset (offset/2 + 1)
 *       byte2 = "from" offset (data-region ref, 0x94-relative; relocated later)
 *               OR, for ops with an inline immediate (icsum16), byte2|byte3 hold
 *               the immediate (icsum base 0x70 with bfxil of icsum16 into [15:0]).
 *       byte3 = 0xff sentinel for "to end" / all-bytes ops (decrement_8).
 *   - TERMINATOR / FRAMING (resolves the old 0x3f00-vs-0xfc00 contradiction):
 *     the list is LENGTH-DELIMITED by cmd_list_data_length. xpe_cmd_end @0x1450
 *     emits NO NOP word; it relocates the "from" offsets, concatenates the
 *     .text+.data regions, and fills the trailing slot slack with the BYTE 0xfc
 *     (loop @0x16e0: `mov w2,0xfc ; str w2,[tail]`). That 0xfc pad is OUTSIDE
 *     cmd_list_data_length and is never decoded. The live "3f 00" in the captured
 *     body is therefore inline data / a relocated from-offset half-word, NOT a
 *     terminator. Our xpe_cmd_end now matches: no NOP word, 0xfc tail pad,
 *     length-delimited execution.
 *   CAVEAT: the captured live body was a GDX-local-delivery program (a path the
 *   open driver does not implement), so this validates the WORD ENCODING and
 *   FRAMING, not a byte-for-byte match of our L2/NAT programs (no L2-accel or
 *   routed-NAT flow was live to capture). The driver<->QEMU-model contract is
 *   kept consistent: the model decodes the same length-delimited, byte-structured
 *   words.
 *
 * ICSUM (incremental ones-complement checksum) carries no inline data: the
 * Runner recomputes the checksum delta from the field(s) the preceding
 * replace/decrement touched. We encode the L4 csum offset in the offset8 field
 * so the model (and real HW) knows which checksum to fix.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include "cmdlist.h"

void xpe_cmdlist_init(struct xpe_cmdlist *cl)
{
	memset(cl, 0, sizeof(*cl));
}

void xpe_emit16(struct xpe_cmdlist *cl, u16 word)
{
	if (cl->len + 2 > XPE_CMDLIST_MAX_BYTES) {
		cl->overflow = true;
		return;
	}
	/* big-endian: high byte first (Runner is BE; xrdp-offload-abi.md sec 2.1) */
	cl->buf[cl->len++] = (word >> 8) & 0xff;
	cl->buf[cl->len++] = word & 0xff;
}

/*
 * Emit a 32-bit command word as two 16-bit BE half-words (the program is a
 * stream of 16-bit BE words; a 32-bit command word is the high half followed by
 * the low half - xrdp-offload-abi.md sec 2.1).
 */
static void xpe_emit_cmd32(struct xpe_cmdlist *cl, u32 cmd)
{
	xpe_emit16(cl, (cmd >> 16) & 0xffff);
	xpe_emit16(cl, cmd & 0xffff);
}

/*
 * SILICON COMMAND-WORD LAYOUT — PINNED BYTE-FOR-BYTE FROM xpe_api.armb53_6813.o
 * (objdump -d, file offsets quoted below). The earlier uniform field split
 * (opcode<<26 | offset8<<18 | position<<13 | width<<8 | nbytes) was WRONG: the
 * stock emitters pack a DIFFERENT 32-bit big-endian word per op family. The two
 * facts common to every family (also live-confirmed):
 *   byte0 = (opcode << 2) | sub-flags     (so opcode = word>>26 = byte0>>2)
 *   byte1 = "to" word-index = (offset >> 1) + 1   [for the offset-based ops]
 * The remaining two bytes are op-specific (see each emitter).
 *
 * NOTE on offsets: the offset-based ops index a 16-bit WORD slot, so the byte
 * offset MUST be even; byte1 = (offset>>1)+1. A 32-bit word lands MSB-first.
 */

/* word-index helper: byte1 = (byte_offset >> 1) + 1  (xpe_api.o, every emitter:
 * `lsr w,offset,#1` then `add w,#1`, e.g. replace_bits_16 @0x1e64/0x1eb8). */
static inline u8 xpe_to_idx(u8 offset)
{
	return (u8)((offset >> 1) + 1);
}

/*
 * REPLACE bit-field: VLAN VID/PCP edit, ToS mangle.
 * PINNED from sym.xpe_cmd_replace_bits_16 @0x1e20:
 *   base word 0x50000000  -> byte0 = 0x50  (opcode 0x14, "replace-bits")
 *   byte1 = (offset>>1)+1                              (bfi #16,#8)
 *   byte2 = data-region "from" ref, 0x94-relative      (bfi #8,#8; relocated
 *           later by xpe_cmd_end — for our flat single-buffer emit we point it
 *           at the inline data word that immediately follows)
 *   byte3 = (position & 0xf) | (((width-1)+position) << 4)   (the pos/width nibble pack)
 *   inline data word = data16 << position               (strh, rev16 in __command_add)
 * (Was: XPE_OP_REPLACE=0x18 with the generic pack -> byte0 would have been 0x60,
 *  wrong op, and position/width were in the wrong bits.)
 */
void xpe_cmd_replace_bits_16(struct xpe_cmdlist *cl, u8 offset, u8 position,
			     u8 width, u16 data16)
{
	u8 b0 = 0x50;					/* opcode 0x14 << 2 */
	u8 b1 = xpe_to_idx(offset);
	u8 b2 = 0x94;					/* .data "from" base (pre-reloc) */
	u8 b3 = (u8)((position & 0xf) |
		     ((((width - 1) + position) & 0xf) << 4));

	xpe_emit_cmd32(cl, ((u32)b0 << 24) | ((u32)b1 << 16) |
			   ((u32)b2 << 8) | b3);
	xpe_emit16(cl, (u16)(data16 << position));	/* inline operand (.data) */
}

/*
 * MOVE-packet (shift packet bytes) — the primitive behind VLAN push/pop.
 * PINNED from sym.__cmd_move_packet @0xd30:
 *   base word 0x4c000000  -> byte0 = 0x4c  (opcode 0x13)
 *   byte1 = from  (RAW byte offset, NOT /2)             (bfi #16,#8)
 *   byte2 = to    (RAW byte offset)                     (bfi #8,#8)
 *   byte3[6:0] = nbytes                                 (bfxil #0,#7)
 *   no inline data.
 * VLAN PUSH = move_packet(from=12,to=12+4) to open a 4-byte hole, then
 * replace_bits to write TPID+TCI. VLAN POP = move_packet(from=16,to=12) only.
 * (stock uses sop_push_replace/sop_pull_replace which wrap this in a GDMA SOP
 *  descriptor; for the in-place L2 tag at offset 12 the plain move_packet word
 *  is the equivalent primitive and is what the model decodes.)
 */
void xpe_cmd_move_packet(struct xpe_cmdlist *cl, u8 from, u8 to, u8 nbytes)
{
	xpe_emit_cmd32(cl, (0x4cU << 24) | ((u32)from << 16) |
			   ((u32)to << 8) | (nbytes & 0x7f));
}

/* INSERT bytes (VLAN push / header expand): open a hole of nbytes at 'offset'
 * by moving the bytes from 'offset' forward by nbytes (caller then writes the
 * new bytes with replace_bits_16). */
void xpe_cmd_insert_16(struct xpe_cmdlist *cl, u8 offset, u8 nbytes)
{
	xpe_cmd_move_packet(cl, offset, offset + nbytes, nbytes);
}

/* DELETE bytes (VLAN pop / header shrink): move the bytes after the deleted
 * region (offset+nbytes) back to 'offset'. */
void xpe_cmd_delete_16(struct xpe_cmdlist *cl, u8 offset, u8 nbytes)
{
	xpe_cmd_move_packet(cl, offset + nbytes, offset, nbytes);
}

/*
 * Finalize the program.
 *
 * RE-CORRECTED (xpe_api.armb53_6813.o sym.xpe_cmd_end @file-off 0x1450): the
 * stock list is *length-delimited* by cmd_list_data_length - there is NO NOP
 * terminator word. xpe_cmd_end walks the .text command words to relocate the
 * inline-data ("from") offsets, concatenates the .text + .data regions, and then
 * fills the *trailing slack* of the context's command_list[] slot with the byte
 * 0xfc (the loop @0x16e0: `mov w2, 0xfc` -> `str w2, [tail, x0, lsl 2]`). That
 * 0xfc pad lies OUTSIDE cmd_list_data_length, so the Runner never executes it;
 * it is pure slot padding, not an opcode.
 *
 * This corrects the earlier guess that the terminator was a 0x3f<<10 = 0xfc00
 * NOP word: the live stock body (60 14 eb 98 3f 00 ...) carries no 0x3f00 NOP -
 * the "3f 00" there is an inline-data / relocated from-offset half-word, and the
 * list is bounded purely by the data-length field. See re-notes/xrdp-offload-abi.md
 * sec 2.5 (UNKNOWN #3 RESOLVED).
 *
 * Our builder therefore emits NO terminator command word. We only pad the emitted
 * byte buffer up to a 4-byte multiple so the context's command_list[] slot is
 * word-aligned; cmd_list_data_length (xpe_cmdlist_data_len) is recorded BEFORE
 * the pad, and cmd_list_length (xpe_cmdlist_len) is the rounded-up figure. The
 * pad byte is 0xfc to match the stock slot fill (cosmetic - it is excluded from
 * the executed length on both real HW and the QEMU model).
 */
void xpe_cmd_end(struct xpe_cmdlist *cl)
{
	/* record the executable length (length-delimited; excludes pad) */
	cl->data_len = cl->len;

	/* pad the slot to a 4-byte multiple with the stock 0xfc fill byte
	 * (sec 2.1: total slot size asserted % 4 == 0). These bytes are past
	 * data_len and are never decoded as commands. */
	while (cl->len & 0x3) {
		if (cl->len + 1 > XPE_CMDLIST_MAX_BYTES) {
			cl->overflow = true;
			return;
		}
		cl->buf[cl->len++] = 0xfc;
	}
}

/* ------------------------------------------------------------------------- *
 * Phase-2 L3/NAT primitives (xrdp-offload-abi.md sec 2.4a / 2.3).
 *
 * Emitted in the exact stock order by xrdp_build_nat_cmdlist (flow_offload.c):
 *   decrement_8(TTL) -> replace_32(IP SA/DA) -> apply_icsum_16(IP csum)
 *   -> replace_16(L4 sport/dport) -> apply_icsum_16(L4 csum).
 * ------------------------------------------------------------------------- */

/*
 * ADD(-1) on an 8-bit field -> IPv4 TTL / IPv6 hop-limit decrement
 * (xpe_cmd_decrement_8, sec 2.3). No inline data; the delta (-1) is implicit in
 * the opcode. We use XPE_OP_ADD with width=8 to mark an 8-bit ADD field.
 */
void xpe_cmd_decrement_8(struct xpe_cmdlist *cl, u8 offset)
{
	/*
	 * PINNED from sym.xpe_cmd_decrement_8 @0x2c50:
	 *   base 0x64000000; byte0 forced to 0x6a  (opcode 0x1a = ADD, sub-flag 0b10)
	 *   byte1 = (offset>>1)+1                         (bfi #16,#8)
	 *   byte2 = (offset>>1)+1   (same word index)     (bfi #8,#8)
	 *   byte3 = 0xff sentinel ("all bytes / to end")  (orr ...,#0xff)
	 *   no inline data; the -1 delta is implicit in the opcode.
	 * (odd offsets set byte0 bits[25:24]=1 -> 0x6a|..; even offset path = 0x6a.)
	 */
	u8 b1 = xpe_to_idx(offset);

	xpe_emit_cmd32(cl, (0x6aU << 24) | ((u32)b1 << 16) |
			   ((u32)b1 << 8) | 0xff);
}

/*
 * REPLACE a full 32-bit field with an immediate (xpe_cmd_replace_32, sec 2.3):
 * IPv4 SA or DA rewrite for SNAT/DNAT. The command word marks a 32-bit replace
 * (width=32 clamps to 0 in the 5-bit width field, so we set width=0 and nbytes=4
 * to mean "4-byte replace"); the 32-bit immediate follows as TWO 16-bit BE data
 * words (high half first, low half second).
 */
void xpe_cmd_replace_32(struct xpe_cmdlist *cl, u8 offset, u32 data32)
{
	/*
	 * PINNED from sym.xpe_cmd_replace_32 @0x1da0 -> __cmd_replace @0x1290:
	 *   byte0 = 0x60  (opcode 0x18 = REPLACE; live-confirmed: live W0 byte0=0x60)
	 *   byte1 = (offset>>1)+1                          (= w19 in __cmd_replace)
	 *   byte2 = data-region "from" ref (0x94-relative; relocated by xpe_cmd_end)
	 *   byte3 = nbytes/sentinel slot for the replace
	 *   data appended to .data as words32 (rev'd 32-bit). For our flat single
	 *   buffer we emit the 4 immediate bytes right after the command word.
	 * nbytes field encodes a 4-byte (2-word) replace: __cmd_replace gets w1 =
	 * 2*words = 4 here (replace_32 passes words*2). We mark byte3 = 4.
	 */
	u8 b1 = xpe_to_idx(offset);

	xpe_emit_cmd32(cl, (0x60U << 24) | ((u32)b1 << 16) | (0x94U << 8) | 0x04);
	xpe_emit16(cl, (data32 >> 16) & 0xffff);	/* high half (BE) */
	xpe_emit16(cl, data32 & 0xffff);		/* low  half (BE) */
}

/*
 * REPLACE a full 16-bit field with an immediate (xpe_cmd_replace_16, sec 2.3):
 * L4 source/destination port rewrite for NAPT. nbytes=2 marks a 2-byte replace;
 * the 16-bit immediate follows as one BE data word.
 */
void xpe_cmd_replace_16(struct xpe_cmdlist *cl, u8 offset, u16 data16)
{
	/* PINNED from sym.xpe_cmd_replace_16 @0x1d20 -> __cmd_replace @0x1290:
	 * byte0 = 0x60 (REPLACE), byte1 = (offset>>1)+1, byte2 = 0x94 .data ref,
	 * byte3 = 2 (2-byte replace); 16-bit immediate appended to .data. */
	u8 b1 = xpe_to_idx(offset);

	xpe_emit_cmd32(cl, (0x60U << 24) | ((u32)b1 << 16) | (0x94U << 8) | 0x02);
	xpe_emit16(cl, data16);
}

/*
 * Incremental ones-complement checksum fixup (xpe_cmd_apply_icsum_16, sec 2.3),
 * applied after a replace/decrement that changed a checksummed field. No inline
 * data: the Runner recomputes the delta. 'offset' is the byte offset of the
 * 16-bit checksum field to fix (IP header csum, or TCP/UDP csum), so the model
 * (and real HW) knows which checksum to recompute.
 */
void xpe_cmd_apply_icsum_16(struct xpe_cmdlist *cl, u8 offset)
{
	/*
	 * PINNED from sym.xpe_cmd_apply_icsum_16 @0x2d60:
	 *   base 0x70000000  -> byte0 = 0x70  (opcode 0x1c, "apply-icsum")
	 *   byte1 = (offset>>1)+1                          (bfi #16,#8)
	 *   bits[15:0] = the 16-bit csum immediate INLINE in the command word's
	 *                low half (bfxil #0,#16) -- NO separate data word.
	 * Here we have no precomputed delta (the Runner recomputes it from the
	 * fields the preceding replace/decrement touched), so the immediate is 0;
	 * byte1 still tells HW which 16-bit checksum slot to fix.
	 */
	u8 b1 = xpe_to_idx(offset);

	xpe_emit_cmd32(cl, (0x70U << 24) | ((u32)b1 << 16) | 0x0000);
}
