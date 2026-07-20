# 00 — Firmware RE master ABI (consolidated) — BCM4916 / BCM6813 XRDP Runner

This is the consolidated, silicon-accurate ABI for the OPEN driver's HW-offload
fast path, distilled from phase-1 reverse engineering of the closed Runner /
RDPA / cmdlist / NAT-C stack. It supersedes the per-subsystem placeholders and
the "ABI UNKNOWN #1/#3/#5" markers scattered through `driver/runner/`.

**Detail docs (do not duplicate — cite):**

| # | doc | subsystem |
|---|---|---|
| 01 | `re-notes/re-firmware/01-natc-abi.md` | NAT-C flow-install engine ABI (key/mask/hash/command) |
| 02 | `re-notes/re-firmware/02-cmdlist-abi.md` | XPE cmdlist emitter + `xpe_cmd_end` relocation/concat |
| 03 | `re-notes/re-firmware/03-fc-ucast-abi.md` | FC_UCAST_FLOW_CONTEXT_ENTRY (the NAT-C "result") |
| 04 | `re-notes/re-firmware/04-rnr-load-datapath.md` | Runner microcode LOAD ABI + datapath bring-up ORDER |
| 05 | `re-notes/re-firmware/05-runner-microcode.md` | Runner microcode blob container + ISA ceiling |

All facts were read from the device's own stock objects by disassembly
(`rdpa.ko`, `xpe_api.armb53_6813.o`, `crossbow_natc.o`) on the build machine;
raw disasm stays under `~/re-scratch/` there. No proprietary source, no blob,
and no PII is reproduced here. This note carries only offsets, bitfields,
encodings, and sequences.

---

## 1. What this pass pinned

Each RE'd ABI fact → the driver placeholder it replaces → the audit gap/milestone
it closes → confidence. Grouped by subsystem. `file:line` are in `driver/runner/`
unless noted.

### NAT-C engine — flow install (doc 01)

| RE'd fact | driver placeholder | closes | conf |
|---|---|---|---|
| ADD command = **3** (engine `+0x10`, cmd bits[2:0]) | `flow_offload.h:61` `NATC_CMD_ADD 3` | M1b/U2 | high |
| **No cmd 4**; delete = cmd-3 write-invalid, or flush `0x10001` (bit16) | `flow_offload.h:62` `NATC_CMD_DEL 4` — **wrong** | M1b/U2 | high |
| Command reg at engine **`+0x10`**; **busy = bit4** poll (~1000×udelay); table_id = bits[14:12]; flush = bit16 | `flow_offload.h:56-59` `NATC_STAGE_*`/`NATC_INDIR_*` PSRAM contract | M1b/U2 | high |
| Key + result staged as **one contiguous 72-byte (18×u32) BE window, key first**, every word `rev32` | `flow_offload.h:56-57` separate `NATC_STAGE_KEY 0x0100`/`NATC_STAGE_CTX 0x0120` | M1b/U2 | high |
| Slot index is **hash-derived** (XOR-fold seed `0x4899b351`, or `rdd_crc_bit_by_bit_natc`; folded to `N=13+ddr_size_enum` bits) then open-addressed probe | `bcm4916_runner.c:663` `idx = p->natc_next_idx++` — **wrong** | M1b/U2 | high(seq)/med(hash const) |
| Key mask rule = `key[i] &= ~byteswap32(mask[key_words-1-i])`, mask **bit=1 ⇒ don't-care**; source `NATC_KEY[table_id]` @ `0x829503b0 + id*0x20` | `flow_offload.c:239-261` naive `key[i] &= ~mask` intent | M1b/U2 | high |
| Install seq = apply-mask → `key_idx_get`(hash+slot) → stage 72-B window → `cmd 3` | `bcm4916_runner.c:660-696` `xrdp_natc_add` | M1b/U2 | high |
| `table_id 0..3` (HW exposes 0..7) = direction/flow-class selector; each owns key-mask + DDR key/result base (`NATC_TBL[id]` @ `0x829502d0 + id*0x10`) | none (new) | M1b | med |
| Per-table geometry struct (stride 0x60): `+0x24` key size, `+0x28` ctx entry size, `+0x40` result base, `+0x58` count | none (new) | M1b | med |

### XPE cmdlist emitter (doc 02)

| RE'd fact | driver placeholder | closes | conf |
|---|---|---|---|
| Command words 32-bit **big-endian**, byte0 = `(opcode<<2)|subflags` | `cmdlist.c:93-97`, `cmdlist.h:31-45` | M1c/U3 | high |
| `move_packet` byte0 `0x4c` / byte1=`from`(raw) / byte2=`to`(raw) / byte3=`nbytes&0x7f` | `cmdlist.c:161-165` | M1c/U3 | high (confirms) |
| `replace_bits_16` byte0 `0x50` / byte1=`idx` / byte3=`(pos&0xf)\|(((w-1)+pos)&0xf)<<4` | `cmdlist.c:133-142` | M1c/U3 | high (confirms b0/1/3) |
| `replace_bits_16` **byte2 = `0x94 + running .data hw index`, NOT constant `0x94`** (relocate per operand) | `cmdlist.c:138` `b2 = 0x94` — **wrong for op #2+** | M1c/U3 (**resolves UNKNOWN #3**) | high |
| Operand lives in a **separate `.data` region** (`rev16`/BE), not inline after the command word | `cmdlist.c:140,144` inline `xpe_emit16` | M1c/U3 | high |
| `decrement_8` **odd** offset → byte0 `0x69` (subflag `01`); even = `0x6a` | `cmdlist.c:250` always `0x6a` — **wrong for odd** | M1c/U3 | high |
| `apply_icsum_16` byte0 `0x70`, byte1=`idx`, **bytes 2-3 = real 16-bit csum immediate** | `cmdlist.c:318` emits `0x0000` | M1c/U3 (encoding; semantics still open) | high |
| `replace_16/32` are **GDMA SRAM-copy DESCRIPTORS** (`sram_packet_gdma`+`__gdma_desc_add`), not a plain `0x60` inline word; descriptor word `len` in bits[30:24], `0x94+dcnt` in bits[23:16] | `cmdlist.c:261-294` invented inline `0x60` word | M1c/U3 | med (fields pinned, full byte layout partial) |
| Builder keeps **five parallel regions** (`.text` u32[]@+260, `.data` u16[]@+516, 3 GDMA/desc), counters @+36/+40 | `cmdlist.h:83-90` single `buf[]` | M1c/U3 | high |
| `xpe_cmd_end`: no terminator; **relocates** `.data` "from" refs then concatenates **`.data → descA → descB → 0xff pad → .text → 0xfc pad`** (`.data` FIRST, `.text` LAST); total `%4==0` | `cmdlist.c:207-221` no concat/relocate | M1c/U3 (**resolves UNKNOWN #3**) | high |
| `0xfc` slot pad + length-delimited framing (no NOP word) | `cmdlist.c:207-221` | M1c/U3 | high (confirms) |

### FC_UCAST_FLOW_CONTEXT_ENTRY — the result (doc 03)

| RE'd fact | driver placeholder | closes | conf |
|---|---|---|---|
| command_list body @ struct **+24 (0x18)**, `rev32` per 32-bit word; total entry **124 B**, cmdlist region **100 B** | `flow_offload.h:97-98` `XPE_CTX_CMDLIST_OFF 24`/`XPE_CTX_ENTRY_MAX 124` (region cap `XPE_CMDLIST_MAX_BYTES 80` short of 100) | M1a/U1 | high |
| `command_list_length_32 = (24 + clen)/4` (whole entry, 32-bit words, incl. 24-B header) in **entry byte 7** = `WORD@4 bits[31:24]` | `flow_offload.c:196-198` two trailing byte counts (`clen/4`, omits header + position) | M1a/U1 | high |
| **valid** = `WORD@4 bit23`, set unconditionally | `flow_offload.h:119` `CTX_OFF_VALID` (trailing byte @106) — **does not exist in real struct** | M1a/U1 | high |
| flags (routed/l2_accel/tunnel) are bitfields over `WORD@4`/`WORD@12`; is_routed vs is_l2_accel = *which builder ran* (no single flags byte) | `flow_offload.h:107-111` `CTX_OFF_FLAGS 8` single byte | M1a/U1 | med |
| egress **vport** = `byte@5` (8-bit) and/or `WORD@12[28:24]` port field | `flow_offload.h:112` `CTX_OFF_VPORT 12` | M1a/U1 | med |
| **service_queue_id** = `WORD@12[28:24]`, **enable** = `byte@21 bit4` | `flow_offload.h:113` `CTX_OFF_SERVICE_Q 13` | M1a/U1 | med |
| **is_hw_cso** ≈ `WORD@11 bit26` (byte14 bit2) | `flow_offload.h:114` `CTX_OFF_IS_HW_CSO 14` bit0 | M1a/U1 | low |
| **policer_id** = `WORD@22 bits[14:10]` (5-bit) | none (new) | M1a | med |
| tunnel: type `WORD@44[27:24]`, `tunnel_index_ref` `WORD@40`, inline hdr bytes 24..39 | `struct xrdp_flow` has no tunnel path | M1a | med |

### Runner microcode load + datapath bring-up (docs 04, 05)

| RE'd fact | driver placeholder | closes | conf |
|---|---|---|---|
| INST load = per-32-bit-word byteswap → `iowrite32be`, stride 4, dest `INST_base + i*4`; 8 cores × 8192 words (index ≤ 8191) | `bcm4916_runner.c:786-788` INST loop | M1a/M2 | high (confirms exactly) |
| PRED load = 512 × u16 source → byteswapped 32-bit slots, stride 4 (1 bit/instr predictor bitmap) | `bcm4916_runner.c:789-791` PRED loop | M1a/M2 | high (confirms exactly) |
| RNR windows `INST=core_base+0x10000`, `PRED=core_base+0x1c000` | `bcm4916_runner.h:55-56` | M2/U11 | high (stock uses runtime ptr arrays; offsets from regmap, not blob) |
| Blob = 8×32 KB inst + 8×1 KB pred, **plaintext** (entropy 6.13–6.58), shared 6-word JMP vector header; instr0 = `JMP entry` (`0x0a<tgt>`), NOP = `0xfc000000`; user builds RFW1 blob byte-exact from own `rdpa.ko` | loader treats image as opaque bytes | M1a | high |
| QM `MEM_AUTO_INIT @0x138`, `STS @0x13c` done=bit0; `ENABLE_CTRL @0x000 = 0x307` (bits {0,1,2,8,9}) | `bcm4916_runner.h:82-83,109-112` | M2/M3 | high (confirms + decodes) |
| Bring-up order: UBUS decode FIRST → CNPL → RNR core-addr → MPM/FPM → queue tables → **ucode load** → sched ×8 → common → SBPM → BBH_RX → DMA → NAT-C → BBH_TX/XLIF → DSPTCHR → **QM enable 0x307 (near END)** → UBUS-master enable → TCAM | `bcm4916_runner.c:1762-1789` `runner_init` order | M2 | high (confirms; adds UBUS-master-after-QM) |
| Stock `drv_qm_init` runs `MEM_AUTO_INIT` **after** FPM-base + `q_context` (auto-init LAST); driver's `runner_qm_init` runs it **first** | `bcm4916_runner.c:1060-1088` `runner_qm_init` | M2/M3 | med (ordering nuance) |

---

## 2. The corrected ABI (what a reimplementer programs against)

Concrete encodings only; see the cited doc for method/evidence.

### 2.1 NAT-C flow install (doc 01 §1–§6)

1. `table_id ≤ 3` selects direction/flow-class. Read that table's key mask from
   `NATC_KEY[table_id]` (`0x829503b0 + id*0x20`).
2. Apply mask in place: for each of `key_words = key_size/4` words,
   `key[i] &= ~byteswap32(mask[key_words-1-i])` (**reverse word order + `rev32`**;
   mask bit 1 = don't-care).
3. Hash the masked key → slot index. Mode 0: `h = rev32(0x4899b351)`, then
   `h ^= rev32(word)` per word; fold to `N = 13 + ddr_size_enum` bits
   (enum0→13 … enum5→18); open-addressed probe from there. Mode ≠ 0:
   `rdd_crc_bit_by_bit_natc`.
4. Stage a **single 72-byte (18×u32) big-endian window**: 4 key words first
   (word-reversed into the front), then up to 56 B of result/context; **every**
   word `rev32`.
5. Issue command at engine `+0x10`: `cmd=3` (write/add). Poll **busy = bit4**
   until clear. `table_id` → bits[14:12]. Delete = a `cmd 3` write of an
   invalidated entry; whole-table wipe = `0x10001` (bit16 flush). **There is no
   cmd 4.**

Absolute NATC engine base is `0x82950000`; the ENG/INDIR sub-block offset (where
`+0x10` command and `+0x30..0x3c` key-staging live) is reached via a runtime
pointer and must be fixed by a live devmem/FDT read (see §Remaining, doc 01
Unresolved).

### 2.2 XPE cmdlist emitter + finalize (doc 02 §B–§E)

Per-op command word (32-bit BE, `byte0 = opcode<<2 | subflags`,
`idx = (offset>>1)+1`, range `≤ 0xfe`):

| op | byte0 | byte1 | byte2 | byte3 | operand |
|---|---|---|---|---|---|
| move_packet | `0x4c` | from (raw) | to (raw) | `nbytes&0x7f` | none |
| replace_bits_16 | `0x50` | idx | **`0x94 + dcnt`** | `(pos&0xf)\|(((w-1)+pos)&0xf)<<4` | 1 hw `data16<<pos` → `.data` |
| decrement_8 (even) | `0x6a` | idx | idx | `0xff` | none |
| decrement_8 (odd) | `0x69` | idx | idx | `0xff` | none |
| apply_icsum_16 | `0x70` | idx | csum hi | csum lo | none |
| replace_16/32 | GDMA descriptor (see below) | — | — | — | data → `.data` |

`dcnt` = `.data` half-word count before this operand. `0x94` = `.data` region
base in half-word units. `replace_16/32` emit **no plain word** — they call
`sram_packet_gdma` + `__gdma_desc_add`, building a descriptor whose word packs
`len` in bits[30:24] and `0x94+dcnt` in bits[23:16], recorded in a 72-byte
per-group record (fields +44/+48/+52).

`xpe_cmd_end`: (1) relocate every descriptor's `.data` "from" ref against the
final running offset; (2) assemble the output buffer in order
**`.data | descA | descB | 0xff align-pad | .text | 0xfc slot-pad`** — `.data`
FIRST, `.text` LAST; (3) assert total length `% 4 == 0`. No terminator word;
execution is length-delimited by `command_list_length_32`.

### 2.3 FC_UCAST result entry (doc 03 §1–§5)

124-byte packed **big-endian bitfield** struct. Program:

- `valid` = `WORD@4 bit23` (set always).
- `command_list_length_32` = `(24 + cmdlist_bytes)/4` → entry **byte 7**
  (`WORD@4[31:24]`).
- command_list body copied to **struct +24**, **`rev32` per 32-bit word**;
  region is 100 bytes.
- `service_queue_id` = `WORD@12[28:24]`; enable = `byte@21 bit4`.
- egress `vport` = `byte@5` and/or `WORD@12[28:24]`.
- `policer_id` = `WORD@22[14:10]`.
- `is_hw_cso` candidate = `WORD@11 bit26`.
- tunnel overlay: type `WORD@44[27:24]`, `tunnel_index_ref` `WORD@40`, inline
  header bytes 24..39.

Flag *positions* are certain (compiler `bfi` operands); several 1-bit flag
*names* are inferred (GPL `FC_UCAST_FLOW_CONTEXT_ENTRY_STRUCT` header is absent
from the 4916 GPL SDK).

### 2.4 Microcode load + bring-up (docs 04 §2–§7, 05)

Load: for each source 32-bit word `w` at index `i`, write `byteswap32(w)` to
`INST_base + i*4` (`iowrite32be`), 8192 words × 8 cores. Prediction: read u16,
`rev32`, store u32 at stride 4, 512 entries/core. QM: `MEM_AUTO_INIT@0x138` then
poll `STS@0x13c` done=bit0; `ENABLE_CTRL@0x000 = 0x307`. Follow the 28-step
bring-up order (doc 04 §5); run `MEM_AUTO_INIT` **after** FPM-base + `q_context`,
`ENABLE_CTRL` near the end, UBUS-master enable after that.

---

## 3. Driver change-list (specify, do NOT apply)

Ordered so the interlocking M1a/b/c land in one generation-consistent pass
(the M1 gate). Line numbers are current-tree anchors.

### A. NAT-C (`01`; M1b) — `flow_offload.{c,h}`, `bcm4916_runner.c`

1. **`flow_offload.h:62`** — delete `NATC_CMD_DEL 4`. Replace `xrdp_natc_del`
   semantics with a `cmd 3` write of an invalidated entry, or a
   `NATC_CMD_FLUSH 0x10001` for table wipe.
2. **`flow_offload.h:56-59`** — replace the 4 PSRAM contract offsets with the
   real model: a single 72-byte BE key+result window and a command register at
   NATC engine `+0x10` (cmd bits[2:0], table_id [14:12], flush bit16, busy bit4).
   Add `XRDP_OFF_NATC` ENG/INDIR sub-block offset as a module param pending the
   live devmem read (doc 01 Unresolved).
3. **`bcm4916_runner.c:663`** — replace `idx = p->natc_next_idx++` with the
   hash+probe slot resolver (§2.1 step 3): XOR-fold seed `0x4899b351` (or CRC),
   fold to `N=13+ddr_size_enum`, open-addressed probe. Drop `natc_next_idx`.
4. **`bcm4916_runner.c:660-696`** (`xrdp_natc_add`) — restage into ONE contiguous
   72-byte BE window (key first, `rev32` all 18 words) instead of separate
   `NATC_STAGE_KEY`/`NATC_STAGE_CTX` writes; issue `cmd 3` at engine `+0x10` and
   poll busy=bit4.
5. **`flow_offload.c:243-278`** (`xrdp_build_key`) — apply the per-table mask
   `key[i] &= ~byteswap32(mask[key_words-1-i])` after packing, sourcing the mask
   from `NATC_KEY[table_id]`. Keep the live-pinned L3 byte layout; make the
   class/trailer/vport bytes (13/15) table-driven rather than the hardcoded
   `0x28`/`0x68`.

### B. cmdlist emitter (`02`; M1c) — `cmdlist.{c,h}`

6. **`cmdlist.h:83-90`** — replace the single `buf[]` with five regions: `.text`
   `u32[]`, `.data` `u16[]` (+ counters), and the GDMA descriptor records. This
   is the structural prerequisite for #7–#11.
7. **`cmdlist.c:138`** — emit `byte2 = 0x94 + dcnt` (running `.data` half-word
   index), not constant `0x94`.
8. **`cmdlist.c:140,144`** — write `replace_bits_16` operands into the `.data`
   region (`rev16`/BE), not inline after the command word.
9. **`cmdlist.c:237-252`** (`xpe_cmd_decrement_8`) — emit byte0 `0x69` for odd
   offsets, `0x6a` for even.
10. **`cmdlist.c:304-319`** (`xpe_cmd_apply_icsum_16`) — carry the real 16-bit
    checksum immediate in bytes 2-3; resolve apply-vs-recompute (open, U3).
11. **`cmdlist.c:261-294`** (`xpe_cmd_replace_16/_32`) — replace the invented
    inline `0x60` word with the GDMA SRAM-copy descriptor path (`sram_packet_gdma`
    + descriptor record); pin the full multi-word layout first (see §Remaining).
12. **`cmdlist.c:207-221`** (`xpe_cmd_end`) — implement relocation of descriptor
    `.data` "from" refs and region concatenation in order
    `.data | descA | descB | 0xff pad | .text | 0xfc pad`; keep length-delimited
    framing. Update the QEMU model to decode the concatenated layout.

### C. FC_UCAST result (`03`; M1a) — `flow_offload.{c,h}`

13. **`cmdlist.h:70`** — raise `XPE_CMDLIST_MAX_BYTES` to the real **100**-byte
    command_list region (was 80).
14. **`flow_offload.c:158-202`** (`xrdp_build_ctx`) — emit the real packed-BE
    124-byte bitfield struct instead of the flat `CTX_OFF_*` bytes: set `valid`
    at `WORD@4 bit23`; write `command_list_length_32 = (24+clen)/4` into byte 7;
    copy the cmdlist to +24 **`rev32` per word**; place service_queue_id/vport at
    `WORD@12[28:24]`, sq-enable at `byte@21 bit4`, policer at `WORD@22[14:10]`.
15. **`flow_offload.h:107-119`** — delete the `CTX_OFF_FLAGS/VPORT/SERVICE_Q/`
    `IS_HW_CSO/CMDLIST_DLEN/LEN/VALID` flat contract; replace with the bitfield
    positions above. Update the QEMU model to the same bitfield contract.

### D. microcode/bring-up (`04`,`05`; M2) — `bcm4916_runner.c`

16. INST/PRED loaders (`bcm4916_runner.c:786-791`) and QM regmap: **no change** —
    RE confirms them byte-exact. Keep.
17. **`bcm4916_runner.c:1060-1088`** (`runner_qm_init`) — if/when `q_context` and
    static QM SRAM writes are added, order them **before** `MEM_AUTO_INIT` to
    match stock (auto-init LAST). Cosmetic today (no q_context yet); flagged so
    the ordering is not inverted later.

---

## Sources

Docs 01–05 in this directory (each carries its stock-object symbol addresses and
build-machine raw-disasm paths). Audit cross-refs:
`docs/audit/09-hardware-acceleration.md` §6, `docs/audit/10-reimplementation-guide.md`
§M1a/M1b/M1c + §3 (U1–U4), `docs/audit/08-hw-abi-regmap.md`.
