# 06 — Merlin16-Shortfin SerDes firmware: load path + host API ABI

Reverse-engineering notes for the 10G XPORT **Merlin16-Shortfin** SerDes PMD
microcode (the ~31 KB image the SerDes microcontroller runs) and, more usefully,
the **load path and host register ABI** that drives it. Unlike the Runner side,
this stack is **source-available** (GPL C), so this pass **reads the C** — no
disassembly was required or performed. The image *contents* stay opaque (custom
uC ISA), but everything the open host driver must do — reset the uC, stream the
image into program-RAM, release it, confirm it runs, CRC-verify, and bring up
the PLL/VCO — is fully known from source.

The blob itself is proprietary / non-redistributable; **no image is committed**
(repo `.gitignore` blocks `*.bin`/`*.o`/`*.ko` and none is placed in the tree).
The open driver loads it at runtime via `request_firmware("brcm/merlin16-shortfin.bin")`.

## Purpose

The open driver has two places that must load this microcode:

* `driver/runner/bcm4916_runner.c` → `runner_serdes_load()` (`:1568-1668`), which
  already streams the blob via the RAM-access interface — an **educated
  reconstruction** carrying two self-flagged RE ambiguities (`SRD_MICRO_RA_INITDONE
  = 0x8001` "tolerate bit0|bit15", and no CRC verify).
* `driver/pcs/pcs-bcm-xport.c` → `bcm_xport_pcs_load_firmware()` (`:167-175`),
  still a **warn-once stub** ("microcode load not implemented … link will not
  come up on real HW").

This note verifies the load sequence against the stock GPL driver
(`bcmdrivers/opensource/phy/merlin_shortfin` + `phy_drv_shortfin.c`), pins every
register/bitfield the sequence touches, resolves (or honestly bounds) the two
ambiguities, and characterises the image framing. It closes audit gap **M4**
(`docs/audit/10-reimplementation-guide.md` §M4 "10G line side: SerDes PMD
microcode + PCS completion") and **H1** (`docs/audit/04-pcs-serdes.md` §H1
"SerDes PMD microcode load is unimplemented"), and pins the firmware facts in
`docs/audit/09-hardware-acceleration.md` §6 / §1.

**Ceiling (honest).** The uC image is an ARM Cortex-M0 (ARMv6-M Thumb) firmware
whose *behaviour* is opaque; we do not and need not decode it. The load path,
the host register interface, the CRC mechanism, and the PLL/VCO bring-up are all
100% source-derived. The only genuinely unresolved bit-position (`ra_initdone`)
is unresolved *inside the SDK's own two header families*, not by our method.

## Method

**No disassembly.** All facts come from reading the GPL C source of the stock
Merlin16-Shortfin driver on the build host (read-only over SSH). Raw provenance
saved to `~/re-scratch/serdes-fw/provenance.txt`. Source tree:

```
SDK = .../asuswrt-merlin.ng/release/src-rt-5.04behnd.4916   (staged on the build host)
D   = $SDK/bcmdrivers/opensource/phy/merlin_shortfin
```

Files read (bounded `sed`/`grep`, never the full ucode array):

| Source | What it gives |
|---|---|
| `D/merlin16_shortfin_ucode_image.h` | image array decl + `SIZE`/`VERSION`/`CRC` macros; first 8 bytes (M0 vector table); embedded Info-table header |
| `D/src/merlin16_shortfin_config.c:712` `ucode_mdio_load` | **the active load path** (RAM-access streaming) |
| `D/src/merlin16_shortfin_config.c:210` `ucode_pram_load` | alt PRAM/AHB path (unused here — see §ABI) |
| `D/src/merlin16_shortfin_config.c:483` `uc_reset_with_info` | uC reset assert/de-assert register writes |
| `D/src/merlin16_shortfin_config.c:546` `wait_uc_active` | uc_active poll |
| `D/src/merlin16_shortfin_config.c:94/109/114` `ucode_crc_verify` etc. | CRC-verify via uC command |
| `D/src/merlin16_shortfin_config.c:763` `ucode_load_verify` | host read-back verify (alt to CRC) |
| `D/src/merlin16_shortfin_config.c:645-711` `configure_pll_*` | PLL divider/VCO API |
| `D/serdes_wrapper.c:34,77-93` | register **transport**: C45 `PMD_DEV=1`; `pmd_wr_pram` is a stub |
| `D/include/merlin16_shortfin_fields.h`, `include/public/merlin16_shortfin_fields_public.h` | per-register `{addr,mask,shift}` + `extract_field` bit positions |
| `D/include/common/srds_api_uc_common.h:121,222-257` | `CMD_CALC_CRC=20`; Info-table layout |
| `D/include/merlin16_shortfin_common.h:52` | `UCODE_MAX_SIZE = 84*1024` |
| `$SDK/bcmdrivers/opensource/phy/phy_drv_shortfin.c:77` `_serdes_core_init` | **the orchestration** that sequences load → active → CRC → PLL/VCO → datapath reset |

Cross-checked against the open driver at `driver/runner/bcm4916_runner.c`,
`driver/runner/bcm4916_runner.h`, `driver/pcs/pcs-bcm-xport.c`.

## ABI reference

### 1. Register transport (how you reach any merlin16 register)

Every merlin16 register named below is a **16-bit PMD register** in MDIO
Clause-45 **device address 1** (`serdes_wrapper.c:34` `#define PMD_DEV 1`). The
stock host wrappers are thin:

| Wrapper (`serdes_wrapper.c`) | Lowers to |
|---|---|
| `merlin16_shortfin_pmd_wr_reg(sa,addr,val)` (`:82`) | `phy_dev_c45_write(sa, 1, addr, val)` |
| `merlin16_shortfin_pmd_rdt_reg(sa,addr,&val)` (`:77`) | `phy_dev_c45_read(sa, 1, addr, &val)` |
| `merlin16_shortfin_pmd_mwr_reg(sa,addr,mask,lsb,val)` (`:87`) | `phy_dev_c45_write_mask(sa, 1, addr, mask, lsb, val)` |
| `merlin16_shortfin_pmd_wr_pram(sa,val)` (`:90`) | **stub — returns 0** |

The open driver reaches the *same* registers through the memory-mapped indirect
ADDR/MASK/CNTRL window at `0x837ff500` (per-core stride `0x100`), encoding the
C45 address as `(PMD_DEV<<27) | (lane<<16) | reg`
(`bcm4916_runner.c:1517` `serdes_xfer`, `bcm4916_runner.h:341-346`). The two
transports are equivalent; the register/bit facts below hold for both.

### 2. Micro-subsystem register map (dev1 space)

Addresses and bit positions are from the generated field headers; bit positions
verified against `extract_field()` (`_public.h`) and, where the C uses a literal
mask, against that literal.

| Reg | Block | Field → bits | Notes |
|---|---|---|---|
| `0xd200` | `MICRO_A_COM_CLK_CONTROL0` | `master_clk_en`=bit0, `core_clk_en`=bit1 | clock gates |
| `0xd201` | `MICRO_A_COM_RESET_CONTROL0` | `master_rstb`=bit0, `core_rstb`=bit1, `pram_if_rstb`=bit3 | active-low de-asserts |
| `0xd202` | `MICRO_A_COM_AHB_CONTROL0` | `ra_wrdatasize`=[1:0], `ra_rddatasize`=[5:4], `ra_init`=[9:8], `autoinc_wraddr_en`=bit12, `autoinc_rdaddr_en`=bit13 | RAM-access control |
| `0xd203` | `MICRO_A_COM_AHB_STATUS0` | `ra_initdone` | **bit0** per RDB `exc_` (fields.h implies bit15 — see §5) |
| `0xd204` / `0xd205` | AHB WRADDR | `ra_wraddr_lsw` / `ra_wraddr_msw` (full 16b) | write start addr |
| `0xd206` | AHB WRDATA | `ra_wrdata_lsw` (full 16b) | streaming write port |
| `0xd208` / `0xd209` | AHB RDADDR | `ra_rdaddr_lsw` / `ra_rdaddr_msw` | read-back start addr |
| `0xd20a` | AHB RDDATA | `ra_rddata_lsw` (read, auto-inc) | read-back port |
| `0xd20c` | `PRAMIF_CONTROL0` | `micro_pramif_en`=bit0 | PRAM path (unused, §3) |
| `0xd20d` / `0xd20e` | `PRAMIF_AHB_WRADDR` | `_lsw`=[15:2], `_msw` (16b) | PRAM path |
| `0xd216` | reset default | write `0x0007` in uc_reset(1) | |
| `0xd225` | reset default | write `0x8201` in uc_reset(1) | |
| `0xd228` | `PMI` | write `0x0101` in uc_reset(1); `pmi_hp_fast_read_en`=bit0 | cleared to 0 on release |
| `0xd0f4` | `DIG_COM_TOP_USER_CONTROL_0` | `uc_active`=**bit15** | "uC running" gate (`extract_field(v,15,15)`) |
| `0xd00d` | uC cmd/status | `uc_dsc_ready_for_cmd`=bit7 (`0x0080`), `uc_dsc_error_found`=bit6, `uc_dsc_supp_info`=[15:8] | command handshake |
| `0xd00e` | uC cmd data | `uc_dsc_data` (16b, R/W) | command operand / result (CRC out) |
| `0xd0f2` | | bit12 = clock select | set to 1 post-load |

### 3. The image is streamed via the RAM-access (RA) interface — not PRAM

Two loaders exist in `config.c`. On this 4916 build the **PRAM/AHB path
(`ucode_pram_load`, `:210`) is inert**: it depends on
`merlin16_shortfin_pmd_wr_pram()`, which is a **stub returning 0**
(`serdes_wrapper.c:90`). The **active path is `ucode_mdio_load` (`:712`)**, which
streams through the RAM-access registers (`0xd204/5/6`). This is what
`phy_drv_shortfin.c:77` calls and what the open driver mirrors — so the open
driver correctly targets the RA interface, **not** the PRAM interface.

### 4. Full uC bring-up sequence (`_serdes_core_init`, `phy_drv_shortfin.c:77-120`)

Ordered register/API sequence per SerDes core. Each step cites the field it
writes; `→v` is the written value.

**(a) Assert uC reset** — `uc_reset(1)` (`config.c:483`):
1. `core_clk_en →0` (`0xd200` bit1), `master_clk_en →0` (`0xd200` bit0).
2. Write `0x0000` to the micro register block: `0xD200,D201,D202,D204..D20E,
   D211..D21B,D220,D221,D224,D226,D229,D22A` (D203/status is **not** written).
3. Write the three non-zero reset defaults: `0xD216→0x0007`, `0xD225→0x8201`,
   `0xD228→0x0101`.

**(b) RAM init + stream** — `ucode_mdio_load` (`config.c:712`):
4. `master_clk_en →1`; toggle `master_rstb`: `→1, →0, →1` (`0xd201` bit0).
5. `ra_init →1` (`0xd202`[9:8]); poll `ra_initdone` (250 ms) — code-RAM init.
6. `ra_init →2`; poll `ra_initdone` — data-RAM init.
7. `ra_init →0` (clear).
8. `autoinc_wraddr_en →1` (`0xd202` bit12); `ra_wrdatasize →1` (16-bit,
   `0xd202`[1:0]); `ra_wraddr_msw →0`, `ra_wraddr_lsw →0`.
9. Stream the image **16 bits at a time** to `ra_wrdata_lsw` (`0xd206`), each
   word = `image[2k] | (image[2k+1]<<8)`, for `ceil(len/2)` words, zero-padded to
   a 4-byte boundary (`(len+3)&~3`). For this image `len=31664` is already
   4-aligned → exactly 15832 word writes, no padding.
10. `ra_wrdatasize →2` (restore 32-bit default); `core_clk_en →1`.

**(c) (optional) verify** — two independent mechanisms:
* **Host read-back** — `ucode_load_verify` (`config.c:763`): `autoinc_rdaddr_en
  →1`, `ra_rddatasize →1`, `ra_rdaddr →0`, then read `ra_rddata_lsw` (`0xd20a`)
  word-by-word and compare to the image. No uC needed.
* **uC-computed CRC** — see §5.

**(d) Release / start uC** — `uc_reset(0)` (`config.c:520`):
11. `master_clk_en →1`; `master_rstb →1`; `core_clk_en →1`;
    `pmi_hp_fast_read_en →0` (`0xd228` bit0, "prevent micro exceptions when
    REFCLK absent"); `core_rstb →1` (`0xd201` bit1) — **uC starts executing.**

**(e) Confirm running** — `wait_uc_active` (`config.c:546`): poll `uc_active`
(`0xd0f4` bit15) nonzero, up to 10000 iterations with a 1 µs delay after the
first 10.

**(f) CRC verify** — `ucode_crc_verify(len=31664, expected=0x4949)` — §5.

**(g) Info-table init** — `init_merlin16_shortfin_info` parses the embedded
Info table (§6) into the driver's `srds_info_t`.

**(h) PLL/VCO + datapath** — §7.

### 5. CRC verification is a uC *command*, not a host algorithm

`ucode_crc_verify` (`config.c:94`) does **not** run a CRC over the image on the
host. It issues the uC command `CMD_CALC_CRC = 20`
(`srds_api_uc_common.h:121`) through the command interface and reads the result:

* Command handshake register `0xd00d`: `uc_dsc_ready_for_cmd` = **bit7** (`0x0080`)
  — confirmed by the literal test `if (rddata & 0x0080)` in
  `merlin16_shortfin_internal.c:2005`.
* Command data / result register `0xd00e` (`uc_dsc_data`, 16-bit).
* Flow: poll `ready_for_cmd`==1 → write cmd+`len` → poll `ready_for_cmd`==1 →
  read `uc_dsc_data` = calculated CRC → compare to `expected` (`0x4949`).
  (`start_ucode_crc_calc`/`check_ucode_crc` at `:109/:114` split the same into
  fire-and-poll halves.)

Consequence for the open driver: to add CRC verification it must talk to the
**running** uC (steps d/e must have completed) via `0xd00d`/`0xd00e`; a host-side
CRC library cannot reproduce it without knowing the uC's internal polynomial.
The cheaper alternative is the host read-back verify (§4c) which needs no uC.
`0x4949` is thus an *expected uC output*, not a computable image checksum.

### 6. Image framing

From `merlin16_shortfin_ucode_image.h`:

| Fact | Value |
|---|---|
| C symbol | `unsigned char merlin16_shortfin_ucode_image[31664]` |
| `MERLIN16_SHORTFIN_UCODE_IMAGE_SIZE` | **31664** bytes (0x7BB0), 4-byte aligned |
| `MERLIN16_SHORTFIN_UCODE_IMAGE_VERSION` | `"D102_0A"` |
| `MERLIN16_SHORTFIN_UCODE_IMAGE_CRC` | `0x4949` (16-bit; a uC-computed value, §5) |
| `UCODE_MAX_SIZE` (RAM budget guard) | `84*1024 = 86016` |

The image is a raw ARM **Cortex-M0** (ARMv6-M / Thumb) firmware, no wrapper
header: the first 8 bytes are the M0 vector table — initial `SP = 0x200007f0`,
reset vector = `0x0000027d` (Thumb bit set → entry `0x27c`). No length/CRC
framing precedes the code; `SIZE`/`CRC`/`VERSION` live only in the C `#define`s,
so a runtime `.bin` is the naked byte array.

An **Info table** is embedded at image offset `0x100`
(`INFO_TABLE_RAM_BASE`, `srds_api_uc_common.h:222`):

| Info offset | Field | For D102_0A |
|---|---|---|
| `+0x00` | signature | `0x36666E49` = ASCII `"Inf6"` (LE bytes `49 6e 66 36`) |
| `+0x04` | uc_version | encodes `D102_0A` (bytes `0a 02 d1 00`) |
| `+0x08` | trace_lane_mem_size | |
| `+0x0C` | other_size | |
| `+0x10` / `+0x14` / `+0x18` / `+0x1C` | trace/core/icore/lane mem pointers | consumed by `init_merlin16_shortfin_info` |

### 7. PLL/VCO + datapath bring-up (post-load, `phy_drv_shortfin.c:104-118`)

After the uC is active and CRC-verified, `_serdes_core_init` finishes the line
side. This is the "STEP 2" the open driver does **not** yet implement:

1. `phy_dev_prog_ext(PMD_setup_50_10p3125_VCO)` — a register-sequence table that
   programs the PLL for a 50 MHz reference → 10.3125 Gbps VCO. (The table is an
   opaque `prog_ext` blob; not fully decoded this pass.)
2. Read core config (`get_uc_core_config`), set `core_conf.field.vco_rate =
   10.3125*4 - 22 = 19.25`, write back (`INTERNAL_set_uc_core_config`). The
   encoding is fixed by `merlin16_api_uc_common.h:81-92`:
   `vco_rate = f_GHz*4 - 22`; inverse `VCO_RATE_TO_MHZ(r) = (r+22)*250` →
   `(19.25+22)*250 = 10312.5 MHz` = 10.3125 GHz. **High confidence** (pure math).
3. `phy_dev_prog_ext(datapath_reset_core)` — core datapath reset. The typed APIs
   behind this are `core_dp_reset`/`tx_dp_reset`/`rx_dp_reset` (`config.c:178-208`),
   which toggle `core_dp_s_rstb` / `tx_s_rstb` / `rx_s_rstb`.
4. `phy_dev_c45_write_mask(dev1, 0xd0f2, BIT(12), 12, 1)` — "clock select".

Per-lane config (media type, DFE, AN/force mode) is a further stage
(`_serdes_lane_uc_cfg`, `phy_drv_shortfin.c:135+`) writing the uC lane-config RAM
variables — out of scope for the *firmware-load* gap but noted as the next layer.

## Mapping to the open driver

| # | RE'd fact | Open-driver placeholder | Status | Conf. |
|---|---|---|---|---|
| 1 | Image = 31664 B, ver `D102_0A`, CRC `0x4949` (uC-computed) | `bcm4916_runner.h:369` `SERDES_FW_SIZE 31664`; `pcs-bcm-xport.c:164` comment "~31 KB, ver D102_0A, CRC 0x4949" | **confirms**; corrects the task's "~192 KB" to 31664 B | high |
| 2 | Regs = C45 `PMD_DEV=1`; indirect addr `(dev<<27)|(lane<<16)|reg` | `bcm4916_runner.h:343` `SERDES_PMD_DEV 1`; `bcm4916_runner.c:1517` `serdes_xfer` | **confirms** | high |
| 3 | Micro reg map `0xd200/d201/d202/d204/d205/d206` + field bits | `bcm4916_runner.h:348-363` `SRD_MICRO_*` | **confirms** | high |
| 4 | `uc_active` = `0xd0f4` **bit15** (`extract_field(v,15,15)`) | `bcm4916_runner.h:366-367` `SRD_UC_ACTIVE 0x8000` | **confirms** | high |
| 5 | `ra_initdone` = `0xd203`: RDB `exc_` says **bit0**, `fields.h` says bit15 — genuine intra-SDK conflict | `bcm4916_runner.h:360` `SRD_MICRO_RA_INITDONE 0x8001` ("tolerate bit0\|bit15") | **confirms the ambiguity is real**; the tolerant mask is justified. Cleanest fix: poll full `0xd203` nonzero (matches stock `if(result)`), or pin bit0 (`0x0001`) | med |
| 6 | `uc_reset(1)` zero-reg set + `D216=0x0007,D225=0x8201,D228=0x0101` | `bcm4916_runner.c:1571-1611` `zero_regs[]` + 3 literal writes | **confirms** (exact set incl. omission of D203) | high |
| 7 | Load order: reset → RAM-init(1,2,0) → stream 16b to `0xd206` → wrdatasize=2 → release → uc_active | `runner_serdes_load` `bcm4916_runner.c:1568-1668` | **confirms** | high |
| 8 | CRC verify = uC cmd `CMD_CALC_CRC=20` via `0xd00d`(bit7)/`0xd00e`, compare `0x4949`; alt host read-back via `0xd208/9/a` | `runner_serdes_load` does **only** `uc_active`, no CRC | **newly-pins** the (currently missing) optional verify step | high |
| 9 | Entire §4 sequence is the content of the PCS-path loader | `pcs-bcm-xport.c:167-175` `bcm_xport_pcs_load_firmware` warn-once **stub** | **fills the stub** (call `runner_serdes_load` or replicate §4) | high |
| 10 | Post-load PLL/VCO: `PMD_setup_50_10p3125_VCO`, `vco_rate=19.25`→10.3125 GHz, `datapath_reset_core`, `0xd0f2` bit12 | neither loader does line bring-up (`pcs-bcm-xport.c` `pcs_config` returns 0 for 10GBASE-R) | **newly-pins** the STEP-2 follow-on; `vco_rate` math high, `prog_ext` tables low | med |
| 11 | PRAM path inert (`pmd_wr_pram` stub) → RA interface is the real path | open driver already uses RA interface | **confirms** the design choice | high |

## Unresolved

* **`ra_initdone` bit position (`0xd203`).** The SDK's two header families
  disagree: the RDB-generated `exc_merlin16_shortfin_MICRO_A_COM_AHB_STATUS0__micro_ra_initdone`
  = `extract_field(v,0,0)` (**bit0**), while `merlin16_shortfin_fields.h`'s
  `rdc_micro_ra_initdone` encodes `(0xd203,15,15)` (**bit15**). The poll only
  checks the isolated field nonzero, so stock works under either reading; the
  physical bit cannot be settled from source alone. A live read of `0xd203`
  right after `ra_init` on silicon would resolve it. Until then the open
  driver's tolerant `0x8001` mask is the safe choice.
* **`prog_ext` register tables** (`PMD_setup_50_10p3125_VCO`,
  `datapath_reset_core`, `usxgmii_switch_pcs_*`). These are opaque ordered
  {addr,val} sequences applied by `phy_dev_prog_ext`; the *effect* (50 MHz→
  10.3125 GHz VCO, core datapath reset, PCS A/B select) is documented but the
  exact per-register writes were not enumerated this pass — they belong to the
  PCS/AN completion stage (M4 STEP 2), not the firmware-load stage (this note).
* **uC ISA / image behaviour.** Deliberately not reversed: the Cortex-M0 image
  is treated as an opaque blob (same posture as the Runner microcode). Only its
  container framing (§6) is characterised.
* **`uc_dsc` command encoding beyond CRC.** Only `CMD_CALC_CRC` was traced; the
  full command opcode table (`srds_pmd_uc_cmd_enum`) exists but is not needed for
  the load path.

## Sources

Build host, read-only, `~/re-scratch/serdes-fw/provenance.txt`. Paths relative to
`$SDK = .../src-rt-5.04behnd.4916`, `$D = $SDK/bcmdrivers/opensource/phy/merlin_shortfin`.

* `$D/merlin16_shortfin_ucode_image.h:1-5` — SIZE/VERSION/CRC + array; first
  16 bytes (M0 vector table + Info-table @0x100).
* `$D/src/merlin16_shortfin_config.c` — `ucode_mdio_load:712`, `ucode_pram_load:210`,
  `uc_reset_with_info:483`, `wait_uc_active:546`, `ucode_crc_verify:94`,
  `ucode_load_verify:763`, `configure_pll_*:645-711`.
* `$D/serdes_wrapper.c:34,77-93` — `PMD_DEV 1`, C45 wrappers, `pmd_wr_pram` stub.
* `$D/include/merlin16_shortfin_fields.h` + `include/public/merlin16_shortfin_fields_public.h`
  — register `{addr,mask,shift}` and `extract_field` bit positions.
* `$D/include/common/srds_api_uc_common.h:121,222-257` — `CMD_CALC_CRC=20`,
  Info-table layout/signature `0x36666E49`.
* `$D/include/merlin16_shortfin_common.h:52` — `UCODE_MAX_SIZE 84*1024`.
* `$D/src/merlin16_shortfin_internal.c:2005` — literal `& 0x0080`
  (`uc_dsc_ready_for_cmd` = bit7).
* `$SDK/bcmdrivers/opensource/phy/phy_drv_shortfin.c:77-120,400-440` —
  `_serdes_core_init` orchestration + caller.
* `$D/include/merlin16_api_uc_common.h:78-92` — `vco_rate` encoding.

Open-driver cross-refs: `driver/runner/bcm4916_runner.c:1517-1668`,
`driver/runner/bcm4916_runner.h:341-370`, `driver/pcs/pcs-bcm-xport.c:167-175`.
