# 07 — QEMU device model (offline validation harness)

Audit of the QEMU device models that let the open BCM4916 driver be developed and
regression-tested **without silicon**. Scope:

| File | Role |
|---|---|
| `qemu/device-model/bcm4916_runner.c` | XRDP "Runner" CPU-conduit datapath model (FPM pool, CPU rings, RX/TX kick, NAT-C offload, Route A egress) |
| `qemu/device-model/bcm4916_sf2.c` | Control-plane: SF2 switch core + UNIMAC MDIO master + fake PHYs + XPORT (serdes/MPCS/XLMAC) 10G blocks |
| `qemu/device-model/runner-emulation-contract.md` | The prose ABI contract the Runner model implements against the driver |
| `qemu/README.md` | Staged bring-up narrative + run commands |
| `qemu/device-model/bcm4916-qemu-virt.patch` | `hw/arm/virt.c` / `meson.build` glue that instantiates + FDT-wires the devices |
| `qemu/scripts/*` | initramfs build, boot smoke test, dgram frame-injection peers (surveyed, not deep-documented) |

These sources live only in this repo; they are **copied onto the build host** into
a QEMU 10.0.0 fork's `hw/net/` and built there (the dev box never builds). All
register offsets quoted are the RE'd BCM4916/BCM6813 XRDP offsets that are the
point of this driver and are safe to publish.

---

## 1. Purpose

The BCM4916 datapath is XRDP/"Runner"-centric: the CPU cannot DMA to a MAC
directly; every host-bound or host-originated frame crosses the Runner's CPU
rings, and every accelerated frame is rewritten by the Runner from a NAT-C
context. Silicon access is off-limits (Ethernet is the box's only management
link), so **the only way to exercise driver logic is to emulate the hardware.**

This subsystem is that emulator. It provides two QOM sysbus devices (three device
`TypeInfo`s) that, wired into QEMU's `virt` machine, present just enough of the
BCM4916 to let an **unmodified** mainline kernel + the open driver:

- **Control plane** (`bcm4916_sf2.c`): probe the Starfighter-2 switch through
  `bcm_sf2`/`b53` DSA, enumerate the internal/external PHYs through
  `mdio-bcm-unimac` + phylib, and bring an XPORT port to a **10G link state**
  through phylink + the open `pcs-bcm-xport` PCS.
- **Datapath** (`bcm4916_runner.c`): move real Ethernet frames MAC↔CPU through the
  open slow-path conduit (`driver/runner/bcm4916_runner.c`) with **no proprietary
  Runner microcode** — the model *is* the Runner. On top of the slow path it also
  models the **NAT-C connection table** so HW flow-offload (L2/VLAN and L3
  route+NAT/NAPT) can be proven: a first packet MISSes to the CPU, the driver
  programs a NAT-C entry, and subsequent packets HIT and are forwarded/rewritten
  in "hardware" with the CPU bypassed. It also models the **Route A** egress
  gating (QM + TM-core RUNNER_GRP + BBH_TX QMQ) so the driver's `route_a=1`
  register writes are regression-guarded.

Where it sits in the pipeline: it stands in for the whole `0x82000000` rdpa
window (FPM, per-core RNR_MEM SRAM, PSRAM staging, QM, BBH_TX, NAT-C) plus the
`bcm_sf2` switch/MDIO/XPORT control blocks. It is a **behavioural stand-in**, not
a cycle- or bit-accurate silicon model — several offsets it decodes are shared
driver↔model **contract placeholders** not yet pinned from the GPL RDD map
(section 5/6).

---

## 2. Architecture & data flow

### 2.1 Devices and where they are mapped

Instantiation and FDT emission live in `create_bcm4916()`, added to `hw/arm/virt.c`
by `bcm4916-qemu-virt.patch`. It runs only when the environment variable
`QEMU_BCM4916` is set (`patch` line 72), so a plain `virt` machine is undisturbed.

| Device / TypeInfo | QEMU MMIO base (synthetic) | Real SoC base | FDT compatible |
|---|---|---|---|
| `bcm4916-sf2` | `0x0a800000` | (hidden behind RUNNER_SW; models 4908 layout) | `brcm,bcm4916-switch` |
| `bcm4916-mdio` | `0x0a900000` | `0x837ffd00` (real) | `brcm,unimac-mdio` |
| `bcm4916-runner` | `0x82000000` (**real rdpa window**) | `0x82000000` | `brcm,bcm4916-runner` |
| `bcm4916-xport` serdes/mpcs/xport | `0x0aa00000` / `0x0aa01000` / `0x0ab00000` | `0x837ff500` / `0x828c4000` / `0x837f0000` | `brcm,bcm4916-{serdes,mpcs,xport}` |

The Runner is mapped at its **real** base `0x82000000` size `0x00caf004`
(`patch` lines 39-40). The XPORT blocks are mapped at *synthetic* bases because
the real MPCS base `0x828c4000` falls **inside** the rdpa window and would
collide (`patch` lines 43-55); the driver reaches them via DT phandles so only
self-consistency matters. The Runner is also the **DSA CPU-port conduit netdev**:
`create_bcm4916()` binds it to `-netdev id=rnet` (or a default `-nic`), and the
SF2 `port@8` (CPU/IMP) points `ethernet=&runner` (`patch` lines 99-103, 296-301),
replacing the Stage-2 Cadence-GEM stand-in.

IRQs: queue0 = GIC SPI 75, fpm = GIC SPI 107 (`patch` lines 41-42, 106-109),
level-high, emitted into the runner FDT node's `interrupts`/`interrupt-names`.

The runner FDT node carries `brcm,runner-emulated` (`patch` line 168), which the
driver reads as "emulated mode → do not load microcode". This is the linchpin
that makes a microcode-free model legitimate: the driver skips RNR_INST/RNR_PRED
uploads and treats the model as the executing Runner.

### 2.2 The Runner window sub-block decode (`bcm4916_runner.c`)

One `MemoryRegion` (`RUNNER_WINDOW_SIZE = 0x00caf004`) covers the whole window;
`runner_read`/`runner_write` decode by offset:

| Sub-block | Offset (from `0x82000000`) | Const | Modelled behaviour |
|---|---|---|---|
| PSRAM | `0x000000` | `XRDP_OFF_PSRAM` | NAT-C offload staging only (key/ctx/index/cmd) |
| RNR_MEM[0..7] | `0x700000 + n*0x20000` | `XRDP_OFF_RNR_MEM0` / `XRDP_RNR_MEM_STRIDE` | **CPU rings** live here (per-core data SRAM) |
| RNR_REGS[0..7] | `0x800000 + n*0x1000` | `XRDP_OFF_RNR_REGS0` | accepted + ignored (model IS the runner) |
| BBH_TX[0..3] | `0x890000 + n*0x2000` | `XRDP_OFF_BBH_TX0` | Route A: QMQ bits at `0x4b0`/`0x7b0` |
| NAT-C | `0x950000` | `XRDP_OFF_NATC` | model-only debug counters (`+0x00/+0x08/+0x10`) |
| FPM | `0xa00000` | `XRDP_OFF_FPM` | token pool cfg + alloc/free |
| QM | `0xc00000` | `XRDP_OFF_QM` | Route A: MEM_AUTO_INIT, ENABLE_CTRL, RUNNER_GRP |

### 2.3 CPU ring layout (the current, resynced-2026-06-22 contract)

The rings do **not** live in PSRAM; they live in per-core RNR_MEM data SRAM. The
model's offsets (`bcm4916_runner.c:92-95`) are byte-for-byte the driver's
(`driver/runner/bcm4916_runner.c` `PSRAM_CPU_RING_DESC_TABLE=0x3000`,
`PSRAM_CPU_TX_RING_DESC_TABLE=0x33e0`, `RDD_FEED_RING_DESC_TABLE=0x0f70`,
`CPU_TX_RING_INDICES_OFF=0x29c8`), cross-verified during this audit:

| Ring / entry | Core | RNR_MEM offset | Model const |
|---|---|---|---|
| RX delivery ring cfg (16-B `CPU_RING_DESCRIPTOR`) | core 3 | `+0x3000` | `RDD_RX_DELIV_RING_OFF` |
| FEED ring cfg | core 3 | `+0x0f70` | `RDD_FEED_RING_OFF` |
| TX ring cfg | core 2 | `+0x33e0` | `RDD_TX_RING_OFF` |
| TX indices `{read_idx@+0, write_idx@+2}` | core 2 | `+0x29c8` | `CPU_TX_RING_INDICES_OFF` |

Within a 16-B cfg block, `write_idx` is BE u16 @ `+6`, `read_idx` @ `+12`
(`RING_CFG_WRITE_IDX_OFF`/`RING_CFG_READ_IDX_OFF`). The runner advances `write_idx`
(RX delivery, feed consumption) and the host advances `read_idx`. **Delivery is
detected by write_idx vs read_idx polling — NOT a per-descriptor ownership bit**
(driver header comment, `bcm4916_runner.h:624`).

### 2.4 RX data flow (backend → guest)

1. Driver posts empty DDR buffers into the **feed ring** (8-B `CPU_FEED_DESCRIPTOR`,
   40-bit ABS pointer) and bumps the feed `write_idx` @ `+6` (the "feed doorbell").
   The model captures that write (`runner_write`, `bcm4916_runner.c:1435`).
2. A frame arrives on the QEMU backend → `runner_receive`. First it runs the
   **offload fast path**: compute the L2 key, then the L3 key, look up NAT-C. On a
   HIT it runs the embedded cmdlist and `qemu_send_packet`s the (rewritten) frame
   straight back out the conduit, **CPU ring untouched** (`bcm4916_runner.c:915-959`).
3. On a MISS (slow path): take the next unconsumed feed buffer, DMA the frame into
   it, write a 16-B `CPU_RX_DESCRIPTOR` into the delivery ring, advance delivery
   `write_idx` and feed `read_idx`, raise the RX IRQ (`bcm4916_runner.c:961-1041`).
4. A virtual-clock timer (`runner_rx_drain_check`) holds the level IRQ high until
   the guest's `read_idx` catches up to `write_idx`, then deasserts and re-arms the
   backend RX poll.

### 2.5 TX data flow (guest → backend)

1. Driver stages a 16-B TX descriptor in DDR, then writes the new `write_idx` as a
   BE u16 into TX indices `+2` (core 2 `+0x29c8+2`). **That index write is the
   doorbell** — there is no MMIO TX doorbell.
2. `runner_write` traps it (`bcm4916_runner.c:1463`) and calls `runner_tx_kick`:
   for each newly produced slot, read the descriptor, resolve `word3`'s FPM token to
   a DDR payload address, (optionally gate on Route A), `qemu_send_packet` the
   frame, free the token, advance the consumer index, and mirror `read_idx` back
   into TX indices `+0`.

### 2.6 FPM token math

`buffer_phys = pool_pbase + index * chunk_size`, with `pool_pbase` from
`POOL1_CFG2` and `chunk_size` from `POOL1_CFG1` (default 512 B). The model tracks a
64K-entry in-use bitmap; `ALLOC` is a **read** of `0x400`, `FREE` is a **write** of
`0x400`. Token format (ABI 3.1): bit31 VALID, bits[28:12] index, bits[11:0] size.

### 2.7 Control-plane data flow (`bcm4916_sf2.c`)

- **Switch probe**: `bcm_sf2` sets `chip_id` from the DT compatible, so `b53` skips
  `b53_switch_detect()` — there is **no register chip-ID read**. The model only has
  to answer `REG_SWITCH_REVISION`/`REG_PHY_REVISION` (in the "reg" region) and
  `CORE_IMP0_PRT_ID` (in the "core" region); everything else reads 0 / accepts writes.
- **PHY enumeration**: on the separate UNIMAC MDIO bus. The model is a synchronous
  MDIO master (START_BUSY always reads clear) with a per-PHY C22 register file that
  returns real Broadcom PHY IDs + a link-up BMSR, plus an MMD-over-C22 indirect path
  for the C45 10G XPHY status regs.
- **10G link**: the XPORT serdes reports PMD link + 10G speed and the MPCS reports
  rx-lock **once the driver has released their reset bits** — faking the proprietary
  Merlin PMD microcode lock so phylink's PCS state machine completes.

### 2.8 Route A egress gating

When `route_a=1`, the driver brings up QM (`0xc00000`) + a TM-core RUNNER_GRP + a
BBH_TX QMQ binding. The model records all three, then `runner_tx_kick` **gates**
emission: a frame egresses only if its descriptor `first_level_q` (word0 [30:22])
lies in an enabled group's `[start,end]` **and** some BBH_TX instance is QM-fed;
otherwise it is dropped with a `LOG_GUEST_ERROR`. With `route_a=0` the QM state
stays zero and TX emits unconditionally (legacy path).

---

## 3. Data structures

### 3.1 `bcm4916_runner.c`

**`RunnerRing`** (`bcm4916_runner.c:223-229`) — one CPU ring's parsed geometry:
`valid`, `base` (DDR phys), `depth` (entries), `entry_size` (bytes), `idx` (the
model's own producer/consumer index).

**`Bcm4916RunnerState`** (`bcm4916_runner.c:231-294`) — the device instance:

- FPM: `fpm_ctl/cfg1/cfg2/spare`, `chunk_size`, `pool_pbase`, `pool_ntokens`,
  `tok_used` (heap bitmap, 1 byte/index), `tok_avail`, `alloc_hint` (round-robin).
- Rings: `rx` (delivery, runner-written), `tx` (consumer index), `feed` (host-posted
  empty-buffer ring), `feed_rcons` (runner's feed read index).
- IRQ: `irq[2]` (queue0, fpm), `irq_asserted`, `rx_timer` (drain-check heartbeat).
- Captured cfg bytes: `rx_cfg[16]`, `tx_cfg[16]`, `feed_cfg[16]`, `tx_indices[4]`
  (`{read_idx@+0, write_idx@+2}` BE) — the model serves runner-advanced indices out
  of these on read.
- NAT-C: `natc_stage_key[16]`, `natc_stage_ctx[124]`, `natc_stage_idx` (staging), a
  64-entry `natc[]` table (`valid`, `key[16]`, `ctx[124]`, `ctx_len`), and
  `off_hits`/`off_misses` counters.
- Route A: `qm_mem_init`, `qm_enable`, `qm_grp[15]` (`start`,`end`,`bb_id`,`task`,
  `en`), `bbh_qmq[4]`, `route_a_egress`.

### 3.2 Register-map macros (`bcm4916_runner.c`)

- Window/blocks: `RUNNER_WINDOW_BASE/SIZE`, `XRDP_OFF_{PSRAM,RNR_MEM0,RNR_REGS0,FPM,
  NATC,QM,BBH_TX0}`, strides.
- Ring offsets: `RDD_RX_DELIV_RING_OFF 0x3000`, `RDD_FEED_RING_OFF 0x0f70`,
  `RDD_TX_RING_OFF 0x33e0`, `CPU_TX_RING_INDICES_OFF 0x29c8`, `CPU_RX_RING_CORE 3`,
  `CPU_TX_RING_CORE 2`, `RING_CFG_WRITE_IDX_OFF 6`, `RING_CFG_READ_IDX_OFF 12`.
- FPM: `FPM_CTL` (+bits `INIT_MEM` 1<<4, `SOFT_RESET` 1<<14, `POOL1_ENABLE` 1<<16),
  `FPM_POOL1_CFG1` (bits[26:24] buf-size), `FPM_POOL1_CFG2` (base, mask
  `0xfffffffc`), `FPM_POOL1_STAT2`, `FPM_SPARE 0xc4`, `FPM_POOL0_ALLOC_DEALLOC 0x400`;
  token bitfields `FPM_TOKEN_VALID`, `FPM_TOKEN_INDEX_SHIFT 12`,
  `FPM_TOKEN_INDEX_MASK` (17-bit).
- QM (Route A): `QM_ENABLE_CTRL 0x000`, `QM_MEM_AUTO_INIT 0x138`,
  `QM_MEM_AUTO_INIT_STS 0x13c`, `QM_RUNNER_GRP_BASE 0x300` stride `0x10`,
  `QM_RGRP_RNR_CONFIG 0x00`, `QM_RGRP_QUEUE_CONFIG 0x04`, `QM_NUM_GROUPS 15`;
  BBH_TX `BBH_TX_QMQ_LAN 0x4b0`, `BBH_TX_QMQ_UNIFIED 0x7b0`.
- NAT-C staging: `PSRAM_NATC_STAGE_KEY 0x0100`, `PSRAM_NATC_STAGE_CTX 0x0120`,
  `PSRAM_NATC_INDIR_INDEX 0x0200`, `PSRAM_NATC_INDIR_CMD 0x0204`
  (`NATC_CMD_ADD 3`, `NATC_CMD_DEL 4`).
- Context byte offsets: `CTX_OFF_FLAGS 8` (`CTX_FLAG_IS_L2_ACCEL` 1<<4,
  `CTX_FLAG_IS_NAT` 1<<3, `CTX_FLAG_IS_ROUTED` 1<<5), `CTX_OFF_VPORT 12`,
  `CTX_OFF_CMDLIST_OFF 24`, `XPE_CMDLIST_MAX_M 80`, `CTX_OFF_CMDLIST_DLEN 104`,
  `CTX_OFF_CMDLIST_LEN 105`, `CTX_OFF_VALID 106`, `CTX_ENTRY_MAX 124`.
- XPE opcodes (`opcode = byte0 >> 2`): `XPE_OP_REPLACE_BITS 0x14` (byte0 0x50),
  `XPE_OP_MOVE_PACKET 0x13` (0x4c), `XPE_OP_REPLACE 0x18` (0x60),
  `XPE_OP_ADD 0x1a` (0x6a, decrement_8/TTL), `XPE_OP_ICSUM 0x1c` (0x70),
  `XPE_OP_NOP 0x3f` (switch default), `XPE_PAD_BYTE 0xfc` (slot pad, outside dlen).
- RX/TX descriptor bitfields: `RXD_W1_ABS 1<<16`, `RXD_W1_PKT_LEN_SHIFT 2`,
  `RXD_W1_PKT_LEN_MASK 0x3fff`, `RXD_W1_PTR_HI_SHIFT 24`, `RXD_W2_IS_SRC_LAN
  0x80000000`, `RXD_W2_SRC_PORT_SHIFT 25`; feed `FEED_W1_ABS 1<<8`,
  `FEED_W1_PTR_HI_MASK 0xff`.
- IPv4 offsets (untagged): `L2_HLEN_M 14`, `IP4_OFF_TTL_M 22`, `IP4_OFF_CSUM_M 24`,
  `IP4_OFF_SADDR_M 26`, `IP4_OFF_DADDR_M 30`.
- IRQ: `RUNNER_IRQ_QUEUE0 0`, `RUNNER_IRQ_FPM 1`, `RUNNER_NUM_IRQ 2`; defaults
  `FPM_CHUNK_SIZE_DEFAULT 512`, `RX_BUF_MAX 2048`, `DESC_SIZE 16`,
  `FPM_MAX_TOKENS 64K`, `NATC_MAX_ENTRIES 64`.

### 3.3 `bcm4916_sf2.c`

**`Bcm4916Sf2State`** (`bcm4916_sf2.c:113-121`): `reg_switch_cntrl`,
`reg_sphy_cntrl`, and a `regfile[0x110/4]` (68 words) backing the "reg" region so
writes read back. Region layout constants `SF2_CORE_BASE/SIZE 0x00000/0x40000`,
`SF2_REG_BASE 0x40000`, `SF2_WINDOW_SIZE 0x41000`; "reg" offsets
`REG4908_SWITCH_REVISION 0x10`, `REG4908_PHY_REVISION 0x14`, `REG4908_SWITCH_CNTRL
0x00`, `REG4908_SPHY_CNTRL 0x24`, `REG4908_CROSSBAR 0xc8`, `REG4908_RGMII_11_CNTRL
0x14c`; "core" `CORE_IMP0_PRT_ID 0x0804`; rev constants `SF2_TOP_REV 0x0053`,
`SF2_CORE_REV 0x0006`, `SF2_PHY_REV 0x0000`.

**`Bcm4916MdioState`** (`bcm4916_sf2.c:316-330`): `cmd`, `cfg`,
`phy_regs[32][32]` C22 files, `phy_present[32]`, MMD indirection
`mmd_devad[32]`/`mmd_addr[32]`, and sparse XPHY MMD stores `xphy_vend1_status[32]`
/`xphy_an_aux[32]`. MDIO CMD bitfields `MDIO_START_BUSY 1<<29`, `MDIO_READ_FAIL
1<<28`, `MDIO_RD 2<<26`, `MDIO_WR 1<<26`, `MDIO_PMD_SHIFT 21`, `MDIO_REG_SHIFT 16`,
`MDIO_DATA_MASK 0xffff`; MMD `MII_MMD_CTRL 0x0d`/`MII_MMD_DATA 0x0e`; XPHY status
`XPHY_VEND1_STATUS_REG 0x400d`, `XPHY_VEND1_STATUS_10G 0x0038`,
`XPHY_AN_AUX_STAT_REG 0xfff9`, `XPHY_AN_AUX_STAT_VAL 0x0200`.

**`Bcm4916PhyDesc bcm4916_phys[]`** (`bcm4916_sf2.c:307-314`): the fixed PHY map —
addr 1/2/3/4 → id `0x359050e0` (internal EGPHY), addr 9 → `0x359050e1` (external
10G), addr 21 → `0x35905081` (external multigig). Any other addr reads `0xffff`.

**`Bcm4916XportState`** (`bcm4916_sf2.c:578-587`): three MMIO regions
(`serdes_io`/`mpcs_io`/`xport_io`), `serdes_ctrl[3]` latches, `mpcs_reg`, and an
`xlmac[XPORT_WINDOW/4]` (8 KB) sparse register file. Serdes fields
`SERDES_CONTROL 0x0c`, `SERDES_STATUS 0x10` (`SS_RX_SIGDET` b0, `SS_LINK_STATUS`
b2, `SS_PLL_LOCK` b3), `SERDES_STATUS_1 0x24` (`SS1_10G_SHIFT 20`),
`SC_SERDES_RESET 1<<2`; MPCS `MPCS_REG_OFF 0xf8` (`MPCS_PMD_RX_LOCK` b0,
`MPCS_SIGNAL_DETECT` b1, functional-group resets b3/b4/b5, `MPCS_FG_RESET_MASK`).

---

## 4. Function reference (file order)

### 4.1 `bcm4916_runner.c`

**`runner_dma_as(s)`** — `bcm4916_runner.c:297`. Returns `&address_space_memory`;
all descriptor/payload DMA uses the system address space. Trivial indirection so
every DMA site reads the same. Called by `runner_receive`, `runner_tx_kick`.

**`fpm_pool_reset(s)`** — `bcm4916_runner.c:306`. Zeroes the `tok_used` bitmap and
sets `pool_ntokens`/`tok_avail = FPM_MAX_TOKENS (64K)`, `alloc_hint = 0`. Called on
device reset/realize and on `FPM_CTL` `SOFT_RESET`/`INIT_MEM` writes. Note the pool
is always sized to the 64K maximum (`FPM_MAX_TOKENS = 32 MB / 512 B`) regardless of
`POOL1_CFG2` — see findings.

**`fpm_alloc(s)`** — `bcm4916_runner.c:316`. Round-robin-scan `tok_used` from
`alloc_hint`, mark the first free index used, decrement `tok_avail`, advance the
hint, and return `VALID | (idx<<12) | (chunk_size & 0xfff)`. Returns `0` (VALID
clear) when exhausted, which the driver reads as TX back-pressure. Called by
`runner_read` on a **read** of `FPM_POOL0_ALLOC_DEALLOC`. HW sequencing: models the
alloc-on-read semantics — a read has the side effect of consuming a token.

**`fpm_free(s, token)`** — `bcm4916_runner.c:336`. Extract `index =
(token>>12)&0x1ffff`; bounds-check; if currently used, clear and bump `tok_avail`.
Called by `runner_write` on a **write** of `0x400` and by `runner_tx_kick` after
each TX frame (and on a Route-A drop). Idempotent on an already-free index.

**`fpm_token_to_phys(s, token)`** — `bcm4916_runner.c:349`. `pool_pbase +
index*chunk_size`. This must agree bit-for-bit with the driver's buffer math. Used
by `runner_tx_kick` to find a TX payload in guest DDR.

**`runner_parse_ring(d, r, res_shift)`** — `bcm4916_runner.c:379`. Parse a 16-B BE
`CPU_RING_DESCRIPTOR`: `entry_size = (w0>>27)&0x1f` (BYTES), `depth =
((w0>>16)&0x7ff) << res_shift`, `base = (w3&0xff)<<32 | w2`. `res_shift` is
`RING_RES_32 (5)` for RX/TX, `RING_RES_64 (6)` for FEED — the `number_of_entries`
field is stored in resolution units. Sets `valid` iff depth, base and entry_size
are all non-zero. Called by the `CAPTURE_CFG` macro in `runner_write` when a cfg
block finishes being memcpy'd. **Ordering constraint**: only reparsed on the
block-completing store, so a partially written cfg is never acted on.

**`natc_add(s)`** — `bcm4916_runner.c:403`. Copy `natc_stage_{key,ctx}` into
`natc[stage_idx % 64]`, set `ctx_len = 124`, `valid = true`, and log the key +
`is_l2_accel` flag + embedded cmdlist bytes (capped at 60) for validation. Mirrors
the driver's `xrdp_natc_add`. Called by `runner_write` on `NATC_CMD_ADD` to the
indirect command register. (The log prints packet-derived key bytes; that is
runtime emulator output, not part of this doc.)

**`natc_del(s)`** — `bcm4916_runner.c:439`. Clear `natc[stage_idx % 64].valid`.
Called on `NATC_CMD_DEL`.

**`natc_compute_key(frame, len, key[16])`** — `bcm4916_runner.c:457`. Build the
16-B masked **L2** BE key exactly as the driver's `xrdp_build_key`: `w0 = DA[0..3]`,
`w1 = DA[4..5]|SA[0..1]`, `w2 = SA[2..5]`, `w3 = ethertype<<16 | vport<<4 |
vlan_present` with `vport = 0`. On a tagged frame (0x8100) it records the inner VID
+ inner ethertype. Called at the top of `runner_receive`'s fast path. Must match the
driver or a model lookup would never hit a driver-programmed entry.

**`natc_compute_l3_key(frame, len, key[16])`** — `bcm4916_runner.c:499`. Build the
16-B **L3 IPv4 5-tuple** BE key for a routed flow: `w0 = orig src IP`, `w1 = orig
dst IP`, `w2 = sport<<16|dport`, and a live-pinned `w3` byte layout `[ToS, 0x28
(key-class), dir|ack, 0x68 (trailer)]`. Only accepts untagged IPv4 TCP/UDP with a
parseable header (returns false otherwise). Uses the **original ingress** tuple
because the lookup runs before the cmdlist rewrites the packet. Called after the L2
key misses. The `w3` constants and direction/ack bits are **pinned from a live
capture** (re-notes), not independently silicon-derived — an inference point.

**`natc_lookup(s, key[16])`** — `bcm4916_runner.c:542`. Linear scan of the 64
entries for a `valid` slot with a matching 16-B key; returns index or -1. O(n);
fine for a 64-entry test table.

**`ones_complement_csum(p, n)`** — `bcm4916_runner.c:554`. Standard 16-bit
ones-complement sum with carry fold, odd-length tail handled. Used by the two csum
fixups.

**`fixup_ip_csum(frame, len, csum_off)`** — `bcm4916_runner.c:574`. Recompute the
IPv4 header checksum in place: derive `ip_off = csum_off - 10`, read IHL, zero the
field, sum over the header, write back. Bounds-checked. Called by `natc_run_cmdlist`
for an ICSUM op targeting `IP4_OFF_CSUM_M`.

**`fixup_l4_csum(frame, len, csum_off)`** — `bcm4916_runner.c:599`. Recompute the
TCP/UDP checksum over pseudo-header + L4 segment, assuming an **untagged IPv4 IHL=5**
frame. Zeroes the field, sums the pseudo-header (src/dst IP, proto, L4 length) then
the L4 bytes, folds, writes; applies the UDP "0 → 0xffff" rule. Called by
`natc_run_cmdlist` for an ICSUM op at any non-IP offset. **Approximation**: HW does
an RFC-1624 incremental delta; the model does a full recompute (identical on-wire
result for a well-formed frame).

**`natc_run_cmdlist(ctx, frame, len)`** — `bcm4916_runner.c:688`. The XPE byte-code
interpreter. Reads `dlen = ctx[CTX_OFF_CMDLIST_DLEN]`, walks command words from
`ctx + CTX_OFF_CMDLIST_OFF (24)` **until `pos` reaches `dlen`** (length-delimited;
no NOP terminator). Each 32-bit BE command word: `opcode = byte0>>2`, offset ops use
`byte1 = (offset>>1)+1`. A leading `0xfc` (`XPE_PAD_BYTE`) breaks out defensively.
Cases:
- `MOVE_PACKET (0x13)`: raw `from`/`to`/`nbytes`, no inline; `to>from` opens a hole
  (VLAN push), `from>to` closes one (VLAN pop), with `RX_BUF_MAX` bounds.
- `ADD (0x1a)`: decrement the byte @offset (TTL −1).
- `ICSUM (0x1c)`: `fixup_ip_csum` at `IP4_OFF_CSUM_M`, else `fixup_l4_csum`.
- `REPLACE (0x18)`: `byte3==4` → 32-bit replace (+4 inline, IP NAT); else 16-bit
  (+2 inline, L4 port NAPT). Immediate consumed from bytes **after** the word.
- `REPLACE_BITS (0x14)`: recover `position = byte3&0xf`, `width = (byte3>>4) -
  position + 1`; the +2 inline operand is pre-shifted `imm<<position`, masked to the
  `[(1<<width)-1]<<position` window (VLAN VID/PCP/ToS edit).
- `NOP (0x3f)` / default: early return (defensive; normally length-delimited).
Returns the new frame length. Called only on a NAT-C HIT in `runner_receive`.
**Ordering**: the emitter order (dec-TTL, IP NAT, IP csum, port NAPT, TCP csum) is
replayed verbatim, so the checksum fixups run *after* the field replaces.

**`rx_set_deliv_write_idx(s, widx)`** — `bcm4916_runner.c:809`. `stw_be_p` the RX
delivery `write_idx` into `rx_cfg+6`. **`rx_set_feed_read_idx(s, ridx)`** —
`bcm4916_runner.c:813`. `stw_be_p` the feed `read_idx` into `feed_cfg+12`. Both are
the runner-advanced indices the guest polls via `runner_read`. Called by
`runner_receive` after a delivery.

**`feed_write_idx(s)`** — `bcm4916_runner.c:820`. `lduw_be_p(feed_cfg+6)`; the
host-advanced feed doorbell value captured live on each doorbell write. Read by
`runner_can_receive` and `runner_receive`.

**`runner_rx_drain_check(opaque)`** — `bcm4916_runner.c:832`. Virtual-clock timer
callback implementing **level-IRQ semantics**. If the RX ring isn't up, re-arm at
1 ms. Otherwise compare delivery `write_idx` (rx_cfg+6) vs host `read_idx`
(rx_cfg+12): if equal (drained), deassert **both** queue0 and fpm lines, clear
`irq_asserted`, and `qemu_flush_queued_packets` to re-arm the backend RX poll (a
dgram backend stops calling us after a deferred receive; without the flush only the
first datagram is ever delivered); then re-arm a 1 ms heartbeat. If still pending,
keep the line high and re-check at 200 µs. Called by the timer set in `realize`,
`runner_receive`, and itself.

**`runner_can_receive(nc)`** — `bcm4916_runner.c:877`. Backend flow control:
returns true only if the delivery ring **and** feed ring are valid **and** an
unconsumed feed buffer exists (`feed_rcons != feed_write_idx`). See findings — this
also gates offload-HIT frames that never need a feed buffer.

**`runner_receive(nc, buf, len)`** — `bcm4916_runner.c:886`. The RX injection path.
Steps: (1) bail if rings not up; truncate `len` to `RX_BUF_MAX`. (2) **Offload fast
path**: compute L2 key → lookup; on miss compute L3 key → lookup; on a HIT, copy the
frame, run `natc_run_cmdlist`, bump `off_hits`, log (routed hits also log the
rewritten 5-tuple/TTL/csums), `qemu_send_packet` the result out the conduit, and
return **without** touching the CPU ring. On a miss bump `off_misses` and continue.
(3) Slow path: pull the next feed buffer (drop if the feed ring is empty), DMA the
frame into it, (4) write the 16-B `CPU_RX_DESCRIPTOR` (`word0` = buf low32, `word1`
= abs | len<<2 | ptr_hi<<24, `word2` = `RXD_W2_IS_SRC_LAN`, `word3` = 0). (5) Advance
`rx.idx` and `feed_rcons`, publish delivery `write_idx` and feed `read_idx` (desc
written first — dma_wmb-equivalent). (6) Raise **both** queue0 (SPI 75) and fpm (SPI
107) — the IRQ-name workaround (see findings) — set `irq_asserted`, and arm the
drain-check at 200 µs. Touches: FPM (via token math indirectly none here — RX uses
feed buffers), delivery/feed rings, IRQ lines. Callers: QEMU net layer.

**`runner_route_a_active(s)`** — `bcm4916_runner.c:1054`. Route A is "on" iff
`qm_enable != 0` and at least one `qm_grp[].en`. **`runner_route_a_egress_ok(s,
first_level_q)`** — `bcm4916_runner.c:1068`. True iff `first_level_q` is inside some
enabled group's `[start,end]` **and** some `bbh_qmq[]` is set. Both called by
`runner_tx_kick`.

**`runner_tx_kick(s, new_widx)`** — `bcm4916_runner.c:1090`. TX consume loop. For
each slot from `tx.idx` up to `new_widx % depth`: read the 16-B TX descriptor from
DDR, extract `len = (w0>>8)&0x3fff` and the `word3` FPM `token`; skip + log
malformed lengths. If Route A is active, gate on `first_level_q = (w0>>22)&0x1ff`
via `runner_route_a_egress_ok` — a failing frame is dropped (token freed, index
advanced, `LOG_GUEST_ERROR`). Otherwise resolve `buf_pa` via `fpm_token_to_phys`,
DMA `len` bytes, `qemu_send_packet`, bump `route_a_egress` if applicable, `fpm_free`
the token, advance `tx.idx`. Finally mirror `read_idx` into `tx_indices+0` so the
driver's ringstat sees consumption. Called by `runner_write` on the TX index-write
doorbell. **Ordering**: relies on the driver's `dma_wmb()` before the index write so
the descriptor is fully visible.

**`runner_read(opaque, addr, size)`** — `bcm4916_runner.c:1165`. MMIO read decode:
- FPM window: `FPM_CTL` returns the stored value with `INIT_MEM` cleared (instant
  init); `POOL1_CFG1/CFG2`/`SPARE` read back; `POOL1_STAT2` returns `tok_avail &
  0x3ffff`; `POOL0_ALLOC_DEALLOC` **allocates** (calls `fpm_alloc`).
- NAT-C window `+0x00/+0x08/+0x10`: `off_hits`/`off_misses`/`route_a_egress` (model-
  only debug counters, not silicon registers).
- QM window: `MEM_AUTO_INIT_STS` returns `MEM_INIT_DONE=1` instantly if init
  requested; `ENABLE_CTRL` returns `qm_enable`.
- RNR_MEM window: serves the runner-advanced indices **byte-swapped** (the driver
  does `be16_to_cpu(ioread16())`): RX delivery `write_idx` (core3 `+0x3000+6`), feed
  `read_idx` (core3 `+0x0f70+12`), TX `read_idx` (core2 `+0x29c8+0`).
- Everything else reads 0.

**`runner_write(opaque, addr, val, size)`** — `bcm4916_runner.c:1253`. MMIO write
decode:
- FPM: `FPM_CTL` (SOFT_RESET/INIT_MEM → `fpm_pool_reset`); `POOL1_CFG1` decodes the
  chunk size (0→512, 1→256, else 512); `POOL1_CFG2` sets `pool_pbase` (masked);
  `SPARE`; `POOL0_ALLOC_DEALLOC` **frees** (`fpm_free`).
- QM (Route A): `MEM_AUTO_INIT`, `ENABLE_CTRL`, and per-group `RUNNER_GRP`
  `QUEUE_CONFIG` (start/end) / `RNR_CONFIG` (bb_id/task/enable).
- BBH_TX: OR the QMQ bits at `0x4b0`/`0x7b0` into `bbh_qmq[inst]`.
- PSRAM: NAT-C staging — key (`stl_le_p` to reconstruct BE byte order from the
  memcpy_toio raw stores), context (byte-wise, size-agnostic), index, and command
  (`natc_add`/`natc_del`).
- RNR_MEM: the CPU-ring cfg captures via the `CAPTURE_CFG` macro (RX delivery @
  core3+0x3000, FEED @ core3+0x0f70, TX @ core2+0x33e0), the feed doorbell
  (core3+0x0f70+6, byte-swapped, + a `qemu_flush_queued_packets` to unblock a
  deferred RX), the RX delivery `read_idx` (core3+0x3000+12), and the **TX doorbell**
  (core2+0x29c8+2 → `runner_tx_kick`). The `read_idx` slot @+0 is ignored.
- RNR_REGS: accepted and ignored (the model is the runner).
- All other writes ignored.

The `CAPTURE_CFG(base_off, cfgbuf, ring, res)` macro (`bcm4916_runner.c:1397`)
stores the memcpy'd bytes into the cfg buffer and calls `runner_parse_ring` on the
block-completing store — the mechanism that turns a word-by-word `memcpy_toio` into
a parsed ring.

**`runner_ops`** (`bcm4916_runner.c:1489`) — `.endianness = DEVICE_NATIVE_ENDIAN`;
`valid` access size 1..8 but `impl` 1..4, so QEMU **splits an 8-byte STP/STR into two
ordered 32-bit handler calls**, matching the driver's word-by-word ring-cfg publish.

**`net_runner_info`** (`bcm4916_runner.c:1507`) — NIC client: `can_receive =
runner_can_receive`, `receive = runner_receive`.

**`bcm4916_runner_reset(dev)`** — `bcm4916_runner.c:1514`. Zero FPM regs
(chunk_size→512), rings, feed index, cfg buffers, TX indices, NAT-C table + staging,
counters; `fpm_pool_reset`; deassert both IRQs. **Does NOT reset the Route A state**
(`qm_*`, `bbh_qmq`, `route_a_egress`) — see findings.

**`bcm4916_runner_realize(dev, errp)`** — `bcm4916_runner.c:1544`. Allocate the 64K
`tok_used` bitmap, create the `rx_timer`, seed `chunk_size` + pool, init the MMIO
region + 2 IRQs, create the NIC (default MAC if unset), and arm the RX heartbeat at
1 ms. Callees: `fpm_pool_reset`, `qemu_new_nic`.

**`bcm4916_runner_class_init` / `vmstate_bcm4916_runner` / `bcm4916_runner_types`**
(`bcm4916_runner.c:1573-1610`). Sets realize/reset/props. **vmstate saves only
`fpm_ctl/cfg1/cfg2/chunk_size/pool_pbase`** — rings, NAT-C, feed indices, tok_used,
and Route A state are **not** migrated (finding). `bcm4916_runner_properties[]` is
`DEFINE_NIC_PROPERTIES(...)` **with no `DEFINE_PROP_END_OF_LIST()` terminator**
(finding).

### 4.2 `bcm4916_sf2.c` — SF2 switch core

**`sf2_read(opaque, addr, size)`** — `bcm4916_sf2.c:133`. If `addr < 0x40000`
(CORE): return 8 for `CORE_IMP0_PRT_ID` (the IMP/CPU port index → `bcm_sf2` sets
`num_ports = val+1`), else 0. Otherwise (`reg` region, `roff = addr - 0x40000`):
`SWITCH_REVISION` → `(SF2_TOP_REV<<16)|SF2_CORE_REV` (0x0053/0x0006), `PHY_REVISION`
→ 0, `SWITCH_CNTRL`/`SPHY_CNTRL` from state, else read the `regfile` word or log
`LOG_UNIMP` and return 0. Cosmetic revisions only — `b53` uses its own chip-table rev.

**`sf2_write(opaque, addr, val, size)`** — `bcm4916_sf2.c:170`. CORE writes accepted
silently. `reg`-region: latch `SWITCH_CNTRL`/`SPHY_CNTRL`; `SWITCH_REVISION`/
`PHY_REVISION` are read-only; else store into the `regfile` (or `LOG_UNIMP`).

**`sf2_ops`** (`bcm4916_sf2.c:202`) — native-endian, 1..4 byte access.
**`bcm4916_sf2_reset`** (`:212`) zeroes the two latches + regfile.
**`bcm4916_sf2_init`** (`:220`) creates the single `SF2_WINDOW_SIZE (0x41000)` MMIO
region. **`bcm4916_sf2_class_init`** (`:240`) sets desc/reset/vmsd. vmstate saves
both latches + the full regfile.

### 4.3 `bcm4916_sf2.c` — UNIMAC MDIO master + fake PHYs

**`mmd_reg_read(s, pa)`** — `bcm4916_sf2.c:333`. Resolve a C45-over-C22 MMD read:
devad 0x1e + addr 0x400d → `xphy_vend1_status[pa]`; devad 0x07 + addr 0xfff9 →
`xphy_an_aux[pa]`; else 0. Feeds the 10G XPHY `read_status` in patch 0004.

**`mdio_phy_read(s, pa, reg)`** — `bcm4916_sf2.c:348`. Absent PHY → `0xffff`. Reg
`MII_MMD_DATA (0x0e)` → `mmd_reg_read`; else the C22 file. **`mdio_phy_write(s, pa,
reg, val)`** — `bcm4916_sf2.c:361`. `MII_MMD_CTRL` latches `mmd_devad`; `MII_MMD_DATA`
latches `mmd_addr` (treated as the address phase — see findings); else write the C22
file. Models `phy-core.c::mmd_phy_indirect`.

**`mdio_read(opaque, addr, size)`** — `bcm4916_sf2.c:386`. `MDIO_CMD` returns `cmd`
with `START_BUSY` forced clear (synchronous model); `MDIO_CFG` returns `cfg`.

**`mdio_write(opaque, addr, val, size)`** — `bcm4916_sf2.c:400`. `MDIO_CFG` latches.
`MDIO_CMD`: decode `pa`/`reg`; if `MDIO_RD` set, perform the read now and stash data
(+ `READ_FAIL` if PHY absent) in `cmd`; if `MDIO_WR`, perform the write; a bare
start-busy kick re-runs a prior RD (WR is not re-run — the WR already happened on
the CMD write). Models `unimac_mdio_start`'s read-then-poll.

**`mdio_ops`** (`:449`) — native-endian, 4-byte only.
**`mdio_init_phy(s, addr, id)`** — `bcm4916_sf2.c:457`. Mark present; set PHYID1/2
from `id`; `BMSR = 0x796d` (autoneg-complete + link-up + caps); `BMCR = 0x1000`
(autoneg on); for the 10G XPHY (`0x359050e1`) preload the VEND1 10G status + AN aux.
**`bcm4916_mdio_reset`** (`:482`) clears all state and re-installs the 6 fixed PHYs
from `bcm4916_phys[]`. **`bcm4916_mdio_realize_init`** (`:499`) creates the
`MDIO_WINDOW_SIZE (0x8)` region. **`bcm4916_mdio_class_init`** (`:518`) — vmstate
saves only `cmd`/`cfg` (the PHY files are rebuilt on reset).

### 4.4 `bcm4916_sf2.c` — XPORT serdes / MPCS / XLMAC

**`serdes_read(opaque, addr, size)`** — `bcm4916_sf2.c:590`. `core = addr/0x100`
(≥3 → 0). `CONTROL` returns the latch; `STATUS` returns
`LINK|PLL_LOCK|RX_SIGDET` **once the driver has cleared `SC_SERDES_RESET`** in
CONTROL, else 0; `STATUS_1` returns `1<<20` (lane0 @ 10G) once linked. This is the
faked PMD lock. **`serdes_write`** (`:620`) latches CONTROL per core.

**`mpcs_read(opaque, addr, size)`** — `bcm4916_sf2.c:640`. At `MPCS_REG_OFF (0xf8)`
return `mpcs_reg | PMD_RX_LOCK | SIGNAL_DETECT` **once all three functional-group
reset bits (clk_en/por_rstb/refclk_rstb) are set**, else the raw latch.
**`mpcs_write`** (`:655`) latches MPCS_REG.

**`xport_read`/`xport_write`** — `bcm4916_sf2.c:673/682`. Plain read/write-back
XLMAC register file (`xlmac[addr/4]`), no side effects — enough for
`bcm_sf2_xport_mac_enable`.

**`bcm4916_xport_reset`** (`:700`) holds all 3 serdes cores in reset
(`SC_SERDES_RESET`) so the link is "down" until the driver releases them; zeroes
mpcs + xlmac. **`bcm4916_xport_init`** (`:711`) creates the 3 MMIO regions
(serdes 0x300 / mpcs 0x100 / xport 0x8000) as sysbus mmio 0/1/2.
**`bcm4916_xport_class_init`** (`:739`) — vmstate saves all three latches/files.

**`bcm4916_types[]` / `DEFINE_TYPES`** (`bcm4916_sf2.c:749-773`) register the SF2,
MDIO and XPORT TypeInfos.

### 4.5 Scripts (surveyed)

- `scripts/build-initramfs.sh` — build a clean busybox aarch64 initramfs (no vendor
  blobs); `/init` mounts proc/sys/dev, prints `BE98_BOOT_OK`, and poweroffs on
  `be98_auto=poweroff`.
- `scripts/run-virt-baseline.sh` — Stage-0 smoke test: boot the mainline Image under
  `-machine virt -cpu cortex-a53 -smp 4 -m 1024`; pass iff the log has
  `BE98_BOOT_OK`. Uses a `virtio-net` `user` NIC (not the BCM4916 device).
- `scripts/dgram_peer.py` — the generic `-netdev dgram` peer: each UDP datagram is
  one raw Ethernet frame; `capture` mode logs+pcaps guest TX, `inject` mode also
  injects N RX frames after a delay.
- `scripts/offload_peer.py` — Phase-1 (L2/VLAN) two-phase peer: inject 4 MISS frames,
  wait for the driver to program NAT-C, then inject 6 HIT frames; counts HW-forwarded
  echoes.
- `scripts/nat_offload_peer.py` — Phase-2 (L3 route+NAT) peer: builds real Eth/IPv4/
  TCP frames with the original 5-tuple, injects MISS then HIT frames, and **verifies**
  the returned frames were SNAT/NAPT-rewritten with TTL decremented and both IP+TCP
  checksums correct (documentation/TEST-NET addresses only).
- `scripts/init-nat-offload.sh` — Phase-2 guest `/init`: bring `rnr0` up + promisc,
  let MISS frames arrive, `echo go > .../offload_nat_selftest` to program NAT-C, print
  `rx_packets` before/after, then poweroff.

Note: the `offload`/Phase-1 init script and the `run-validate.sh` referenced by the
contract are **not in this repo** — they live on the build host (see findings).

---

## 5. Audit findings

**F1 — The contract markdown documents a superseded ring layout (stale spec).**
`runner-emulation-contract.md` §3.1/§3.2/§4.1/§4.3/§7 still describe the CPU rings as
living in **PSRAM at `0x0000` (RX) / `0x0080` (TX)** with the **TX index doorbell in
`RNR_MEM[n]+0x0000`**, and §3 uses a per-descriptor `packet_length`/word2-ownership
protocol with **no feed ring**. The implemented model (and the driver, verified
against `driver/runner/bcm4916_runner.{c,h}`) instead puts the rings in per-core
RNR_MEM SRAM (RX `core3+0x3000`, TX `core2+0x33e0`, FEED `core3+0x0f70`, TX indices
`core2+0x29c8`), uses a **feed ring** for RX buffers, and detects delivery by
`write_idx`/`read_idx` polling (`bcm4916_runner.h:624`). Only the §6-Route A and §6b-
NAT-C parts of the contract are current. The prose contract is the audited artifact
but no longer matches the code it claims to specify — high-value to resync.
`runner-emulation-contract.md:91-219,347-369`.

**F2 — vmstate is drastically incomplete.** `vmstate_bcm4916_runner`
(`bcm4916_runner.c:1573`) migrates only 5 FPM scalars. Rings (`rx/tx/feed`),
`feed_rcons`, all cfg buffers, `tx_indices`, the 64-entry NAT-C table + staging, the
`tok_used` bitmap + `tok_avail`, and the entire Route A block are **not** saved. A
`savevm`/migrate would resume with the pool marked fully free but the guest believing
tokens are outstanding, and with the rings/NAT-C table gone. Acceptable for a
never-migrated harness, but it is silent state loss. (Same class, milder: the SF2/
MDIO/XPORT vmstates rebuild PHYs on reset so an in-flight MDIO/MMD transaction or
serdes/mpcs progress mid-migration is lost.)

**F3 — Device reset does not clear Route A state.** `bcm4916_runner_reset`
(`bcm4916_runner.c:1514`) resets FPM/rings/NAT-C/counters but leaves `qm_mem_init`,
`qm_enable`, `qm_grp[]`, `bbh_qmq[]`, and `route_a_egress` untouched. A cold boot is
fine (QOM zero-inits the instance) but a **warm reset/reboot** keeps stale QM/RUNNER_
GRP/QMQ config and a non-zero egress counter, which could mask a driver that fails to
re-program Route A after reset.

**F4 — `runner_can_receive` blocks offload-HIT frames when the feed ring is empty.**
`runner_can_receive` (`bcm4916_runner.c:877`) requires an unconsumed feed buffer.
But an offload HIT (`runner_receive` fast path, `:930`) forwards without consuming a
feed buffer. So when the feed ring drains, the backend stops delivering frames
entirely — including frames that would HIT and never touch the feed ring. In practice
the driver keeps the feed ring filled, so it rarely bites, but the gating condition is
stricter than the RX path actually needs.

**F5 — `Property` array lacks a `DEFINE_PROP_END_OF_LIST()` terminator.**
`bcm4916_runner_properties[]` (`bcm4916_runner.c:1587`) is just
`DEFINE_NIC_PROPERTIES(...)`. The classic QEMU `Property`-array API is NULL-name
terminated; without the terminator, iteration can run past the array. It reportedly
builds and runs on the build host, so either the QEMU 10.0 counted-props API tolerates
it or this is latent UB — must be verified against the exact QEMU 10.0.0 `Property`
API in the fork. (`bcm4916_sf2.c` devices define no extra props and are unaffected.)

**F6 — `word2 bit31` is overloaded (is_src_lan vs. ownership).** `runner_receive`
writes `word2 = RXD_W2_IS_SRC_LAN (0x80000000)` (`bcm4916_runner.c:1001`). The current
driver detects delivery via `write_idx` (correct), but the README Stage-3 evidence
shows an older driver polling `word2` bit31 as an ownership bit, and the contract §7
lists "ownership bit = word2 bit31". These are the *same* bit serving two different
semantic roles; the coincidence works but the intent is undocumented in the code and
the contract's ownership description is stale (tied to F1).

**F7 — Hardcoded / placeholder offsets, explicitly flagged.**
- PSRAM NAT-C staging offsets (`0x0100/0x0120/0x0200/0x0204`), the indirect
  add/del command values (3/4), the NAT-C key packing, and the context byte offsets
  (`CTX_OFF_*`) are **driver↔model contract placeholders**, internally consistent but
  not pinned from 6813 silicon (`bcm4916_runner.c:132-152`, README Stage-4/5 gaps).
- The L3-key `w3` layout (`0x28`/`0x68`/dir/ack) is **pinned from a single live
  capture**, not independently derived (`bcm4916_runner.c:496-537`).
- The Route A QM/BBH_TX register offsets (`QM_MEM_AUTO_INIT 0x138`, `QM_RUNNER_GRP_
  BASE 0x300`, `BBH_TX_QMQ_LAN 0x4b0`/`UNIFIED 0x7b0`) come from the RE notes; the QM
  RX/TX ring cfg-table offsets (`0x3000`/`0x33e0`) carry the misnomer
  `PSRAM_CPU_*` in the driver even though they are RNR_MEM (not PSRAM) offsets.

**F8 — FPM pool is always sized to the 64K maximum, ignoring `POOL1_CFG2`.**
`fpm_pool_reset` (`bcm4916_runner.c:306`) always sets `pool_ntokens = FPM_MAX_TOKENS
(65536)` regardless of the reserved region the guest advertises via `POOL1_CFG2`. If
the driver ever reserved a *smaller* pool, `fpm_alloc` could hand out an index whose
`fpm_token_to_phys` lands past the reserved DDR. The contract §2 says "size the pool
from POOL1_CFG2"; the model does not. Harmless with the driver's 32 MB/512 B pool
(exactly 64K), but not defensive.

**F9 — Jumbo RX is silently truncated.** `runner_receive` (`bcm4916_runner.c:903`)
clamps `len` to `RX_BUF_MAX (2048)` with only a "truncate jumbo to one chunk for
first light" comment — a frame >2048 B is corrupted, not dropped. First-light
limitation, but worth an explicit drop or scatter later.

**F10 — Read-with-side-effect + no MMIO reentrancy guard on alloc.** `runner_read`
of `FPM_POOL0_ALLOC_DEALLOC` mutates the pool (`fpm_alloc`). This is the intended
alloc-on-read ABI, but any double/speculative read path would consume two tokens.
Similarly the RX-inject and TX-kick DMA within an MMIO handler; QEMU's
`mem_reentrancy_guard` is passed to the NIC but the ring DMA has no explicit guard —
acceptable in the single-threaded model, noted for completeness.

**F11 — Both IRQ lines are pulsed on every RX (workaround, not fidelity).**
`runner_receive`/`runner_rx_drain_check` assert and deassert **both** SPI 75 and SPI
107 because `platform_get_irq_byname("queue0")` resolves to the fpm SPI on this DT
(README Stage-3 gap). This is a deliberate workaround; the real RDD interrupt id and
the DT `interrupts`/`interrupt-names` order remain unpinned. `bcm4916_runner.c:1034`,
`:850`.

**F12 — `run-validate.sh` (the TX=4/RX=6 proof) is not in the repo.** The contract
§6 asserts both the legacy and `route_a=1` variants "reach `VALIDATE_DONE` tx=4/rx=6"
via `run-validate.sh`, and the README quotes evidence logs, but no `run-validate.sh`
(nor the Phase-1 `init-offload.sh`) exists under `qemu/scripts/`. They live on the
build host only, so the published proof cannot be reproduced from this repo alone.

**F13 — MMD data writes are mis-latched as address phases.** `mdio_phy_write`
(`bcm4916_sf2.c:373`) always treats an `MII_MMD_DATA` write as the register-address
phase. A genuine MMD *data* write would be swallowed as an address. Benign because the
modelled MMD regs are read-only status, but a driver that writes an MMD register would
see no effect.

**F14 — Static link, no interrupt regions on the switch.** The SF2 `intrl2_0/1` are
not decoded (the model has no IRQ for the switch beyond the FDT stub SPIs 180/181),
link is static BMSR-up, and `CORE` reads default to 0 — so switch ISRs never fire and
counters/PHY state never change (README Stage-2 gaps). Expected for bind-only proof.

---

## 6. Open questions / unknowns

1. **True RDD table offsets.** `0x3000` (RX delivery cfg), `0x33e0` (TX cfg),
   `0x0f70` (FEED), `0x29c8` (TX indices) are pinned "vs SDK oracle 2026-06-22" but
   the driver still names them `PSRAM_CPU_*`; whether these are the exact per-image
   RDD core-data offsets on live silicon (and for which Runner image/core mapping)
   needs confirmation from the GPL RDD map / a live devmem capture.

2. **NAT-C key + context byte layout on 6813.** The key packing, the L3 `w3`
   direction/ack/key-class bytes, the context `CTX_OFF_*` offsets, `XPE_CMDLIST_MAX
   = 80`, and the indirect command/index register offsets are contract placeholders
   or single-capture inferences — the authoritative layout requires RE of the GPL
   `FC_UCAST_FLOW_CONTEXT_ENTRY_STRUCT` and `xpe_api` object.

3. **XPE opcode encoding vs. silicon.** The per-op byte0 values (0x50/0x4c/0x60/
   0x6a/0x70) and the `replace_bits` position/width packing are pinned from the RE'd
   emitter; the driver and model agree by construction, but a live cmdlist capture is
   needed to confirm the silicon Runner decodes them identically (esp. the
   length-delimited framing + `0xfc` pad).

4. **QM/BBH_TX Route A semantics.** The model treats "QM enabled + ≥1 RUNNER_GRP
   enabled + any BBH_TX QMQ set + first_level_q in range" as sufficient to egress.
   Whether real silicon additionally requires DSPTCHR egress credit / reorder-credit
   sequencing (the driver header notes stock reads `0x307` in ENABLE_CTRL and a
   credit-gated wakeup) is unmodelled — the gate is a self-consistency check, not a
   silicon egress model.

5. **The IRQ contract.** Which SPI actually carries `queue0` (75 vs 107), the
   `interrupt-names` order, and whether the real Runner raises an FPM IRQ at all are
   unresolved (F11); the model sidesteps it by pulsing both.

6. **10G is link-*state* only.** The serdes/MPCS "lock on reset-release" fakes the
   Merlin PMD microcode; real silicon needs the non-redistributable ~31 KB Merlin
   image and true AN. And because the modelled SF2/b53 MAC is 1G-class (MII/GMII),
   phylink caps the copper 10G XPHY (eth0) to GMII — the harness proves bind +
   `read_status` parse, not a live 10G copper link. True 10G data movement runs
   through the Runner/crossbar, which the control-plane model does not carry.

7. **Pool sizing / DSA user-port MTU.** The 512 B FPM chunk caps the DSA conduit MTU
   (~498 B) so switch *user* ports can't be exercised end-to-end (README Stage-3 gap);
   whether the real chunk is 512 B or larger, and how the conduit MTU is meant to be
   raised, is unresolved.
