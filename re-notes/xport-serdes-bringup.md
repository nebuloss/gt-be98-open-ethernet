# BCM4916 XPORT / serdes / MPCS 10G port bring-up — open driver design

Date: 2026-06-20. Closes the bind-coverage gap from `dts-upstream-rebase.md` sec 4:
the three multi-gig ports (eth0/eth1/eth3) hang off XPORT/XLMAC + MPCS + Merlin
serdes blocks that had placeholder DT compatibles and **no mainline driver**, so
only eth2 (1G UNIMAC→EGPHY) could link. This note records the GPL register
sequences recovered, the open driver design, the QEMU link result, and the
honest real-silicon gaps (the serdes microcode blob).

**Confidence tags:** `[SDK file:line]` = read from the Broadcom GPL "behnd" SDK
(asuswrt-merlin.ng src-rt-5.04behnd.4916, 6813A0 == 4916 variant); `[MAIN]` =
mainline v7.1 phylink/pcs reference; `[QEMU]` = observed in the QEMU model boot.

---

## 1. Blocks + register bases (RE'd from the GPL SDK)

| Block | Base | Window | Source |
|---|---|---|---|
| XPORT/XLMAC core | `0x837f0000` | per-port `+quad*0x4000 +port_in_quad*0x400` | `[SDK xport/ag/6813A0/XPORT_XLMAC_CORE_AG.c:6860]` |
| XPORT_TOP (GMII/XGMII mux) | `0x837f2000` (q0), `0x837f6000` (q1) | `+0x0` CONTROL | `[SDK XPORT_TOP_AG.c:382]` |
| MPCS (10G PCS) | `0x828c4000` | `MPCS_REG @ +0xf8` | `[SDK mpcs.c:142]`, DT `kernel/dts/6813/6813.dtsi:272` |
| Merlin serdes (Shortfin) | `0x837ff500` | 3 cores × `0x100` | `[SDK serdes_access_6813.h]`, DT `6813.dtsi:209` |

NB the older `bcm4916-regmap.md` placed the XLMAC at `0x828b2000`; the
authoritative `XPORT_XLMAC_CORE_ADDRS[]` array pins the XLMAC core inside the
XPORT window at **`0x837f0000`** (the `0x828b2000` window is the BBH/XLIF datapath
glue, not the MAC config regs). This driver uses `0x837f0000`.

## 2. XPORT / XLMAC MAC bring-up `[SDK xport_drv.c]`

XLMAC_CORE register offsets (64-bit regs; driver touches low 32 bits)
`[SDK XPORT_AG.h:4219+]`:

| Reg | Offset | Fields used |
|---|---|---|
| `XLMAC_CORE_CTRL` | `+0x00` | `TX_EN`[0], `RX_EN`[1], `SOFT_RESET`[6], `SW_LINK_STATUS`[12] |
| `XLMAC_CORE_MODE` | `+0x08` | `SPEED_MODE`[6:4] (shift 4, width 3) |
| `XLMAC_CORE_TX_CTRL` | `+0x20` | crc_mode/pad_en/tx_threshold |
| `XLMAC_CORE_RX_CTRL` | `+0x30` | strip_crc/runt/pass_ctrl |

`SPEED_MODE` encoding `[SDK xport_defs.h:103]`: 10M=0 100M=1 1G=2 2.5G=3
**10G=4 5G=5** (note the 10G/5G ordering quirk). GMII vs XGMII path is selected
per-port in `XPORT_TOP CONTROL` (Pn_MODE bit per port) `[SDK XPORT_AG.h:1804]`.

MAC enable sequence (`xport_xlmac_init` → `xport_xlmac_release_reset`
`[SDK xport_drv.c:411,373]`): set TX/RX_CTRL, write `XLMAC_MODE.SPEED_MODE`,
then **clear `XLMAC_CTRL.SOFT_RESET` and set TX_EN|RX_EN** (+ SW_LINK_STATUS).

Open driver: folded into `bcm_sf2_xport_mac_enable()` in `bcm_sf2.c`
(patch 0005) — called from `mac_link_up` for ports 5/6/7: programs SPEED_MODE
from the phylink-negotiated speed, then releases SOFT_RESET + enables TX/RX.

## 3. MPCS 10G PCS bring-up `[SDK mpcs.c]`

`MPCS_REG` (`0x828c40f8`) bitfields `[SDK mpcs.c:88]`: `pmd_mpcs_rx_lock`[0],
`md_mpcs_signal_detect`[1], `md_mpcs_tx_clk_vld`[2], `fg_mpcs_por_rstb`[3],
`fg_mpcs_clk_en`[4], `fg_mpcs_refclk_rstb`[5].

`mpcs_reg_init` `[SDK mpcs.c:208]`: deassert the functional-group resets in
order (clk_en → por_rstb → refclk_rstb), then poll `pmd_mpcs_rx_lock` /
`md_mpcs_signal_detect`. USXGMII speed programming (`mpcs_cfg` `[SDK mpcs.c:302]`)
writes the `WAN__AN_X4_USXGMII_*` an_config regs (0xc4b0/0xc4b1/0xc4b2; speed
code bits[11:9], 10G=3) — only needed for USXGMII; 10GBASE-R needs just the
reset release.

Open driver: `bcm_xport_mpcs_reg_init()` in `pcs-bcm-xport.c` (`.pcs_enable`).

## 4. Merlin16-Shortfin serdes bring-up `[SDK serdes_access_6813.h / phy_drv_shortfin.c]`

Per-core (stride `0x100`) regs `[SDK serdes_access_6813.h]`:
`CONTROL +0x0c` (`serdes_control_t`: iddq[0], refclk_reset[1], serdes_reset[2],
refsel[5:3], serdes_prtad[10:6], **serdes_ln_offset[15:11]** = lane→port mux),
`STATUS +0x10` (rx_sigdet[0], cdr_lock[1], **link_status[2]**, pll_lock[3]),
`STATUS_1 +0x24` (per-speed nibbles, 1 bit/lane: 10G nibble @ bit20).

Core init `_serdes_core_init` `[SDK phy_drv_shortfin.c:77]`: release
iddq/refclk_reset/serdes_reset + comclk, **PRAM-load the Merlin microcode**,
verify CRC, `PMD_setup_50_10p3125_VCO`, `datapath_reset_core`. Link/speed
read-back `[SDK serdes_access.c:336]`: `link = STATUS.link_status & (1<<lane)`,
speed decoded from `STATUS_1`.

Open driver: `bcm_xport_serdes_core_init()` + `bcm_xport_pcs_get_state()` in
`pcs-bcm-xport.c` — releases the reset bits, reads `STATUS`/`STATUS_1` to report
link + 10G speed.

### ⚠ serdes-firmware blob dependency (the real-silicon gap)

The Merlin serdes **will not achieve PMD lock on real hardware** until a
**~31 KB proprietary microcode** (`merlin16_shortfin_ucode_image[]`, version
`D102_0A`, **CRC 0x4949**) is PRAM-loaded into the serdes micro via
`merlin16_shortfin_ucode_pram_load()` `[SDK merlin16_shortfin_config.c:210]`.
That blob is **non-redistributable** (same class as the Runner microcode). The
open driver does all the surrounding register bring-up and polls PMD/PCS lock;
`bcm_xport_pcs_load_firmware()` is the TODO hook (a `dev_warn_once` placeholder
today). On real HW the user must supply the blob (extract from their own vendor
serdes lib, or a future licensed image). On QEMU the model fakes the lock so the
phylink path runs end to end.

## 5. Open driver design (mainline-idiomatic)

1. **`drivers/net/pcs/pcs-bcm-xport.c`** (patch 0006) — a register-mapped
   `phylink_pcs` (mirrors `pcs-lynx.c` `[MAIN]`): `.pcs_enable` brings up
   MPCS+serdes, `.pcs_config` validates the mode, `.pcs_get_state` reports
   link/speed from serdes `STATUS`/`STATUS_1` + MPCS `rx_lock`, `.poll = true`
   (no PCS IRQ). `supported_interfaces` = {10GBASER, USXGMII, 2500BASEX}.
   Exposed via `bcm_xport_pcs_create(dev, serdes_base, mpcs_base, emulated)`.

2. **bcm_sf2 wiring** (patch 0005):
   - `bcm_sf2_xport_pcs_setup()` resolves the `brcm,serdes` / `brcm,mpcs` /
     `brcm,xport` phandles on the switch node, `of_iomap`s the windows, and
     creates the PCS (propagating `brcm,runner-emulated` → emulated flag).
   - `.mac_select_pcs` returns the PCS for ports 5/6/7 in the 10G modes `[MAIN
     phylink.c:516]`.
   - `bcm_sf2_sw_get_caps` extended: for the XPORT ports advertise
     `PHY_INTERFACE_MODE_{10GBASER,USXGMII,2500BASEX}` in `supported_interfaces`
     and `MAC_2500FD|MAC_5000FD|MAC_10000FD` in `mac_capabilities` (without
     this, phylink gates out the 10G modes and the PCS is never selected).
   - `bcm_sf2_crossbar_setup` FIXME flipped: internal port 7 routes to
     `CROSSBAR_BCM4908_EXT_SERDES` when its mode is a serdes mode.
   - `bcm_sf2_xport_mac_enable` (XLMAC enable/speed) in `mac_link_up`.

3. **DTS** (`dts/bcm4916-enet.dtsi`): real compatibles
   (`brcm,bcm4916-xport`/`-mpcs`/`-serdes`), the switch node carries the
   `brcm,xport`/`brcm,mpcs`/`brcm,serdes` phandles; port phy-modes set
   (`10gbase-r`/`usxgmii`). dtc clean (`W=1`, no warnings).

## 6. QEMU model + link result

`qemu/device-model/bcm4916_sf2.c` gains `TYPE_BCM4916_XPORT`: three MMIO regions
(serdes `0x300` / mpcs `0x100` / xport `0x8000`) modelling exactly the regs the
driver touches. The serdes reports `link_status`+`pll_lock`+10G `STATUS_1` once
`SERDES_RESET` is cleared; MPCS reports `pmd_rx_lock` once the fg resets are
released — i.e. the model **fakes the proprietary PMD lock** so the phylink path
runs. `virt.c::create_bcm4916()` maps it at synthetic bases (the real MPCS base
`0x828c4000` falls inside the rdpa runner window, so all three use a separate
peripheral hole; the driver resolves them via DT phandles) and emits the FDT
nodes + switch phandles + a `port@6` 10GBASE-R fixed-link demo port.

### Result `[QEMU]` (mainline v7.1, `QEMU_BCM4916=1`, `cma=64M`)

```
brcm-sf2 a800000.ethernet-switch: BCM XPORT PCS created (serdes .. mpcs .., emulated)
brcm-sf2 a800000.ethernet-switch: found switch: BCM4916, rev 0
bcm4916-runner 82000000.runner: BCM4916 Runner conduit ready (hw, irq 21, fw absent)
DSA: tree 0 setup
brcm-sf2 a800000.ethernet-switch eth1: configuring for fixed/10gbase-r link mode
brcm-sf2 a800000.ethernet-switch eth1: Link is Up - 10Gbps/Full - flow control off
```
sysfs: `eth1 speed=10000`, `eth3 speed=1000` (rgmii), `eth0 speed=-1`.

**eth1 (DSA port@6 = XPORT) links at 10Gbps/Full** through phylink →
`bcm_sf2_sw_mac_select_pcs` → `pcs-bcm-xport` (serdes+MPCS) → `pcs_get_state`
reports 10G → `bcm_sf2_xport_mac_enable` (XLMAC). This is the first XPORT-port
10G link via the open driver (previously only eth2 1G could come up).

Boot prerequisites: `cma=64M` (the runner conduit reserves a 32 MB FPM pool) and
`-m 1024` (so the rdpa window `0x82000000` does not collide with `virt` RAM).

## 7. Honest TODOs / real-silicon unknowns

- **serdes PMD microcode (blob):** mandatory on real HW; not implemented (sec 4).
  The driver warns once and the lanes will not lock until it is loaded.
- **QEMU fakes PMD/PCS lock:** the model proves bind + phylink-select + PCS
  get_state + MAC enable + 10G link *state machine*, NOT silicon timing. No real
  serdes analog/AN, no real frames over the XPORT MAC (the datapath is the
  separate Runner conduit).
- **USXGMII per-mode programming** (`an_config` 0xc4b0 etc., MPCS PCS A/B
  select) is stubbed in `.pcs_config`; only 10GBASE-R is exercised. eth0/eth3
  (usxgmii + external PHY) are wired but only proven to the PCS-select stage.
- **XPORT_TOP GMII/XGMII mux + per-port crossbar lane (`serdes_ln_offset`)**
  values are RE'd but not written by the open driver yet (the QEMU 10GBASE-R
  fixed path does not need them); needed for the multi-lane / cascade ports.
- **Real SF2/XLMAC base offsets** are modelled from the SDK 6813A0 arrays, not
  confirmed against a live regdump (device work is read-only, no live test).
- **eth1 base computed via `bcm_sf2_xlmac_off(6)`** = quad1 port2 = `0x4800`;
  confirm the DSA-port→XLMAC-core index identity (assumed 1:1) on real HW.

---

## Source index
- GPL behnd SDK (4916, 6813A0): `bcmdrivers/opensource/phy/xport/{xport_drv.c,
  xport_defs.h,ag/6813A0/{XPORT_AG.h,XPORT_XLMAC_CORE_AG.c,XPORT_TOP_AG.c}}`,
  `bcmdrivers/opensource/phy/{mpcs.c,serdes_access.c,serdes_access_6813.h,
  phy_drv_shortfin.c,merlin_shortfin/src/merlin16_shortfin_config.c}`.
- mainline v7.1: `drivers/net/pcs/pcs-lynx.c`, `include/linux/phylink.h`,
  `drivers/net/phy/phylink.c`, `drivers/net/dsa/{bcm_sf2.c,bcm_sf2_regs.h,
  b53/b53_common.c}`.
- deliverables: `driver/pcs/pcs-bcm-xport.{c,h}`,
  `driver/mainline-patches/0005-*.patch`, `0006-*.patch`,
  `dts/bcm4916-enet.dtsi`, `qemu/device-model/bcm4916_sf2.c` +
  `bcm4916-qemu-virt.patch`.
```
