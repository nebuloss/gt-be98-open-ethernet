# 05 — Runner microcode blob: structural ABI + ISA-reversing starting point

Reverse-engineering notes for the raw XRDP "Runner" microcode embedded in the stock
`rdpa.ko` (BCM4916 / BCM6813). This is interoperability RE for the clean-room open
driver at `driver/runner/`. **Read-only** analysis of a staged copy on the build host;
no blob is committed (repo `.gitignore` blocks `*.bin`/`*.ko`, and none is placed in the
tree). Extracted images live only in `~/re-scratch/microcode/` on the build machine.

## Purpose

The open driver loads a Runner microcode blob (`request_firmware("brcm/bcm4916-runner-microcode.bin")`,
`driver/runner/bcm4916_runner.c:102`) and pushes it into per-core instruction/prediction
SRAM (`runner_load_microcode()`, `bcm4916_runner.c:709-798`). Until now the *format* the
driver parses ("RFW1" header + per-core inst/pred images) and the *load semantics*
(4-byte big-endian instruction words, u16→u32 prediction slots, `XRDP_RNR_INST_OFF=0x10000`,
`XRDP_RNR_PRED_OFF=0x1c000`) were an **educated reconstruction** from `re-notes/realhw/10`,
not verified against the stock artifact. This pass verifies the blob's structure against
the stock `rdpa.ko` data objects **and** the stock loader code, so the open driver's
loader can be trusted (or corrected) and a future blob-builder produces a byte-exact image.

It closes the microcode half of audit gap **M1a** ("what does the firmware blob look like
and how is it loaded") and pins the firmware facts in `docs/audit/09` §1.2 / §4. It does
**not** attempt (and cannot, this pass) an instruction-level decompile of the RNR ISA —
see the honest ceiling in §ABI reference.

## Method

All disassembly/extraction done on the build host, raw output kept under `~/re-scratch/`.
Target = the unstripped staged stock module:

```
rdpa.ko = .../release/src-rt-5.04behnd.4916/rdp/projects/BCM6813/target/rdpa/rdpa.ko
```

Symbol table (`nm rdpa.ko | sort`) — the microcode data objects and their loaders:

| symbol | vaddr (.data-relative) | size | what |
|---|---|---|---|
| `fw_binary_0` .. `fw_binary_7` | `0x603d8` + n·`0x8008` | `0x8000` (32768) each | per-core instruction-SRAM images |
| `rdpa_version_fw_0` .. `_7` | follows each `fw_binary_n` | 8 B each | per-core fw version word (see note) |
| `fw_predict_0` .. `fw_predict_7` | `0x57ed0` + n·`0x400` | `0x400` (1024) each | per-core branch-prediction RAM images |
| `fw_inst_binaries` | `0x57e90` | 8 ptrs | table of pointers → `fw_binary_n` |
| `fw_pred_binaries` | `0x57e50` | 8 ptrs | table of pointers → `fw_predict_n` |
| `runner_core_size` | `0x5fd70` | 8 × u32 | per-core instruction count |
| *(num cores word)* | `.data+0x5d9b8` | u32 | loop bound = 8 |
| `RNR_INST_BLOCK` | `0xbae8` | ptr array | per-core inst-SRAM MMIO dest base |
| `RNR_PRED_BLOCK` | `0xbb88` | ptr array | per-core pred-RAM MMIO dest base |
| `drv_rnr_load_microcode` | `0xae420` | — | top-level per-core instruction load loop |
| `drv_rnr_load_instructions` | `0xae3d0` | — | single-core instruction copy helper |
| `drv_rnr_load_prediction` | `0xae510` | — | per-core prediction copy loop |

.data maps at file offset `0x2525e0` (vaddr 0, relocatable object), so `file_off = 0x2525e0 + symval`.
A python3 slicer (`~/re-scratch/microcode/extract_ucode.py`) dumped each image; analysis
scripts (`analyze_ucode.py`, `opcode.py`, `sizes.py`) computed entropy, similarity, and
field histograms. The three loaders were disassembled one function at a time with
`aarch64-linux-gnu-objdump -dr` (bounded by adjacent symbols).

## ABI reference

### Blob geometry (CONFIRMED)

| fact | value | evidence |
|---|---|---|
| number of cores | **8** | `runner_core_size[8]`, num-cores word `.data+0x5d9b8` = 8; load loops bound by it |
| instruction image size | **32768 B** each, ×8 = 256 KB | `nm` sizes; `runner_core_size[c]=8192` × 4 B |
| instructions per core | **8192** | `runner_core_size = {8192,8192,8192,8192,8192,8192,8192,8192}` |
| instruction width | **4 bytes (32-bit unit)** | see width evidence below — five independent signals |
| prediction image size | **1024 B** each = **512 × u16** | `nm` sizes; load computes `core_size/16 = 512` entries |
| prediction density | **1 bit per instruction** | 512 u16 = 8192 bits = 8192 instructions (bijection) |
| encoding | **plaintext native code** (not encrypted/compressed) | image entropy 6.13–6.58 bits/byte; pred 7.05–7.63 |

`rdpa_version_fw_n` are **all-zero in static `.data`** — populated at runtime (or the version
lives inside the image), so image→role cannot be read from the version words statically.

### Instruction load semantics (CONFIRMED from `drv_rnr_load_instructions` @ 0xae3d0)

The stock per-core copy loop (body `0xae3e8..0xae40c`):
- streams the source image in **8-byte chunks** (`ldr`/`str` 64-bit), loop counter += 2;
- applies **`rev32`** — byte-swaps each 32-bit lane *independently* (NOT `rev` over 64 bits);
- destination is addressed as `RNR_INST_BLOCK[core] + (dst_word << 3)` where `dst_word`
  advances by 1 per 8-byte store, i.e. the instruction-memory row is **64 bits wide and
  holds two 4-byte instructions**; the instruction *address* counts in 4-byte units.

The use of `rev32` (per-32-bit swap) rather than `rev` is the load-side proof that the
**fundamental instruction unit is 32 bits**. Byte order: source is stored **little-endian**
in `.data`; the loader byte-swaps to **big-endian** into SRAM. The executed instruction word
therefore equals `le32(raw 4-byte unit)`.

`drv_rnr_load_microcode` @ 0xae420 is the wrapper: for `core in 0..num_cores`, if
`runner_core_size[core] != 0`, copy `fw_inst_binaries[core]` into `RNR_INST_BLOCK[core]`
using the same rev32/8-byte inner loop.

### Prediction load semantics (CONFIRMED from `drv_rnr_load_prediction` @ 0xae510)

Per core: entry count = `((core_size + 7) >> 3) >> 1 = core_size/16 = 512`. Inner loop
(`0xae570..0xae584`): read source **u16** (`ldrh`, post-inc +2), `rev32`, store as **u32**
(`str w`, post-inc +4) into `RNR_PRED_BLOCK[core]`. So the prediction RAM is **512 × u32
slots, each carrying a 16-bit value**, and there is exactly **one prediction bit per
instruction** (512 × 16 = 8192). This is a branch-predictor bitmap over the instruction
slots, not a branch-target table.

### Per-image structural map

| core / image | role (audit 09 §1.2) | effective instrs* | idle-tail | reset-vector instr0 |
|---|---|---|---|---|
| 0 | CFE2/direct-BBH (bootloader) | 7258 | 934 | `0x0a0000e2` |
| 1 | (image_1) | 7822 | 370 | `0x0a0000e2` |
| 2 | **CPU_TX** (core2, thr6) | 7192 | 1000 | `0x0a0000e2` |
| 3 | **CPU_RX** (core3, thr1) | 7873 | 319 | `0x0a0000e2` |
| 4 | (image_4) | 7240 | 952 | `0x0a0000e2` |
| 5 | (image_5) | 7386 | 806 | `0x0a0000e2` |
| 6 | **US_TM** | 7001 | 1191 | `0x0a000072` |
| 7 | **DS_TM** | 6906 | 1286 | `0x0a000072` |

\* instruction count after stripping the trailing idle word `0xfc000000`. Idle words also
appear mid-code as filler, so distinct-instruction diversity is ~4900–5600 per image.

**Shared vector header:** instruction words **0..5** are common across all 8 images
(`… 0x88840100 0x8c840120 0x88240100 0x8c940040 …`), except instr0/instr1 which differ for
the TM pair (cores 6/7 start `0x0a000072` vs `0x0a0000e2` on cores 0–5). Instr0 is a
**JMP to the image entry point** (opcode `0x0a`, 24-bit target: `0xe2` for cores 0–5, `0x72`
for the TM cores — both valid 8192-slot addresses). The header is the reset/dispatch vector.

**Similarity matrix (byte-identical fraction):** off-diagonal 0.10–0.22 → images are
**87–89 % unique**; each core runs a genuinely distinct program. The shared 10–22 % is the
vector header + the `0xfc000000` idle fill + common constants/subroutines. Cores 6↔7 are the
most alike (0.22), consistent with the US_TM/DS_TM pair being one image family; cores 1 and 3
are the most divergent from the rest (0.10–0.13).

### Instruction-width & opcode hypothesis (best-supported)

**Width = 4 bytes (32-bit), big-endian in SRAM.** Five independent signals agree:
1. loader uses `rev32` (per-32-bit lane swap), not 64-bit `rev`;
2. `runner_core_size = 8192` × 4 B = the full 32768-byte image;
3. prediction is exactly 1 bit/instruction (512 u16 = 8192 bits = 8192 instrs);
4. the idle/NOP word `0xfc000000` tiles the code tail on clean 4-byte boundaries;
5. byte-lane entropy: treating each executed word as `le32(raw)`, lane 3 (bits 31:24) shows
   only **86 distinct values** across the image vs **230–245** for lanes 0–2.

**Opcode field = high 8 bits (bits 31:24).** Signal (5) localizes the opcode to lane 3;
≤86 opcodes are in use. Low 24 bits carry operands (near-full-range registers/immediates/
targets). Two encodings are pinned by structure:

| opcode (bits 31:24) | meaning | evidence |
|---|---|---|
| `0xfc` (`0xfc000000`) | **NOP / idle** (zero operands) | tiles every image tail; most-common word (568–1372×/image) |
| `0x0a` | **JMP <24-bit target>** | reset vector instr0 = `0x0a00_00e2`/`0x0a00_0072`; target within 8192-slot range |

Other frequent high-byte opcodes (bin2/CPU_TX): `0x38`, `0xb0`, `0xaa`, `0x82`, `0x3c`,
`0xa8` — an opcode-frequency profile, not yet semantically decoded.

### Decodability assessment (honest ceiling)

The RNR core is a **custom Broadcom instruction set** with no public disassembler and no
objdump/ghidra processor module. This pass rigorously pins the **container** (8×32 KB,
4-byte BE instructions, 1-bit/instr predictor, shared JMP vector, per-core role map) and
**two opcodes** (NOP `0xfc`, JMP `0x0a`), plus the opcode field position. A full
instruction-level decompile is **NOT achievable** here and is not claimed. What is and isn't
decodable:

- **Decodable now:** image geometry, per-core roles, load ABI, byte order, instruction
  address model, predictor-bit mapping, opcode-field location, JMP/NOP encodings,
  entry-point vectors, effective code lengths.
- **Not decodable now:** the operand grammar for the other ~84 opcodes (register vs
  immediate vs memory fields), the thread model inside an image, cmdlist-interpreter
  opcodes, and any correlation of instruction addresses to the CPU_TX/RX thread numbers
  (thr6/thr1) — the microcode alone does not distinguish core2 from core3.

**ISA-reversing next steps** (for a future custom decoder):
1. Use the **prediction bitmap** as a branch oracle: the set bits mark branch/predict
   instruction slots — cross-reference those slot opcodes to enumerate the branch opcode
   family and validate the JMP/`0x0a` finding.
2. Treat the **shared vector header** as a Rosetta stone: known JMP semantics + identical
   words 2–5 across 8 images give a fixed reference frame for field boundaries.
3. Build an **opcode→operand-shape model** by histogramming each low-byte lane conditioned
   on the high-byte opcode (fixed-immediate opcodes will pin certain lanes; register
   opcodes will spread them).
4. Diff the **bootloader image (core0)** against the runtime images to isolate the minimal
   startup routine (smallest distinct-instruction set) for first hand-decode.
5. If a Broadcom `rnr`/`ucode` assembler ever surfaces in an SDK drop, assemble known
   mnemonics and match against these images to bootstrap the opcode table.

## Mapping to the open driver

| RE'd fact | driver placeholder (`file:line`) | status | conf |
|---|---|---|---|
| 8 cores, `num_cores` word = 8 | `bcm4916_runner.h:48` `XRDP_RNR_CORES 8`; loader `bcm4916_runner.c:753` `ncores > XRDP_RNR_CORES` | confirms | high |
| instruction unit = 4-byte, **big-endian** in SRAM (stock `rev32`) | `bcm4916_runner.c:785-787` INST via per-4-byte `iowrite32be` | confirms (byte-exact) | high |
| prediction = 512 × u16 → u32 slots, BE (stock `ldrh`/`rev32`/`str w`) | `bcm4916_runner.c:788-790` PRED u16→u32 BE; comment `:782-783` "512 u32 slots each holding a 16-bit pred value" | confirms | high |
| 8192 instrs/core, images fully used (32 KB) | RFW1-layout comment `bcm4916_runner.c:733-737` "8 inst images (32KB)" | confirms | high |
| prediction is 1 bit/instruction (512×16 = 8192), branch-predictor bitmap | loader comment guess `re-notes/…` "predict RAM usually holds branch source/target addresses" | corrects (bitmap, not target table) | high |
| per-core lengths come from `runner_core_size[]`, not a tail-scan | driver reads `inst_len`/`pred_len` from the RFW1 per-core table `bcm4916_runner.c:761-764` | confirms the table-driven approach; a blob-builder should emit `inst_len=32768`, `pred_len=1024` | high |
| reset vector = JMP(entry) at instr0; TM cores (6/7) differ | none (driver treats image as opaque bytes) | newly-pins | high |
| image→role: cores 6/7 = TM family; core2=CPU_TX, core3=CPU_RX indistinguishable by microcode | `bcm4916_runner.h:160-161` `RNR_CPU_RX_THREAD=1`/`RNR_CPU_TX_THREAD=6` | confirms TM grouping; RX/TX index unverified by blob | med |
| dest offsets `INST=+0x10000`, `PRED=+0x1c000` | `bcm4916_runner.h:55-56` | **unverified here** — stock uses runtime `RNR_INST_BLOCK`/`RNR_PRED_BLOCK` pointer arrays; offsets not derivable from the blob | med |
| blob is proprietary, plaintext (entropy ~6.3) — user extracts from own `rdpa.ko` | `docs/audit/09` §4 firmware-status | confirms (no decryption needed to build the RFW1 blob) | high |

## Unresolved

- **INST/PRED SRAM offsets** (`0x10000`/`0x1c000`) are the driver's assumption; the stock
  loader dereferences the `RNR_INST_BLOCK`/`RNR_PRED_BLOCK` pointer arrays (built elsewhere
  from the XRDP register map), so this pass neither confirms nor corrects them — needs the
  RNR core register-window base RE (audit §6 / `08-hw-abi-regmap`).
- **CPU_TX vs CPU_RX image index** (core2 vs core3): the two images share the identical
  reset vector and are both in the "cores 0–5" family; the microcode alone can't confirm
  which carries thread6 (TX) vs thread1 (RX). Mapping stays from audit 09 §1.2 / live recon.
- **`rdpa_version_fw_n`** are zero statically — the fw version string/word is filled at
  runtime or embedded inside the image; not located this pass.
- **The RNR ISA** beyond opcode-field position + JMP/NOP: ~84 opcodes' operand grammar,
  thread dispatch, and the cmdlist-interpreter opcode set remain undecoded (custom ISA, no
  disassembler). See ISA-reversing next steps.
- Whether the open driver's "RFW1" container is the only viable packaging (vs. shipping the
  8 raw images + a sidecar size table) is a driver-design choice; the stock artifact stores
  the images as separate `.data` arrays + `runner_core_size`, which the RFW1 header
  faithfully re-encodes.

## Sources

- `rdpa.ko` (unstripped, staged, read-only): `.../src-rt-5.04behnd.4916/rdp/projects/BCM6813/target/rdpa/rdpa.ko`.
- Data objects: `fw_binary_0..7` @ `.data+0x603d8`(+n·0x8008); `fw_predict_0..7` @ `.data+0x57ed0`(+n·0x400);
  `fw_inst_binaries` @ `0x57e90`; `fw_pred_binaries` @ `0x57e50`; `runner_core_size` @ `0x5fd70`;
  num-cores word @ `.data+0x5d9b8`; `RNR_INST_BLOCK` @ `0xbae8`; `RNR_PRED_BLOCK` @ `0xbb88`.
- Loaders disassembled: `drv_rnr_load_instructions` @ `0xae3d0`, `drv_rnr_load_microcode` @ `0xae420`,
  `drv_rnr_load_prediction` @ `0xae510`.
- Extracted images + analysis scripts (build host, NOT committed): `~/re-scratch/microcode/`.
- Cross-referenced: `docs/audit/09-hardware-acceleration.md` §1.2/§4; `driver/runner/bcm4916_runner.c`
  (`:102`, `:709-798`), `driver/runner/bcm4916_runner.h` (`:48`, `:55-56`, `:160-161`);
  `re-notes/runner-microcode-and-cpuring.md`.
