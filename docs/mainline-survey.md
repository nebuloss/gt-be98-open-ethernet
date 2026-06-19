# Mainline Linux survey — Ethernet/switch extension surface for BCM4916 (ASUS GT-BE98)

**Surveyed tree:** mainline `linux` (stable.git checkout), `Makefile` reports
`VERSION=7 PATCHLEVEL=1 SUBLEVEL=0` → **kernel v7.1** (clone done `--depth=1` on dev-build, `~/mainline`).
**Method:** read source files directly via SSH on the build box. All file:line references below are against this v7.1 checkout. Nothing was built.

**Headline findings**
- The MAC (`bcm4908_enet.c`), switch (`bcm_sf2.c` + `b53`), and unimac MDIO drivers are all present and BCM4908 is a first-class, *fully wired* citizen of each.
- The GT-BE98's likely 10G PHY family is **already supported**: `drivers/net/phy/bcm84881.c` carries **BCM84881 / BCM84891 / BCM84892** (PHY ID `0x35905080` = BCM84891). This is the single biggest piece of "expected new work" that turns out to be drop-in.
- **No BCM4916 anything exists** (no compatible strings, no chip ID, no dtsi). Confirmed: `grep bcm4916` over `arch/arm64/.../bcmbca/` and the drivers returns nothing.
- **Caveat / gap:** `bcm4912.dtsi` exists but contains **no enet/switch/mdio/phy nodes at all** (only CPU, hsspi, base SoC). So there is *no* arm64 BCMBCA board in mainline that actually instantiates this Ethernet stack except the BCM4908 boards. BCM4908 is the *only* working reference DTS for the whole stack. BCM4916 register layout deltas are therefore **unknown from mainline** and must come from the vendor SDK / DT (not surveyed here, flagged below).

---

## 1. MAC — `drivers/net/ethernet/broadcom/bcm4908_enet.c` (+ `.h`)

Single-file driver (`bcm4908_enet.c`, 798 lines) + private header `bcm4908_enet.h` (96 lines).
Kconfig/Makefile: `drivers/net/ethernet/broadcom/Kconfig:55` `config BCM4908_ENET` ("Broadcom BCM4908 internal mac support"); `Makefile:7` `obj-$(CONFIG_BCM4908_ENET) += bcm4908_enet.o`.

### Binding / DT
- **Compatible (only one):** `"brcm,bcm4908-enet"` — `bcm4908_enet.c:782`.
- Resources from DT:
  - `enet->base = devm_platform_ioremap_resource(pdev, 0)` — single MMIO window (`:724`). In `bcm4908.dtsi` that is `reg = <0x2000 0x1000>` (a **0x1000 / 4 KiB** register block at SoC offset 0x2000) — `bcm4908.dtsi:135-142`.
  - IRQs by name: `platform_get_irq_byname(pdev, "rx")` (`:730`) and `"tx"` (`:734`). DT supplies `interrupt-names = "rx", "tx"` (GIC SPI 86/87) — `bcm4908.dtsi:139-141`.
  - MAC address: `of_get_ethdev_address()` else random (`:745-749`).
- Binding doc: `Documentation/devicetree/bindings/net/brcm,bcm4908-enet.yaml` — `compatible: const: brcm,bcm4908-enet` (`:19`), reg maxItems 1, interrupts rx(req)+tx(optional), `interrupt-names rx,tx`.

### Register map (all from `bcm4908_enet.h`)
Block bases (offsets within the single ioremap window):
- `ENET_CONTROL 0x000`, `ENET_MIB_CTRL 0x004` (`:5-6`), `ENET_GMAC_STATUS 0x028` (`:15`, with SPEED/HD/AUTO_CFG_EN/LINK_UP bits `:16-22`), `ENET_FLUSH 0x034` (RX/TX fifo flush `:26-28`).
- `ENET_MIB 0x200` (`:35`), **`ENET_UNIMAC 0x400`** (`:36`) — the UNIMAC sub-block base; all `enet_umac_*` accesses are `ENET_UNIMAC + offset` (`bcm4908_enet.c:119-132`). UNIMAC register *offsets* (UMAC_CMD, UMAC_MAX_FRAME_LEN, CMD_* speed bits, CMD_SW_RESET/TX_EN/RX_EN) come from the **shared `unimac.h`** (`#include "unimac.h"`, `:18`) — same header GENET/bcmgenet uses.
- **DMA block `ENET_DMA 0x800`** (`:37`): `ENET_DMA_CONTROLLER_CFG 0x800` (MASTER_EN bit0, FLOWC_CH1_EN bit1) `:38-41`; `ENET_DMA_CTRL_CHANNEL_RESET 0x834` `:55`.
  - Per-channel cfg blocks: `ENET_DMA_CH0_CFG 0xa00` (**RX**), `ENET_DMA_CH1_CFG 0xa10` (**TX**) `:59-60`.
  - Per-channel state-RAM blocks: `ENET_DMA_CH0_STATE_RAM 0xc00` (RX), `ENET_DMA_CH1_STATE_RAM 0xc10` (TX) `:61-62`.
  - In-block channel-cfg offsets: `ENET_DMA_CH_CFG 0x00` (ENABLE bit0), `_INT_STAT 0x04`, `_INT_MASK 0x08`, `_MAX_BURST 0x0c` `:64-76`.
  - State-RAM offsets: `_BASE_DESC_PTR 0x00`, `_STATE_DATA 0x04`, `_DESC_LEN_STATUS 0x08`, `_DESC_BASE_BUFPTR 0x0c` `:78-82`.
  - Descriptor ctl bits: `DMA_CTL_STATUS_APPEND_CRC 0x100`, `_WRAP 0x1000`, `_SOP 0x2000`, `_EOP 0x4000`, `_OWN 0x8000`, length field `DMA_CTL_LEN_DESC_BUFLENGTH 0x0fff0000 >>16` `:84-94`.

The driver aliases `ENET_DMA_CH_RX_CFG = ENET_DMA_CH0_CFG` / `ENET_DMA_CH_TX_CFG = ENET_DMA_CH1_CFG` (`bcm4908_enet.c:20-23`) — i.e. **hardwired to exactly 1 RX + 1 TX channel** (`reset_channel=0` "We support only 1 main channel", `:301`).

### Ring setup / sizes
- Ring sizes: `ENET_TX_BDS_NUM 200`, `ENET_RX_BDS_NUM 200`, `ENET_RX_BDS_NUM_MAX 8192` (`:25-27`). Allocated in `bcm4908_enet_dma_alloc()` (`:214`), descriptors via `dma_alloc_coherent` with 0x40 alignment check (`:169-194`). NAPI per ring (`:754-755`, tx via `netif_napi_add_tx`).
- Max burst `ENET_DMA_MAX_BURST_LEN 8` (64-bit words) `:32`.

### PHY / MII
- **No phylib, no MDIO, no phylink in this driver.** It calls *no* `phy_*`/`of_phy_*`. `bcm4908_enet_gmac_init()` hard-forces the MAC to **1000/full + LINK_UP** via `ENET_GMAC_STATUS` (`:420-430`) and sets UMAC `CMD_SPEED_1000`. Link/PHY management is entirely the **switch's** job (DSA). In DT the enet attaches to the switch CPU/IMP port via a `fixed-link { speed=1000; full-duplex; }` (`bcm4908.dtsi:268-277`, `port@8 ... ethernet = <&enet>`).

### MTU
- `bcm4908_enet_set_mtu()` writes `UMAC_MAX_FRAME_LEN = mtu + ENET_MAX_ETH_OVERHEAD(32)` (`:138-141`). `ENET_MTU_MAX = ETH_DATA_LEN` (1500; jumbo not supported) `:34`. `min_mtu=ETH_ZLEN, max_mtu=ENET_MTU_MAX` (`:751-753`). `.ndo_change_mtu` (`:692`).

### What BCM4916 needs (MAC)
The MAC IP is the GENET/UNIMAC + BCA-runner-DMA design shared across the BCA family, so the driver is highly likely to work *as-is* or with a new compatible string. Concretely:
1. Add `{ .compatible = "brcm,bcm4916-enet" }` to `bcm4908_enet_of_match[]` (`:781`) — or simply reuse `"brcm,bcm4908-enet"` in the bcm4916 DTS if the register block is identical (cheapest path).
2. **Unverified deltas** (cannot be confirmed from mainline — `bcm4912.dtsi` has no enet node): the DMA channel base offsets (0xa00/0xc00) and the number of DMA channels. On BCA SoCs with the "runner"/RDPA datapath the per-port enet may differ; if BCM4916 needs >1 channel or different offsets, that requires `of_device_id .data` carrying a register-layout/channel-count struct (the driver currently has **none** — all offsets are compile-time `#define`s). This is the main risk item; budget for a small `.data`-driven offset table if 4908 offsets don't match.
3. DT node: `reg` window + `rx`/`tx` named interrupts + `fixed-link` to the switch IMP port. Mirror `bcm4908.dtsi:135-142`.

---

## 2. Switch DSA — `drivers/net/dsa/bcm_sf2.c` + `drivers/net/dsa/b53/`

Architecture: **`bcm_sf2.c` is the platform/MMIO front-end for the integrated "Starfighter 2" switch**; it allocates a `b53_device` (`bcm_sf2.c:1390` `b53_switch_alloc(... &bcm_sf2_io_ops ...)`) and layers SF2-specific ops (`ds->ops = &bcm_sf2_ops`, phylink mac ops) on top of the generic **b53** core (`b53_common.c`). BCM4908 uses this SF2 path (MMIO), *not* the MDIO/SRAB/SPI b53 attach variants.

### Chip IDs / families
- `BCM4908_DEVICE_ID = 0x4908` defined in `b53_priv.h:75` (alongside `BCM7445=0x7445 :99`, `BCM7278=0x7278 :100`, `BCM58XX=0x5800 :97`).
- b53 chip table entry for BCM4908 — `b53_common.c:3034-3046`: `dev_name "BCM4908"`, `vlans=4096`, `enabled_ports=0x1bf` (ports 0-5,7,8 minus bit6), `arl_bins=4`, `arl_buckets=256`, **`imp_port=8`**, `B53_VTA_REGS`, GE duplex/jumbo regs, `b53_arl_ops_95`. Comment marks the SF2 group ("Starfighter 2", `:3032`).
- CPU/IMP port constants: `B53_CPU_PORT 8`, `B53_CPU_PORT_25 5` (`b53_priv.h:271-272`). BCM4908 uses port 8 as IMP/CPU (matches DTS `port@8`).
- Family helper `is58xx()` (`b53_priv.h:262-267`) groups 58xx/583xx/7445/7278/53134 — **BCM4908 is NOT in `is58xx`**; SF2 chips are driven mainly off `priv->type == BCM4908_DEVICE_ID` checks in `bcm_sf2.c` (e.g. `:38,77,100,520,874`).

### Attach / register layout (MMIO)
- `bcm_sf2_of_match[]` (`bcm_sf2.c:1348-1362`): **`brcm,bcm4908-switch`** (`:1349`, `.data = &bcm_sf2_4908_data`), plus `brcm,bcm7445-switch-v4.0`, `brcm,bcm7278-switch-v4.0/-v4.8`.
- Per-chip data `bcm_sf2_4908_data` (`bcm_sf2.c:1292-1299`): `type=BCM4908_DEVICE_ID`, `core_reg_align=0`, `reg_offsets=bcm_sf2_4908_reg_offsets` (`:1272`), `num_cfp_rules=256`, **`num_crossbar_int_ports=2`, `num_crossbar_ext_bits=2`**.
- Probe maps **6 named reg windows** `BCM_SF2_REGS_NAME` = `core, reg, intrl2_0, intrl2_1, fcb, acb` (loop `:1456` `for i<BCM_SF2_REGS_NUM`). DTS supplies exactly those 6 ranges + names — `bcm4908.dtsi:227-234` (`core 0x40000`, `reg 0x110`, `intrl2_0/1 0x30`, `fcb 0x34`, `acb 0x208`) and 2 interrupts (SPI 57/58).
- Two SF2 interrupts: `irq0/irq1 = irq_of_parse_and_map(dn, 0/1)` (`:1453-1454`).
- Ports parsed from the `ports` subnode (`bcm_sf2_identify_ports`, `:1448`).

### Ports / VLAN / CPU model
- Ports modeled the standard DSA way: `ports { port@N { reg; phy-mode; phy-handle | ethernet+fixed-link } }` (`bcm4908.dtsi:240-278`). BCM4908: ports 0-3 = internal GPHYs (`phy-handle = &phy8..11`, `phy-mode="internal"`), port 8 = CPU/IMP (`ethernet=<&enet>`, `fixed-link 1000/full`). DTS advertises `brcm,num-gphy=<5>` and `brcm,num-rgmii-ports=<2>` (`:237-238`).
- VLAN/ARL/FDB all handled generically in `b53_common.c` (`b53_imp_vlan_setup` `:540`, port enable/IMP handling around `:1331-1376`, `:2269-2371`).
- **Crossbar** (port-7 muxing to SERDES/GPHY4/RGMII) is BCM4908-specific: `bcm_sf2_crossbar_setup()` (`bcm_sf2.c:509`) switches on `BCM4908_DEVICE_ID` and writes `REG_CROSSBAR` using `CROSSBAR_BCM4908_*` bits (`bcm_sf2_regs.h:57-61`). Note the `if(0) /* FIXME */` SERDES branch (`:524`) — **10G SERDES on port 7 is stubbed/incomplete** even for BCM4908.

### DT bindings
- `Documentation/devicetree/bindings/net/dsa/brcm,sf2.yaml`: compatible enum (`:15-19`) = bcm4908-switch / bcm7278-v4.0 / bcm7278-v4.8 / bcm7445-v4.0; reg-names enum `core/reg/intrl2_0/intrl2_1/fcb/acb` (`:27-32`).
- `Documentation/devicetree/bindings/net/dsa/brcm,b53.yaml` covers the MDIO/standalone b53 family (not used by SF2/4908).

### What BCM4916 needs (switch)
1. **New chip ID** `BCM4916_DEVICE_ID = 0x4916` in `b53_priv.h` (next to `:75`), and a **b53 chip table entry** in `b53_common.c` (clone the BCM4908 entry `:3034`, adjusting `enabled_ports` to GT-BE98's actual port count and `imp_port`).
2. **New SF2 of_data + of_match**: add `bcm_sf2_4916_data` (clone `:1292`) and `{ .compatible="brcm,bcm4916-switch", .data=... }` to `bcm_sf2_of_match[]` (`:1348`). Provide a `bcm_sf2_4916_reg_offsets[]` if the SWITCH_REG sub-block moved (compare against `bcm_sf2_4908_reg_offsets` `:1272`).
3. Verify `num_crossbar_int_ports`/`num_crossbar_ext_bits` and the `CROSSBAR_*` encoding for the 4916 (likely different given the BE98's 10G ports). The crossbar code (`:509`) currently only knows `BCM4908_DEVICE_ID` — add a `case BCM4916_DEVICE_ID:` branch.
4. **10G / 2.5G port modes:** the 4908 path forces internal GPHYs and stubs SERDES (`if(0)` `:524`). The GT-BE98's 10G ports (BCM84891 PHYs on MDIO) will need the crossbar/SERDES path actually implemented, or those ports wired as external PHY ports through the unimac MDIO bus + phylink. This is the **largest switch-side work item** and is *not* covered by the existing BCM4908 code.
5. **DT:** clone the `bcm4908.dtsi` `ethernet-switch@0` + `ports` + `mdio` subtree; the register sub-windows (core/reg/intrl2/fcb/acb) and SPI numbers must come from the BCM4916 datasheet/vendor DT (not in mainline).

---

## 3. MDIO — `drivers/net/mdio/mdio-bcm-unimac.c`

- Compatible list `unimac_mdio_ids[]` (`:338-348`) includes **`brcm,unimac-mdio`** (`:347`) — the generic one BCM4908 uses — plus genet-mdio-v1..v5, bcm6846-mdio, asp-v2.x/v3.0-mdio.
- Bus model: a thin MMIO MDIO master. Registers (`:21-36`): `MDIO_CMD 0x00` (START_BUSY bit29, RD `2<<26`/WR `1<<26`, PMD shift 21, REG shift 16), `MDIO_CFG 0x04` (C22 bit0, CLK_DIV, SUPP_PREAMBLE). Poll-based completion (`unimac_mdio_poll` `:76`, ~30 µs/C22). Note `MDIO_C45 0` (`:33`) — C45 support is minimal/commented as TBD (`:82-84`).
- Reg window is tiny: DTS `mdio@405c0 { reg = <0x405c0 0x8>; reg-names="mdio"; }` (`bcm4908.dtsi:281-284`) — i.e. just the 8-byte CMD/CFG pair.
- PHYs hang off it as standard `ethernet-phy@N` children (`bcm4908.dtsi:288-306`: phy8..phy12 at addrs 8-12); switch ports reference them by `phy-handle`. The DSA driver also finds the bus via `of_find_compatible_node(... "brcm,unimac-mdio")` (`bcm_sf2.c:617`).

### What BCM4916 needs (MDIO)
**Drop-in.** Reuse `brcm,unimac-mdio` verbatim in the bcm4916 DTS; only the `reg` base changes. The BCM84891 10G PHYs are **Clause 45**; the driver's C45 path is thin (`:33,82`) — confirm C45 transactions work, but the BCM84891 driver itself uses `phy_read_mmd`/`phy_write_mmd` (MMD/C45) so the bus must support C45. If the unimac MDIO C45 is insufficient, the BE98 10G PHYs may sit on a separate C45-capable MDIO bus (vendor detail; flag for hardware verification).

---

## 4. PHY — `drivers/net/phy/bcm84881.c` (BCM84891 already supported!)

- **`bcm84881.c` covers BCM84881 / BCM84891 / BCM84892.** Driver array `bcm84881_drivers[]` (`:419`): BCM84881 (`phy_id 0xae025150`), **BCM84891 (`PHY_ID_MATCH_MODEL(0x35905080)`, `:432`)**, BCM84892 (`0x359050a0`, `:446`). `MODULE_DESCRIPTION("Broadcom BCM84881/BCM84891/BCM84892 PHY driver")` (`:472`). Author Russell King.
- Capabilities: Clause-45 PHY, copper NBASE-T up to **10G** with **10000/5000/2500/1000** speeds (`read_status` sets SPEED_10000/5000/2500 `:397-403`), 2500BASE-X + SGMII line-side (`:7,64,74`). LED HW-offload (`bcm8489x_led_*`) for the 8489x parts (`:425-444`). Ops: `config_init`/`bcm8489x_config_init`, `get_features`, `config_aneg`, `read_status`, `inband_caps` (`:421-444`).
- bcm-phy-lib (`bcm-phy-lib.c/.h`) provides shared Broadcom PHY helpers; `broadcom.c` covers the 1G BCM5xxx GPHYs (the switch's internal GPHYs are handled inside the switch, not here).

### What BCM4916 needs (PHY)
**Essentially drop-in for the 10G ports** *if* the GT-BE98 uses BCM84891 (PHY ID `0x35905080`). Action: just reference the PHY in DT (`phy-handle` / phy node on the MDIO bus) and enable `CONFIG_BCM84881_PHY`. New PHY work is needed **only** if the BE98 uses a different/newer PHY ID not in the table (would be a small additive entry, since the 8489x ops are already generic). The internal 1G GPHYs are handled by the switch core, no separate driver. **This eliminates what would otherwise be the hardest layer.**

---

## 5. DTS — `arch/arm64/boot/dts/broadcom/bcmbca/`

- Kconfig: **`ARCH_BCMBCA`** (`arch/arm64/Kconfig.platforms:90`, "Broadcom Broadband Carrier Access (BCA) origin SoC", selects GPIOLIB). This is the platform symbol the GT-BE98 build will set.
- Existing dtsi/dts: `bcm4908.dtsi`, `bcm4912.dtsi`, `bcm4906.dtsi`, plus boards incl. `bcm4908-asus-gt-ac5300.dts`, `bcm4912-asus-gt-ax6000.dts`, `bcm94908.dts`, `bcm94912.dts`. **No bcm4916.** (`ls` of the dir.)
- **`bcm4908.dtsi` is the canonical reference** for the whole stack. SoC node structure:
  - Root: `interrupt-parent=<&gic>`, `#address-cells=2 #size-cells=2` (`:13-14`); 4× `brcm,brahma-b53` CPUs spin-table (`:28-62`); `arm,gic-400` in `axi@81000000` (`:81-95`); armv8 timer, a53 PMU (`:97-112`); fixed clocks `periph_clk 50MHz`, `hsspi_pll 400MHz` (`:114-127`).
  - **`soc { ranges = <0 0 0x80000000 0x281000> }`** (`:129-133`) — SoC peripheral window based at **0x8000_0000**.
    - `enet: ethernet@2000` `brcm,bcm4908-enet` reg `<0x2000 0x1000>`, SPI 86/87 rx/tx (`:135-142`).
    - `bus@80000 { ranges = <0 0x80000 0x50000> }` (`:219-223`) holds the switch + mdio:
      - `ethernet-switch@0` `brcm,bcm4908-switch`, the 6 reg windows + names, SPI 57/58, `brcm,num-gphy=5`, `brcm,num-rgmii-ports=2`, `ports` (0-3 internal GPHY, 8 = CPU→enet fixed-link) (`:225-279`).
      - `mdio: mdio@405c0` `brcm,unimac-mdio` reg `<0x405c0 0x8>`, phy8..phy12 (`:281-307`).
    - `procmon: bus@280000` → `pmb` power-controller `brcm,bcm4908-pmb` (`:310-323`).
  - `bus@ff800000` (PERF peripherals, base 0xff80_0000): twd/watchdog syscon, 10× gpio banks, pinctrl (`brcm,bcm4908-pinctrl`), uart0 (`brcm,bcm6345-uart` SPI 32), leds, rng, hsspi, nand, i2c, pl081 dma (`:327-749`); `syscon-reboot` via twd (`:751-756`).
- **`bcm4912.dtsi` (gap):** `compatible="brcm,bcm4912","brcm,bcmbca"` (`:10`) but **contains no enet / switch / mdio / phy / serdes node** (grep returns only the hsspi compatible). So there is no closer-generation reference than 4908 for the networking subtree.
- **No `serdes` / dedicated 10G-serdes node** anywhere in bcm4908.dtsi (the SF2 SERDES path is the stubbed `if(0)` in the driver). No standalone `syscon` for enet beyond twd.
- Binding docs present: enet `brcm,bcm4908-enet.yaml`, mdio `brcm,unimac-mdio.yaml`, switch `dsa/brcm,sf2.yaml` (+ `dsa/brcm,b53.yaml`). No `brcm,bcm4916-*` binding exists.

### What BCM4916 needs (DTS)
1. New `bcm4916.dtsi` modeled on `bcm4908.dtsi`: CPU cluster (4× A53), GIC, timers, clocks, then the `soc`/`bus` ranges and the enet/switch/mdio subtree. **Every base address, reg size and SPI number must come from the BCM4916 datasheet or the vendor (SDK) DT** — they are *not* derivable from mainline; 4908 values are placeholders only.
2. A board `bcm4916-asus-gt-be98.dts` including the dtsi, enabling the right ports, wiring the BCM84891 10G PHYs on the MDIO bus, and the CPU/IMP `fixed-link`.
3. Add the board to `bcmbca/Makefile`. `ARCH_BCMBCA` already covers the platform Kconfig.

---

## REUSE MAP

| Layer | Mainline base (file) | Compatible / ID today | Effort for BCM4916 |
|---|---|---|---|
| **MAC** | `drivers/net/ethernet/broadcom/bcm4908_enet.c` (+ `.h`, `unimac.h`) | `brcm,bcm4908-enet` | **extend** — add compatible (or reuse 4908's); risk = DMA offsets/channel count if HW differs (no `.data` table today) |
| **Switch front-end** | `drivers/net/dsa/bcm_sf2.c` (+ `bcm_sf2_regs.h`) | `brcm,bcm4908-switch`, `bcm_sf2_4908_data` | **extend** — new of_data + of_match + crossbar `case`; verify reg_offsets |
| **Switch core** | `drivers/net/dsa/b53/b53_common.c` (+ `b53_priv.h`) | `BCM4908_DEVICE_ID 0x4908` chip entry | **extend** — add `BCM4916_DEVICE_ID` + chip table row |
| **10G SERDES / port-7 mux** | `bcm_sf2.c:509` crossbar (`if(0)` SERDES) | — (stubbed even for 4908) | **new** — biggest switch-side work; needed for BE98 10G uplinks |
| **MDIO** | `drivers/net/mdio/mdio-bcm-unimac.c` | `brcm,unimac-mdio` | **drop-in** — reuse, change reg base; confirm C45 for 10G PHYs |
| **PHY (10G NBASE-T)** | `drivers/net/phy/bcm84881.c` | **BCM84891 = `0x35905080` present** | **drop-in** if BE98 uses BCM84891; tiny add if different ID |
| **PHY (internal 1G GPHY)** | handled inside switch core | — | **drop-in** (via switch) |
| **DTS** | `arch/arm64/.../bcmbca/bcm4908.dtsi` | `brcm,bcm4908` (no 4916) | **new** dtsi+board, modeled on 4908; addresses from vendor/datasheet |
| **Platform Kconfig** | `arch/arm64/Kconfig.platforms:90` `ARCH_BCMBCA` | exists | **drop-in** |

Effort legend: **drop-in** = reuse unchanged / config only; **extend** = add IDs/compatibles/data entries to existing code; **new** = code that does not exist yet.

---

## Smallest path to the PRIME DIRECTIVE (one port: link + ping)

Goal: bring up **one** port (use a 1G internal-GPHY port + the CPU/IMP port) so we avoid the unimplemented 10G SERDES entirely.

1. **Confirm register/IRQ layout** for BCM4916 enet + switch + mdio from the vendor SDK/DT (the one thing mainline cannot give). Map: enet reg window + rx/tx IRQs; switch 6 reg windows (core/reg/intrl2_0/intrl2_1/fcb/acb) + 2 IRQs; unimac-mdio reg base; internal GPHY MDIO addresses.
2. **DSA core: add the chip.** `BCM4916_DEVICE_ID = 0x4916` in `b53_priv.h` + a chip-table row in `b53_common.c` (clone BCM4908 `:3034`; set `imp_port`, `enabled_ports` to just the one GPHY port + IMP for first bring-up).
3. **SF2 front-end: add of_data + of_match.** `bcm_sf2_4916_data` + `{ .compatible="brcm,bcm4916-switch" }` in `bcm_sf2.c:1348`; reuse `bcm_sf2_4908_reg_offsets` unless they moved. Add a `case BCM4916_DEVICE_ID` to `bcm_sf2_crossbar_setup` (`:509`) — or, since the prime-directive port is an internal GPHY (not port-7 SERDES), make the crossbar a no-op for it.
4. **MAC: make it bind.** Either add `{ .compatible="brcm,bcm4916-enet" }` to `bcm4908_enet_of_match` (`:781`) or reuse `brcm,bcm4908-enet` if offsets match. Confirm the 0xa00/0xc00 DMA channel offsets are valid on 4916 (the single risk).
5. **Minimal DTS.** New `bcm4916.dtsi` (CPU/GIC/clocks/soc ranges) + an `enet`, an `ethernet-switch@0`, and a `mdio` node with **one** internal GPHY child, **one** user `port@N (phy-mode="internal", phy-handle)`, and the CPU `port (ethernet=<&enet>, fixed-link 1000/full)`. Mirror `bcm4908.dtsi:135-307` exactly, swapping addresses. Add board `.dts` to `bcmbca/Makefile`.
6. **Kconfig:** `ARCH_BCMBCA=y`, `BCM4908_ENET=y`, `B53`+`BCM_SF2` (`CONFIG_B53`, `CONFIG_NET_DSA_BCM_SF2`), `BCM84881_PHY=y` (only needed later for 10G; not for the GPHY prime-directive port).
7. Bring up: `ip link set <port> up`, give it an IP, **ping** the directly-attached peer. Internal GPHY + IMP-fixed-link avoids MDIO/PHY/SERDES complexity for first light.

After the prime directive: tackle (a) BCM84891 10G PHYs on the unimac MDIO bus (driver already exists, just DT + C45 verification) and (b) the port-7/SERDES crossbar (`if(0)` stub) for 10G uplinks — the only genuinely *new* code.

---

## Things I could NOT verify from mainline (flagged, not fabricated)
- BCM4916 register bases / sizes / IRQ numbers / DMA channel count — **no BCM4916 in mainline**; `bcm4912.dtsi` has **no** networking nodes either. Must come from the BCM4916 datasheet or vendor SDK DT.
- Whether the GT-BE98's 10G PHY is exactly **BCM84891** (`0x35905080`). If yes → drop-in. The Broadcom GPL SDK (`broadcom-sdk-416L05`, datashed mirror) was *not* fetched in this pass; recommend a follow-up to pull the vendor `bcmbca` DT and `enet`/`rdpa_mw` glue to fill the register-delta gaps above.
- UNIMAC register offsets live in shared `drivers/net/ethernet/broadcom/unimac.h` (included by `bcm4908_enet.c:18`) — I confirmed the include and usage but did not dump that header's individual offsets; they are the standard GENET/UNIMAC set and are not expected to change for 4916.
