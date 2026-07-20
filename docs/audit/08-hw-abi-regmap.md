# 08 — Hardware register map & ABI reference (BCM4916 / BCM6813 XRDP "Runner")

The hardware reference the whole open driver programs against: block base addresses, per-block
register offsets and bitfields, the CPU-ring descriptor/entry layouts, the flow-offload (NAT-C +
cmdlist) ABI, the Runner core/image model, and the bring-up ordering constraints.

This document **documents hardware**, not C functions. Every value below is distilled from the
reverse-engineering notes; each entry carries a `file:line` reference into `re-notes/` so a driver
author can jump to the provenance. Confidence tags used in the source notes are preserved:

- `[FDT]` — read directly from the live stock device tree (confirmed silicon topology).
- `[KO]` — recovered from the stock proprietary `rdpa.ko` (ELF `*_ADDRS`/`*_REG` decode + targeted
  disasm). These bases are the authoritative ones the device actually uses.
- `[SDK]` — read from the matched 4916/6813 "behnd" GPL SDK source.
- `[SDK-OLD]` — the older 63138/63148 RDP GPL drop (structural template only; bitfields differ).
- `[INFER]` — deduced/cross-referenced.
- `[LIVE]` — captured read-only from live stock silicon (devmem MMIO peek + kprobe oracle).

Source RE notes:
`re-notes/bcm4916-regmap.md`, `re-notes/xrdp-datapath-abi.md`, `re-notes/xrdp-offload-abi.md`,
`re-notes/xrdp-cpu-datapath.md`, `re-notes/runner-microcode-and-cpuring.md`,
`re-notes/realhw/10-runner-bringup-spec.md`, `re-notes/realhw/11-route-a-egress-spec.md`.

---

## 1. Purpose

BCM4916 (marketed silicon behind the ASUS GT-BE98; internally the **BCM6813** XRDP part) is a
Broadcom **Runner / RDPA / XRDP** datapath SoC. The MAC cores (UNIMAC / XLMAC / XPORT) do **not**
DMA into CPU-visible descriptor rings the way a `bcm4908_enet`-style direct-DMA MAC does. They DMA
into Runner-managed buffer pools (FPM / SBPM), and it is the **Runner firmware executing on the
packet-processor cores** that classifies each frame, writes a CPU-RX descriptor into a host DDR
ring on ingress, and on egress pulls the host's TX descriptor out of Runner SRAM and DMAs it out.
There is no hardware bypass / "Runner idle" direct-DMA mode
(`re-notes/xrdp-cpu-datapath.md:13-27`, `:138-141`).

Consequences that shape every table below:

- The open driver must program a large set of XRDP sub-blocks (FPM, SBPM, BBH_RX/TX, DSPTCHR, QM,
  DQM, UBUS_SLV, DMA, per-core RNR) and then hand the Runner cores a microcode image before a single
  frame can move. This is the whole "bring-up ordering" section.
- None of these offsets are exposed in the stock FDT — the FDT gives only the outer `brcm,rdpa`
  window and the MAC/PHY "misc" page. The block bases and register offsets come from RE of
  `rdpa.ko` and the matched GPL/oracle sources (`re-notes/bcm4916-regmap.md:270-276`).
- The Runner microcode is **proprietary and non-redistributable** (embedded only inside
  `rdpa.ko`, `license=Proprietary`, taints the kernel `P`); the open driver loads it via
  `request_firmware()` from a user-supplied image. Firmware handling is described generically here
  (`re-notes/runner-microcode-and-cpuring.md:21-30`).

---

## 2. Address map (all block bases)

### 2.1 Top-level SoC layout `[FDT]`

From the live device tree (`re-notes/bcm4916-regmap.md:22-28`).

| Region | SoC base | size / window | notes |
|---|---|---|---|
| GIC distributor | `0x81001000` | `0x1000` | `arm,cortex-a15-gic`, 3-cell interrupts |
| GIC CPU | `0x81002000` | `0x2000` | |
| `periph` bus | `0xff800000` | `0x62000` | serial/nand/spi/pinctrl/gpio/i2c |
| `xrdp` bus window | `0x82000000` | `0x1000000` (`0x00cd0000` used) | simple-bus parent of switch0/mdio/serdes |
| `rdpa_drv` (whole Runner) | `0x82000000` | `0x00caf004` | `brcm,rdpa`; the entire Runner register window |

The MAC/switch blocks carry **no `interrupts` property** — all datapath IRQs belong to the Runner
via `rdpa_drv` (`re-notes/bcm4916-regmap.md:30-32`).

### 2.2 XRDP block bases (inside the `0x82000000` window) `[KO]`

The authoritative set, decoded from the `*_ADDRS` relocated pointer arrays in `rdpa.ko` (the
device's own module). Table consolidated from `re-notes/xrdp-datapath-abi.md:45-71` and the
binary-confirmed corrections in `re-notes/realhw/10-runner-bringup-spec.md:137-146`.

| Block | Base(s) | Count / stride | Purpose |
|---|---|---|---|
| **PSRAM** | `0x82000000` | 1 | Runner Private-SRAM (RDD tables: rings, reason table, contexts) |
| **PSRAM1** | `0x82200000` | 1 | second PSRAM bank |
| **RNR_MEM[0..7]** (data) | `0x82700000` + n·`0x20000` | 8 cores | per-core data/scratch SRAM (★corrected — see §8 findings) |
| **RNR_INST[0..7]** | core_base + `0x10000` | 8 | per-core **instruction SRAM**; 8192 words ×4 B = 32 KB |
| **RNR_CNTXT[0..7]** | core_base + `0x18000` | 8 | per-core context SRAM (512 ×4 B) |
| **RNR_PRED[0..7]** | core_base + `0x1c000` | 8 | per-core **prediction RAM**; 512 ×2 B = 1 KB |
| **RNR_REGS[0..7]** | `0x82800000` + n·`0x1000` | 8 cores | per-core Runner control regs (enable/reset/pc) |
| **RNR_QUAD** | `0x82808400` | 1 | quad/common Runner config |
| **DSPTCHR** | `0x82880000` | 1 | ingress dispatcher / queue→core scheduling |
| **UBUS_SLV** | `0x828a0000` | 1 | UBUS slave (Runner's SoC address-decode windows) |
| **SBPM** | `0x828a1000` | 1 | SRAM buffer-pool mgr (short bufs / headers) |
| **DMA[0..2]** | `0x828a1800`, `0x828a1c00`, `0x828a2000` | 3 | BBH↔DDR DMA/SDMA engines (see §7 note on DMA1=+0x800) |
| **BBH_RX[0..11]** | `0x82898000` + n·`0x400` | 12 ports (0–10 MAC) | per-port RX Buffer/Burst Handler (MAC→pool DMA) |
| **BBH_TX[0..3]** | `0x82890000` + n·`0x2000` | 4 | TX Buffer/Burst Handler (pool→MAC DMA) |
| **UNIMAC_RDP[0..3]** | `0x828a8004` + n·`0x1000` | 4 | UNIMAC datapath glue (1G ports) |
| **TCAM** | `0x82900000` | 1 | ternary CAM (classification) |
| **HASH** | `0x82920000` | 1 | hash/CAM lookup engine (multicast/IPTV/aux, fast path) |
| **CNPL** | `0x82948000` | 1 | Counter aNd Policer Layer |
| **NATC** | `0x82950000` | 1 | NAT-cache engine (primary unicast flow table, fast path) |
| **NATC_TBL[0..7]** | `0x829502d0` + n·`0x10` | 8 | NAT-C per-table cfg |
| **NATC_DDR_CFG** | `0x8295038c` | 1 | NAT-C DDR table cfg |
| **NATC_KEY[0..7]** | `0x829503b0` + n·`0x20` | 8 | NAT-C key regs |
| **FPM** | `0x82a00000` | 1 | **Free-Pool Mgr** (the main RX/TX buffer pool) |
| **QM** | `0x82c00000` | 1 | Queue Manager (egress queues/schedulers) |
| **DQM** | `0x82c80000` (accessor base `0x82c80034`) | 1 | descriptor/doorbell queue mgr (CPU-TX/QM path) |
| XLIF0/1_* (RX_IF/TX_IF/EEE/FLOW_CONTROL/Q_OFF) | various `0x82…` | per-XLMAC | 10G XLMAC datapath interface (eth0/1/3) |

Source `*_ADDRS` arrays in `rdpa.ko .data`: `FPM_ADDRS`, `SBPM_ADDRS`, `BBH_RX_ADDRS`(12),
`BBH_TX_ADDRS`(4), `DSPTCHR_ADDRS`, `QM_ADDRS`, `DQM_ADDRS`, `DMA_ADDRS`(3), `NATC_*_ADDRS`,
`HASH_ADDRS`, `PSRAM_ADDRS`, `UNIMAC_RDP_ADDRS`(4), `UBUS_SLV_ADDRS`, `CNPL_ADDRS`,
`RNR_REGS_ADDRS`(8), `RNR_INST/MEM/CNTXT/PRED_ADDRS`(8 each), `RNR_QUAD_ADDRS`
(`re-notes/xrdp-datapath-abi.md:73-78`, cross-checked `re-notes/realhw/10-runner-bringup-spec.md:143-146`).

> **The MPM DMA engine is NOT in this window.** MPM (the bulk/offload buffer-alloc DMA front-end)
> lives at SoC `0x80020000` (size `0x4000`), a separate block outside `0x82xxxxxx`, and its HW
> bring-up sequence exists only in a proprietary module. The open first-light path is **MPM-free**
> (host-managed feed-ring buffers + GPL FPM token pool). See §6.7 / §8
> (`re-notes/realhw/10-runner-bringup-spec.md:121-135`, `:148-160`).

### 2.3 MAC / PHY / switch "misc" page and MAC cores `[FDT]`

From `re-notes/bcm4916-regmap.md:42-56` and `:132-138`. These are the memory-mapped MAC cores and
the `0x837ffxxx` switch/PHY control page (distinct from the Runner datapath window above).

| FDT node | compatible | reg base(s) | size | key offset props |
|---|---|---|---|---|
| `/unimac` | `brcm,unimac3` | `0x828a8000`; `0x828b0000` | `0x5000`; `0x1400` | `conf_offset=0x1000`, `mib_offset=0x400`, `top_offset=0x400` |
| `/xlmac` | `brcm,xlmac1` | `0x828b2000`; `0x82890000` | `0x680`; `0x6b88` | `xlmac_offset=0x200`, `bbh_tx_offset=0x2000` |
| `/xport` | `brcm,xport` | `0x837f0000`; `0x828b2000`; `0x837ff1f8` | `0x8000`; `0x1000`; `0x4` | — |
| `/mpcs` | `brcm,mpcs` | `0x828c4000` | `0x100` | 10G PCS |
| `/serdes` | `brcm,serdes1` | `0x837ff500` | `0x300` | on-die serdes control |
| `/ethphytop` | `brcm,eth-phy-top` | `0x837ff000` | `0x1000` | `xphy0-addr=0x9`, `xphy0-enabled` |
| `/egphy` | `brcm,egphy` | `0x837ff00c` | `0x20` | internal quad-GPHY power/analog |
| `/swblks` | `brcm,swblks` | `0x837ff000` (bcast-ctrl); `0x837ff014` (qphy-ctrl) | `0x4`; `0x4` | `phy_base=1` |
| `/mdiosf2` | `brcm,mdio-sf2` | `0x837ffd00`; `0xff85a024` | `0x10`; `0x4` | the SF2 MDIO transactor (+ clk/strap reg) |

### 2.4 Port → PHY → MDIO map `[FDT]`

The stock switch (`/xrdp/switch0`, `brcm,enet`, `sw-type="RUNNER_SW"`) has **no `reg` of its own** —
it is a logical switch fronting the Runner (`re-notes/bcm4916-regmap.md:61-66`). Ports at
`/xrdp/switch0/ports/*` (`re-notes/bcm4916-regmap.md:73-86`):

| Port node | label | port# | mac-type | mac-index | phy-mode | MDIO addr | serdes core/lane | status |
|---|---|---|---|---|---|---|---|---|
| `port_gphy1` | **eth2** | 1 | UNIMAC | — | gmii | 2 (EGPHY) | — | okay (first-light target) |
| `port_xphy` | eth0 | 5 | XPORT | 0 | serdes | 9 (EXT3 10G copper) | — | okay |
| `port_sgmii1` | eth1 | 6 | XPORT | 2 | serdes | 7 (10GAE, `config-xfi=10GBase-R`) | core0/lane0 | okay |
| `port_sgmii2` | eth3 | 7 | XPORT | 4 | serdes | 8 → cascade @21 | core1/lane0 | okay |
| `port_gphy0/2/3` | — | 0/2/3 | UNIMAC | — | gmii | 1/3/4 (EGPHY) | — | disabled |

All PHYs enumerate on the single `/xrdp/mdio` bus whose transactor is `/mdiosf2`
(`re-notes/bcm4916-regmap.md:99-103`, `:160-170`).

---

## 3. Register & ABI reference — pool & DMA front-end

### 3.1 The register-descriptor struct convention `[KO]`

Every `ag_drv_*` accessor in `rdpa.ko` reads a 32-byte descriptor object (in `.rodata`) that
encodes the register's block-relative offset. The layout (`re-notes/xrdp-datapath-abi.md:81-98`):

| offset in descriptor | field |
|---|---|
| `+0x00` | name pointer (reloc) |
| `+0x08` | **register offset within the block** |
| `+0x10` | array-count-1 |
| `+0x14` | entry stride (bytes) |
| `+0x18` | block id |

This is why the offsets below are trustworthy: the `+0x08` field is the raw block-relative offset,
readable straight out of each `*_REG` object.

### 3.2 FPM — Free-Pool Manager (base `0x82a00000`)

**Registers** (block-relative). From `re-notes/xrdp-datapath-abi.md:88-98` and
`re-notes/realhw/10-runner-bringup-spec.md:52-59`:

| Register | Offset | Bitfields / value |
|---|---|---|
| `FPM_FPM_CTL` | `0x000` | INIT_MEM b4, BB_SOFT_RESET b14, POOL1_ENABLE b16, POOL2_ENABLE b17 |
| `FPM_FPM_CFG1` | `0x004` | FPM config |
| `FPM_POOL1_CFG1` | `0x040` | FPM_BUF_SIZE [26:24] (1 = 512 B default) |
| `FPM_POOL1_CFG2` | `0x044` | POOL_BASE_ADDRESS [31:2] (word address) |
| `FPM_POOL1_STAT1` | `0x050` | pool0 status (tokens available; `NUM_OF_TOKENS_AVAL` mask `0x3ffff`) |
| `FPM_POOL1_ALLOC_DEALLOC` | `0x400` | **alloc = READ token / dealloc = WRITE token** |

**FPM token format** (32-bit) `[SDK]` — from the GPL FPM driver, `re-notes/xrdp-datapath-abi.md:216-238`:

| bits | field |
|---|---|
| [31] | VALID |
| [30:29] | pool (1 bit used) |
| [28:12] | token index (17 b) |
| [11:0] | token size in bytes (12 b) |

- buffer addr = `pool_pbase[pool] + token_index * chunk_size`.
- alloc = READ `FPM_POOL1_ALLOC_DEALLOC` (`0x400`); free = WRITE the token back.
- Pool sizing: 512-B token pool = 32 MB, 256-B token pool = 16 MB. Default buf size 512 B, 256 K tokens.
- The FPM token pool driver is **fully GPL-portable** — a major de-risk for the slow path
  (`re-notes/xrdp-datapath-abi.md:236-238`).

**`FPM_GLOBAL_CFG` (RDD software struct the Runner reads)** `[KO]` — `re-notes/xrdp-datapath-abi.md:240-247`:
`fpm_base_low`, `fpm_base_high` (40-bit pool DDR base), `fpm_token_size`, `fpm_token_shift`,
`fpm_token_add_shift`, `fpm_token_inv_mant`, `fpm_token_inv_exp` (size↔token math),
`ddr_token_info_low/high`, `fpm_token_num_pool0..3`. Must agree with the host FPM driver constants.

**`FPM_RING_CFG_ENTRY` (RDD)** `[KO]` — `re-notes/xrdp-datapath-abi.md:249-253`:
`clr_tail_offset`, `clr_tail_size`, `is_valid`, `prio`, `buffer_nums`, `buffer_size` (per-ring FPM
buffer geometry).

### 3.3 SBPM — SRAM Buffer-Pool Manager (base `0x828a1000`) `[SDK]/[KO]`

From `re-notes/realhw/10-runner-bringup-spec.md:60-65`:

| Register | Offset | Value / bits |
|---|---|---|
| `SBPM_INIT_FREE_LIST` | `0x000` | `0x03FFC000` (INIT_OFFSET `0xFFF<<14` → 0x1000 buffers); poll RDY bit31 (★project-variant, see §8) |
| `SBPM_UG0_TRSH` | `0x050` | `0x400` |
| `SBPM_SP_RNR_LOW` | `0x184` | egress Runner src-port bitmap (RMW) |
| `SBPM_SP_RNR_HIGH` | `0x188` | |
| `SBPM_UG_MAP_LOW` | `0x18c` | |
| `SBPM_UG_MAP_HIGH` | `0x190` | |

Bring-up: seed free list (`INIT_FREE_LIST`), set the egress Runner src-port bit
(RMW `SP_RNR_LOW`/`UG_MAP_LOW`), poll `INIT_FREE_LIST` RDY.

### 3.4 BBH_RX — per-port ingress DMA (base `0x82898000` + id·`0x400`) `[KO]`

Bitfields from `re-notes/realhw/10-runner-bringup-spec.md:77-83` and the eth2/BBH0 concrete
values in `:299-305`. Fixed BB-IDs: DISPATCHER_REORDER=18, FPM=23, SBPM=56, SDMA0=21/SDMA1=22,
TX_LAN=32.

| Register | Offset | Bitfields / value (eth2 = BBH0) |
|---|---|---|
| `BBCFG` | `0x00` | SDMABBID[5:0]=21, DISPBBID[15:8]=18, SBPMBBID[23:16]=56 → `0x00380015` |
| `DISPVIQ` | `0x04` | NORMALVIQ / EXCLVIQ = bbh_id (VIQ == bbh_id firm rule) |
| `SDMAADDR` | `0x1c` | DATABASE / DESCBASE |
| `SDMACFG` | `0x20` | NUMOFCD=4, EXCLTH=4 → `0x00000404` |
| `MINPKT` | `0x24` | 64 |
| `MAXPKT` | `0x28` | 4 selectors |
| `SOPOFFSET` | `0x30` | 0 |
| `FLOWCTRL` | `0x34` | flow control |
| `ENABLE` | `0x3c` | PKTEN b0, SBPMEN b1 → `0x3` (**LAST**, in rdp_block_enable) |
| `SBPMCFG` | `0x64` | MAXREQ = 0xf |

SDMA bb_id = 21 for BBH 0–5, 22 for BBH 6–11 (`re-notes/realhw/10-runner-bringup-spec.md:305`).
The full accessor surface actually written by stock (`general_configuration_enable`,
`flow_ctrl_timer`, `sdma_config`, `pkt_size0..3`, `min/max_pkt_sel_flows_*`, `pkt_sel_group_0/1`,
etc.) is enumerated at `re-notes/xrdp-datapath-abi.md:505-511`; the per-port **register values**
remain the one deep-RE item (see §8).

### 3.5 BBH_TX — per-port egress DMA (base `0x82890000` + id·`0x2000`, LAN = id 0/1) `[KO]/[SDK]`

Route A (QM/TM-fed LAN egress) register set, offsets verified against `XRDP_AG.h`
(`re-notes/realhw/11-route-a-egress-spec.md:70-100`, with the base set from
`re-notes/realhw/10-runner-bringup-spec.md:84-88`, `:306-309`):

| Register | Offset | Layout / value |
|---|---|---|
| `MACTYPE` | `0x000` | TYPE[2:0]; **GPON = 1** (7 is invalid) |
| `BBCFG_1_TX` | `0x004` | SBPMSRC[21:16]=56, FPMSRC[29:24]=23 → `0x17380000` |
| `BBCFG_2_TX` | `0x008` | PDRNR0SRC[5:0]/PDRNR1SRC[13:8]/PDRNR2SRC[21:16]/PDRNR3SRC[29:24] = BB-ID of feeding TM/QM core(s) (6813 = 4-source) |
| `BBCFG_3_TX` | `0x00c` | MSGRNRSRC[5:0], STSRNRSRC[13:8] |
| `DFIFOCTRL` | `0x03c` | PSRAMSIZE[9:0], DDRSIZE[19:10], PSRAMBASE[29:20], REORDER_PER_Q_EN[30] |
| `RNRCFG_1` (idx0..3, stride 8) | `0x050` | TCONTADDR[15:0], SKBADDR[31:16] |
| `RNRCFG_2` (idx0..3, stride 8) | `0x060` | PTRADDR[15:0] = TM egress-counter-table>>3, TASK[19:16] = TM thread |
| `PERQTASK` | `0x0a0` | TASK0..7 ×4 bits |
| `GPR` | `0x0bc` | stock writes 3 |
| `Q2RNR` (LAN single) | `0x400` | Q0[1:0], Q1[3:2] = runner-core select |
| `QPROF` (LAN single) | `0x450` | Q0[0], Q1[1], DIS0[2], DIS1[3] |
| `QMQ` (LAN single) | `0x4b0` | Q0[0], Q1[1] — **1 = QM-fed, 0 = runner-fed** |
| `Q2RNR` (unified idx) | `0x700` | Q0[1:0], Q1[3:2] |
| `QMQ` (unified idx) | `0x7b0` | Q0[0], Q1[1] |
| `DDRTMBASEL/H` | `0x2c` / `0x34` | FPM packet-pool phys |

A LAN BBH_TX exposes **8 queues = 4 pairs** (`BBH_TX_NUM_OF_LAN_QUEUES=8`)
(`re-notes/realhw/11-route-a-egress-spec.md:100`). Direct-to-BBH per-frame encoding
`BB_ID_TX_LAN + (tx_port<<6)` (BB_ID_TX_LAN=0x20 → port1=0x60) is a **CFE2/image_0-only** model,
NOT a register the QM path writes (`re-notes/realhw/11-route-a-egress-spec.md:101-106`).

### 3.6 DMA / SDMA engines (3×; bases `0x828a1800`/`0x828a1c00`/`0x828a2000`) `[KO]`

The BBH↔DDR DMA/SDMA engines. Accessors written in `data_path_init`
(`re-notes/xrdp-datapath-abi.md:512-515`): `config_target_mem`, `num_of_reads`, `u_thresh`, `pri`,
`weight`, `periph_source`, `ubus_dpids`, `num_of_writes`, `max_otf`, `ubus_credits`, `psram_base`.
Wave-9 places the DMA/SDMA base at `0x828b2000` with DMA1 at +0x800
(`re-notes/realhw/10-runner-bringup-spec.md:186`) — reconcile against the `DMA_ADDRS` triple
above (see §8).

---

## 4. Register & ABI reference — Runner cores, dispatch, queueing

### 4.1 RNR_REGS — per-core Runner control (base `0x82800000` + core·`0x1000`) `[KO]`

From `re-notes/realhw/10-runner-bringup-spec.md:33-40`, `:113-114`, `:220-226`:

| Register | Offset | Bitfields / value |
|---|---|---|
| `CFG_GLOBAL_CTRL` | `0x00` | EN b0 (core enable) |
| `CFG_CPU_WAKEUP` | `0x04` | THREAD_NUM[3:0] — **write-to-start / edge-wake a thread** |
| `CFG_GEN_CFG` | `0x30` | DISABLE_DMA_OLD_FLOW_CONTROL b0, ZERO_DATA_MEM b2, ZERO_CONTEXT_MEM b3; **poll ZERO_DATA_MEM_DONE b4 + ZERO_CONTEXT_MEM_DONE b5 → 1** |
| `CFG_DDR_CFG` | `0x40` | DMA_BASE = `(phys_hi<<12)|(phys_lo>>20)` (40-bit fold) + DMA_BUF_SIZE_MODE b23=1 |
| `CFG_PSRAM_CFG` | `0x44` | `0x820` (psram base `0x82000000>>20`) |
| `CFG_SCH_CFG` | `0x4c` | `4` (DRV_RNR_16SP) |

Zero-mem completion is **NOT self-clearing** — poll the `*_DONE` bits to 1
(`re-notes/realhw/10-runner-bringup-spec.md:220-222`).

### 4.2 Runner core / image / thread model `[KO]/[SDK]`

The device runs the multi-image `rdpa` runtime (not the single-image CFE bootloader image), so the
runtime image/thread numbers are authoritative (`re-notes/realhw/10-runner-bringup-spec.md:188-209`):

| role | core | thread | notes |
|---|---|---|---|
| CPU RX | core 3 (image_3) | thread 1 | woken by the **DSPTCHR**, not `CFG_CPU_WAKEUP` |
| CPU TX | core 2 (image_2) | thread 6 | edge-woken **per frame** via `CFG_CPU_WAKEUP(core2, thr6)` |
| US_TM (egress) | core 6 (image_6) | — | services QM queues (Route A) |
| DS_TM (egress) | core 7 (image_7) | — | services QM queues (Route A) |

core→image mapping is identity (`re-notes/realhw/10-runner-bringup-spec.md:209`).

### 4.3 Microcode / prediction image geometry `[KO]`

The per-core image sizes are fixed by the RNR_INST/RNR_PRED window geometry
(`re-notes/runner-microcode-and-cpuring.md:44-58`, `re-notes/realhw/10-runner-bringup-spec.md:33-37`):

| image | count | size each | loads into |
|---|---|---|---|
| instruction image | 8 (one per core) | 32 KB (8192 words ×4 B) | `RNR_INST[core]` = core_base + `0x10000` |
| prediction image | 8 (one per core) | 1 KB (512 entries; source u16 → written as u32) | `RNR_PRED[core]` = core_base + `0x1c000` |

Load model: per core, MEMSET `RNR_INST[core]` to NOP `0xFC000000` (8192 words), then write the
instruction words (skipping NOPs); for prediction, read u16 source → write as u32 into `RNR_PRED`.
Then enable cores (`CFG_GLOBAL_CTRL` EN, `CFG_CPU_WAKEUP`)
(`re-notes/realhw/10-runner-bringup-spec.md:34-40`).

> **Firmware handling (generic):** the microcode is **proprietary / non-redistributable**,
> present only inside the stock `rdpa.ko` (`license=Proprietary`, taints `P`); it is not a GPL
> array and not a standalone file on the device. The open driver therefore loads it as an opaque
> `request_firmware()` blob supplied by the user from their own device, and inherits the `P`
> taint dependency (`re-notes/runner-microcode-and-cpuring.md:21-30`, `:100-116`). The
> 8×32 KB + 8×1 KB geometry above is a **hardware window fact** (RNR_INST/RNR_PRED sizes), not
> firmware content.

### 4.4 UBUS_SLV — the Runner's SoC address-decode windows (base `0x828a0000`) `[KO]`

Each window = a START register + an END register, each a full 32-bit address (no mask, no enable
bit). Fully pinned (`re-notes/realhw/10-runner-bringup-spec.md:41-51`, cross-checked
`re-notes/xrdp-datapath-abi.md:478-486`):

| window | START off | START val | END off | END val |
|---|---|---|---|---|
| VPB (RNR_MEM/per-core SRAM) | `0x04` | `0x82700000` | `0x08` | `0x82900000` |
| APB (SBPM/DMA/BBH) | `0x0c` | `0x82900000` | `0x10` | `0x82a00000` |
| dev0 FPM | `0x14` | `0x82a00000` | `0x18` | `0x82c00000` |
| dev1 QM | `0x1c` | `0x82c00000` | `0x20` | `0x82c80000` |
| dev2 DQM | `0x24` | `0x82c80000` | `0x28` | `0x82d00000` |

Program order: dev0, dev1, dev2, vpb, apb. `ubus_mstr_hyst_ctrl = 2` for masters 0,1,2.

### 4.5 DSPTCHR — ingress dispatcher / reorder (base `0x82880000`) `[KO]`

From `re-notes/realhw/10-runner-bringup-spec.md:66-70` and the CPU_RX wake recipe `:257-277`:

| Register | Offset | Bitfields / value |
|---|---|---|
| `REORDER_CFG` (`DSPTCHR_REORDR_CFG`) | `0x000` | EN b0, AUTO_INIT b4 (`~0x11`); poll RDY b8 (**FINAL enable**) |
| `REORDER_CFG_VQ_EN` | `0x004` | VQ enable bitmask |
| `INGRS` limits | `0x300` | ingress limits |
| `QUEUE_MAPPING_CRDT_CFG` (per-VIQ) | `0x400` + 4·viq | credit config |
| `QUEUE_MAPPING_PD_DSPTCH_ADD` (per-core) | `0x480` + 4·core | base_add[15:0] / offset_add[31:16] |
| `QUEUE_MAPPING_Q_DEST` (per-VIQ) | `0x4c0` + 4·viq | destination |
| `MASK_MSK_TSK_255_0` | `0x500` + 4·(grp·8+w) | 8 words/grp; word0 = tasks[255:224] |
| `MASK_MSK_Q` (per-grp) | `0x600` + 4·grp | queue mask |
| `MASK_DLY_Q` / `MASK_NON_DLY_Q` | `0x620` / `0x624` | delay/non-delay queue masks |
| `LOAD_BALANCING_TSK_TO_RG_MAPPING` | `0x900` + 4·(task/8) | 8×3-bit task→group |
| `RG_AVLABL_TSK_0_3` / `_4_7` | `0x980` / `0x984` | group available-task |
| `QDES head/tail` | `0x2000` / `0x200c` | queue descriptor |
| `FLL desc` (free linked-list) | `0x2700` | minbuf/head/tail |
| `BDRAM_NEXT_DATA` (free list seed) | `0x3000` | node i ← i+1 |

**CPU_RX wake — the missing write** (`re-notes/realhw/10-runner-bringup-spec.md:265-269`): for
core3/task1/group1, `QUEUE_MAPPING_PD_DSPTCH_ADD[core3]` @ `0x048C` = `0x728`
(= `IMAGE_3_PD_FIFO_TABLE_ADDRESS 0x3940 >> 3`), offset_add=0. Without it the CPU_RX thread is in a
group but has no delivery address and never wakes. Also set `MASK_MSK_TSK` @ `0x0524` = `0x00020000`
and the group/VIQ masks. (VIQ 13 = CPU_TX egress, see §4.6.)

### 4.6 QM — Queue Manager (base `0x82c00000`) `[KO]/[SDK]`

Offsets verified vs `XRDP_AG.h` (`re-notes/realhw/11-route-a-egress-spec.md:28-49`):

| Register | Offset |
|---|---|
| `GLOBAL_CFG_QM_ENABLE_CTRL` | `0x000` |
| `GLOBAL_CFG_QM_SW_RST_CTRL` | `0x004` |
| `GLOBAL_CFG_FPM_CONTROL` | `0x00c` |
| `GLOBAL_CFG_AGGREGATION_CTRL` | `0x02c` |
| `GLOBAL_CFG_FPM_BASE_ADDR` | `0x034` |
| `GLOBAL_CFG_FPM_COHERENT_BASE_ADDR` | `0x038` |
| `GLOBAL_CFG_DDR_SOP_OFFSET` | `0x03c` |
| `GLOBAL_CFG_MEM_AUTO_INIT` | `0x138` |
| `GLOBAL_CFG_MEM_AUTO_INIT_STS` | `0x13c` |
| `FPM_POOLS_THR` | `0x200` |
| `FPM_USR_GRP_{LOWER,MID,HIGHER}_THR` (per-UG) | `0x280` / `0x284` / `0x288` |
| `RUNNER_GRP_RNR_CONFIG` (per-grp, stride 0x10) | `0x300` |
| `RUNNER_GRP_QUEUE_CONFIG` (per-grp) | `0x304` |
| `RUNNER_GRP_PDFIFO_CONFIG` (per-grp) | `0x308` |
| `RUNNER_GRP_UPDATE_FIFO_CONFIG` (per-grp) | `0x30c` |

Key values (`re-notes/realhw/11-route-a-egress-spec.md:50-64`, `:116-119`,
`re-notes/realhw/10-runner-bringup-spec.md:71-76`):
- `QM_ENABLE_CTRL(0x000)` = **`0x307`** = fpm_prefetch[0] | reorder_credit[1] | dqm_pop[2] |
  rmt_fixed_arb[8] | dqm_push_fixed_arb[9]. Minimal drain subset likely `0x7` (untested).
- `MEM_AUTO_INIT(0x138)` MEM_INIT_EN[0]=1, MEM_SEL_INIT[10:8]=0 (all banks); poll
  `MEM_AUTO_INIT_STS(0x13c)`. **Live-confirmed done = bit0** (`0x01`) — see §4.6.1.
- `FPM_CONTROL(0x00c)` fixed bits ≈ `0x7D000001 | (prefetch_min_pool_size<<8)` (override_bb_id_en +
  bb_id=0x3e FPM + pool_bp_enable).
- `FPM_BASE_ADDR(0x034)` = `bufmem_phys>>8` (256-B resolution); `DDR_SOP_OFFSET(0x03c)` = 18 (gen-1).
- UG thresholds (gen-1 start): UG0=20K, UG1=40K, UG2=0, UG3=64K-1.

**QM ↔ BBH_TX binding = the RUNNER_GRP registers** (NOT a `QM_QUEUE_TO_TX_FLOW` table, which is a
no-op on 6813) (`re-notes/realhw/11-route-a-egress-spec.md:109-125`):
- `RUNNER_GRP_QUEUE_CONFIG(0x304)`: START_QUEUE[8:0], END_QUEUE[24:16] (contiguous QM queue range).
- `RUNNER_GRP_RNR_CONFIG(0x300)`: RNR_BB_ID[5:0] (TM core), RNR_TASK[11:8] (thread), RNR_ENABLE[16].
- `MAX_TX_QUEUES=160`, `QM_QUEUE_DROP=0xFF`.

#### 4.6.1 QM live-oracle results `[LIVE]`

Captured read-only from live stock silicon (`re-notes/realhw/11-route-a-egress-spec.md:198-241`):

- `QM_ENABLE_CTRL` = `0x0307` (confirms the value); `MEM_AUTO_INIT_STS@0x13c` = `0x01`
  (MEM_INIT_DONE = bit0, confirms the SRAM-auto-init poll); `FPM_BASE@0x34` = `0x006d0000`.
- Enabled RUNNER_GRP map (QM `0x82c00300`, stride 0x10; `bb_id == RNR core number`):

| grp | queues | bb_id | task | TM core |
|---|---|---|---|---|
| 1 | 0–79 | 6 | 4 | core6 US_TM |
| 0 | 80–111 | 7 | 3 | core7 DS_TM |
| 9 | 112–143 | 0 | 7 | (img0) |
| 3 | 145 | 3 | 1 | |
| 6 | 146–148 | 3 | 3 | |
| 7 | 149–151 | 4 | 1 | |
| 10 | 154–155 | 1 | 0 | |
| 14 | 158 | 3 | 4 | |

- **BBH_TX QMQ (unified `0x7b0`):** BBH0=0, **BBH1=`0x01`** (Q0 QM-fed), BBH2=0 → the QM-fed LAN
  egress instance is **BBH_TX[1]** (MACTYPE=1). Route A pins `route_a_bbh_inst = 1`.
- The exact physical QM queue for `port_gphy1`'s CPU-TX egress (port-relative q0 → which global
  queue in 80–111) is still unresolved — carried as module params (candidate B: queue≈80/grp0/
  bb_id7/task3, or candidate A: queue0/grp1/bb_id6/task4). See §8/§9.

### 4.7 DQM (base `0x82c80000`, accessor base `0x82c80034`) `[KO]`

`DQM_DQMOL_CFGB[q]` @ `0x1fd4` ENABLE b0 (per-queue enable)
(`re-notes/realhw/10-runner-bringup-spec.md:74`). `DQM_DQMOL_PUSHTOKEN_REG` exists but is used only
by a CLI test tool and the QM/forwarding fast path — **not** by the host CPU-TX-ring send
(`re-notes/xrdp-datapath-abi.md:463-465`).

---

## 5. Register & ABI reference — CPU ring datapath (slow path)

The XRDP CPU datapath uses **separate rings** (unlike the single inline-refill ring of the older
63138 part) (`re-notes/xrdp-datapath-abi.md:110-121`):
- **data (delivery) ring** — Runner writes RX packet descriptors here (host DDR); host consumes.
- **feed ring** — host posts FREE buffers to the Runner to refill RX.
- **recycle ring** — Runner returns spent buffers (on 6813 recycle folds back into the feed ring).
- **CPU-TX ring** — host pushes TX descriptors (in Runner SRAM; indices in per-core SRAM).

### 5.1 `CPU_RING_DESCRIPTOR` — per-ring control block (16 B, big-endian in SRAM) `[KO]`

The control block the host fills to register a ring with the Runner
(`re-notes/xrdp-datapath-abi.md:123-140`):

| field | location (byte off) | width | meaning |
|---|---|---|---|
| `size_of_entry` | byte0 bits[7:3] | 5 | descriptor size code (in **bytes**, sizeof desc) |
| `number_of_entries` | half@0 bits[10:0] | 11 | ring depth — **in units of the ring resolution** (see §5.5) |
| `interrupt_id` | half@2 | 16 | IRQ/queue id |
| `drop_counter` | half@4 | 16 | drops |
| `write_idx` | half@6 | 16 | **producer index (Runner-written)** |
| `base_addr_low` | word@8 | 32 | ring DDR base, low |
| `read_idx` | half@0xc | 16 | **consumer index (host-written)** |
| `reserved0` | byte@0xe | 8 | — |
| `base_addr_high` | byte@0xf | 8 | ring DDR base, high (40-bit phys) |

### 5.2 `CPU_FEED_DESCRIPTOR` — feed-ring entry (8 B) `[SDK]/[KO]`

Host posts free DDR buffers as 40-bit ABS pointers
(`re-notes/realhw/10-runner-bringup-spec.md:180-186`, `:229-231`):

| field | location | value |
|---|---|---|
| `ptr_low` | byte+0 (word) | buffer phys addr low |
| `type` / ABS bit | byte+6 bit0 | host writes `BIT(8)`; ABS_TYPE = 1 |
| `ptr_hi` | byte+7 (shift 0) | buffer phys addr [39:32] |

Feed-ring table lives at core3 RDD `0x0f70`; doorbell = 16-bit big-endian `write_idx` write @ +6;
batch threshold 128; CPU_RING_SIZE_64_RESOLUTION = 6 (feed ring is **64-resolution**).

### 5.3 `PROCESSING_RX_DESCRIPTOR` = host `CPU_RX_DESCRIPTOR` (16 B / 4 words, big-endian) `[KO]/[SDK]`

The host RX delivery-ring entry **is** the Runner's `PROCESSING_RX_DESCRIPTOR` (there is no
separate host descriptor on 4916). Byte offsets are authoritative; bit positions are within the
host-order value after `rev16`/`rev32` (the descriptor is big-endian in DDR; ARM host must
`swap4bytes` each 32-bit word) (`re-notes/xrdp-datapath-abi.md:390-441`):

| # | field | load (byte off) | bits (post-rev) | slow-path use |
|---|---|---|---|---|
| 1 | `pd_info` | +0 (word) rev32 | 32 | word0 composite |
| 5 | `abs_or_dsl` | +5 (byte) | bit3 | **1 = ABS DDR addr / 0 = FPM token** |
| 6 | `l3_packet` | +5 | bit2 | L3 packet |
| 7 | `error_type_or_cpu_tx` | +4 (word) rev32 | [14:17] | error type / cpu-tx code |
| 8 | **`packet_length`** | +6 (half) rev16 | `&0x3fff` [0:13] | **RX payload length** |
| 9 | `error` | +8 (byte) | bit7 | error flag |
| 12 | `is_emac` | +8 | bit3 | EMAC (UNIMAC) vs XPORT ingress |
| 13 | **`ingress_vport_or_flow`** | +8 (half) rev16 | [3:10] | **source vport/flow → netdev** |
| 14 | `bn1_last_or_abs1` | +8 (word) rev32 | `&0x7ffff` [0:18] | 2nd buffer-number / abs1 (multi-buf) |
| 15 | `agg_pd` | +0xc (byte) | bit7 | aggregated-PD flag |
| 17 | **`payload_offset_sop`** | +0xc (word) rev32 | `&0x3fffffff` [0:29] | **SOP/payload offset + buffer ptr/token** |

(Full 17-field list — including `serial_num`, `ploam`, `ingress_cong`, `cong_state`,
`target_mem_0/1` — at `re-notes/xrdp-datapath-abi.md:408-426`.) The buffer pointer / FPM token
lives in **word3** (bytes 12–15) overlaid with `payload_offset_sop`; `abs_or_dsl`/`bn1_last_or_abs1`
select ABS-DDR-addr vs FPM-token mode. The **reason** is not a descriptor bitfield — it is implicit
in *which queue* the packet landed (the reason→queue trap table, §7 step 10)
(`re-notes/xrdp-datapath-abi.md:428-441`).

> **⚠ RX consumer model — UNRESOLVED CONFLICT in the notes (see §8).** One RE pass
> (`re-notes/xrdp-datapath-abi.md:191-210`, `:428-431`, from the GPL WFD path + `rdpa.ko`) says
> RX is **ownership-bit polling**: ownership = bit31 of word2 post-swap; host re-arms by setting
> it. A later bring-up pass (`re-notes/realhw/10-runner-bringup-spec.md:182-184`, `:243-247`) says
> the 6813 CPU delivery ring is **index-polled**: host polls `read_idx`(@0xc) vs `write_idx`(@6)
> of the `CPU_RING_DESCRIPTOR` (§5.1), **not** a word2 ownership bit. These are contradictory; the
> driver must settle this against live silicon.

### 5.4 `RING_CPU_TX_DESCRIPTOR` — host TX descriptor (16 B, big-endian) `[KO]`

The descriptor the host writes to push a packet to the Runner for egress. Two variants of the
byte/bit layout appear in the notes; the **byte-precise (later, corrected) layout** is
authoritative (`re-notes/realhw/10-runner-bringup-spec.md:94-96`,
`re-notes/realhw/11-route-a-egress-spec.md:129-155`), superseding the earlier bit-position table
at `re-notes/xrdp-datapath-abi.md:146-169`:

| W.byte | field | shift, width | meaning |
|---|---|---|---|
| W0+0 | `is_egress` | b7, 1 | egress vs ingress (path selector; =1 for Route A) |
| W0+0 | `first_level_q` | b6, 9 | target QM queue (Route A, if explicit) |
| W0+0 | `packet_length` | b8, 14 | payload length |
| W2+8 | `color` | b7, 1 | QoS color |
| W2+8 | `do_not_recycle` | b6, 1 | don't return buffer to pool (0 ⇒ Runner auto-frees the FPM buf) |
| W2+8 | `flag_1588` | b5, 1 | 1588/PTP |
| W2+8 | `is_emac` | b4, 1 | EMAC (UNIMAC) vs XPORT egress |
| W2+8 | `is_vport` | b3, 1 | =1 for vport egress |
| W2+8 | `flow_or_port_id` / `wan_flow_source_port` | u16@+8 [11:4] / [10:4] | target LAN vport/flow |
| W2+9 | `abs` | b0, 1 | **1 = absolute host DDR addr / 0 = FPM buffer-number** |
| W2+10 | `ssid` | [5:2] | |
| W2+10 | `egress_dont_drop` | b1, 1 | drop override |
| W2+11 | `pkt_buf_ptr_high` | [7:0] | packet-buffer ptr high (abs mode) |
| W3+12 | `pkt_buf_ptr_low` / `fpm_bn0` | u32 | packet-buf ptr low **or** FPM token: bn=`index|(pool<<17)` [19:0], sop [29:20] |

- `abs=1` ⇒ host gives an absolute DDR buffer address; `abs=0` ⇒ host gives an FPM token in
  word3 (`bn = index | (pool<<17)`; `sop` = data offset, 0 for buffer-start, 240 = standard FPM
  head pad) (`re-notes/realhw/10-runner-bringup-spec.md:232-235`).
- For QM/Route A egress the open driver must additionally set `is_egress=1`, `is_vport=1`,
  `flow_or_port_id=LAN vport`, `is_emac`, and either `first_level_q` or rely on microcode
  VPORT_TX_FLOW_TABLE resolution (image_2 RDD `0x0fc0`)
  (`re-notes/realhw/11-route-a-egress-spec.md:146-152`).

### 5.5 CPU-TX doorbell + ring resolution `[KO]`

There is **no per-packet MMIO doorbell**. The CPU-TX kick sequence
(`re-notes/xrdp-datapath-abi.md:443-469`, corrected by
`re-notes/realhw/10-runner-bringup-spec.md:243-256`):

1. Per-ring TX descriptor struct stride = `0x88` bytes; entry stride within the ring = 16 B.
2. Write the 16-byte big-endian `RING_CPU_TX_DESCRIPTOR` into the ring slot at `write_idx`.
3. Advance the local `write_idx` (wrap at ring depth), then **`dmb oshst`** (ordering barrier).
4. Publish the new `write_idx` as a **big-endian u16** into the
   `CPU_TX_RING_INDICES_VALUES_TABLE` in each serving core's SRAM (a `0xffffff` sentinel in the
   per-core address array marks "core not serving this ring"). **The index write is the doorbell.**
5. **★Then** edge-wake the CPU_TX thread: write `CFG_CPU_WAKEUP(core2, thread6)` — the index bump
   alone is NOT sufficient (`re-notes/realhw/10-runner-bringup-spec.md:248-250`).

**Ring resolution** (`re-notes/realhw/10-runner-bringup-spec.md:243-245`, `:227`): the RX-delivery
and TX rings are **32-resolution** (`number_of_entries = depth>>5`); only the **feed ring** is
**64-resolution** (`depth>>6`). `size_of_entry` is in bytes; the Runner wraps `write_idx` at
`number_of_entries << resolution`.

### 5.6 Per-core RDD table addresses (Runner SRAM) `[KO]`

Project = BCM6813 (PKTFLOW), not BCM6813_FPI — the value that resolved earlier TX-ring ambiguity
(`re-notes/realhw/10-runner-bringup-spec.md:201-213`, `:143`,
`re-notes/realhw/11-route-a-egress-spec.md:154-155`). RDD offsets are relative to the owning core's
RNR_MEM base:

| table | core / image | RDD offset | abs (RNR_MEM base + off) |
|---|---|---|---|
| RX delivery ring desc | core3 / image_3 | `0x3000` | `0x82763000` |
| FEED ring desc | core3 / image_3 | `0x0f70` | `0x82760f70` |
| CPU_TX ring desc | core2 / image_2 | `0x33e0` | `0x827433e0` |
| CPU_TX ring indices | core2 / image_2 | `0x29c8` | `0x827429c8` |
| CPU_TX egress dispatcher credit | core2 / image_2 | `0x29d0` | — |
| CPU_TX sync FIFO | core2 / image_2 | `0x3780` | — |
| VPORT_TX_FLOW_TABLE | image_2 | `0x0fc0` | — |

(core_base = `0x82700000 + core*0x20000`, so core2 = `0x82740000`, core3 = `0x82760000`.)

### 5.7 63138 `CPU_RX_DESCRIPTOR` template — reference only `[SDK-OLD]`

The older RDP layout is the structural template the 4916 diverges from; **do not** use it
bit-for-bit (`re-notes/xrdp-cpu-datapath.md:99-122`):

```
word0: packet_length:14 | source_port:5 | is_chksum_verified:1 | flow_id:12
word1: descriptor_type:4 | reserved:5 | dst_ssid:16 | reason:6 | payload_offset_flag:1
word2: host_buffer_data_pointer:31 | ownership:1   (bit31 = ownership, 1=HOST/0=RUNNER)
word3: {wl_metadata / free_index / ssid_vector} | ... | is_ucast:1 | is_rx_offload:1
```

---

## 6. Register & ABI reference — flow offload (fast path)

The fast path is out of scope for the slow-path v1, but its ABI is the offload subsystem's
foundation. Documented here as the hardware/entry-layout reference; C-side detail is in
`docs/audit/02-flow-offload.md` and `docs/audit/03-cmdlist.md`.

### 6.1 NAT-C — primary unicast flow cache (DDR-backed, engine @ `0x82950000`) `[XRDP-BIN]`

Each NAT-C entry = a 16-byte masked **key** + a **result that is literally the
`FC_UCAST_FLOW_CONTEXT_ENTRY`** (which embeds the cmdlist)
(`re-notes/xrdp-offload-abi.md:26-45`, `:49-89`):

- **Key** = 16 bytes = 4×u32, AND-masked with a per-table key mask (unused 5-tuple bytes zeroed),
  stored **big-endian** (rev32). Masks are programmable per table (`re-notes/xrdp-offload-abi.md:66-71`).
- **DDR geometry**: buckets → bins (two size classes `ddr_bins_per_bucket_0/1`, set-associative);
  up to **8 NAT-C tables** (matching the `NATC_TBL[0..7]`/`NATC_KEY[0..7]` banks at
  `0x829502d0`/`0x829503b0`) (`re-notes/xrdp-offload-abi.md:76-89`).
- Engine programmed via indirect registers `ag_drv_natc_indir_addr/data_*`; add op = command 3.
  HW computes its own hash over the masked key (polynomial internal/unknown).

### 6.2 `FC_UCAST_FLOW_CONTEXT_ENTRY` — the NAT-C result / context `[XRDP-BIN]`

Complete 4916 field set (from `rdpa.ko` dump strings) — carries the per-flow cmdlist + egress
metadata (`re-notes/xrdp-offload-abi.md:94-124`). Key fields:

| field | meaning |
|---|---|
| HW header (`done`, `natc_hit`, `cache_hit`, `has_iter`, `hash_val`) | HW-written control |
| `valid`, `multicast_flag`, `is_routed`, `is_l2_accel`, `is_tos_mangle`, `is_hw_cso` | flow class |
| `command_list_length_32` | cmdlist length in **32-bit words** (XRDP unit; old RDP used _64) |
| `vport` | egress virtual port (replaces old "egress_phy") |
| `service_queue_id` | egress queue |
| `policer_id` | CNPL policer binding |
| `tunnel_index_ref` | GRE/VXLAN/MAP-T tunnel descriptor index |
| `q_bytes_cnt`, `flow_hits`, `flow_bytes` | stats (CNPL-backed) |
| `command_list[ ]` | the XPE byte-code, embedded inline |

**Exact 6813 byte offsets are UNKNOWN** (the autogen header is stripped from the GPL drop). The
RDP-impl2 template — cmdlist[80] at +16, `valid` at +96, `command_list_length_64` at +97 — is the
structural starting point only (`re-notes/xrdp-offload-abi.md:126-149`); the driver's context
buffer is sized `XPE_CTX_ENTRY_MAX = 124` with only `XPE_CTX_CMDLIST_OFF = 24` pinned (see
`docs/audit/02-flow-offload.md`).

### 6.3 XPE cmdlist — opcode set & command-word encoding `[XRDP-BIN]`

The per-flow packet-modification micro-program the Runner runs on egress. Command word = 32-bit,
**big-endian**, `opcode = word >> 26` (bits[31:26]); byte0 = `opcode<<2 | sub-flags`. Text/data
words are 16-bit big-endian in separate `.text`/`.data`/`.gdma` regions, 4-byte aligned. The list
is **length-delimited** by `cmd_list_data_length` (no NOP terminator; trailing context slack padded
with byte `0xfc`) (`re-notes/xrdp-offload-abi.md:158-193`, `:252-316`).

| opcode (bits[31:26]) | name | meaning |
|---|---|---|
| `0x01` | CMP_JMP | compare + conditional jump |
| `0x02` | JMPCOND | conditional jump |
| `0x03` | JMPREG | jump via register |
| `0x13` | MCOPY | multi-word/memcpy copy (GDMA) |
| `0x18` | REPLACE | replace N bytes/words with inline data (NAT addr/port, VLAN edit) |
| `0x1c` | GDMA / ICSUM group | GDMA descriptor / incremental checksum (emitter uses 0x1c for icsum16) |
| `0x2c` | MOVE | move bytes within packet |
| `0x36` | ICSUM | incremental ones-complement checksum update |
| `0x1a` | ADD | add immediate (decrement_8 emits 0x6a top byte = ADD; TTL/hop-limit −1) |
| `0x14` | REPLACE bits | replace_bits_16 base word `0x50000000` (VLAN/ToS) |
| `0x3f` | NOP | default / no-op |

Command-word field structure (`re-notes/xrdp-offload-abi.md:265-287`):

| field | bits | source |
|---|---|---|
| opcode | [31:26] | `>>26`; byte0 = opcode<<2 \| sub-flags |
| sub-flags | [25:24] | size class |
| "to" word-count / dst offset | [23:16] | `(offset>>1)+1` |
| "from" data-ref OR inline immediate | [15:8] / [15:0] | data-region ref (0x94-relative) OR 16-bit immediate |
| sentinel / mask | [7:0] | `0xff` for "to end" ops (decrement_8) |

(The `>>26` emitted opcodes differ from the opcode-name LUT `cmp` values for the ADD/ICSUM groups —
the emitted values are what silicon decodes. `REPLACE=0x18` and `byte0=opcode<<2` are also
live-confirmed.) The full emitter API and compiled IPv4-NAT / L2 examples are at
`re-notes/xrdp-offload-abi.md:197-250`.

### 6.4 HASH & CNPL (fast-path aux) `[XRDP-BIN]`

- **HASH/CAM @ `0x82920000`** — CAM-backed hash + context RAM for multicast/IPTV/aux lookups (NOT
  main unicast). Accessors `ag_drv_hash_*` (`cam_base_addr`, `cam_configuration_tm_cfg`,
  `context_ram_context_23_0`, `cam_indirect_*`) (`re-notes/xrdp-offload-abi.md:320-326`). Skip for v1.
- **CNPL @ `0x82948000`** — token-bucket policers + stat counters. `ag_drv_cnpl_counter_cfg_*`,
  `ag_drv_cnpl_policer_cfg_*`. Context `policer_id` → CNPL policer; flow stats
  (`flow_hits`/`flow_bytes`/`q_bytes_cnt`) are CNPL counters
  (`re-notes/xrdp-offload-abi.md:328-334`).

---

## 7. Bring-up ordering

Two consistent orderings appear in the notes. The **slow-path (CPU-forwarded) checklist**
(`re-notes/xrdp-datapath-abi.md:275-316`, refined by the CFE2 `_data_path_init` order in
`re-notes/realhw/10-runner-bringup-spec.md:26-32`), and the **Route A egress order** that folds in
QM/TM (`re-notes/realhw/11-route-a-egress-spec.md:172-179`).

Consolidated init order:

1. **Map register windows** — ioremap `0x82000000`/`0x00caf004`; derive block bases (§2.2).
2. **UBUS_SLV decode windows** (§4.4) — program dev0, dev1, dev2, vpb, apb (fully pinned values).
3. **RNR cores addr init** — `CFG_GEN_CFG` zero data/context mem + **poll the `*_DONE` bits**
   (§4.1); set `CFG_DDR_CFG`/`CFG_PSRAM_CFG`/`CFG_SCH_CFG`.
4. **QM init (Route A)** — QM SRAM `MEM_AUTO_INIT` + poll done (bit0); FPM/SBPM must be ready
   first; UG thresholds; RUNNER_GRP for the TM egress task (§4.6). Do this **before** enable.
5. **FPM / (MPM) pool init** — `FPM_FPM_CTL` BB reset → INIT_MEM pulse → `POOL1_CFG1/2` →
   `FPM_GLOBAL_CFG` (§3.2). MPM is skipped on the open MPM-free path.
6. **SBPM init** — `INIT_FREE_LIST`, Runner src-port bit, poll RDY (§3.3).
7. **Load Runner microcode** — per core: MEMSET `RNR_INST` to NOP, block-write the instruction
   image, write prediction; seed context/mem; then enable cores (§4.3). *(Proprietary blob.)*
8. **Per-port BBH RX/TX + DMA/SDMA** — `BBH_RX` BBCFG/DISPVIQ/SDMACFG (ENABLE last, §3.4);
   `BBH_TX` MACTYPE/BBCFG_1/2/RNRCFG_2/QMQ=1 for Route A (§3.5); the 3 DMA engines (§3.6).
9. **DSPTCHR init** — free-list/FLL seed, per-VIQ config, the CPU_RX group + PD_DSPTCH_ADD wake
   write, CPU_TX egress VIQ 13; `REORDER_CFG` EN|AUTO_INIT last + poll RDY (§4.5).
10. **CPU rings** — allocate non-cacheable DDR for data/feed/(recycle) rings; fill a
    `CPU_RING_DESCRIPTOR` per ring (§5.1); seed the feed ring with fresh buffers (§5.2).
11. **CPU-TX ring** — set up the TX ring desc + indices tables in per-core SRAM (§5.6).
12. **Reason→CPU-queue trap table** — program `CPU_REASON_TO_TC` + per-port reason so all ingress
    traps to the chosen CPU RX queue with no flow lookup (dumb-pipe v1)
    (`re-notes/xrdp-datapath-abi.md:263-271`).
13. **`rdp_block_enable` — LAST, in this order** (`re-notes/realhw/10-runner-bringup-spec.md:30-32`,
    `re-notes/realhw/11-route-a-egress-spec.md:176-178`): poll SBPM ready → XLIF tx/rx enable →
    `BBH_RX ENABLE` → `DSPTCHR REORDER_CFG` enable → **`QM_ENABLE_CTRL=0x307`** → UBUS-master
    enable → **RNR enable + `CFG_CPU_WAKEUP`** last.
14. **IRQ → NAPI** — wire the GIC SPIs (§10); each data-ring queue IRQ schedules NAPI, FPM IRQ
    drives refill.
15. **MACs / link (control plane)** — UNIMAC/XPORT/XLMAC + MDIO + PHY (mdio-bcm-unimac + b53/bcm_sf2
    4916 variant + phylink; a separate subsystem).

**Hard ordering rules** (`re-notes/realhw/11-route-a-egress-spec.md:179`): FPM + SBPM ready **and**
QM SRAM/FPM/UG/RUNNER_GRP done **before** QM enable; QM enable **before** RNR enable; **RNR last**.

### 7.1 1G EGPHY / UNIMAC port bring-up (first-light MAC) `[SDK]`

For `port_gphy1` = eth2 (UNIMAC **inst 1**), no blob needed
(`re-notes/realhw/10-runner-bringup-spec.md:284-297`):

| block | reg | offset / value |
|---|---|---|
| UNIMAC (conf base `0x828a9000` = `0x828a8000` + 1·`0x1000`) | `CMD` | +`0x08`: tx_ena b0, rx_ena b1, eth_speed[3:2]=2 (1G), sw_reset b13, crc_fwd b6, pause_fwd b7, cntl_frm_ena b23 |
| UNIMAC | `FRM_LEN` | +`0x14` = `0x3fff` (max) |
| EGPHY (internal quad, `0x837ff00c`+) | `QEGPHY_TEST_CTRL` | `0x837ff00c` |
| EGPHY | `QEGPHY_CTRL` | `0x837ff010` (PHY_RESET, PLL_CLK125_250_SEL, IDDQ_*, PHY_PHYAD, EXT_PWR_DOWN) |
| EGPHY | `QEGPHY_STATUS` | `0x837ff014` (poll PLL_LOCK) |
| per-PHY via MDIO addr 2 on mdiosf2 (`0x837ffd00`, Clause-22) | `BMCR` | `0x00` (clear POWERDOWN, set ANRESTART `0x200`) |
| per-PHY | `AUXSTAT` | `0x19` (link b2, speed [10:8]) |

Config UNIMAC under `sw_reset=1`, then `=0`, then set `tx_ena|rx_ena`. EGPHY power sequence per
`re-notes/realhw/10-runner-bringup-spec.md:290-292`.

---

## 8. Audit findings (unverified / placeholder / conflicting values)

These are values a driver author must NOT trust blindly. Each is flagged in the source notes as
placeholder, proxy, project-variant, or unresolved.

1. **RNR_MEM base was wrong in an early header.** `re-notes/realhw/10-runner-bringup-spec.md:4-5`
   states `0x82600000 + core*0x20000` — this is a **6837/6888 proxy value, WRONG by +0x100000**.
   The authoritative device value is **`0x82700000`** (Wave-4,
   `re-notes/realhw/10-runner-bringup-spec.md:139-141`). Use `0x82700000`.

2. **RNR_REGS base was a proxy, later confirmed.** Flagged "★base from 6888 proxy; re-confirm"
   at `re-notes/realhw/10-runner-bringup-spec.md:16`; later confirmed `0x82800000`/stride `0x1000`
   from the device `rdpa.ko` (`:144`, `:113`). Resolved, but note the provenance.

3. **DQM base vs accessor base.** The stock accessor base is `0x82c80034`
   (`re-notes/realhw/10-runner-bringup-spec.md:142`); the block base is `0x82c80000` and `+0x34` is
   a register. Earlier text (`:24`) briefly used `0xc80000` then reverted. Do not treat `…0034` as
   the block base.

4. **RX consumer model is contradictory (the biggest open item).** Ownership-bit-on-word2 polling
   (`re-notes/xrdp-datapath-abi.md:191-210`) vs `read_idx`/`write_idx` index polling
   (`re-notes/realhw/10-runner-bringup-spec.md:182-184`, `:243-247`). The later pass explicitly
   "corrects wave-1"; the driver header comment was also wrong once (`:93`). **Must be pinned on
   silicon.** (See §5.3 warning.)

5. **CPU-TX ring descriptor address is project-variant.** BCM6813 = `0x33e0` vs BCM6813_FPI =
   `0x3360` (`re-notes/realhw/10-runner-bringup-spec.md:190-191`, `:201-208`). Resolved to
   **`0x33e0`** because the GT-BE98 build compiles the BCM6813 (PKTFLOW) project, not _FPI. An
   earlier driver used the wrong `0x3360`.

6. **Ring resolution was initially wrong everywhere.** RX-delivery + TX rings are 32-resolution
   (`depth>>5`); only the feed ring is 64-resolution (`depth>>6`). The first implementation used
   `>>6` everywhere and half-sized the rings (`re-notes/realhw/10-runner-bringup-spec.md:243-245`).

7. **TX word3 FPM fields were unverified / carried from another chip.** `TXD_W3_FPM_BN0/SOP`
   flagged "no such fields in the SDK TX struct" (`re-notes/realhw/10-runner-bringup-spec.md:95`),
   later re-derived as `fpm_bn0[19:0] | fpm_sop[29:20]`, `bn = index|(pool<<17)` (`:232-234`). Use
   the re-derived form.

8. **SBPM `INIT_FREE_LIST` value is project-variant.** `0x03FFC000` (0xFFF buffers) vs `0x5fc000`
   (0x17F buffers, CFE2 bootloader pool) (`re-notes/realhw/10-runner-bringup-spec.md:192`,
   `:212-213`). Use the BCM6813 rdpa value; SBPM is slow-path-optional for the first CPU frame.

9. **Per-port BBH RX/TX register VALUES are not individually disassembled.** The accessor set and
   call order are pinned, but the per-port immediates (`sdma_config`, `pkt_size`,
   `flow_ctrl_timer`, `min/max_pkt_sel_flows`, BBH-TX `q2rnr`/`rnrcfg`) still need per-call disasm
   or a live regdump (`re-notes/xrdp-datapath-abi.md:562-564`,
   `re-notes/realhw/10-runner-bringup-spec.md:98-102`).

10. **DMA/SDMA base is stated two ways.** `DMA_ADDRS` triple `0x828a1800/1c00/2000`
    (`re-notes/xrdp-datapath-abi.md:57`) vs Wave-9 "DMA/SDMA base 0x828b2000 (DMA1 +0x800)"
    (`re-notes/realhw/10-runner-bringup-spec.md:186`). Reconcile before coding DMA (the latter may
    be the XLMAC-adjacent SDMA view). Flagged.

11. **QM `MEM_AUTO_INIT` done bit was ★UNKNOWN, now LIVE-confirmed.** Originally the prime hang
    suspect (`re-notes/realhw/11-route-a-egress-spec.md:51-52`); live capture pinned
    `MEM_AUTO_INIT_STS@0x13c` = `0x01`, done = **bit0**
    (`re-notes/realhw/11-route-a-egress-spec.md:203-205`).

12. **Route A queue/group is not fully resolved.** `route_a_bbh_inst = 1` is solid (only QM-fed
    instance). The logical→physical QM queue for `port_gphy1` CPU-TX egress (port-relative q0 →
    which global queue in 80–111) is **unresolved**; carried as module params with candidate B
    (queue≈80/grp0/bb_id7/task3) vs candidate A (queue0/grp1/bb_id6/task4)
    (`re-notes/realhw/11-route-a-egress-spec.md:230-241`). Also unconfirmed that BBH_TX[1] is the
    instance for `port_gphy1` specifically.

13. **CPU_TX `is_egress`/`first_level_q`/`flow_or_port_id` logic + QoS-table format ★UNKNOWN.**
    Lives only in proprietary `rdpa_cpu_tx` (`re-notes/realhw/11-route-a-egress-spec.md:146-152`,
    `:187`).

14. **`rnr_image_first_task[core]` was flagged unpinned, then resolved to debug-only zeros** —
    cores start per-subsystem via `cfg_cpu_wakeup_set(...)`
    (`re-notes/realhw/10-runner-bringup-spec.md:39`, `:108-111`).

15. **MPM HW bring-up sequence has no GPL source** — register map recovered from `bcm_mpm.ko`
    disasm (`re-notes/realhw/10-runner-bringup-spec.md:162-170`) but bitfield names / per-ring
    strides are gaps; a single read-only `mpm_reg_dump` on silicon would resolve. MPM is outside
    the XRDP window (`0x80020000`/`0x4000`) and is skipped on the open first-light path.

16. **Exact 6813 `FC_UCAST_FLOW_CONTEXT_ENTRY` byte offsets are UNKNOWN** (autogen header stripped)
    — only the RDP-impl2 template is available (`re-notes/xrdp-offload-abi.md:126-149`, `:467`).

17. **NAT-C HW hash polynomial / bucket selection is engine-internal / UNKNOWN**
    (`re-notes/xrdp-offload-abi.md:88`, `:469`); the host writes key+result and lets HW place.

18. **ADD/COPY exact XPE opcode numbers are compiler-grouped ranges `[INFER]`**, not single
    pinned values (`re-notes/xrdp-offload-abi.md:190-196`, `:461`); the live cmdlist capture was
    GDX-local, so it pins encoding/framing, not a byte-match of L2/NAT programs (`:311-316`).

19. **SF2 switch core register base is NOT in the FDT.** The stock switch is exposed as
    "RUNNER_SW" with no `reg`; a `bcm_sf2`/DSA driver needs the SF2 core base, which requires
    deeper RE (`re-notes/bcm4916-regmap.md:270-276`). Separate subsystem.

20. **CPU/IMP port index + `mdio-sf2` busy/start-bit semantics + EGPHY/ethphytop mux sequence** are
    guessed/`[INFER]` in the DTS skeleton and need confirmation
    (`re-notes/bcm4916-regmap.md:249-266`, `:282-288`).

---

## 9. Open questions

1. **RX consumer model — ownership bit vs index polling?** (§5.3, finding 4.) The single most
   important thing to resolve on live silicon before the RX path can be trusted: does the 6813 CPU
   delivery ring signal via word2 bit31 ownership, or via `write_idx`/`read_idx` in the
   `CPU_RING_DESCRIPTOR`? The two RE passes disagree.

2. **Route A logical→physical queue map** (finding 12): the exact global QM queue for
   `port_gphy1`'s CPU-TX egress, and confirmation that BBH_TX[1] is that port's instance. Resolve
   by a refined kprobe on the port-object→queue base, or empirically via the `route_a_*` module
   params in the first egress test.

3. **Which TM image/thread owns LAN egress** (`RNRCFG_2.TASK` / `RUNNER_GRP.RNR_TASK`) for
   96813GW — the live map gives grp0=core7/task3 (DS_TM) and grp1=core6/task4 (US_TM); which one a
   LAN CPU-TX frame actually lands in is the queue-map question above
   (`re-notes/realhw/11-route-a-egress-spec.md:186`, `:230-241`).

4. **Per-port BBH register values** (finding 9) and **dispatcher credit/VIQ values** — accessor set
   and order pinned; the immediates need per-call disasm or a live regdump
   (`re-notes/xrdp-datapath-abi.md:562-566`).

5. **DMA/SDMA base reconciliation** (finding 10): is the datapath DMA at `0x828a1800`/`1c00`/`2000`
   (the `DMA_ADDRS` triple) or `0x828b2000`+`0x800` (the Wave-9 XLMAC-adjacent view), or are these
   two different DMA views?

6. **CPU_TX `is_egress`/`first_level_q` resolution + QoS-table format** (finding 13) — needed for
   correct QM-fed egress addressing.

7. **Exact 6813 `FC_UCAST_FLOW_CONTEXT_ENTRY` offsets, NAT-C hash, and the ADD/COPY opcode
   numbers** (findings 16–18) — the fast-path blockers; a single read-only dump of a live NAT'd
   flow's NAT-C entry (key + context + cmdlist bytes) would collapse most of these
   (`re-notes/xrdp-offload-abi.md:484-490`).

8. **Firmware licensing** — the Runner microcode is proprietary and non-redistributable, so a
   truly fully-open shippable datapath is not achievable today; any working datapath inherits a
   `P`-taint dependency on a user-supplied microcode image
   (`re-notes/runner-microcode-and-cpuring.md:100-116`).

9. **SF2 switch core base** (finding 19) and **clocks/resets** for the MAC/serdes nodes (not in the
   stock FDT) (`re-notes/bcm4916-regmap.md:291-293`) — control-plane subsystem inputs.
</content>
</invoke>
