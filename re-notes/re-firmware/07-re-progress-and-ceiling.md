# 07 — RE progress & ceiling: did we fully reverse the firmware?

Honest answer to "did we reverse the closed BCM4916/BCM6813 XRDP firmware well
enough to reimplement HW acceleration in the open driver?" Companion to the
consolidated ABI in `re-notes/re-firmware/00-firmware-re-master.md`.

Short version: the **ARM control-plane ABI is fully pinned and reimplementable**;
the **two custom microcode blobs are structurally characterized but not
instruction-decoded** — and, crucially, they do **not** need to be, because the
open driver loads them verbatim (the user extracts them from their own `rdpa.ko`).

---

## Fully pinned — the ARM control-plane ABI

Everything the A53 host must do to install and run a hardware flow is now
reimplementable from the RE'd encodings, without guessing:

- **NAT-C flow install** (doc 01): the install sequence (mask → hash-slot →
  72-byte BE key+result window → `cmd 3` at engine `+0x10`, busy=bit4 poll), the
  key-mask rule (`key[i] &= ~byteswap32(mask[3-i])`), the hash (XOR-fold seed
  `0x4899b351` / CRC, folded to `13+ddr_size_enum` bits), the command/flush
  encoding, the per-table DDR addressing, and the fact that **there is no delete
  opcode** (delete = cmd-3 write-invalid / `0x10001` flush).
- **cmdlist emitter + finalizer** (doc 02): every plain op's 32-bit BE word
  (`move_packet`/`replace_bits_16`/`decrement_8` even+odd/`apply_icsum_16`), the
  `.text`/`.data` two-region model, the `0x94 + dcnt` operand relocation (the
  former "ABI UNKNOWN #3"), and the `xpe_cmd_end` relocate-then-concatenate
  algorithm with its `.data`-first / `.text`-last order and `0xff`/`0xfc` pads.
- **FC_UCAST result entry** (doc 03): the 124-byte packed-BE bitfield map —
  `valid` @ `WORD@4 bit23`, `command_list_length_32 = (24+clen)/4` @ byte 7,
  command_list @ +24 (`rev32`/word), service_queue/port @ `WORD@12[28:24]`,
  policer @ `WORD@22[14:10]`, tunnel overlay. Bit *positions* are compiler-certain.
- **Microcode LOAD ABI + datapath bring-up** (docs 04/05): the per-core INST/PRED
  block-write (byteswap-to-BE, stride, counts), the 28-step `data_path_init`
  order, and the QM `MEM_AUTO_INIT`/`ENABLE_CTRL=0x307` registers — all verified,
  confirming the open loader byte-exact.

This is the entire host side of HW offload. With the change-list in doc 00 §3
applied, the driver programs the same `{key, mask, slot, 72-B window, cmd}`, the
same 124-B result, and the same cmdlist byte-code the stock stack does.

---

## Characterized but not decompiled — the two custom-ISA blobs

Two proprietary microcode images sit below the ARM ABI. Both are **plaintext and
loadable verbatim**; neither is instruction-decoded, and neither needs to be to
run stock-equivalent acceleration.

### 1. Runner RNR core microcode (doc 05)

- **Known:** container geometry (8 cores × 32 KB = 8192 × 4-byte instructions;
  512×u16 prediction = 1 bit/instr), byte order (4-byte BE in SRAM), the shared
  6-word JMP reset/dispatch vector, per-core role family (TM cores 6/7 distinct;
  CPU_TX/RX = cores 2/3), the opcode field location (high 8 bits, ≤86 opcodes),
  and two opcodes (NOP `0xfc`, JMP `0x0a<target>`). Entropy 6.13–6.58 ⇒ not
  encrypted; the user rebuilds the RFW1 blob byte-exact from their own `rdpa.ko`.
- **Why a full decode is a separate effort:** the RNR core is a **custom Broadcom
  ISA** with no public disassembler and no ghidra/objdump processor module. The
  operand grammar of the other ~84 opcodes, the thread model, and the
  cmdlist-interpreter opcode set are undecoded.
- **Next steps (offline RE):** use the prediction bitmap as a branch oracle to
  enumerate the branch-opcode family; treat the shared vector header as a Rosetta
  stone for field boundaries; histogram low-byte lanes conditioned on the
  high-byte opcode to separate immediate vs register fields; hand-decode the
  smallest image (core0 bootloader) first; assemble known mnemonics if a Broadcom
  `rnr` assembler ever surfaces in an SDK drop.

### 2. Merlin16-Shortfin SerDes PMD microcode (10G line side)

- **Known:** the ~31 KB `merlin16_shortfin_ucode_image[]` blob is extracted and
  CRC-verified (see memory `serdes-blob-extraction`); it is non-redistributable
  and loaded via `firmware_class`.
- **Not reversed here:** the PRAM load mechanism, VCO/datapath bring-up steps, and
  lane-lock timing (audit U9). This is a PHY-side blob, orthogonal to the offload
  fast path; the 10G ports simply will not lock without it, but 1G first-light and
  the whole NAT-C/cmdlist datapath do not depend on it.
- **Next steps (needs blob + silicon):** implement PRAM load, observe lane lock on
  a real 10G link, then USXGMII/2500BASEX AN.

Neither blob is a decode blocker for shipping stock-equivalent acceleration: the
open driver treats both as opaque firmware, exactly as the stock stack ships them
inside `rdpa.ko` / the SerDes SDK header.

---

## Remaining work & how each closes

| item | status | how it closes |
|---|---|---|
| NATC ENG/INDIR sub-block absolute offset (command `+0x10`, key `+0x30..0x3c` are relative to a runtime-pointer base) | offset relative-only | **needs live oracle**: one read-only devmem/FDT read on silicon (doc 01 Unresolved; audit U11/U12) |
| Full GDMA/SRAM-copy descriptor byte layout for `replace_16/32` (fields `len`/`0x94+dcnt` pinned; multi-word serialization partial) | partial | **offline RE**: fully disassemble `__gdma_add`/`__prepend_csum_desc`/`__cmdlist_return` in `xpe_api.o` (doc 02 Unresolved #1) |
| icsum: apply-immediate vs recompute | encoding pinned, semantics open | **offline RE + oracle**: cross-check a live NAT cmdlist body (audit U3/U4) |
| FC_UCAST 1-bit flag *names* + the 14-bit `WORD@12[13:0]` / 7-bit `WORD@16[14:8]` descriptors | positions certain, names inferred | **needs live oracle**: `natc_dump.ko` on an active routed/NAT stock flow + GPL header if it ever surfaces (audit U1) |
| Byte-for-byte validation of the emitted L2-accel and NAT cmdlist programs | structurally validated only | **needs live oracle**: capture a real bridge-accel + NAT flow via `rdpa_trace grp=2` (audit U4) |
| Per-table key byte composition (which flow field → which key byte) | live-pinned for one table | **needs live oracle**: capture more table classes (doc 01 Unresolved) |
| NAT-C hash CRC variant per config byte | XOR-fold pinned, CRC variant not | **offline RE**: disassemble `rdd_crc_bit_by_bit_natc` (doc 01 Unresolved) |
| `runner_core_size[]` exact per-core literals; num-cores global | modeled as 8×8192 | **offline RE**: `.data` byte dump (doc 04 Unresolved) |
| QM `MEM_AUTO_INIT` done-poll timeout; `route_a_queue` logical→physical; `BBH_TX[1]`↔port map | derived/param | **needs silicon test**: trial-slot Route A egress, recovery-armed (audit U5–U7) |
| RNR + Merlin16 full ISA decode | characterized only | **offline RE research effort** (Runner) / **needs blob + silicon** (Merlin) — not required for stock-equivalent offload |

---

## Sources

Docs 01–05 in this directory; `re-notes/re-firmware/00-firmware-re-master.md`;
`docs/audit/09-hardware-acceleration.md` §6, `docs/audit/10-reimplementation-guide.md`
§3 (U1–U12); memory `serdes-blob-extraction`, `runner-firmware-gpl-shippable`.
