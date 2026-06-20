// SPDX-License-Identifier: GPL-2.0
/*
 * cmdlist.c - open XPE cmdlist (byte-code) builder for the BCM4916 XRDP Runner.
 *
 * CLEAN-ROOM emitter: emits 16-bit BIG-ENDIAN command words per the RE'd opcode
 * encoding (opcode = word>>26, see re-notes/xrdp-offload-abi.md sec 2.1/2.2). No
 * proprietary cmdlist.ko code is copied; only the documented bit layout is used.
 *
 * Phase 1 implements the L2/VLAN primitives (REPLACE-bits for VLAN VID/PCP edit,
 * INSERT/DELETE for VLAN push/pop, and the NOP terminator). The Phase-2 L3/NAT
 * primitives are present as stubs so addIpv4Commands-style sequences can slot in
 * later (xrdp-offload-abi.md sec 2.4a) without restructuring.
 *
 * COMMAND WORD ENCODING (Phase 1, the bits below bit 26 are the open driver's
 * own packing of the documented operand fields - offset8 / position / width;
 * the exact stock sub-opcode bit math is UNKNOWN #3 in the ABI doc and is
 * re-derivable from xpe_api.o. The QEMU model decodes the SAME packing, so the
 * driver<->model pair is self-consistent; real silicon needs the pinned packing.)
 *
 *   word0 (command): [31:26]=opcode  [25:18]=offset8  [17:13]=position
 *                    [12:8]=width    [7:0]=nbytes (for insert/delete)
 *   word1 (data, when present): the 16-bit immediate (BE)
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

static u32 xpe_pack_cmd(u8 opcode, u8 offset8, u8 position, u8 width, u8 nbytes)
{
	return ((u32)(opcode & 0x3f) << 26) |
	       ((u32)offset8 << 18) |
	       ((u32)(position & 0x1f) << 13) |
	       ((u32)(width & 0x1f) << 8) |
	       ((u32)nbytes);
}

/* REPLACE bit-field: VLAN VID/PCP edit, ToS mangle (sec 2.3 replace_bits_16). */
void xpe_cmd_replace_bits_16(struct xpe_cmdlist *cl, u8 offset, u8 position,
			     u8 width, u16 data16)
{
	xpe_emit_cmd32(cl, xpe_pack_cmd(XPE_OP_REPLACE, offset, position,
					width, 0));
	xpe_emit16(cl, data16);		/* inline operand data (.data region) */
}

/* INSERT bytes (VLAN push / header expand; sec 2.3 insert_16 / sop_push). */
void xpe_cmd_insert_16(struct xpe_cmdlist *cl, u8 offset, u8 nbytes)
{
	xpe_emit_cmd32(cl, xpe_pack_cmd(XPE_OP_MCOPY, offset, 0, 0, nbytes));
}

/* DELETE bytes (VLAN pop / header shrink; sec 2.3 delete_16 / sop_pull). */
void xpe_cmd_delete_16(struct xpe_cmdlist *cl, u8 offset, u8 nbytes)
{
	xpe_emit_cmd32(cl, xpe_pack_cmd(XPE_OP_MOVE, offset, 0, 0, nbytes));
}

/* Terminator / NOP (sec 2.3 xpe_cmd_end; NOP=0x3f observed live as 0x3f00). */
void xpe_cmd_end(struct xpe_cmdlist *cl)
{
	xpe_emit16(cl, (u16)XPE_OP_NOP << 10);	/* 0x3f00: NOP in the high bits */

	/* pad to a 4-byte multiple (sec 2.1: total size asserted % 4 == 0) */
	while (cl->len & 0x3)
		xpe_emit16(cl, 0);
}

/* ------------------------------------------------------------------------- *
 * Phase-2 stubs (L3/NAT). Declared so addIpv4Commands-style sequences slot in
 * later; intentionally NOT implemented in Phase 1. (xrdp-offload-abi.md 2.4a.)
 * ------------------------------------------------------------------------- */
void xpe_cmd_decrement_8(struct xpe_cmdlist *cl, u8 offset)
{
	/* Phase 2: ADD(-1) on the IPv4 TTL / IPv6 hop-limit byte. */
}

void xpe_cmd_replace_32(struct xpe_cmdlist *cl, u8 offset, u32 data32)
{
	/* Phase 2: replace a full 32-bit IP SA/DA (SNAT/DNAT). */
}

void xpe_cmd_apply_icsum_16(struct xpe_cmdlist *cl, u8 offset)
{
	/* Phase 2: incremental ones-complement checksum fixup after a replace. */
}
