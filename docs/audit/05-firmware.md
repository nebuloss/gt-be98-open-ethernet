# 05 — Firmware blobs (Runner microcode + Merlin16 SerDes microcode)

Audit of the two proprietary firmware images the BCM4916 / BCM6813 datapath
needs and of every piece of code in this tree that produces, packages, or loads
them. This is the "firmware half" of the audit: what each blob *is*, why it is
mandatory, how a user extracts it from their **own** device/SDK, how the open
driver loads it via `request_firmware()`, and the licensing reality that gates
the whole HW-offload goal.

Files audited (line-by-line):

- `tools/extract-runner-microcode.sh` — Runner-microcode extractor (GITIGNORED;
  read for understanding, no proprietary offsets copied here)
- `tools/extract-serdes-fw.py` — Merlin16 SerDes-microcode extractor (GITIGNORED)
- `driver/runner/bcm4916_runner.c` — the loaders `runner_load_microcode()`
  (Runner) and `runner_serdes_load()` + the `serdes_*` helpers (SerDes uC)
- `driver/runner/bcm4916_runner.h` — firmware names/sizes, RNR-SRAM offsets,
  SerDes indirect-window register map
- `driver/pcs/pcs-bcm-xport.c` — the *other* SerDes-firmware entry point
  (`bcm_xport_pcs_load_firmware()`, a TODO stub)

Read for accuracy / cross-reference (the RE oracle):

- `re-notes/runner-microcode-and-cpuring.md` — the Runner-microcode **verdict**
  (PROPRIETARY / non-redistributable) + where it lives in `rdpa.ko`
- `re-notes/xport-serdes-bringup.md` — the SerDes bring-up + blob dependency
- `re-notes/gpl-source-inventory.md` — proof the microcode is absent from the
  GPL SDK
- `re-notes/realhw/10-runner-bringup-spec.md` — the `RFW1` container the driver
  actually parses (§ "Wave-5")

All `file:line` references are to the committed source at audit time. Where a
value is a placeholder, guess, or hardcoded-pending-silicon it is called out
explicitly. **No proprietary Broadcom symbol offsets/addresses from the
gitignored extractor scripts are reproduced here**; firmware handling is
described generically per the repo's sanitization rule.

---

## 1. Purpose

The BCM4916 datapath is **not** a direct-DMA MAC. Packets flow
MAC ↔ BBH ↔ FPM/SBPM buffer pools ↔ the 8-core XRDP "Runner" packet processor,
and it is the *Runner firmware* — not fixed-function hardware — that fills the
host CPU rings. On the line side, the multi-gig/10G ports run through a
Merlin16-Shortfin SerDes whose PMD is itself driven by an on-die microcontroller
that needs its own firmware image to achieve lock. Two independent firmware
blobs are therefore in the critical path:

| Blob | What it drives | Mandatory for | Source |
|---|---|---|---|
| **Runner microcode** — 8 per-core instruction images + 8 branch-prediction images | The 8 XRDP Runner cores that move every CPU-forwarded / trapped frame | **Any** packet MAC↔CPU (even the dumb slow path) | Compiled into the stock proprietary `rdpa.ko` |
| **Merlin16-Shortfin SerDes microcode** — one ~31 KB PMD µC image | The SerDes on-die micro that brings the multi-gig/10G lanes to PMD lock | Any real 2.5G/5G/10G XPORT link | A compiled-in C array in the Broadcom SDK header |

Both are **proprietary and non-redistributable**. This subsystem is where the
"fully-open, shippable driver" goal collides with legal reality: the open driver
ships **no** microcode. It only `request_firmware()`s images the *end user*
must extract from hardware/SDK they already own. Consequently a working datapath
inherits a `P` (proprietary) taint dependency exactly like the stock module —
this is documented, not hidden (`bcm4916_runner.c:23-31`).

Where each blob sits in the pipeline:

```
                 +-------- Runner microcode (8x32KB inst + 8x1KB pred) -------+
                 |  loaded into RNR_INST[c]/RNR_PRED[c] SRAM before core enable |
   line side     v                          host side
 [SFP/PHY]==Merlin16 SerDes==[XPORT/XLMAC MAC]==[BBH]==[FPM pool]==[Runner cores]==[CPU rings]==host
     ^                                                                    |
     +------ Merlin16 SerDes microcode (~31KB) into the SerDes micro PRAM -+
             (needed for PMD lock; without it the lanes never link)
```

---

## 2. Architecture & data flow

### 2.1 Runner microcode — where it lives and what it is

Per `re-notes/runner-microcode-and-cpuring.md` (Part A) the 4916 Runner
microcode exists **only** compiled into the stock `rdpa.ko` as global data
objects:

| Symbol group | Count × size | Total | Meaning |
|---|---|---|---|
| `fw_binary_0` … `fw_binary_7` | 8 × 32768 B (0x8000) | **262144 B (256 KB)** | the 8 Runner-core instruction-SRAM images |
| `fw_predict_0` … `fw_predict_7` | 8 × 1024 B (0x400) | **8192 B (8 KB)** | per-core branch-prediction RAM images |
| `rdpa_version_fw_0/2/4/6` | 4 × 8 B | 32 B | per-core fw version words (informational) |

The 8×32 KB layout maps to XRDP's 8 packet-processor cores (the older 63138 RDP
had 4). The stock loaders `drv_rnr_load_microcode / _load_instructions /
_load_prediction` write these into the Runner instruction SRAM through the
`ag_drv_rnr_inst_*` accessors (block MWRITE into the `RNR_INST` window) — the
same load *model* as the older RDP but with XRDP-specific accessors
(`runner-microcode-and-cpuring.md` §"Where … lives").

**License evidence (the load-bearing citations):** `rdpa.ko` `.modinfo` declares
`license=Proprietary`; loading it sets the kernel `P` taint; the module is
absent from the GPL SDK (`gpl-source-inventory.md`: "Runner microcode … ABSENT
entirely — none in the tree"). Extracting `fw_binary_*` and redistributing it
under GPL/`linux-firmware` would be redistributing proprietary code without a
license. **Verdict: PROPRIETARY / NON-REDISTRIBUTABLE.**

### 2.2 Runner microcode — the driver's on-disk container ("RFW1")

The open driver does **not** load raw `rdpa.ko` symbols; it loads a repackaged
container it calls **`RFW1`** from `"/lib/firmware/brcm/bcm4916-runner-microcode.bin"`
(`RUNNER_FW_NAME`, `bcm4916_runner.c:102`). The `RFW1` layout the driver parses
(`bcm4916_runner.c:731-794`; spec `re-notes/realhw/10-runner-bringup-spec.md:175-178`):

```
off 0x00  'R','F','W','1'                          (4 B magic)
off 0x04  u32 version = 1                           (LE)
off 0x08  u32 num_cores = 8                         (LE)
off 0x0c  u32 hdr_size (= 32)                       (LE)
off 0x10  u32 entry_size (= 16)                     (LE)
off 0x14  u32 total                                 (LE)
off 0x18  u32 rsvd[2]                               (8 B)
off 0x20  num_cores x { u32 inst_off, inst_len, pred_off, pred_len }  (16 B each)
   ...    payload: 8 inst images (32 KB) then 8 pred images (1 KB, u16-packed)
```

Reference blob size (spec §Wave-5): **270496 B** = 32 B header + 128 B table
(8×16) + 262144 B inst + 8192 B pred.

**Endianness transform at load (critical):** the Runner instruction SRAM is
**big-endian** (the stock loader byte-swaps every word, MWRITE_32), while the
`RFW1` payload stores inst/pred as native **little-endian**. So the loader:
- **INST:** reads each native LE u32 and writes it big-endian —
  `iowrite32be(get_unaligned_le32(...))` — into `rnr_mem[c] + XRDP_RNR_INST_OFF`
  (`0x10000`; `bcm4916_runner.h:55`).
- **PRED:** reads each native LE u16 and writes it as a big-endian **u32** at
  stride 4 into `rnr_mem[c] + XRDP_RNR_PRED_OFF` (`0x1c000`;
  `bcm4916_runner.h:56`) — i.e. 1 KB of packed u16 expands to 512 u32 SRAM
  slots, each holding a 16-bit prediction value.

Per-core SRAM base = `XRDP_OFF_RNR_MEM0` (`0x00700000`) + `c ×
XRDP_RNR_MEM_STRIDE` (`0x00020000`), computed once in probe into
`p->rnr_mem[c]` (`bcm4916_runner.c:1741-1746`, `bcm4916_runner.h:46-48`). Core
enable/wakeup happens **later**, in `runner_rnr_enable()` (probe step, `:1794`),
strictly after the microcode is written — a hard ordering constraint.

### 2.3 Merlin16 SerDes microcode — where it lives and what it is

Per `xport-serdes-bringup.md` §4, the SerDes lanes "will not achieve PMD lock on
real hardware" until a **~31 KB proprietary microcode**
(`merlin16_shortfin_ucode_image[]`, version `D102_0A`, **CRC-16/CCITT-FALSE ==
0x4949**) is PRAM-loaded into the SerDes micro. Unlike the Runner blob this one
ships as a **compiled-in C array in an SDK header**, not as a
`request_firmware()` file — so the extractor turns the array back into a raw
binary. Expected size **31664 B**, CRC **0x4949** (`SERDES_FW_SIZE`,
`bcm4916_runner.h:369`; extractor `EXPECT_SIZE`/`EXPECT_CRC`). Same
non-redistributable class as the Runner microcode.

### 2.4 Merlin16 SerDes microcode — how the driver streams it (the indirect window)

The open driver reaches the SerDes through an **indirect register window** at
`SERDES_PHYS_BASE = 0x837ff500`, size `0x300`, per-core stride `0x100`
(`bcm4916_runner.h:335-337`). This window is in the `0x83000000` SoC region,
**outside** the rdpa/XRDP window, so it is `devm_ioremap`'d separately and only
when the opt-in `serdes_fw_load` param is set (`bcm4916_runner.c:1733-1738`).

A merlin "lane register" access is encoded as `(dev<<27) | (lane<<16) | reg` and
driven through an ADDR/MASK/CNTRL triplet per core (`bcm4916_runner.h:325-346`):

| Field | Offset (per core = base + core×0x100) | Notes |
|---|---|---|
| INDIR_ADDR | `+0x04` | the encoded `(dev<<27)|(lane<<16)|reg` |
| INDIR_MASK | `+0x08` | write mask (driver writes `~mask`) |
| INDIR_CNTRL | `+0xf0` | `reg_data[15:0]`, `r_w[16]`, `start_busy[17]`, `delayed_ack[18]` |

Micro-subsystem control registers used during the load (merlin dev1 space,
`bcm4916_runner.h:347-367`): `SRD_MICRO_CLK_CTRL 0xd200`, `RST_CTRL 0xd201`,
`AHB_CTRL 0xd202` (ra_wrdatasize[1:0], ra_init[9:8], autoinc[12]),
`AHB_STATUS 0xd203`, `RA_WRADDR_LSW/MSW 0xd204/0xd205`,
`RA_WRDATA_LSW 0xd206`, `PMI_IF_CTRL 0xd228`, and the running gate
`SRD_UC_ACTIVE_REG 0xd0f4` bit15 (`uc_active`).

**Load flow (`runner_serdes_load`, `bcm4916_runner.c:1578-1670`):** assert µC
reset + drive the micro register block to its reset-default values (an opaque
recipe of ~30 zero writes + three non-zero writes `0xD216=0x0007`,
`0xD225=0x8201`, `0xD228=0x0101`, RE'd from the SDK `uc_reset`) → toggle
subsystem reset → init code + data RAM (two `ra_init` phases, each polled) →
set the write port to autoinc/16-bit at address 0 → **stream the whole blob as
16-bit little-endian words** into `RA_WRDATA_LSW` (raw, no transform) → release
the µC → poll `uc_active` (bit15) up to 10000 µs to confirm the firmware runs.

The word-transfer engine is `serdes_xfer()` (`:1517`), which posts ADDR, then
(for writes) `~mask` + CNTRL with `START_BUSY|DELAYED_ACK`, and spins on
`START_BUSY` clearing (up to 1000 µs). Thin wrappers `serdes_wr_reg`, `serdes_wr_f`
(field {mask,shift}), `serdes_rd` sit on top.

### 2.5 The *second* SerDes-firmware path (the PCS stub)

There is a **parallel, unfinished** SerDes-firmware entry point in the mainline
PCS driver: `bcm_xport_pcs_load_firmware()` (`pcs-bcm-xport.c:167-175`). It does
**not** call `request_firmware()` or touch the PRAM at all — it just
`dev_warn_once`s "microcode load not implemented (proprietary blob); link will
not come up on real HW" and returns 0. It is called for cores 0 and 1 from
`bcm_xport_pcs_enable()` (`:186-187`). So the tree contains **two disagreeing
implementations** of the same SerDes-µC load against the same block — a real
streaming loader in `bcm4916_runner.c` and a warn-only TODO in
`pcs-bcm-xport.c`. See Audit findings §5.2.

### 2.6 How the driver finds the files (`firmware_class`)

Both loaders use the standard `request_firmware(&fw, NAME, dev)` synchronous
path, so the kernel looks under `/lib/firmware/` (and any extra directory the
user configures). Install targets:

- Runner: `/lib/firmware/brcm/bcm4916-runner-microcode.bin` (`RUNNER_FW_NAME`).
- SerDes: `/lib/firmware/brcm/merlin16-shortfin.bin` (`SERDES_FW_NAME`,
  `bcm4916_runner.h:368`).

The live-trial helper scripts additionally point `firmware_class` at a custom
directory via `echo <dir> > /sys/module/firmware_class/parameters/path`
(`driver/trial/{load,takeover}.sh`), which is how the non-redistributable blobs
are supplied on the device without touching the rootfs `/lib/firmware`.
`MODULE_FIRMWARE(RUNNER_FW_NAME)` is declared (`bcm4916_runner.c:1899`) so the
dependency is visible to `modinfo`/initramfs tooling; **there is no
`MODULE_FIRMWARE` for the SerDes blob** (finding §5.5).

---

## 3. Data structures

### 3.1 `RFW1` container header + per-core table (driver-defined ABI)

Parsed inline in `runner_load_microcode()` (no C struct; raw offset reads via
`get_unaligned_le32`). Fields as in §2.2. Validation constraints the parser
enforces (`bcm4916_runner.c:745-773`):

- `fw->size >= 32` and first 4 bytes == `"RFW1"`, else `-EINVAL`.
- `num_cores <= XRDP_RNR_CORES` (8); `entry_size >= 16`;
  `hdr_size + num_cores*entry_size <= fw->size`, else `-EINVAL`.
- per core: `rnr_mem[c]` mapped, `inst_off+inst_len <= size`,
  `pred_off+pred_len <= size`, else `-EINVAL`.

### 3.2 `struct firmware` (kernel) — used by both loaders

Standard `<linux/firmware.h>` handle: `fw->data` (blob bytes), `fw->size`.
Released with `release_firmware(fw)` on every exit path.

### 3.3 Firmware-related fields in `struct runner_priv`

(`bcm4916_runner.c` / `bcm4916_runner.h`)

| Field | Decl | Meaning |
|---|---|---|
| `void __iomem *serdes` | `bcm4916_runner.c:192` | ioremap of the Merlin indirect window `0x837ff500` (only when `serdes_fw_load`) |
| `void __iomem *rnr_mem[8]` | `:1741-1746` | per-core Runner SRAM windows (INST/PRED targets) |
| `bool fw_loaded` | `bcm4916_runner.c:223` | true only after a successful Runner-microcode load; false in emulated/absent cases; printed at probe end (`:1833`) |

### 3.4 Module parameters that gate firmware handling

(`bcm4916_runner.c:116-145`, all `0444` read-only)

| Param | Default | Effect |
|---|---|---|
| `runner_emulated` | false | skip the Runner-microcode load entirely (QEMU path); also settable via DT `brcm,runner-emulated` |
| `serdes_fw_load` | false | opt-in: ioremap the SerDes window + run `runner_serdes_load()` (HW only) |
| `serdes_core` | 0 | which merlin16 core (0/1/2) the 10G port uses |
| `serdes_lane` | 0 | lane index within the core |

### 3.5 SerDes register-map macros

The complete indirect-window + micro-subsystem macro set lives at
`bcm4916_runner.h:325-369` (enumerated in §2.4). Notably `SRD_MICRO_RA_INITDONE
= 0x8001` is defined to "tolerate bit0|bit15 (RE ambiguity)" — an explicit
unknown baked into a mask (finding §5.6).

### 3.6 Extractor container header (`B4916UC`) — **does not match the driver**

`extract-runner-microcode.sh` emits a *different* 32-byte header:
`magic "B4916UC\0"` + `u32 version=1, n_cores=8, inst_sz=0x8000, pred_sz=0x400`
+ 8 B reserved, then raw `fw_binary_0..7` (256 KB) then `fw_predict_0..7` (8 KB);
total `0x42020 = 270368 B`. This is **not** the `RFW1` layout the driver parses
(§2.2). See finding §5.1 — the single most important defect in this subsystem.

---

## 4. Function reference (in file order, per file)

### 4.1 `tools/extract-runner-microcode.sh` (GITIGNORED, user-run)

Bash script; extracts `fw_binary_*` / `fw_predict_*` from the user's own
`rdpa.ko` and concatenates them into a container. Resolves symbol → file-offset
dynamically from the ELF at runtime (`readelf`), so it hardcodes **no**
proprietary addresses. Takes `rdpa.ko` path (default is a generic local path)
and output path as args (`:66-67`).

- **`die(msg)`** — `:73`. `echo` to stderr + `exit 1`. Used for the tool-presence
  and file-presence checks (`:75-76`) and every hard error below.

- **`sym_offsize(name)`** — `:96-119`. Given a symbol name, echoes
  `"<file_offset_dec> <size_dec>"`. Reads the symbol's `st_value`, size and
  section index from `readelf -sW`, then maps VA→file offset via the section
  header: `file_off = st_value - sh_addr + sh_offset`. The section-header parse
  uses tolerant `awk` heuristics (first 8–16-hex field = Address, second
  6–16-hex field = Off) because `readelf -SW` column layout varies — **fragile**
  (finding §5.7). `die`s if the symbol or its section header cannot be resolved
  (guards against a stripped/wrong `rdpa.ko`). Callee of `extract_into`.

- **header heredoc (python3)** — `:126-134`. Writes the 32-byte **`B4916UC`**
  header (magic + `version, n_cores, inst_sz, pred_sz, 0` + reserved word),
  `assert len == 0x20`. This is the container header that does **not** match the
  driver (finding §5.1).

- **`extract_into(sym, expect_sz)`** — `:139-148`. Calls `sym_offsize`, warns if
  the actual size differs from expected (continues with actual), then
  `dd bs=1 skip=<foff> count=<sz>` to pull the bytes and appends to the growing
  body. `bs=1` = byte-at-a-time (slow for 256 KB but correct). Called in two
  loops: `fw_binary_0..7` then `fw_predict_0..7` (`:150-153`).

- **top-level flow** — `:64-168`. `set -euo pipefail`; verifies `readelf`;
  optionally prints `modinfo -F license` and warns loudly if Proprietary
  (`:81-89`); builds header + body in a `mktemp -d` (trap-cleaned); concatenates
  to `$OUT`; checks the final size against `0x20 + 8*INST + 8*PRED`; prints an
  install reminder that the blob is proprietary and must stay off the repo /
  `linux-firmware`. **No CRC/version cross-check** of the extracted images.

### 4.2 `tools/extract-serdes-fw.py` (GITIGNORED, user-run)

Python3; turns the SDK's compiled-in `merlin16_shortfin_ucode_image[]` C array
back into a raw `.bin`. Can be pointed at the header directly or at an SDK root
(`REL_HDR` relative path). Also documents (in comments) the alternative of
pulling the symbol out of a compiled `phy_drv.ko`, exactly like the Runner path.

- **`crc16_ccitt_false(data) -> int`** — `:33-39`. Classic CRC-16/CCITT-FALSE
  (init `0xFFFF`, poly `0x1021`, no reflection, no final xor). Used to verify the
  extracted blob against `EXPECT_CRC = 0x4949` — "the same check the driver
  uses" per the header comment (though the *open* driver does **not** actually
  check it — finding §5.4).

- **`parse_array(text) -> bytes`** — `:42-49`. Regex-extracts the first
  `{ ... }` body, then all `0x??`/`0x?` byte tokens, and packs them to `bytes`.
  `sys.exit` if no array body or no hex bytes found.

- **`main()`** — `:52-76`. Argparse (`header` | `--sdk` | `-o`), resolves the
  header path, parses the array, computes CRC, warns (not fatal-before-write) on
  size ≠ 31664 or CRC ≠ 0x4949, writes the `.bin` **regardless**, then
  `sys.exit(0 if ok else 1)`. So a mismatched blob is still written but the exit
  code is nonzero (finding §5.8).

### 4.3 `driver/runner/bcm4916_runner.c` — Runner-microcode loader

- **`runner_load_microcode(struct runner_priv *p) -> int`** — `:709-798`.
  The Runner-microcode load. **Emulated short-circuit** (`:714-720`): if
  `runner_emulated` or DT `brcm,runner-emulated`, log "emulated mode", set
  `fw_loaded=false`, return 0 (no firmware needed against the QEMU model).
  Otherwise `request_firmware(RUNNER_FW_NAME)`; **absence is non-fatal**
  (`:722-729`): warn that the HW datapath will not move packets, set
  `fw_loaded=false`, return **0** so the driver still binds for emulation. On
  success it parses the `RFW1` container (§2.2/§3.1), validates magic + table +
  per-core extents (rejects with `-EINVAL`), and for each core writes the INST
  image via `iowrite32be(get_unaligned_le32(...))` to `rnr_mem[c]+0x10000` and
  the PRED image u16→BE-u32 (stride 4) to `rnr_mem[c]+0x1c000` (§2.2 endianness).
  Sets `fw_loaded=true`, `release_firmware`, returns 0.
  **Hardware sequencing:** must run **after** `runner_rnr_precfg()` (zeroes the
  core SRAM) and **before** `runner_rnr_enable()` (core wakeup) — enforced by
  probe order (`:1763-1764`, `:1794`). Its `-EINVAL` returns are the only
  microcode failures that **abort probe** (`:1765-1766`); a plain absent file
  does not. Caller: `runner_probe` (`:1764`).

### 4.4 `driver/runner/bcm4916_runner.c` — SerDes µC loader + helpers

- **`serdes_xfer(p, reg, mask, val, write, *out) -> int`** — `:1517-1547`. The
  primitive indirect-window transfer. Computes the per-core window
  `p->serdes + serdes_core*0x100`, builds the encoded addr
  `(SERDES_PMD_DEV<<27)|(serdes_lane<<16)|reg`, writes ADDR; for a write posts
  `~mask` to INDIR_MASK and CNTRL = `val | START_BUSY | DELAYED_ACK`; for a read
  posts CNTRL = `RW | START_BUSY | DELAYED_ACK`. Spins on `START_BUSY` up to
  **1000 µs** (`udelay(1)`), `-ETIMEDOUT` on timeout; on read returns
  `cntrl & mask` via `*out`. Touches the SerDes indirect window only. Callee of
  all wrappers below.

- **`serdes_wr_reg(p, reg, val)`** — `:1549-1552`. Full-word write
  (`mask=0xffff`). Inline wrapper over `serdes_xfer`.

- **`serdes_wr_f(p, reg, mask, shift, v)`** — `:1554-1558`. Field write:
  `serdes_xfer(reg, mask, (v<<shift)&mask, write)`. Inline wrapper.

- **`serdes_rd(p, reg, mask, *out)`** — `:1559-1562`. Masked read. Inline wrapper.

- **`serdes_poll_ra_initdone(p) -> int`** — `:1564-1576`. Polls
  `SRD_MICRO_AHB_STATUS & SRD_MICRO_RA_INITDONE` (mask `0x8001`) up to **250 µs**;
  `-ETIMEDOUT` + `dev_warn` on timeout. Called twice by `runner_serdes_load`
  around the two `ra_init` phases. The mask deliberately tolerates bit0|bit15
  because which bit signals "done" is an unresolved RE ambiguity (finding §5.6).

- **`runner_serdes_load(struct runner_priv *p) -> int`** — `:1578-1670`. The
  full SerDes-µC PRAM load. `request_firmware(SERDES_FW_NAME)`; on absence
  `dev_warn` + return the error (`:1592-1597`). **Size is checked** against
  `SERDES_FW_SIZE = 31664` but only warns on mismatch and continues; **CRC is
  never checked** (finding §5.4). Then: assert µC reset + drive the micro
  register block to reset defaults (the `zero_regs[]` list + `0xD216=0x0007`,
  `0xD225=0x8201`, `0xD228=0x0101`), toggle subsystem reset, run the two
  `ra_init` code/data-RAM phases (each `serdes_poll_ra_initdone`, `goto out` on
  failure), set the write port to autoinc/16-bit @ addr 0, **stream the blob as
  16-bit LE words** into `RA_WRDATA_LSW` (odd trailing byte handled at `:1641`),
  release/start the µC, and poll `uc_active` (bit15) up to **10000 µs**. Logs
  "uC ACTIVE" (ret 0) or "uC NOT active" (`-EIO`). `release_firmware` on the
  `out:` path. **Hardware sequencing:** the reset-defaults → RAM-init →
  stream → release → uc_active order is load-bearing; timeouts (1000/250/10000)
  are guesses pending silicon (finding §5.9). Caller: `runner_probe` (`:1789`),
  **but the return value is ignored** (finding §5.3).

- **probe wiring** — `runner_probe` (`:1672`): ioremaps the SerDes window only
  when `!runner_emulated && serdes_fw_load` (`:1733-1738`); calls
  `runner_load_microcode(p)` at `:1764` (abort-on-`-EINVAL`); calls
  `runner_serdes_load(p)` at `:1788-1789` (return dropped). `MODULE_FIRMWARE`
  for the Runner blob at `:1899`.

### 4.5 `driver/pcs/pcs-bcm-xport.c` — the SerDes-firmware stub (second path)

- **`bcm_xport_serdes_core_init(xp, core)`** — `:149-162`. Not a firmware
  function but its documented next step *is* the µC load: releases IDDQ / refclk
  reset / serdes reset and enables comclk on a core via the (different, direct)
  SerDes register map used by the PCS driver. Comment (`:144-147`) says real HW
  must then "PRAM-load the Merlin microcode + verify CRC → see
  bcm_xport_pcs_load_firmware() TODO."

- **`bcm_xport_pcs_load_firmware(xp, core) -> int`** — `:167-175`. **Stub.**
  Returns 0 immediately if `emulated`; otherwise `dev_warn_once` that the load
  is not implemented and "link will not come up on real HW", returns 0. Does
  **not** `request_firmware`, does **not** touch PRAM. Called for cores 0 **and**
  1 from `bcm_xport_pcs_enable` (`:186-187`). This is the mainline-PCS path's
  placeholder for the very load `runner_serdes_load()` actually implements
  (finding §5.2).

- **`bcm_xport_pcs_enable(pcs)`** — `:177-189`. Brings up MPCS + both serdes
  cores then calls the two `load_firmware` stubs. Context only.

---

## 5. Audit findings

### 5.1 [HIGH] Extractor emits `B4916UC`; driver only accepts `RFW1` — the extracted blob is rejected
The `extract-runner-microcode.sh` container header is `"B4916UC\0"` with an
`{inst_sz,pred_sz}` layout and raw concatenated payload (`tools/extract-runner-microcode.sh:37-47,129`;
doc §3.6). The driver's `runner_load_microcode()` accepts **only** magic
`"RFW1"` with a per-core `{inst_off,inst_len,pred_off,pred_len}` table
(`bcm4916_runner.c:745`, §2.2). A blob produced by the shipped extractor is
therefore **rejected** ("bad magic or short", `-EINVAL`) and, because that
`-EINVAL` aborts probe (`:1765-1766`), the driver fails to bind. The canonical
`RFW1` blob was built by a **separate generator not in this repo**
(`/tmp/gen_runner_fw.py` on the build host, per `10-runner-bringup-spec.md:174`).
Net: **no committed tool produces a loadable Runner blob.** Either the extractor
must be updated to emit `RFW1` (add the 128-byte core table, drop the
`B4916UC` header) or the missing `gen_runner_fw.py` must be committed.

### 5.2 [HIGH] Two divergent SerDes-firmware implementations for the same block
`runner_serdes_load()` is a complete streaming PRAM loader
(`bcm4916_runner.c:1578`), while `bcm_xport_pcs_load_firmware()` for the same
Merlin16 block is a warn-only TODO that never loads anything
(`pcs-bcm-xport.c:167`). The two drivers also disagree on which cores to touch:
the runner path programs a single `serdes_core` (module param), the PCS path
hardcodes cores 0 and 1 (`:186-187`). Only one can be the real bring-up path on
silicon; today they contradict each other and neither has been silicon-proven.

### 5.3 [MED] `runner_serdes_load()` return value is ignored
Probe calls `runner_serdes_load(p);` and discards the result
(`bcm4916_runner.c:1788-1789`). A `request_firmware` failure, a µC-init timeout,
or `uc_active==0` (`-EIO`) does not surface to probe; the driver binds reporting
success even though the 10G line side never came up. At minimum the failure
should be logged/propagated (it already logs internally, but the caller's silent
drop makes the opt-in feel like a no-op on failure).

### 5.4 [MED] SerDes CRC 0x4949 is never verified by the driver
The blob's integrity check (`CRC-16/CCITT-FALSE == 0x4949`) is computed by the
extractor (`extract-serdes-fw.py:33-39,69`) and named as "the same check the
driver uses", and the header/TODO comments all reference CRC verification
(`bcm4916_runner.h:331`, `pcs-bcm-xport.c:164-166`). But `runner_serdes_load()`
checks **only the size** (`bcm4916_runner.c:1598-1600`) and streams whatever
bytes it got — a truncated/corrupt-but-right-sized image would be loaded and
only caught (if at all) by the `uc_active` poll. Add the CRC-16/CCITT check
before streaming.

### 5.5 [LOW] No `MODULE_FIRMWARE` for the SerDes blob
`MODULE_FIRMWARE(RUNNER_FW_NAME)` is declared (`bcm4916_runner.c:1899`) but there
is no `MODULE_FIRMWARE(SERDES_FW_NAME)`, so `modinfo`/initramfs tooling will not
know to bundle `brcm/merlin16-shortfin.bin`.

### 5.6 [MED] `RA_INITDONE` mask encodes an unresolved RE ambiguity
`SRD_MICRO_RA_INITDONE = 0x8001` "tolerate bit0|bit15 (RE ambiguity)"
(`bcm4916_runner.h:359-360`). The poll (`serdes_poll_ra_initdone`,
`bcm4916_runner.c:1564`) succeeds on *either* bit, so a spurious set of the
wrong bit could pass a RAM-init that did not actually complete. Which bit is the
real "done" flag must be pinned on silicon.

### 5.7 [LOW] `sym_offsize()` ELF parse is fragile across binutils versions
The section-header VA→offset parse (`tools/extract-runner-microcode.sh:110-116`)
relies on positional `awk` heuristics for `readelf -SW` output, whose column
layout varies by binutils version and section-flag width. A mis-parse yields a
wrong file offset and a silently corrupt extraction (only the final size check
would catch a gross error). Prefer a structured reader (e.g. `readelf`
`--hex-dump`/`objcopy --dump-section`, or pyelftools).

### 5.8 [LOW] `extract-serdes-fw.py` writes the output even on size/CRC mismatch
`main()` warns on mismatch but writes the `.bin` regardless, returning exit 1
(`extract-serdes-fw.py:66-76`). A caller that ignores the exit code gets a bad
blob on disk with a plausible name. Consider refusing to write (or writing to a
`.bad` name) on mismatch.

### 5.9 [MED] SerDes poll timeouts and reset-recipe are hardcoded/guessed pending silicon
The `serdes_xfer` busy spin (1000 µs), `serdes_poll_ra_initdone` (250 µs) and
the `uc_active` poll (10000 µs) are magic loop counts
(`bcm4916_runner.c:1536,1569,1654`), and the µC reset-defaults recipe (the
`zero_regs[]` list + `0xD216/0xD225/0xD228` writes, `:1582-1611`) is a
transcribed opaque sequence from the SDK `uc_reset`. None has been validated
against real Merlin timing (the device work is read-only; `xport-serdes-bringup.md`
§7 lists this as an open real-silicon gap).

### 5.10 [MED] Driver comment claims a GPL origin for the Runner microcode that the RE verdict contradicts
`runner_load_microcode()`'s comment calls the `RFW1` blob "built from the SDK's
GPL per-core microcode arrays" (`bcm4916_runner.c:732-733`), and
`10-runner-bringup-spec.md:178` tags it "GPLv2+linking-exception → MODULE_FIRMWARE
ok". This directly contradicts the dedicated verdict in
`runner-microcode-and-cpuring.md` (§A) and `gpl-source-inventory.md`: the 4916
Runner microcode is **absent from the GPL SDK** and exists only inside the
Proprietary `rdpa.ko`; the `extract-runner-microcode.sh` path (pulling
`fw_binary_*` from `rdpa.ko`) reflects that reality. The "GPL arrays" framing
appears to be residual optimism from the older 63138/416L05 SDK that
`runner-microcode-and-cpuring.md` explicitly corrects. The comment/spec should
be reconciled to the proprietary verdict so the licensing story in the code is
not misleading.

### 5.11 [INFO] Firmware absence is handled asymmetrically
Runner-microcode absence is non-fatal and lets the driver bind (`:722-729`,
by design for emulation); SerDes-fw absence returns an error from the loader but
the caller drops it (§5.3). The two "missing blob" behaviors differ; document the
intended contract (bind-anyway vs feature-off) so operators can tell a broken
install from an intended slow-path-only run.

---

## 6. Open questions / unknowns

1. **Where is the canonical `RFW1` generator?** The driver parses `RFW1` but the
   only committed extractor emits `B4916UC`, and the real generator
   (`gen_runner_fw.py`) lives only in build-host `/tmp`
   (`10-runner-bringup-spec.md:174`). Its exact packing (esp. how it sources the
   per-core inst/pred — from `rdpa.ko` or from claimed "GPL arrays", cf. §5.10)
   could not be determined from the repo and must be recovered or rewritten.
2. **Runner-microcode provenance/licensing, definitively.** §5.10: are there
   *any* GPL per-core arrays for the 4916 (none found in the SDK per
   `gpl-source-inventory.md`), or is `rdpa.ko` extraction the only route (making
   the blob strictly non-redistributable)? This determines whether *any* legal
   ship path exists.
3. **PRED image encoding on silicon.** The driver expands 1 KB of u16 into 512
   BE-u32 SRAM slots (§2.2); the spec confirms "pred is uint16_t = 1024B (NOT
   u32)" but the *SRAM slot stride/endianness* was inferred from the stock loader,
   not observed. Needs a live RNR_PRED read-back.
4. **Runner INST SRAM endianness** is taken from the stock loader's byte-swap
   (`iowrite32be`); unverified against a live RNR_INST dump.
5. **SerDes core/lane mapping for the actual 10G port.** `serdes_core`/`serdes_lane`
   default to 0 and the lane→port mux (`serdes_ln_offset`) is not programmed
   (`xport-serdes-bringup.md` §7); the correct core/lane for the device's 10G
   port is unknown until a live regdump.
6. **Which SerDes bring-up path is authoritative** — the runner-driver
   `runner_serdes_load()` or the PCS `bcm_xport_pcs_load_firmware()` stub (§5.2)?
   Both target the same block via different windows/register maps; only silicon
   can say which register model is correct.
7. **The `RA_INITDONE` bit** (§5.6), the **µC reset-recipe** correctness, and all
   **poll timeouts** (§5.9) are unresolved without real Merlin hardware — QEMU
   fakes PMD/PCS lock and does not exercise the PRAM load or µC timing at all
   (`xport-serdes-bringup.md` §7).
8. **`uc_active` semantics.** The driver treats bit15 of `0xd0f4` as "fw running"
   (`bcm4916_runner.h:366-367`); whether that bit alone is sufficient evidence
   the PMD microcode is executing (vs merely clocked) is unconfirmed on silicon.
