# 03 — cmdlist: the per-flow XPE packet-modification micro-program builder

Audit of `driver/runner/cmdlist.c` and `driver/runner/cmdlist.h` (the open, clean-room
XPE cmdlist emitter for the BCM4916 XRDP "Runner").

Cross-referenced against `re-notes/live-flow-dump.md` (a live read-only capture of a real
stock cmdlist byte stream) and `re-notes/rdpa-offload-controlplane.md` (the RE'd offload
control-plane / cmdlist opcode model). The consumer of this builder,
`driver/runner/flow_offload.c`, and the QEMU decoder `qemu/device-model/bcm4916_runner.c`
are cited where they define the driver↔model contract.

> Scope note: this file is only the **byte emitter**. Which commands to emit, in what
> order, for a given flow is decided by the two callers in `flow_offload.c`
> (`xrdp_build_l2_cmdlist`, `xrdp_build_nat_cmdlist`); those are documented in
> `02-flow-offload.md` but are summarised here because they define the emitter's contract.

---

## 1. Purpose

A **cmdlist** is a compact byte-code "micro-program" that the Runner executes **per flow, on
egress**, to transform an offloaded packet in place: decrement TTL, rewrite IP addresses and
L4 ports for NAT/NAPT, fix up IP/TCP/UDP checksums, push/pop/edit VLAN tags, and (in the
stock stack) build L2/tunnel headers. It is the *action* half of a HW flow-offload entry —
the *match* half is the 16-byte NAT-C key. On a flow **hit**, the Runner runs the flow's
cmdlist entirely in hardware and the A53 CPU is never touched; that is how the device sustains
10G forwarding without CPU load (`re-notes/rdpa-offload-controlplane.md` lines 85-118).

In the stock BCA stack this program is produced by the proprietary `cmdlist.ko` (222 KB,
license Proprietary — `re-notes/rdpa-offload-controlplane.md` line 31). `cmdlist.c` is a
**clean-room re-implementation of the emitter**: it copies no proprietary code, only the RE'd
opcode/byte encoding (`cmdlist.c` lines 1-7). The resulting byte buffer is embedded **inline**
inside the flow's `FC_UCAST_FLOW_CONTEXT_ENTRY` (the NAT-C "result"), at struct byte +24, by
`xrdp_build_ctx()` — confirmed against the live capture where `cmd_list=...` appears inside the
`ucast result={...}` dump (`re-notes/live-flow-dump.md` lines 109-124, 244).

Where it sits in the datapath:

```
flow miss → learn (netfilter flowtable) → xrdp_build_{l2,nat}_cmdlist()  [flow_offload.c]
                                              │  emits XPE byte-code via cmdlist.c
                                              ▼
                                       xrdp_build_ctx()  → FC_UCAST context entry (cmdlist @ +24)
                                              ▼
                                       xrdp_natc_add()  → NAT-C DDR table (key + context)
                                              ▼
flow hit → Runner executes the cmdlist in HW on every subsequent packet (no CPU)
```

The QEMU model implements the Runner side of that last step in `natc_run_cmdlist()`
(`qemu/device-model/bcm4916_runner.c` line 688), decoding the exact bytes this file emits.

---

## 2. Architecture & data flow

### 2.1 The program model

A cmdlist is a stream of **32-bit big-endian command words**, each emitted as **two 16-bit
big-endian half-words** (`cmdlist.c` `xpe_emit_cmd32`, lines 93-97). Some commands carry an
**inline immediate** in the bytes that immediately follow the command word (1 word for
`replace_16`/`replace_bits_16`, 2 words for `replace_32`). The Runner (and the QEMU model)
walk the buffer 4 bytes at a time, decoding `opcode = byte0 >> 2`, then consuming any inline
operand bytes.

The program is **length-delimited**, not terminated. The context entry carries two length
fields (`re-notes/live-flow-dump.md` lines 109, 121-126, 266-271):
- `cmd_list_data_length` — the executable byte count (28 in the live capture);
- `cmd_list_length` — a larger, aligned figure (40 in the live capture).

There is **no NOP/terminator word**. The RE of the stock `xpe_cmd_end` (`cmdlist.c` lines
46-54, 182-206) established that the list is bounded purely by `cmd_list_data_length`, and any
trailing slack in the context's `command_list[]` slot is filled with the pad **byte `0xfc`**,
which lies past the executable length and is never decoded. This corrected an earlier guess
that `0x3f00` (opcode `0x3f`, the switch "default") was a terminator — the live body
`60 14 eb 98 3f 00 ...` carries `3f 00` as inline data / a relocated offset, not a terminator
(`cmdlist.c` lines 46-53, 194-198; `re-notes/live-flow-dump.md` lines 251-255).

### 2.2 The command-word byte layout (pinned from the stock emitter disasm)

The two facts common to every command family, live-confirmed and pinned from the stock
`xpe_api.armb53_6813.o` disassembly (`cmdlist.c` lines 33-45, 100-118):

- `byte0 = (opcode << 2) | sub-flags` ⇒ `opcode = byte0 >> 2 = command_word >> 26`
- for offset-addressed ops, `byte1 = (byte_offset >> 1) + 1` — a **16-bit word index**. The
  helper `xpe_to_idx()` (lines 115-118) computes this. Because it divides the byte offset by
  2, **offsets must be even**; the model recovers `offset = (byte1 - 1) * 2`
  (`bcm4916_runner.c` line 701).

bytes 2 and 3 are op-specific. Per-op layout (the authoritative table is duplicated in
`qemu/device-model/runner-emulation-contract.md` lines 313-320):

| emitter | byte0 | opcode | byte1 | byte2 | byte3 | inline data | effect |
|---|---|---|---|---|---|---|---|
| `xpe_cmd_replace_bits_16` | `0x50` | `0x14` | `(off>>1)+1` | `0x94` (.data ref) | `(pos&0xf) \| (((width-1)+pos)&0xf)<<4` | 2 B = `data16<<pos` | replace `width` bits at `pos` in the 16-bit word @off |
| `xpe_cmd_move_packet` | `0x4c` | `0x13` | `from` (RAW byte off) | `to` (RAW byte off) | `nbytes` (`[6:0]`) | — | move `nbytes`; `to>from`=insert, `from>to`=delete |
| `xpe_cmd_replace_32` | `0x60` | `0x18` | `(off>>1)+1` | `0x94` | `4` | 4 B = 32-bit imm (hi,lo BE) | overwrite 4 B @off |
| `xpe_cmd_replace_16` | `0x60` | `0x18` | `(off>>1)+1` | `0x94` | `2` | 2 B = 16-bit imm (BE) | overwrite 2 B @off |
| `xpe_cmd_decrement_8` | `0x6a` | `0x1a` (ADD) | `(off>>1)+1` | `(off>>1)+1` | `0xff` | — | byte @off -= 1 (TTL) |
| `xpe_cmd_apply_icsum_16` | `0x70` | `0x1c` | `(off>>1)+1` | imm hi (`0`) | imm lo (`0`) | — | IP/L4 incremental checksum fixup @off |

byte0 values 0x50/0x4c/0x60/0x6a/0x70 are each pinned from a named stock symbol offset quoted
in the source (`xpe_cmd_replace_bits_16 @0x1e20`, `__cmd_move_packet @0xd30`, `__cmd_replace
@0x1290`, `xpe_cmd_decrement_8 @0x2c50`, `xpe_cmd_apply_icsum_16 @0x2d60`). The `0x60`/REPLACE
byte0 is additionally **live-confirmed** by the captured word `0x6014eb98` (byte0 `0x60` →
opcode `0x18`; `cmdlist.c` lines 35-38, 266).

> **Important caveat carried in the source itself** (`cmdlist.c` lines 21-25): the byte0
> opcode field and the fact that byte1 is a word-index are pinned, but the *rest* of the
> sub-opcode/operand bit-math below bit 26 (byte2 `.data` reference semantics, the exact
> inline-vs-relocated-data split) is the open driver's **own packing**, matched only to its
> own QEMU model. On real silicon it is UNKNOWN #3 in the ABI doc and must be re-derived from
> `xpe_api.o`. The driver↔model pair is self-consistent; it is *not* proven byte-for-byte
> against silicon for the L2/NAT programs (the only live-captured body was a GDX
> local-delivery program the open driver does not emit — `cmdlist.c` lines 55-60).

### 2.3 Hardware blocks and registers driven

`cmdlist.c` itself touches **no MMIO** — it writes only into an in-memory `struct xpe_cmdlist`
byte buffer. The hardware is driven downstream, by `flow_offload.c`, which embeds the buffer
into the context entry and stages it into the NAT-C engine. For completeness, the relevant
register geometry from the notes and `flow_offload.h`:

- **NAT-C engine block base** `0x82950000` (rdpa-window-relative `0x00950000`,
  `flow_offload.h` line 46; `re-notes/rdpa-offload-controlplane.md` line 229 / the datapath
  ABI). The 16-byte key + variable-length context (with the cmdlist inside it) are staged and
  added via an indirect add interface (`flow_offload.h` lines 48-62 — these staging offsets
  `NATC_STAGE_KEY 0x0100`, `NATC_STAGE_CTX 0x0120`, `NATC_INDIR_INDEX 0x0200`,
  `NATC_INDIR_CMD 0x0204`, and `NATC_CMD_ADD 3` are **driver↔model contract placeholders**,
  not silicon-pinned).
- **cmdlist offset inside the context** `XPE_CTX_CMDLIST_OFF = 24` (`flow_offload.h` line 97) —
  the one context field claimed pinned to real silicon (cmdlist body @ struct +24..+51 in the
  live/stock-watch capture, `flow_offload.h` lines 82-98).

The Runner cores that *execute* the cmdlist are the 8 XRDP microcode cores (`i0..i7`,
`re-notes/live-flow-dump.md` lines 146-156); the cmdlist encoding must match the shipped
microcode generation exactly (`re-notes/rdpa-offload-controlplane.md` lines 289-292).

---

## 3. Data structures

### `struct xpe_cmdlist` — `cmdlist.h` lines 83-90

The builder's entire mutable state; one instance per flow being compiled (stack-allocated by
the callers, e.g. `flow_offload.c` line 500).

| field | type | meaning |
|---|---|---|
| `buf[XPE_CMDLIST_MAX_BYTES]` | `u8[80]` | the emitted byte stream: command words + inline operand data, packed contiguously (the live dump shows `.text`/`.data` packed together in the slot) plus the trailing `0xfc` pad |
| `len` | `u16` | bytes emitted so far, **including** the trailing `0xfc` pad added by `xpe_cmd_end` |
| `data_len` | `u16` | executable length, **excluding** pad; set by `xpe_cmd_end`. The list is length-delimited by this value (→ `cmd_list_data_length`). 0 until `xpe_cmd_end` runs |
| `overflow` | `bool` | set if any emit would exceed `XPE_CMDLIST_MAX_BYTES`; the emit is then dropped |

`data_len` maps to `cmd_list_data_length`, and `round_up(len,4)` to `cmd_list_length`, matching
the two live length fields (`cmdlist.h` lines 72-90; `re-notes/live-flow-dump.md` lines 266-271).

### `#define XPE_CMDLIST_MAX_BYTES 80` — `cmdlist.h` line 70

Cap on the body. Justified by the live entry (`cmd_list_data_length=28`, `cmd_list_length=40`,
RDP-impl2 template reserves `command_list[80]`; `cmdlist.h` lines 65-70). Note the real GPL
`FC_UCAST_FLOW_CONTEXT_ENTRY` reserves `command_list[100]` (`flow_offload.h` line 84), so 80 is
a **conservative** cap, not the true slot size.

### XPE opcode enum — `cmdlist.h` lines 47-63

`opcode = command_word >> 26 = byte0 >> 2`. Values now pinned from the stock emitters:

| macro | value | byte0 | notes |
|---|---|---|---|
| `XPE_OP_CMP_JMP` | `0x01` | — | RE'd from opcode table; **not emitted** by this driver |
| `XPE_OP_JMPCOND` | `0x02` | — | not emitted |
| `XPE_OP_JMPREG` | `0x03` | — | not emitted |
| `XPE_OP_MCOPY` | `0x13` | `0x4c` | move-packet (VLAN push/pop primitive) |
| `XPE_OP_REPLACE_BITS` | `0x14` | `0x50` | replace_bits_16 (VLAN/ToS edit) |
| `XPE_OP_REPLACE` | `0x18` | `0x60` | replace_16/_32 (full field), live-confirmed |
| `XPE_OP_ICSUM` | `0x1c` | `0x70` | apply_icsum_16 |
| `XPE_OP_ADD` | `0x1a` | `0x6a` | decrement_8 = ADD(-1), byte3 `0xff` |
| `XPE_OP_GDMA` | `0x1c` | — | **duplicate value with `XPE_OP_ICSUM`** (see findings); not emitted |
| `XPE_OP_MOVE` | `0x2c` | — | not emitted; not referenced |
| `XPE_OP_NOP` | `0x3f` | — | opcode-switch "default", **not** a terminator; kept for defensive decoding |

Critically, `cmdlist.c` does **not** build words from these macros. The actual byte structure
is hand-assembled per emitter from the pinned `byte0` constants (`cmdlist.h` lines 33-45); the
`XPE_OP_*` macros are kept "for defensive decoding only" and are effectively dead in the emit
path (grep confirms only comment references — see findings).

---

## 4. Function reference (file order)

Two files. Emitters live in `cmdlist.c`; two length accessors are `static inline` in
`cmdlist.h`.

### `xpe_cmdlist_init(struct xpe_cmdlist *cl)` — `cmdlist.c:72`
Zeroes the whole builder state (`memset(cl, 0, sizeof(*cl))`). Resets `buf`, `len`,
`data_len`, `overflow`. Must be called before any emit. **Callers:** `xrdp_build_l2_cmdlist`
(`flow_offload.c:74`), `xrdp_build_nat_cmdlist` (`flow_offload.c:124`). **Callees:** `memset`.
No hardware, no ordering constraint beyond "first".

### `xpe_emit16(struct xpe_cmdlist *cl, u16 word)` — `cmdlist.c:77`
The single low-level primitive: appends one 16-bit value **big-endian** (high byte first) to
`buf`, advancing `len` by 2. If the append would exceed `XPE_CMDLIST_MAX_BYTES` it sets
`cl->overflow = true` and returns **without** writing (lines 79-82). This is the only place BE
ordering is realised — the Runner is big-endian (`cmdlist.c` line 83). Declared public in the
header (`cmdlist.h:93`) though in practice only used internally and by `xpe_emit_cmd32`.
**Callers:** `xpe_emit_cmd32` (all command words), `xpe_cmd_replace_bits_16` (inline operand),
`xpe_cmd_replace_32` (2 inline halves), `xpe_cmd_replace_16` (1 inline half). **Callees:** none.
**Ordering:** the two byte writes must stay high-then-low; any caller emitting a command word
plus inline data must emit the command word first (all do).

### `static void xpe_emit_cmd32(struct xpe_cmdlist *cl, u32 cmd)` — `cmdlist.c:93`
Emits a 32-bit command word as two `xpe_emit16` calls: high half `(cmd>>16)&0xffff` then low
half `cmd&0xffff` (lines 95-96). This is how every command word reaches the buffer.
**Callers:** every `xpe_cmd_*` emitter. **Callees:** `xpe_emit16` ×2. **Ordering:** high half
before low half (a 32-bit BE word is high-half-first in the 16-bit-word stream — `cmdlist.c`
lines 88-92).

### `static inline u8 xpe_to_idx(u8 offset)` — `cmdlist.c:115`
Returns `(offset >> 1) + 1` — the 16-bit **word index** stored in `byte1` of offset-addressed
commands. Pinned from every stock emitter (`lsr w,offset,#1; add w,#1`, e.g. `replace_bits_16
@0x1e64/0x1eb8` — `cmdlist.c` lines 113-114). **Consequence:** the offset must be **even**;
an odd offset loses its low bit (see findings). **Callers:** `xpe_cmd_replace_bits_16`,
`xpe_cmd_decrement_8`, `xpe_cmd_replace_32`, `xpe_cmd_replace_16`, `xpe_cmd_apply_icsum_16`
(every offset-addressed emitter). **Callees:** none.

### `xpe_cmd_replace_bits_16(cl, u8 offset, u8 position, u8 width, u16 data16)` — `cmdlist.c:133`
Overwrites a **bit-field** — `width` bits starting at bit `position` — within the 16-bit word
at byte `offset`. Used for VLAN VID/PCP edit and ToS mangle (the L2 caller uses it to remark
just the 12-bit VID). Encoding (lines 136-144):
- `byte0 = 0x50` (opcode `0x14`)
- `byte1 = xpe_to_idx(offset)`
- `byte2 = 0x94` — the stock `.data` "from" base reference (pre-relocation); in the flat
  single-buffer emit it points at the inline data word that immediately follows, and the model
  ignores it (lines 138, 128-131)
- `byte3 = (position & 0xf) | ((((width-1)+position) & 0xf) << 4)` — the pos/width nibble pack
- inline data word = `data16 << position` (pre-shifted so the model can mask it back)

**Registers/HW:** none directly. **Callers:** `xrdp_build_l2_cmdlist` for VLAN mangle —
`xpe_cmd_replace_bits_16(cl, 14, 0, 12, vid&0xfff)` (`flow_offload.c:93`). **Callees:**
`xpe_to_idx`, `xpe_emit_cmd32`, `xpe_emit16`. **Sequencing:** command word then the single
inline data word (order matters: model reads the operand right after the word). The decoder
recovers `position = byte3 & 0xf`, `width = ((byte3>>4)&0xf) - position + 1`
(`bcm4916_runner.c` lines 769-790).
**Historical note in source:** the earlier generic pack used `XPE_OP_REPLACE=0x18` → byte0
`0x60` (wrong op) with position/width in the wrong bits (lines 128-131); that is now corrected.

### `xpe_cmd_move_packet(cl, u8 from, u8 to, u8 nbytes)` — `cmdlist.c:161`
The move-packet primitive behind VLAN push/pop: shift `nbytes` packet bytes from `from` to
`to`. Encoding is a single command word, **no inline data** (lines 163-164):
`byte0 = 0x4c` (opcode `0x13`), `byte1 = from` (RAW byte offset, **not** `/2`),
`byte2 = to` (RAW), `byte3 = nbytes & 0x7f` (7-bit). Pinned from `__cmd_move_packet @0xd30`.
VLAN PUSH = `move_packet(from=12, to=16)` to open a 4-byte hole then `replace_bits`/`replace_32`
to write the tag; VLAN POP = `move_packet(from=16, to=12)` (lines 154-159). The stock stack
wraps this in `sop_push_replace`/`sop_pull_replace` (GDMA SOP descriptors); for an in-place L2
tag at offset 12 the plain move word is the equivalent primitive the model decodes (lines
156-159). **Callers:** `xpe_cmd_insert_16`, `xpe_cmd_delete_16`. **Callees:** `xpe_emit_cmd32`.
**Decoder:** `to>from` ⇒ insert/open hole; `from>to` ⇒ delete/close hole (`bcm4916_runner.c`
lines 709-729).

### `xpe_cmd_insert_16(cl, u8 offset, u8 nbytes)` — `cmdlist.c:170`
VLAN-push / header-expand helper: `move_packet(offset, offset+nbytes, nbytes)` — opens an
`nbytes` hole at `offset` (caller then writes the new bytes). **Callers:**
`xrdp_build_l2_cmdlist` VLAN push — `xpe_cmd_insert_16(cl, 12, VLAN_HLEN)` (`flow_offload.c:82`).
**Callees:** `xpe_cmd_move_packet`.

### `xpe_cmd_delete_16(cl, u8 offset, u8 nbytes)` — `cmdlist.c:177`
VLAN-pop / header-shrink helper: `move_packet(offset+nbytes, offset, nbytes)` — moves the bytes
after the deleted region back to `offset`. **Callers:** `xrdp_build_l2_cmdlist` VLAN pop —
`xpe_cmd_delete_16(cl, 12, VLAN_HLEN)` (`flow_offload.c:89`). **Callees:** `xpe_cmd_move_packet`.
**Model caveat:** delete removes `(from - to)` bytes; for `delete_16` that equals `nbytes`
(`bcm4916_runner.c` lines 721-727) — i.e. the `nbytes` field is not actually used by the delete
decode, the byte count is derived from the two offsets. Consistent here but a latent coupling.

### `xpe_cmd_end(struct xpe_cmdlist *cl)` — `cmdlist.c:207`
Finalizes the program. It does **not** emit a terminator word. It (1) records the executable
length `cl->data_len = cl->len` (line 210) and (2) pads `buf` up to a 4-byte multiple with the
stock fill **byte `0xfc`** (lines 215-221), so the context slot is word-aligned; the pad lies
past `data_len` and is never decoded. The overflow guard in the pad loop sets `cl->overflow`
and returns early if the pad would exceed the cap (lines 216-219). Pinned from stock
`xpe_cmd_end @0x1450` (lines 46-54, 182-206). **Callers:** `xrdp_build_l2_cmdlist`
(`flow_offload.c:97`), `xrdp_build_nat_cmdlist` (`flow_offload.c:154`). **Callees:** none.
**Ordering:** must be the last emit for a program (it sets `data_len`, which every downstream
length query relies on).
**NOT implemented vs stock:** the real `xpe_cmd_end` also *relocates* the inline-data ("from")
offsets and *concatenates* the `.text` and `.data` regions (lines 185-192). The open emitter
inlines each operand right after its command word instead, so no relocation is done — see
findings.

### `xpe_cmd_decrement_8(struct xpe_cmdlist *cl, u8 offset)` — `cmdlist.c:237`
IPv4 TTL / IPv6 hop-limit decrement, encoded as `ADD(-1)` on an 8-bit field. Single command
word, no inline data; the `-1` delta is implicit in the opcode (lines 250-251):
`byte0 = 0x6a` (opcode `0x1a` = ADD, sub-flag `0b10`), `byte1 = byte2 = xpe_to_idx(offset)`
(same word index in both), `byte3 = 0xff` ("all bytes / to end" sentinel). Pinned from
`xpe_cmd_decrement_8 @0x2c50`. **Source note (lines 246-247):** odd offsets are supposed to set
byte0 bits[25:24]=1 (`0x6a|..`); the even-offset path is plain `0x6a`, and the code **always**
emits `0x6a` — correct only for even offsets (TTL @ 22 is even; see findings). **Callers:**
`xrdp_build_nat_cmdlist` — `xpe_cmd_decrement_8(cl, IP4_OFF_TTL)` (`flow_offload.c:127`).
**Callees:** `xpe_to_idx`, `xpe_emit_cmd32`. **Model:** `frame[offset] -= 1` (`bcm4916_runner.c`
lines 731-735). **Sequencing:** must precede the IP checksum fixup so the icsum covers the TTL
change.

### `xpe_cmd_replace_32(struct xpe_cmdlist *cl, u8 offset, u32 data32)` — `cmdlist.c:261`
Overwrite a full 32-bit field with an immediate — IPv4 SA or DA for SNAT/DNAT, and (reused by
the L2 path) a full 4-byte 802.1Q tag. Command word then **two** inline BE half-words (lines
276-278): `byte0 = 0x60` (opcode `0x18` REPLACE, live-confirmed), `byte1 = xpe_to_idx(offset)`,
`byte2 = 0x94` (`.data` ref), `byte3 = 0x04` (4-byte / 2-word replace); then `data32>>16`
(high half) and `data32&0xffff` (low half). Pinned from `xpe_cmd_replace_32 @0x1da0 →
__cmd_replace @0x1290`. **Callers:** `xrdp_build_nat_cmdlist` for IP SA/DA
(`flow_offload.c:131,133`, passing `be32_to_cpu(f->nat_*_val)` so the immediate is in host
order and re-emitted BE by `xpe_emit16`); `xrdp_build_l2_cmdlist` for the VLAN-push tag
(`flow_offload.c:86`). **Callees:** `xpe_to_idx`, `xpe_emit_cmd32`, `xpe_emit16` ×2. **Model:**
disambiguated from replace_16 by `byte3 == 4`; consumes 4 inline bytes, overwrites 4 @offset
(`bcm4916_runner.c` lines 743-756). **Sequencing:** must precede the IP icsum fixup.

### `xpe_cmd_replace_16(struct xpe_cmdlist *cl, u8 offset, u16 data16)` — `cmdlist.c:286`
Overwrite a full 16-bit field — L4 source/dest port for NAPT. Command word then **one** inline
BE half-word (lines 293-294): `byte0 = 0x60`, `byte1 = xpe_to_idx(offset)`, `byte2 = 0x94`,
`byte3 = 0x02` (2-byte replace); then `data16`. Pinned from `xpe_cmd_replace_16 @0x1d20 →
__cmd_replace @0x1290`. **Callers:** `xrdp_build_nat_cmdlist` for L4 sport/dport
(`flow_offload.c:140,142`, `be16_to_cpu(f->nat_*port_val)`). **Callees:** `xpe_to_idx`,
`xpe_emit_cmd32`, `xpe_emit16`. **Model:** the `else` branch of the REPLACE case (`byte3 != 4`),
consumes 2 inline bytes (`bcm4916_runner.c` lines 757-767). **Sequencing:** must precede the L4
icsum fixup.

### `xpe_cmd_apply_icsum_16(struct xpe_cmdlist *cl, u8 offset)` — `cmdlist.c:304`
Incremental ones-complement checksum fixup, applied after a replace/decrement that changed a
checksummed field. A single command word, **no separate data word** — the 16-bit immediate is
carried inline in the command word's low half (lines 316-318): `byte0 = 0x70` (opcode `0x1c`),
`byte1 = xpe_to_idx(offset)`, low half = `0x0000`. The open driver has no precomputed delta so
the immediate is 0; `byte1` still tells HW which 16-bit checksum slot to fix. Pinned from
`xpe_cmd_apply_icsum_16 @0x2d60`. `offset` is the byte offset of the checksum field (IP header
csum, or TCP/UDP csum). **Callers:** `xrdp_build_nat_cmdlist` — once for the IP csum
(`IP4_OFF_CSUM`, `flow_offload.c:136`) and once for the L4 csum (`TCP_OFF_CSUM`/`UDP_OFF_CSUM`
selected by `f->ip_proto`, lines 148-151). **Callees:** `xpe_to_idx`, `xpe_emit_cmd32`.
**Model:** distinguishes IP vs L4 by comparing `offset` to `IP4_OFF_CSUM` and **recomputes**
the whole checksum rather than applying a delta (`bcm4916_runner.c` lines 736-742;
`runner-emulation-contract.md` lines 341-343) — functionally identical for a well-formed frame.
**Sequencing (critical):** the IP icsum must follow the TTL-dec + IP SA/DA replaces; the L4
icsum must follow **all** replaces because the TCP/UDP checksum covers the pseudo-header (IP
SA/DA) as well as the ports (`flow_offload.c` lines 144-152).

### `static inline u16 xpe_cmdlist_data_len(const struct xpe_cmdlist *cl)` — `cmdlist.h:157`
Returns `cl->data_len ? cl->data_len : cl->len` — the executable byte count
(`cmd_list_data_length`). Falls back to the raw emitted length before `xpe_cmd_end` has set
`data_len`. **Callers:** `xrdp_build_ctx` (`flow_offload.c:161`), plus logging/self-test paths
(`flow_offload.c:546,757,758,834,835`).

### `static inline u16 xpe_cmdlist_len(const struct xpe_cmdlist *cl)` — `cmdlist.h:167`
Returns `round_up(cl->len, 4)` — the 4-byte-aligned length the Runner walks (`cmd_list_length`).
**Callers:** `xrdp_build_ctx` (`flow_offload.c:162`), logging paths.

---

## 5. Audit findings

Severity tags are the auditor's; `file:line` is exact.

1. **[high — real-HW blocker] `.text`/`.data` relocation is not implemented; byte2 `0x94`
   references are dead.** Every offset-addressed emitter writes `byte2 = 0x94`, the stock
   pre-relocation `.data` "from" reference (`cmdlist.c:138,267,289,276,293`), but the open
   `xpe_cmd_end` (`cmdlist.c:207`) only records the length and pads with `0xfc` — it does **not**
   relocate those references or concatenate a separate `.data` region, which the stock
   `xpe_cmd_end @0x1450` does (`cmdlist.c:185-192`). The open driver instead inlines each operand
   immediately after its command word, and the QEMU model reads it there. On **real silicon the
   `0x94` byte2 would be interpreted as a `.data` offset**, so the current buffers will not run
   on hardware unmodified. Self-consistent driver↔model only. Explicitly acknowledged in-source
   but is the single largest reimplementation gap.

2. **[high — unverified vs silicon] the operand bit-packing below the opcode field is the open
   driver's own invention.** `cmdlist.c:21-25,100-111` state that only `byte0 = opcode<<2` and
   `byte1 = (off>>1)+1` are pinned; the byte2/byte3 semantics, the inline-vs-relocated data
   split, the `replace_32`/`replace_16` disambiguation by `byte3==4`, and the `replace_bits`
   pos/width nibble pack are all UNKNOWN #3 in the ABI and are matched only to the QEMU model.
   Must be re-derived from `xpe_api.o` before any silicon bring-up.

3. **[high — encoding not validated for the emitted programs] no live L2/NAT cmdlist was ever
   captured.** The only live body (`re-notes/live-flow-dump.md:109-124`) is a `is_routed=0,
   is_l2_accel=0` GDX local-delivery program — a path this driver does not emit. It validates the
   word encoding and framing but **not** a byte-for-byte match of the driver's VLAN or NAT
   programs (`cmdlist.c:55-60`; `re-notes/live-flow-dump.md:289-295`). A routed/NAT and an
   L2-accel flow must be captured on silicon to close this.

4. **[medium] `decrement_8` ignores offset parity — always emits `byte0 = 0x6a`.** The source
   itself notes odd offsets should set byte0 bits[25:24] (`cmdlist.c:246-247`), but
   `xpe_cmd_decrement_8` (`cmdlist.c:250`) hardcodes `0x6a`. Correct only for even offsets. The
   sole caller uses `IP4_OFF_TTL = 22` (even, `flow_offload.h:127`), so it is currently safe, but
   the function is wrong for odd-offset use (e.g. an IPv6 hop-limit at an odd SOP offset in a
   tagged frame).

5. **[medium] all offset-addressed emitters silently round odd offsets down.** `xpe_to_idx`
   (`cmdlist.c:115`) computes `(offset>>1)+1`, discarding the low bit; the decoder reconstructs
   `(byte1-1)*2` (`bcm4916_runner.c:701`), so an odd `offset` targets the even byte below with no
   error. All current callers pass even offsets (TTL 22, IP csum 24, SA 26, DA 30, ports 34/36,
   L4 csum 40/50 — `flow_offload.h:127-135`), but tagged-frame paths (which shift every L3/L4
   offset by `VLAN_HLEN=4`, still even) or any future odd offset would misfire silently. No
   assertion guards evenness.

6. **[medium] `replace_bits_16` pos/width nibble pack overflows silently for wide/high fields.**
   `byte3` packs `position` in the low nibble and `(width-1)+position` in the high nibble, each
   masked `& 0xf` (`cmdlist.c:139-140`). If `(width-1)+position > 15` the high nibble wraps and
   the decoded `width` is wrong. The only caller uses `position=0, width=12` → high nibble `0xb`
   (safe, `flow_offload.c:93`), but a 16-bit field at a non-zero position, or ToS mangles at
   arbitrary positions, could overflow with no diagnostic. Also `width == 0` would make `width-1`
   underflow to `0xff`.

7. **[low] duplicate opcode macro value: `XPE_OP_ICSUM` and `XPE_OP_GDMA` are both `0x1c`**
   (`cmdlist.h:53,55`). Neither is used to build a word (emitters use raw `byte0`), so there is no
   functional bug today, but the collision is a latent trap for any future decoder that switches
   on these macros. `XPE_OP_MOVE = 0x2c` (`cmdlist.h:56`) is defined and never referenced.

8. **[low] the `XPE_OP_*` opcode macros are effectively dead in the emit path.** `cmdlist.c`
   assembles every word from hardcoded hex `byte0` values (0x50/0x4c/0x60/0x6a/0x70), not from
   the macros; grep shows the macros appear only in comments (`cmdlist.c:17,130,235`) and the
   header. The header even says they are "kept for defensive decoding only" (`cmdlist.h:40-41,
   57-62`). Worth either wiring the emitters to the macros or clearly marking them decode-only to
   avoid drift between the two encodings.

9. **[low — but flagged in source] the two context length fields are stored as raw bytes, not the
   silicon `command_list_length_32` bitfield.** `xrdp_build_ctx` writes
   `ctx->buf[CTX_OFF_CMDLIST_DLEN] = dlen` and `... = clen` as single bytes
   (`flow_offload.c:196-198`), and the model reads `dlen` as one `uint8_t`
   (`bcm4916_runner.c:690`). Real silicon carries the length as `round_up(clen,4)/4` in a WORD-1
   bitfield of the 124-byte BE struct (`flow_offload.c:190-198`; `flow_offload.h:82-98`). This is
   a `flow_offload.c` concern, but it directly consumes this file's `xpe_cmdlist_data_len` /
   `xpe_cmdlist_len` outputs, so a real-HW length encoder is still owed. (Numerically safe today:
   max body 80 fits in a byte.)

10. **[low] overflow is set but the emit is silently dropped mid-word.** `xpe_emit16`
    (`cmdlist.c:79-82`) drops the write and sets `overflow`, but a command word that overflows on
    its *second* half-word, or a `replace_32` that overflows between its two data words, leaves a
    partially-written command in `buf`. The three callers do check `cl.overflow` afterwards and
    return `-E2BIG` (`flow_offload.c:523,732,808`), so the truncated buffer is never programmed —
    good — but the partial bytes remain in `buf` and `len` is left mid-word (not 4-aligned) if the
    caller ignored the flag. Adequate given current callers; brittle if the flag is ever unchecked.

11. **[low — open question, not a bug] `apply_icsum_16` emits a zero immediate.** The open driver
    relies on the Runner recomputing the checksum delta and emits `0x0000` in the low half
    (`cmdlist.c:314-318`). The stock emitter's low half carries the precomputed 16-bit csum
    (`bfxil #0,#16`, `cmdlist.c:42-44`). If real silicon *applies* the inline immediate rather
    than recomputing, a zero immediate would corrupt the checksum. The QEMU model recomputes, so
    the pair is self-consistent, but silicon behavior is unverified (see open questions).

12. **[info] `XPE_CMDLIST_MAX_BYTES = 80` is smaller than the real slot.** The GPL
    `FC_UCAST_FLOW_CONTEXT_ENTRY` reserves `command_list[100]` (`flow_offload.h:84`), and the live
    `cmd_list_length` unit implies a 100-byte region; 80 is a safe cap for the short L2/NAT
    programs but would truncate a longer (e.g. tunnel/encap) program. `xrdp_build_ctx` additionally
    clamps `clen` to 80 before the `memcpy` (`flow_offload.c:186-188`).

---

## 6. Open questions / unknowns

1. **Exact sub-opcode / operand bit encoding on silicon.** byte0 (`opcode<<2`) and byte1
   (`(off>>1)+1`) are pinned; everything below (byte2/byte3 meaning per family, the
   `.data`-reference / relocation contract, how `replace_16` vs `replace_32` and the
   `replace_bits` pos/width are actually encoded) is the open driver's own packing (ABI UNKNOWN
   #3). Resolve by disassembling `xpe_api.o` / `cmdlist.ko` `xpe_cmd_*` and correlating with the
   GPL microcode arrays of a matched 4916 SDK (`re-notes/rdpa-offload-controlplane.md:278-281`).

2. **Does silicon recompute or apply the icsum immediate?** `xpe_cmd_apply_icsum_16` emits a zero
   immediate assuming HW recomputes the delta from the touched fields (`cmdlist.c:62-65,314-318`).
   Whether real HW recomputes or requires the precomputed 16-bit delta in the command word's low
   half (as the stock `bfxil` suggests) is unverified. If the latter, the builder must compute and
   emit the ones-complement delta for every changed field.

3. **The relocation/`.data` model of the real `xpe_cmd_end`.** The stock finalizer relocates the
   `.text` "from" offsets and concatenates a separate `.data` region (`cmdlist.c:185-192`); the
   open driver inlines operands and does no relocation. The exact stock relocation math (how
   byte2's `0x94` base is fixed up, where `.data` lands relative to `.text`) must be RE'd before a
   silicon-valid buffer can be produced.

4. **Byte-for-byte validation of the emitted L2 and NAT programs.** No routed/NAT, L2-accel, or
   VLAN-edit cmdlist has been captured live — only a GDX local-delivery body
   (`re-notes/live-flow-dump.md:289-295`). Generating a forwarded/NAT'd or bridged flow on silicon
   and capturing its `cmd_list` bytes is the only way to confirm the emitted programs match stock.
   (The live capture rule forbids disturbing the management link, so this needs a dedicated test
   flow — `re-notes/live-flow-dump.md:289-304`.)

5. **True `command_list[]` slot size and the `cmd_list_length` unit.** The live entry showed
   `cmd_list_data_length=28` and `cmd_list_length=40` with a 162-byte tail pad
   (`re-notes/live-flow-dump.md:109-126`); the GPL struct reserves `command_list[100]`. The exact
   relationship (is `cmd_list_length` a 32-bit-word count, `round_up(bytes,4)/4`? bytes?) and the
   real slot size need pinning from `rdpa.ko _ucast_prepare_rdd_*`
   (`re-notes/rdpa-offload-controlplane.md:196-214,282-284`).

6. **Tagged-frame packet offsets.** All L3/L4 offsets (`flow_offload.h:126-135`) assume an
   untagged IPv4 frame with no IP options; for VLAN-tagged frames every offset shifts by
   `VLAN_HLEN`, and the interaction between a VLAN push/pop in the same cmdlist and the NAT
   offsets is untested. The emitter is offset-agnostic, but the caller's offset constants are not
   parameterised for tags/options.

7. **`decrement_8` odd-offset (and general non-TTL) encoding.** The source hints the stock
   encoding differs for odd offsets (`cmdlist.c:246-247`) but the open code only implements the
   even path. The odd-offset byte0 bits, and whether ADD is ever used for anything other than
   TTL/hop-limit `-1`, are unresolved.
