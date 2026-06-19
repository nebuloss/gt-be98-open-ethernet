# BCM4916 / ASUS GT-BE98 — Ethernet + Switch register/topology RE notes

Reverse-engineered for mainline `bcm4916.dtsi` + `gt-be98.dts` authoring and a possible
`bcm4908_enet` / `bcm_sf2` extension.

- **Gold source**: live `/proc/device-tree` from the running stock kernel
  (`/opt/re-bins/live-fdt-device-tree.tar`, extracted to `/tmp/fdt` inside CT 310).
  Every FDT value below is decoded big-endian from the property file at the noted node path.
- **Secondary source**: vendor `bcm_enet.ko` (aarch64, "Broadcom Ethernet Interface v7.0")
  analyzed with r2 (`aa` + targeted `pdf`/`afl`/`iz`). No CT 310 reboot was needed.
- **Confidence tags**: `[FDT]` = read directly from the live device tree (confirmed hardware).
  `[DISASM]` = from bcm_enet.ko disassembly. `[INFER]` = deduced/cross-referenced, not a raw read.

SoC is **BCM4916** (Broadcom-v8A, 4× Cortex-A?? v8A), GIC `arm,cortex-a15-gic`. This is a
Broadcom **Runner / RDPA / XRDP** datapath SoC — NOT the simple DMA-controller design that
mainline `bcm4908_enet` targets. This single fact dominates the reuse verdict (see PART B).

---

## Top-level address layout `[FDT]`

| Block | SoC base | size | notes |
|---|---|---|---|
| GIC dist/cpu (`interrupt-controller@81000000`) | `0x81001000` / `0x81002000` | `0x1000`/`0x2000` | `arm,cortex-a15-gic`, phandle `0x1`, `#interrupt-cells=3` |
| `periph` bus (`ranges`) | child 0x0 → SoC `0xff800000` | `0x62000` | simple-bus, holds serial/nand/spi/pinctrl/gpio/i2c |
| `xrdp` bus (`ranges`) | child 0x0 → SoC `0x82000000` | `0x1000000` (`0x00cd0000` window) + second range child `0x1_00000000` → SoC `0xff800000` size `0x62000` | simple-bus, parent of `switch0`/`mdio`/`wan_serdes_bus` |
| `rdpa_drv` (whole Runner) | `0x82000000` | `0x00caf004` | `brcm,rdpa`; the entire runner register window |

Interrupt cells everywhere are `<type=GIC_SPI(0) num flags>` (3-cell, GIC).
The MAC/switch blocks themselves have **no `interrupts` property** — datapath IRQs are owned
by the runner via `rdpa_drv` (32 queue IRQs + fpm).

---

# DTS SOURCE MAP

Ready-to-translate node list for `bcm4916.dtsi` / `gt-be98.dts`. All reg values are
`<hi lo size_hi size_lo>` 2-cell/2-cell as in the live tree (`#address-cells=2`, `#size-cells=2`).

## MAC / datapath register blocks `[FDT]`

| FDT node | compatible | reg base(s) | size | extra props | maps to (mainline) |
|---|---|---|---|---|---|
| `/unimac` | `brcm,unimac3` | `0x828a8000`; `0x828b0000` | `0x5000`; `0x1400` | `conf_offset=0x1000` `mib_offset=0x400` `top_offset=0x400` | UNIMAC MAC cores (1G ports). cf. mainline `unimac`/`bcmgenet`-style block |
| `/xlmac` | `brcm,xlmac1` | `0x828b2000`; `0x82890000` | `0x680`; `0x6b88` | `xlmac_offset=0x200` `bbh_tx_offset=0x2000` | XLMAC (10G/multigig MAC) — no mainline equivalent in bcm4908_enet |
| `/xport` | `brcm,xport` | `0x837f0000`; `0x828b2000`; `0x837ff1f8` | `0x8000`; `0x1000`; `0x4` | — | XPORT 10G/multigig MAC+PCS wrapper (eth0/eth1/eth3 use this) |
| `/mpcs` | `brcm,mpcs` | `0x828c4000` | `0x100` | — | 10G PCS for XPORT/serdes |
| `/serdes` | `brcm,serdes1` | `0x837ff500` | `0x300` | — | on-die multi-lane serdes control |
| `/wan_serdes` | `brcm,pon-drv` | `0x828c4000` | `0x1c000` | `status=disabled` | PON/WAN serdes (overlaps mpcs base — WAN-only, leave disabled) |
| `/ethphytop` | `brcm,eth-phy-top` | `0x837ff000` | `0x1000` | `xphy0-addr=0x9` `xphy0-enabled` | top-level PHY/serdes mux + the external xphy@9 enable |
| `/egphy` | `brcm,egphy` | `0x837ff00c` | `0x20` | — | internal quad-GPHY (1G) analog/power control |
| `/swblks` | `brcm,swblks` | `0x837ff000`(`bcast-ctrl`); `0x837ff014`(`qphy-ctrl`) | `0x4`;`0x4` | `phy_base=1` | switch glue control regs |
| `/mdiosf2` | `brcm,mdio-sf2` | `0x837ffd00`; `0xff85a024` | `0x10`; `0x4` | — | **the SF2/switch MDIO controller** → mainline `mdio-bcm-unimac` (see PART B §5) |
| `/ephyled` | `brcm,ephy-led` | (12 reg pairs) | — | `led_reg_max=0xb` | PHY LED control (cosmetic, skip for first-light) |
| `/wantype_detect` | `brcm,wantypedetect` | gpon/epon regs | — | WAN-only, skip | — |

Note: several blocks live in the `0x837ffxxx` page (ethphytop/egphy/swblks/serdes/mdiosf2/xport
sub-reg) — that page is the **switch/PHY "misc" control area** at SoC `0x837f0000`+.
The runner/MAC datapath proper is the `0x828xxxxx` range and the `0x82000000` rdpa window.

## Switch node `[FDT]` `/xrdp/switch0`

- compatible `brcm,enet`, `label="bcmsw"`, `sw-type="RUNNER_SW"`. **No reg of its own** —
  it is a logical switch fronting the runner. Children describe the port→PHY wiring.
- This is the SF2-class integrated switch; mainline analogue is `bcm_sf2` / `dsa`, but the
  stock model is "RUNNER_SW" (ports are runner endpoints), see verdict.

## Port → PHY → MDIO → serdes map `[FDT]` (the headline deliverable)

Ports live at `/xrdp/switch0/ports/*`; PHYs at `/xrdp/mdio/*` and `/xrdp/wan_serdes_bus/*`.
`phy-handle` phandles resolved against the `phandle` property of each PHY node.

| Port node | label | reg (port#) | mac-type | mac-index | phy-mode | phy-handle → PHY node | PHY reg (MDIO addr) | phy-type | serdes core/lane | status |
|---|---|---|---|---|---|---|---|---|---|---|
| `port_gphy0` | — | 0 | UNIMAC | — | gmii | `0x10`→`phy_gphy0` | **1** | EGPHY | — | disabled |
| **`port_gphy1`** | **eth2** | **1** | **UNIMAC** | — | **gmii** | `0x11`→`phy_gphy1` | **2** | **EGPHY** | — | **okay** |
| `port_gphy2` | — | 2 | UNIMAC | — | gmii | `0x12`→`phy_gphy2` | 3 | EGPHY | — | disabled |
| `port_gphy3` | — | 3 | UNIMAC | — | gmii | `0x13`→`phy_gphy3` | 4 | EGPHY | — | disabled |
| **`port_xphy`** | **eth0** | 5 | XPORT | 0 | serdes | `0x15`→`phy_xphy` | **9** | EXT3 (cascade) | — | **okay** |
| **`port_sgmii1`** | **eth1** | 6 | XPORT | 2 | serdes | `0x16`→`phy_serdes0` | **7** | 10GAE `phy-fixed` `config-xfi="10GBase-R"` | core0/lane0 | **okay** |
| **`port_sgmii2`** | **eth3** | 7 | XPORT | 4 | serdes | `0x17`→`phy_serdes1` | **8** | 10GAE → `phy-handle 0xf`→`phy_cascade1`@**0x15(21)** | core1/lane0 | **okay** |
| `port_sgmii2_1` | — | 8 | XPORT | 5 | serdes | `0x18`→`phy_serdes1_1` | 0x108 | 10GAE | core1/lane0 | disabled |
| `port_sgmii2_2` | — | 9 | XPORT | 6 | serdes | `0x19`→`phy_serdes1_2` | 0x208 | 10GAE | core1/lane0 | disabled |
| `port_sgmii2_3` | — | 0xa | XPORT | 7 | serdes | `0x1a`→`phy_serdes1_3` | 0x308 | 10GAE | core1/lane0 | disabled |
| `port_wan@ae` | — | 4 | XPORT | 1 | xfi | `0x14`→`wan_ae` (wan_serdes_bus reg1) | — | AE | — | disabled (WAN) |
| `port_wan@fiber` | — | 0xb | — | serdes | `0x1b`→`phy_wan_serdes` (reg0) | — | PON | — | disabled (WAN) |

### Reconciliation with recon (all CONFIRMED):
- **eth0 = WAN10G** external PHY @ MDIO **9** (BCM84891), id `0x359050e1` recon →
  `port_xphy`/`phy_xphy` EXT3 @ reg 9, `ethphytop xphy0-addr=9 xphy0-enabled`. ✅
  (Note: in the stock tree eth0 is the external 10G copper PHY on XPORT mac-index 0, not WAN.)
- **eth2 = internal GPHY 1G** @ MDIO **2**, id `0x359050e0` recon →
  `port_gphy1`/`phy_gphy1` EGPHY @ reg 2, UNIMAC. ✅ **This is our first-light target.**
- **eth3 = multigig** @ MDIO **21**, id `0x35905081` recon →
  `port_sgmii2`/`phy_serdes1` (XPORT serdes core1) cascaded to ext PHY `phy_cascade1`@reg `0x15`=**21**. ✅
- **eth1 = 10GBase-R serdes** → `port_sgmii1`/`phy_serdes0` 10GAE, `phy-fixed`,
  `config-xfi="10GBase-R"`, serdes core0/lane0. ✅

### PHY id ↔ MDIO bus
All PHYs hang off the single `/xrdp/mdio` bus (`bus-type="DSL_ETHSW"`, simple-bus, no own reg —
the actual MDIO transactor is `/mdiosf2`, `brcm,mdio-sf2`). External 10G PHYs (xphy@9,
cascade@21) and the four internal EGPHYs (1–4) and the serdes pseudo-PHYs (7,8,…) all share it.
WAN serdes PHYs are on a separate logical bus `/xrdp/wan_serdes_bus`.

---

# MAC REGISTER DELTAS — bcm4916 vs mainline `bcm4908_enet`

## Findings from `bcm_enet.ko` `[DISASM]`
- 241 functions. Symbol set is `enet_probe`, `of_sw_probe`, `of_port_probe`, `port_mac_phy_init`,
  `set_mac_cfg_by_phy`, `port_runner_port_init`, `runner_*`, `_rdpa_create_queue`,
  `enetxapi_queues_init`, `runner_get_pkt_from_ring`, `runner_ring_create_delete`, etc.
- **There are NO DMA-ring register offset constants** in this driver — no `0xa00`/`0xc00`
  groups, no `ENET_DMA_*` writes. String scan for `unimac/xport/mdio/runner/rdpa` returns only
  `runner_*`/`rdpa_*` symbol names and log strings; no register-name strings.
- `set_mac_cfg_by_phy` (`0x08001ee0`) does all MAC config through an **indirect ops vtable**:
  it loads a `mac_dev` pointer, then `ldr xN,[ops,#0x30]` / `ldr xN,[ops,#0x38]` and
  `blr xN` — i.e. function-pointer calls into the `mac_dev` driver, plus speed comparisons
  (`#0xa`,`#0x64`,`#0x3e8`,`#0x9c4`,`#0x1388`,`#0x2710` = 10/100/1000/2500/5000/10000 Mbps).
  The actual UNIMAC/XLMAC/XPORT MMIO pokes live behind that vtable, in the runner/rdpa GPL
  layer (rdpa_gpl.ko / cmdlist.ko / the closed runner firmware), **not in bcm_enet.ko**.
- Datapath is packet rings via RDPA (`runner_get_pkt_from_ring`, `enetxapi_queues_init`,
  `_rdpa_create_queue`), driven by the 32 runner queue IRQs below — a completely different
  model from a memory-mapped TX/RX DMA descriptor ring engine.

## Runner / RDPA datapath IRQs `[FDT]` (`/rdpa_drv`)
`interrupt-names` = `fpm,queue0..queue31`; `interrupts` (3-cell GIC_SPI, flags=4=LEVEL_HIGH):
- `fpm`   = SPI **0x6b (107)**
- `queue0..queue31` = SPI **0x4b..0x6a (75..106)** contiguous.

## The bcm4916 register block layout the hardware actually exposes `[FDT]`
Since the driver hides offsets, the authoritative offsets come from the FDT `*_offset` props:
- UNIMAC (1G MAC, our eth2 path): base `0x828a8000`, `top_offset=0x400`, `conf_offset=0x1000`,
  `mib_offset=0x400`. Second window `0x828b0000` (+`0x1400`) is the UNIMAC "misc"/MIB block.
- XLMAC (10G): base `0x828b2000` (+`0x680`), `xlmac_offset=0x200`, second window `0x82890000`
  (+`0x6b88`) = XLMAC RDP/BBH region, `bbh_tx_offset=0x2000`.
- XPORT wrapper: `0x837f0000` (+`0x8000`) main, `0x828b2000` (+`0x1000`) overlaps XLMAC,
  `0x837ff1f8` (+4) a status reg.

## VERDICT: can mainline `bcm4908_enet` be extended? **NO — not as-is.**
`bcm4908_enet.c` is a self-contained driver for the BCM4908 **on-chip DMA Ethernet controller**
with compile-time `#define`s for a TX/RX descriptor-ring register file (the `0xa00`/`0xc00`
groups) and `ENET_*` control. **BCM4916 has no such exposed DMA-ring controller in the stock
software model** — the datapath is the Broadcom Runner/RDPA accelerator, programmed through a
proprietary GPL-shim (`bcm_enet.ko`) on top of closed runner firmware (rdpa). The MAC cores
(`brcm,unimac3` / `brcm,xlmac1` / `brcm,xport`) are real and memory-mapped (bases above), but
there is no mainline driver that speaks "runner". Consequences:
- `bcm4908_enet` cannot be pointed at these reg bases and made to work; its descriptor-ring
  abstraction does not exist on this part. Extending it would mean writing a new runner/RDPA
  datapath backend — out of scope for a quick reuse.
- **Realistic mainline path**: treat the four internal GPHY ports as a switch behind a UNIMAC
  CPU/IMP port. The 1G UNIMAC core (`brcm,unimac3`) is close to what mainline `unimac`/`bcm_sf2`
  already understand. The switch is SF2-class but stock exposes it as "RUNNER_SW", so a
  `bcm_sf2`/DSA bring-up will need the SF2 core registers (NOT fully exposed in this FDT —
  only the `0x837ff000` glue page + `mdiosf2`), i.e. **needs deeper RE** (see open questions).
- Net: plan for a **new minimal MAC/switch driver or substantial `bcm_sf2` extension**, not a
  drop-in `bcm4908_enet` reuse. The FDT gives us all the reg bases to start; the datapath
  programming sequence is the missing piece and lives in the closed runner stack.

## §5 — Switch/PHY MDIO access in the stock driver `[FDT]`+`[INFER]`
- The MDIO transactor is `/mdiosf2`, compatible **`brcm,mdio-sf2`**, reg `0x837ffd00` (size `0x10`,
  the command/data window) + `0xff85a024` (size 4, a clock/strap reg in the periph page).
- A 0x10 (4-register) command window with a separate config reg is exactly the
  classic Broadcom UniMAC MDIO master shape → maps to mainline **`mdio-bcm-unimac`**
  (`drivers/net/mdio/mdio-bcm-unimac.c`) with a `brcm,unimac-mdio` compatible. The register
  semantics (CMD/READ/WRITE/poll-busy) should match; the `compatible` differs only in name.
  Confidence: high that mainline `mdio-bcm-unimac` can drive this with the `0x837ffd00` base;
  verify the busy/start bit layout against the unimac-mdio driver before trusting it `[INFER]`.
- All EGPHY/serdes/cascade PHYs enumerate on this one bus (the `/xrdp/mdio` simple-bus is just
  the logical container; addresses are the `reg` values in the table above).

---

# PRIME-DIRECTIVE DTS SKELETON

Minimal nodes to bring up **ONE internal-GPHY port (eth2)** with a **fixed-link CPU/IMP** port
(the agreed first-light shortcut). Real reg/irq values filled in from `[FDT]`; `TODO` where the
mainline driver model needs data the stock FDT does not expose (the runner/SF2 datapath).

```dts
// bcm4916.dtsi (SoC-level)  -- bases all [FDT]-confirmed
/ {
    #address-cells = <2>;
    #size-cells = <2>;
    compatible = "brcm,bcm4916", "brcm,brcm-v8A";

    gic: interrupt-controller@81001000 {
        compatible = "arm,cortex-a15-gic";
        #interrupt-cells = <3>;
        interrupt-controller;
        reg = <0x0 0x81001000 0x0 0x1000>,   // GIC dist
              <0x0 0x81002000 0x0 0x2000>;    // GIC cpu
    };

    // SF2/UniMAC-style MDIO master  -> mainline mdio-bcm-unimac
    mdio_sf2: mdio@837ffd00 {
        compatible = "brcm,unimac-mdio";      // [INFER] stock = "brcm,mdio-sf2"
        reg = <0x0 0x837ffd00 0x0 0x10>;      // [FDT]  (+0xff85a024/4 = clk strap, see note)
        #address-cells = <1>;
        #size-cells = <0>;

        gphy2: ethernet-phy@2 {               // eth2 internal EGPHY [FDT reg=2]
            reg = <2>;
            // EGPHY analog block at /egphy 0x837ff00c (+0x20); EGPHY power via brcm,egphy TODO
        };
    };

    // 1G MAC core feeding the internal GPHYs (eth2 lives here) [FDT /unimac]
    unimac: ethernet@828a8000 {
        compatible = "brcm,unimac3";          // TODO: no mainline binding; bcm4908_enet does NOT fit
        reg = <0x0 0x828a8000 0x0 0x5000>,
              <0x0 0x828b0000 0x0 0x1400>;
        // brcm,conf-offset=0x1000 brcm,mib-offset=0x400 brcm,top-offset=0x400  [FDT]
        // interrupts: none on the MAC; datapath IRQs are runner queues (below) [FDT]
        // TODO: runner/RDPA datapath is NOT a mainline DMA-ring engine -> needs new backend
    };

    // Reference only: the whole runner window + its 32 queue IRQs [FDT /rdpa_drv]
    // reg = <0x0 0x82000000 0x0 0x00caf004>;
    // interrupts: fpm=GIC_SPI 107; queue0..31 = GIC_SPI 75..106 (level-high, flag 4)
};
```

```dts
// gt-be98.dts (board) -- first-light: eth2 (GPHY@2) + fixed-link CPU/IMP
#include "bcm4916.dtsi"
/ {
    model = "ASUS GT-BE98";
    compatible = "asus,gt-be98", "brcm,bcm4916";

    // Logical switch (stock: /xrdp/switch0 brcm,enet RUNNER_SW).
    // For DSA bring-up this becomes a bcm_sf2 node -- BUT SF2 core regs are NOT in the
    // stock FDT (only the 0x837ff000 glue page + mdiosf2). TODO: RE the SF2 core base.
    switch0: switch@TODO {
        compatible = "brcm,bcm4916-switch";   // TODO placeholder (likely bcm_sf2-family)
        // reg = <SF2 core base>;             // TODO: NEEDS DEEPER RE (not in live FDT)
        dsa,member = <0 0>;
        ports {
            #address-cells = <1>;
            #size-cells = <0>;

            port@1 {                          // eth2 -> internal GPHY [FDT port_gphy1 reg=1]
                reg = <1>;
                label = "eth2";
                phy-mode = "gmii";            // [FDT]
                phy-handle = <&gphy2>;
            };

            port@8 {                          // CPU/IMP fixed-link shortcut (port# TODO)
                reg = <8>;                    // TODO: confirm IMP/CPU port index for runner
                ethernet = <&unimac>;
                phy-mode = "internal";        // TODO confirm
                fixed-link {
                    speed = <1000>;
                    full-duplex;
                };
            };
        };
    };
};
```

**Skeleton status**: all GPHY/MDIO/UNIMAC reg+addr values are real `[FDT]`. The two real gaps
blocking compile-to-link are (1) **no mainline driver speaks the runner/RDPA datapath**, so
`unimac`/`switch0` need a real driver decision, and (2) the **SF2 switch core base is not in the
live FDT** (stock hides it behind RUNNER_SW). The fixed-link CPU-port index is a guess.

---

# OPEN QUESTIONS / NEEDS-DEEPER-RE

1. **SF2 switch core register base** — `/xrdp/switch0` (`brcm,enet`, RUNNER_SW) has NO `reg`.
   The only switch-glue regs in the FDT are the `0x837ff000` page (`swblks` bcast/qphy-ctrl,
   `ethphytop`, `egphy`, `serdes`). If we want a `bcm_sf2`/DSA driver we must find the SF2 core
   base (likely inside the `0x82000000` runner window or `0x837fxxxx`). Needs RE of rdpa.ko /
   the runner driver, or a memory dump of the live `/proc/iomem` if obtainable.
2. **Runner/RDPA datapath sequence** — the actual TX/RX path is closed firmware behind the
   `mac_dev` vtable. To do open networking we either (a) write a runner backend (large), or
   (b) find whether the UNIMAC core can be driven standalone (CPU-direct, bypassing runner) —
   this is the make-or-break and requires RE of the `mac_dev` ops in rdpa_gpl.ko/cmdlist.ko.
3. **`mdio-sf2` exact register semantics** — confirm the CMD/busy/data bit layout at
   `0x837ffd00` matches `mdio-bcm-unimac` (start-busy bit, c22/c45) before relying on it.
   Also identify what the second reg `0xff85a024` (4 bytes, periph page) gates (MDIO clock /
   strap). RE: `pdf` the mdio read/write fns in rdpa.ko or the bcm_sf2 mdio path.
4. **EGPHY power-up / `ethphytop` mux sequence** — bringing up GPHY@2 likely needs the
   `brcm,egphy` (`0x837ff00c`) and `brcm,eth-phy-top` (`0x837ff000`, `xphy0-addr=9`) init
   writes. The exact poke sequence is in the vendor phy lib (not bcm_enet.ko). Needs RE if
   the PHY does not come out of reset by default.
5. **CPU/IMP port index & MAC binding for the runner switch** — guessed port@8 + fixed-link.
   Confirm against rdpa port mapping (`port_runner_port_type_mapping` in bcm_enet.ko).
6. **Clocks/resets** — MAC/serdes likely need `clocks`/`resets` phandles; the stock FDT MAC
   nodes don't carry them (clocks are implicit/PMC-driven). Mainline drivers will want explicit
   clock/reset entries — source TBD (`/pmc`, `/clocks`).

---

## Provenance quick-reference
- All `reg`/`interrupts`/`phy-handle`/`reg-names`/`*_offset` values: `/tmp/fdt/<path>` property
  files, decoded `od -An -tx4 --endian=big`. Re-derive with `tar -xf
  /opt/re-bins/live-fdt-device-tree.tar` then walk the tree.
- bcm_enet.ko facts: `r2 bcm_enet.ko` → `aa` → `pdf @ sym.set_mac_cfg_by_phy` (0x08001ee0),
  `afl~...`, `iz~...`. No raw register offsets present in this object (confirmed absence).
