# DTS rebase onto the upstream mainline SoC dtsi (bcm6813)

Date: 2026-06-20. Kernel: mainline v7.1 (aarch64 cross build).

Goal: rebase the GT-BE98 board DTS off our hand-written SoC dtsi onto the
**upstream** mainline SoC dtsi, since `BCM4916 == the BCM6813 die` and mainline
already ships `arch/arm64/boot/dts/broadcom/bcmbca/bcm6813.dtsi` +
`bcm96813.dts`. Keep all our open-Ethernet/switch/Runner specifics.

---

## 1. Upstream SoC dtsi inventory

### `bcm6813.dtsi` (mainline v7.1, 164 lines, `(GPL-2.0+ OR MIT)`)

What it provides, node-by-node:

| Node | Detail |
|------|--------|
| `/ compatible` | `"brcm,bcm6813", "brcm,bcmbca"`; `#address/#size-cells = <2>`; `interrupt-parent = <&gic>` |
| `cpus` | 4x `brcm,brahma-b53` (cpu@0..3), `enable-method="psci"`, shared `L2_0` unified L2 cache |
| `timer` | `arm,armv8-timer`, PPI 13/14/11/10 |
| `pmu` | `arm,cortex-a53-pmu`, SPI 7..10, affinity to the 4 cores |
| `clocks` | `periph_clk` (200 MHz fixed), `uart_clk` (periph/4), **`hsspi_pll`** (200 MHz) |
| `psci` | `arm,psci-0.2`, method `smc` |
| `axi@81000000` | simple-bus, `gic: interrupt-controller@1000` = `arm,gic-400` (dist/cpu/hyp/vcpu at +0x1000/+0x2000/+0x4000/+0x6000) |
| `bus@ff800000` | simple-bus (window 0x800000) containing: `hsspi: spi@1000` (`brcm,bcm6813-hsspi`), `nand_controller@1800` (+`nandcs: nand@0`), `uart0: serial@12000` (`arm,pl011`, SPI 32) |

### `bcm96813.dts` (44 lines)

Reference board: `model`, aliases `serial0`, `chosen stdout-path`, `memory@0`
(128 MiB), and `&uart0/&hsspi/&nand_controller/&nandcs` set okay. No ethernet.

### `bcm4912.dtsi`

Byte-for-byte identical to `bcm6813.dtsi` except the `compatible` strings and
the `hsspi`/`nand` `brcm,bcm4912-*` compatibles. (Confirms the bcmbca SoC base
is shared boilerplate.)

### Critical finding: **upstream models ZERO ethernet**

`bcm6813.dtsi` / `bcm96813.dts` contain **none** of:
ethernet controller, SF2/switch, UniMAC MDIO, PHYs, pinctrl, reset/syscon,
serdes, PCS. So there is nothing upstream to *reuse* on the datapath side — only
the SoC base (CPU/GIC/timer/PMU/clocks/console/hsspi/nand) is shared.

### Upstream base vs our old hand-written `bcm4916.dtsi`

Our old `bcm4916.dtsi` re-implemented the SoC base (CPUs/GIC/timer/clocks/uart),
nearly identical to upstream, **plus** the whole Ethernet subsystem. Diffs of
the base portion:

- Old dtsi lacked `hsspi_pll`, `hsspi: spi@1000`, `nand_controller@1800` — all
  present upstream. (Pure win to take upstream.)
- GIC/timer/PMU/clocks/uart0/psci/cpus otherwise match upstream exactly.
- `compatible` upstream is `"brcm,bcm6813", "brcm,bcmbca"`; our board adds
  `"brcm,bcm4916"` at board level.

Conclusion: the SoC base is fully superseded by upstream. Only our ethernet
nodes are unique and must be preserved.

---

## 2. Reconciled structure

New three-file layout under `dts/`:

1. **`dts/bcm6813.dtsi`** — VERBATIM copy of mainline `bcm6813.dtsi` (provenance
   + license header prepended; body unchanged). Vendored so the board file
   builds standalone with cpp+dtc; in-tree builds resolve the identical in-tree
   copy. Provenance: `arch/arm64/boot/dts/broadcom/bcmbca/bcm6813.dtsi`,
   mainline v7.1, license `(GPL-2.0+ OR MIT)`.

2. **`dts/bcm4916-enet.dtsi`** — our open Ethernet overlay (the SoC base is now
   gone from here). Root-augment `/ { ... }` carrying only what upstream lacks:
   - `switch0: ethernet-switch@837ff000` (`brcm,bcm4916-switch`) + the 9 ports
     (port#0..7 + CPU port@8) with the [FDT]/[SDK] port->PHY map
   - `mdio: mdio@837ffd00` (`brcm,unimac-mdio`) + the EGPHY/serdes/xphy children
     (addr 1/2/3/4/7/8/9)
   - MAC cores: `unimac@828a8000`, `xlmac@828b2000`, `xport@837f0000`,
     `mpcs@828c4000`, `serdes@837ff500`
   - phy-mux/power: `ethphytop`, `egphy`, `wan_serdes_bus`
   - `runner: runner@82000000` (`brcm,bcm4916-runner`) CPU conduit
   - Uses `GIC_SPI`/`IRQ_TYPE_*` macros, in scope because `bcm6813.dtsi`
     (included first) pulls in the arm-gic/irq bindings.

3. **`dts/gt-be98.dts`** — clean board file:
   `#include "bcm6813.dtsi"` then `#include "bcm4916-enet.dtsi"`, then
   `model`/`compatible` (`asus,gt-be98` + bcm4916/bcm6813/bcmbca), aliases,
   chosen, `memory@0` (2 GiB), and the `&node { okay }` board overrides
   (enable the 4 wired ports + their PHYs, the cascade PHY@21, the runner, the
   placeholder MAC).

**Retired:** `dts/bcm4916.dtsi` deleted (SoC base superseded by upstream;
ethernet content moved to `bcm4916-enet.dtsi`).

What was reused from upstream vs added by us:

| Area | Source |
|------|--------|
| CPU/PSCI/GIC/timer/PMU/clocks/console/hsspi/nand | REUSED upstream `bcm6813.dtsi` (verbatim) |
| switch/MDIO/PHYs/MACs/PCS/serdes/phy-mux/runner | ADDED by us (`bcm4916-enet.dtsi`) — none exist upstream |
| board model/memory/chosen/port-enables/PHY compat | ADDED by us (`gt-be98.dts`) |

---

## 3. Build + validation (mainline v7.1, aarch64 cross build)

Placed `bcm4916-enet.dtsi` + `gt-be98.dts` into
`arch/arm64/boot/dts/broadcom/bcmbca/` (in-tree `bcm6813.dtsi` reused, identical
to vendored copy). Added `gt-be98.dtb` to the bcmbca `Makefile` `dtb-$(CONFIG_ARCH_BCMBCA)` list.

- **dtc (cpp+dtc, `W=1`):**
  `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- W=1 broadcom/bcmbca/gt-be98.dtb`
  → `DTC arch/.../gt-be98.dtb`, exit 0, **no warnings, no errors**.
  Output dtb 6663 bytes.
- **Decompile sanity (`dtc -I dtb -O dts`):** merged tree contains BOTH the
  upstream SoC nodes (`cpu@0`, `arm,gic-400`, `spi@1000`, `nand-controller@1800`,
  `serial@12000`) AND our ethernet nodes (`ethernet-switch@837ff000`,
  `mdio@837ffd00`, `ethernet-phy@21`, `ethernet@828a8000`, `runner@82000000`);
  aliases resolve (`serial0`, `ethernet0 = /ethernet@828a8000`).
- **All bcmbca dtbs:** `make broadcom/bcmbca/` → exit 0, no errors/warnings
  (Makefile edit valid).
- **Kernel Image:** with `CONFIG_ARCH_BCMBCA=y` and the target drivers
  (`B53=m`, `NET_DSA_BCM_SF2=m`, `MDIO_BCM_UNIMAC=y`, `BROADCOM_PHY=y`,
  `BCM84881_PHY=y`, `BCM4916_RUNNER=y`, `NET_DSA=m`, `NF_FLOW_TABLE=m`) plus our
  in-tree driver patches (b53 BCM4916 chip, bcm_sf2 `brcm,bcm4916-switch`
  binding, BCM4916 EGPHY `0x359050e0`, BCM84891/BCM4916 XPHY `0x359050e1`, and
  the `bcm4916_runner`/`cmdlist`/`flow_offload` objects): forced rebuild of
  `bcm4916_runner.c` + relink → `CC bcm4916_runner.o`, Image relinked, exit 0,
  **no errors/warnings**. Image = 42088960 bytes.

(Honesty note: the kernel Image does not embed the DTS, so the Image build does
not re-exercise dtc; the dtb was validated separately as above. The Image build
shown is a forced incremental rebuild of our driver + relink, confirming the
driver set compiles clean against this tree.)

---

## 4. Bind-coverage assessment (real-HW control-plane bring-up)

With the rebased DTS, on real silicon:

### WOULD bind (driver exists in this tree)

| DT node | compatible | Driver |
|---------|-----------|--------|
| `switch0` ethernet-switch@837ff000 | `brcm,bcm4916-switch` | `bcm_sf2` (+b53), via our `brcm,bcm4916-switch` binding + b53 BCM4916 chip patch |
| `mdio@837ffd00` | `brcm,unimac-mdio` | `mdio-bcm-unimac` (`MDIO_BCM_UNIMAC=y`) |
| `phy_gphy1@2` (eth2 EGPHY) | matched by PHY id `0x359050e0` | `broadcom.c` (our BCM4916 EGPHY patch) |
| `phy_xphy@9` (eth0 BCM84891) | `ethernet-phy-id3590.50e1` / c45 | `bcm84881.c` (our BCM84891/BCM4916 XPHY patch) |
| `phy_cascade1@21` (eth3 ext PHY) | `ethernet-phy-id3590.5081` / c45 | likely `broadcom.c`/`bcm84881.c` — **needs id confirmation** ([recon] id 0x35905081) |
| `runner@82000000` | `brcm,bcm4916-runner` | `bcm4916_runner` (`BCM4916_RUNNER=y`); slow-path CPU conduit (emulated flag for QEMU) |

### Would NOT bind — placeholder compatible, NO driver (the remaining gap)

| DT node | compatible | What's missing |
|---------|-----------|----------------|
| `unimac@828a8000` | `brcm,unimac3` | No mainline UniMAC-3 MAC driver. The MAC is currently driven implicitly via the switch/runner path; a standalone MAC driver (or folding MAC setup into bcm_sf2/runner) is needed for true per-port MAC control. |
| `xlmac@828b2000` | `brcm,xlmac1` | No XLMAC driver. |
| `xport@837f0000` | `brcm,xport` | No XPORT (10G/multigig MAC+PCS wrapper) driver — this is the key gap for the three XPORT ports (eth0/eth1/eth3) link bring-up. |
| `mpcs@828c4000` | `brcm,mpcs` | No 10G PCS driver; needed for serdes/10GBase-R + usxgmii link-up (a phylink/PCS driver). |
| `serdes@837ff500` | `brcm,serdes1` | No serdes/lane driver; needed to power/configure serdes0/serdes1 lanes. |
| `ethphytop@837ff004` | `brcm,eth-phy-top` | No phy-mux/reset driver; needed to release internal EGPHYs + external xphy from reset (stock `xphy0-enabled`). Likely should be a syscon/reset sub-function of the switch register window (overlaps switch0 reg @0x837ff000). |
| `egphy@837ff00c` | `brcm,egphy` | No EGPHY analog/power-control driver. |
| `phy_serdes0@7`, `phy_serdes1@8` | (no id; pseudo-PHY) | These are serdes pseudo-PHYs, not c22/c45-addressable copper PHYs; they need the serdes/PCS driver above, not a PHY driver. eth1 is modeled fixed-link (10GBase-R) so it can come up without an addressable PHY *once* the XPORT+PCS+serdes drivers exist. |

### Honest summary of the gap

- **Control plane that binds today:** the SF2 switch + DSA, the UniMAC MDIO bus,
  and the copper/c45 PHYs (eth2 1G EGPHY, eth0 BCM84891 10G, eth3 cascade) — so
  MDIO enumeration and DSA port/topology setup should work on real HW. The
  Runner conduit binds (slow path).
- **Still needs new drivers for full link bring-up:** the XPORT MAC wrapper, the
  MPCS 10G PCS, the serdes lane control, and the eth-phy-top/egphy
  reset/power blocks. Until these exist, the three XPORT ports
  (eth0 10G / eth1 10GBase-R / eth3 multigig) cannot achieve PHY/serdes link on
  real silicon even though their DT is complete; **eth2 (port#1, UniMAC gmii ->
  internal EGPHY @MDIO2)** is the cleanest first-light path because it avoids the
  XPORT/PCS/serdes stack entirely (only needs switch + MDIO + EGPHY, all of which
  bind today — modulo the egphy reset/power block which the bootloader may
  already have released).
- The MAC nodes (`unimac3`/`xlmac1`) have no standalone mainline binding; for a
  DSA-style model the MAC setup is expected to be subsumed by the switch/runner
  drivers rather than bound as separate platform devices.

### Open DTS TODOs (carried from the old dtsi, unchanged by the rebase)

- IMP/CPU port index (assumed `port@8`) — confirm against HW.
- MDIO second reg `0xff85a024` (clk/strap) not yet modeled.
- switch sub-block offsets (core/intrl2/acb) beyond `SWITCH_REG_BASE` unconfirmed.
- phy-mode mapping for XPORT ports (`usxgmii`/`10gbase-r`) is [INFER].
