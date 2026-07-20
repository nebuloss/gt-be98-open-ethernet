# 02 — XPE cmdlist byte-code emitter ABI (stock `xpe_api.o` reverse engineering)

## Purpose

This note pins the **exact command-word encoding and the `xpe_cmd_end` finalize/relocate
algorithm** used by the closed Broadcom cmdlist builder on the BCM6813/BCM4916 XRDP
"Runner", so the open driver's clean-room emitter (`driver/runner/cmdlist.c`) produces
buffers that would actually run on silicon.

It closes (or narrows) the single hardest reimplementation gap:

- **M1c / ABI UNKNOWN #3** — "the operand bit-packing below the opcode field is the open
  driver's own invention" (`docs/audit/03-cmdlist.md` finding #2, open question #1).
- **The relocation / `.data` model of `xpe_cmd_end`** (`docs/audit/03-cmdlist.md` open
  question #3; finding #1 "the `0x94` byte references are dead").
- Confirms the framing decision (length-delimited, `0xfc` slot pad, no terminator word)
  from `docs/audit/03-cmdlist.md` §2.1.

The headline results:

1. **Two separate build regions** are pinned, not one flat buffer: a `.text` **command-word
   array** (`u32[]`, big-endian) and a `.data` **operand array** (`u16[]`, big-endian). The
   open driver interleaves operands inline after each command word; the stock builder keeps
   them apart and **concatenates them in `xpe_cmd_end`**, `.data` region **first**, `.text`
   region **last**.
2. The mysterious constant **`0x94` is the `.data` region base**, and the real "from"
   reference the open driver hard-codes as a constant is actually **`0x94 + running .data
   half-word index`** — i.e. it must be **relocated per operand**. This is the exact fact
   the audit called dead/unknown.
3. The plain command ops (`move_packet`, `replace_bits_16`, `decrement_8`,
   `apply_icsum_16`) are **byte-for-byte confirmed** against the open driver.
4. **`replace_16` / `replace_32` are NOT plain `0x60` command words in stock** — they are
   built through the **GDMA / SRAM-packet-copy descriptor** subsystem, a different
   structure. The open driver's single inline `0x60` word is an invention (see §4/§5).

---

## Method

All disassembly done on the build machine (read-only), raw dumps kept under
`~/re-scratch/xpe-cmdlist/` there. No proprietary listing or reconstructed C is reproduced
below — only offsets, bitfields, and sequences expressed in prose/tables.

- **Primary binary:** `xpe_api.armb53_6813.o_saved` (the BCM6813/armb53 build), in
  `$SDK/bcmdrivers/broadcom/char/cmdlist/impl1/`. ELF `elf64-littleaarch64`, relocatable.
- Tools: `aarch64-linux-gnu-objdump -dr` (relocations inline), `nm -n`, `readelf -S`.

Symbols disassembled (file offsets = symbol addresses in `.text`):

| symbol | addr | role |
|---|---|---|
| `xpe_init` | `0xf00` | initialises the global builder state `xpe_ctrl_g` (`.bss`, 0x308 B) |
| `xpe_cmd_offset` | `0x10b0` | returns current `.text` word count (`xpe_ctrl_g+36`) |
| `xpe_get_length` | `0x10c0` | computes total emitted length from the region counters |
| `__command_add.constprop.0` | `0x700` | **core**: append/patch a command word + copy operand into `.data` |
| `__cmdlist_return` | `0xa00` | emits the trailing GDMA "return"/flush descriptor words |
| `__gdma_desc_add` | `0xaa0` | builds a GDMA descriptor word; **adds `0x94`** to the `.data` index |
| `__gdma_add` | `0x2d0` | stores GDMA descriptors into per-group 0x48-byte arrays |
| `sram_packet_gdma` | `0x11a0` | GDMA for the SRAM packet region (used by replace) |
| `__cmd_move_packet` | `0xd30` | VLAN push/pop primitive (opcode `0x13`) |
| `__cmd_replace.constprop.0` | `0x1290` | replace core → routes through GDMA, not a plain word |
| `xpe_cmd_end` | `0x1410` | **crux**: relocate, concatenate regions, pad, finalise |
| `xpe_cmd_replace_16` | `0x1d20` | thin wrapper → `__cmd_replace` (width flag 0) |
| `xpe_cmd_replace_32` | `0x1da0` | thin wrapper → `__cmd_replace` (width flag 1, count×2) |
| `xpe_cmd_replace_bits_16` | `0x1e20` | bit-field replace (opcode `0x14`) |
| `xpe_cmd_decrement_8` | `0x2c50` | TTL/hop-limit ADD(-1) (opcode `0x1a`) |
| `xpe_cmd_apply_icsum_16` | `0x2d60` | incremental checksum fixup (opcode `0x1c`) |

Cross-check object (same encoding, larger): `cmdlist.o` (the linked `cmdlist.ko` content),
same directory. Open-driver targets: `driver/runner/cmdlist.c`, `driver/runner/cmdlist.h`,
`driver/runner/flow_offload.{c,h}`, `qemu/device-model/bcm4916_runner.c`.

---

## ABI reference

### A. The builder state `xpe_ctrl_g` (global, `.bss`, 0x308 = 776 bytes)

`xpe_init(base, size)` zeroes 0x308 bytes and seeds it (`base` at +0, `size` at +8, `9` at
+24). All emitters index this single global; there is no per-flow object passed by pointer
(the open driver's `struct xpe_cmdlist` is the analogue). Pinned field offsets:

| offset | dec | field | evidence |
|---|---|---|---|
| +0x00 | 0 | output base pointer (final buffer dest) | `xpe_init` `str x21,[x22]`; `xpe_cmd_end` `ldr x0,[x22]` |
| +0x08 | 8 | max size (buffer cap, from `xpe_init` arg1) | `str w19,[x20,#8]`; bound checks in `__command_add` |
| +0x10 | 16 | max **packet** word-index touched (bounds tracking) | `ldr/str [.bss+16]` in every offset op |
| +0x24 | 36 | **`.text` command-word count** (u32 index) | `xpe_cmd_offset` returns `[+36]`; `__command_add` writes `.text[cnt]` |
| +0x28 | 40 | **`.data` operand half-word count** (u16 index) | `__command_add`/`__gdma_desc_add` `ldr w,[.bss+40]` |
| +0x30 | 48 | GDMA/SRAM descriptor region-A half-word count | `xpe_cmd_end` concat len `2*[+48]` |
| +0x34 | 52 | GDMA/SRAM descriptor region-A base (u16[]) | `xpe_cmd_end` `memcpy(out, ctrl+0x34, 2*[48])` |
| +0x78 | 120 | GDMA descriptor region-B half-word count | `xpe_cmd_end` concat len `2*[+120]` |
| +0x7c | 124 | GDMA descriptor region-B base (u16[]) | `xpe_cmd_end` `memcpy(out, ctrl+0x7c, 2*[120])` |
| +0xc0 | 192 | csum/GDMA descriptor region-C half-word count | `xpe_cmd_end` relocation loop bound `[+192]/2` |
| +0xc4 | 196 | csum/GDMA descriptor region-C base (u32[]) | relocation loop reads `[ctrl + i*4 + 196]`, `rev` |
| +0x104 | 260 | **`.text` command-word array base** (u32[], BE) | `__command_add` `str w5,[base + idx*4 + 260]` |
| +0x204 | 516 | **`.data` operand array base** (u16[], BE) | `__command_add`/`__gdma*` `strh w,[base + idx*2 + 516]` |

So the builder maintains **five parallel regions** (text commands, data operands, and three
GDMA/descriptor regions) plus counters — not the single byte buffer the open driver models.

### B. Command-word storage: 32-bit, big-endian, byte0 = opcode<<2

`__command_add` writes each command word with `rev w,w` (byte-swap) before `str`, so the
in-memory byte order is big-endian: **`byte0` (lowest address) = logical bits[31:24]**.
Recovered mapping for every plain command word:

- `byte0` = bits[31:24] = `(opcode << 2) | sub-flags`
- `byte1` = bits[23:16]
- `byte2` = bits[15:8]
- `byte3` = bits[7:0]

`__command_add(cmd, data_ptr, data_hwcount, out_off_ptr, mode)`:
- `mode == -1`: **append** a new word to `.text`, then copy `data_hwcount` half-words from
  `data_ptr` into `.data` (each `rev16`, i.e. BE), advance `.data` count.
- `mode == -2`: append the word, **no** operand copy.
- `mode >= 0`: **overwrite** `.text[mode]` (back-patch), then copy operand — this is the
  back-patch primitive used during relocation.
- If `out_off_ptr != 0`, it stores `2 * (.data count before this call)` there = the byte
  offset of the operand inside `.data` (used by callers to build the "from" reference).

### C. Per-op command-word encoding (plain `.text` ops via `__command_add`)

`idx = (offset >> 1) + 1` is the 16-bit **word index into the packet** (the open driver's
`xpe_to_idx`). Confirmed in every emitter as `lsr w,offset,#1 ; add w,#1`.

| op (sym @addr) | byte0 | opcode | byte1 | byte2 | byte3 | operand → `.data` | notes |
|---|---|---|---|---|---|---|---|
| `move_packet` `@0xd30` | `0x4c` | `0x13` | `from` (RAW byte off) | `to` (RAW byte off) | `nbytes & 0x7f` | none | base word `0x4c000000`; `bfi #16,#8`/`bfi #8,#8`/`bfxil #0,#7` |
| `replace_bits_16` `@0x1e20` | `0x50` | `0x14` | `idx` | **`0x94 + dcnt`** | `(pos&0xf) \| (((width-1)+pos)&0xf)<<4` | 1 hw = `data16 << pos` | base `0x50000000`; operand copied to `.data`, **not inline** |
| `decrement_8` even `@0x2c50` | `0x6a` | `0x1a` (ADD, subflag `0b10`) | `idx` | `idx` | `0xff` | none | base `0x64000000`; `orr …,#0xff` |
| `decrement_8` **odd** | `0x69` | `0x1a` (subflag `0b01`) | `idx` | `idx` | `0xff` | none | odd offset sets bits[25:24]=`01` (`bfi #24,#2`, val 1) |
| `apply_icsum_16` `@0x2d60` | `0x70` | `0x1c` | `idx` | csum hi | csum lo | none | base `0x70000000`; **16-bit csum immediate in bytes 2–3** (`bfxil #0,#16`) |

Key corrections to the open driver, pinned here:

1. **`byte2` of `replace_bits_16` is `0x94 + dcnt`, not the constant `0x94`.** `dcnt` =
   `.data` half-word count *before* this operand (`xpe_ctrl_g+40`). `0x94` is the `.data`
   region base in half-word units; each successive operand-bearing op increments the
   reference. The open driver emits a fixed `0x94` (`cmdlist.c:138`), correct only for the
   first operand-bearing op in a program.
2. **The operand goes into the separate `.data` region**, not inline after the command word.
   `replace_bits_16` passes the shifted `data16` to `__command_add` with `hwcount=1`; it is
   `rev16`-stored into `.data[dcnt]`. The open driver appends it inline (`cmdlist.c:140`).
3. **`decrement_8` odd offset really does change byte0** (`0x69`, subflag `01`) — confirms
   audit finding #4. The even path (`0x6a`, subflag `10`) matches the open driver; the odd
   path is unimplemented there.
4. **`apply_icsum_16` carries a real 16-bit checksum immediate in bytes 2–3** (the stock API
   takes a `csum16` argument, `bfxil` into bits[15:0]). The open driver hard-emits `0x0000`
   (`cmdlist.c:316`). No `0x94` relocation applies to icsum (bytes 2–3 are the immediate,
   not a `.data` ref). Whether HW *applies* or *recomputes* is still open (see §5), but the
   encoding slot is now pinned.

### D. `replace_16` / `replace_32` — GDMA descriptor path (NOT a plain `0x60` word)

`xpe_cmd_replace_16(offset, count, u16 *data)` and `xpe_cmd_replace_32(offset, count,
u32 *data)` are thin wrappers over `__cmd_replace.constprop.0`. `replace_32` doubles the
half-word `count` and sets a 32-bit width flag. `__cmd_replace`:

- computes `idx = (offset>>1)+1`, range-checks `idx ≤ 0xfe`, updates the max-index tracker
  (`+16`);
- calls **`sram_packet_gdma()`** and **`__gdma_desc_add()`** — it does **not** call
  `__command_add`, i.e. it emits **no plain `.text` command word**;
- copies the immediate data into `.data` (`rev16` for 16-bit fields, `rev` for 32-bit) and
  advances the `.data`/descriptor counters and the `+12/+16/+20` relocation bookkeeping.

`__gdma_desc_add` builds the descriptor word: `dref = [ctrl+40] + 0x94` (the same
`0x94`-relative `.data` reference), `len = (len==0x80 ? 0 : len)`, packed as
`word[30:24]=len` (7-bit), `word[23:16]=dref` (8-bit), then `__gdma_add` stores it plus the
copy source/dest into a per-group **0x48-byte (72-B) descriptor record** (fields at record
offsets +44/+48/+52). These descriptor records are what `xpe_cmd_end` later serialises into
descriptor regions A/B/C (§E).

**Consequence:** a stock replace/NAT rewrite is a **GDMA SRAM-copy descriptor**, structurally
unlike the open driver's single inline `(0x60 << 24)|idx|(0x94<<8)|len` word
(`cmdlist.c:276,293`). The `0x60`/opcode-`0x18` the open driver uses (and the live
`0x6014eb98`) is consistent with a GDMA descriptor's leading word carrying the `len`/`dref`
fields above, but the **full multi-word descriptor byte layout is only partially pinned**
(see §5). The open driver's replace encoding must be replaced by a descriptor emitter, or the
open datapath must avoid HW replace (do NAT rewrites another way).

### E. `xpe_cmd_end` — the finalise / relocate / concatenate algorithm (crux)

`xpe_cmd_end` (`@0x1410`) runs three phases:

**Phase 1 — descriptor relocation loop** (`0x14bc`–`0x1728`). Iterates `[ctrl+192]/2`
entries of region-C (`u32[]` @ +196). For each entry it `rev`s the word, extracts
`byte2 = bits[16:23]` (the `0x94`-relative `.data` ref) and the 7-bit length
(`bits[24:30]`), recomputes the reference against the running data offset, and re-emits it via
`__prepend_csum_desc` (`@0x480`). This is the **relocation** the audit said was missing:
GDMA/csum descriptors have their `.data` "from" references fixed up to the final concatenated
layout. It then calls `sram_packet_gdma()` and appends the trailing GDMA "return"/flush
descriptor via `__cmdlist_return` (`@0xa00`, three words beginning `0x08800021`).

**Phase 2 — region concatenation** (`0x1868`–`0x18e4`, then `0x1660`–`0x16d0`). The final
output buffer (dest = `xpe_ctrl_g+0`) is assembled by `memcpy`/`memset` in **this order**:

| # | source region | length (bytes) | fill |
|---|---|---|---|
| 1 | `.data` operands (`ctrl+0x204`) | `2 * [ctrl+40]` | — |
| 2 | descriptor region-A (`ctrl+0x34`) | `2 * [ctrl+48]` | — |
| 3 | descriptor region-B (`ctrl+0x7c`) | `2 * [ctrl+120]` | — |
| 4 | alignment pad | computed `& 3` slack | **`0xff`** (`memset`) |
| 5 | `.text` command words (`ctrl+0x104`) | `4 * [ctrl+36]` | — |
| 6 | trailing slot pad to `size` | `[ctrl+8] - .text_bytes` | **`0xfc`** word fill |

So on silicon the buffer is **`.data`-region first, `.text`-region last**, with a `0xff`
inter-region alignment pad and a `0xfc` trailing slot pad. The whole buffer length is
asserted `% 4 == 0` (`tst x20,#3` → `brk` on failure), and copies are bounded (`.data`
≤ 0x104, `.text` ≤ 0x204, else `fortify_panic`). `xpe_ctrl_g+0` is then cleared (list
finalised).

**Phase 3 — framing.** There is **no terminator command word**. The list is length-delimited
(the context's `cmd_list_data_length`); the `0xfc` tail (phase-2 row 6) lies past the
executable length and is never decoded. This **confirms** the open driver's framing decision
(`cmdlist.c:207-221`), including the `0xfc` pad byte — but note the stock finalizer also emits
a `0xff` *inter-region* pad (row 4) that the open driver never produces, because the open
driver has no separate `.data` region to concatenate.

`xpe_get_length` (`@0x10c0`) computes the executable length as
`2*(.text_words) + 2*(.data + regionA + regionB + regionC counts)` rounded to a 4-byte
multiple minus the text-word contribution — i.e. length is derived from the region counters,
not a running byte cursor.

---

## Mapping to the open driver

| RE'd fact | open-driver placeholder (`file:line`) | status | conf |
|---|---|---|---|
| `move_packet`: byte0 `0x4c`, byte1=`from` raw, byte2=`to` raw, byte3=`nbytes&0x7f`, no data | `driver/runner/cmdlist.c:161-165` | **confirms** exactly | high |
| `replace_bits_16`: byte0 `0x50`, byte1=`idx`, byte3 pos/width nibble = `(pos&0xf)\|(((w-1)+pos)&0xf)<<4` | `driver/runner/cmdlist.c:133-142` | **confirms** byte0/1/3 | high |
| `replace_bits_16` byte2 = **`0x94 + dcnt`**, not constant `0x94` | `driver/runner/cmdlist.c:138` (`b2 = 0x94`) | **corrects** (must relocate per operand) | high |
| `replace_bits_16` operand lives in separate `.data` region, `rev16`/BE, not inline | `driver/runner/cmdlist.c:140` (inline `xpe_emit16`) | **corrects** | high |
| `decrement_8` even: byte0 `0x6a`, byte1=byte2=`idx`, byte3 `0xff` | `driver/runner/cmdlist.c:237-251` | **confirms** | high |
| `decrement_8` **odd**: byte0 `0x69` (subflag `01`) | `driver/runner/cmdlist.c:250` (always `0x6a`) | **corrects** (finding #4) | high |
| `apply_icsum_16`: byte0 `0x70`, byte1=`idx`, bytes 2–3 = 16-bit csum immediate | `driver/runner/cmdlist.c:304-318` (emits `0x0000`) | **corrects** encoding slot (semantics still open) | high |
| Command words are 32-bit **big-endian**, byte0 = `opcode<<2` | `driver/runner/cmdlist.c:34-45,93-97` | **confirms** | high |
| `idx = (offset>>1)+1` word index; range `≤ 0xfe` | `driver/runner/cmdlist.c:115-118` | **confirms** (adds range bound) | high |
| Separate `.text`(u32[]@260) and `.data`(u16[]@516) regions with counters @36/@40 | `driver/runner/cmdlist.h:83-90` (single `buf[]`) | **corrects / newly-pins** | high |
| `xpe_cmd_end`: no terminator, length-delimited, `0xfc` slot pad | `driver/runner/cmdlist.c:207-221` | **confirms** | high |
| `xpe_cmd_end` concatenation order `.data → descA → descB → 0xff pad → .text → 0xfc pad` | `driver/runner/cmdlist.c:207` (no concat at all) | **newly-pins** | high |
| `0x94` = `.data` region base (half-word units); "from" refs relocated in `xpe_cmd_end` | `docs/audit/03-cmdlist.md` finding #1 / OQ #3 | **resolves** UNKNOWN #3 (the `0x94` question) | high |
| `replace_16/32` are GDMA SRAM-copy **descriptors**, not a plain `0x60` word | `driver/runner/cmdlist.c:261-294` | **corrects** (open encoding is invented) | med |
| GDMA descriptor word: `len` in bits[30:24], `0x94+dcnt` in bits[23:16] | `driver/runner/cmdlist.c:276,293` | **partially pins** the descriptor | med |

---

## Unresolved

1. **Full GDMA/SRAM-copy descriptor byte layout for `replace_16/32`.** `__gdma_desc_add`
   pins the `len`/`0x94+dcnt` fields and the 0x48-byte per-group record, but the exact
   serialised multi-word descriptor emitted by `xpe_cmd_end`'s `__prepend_csum_desc`
   (`@0x480`) and `__cmdlist_return` (`@0xa00`) was not fully decoded (record fields +44/+48/
   +52, the `0x08800021`/`0xb0800008` return words). Needs `__gdma_add`/`__prepend_csum_desc`
   fully RE'd to reproduce a byte-exact replace. Until then the open driver cannot emit a
   silicon-valid HW NAT rewrite.
2. **Absolute meaning of the `0x94` base at Runner runtime.** `0x94` is pinned as the `.data`
   half-word base used in "from" references, and `xpe_cmd_end` places `.data` first in the
   output. Whether `0x94` is an offset into the loaded cmdlist image or into a fixed Runner
   SRAM/scratch window (i.e. the mapping applied when the microcode loads the buffer) is not
   settled from this object alone; it needs the microcode / RDD loader side.
3. **icsum: apply-immediate vs recompute.** The 16-bit immediate slot (bytes 2–3) is pinned,
   but whether silicon adds the inline delta or recomputes the checksum is unverified (open
   question #2 in the audit). If it applies the immediate, the open driver's `0x0000` is a
   bug; if it recomputes, `0x0000` is harmless.
4. **Region-C (`+196`) semantics.** Identified as a `u32[]` descriptor region walked and
   relocated in phase 1, but its exact producer and whether it overlaps the concatenated
   region-B tail was not separated with certainty.
5. **Byte-for-byte validation against a live L2/NAT capture** remains outstanding (audit
   finding #3 / OQ #4) — this note pins the *emitter* ABI from the object, not a silicon
   trace of the driver's own programs.

---

## Sources

- Binary: `xpe_api.armb53_6813.o_saved` — `$SDK/bcmdrivers/broadcom/char/cmdlist/impl1/`
  (`$SDK = .../release/src-rt-5.04behnd.4916`, on the build machine, read-only).
  Cross-check: `cmdlist.o` (same dir).
- Symbol addresses (all in `.text`): `xpe_init@0xf00`, `xpe_cmd_offset@0x10b0`,
  `xpe_get_length@0x10c0`, `__command_add@0x700`, `__cmdlist_return@0xa00`,
  `__gdma_add@0x2d0`, `__gdma_desc_add@0xaa0`, `sram_packet_gdma@0x11a0`,
  `__cmd_move_packet@0xd30`, `__cmd_replace@0x1290`, `__prepend_csum_desc@0x480`,
  `xpe_cmd_end@0x1410`, `xpe_cmd_replace_16@0x1d20`, `xpe_cmd_replace_32@0x1da0`,
  `xpe_cmd_replace_bits_16@0x1e20`, `xpe_cmd_decrement_8@0x2c50`,
  `xpe_cmd_apply_icsum_16@0x2d60`.
- `xpe_ctrl_g` in `.bss` (0x308 B); key struct offsets +0/+8/+16/+36/+40/+48/+52/+120/+124/
  +192/+196/+260/+516 as tabulated in §A.
- Raw disassembly retained at `~/re-scratch/xpe-cmdlist/dis_*.txt` on the build machine.
- Open-driver cross-refs: `driver/runner/cmdlist.c`, `driver/runner/cmdlist.h`,
  `driver/runner/flow_offload.{c,h}`, `docs/audit/03-cmdlist.md`,
  `qemu/device-model/bcm4916_runner.c`.
