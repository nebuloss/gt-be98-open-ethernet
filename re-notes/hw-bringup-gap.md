# BCM4916 (BCM6813) mainline SoC bring-up gap — honest assessment

What it actually takes to get a mainline arm64 kernel to **first serial output** and
**first init** on the real ASUS GT-BE98 (BCM4916 / "bcm6813"), and what is still missing.
Companion to `runner-microcode-and-cpuring.md` (datapath) and `bcm4916-regmap.md` (regmap).

All builds were done on the build host (mainline v7.1/7.2-era, aarch64 GCC 14.2); **no device
was touched** for this pass. Confidence tags: `[MAIN]` read from the mainline tree,
`[BUILT]` produced/verified by an actual cross-build, `[INFER]` deduced.

---

## TL;DR — is first-serial-output reachable on mainline today?

**Yes, very likely, and the gap is much smaller than feared.** Mainline already ships a
**working BCM6813 SoC device tree** (`arch/arm64/boot/dts/broadcom/bcmbca/bcm6813.dtsi`) and a
reference board (`bcm96813.dts`), plus `CONFIG_ARCH_BCMBCA`. The BCM4916 *is* the BCM6813 die.
The SoC core bring-up — CPU enable (PSCI), GIC-400, ARMv8 arch timer, the PL011 console, fixed
clocks — is **already modelled and uses only generic, already-upstream drivers**. There is **no
custom clock driver, no pinctrl driver, and no reset/syscon driver required to reach a console
and run init**, because the mainline bcmbca model deliberately leans on the bootloader (U-Boot)
having already configured clocks/pinmux/DDR, and on PSCI (in ARM Trusted Firmware) for CPU and
power. `[MAIN]`

So the risk for **Steps 0–1 of the live test (reach serial + init)** is **LOW**. The hard,
unsolved problems are all *downstream* of "it booted": the Ethernet **datapath** (Runner
microcode is proprietary; XRDP regmap must be RE'd) and, to a lesser degree, the MAC-core
bring-up. Do not conflate "kernel boots on the SoC" (close) with "open Ethernet moves packets"
(far). This document is only about the former.

---

## What we built (device-targeted, not QEMU-virt) `[BUILT]`

Configured from arm64 `defconfig` + a device fragment, cross-compiled clean:

- **`Image`** — 42,088,960 bytes, "Linux kernel ARM64 boot executable Image, little-endian,
  4K pages". Built in ~56 s. `[BUILT]`
- **`gt-be98.dtb`** — 6,024 bytes, DTB v17, compiled from our `dts/gt-be98.dts` +
  `dts/bcm4916.dtsi` with the kernel cpp + dtc, clean (no warnings). `[BUILT]`
- Driver placement: the open Runner driver is **built into vmlinux** (`runner_probe`,
  initcall `runner_driver_init` present in `vmlinux` symbols); the PHYs and MDIO master are
  **built-in** (`bcm84881`, `bcm54xx_config_init`, `unimac_mdio_probe`). The DSA stack
  (`dsa_core`, `tag_brcm`, `bcm-sf2`, `b53_common/mdio/mmap`) and the netfilter flowtable
  (`nf_flow_table`, `nf_flow_table_inet`) build as **modules** for the initramfs. `[BUILT]`

### Config symbols that matter (all verified set) `[BUILT]`

| Symbol | State | Note |
|---|---|---|
| `CONFIG_ARCH_BCMBCA` | y | the bcmbca platform |
| `CONFIG_SERIAL_AMBA_PL011[_CONSOLE]` | y | PL011 = ttyAMA0 console |
| `CONFIG_MDIO_BCM_UNIMAC` | y | the MDIO master |
| `CONFIG_BROADCOM_PHY` | y | internal EGPHY (our patch 0003) |
| `CONFIG_BCM84881_PHY` | y | 10G XPHY (our patch 0004) |
| `CONFIG_BCM4916_RUNNER` | y | our open datapath driver (built-in) |
| `CONFIG_NET_DSA`, `_BCM_SF2`, `B53`, `TAG_BRCM` | m | DSA stack (our patches 0001/0002) |
| `CONFIG_NF_TABLES` | y | needed by the flowtable |
| `CONFIG_NF_FLOW_TABLE[_INET]` | m | offload step |
| `CONFIG_NF_CONNTRACK`, `BRIDGE`, `VLAN_8021Q` | y | |

Note: `NET_DSA` resolves to `=m` on arm64 even when requested `=y` (a tristate-selecting
dependency keeps it modular); this is fine for a netboot that carries an initramfs with the
modules. If a fully built-in DSA is wanted later, the selecting symbols must be chased down.

---

## The mainline platform pieces — present vs. missing

### Already present and sufficient for first boot `[MAIN]`

| Need | Mainline status | Where |
|---|---|---|
| Platform Kconfig | `CONFIG_ARCH_BCMBCA=y` | `arch/arm64/Kconfig.platforms` |
| SoC DTSI | **exists** for 6813 | `.../bcmbca/bcm6813.dtsi` |
| Reference board DTS | **exists** | `.../bcmbca/bcm96813.dts` |
| CPU bring-up | `enable-method = "psci"`, `arm,psci-0.2`, method `smc` | bcm6813.dtsi |
| Interrupt controller | `arm,gic-400` @0x81001000/0x81002000 | bcm6813.dtsi (generic GIC driver) |
| Arch timer | `arm,armv8-timer` (PPIs 13/14/11/10) | bcm6813.dtsi (generic) |
| Console UART | `arm,pl011`,`arm,primecell` @0xff812000, SPI 32 | bcm6813.dtsi (generic AMBA PL011) |
| Clocks | `fixed-clock` (periph 200 MHz) + `fixed-factor-clock` (uart /4) | bcm6813.dtsi |
| SPI / NAND | `brcm,bcm6813-hsspi`, `brcm,nand-bcm63138` | bcm6813.dtsi (upstream drivers) |
| MAINTAINERS / binding | `brcm,bcmbca.yaml` covers `bcmbca/*` | MAINTAINERS |

**Critical implication:** the entire path to a login/init prompt — CPU online, timer ticking,
GIC delivering, PL011 printing — needs **only generic ARM/PrimeCell drivers that are already in
mainline.** There is nothing BCM6813-specific to write to get a serial console. `[MAIN][INFER]`

### NOT present in mainline — but NOT required for first boot

| Piece | Mainline status | Required to reach console+init? |
|---|---|---|
| **bcmbca clock driver** | absent (`drivers/clk/bcm/` has only `clk-bcm63xx*` for MIPS-era) | **No** — bcm6813.dtsi uses `fixed-clock`; real PLLs are left as the bootloader set them. Peripherals that need exact rates (HS-SPI/NAND timing) may be approximate but the console works. |
| **bcmbca pinctrl driver** | absent (`pinctrl-bcm4908.c` exists but is **not** referenced by bcm6813.dtsi; no 6813 pinctrl) | **No** — no DT node references a pinctrl; U-Boot leaves pinmux configured. Pinmux only matters if a peripheral needs re-muxing from Linux. |
| **reset / syscon** | not modelled for 6813 | **No** — not referenced by the SoC dtsi. |
| **DEBUG_LL / earlycon** | `earlycon` works generically via `stdout-path`; no `CONFIG_DEBUG_*` board entry needed | optional — can pass `earlycon` on cmdline for *earlier* output; not required for normal console. |

### Our DTS: compatibles with **no mainline driver** (won't bind — by design or as a gap)

Audited `dts/bcm4916.dtsi` + `dts/gt-be98.dts`. The following nodes carry placeholder
compatibles with **no matching mainline driver**. None of these block first boot; they are the
**Ethernet datapath gap**, listed honestly:

| Node | compatible | Mainline driver? | Consequence |
|---|---|---|---|
| `unimac` (1G MAC) | `brcm,unimac3` | **none** | MAC core unmanaged by Linux (the Runner owns it on stock). Control-plane only via DSA; MAC bring-up is a real gap. |
| `xlmac` | `brcm,xlmac1` | **none** | same |
| `xport` (10G MAC/PCS) | `brcm,xport` | **none** | same; 10G ports need this |
| `mpcs` | `brcm,mpcs` | **none** | 10G PCS unmanaged |
| `serdes` | `brcm,serdes1` | **none** | serdes lanes unconfigured from Linux |
| `ethphytop` | `brcm,eth-phy-top` | **none** | PHY-mux/xphy-enable not released by Linux |
| `egphy` | `brcm,egphy` | **none** | internal GPHY analog/power not enabled by Linux |
| `runner` | `brcm,bcm4916-runner` | **ours (built-in)** | binds our driver; needs proprietary microcode on real HW (emulated flag for QEMU) |
| `switch0` | `brcm,bcm4916-switch` | **ours (patched into bcm_sf2)** | binds via patches 0001/0002 |
| `mdio` | `brcm,unimac-mdio` | **yes** (`mdio-bcm-unimac`) | binds |

So of the control-plane nodes, **the switch, MDIO and PHYs will bind; the MAC cores, PCS,
serdes and the PHY-mux/EGPHY-enable glue will NOT** (no mainline driver). On stock hardware the
bootloader/Runner already enabled the EGPHY and serdes, so a first link on a port that the
bootloader left up may still come up via DSA+PHY without our own MAC driver — but this is
**unproven on hardware** and is the first real uncertainty after "it booted".

---

## Honest "what it takes to get first serial output + first init"

1. **Reach SPL/U-Boot** — already on the device (U-Boot 2019.07). No work. *(device-side)*
2. **Load `Image` + `gt-be98.dtb` to RAM and `booti`/`bootm`** — see the runbook. No new kernel
   code needed. The DTB's `chosen/stdout-path = "serial0:115200n8"` + `bootargs
   console=ttyAMA0,115200` drives PL011. `[BUILT][INFER]`
3. **CPU + GIC + timer + console** — all generic mainline drivers, already configured. Expect
   the normal arm64 boot banner on ttyAMA0. **No missing platform code.** `[MAIN][INFER]`
4. **Mount initramfs and run init** — `CONFIG_BLK_DEV_INITRD=y`, `DEVTMPFS` on; a busybox
   initramfs runs. **No missing platform code.** `[BUILT]`

**Residual first-boot risks (LOW but nonzero):**
- **PL011 base / DDR layout assumption.** Our dtsi mirrors mainline bcm6813.dtsi for the UART
  (`serial@12000` under `bus@ff800000` → 0xff812000) and a 2 GiB `memory@0`. If U-Boot's actual
  DDR map differs, the bootloader-passed memory node should override ours; worst case, add
  `earlycon=pl011,0xff812000` to see output before DT parse. `[INFER]`
- **No earlycon by default** — if the kernel dies *before* the PL011 console comes up, you see
  nothing. Mitigation: boot with `earlycon` (generic, uses stdout-path) for the first attempt.
- **PSCI dependency** — CPU bring-up assumes ATF/PSCI in the stock bootloader. The GT-BE98 stock
  boot uses PSCI (`arm,psci-0.2`); a RAM-booted mainline kernel reuses the resident firmware, so
  this should hold, but is **unverified on this unit**. If PSCI is absent/incompatible, only CPU0
  comes up (still enough for a console). `[INFER]`

**Bottom line:** getting **first serial output and init is a low-risk, mostly-already-done step**
— the mainline bcmbca model was built exactly for this class of chip and the 6813 die is already
in-tree. The project's hard problems live entirely *after* first boot (MAC cores have no mainline
driver; the datapath needs proprietary Runner microcode). Do not let "the kernel boots" be read
as "open Ethernet works" — those are far apart.

---

## SOURCES
- mainline tree (build host, v7.1/7.2-era): `arch/arm64/boot/dts/broadcom/bcmbca/bcm6813.dtsi`,
  `bcm96813.dts`; `arch/arm64/Kconfig.platforms` (`ARCH_BCMBCA`); `drivers/clk/bcm/`,
  `drivers/pinctrl/bcm/` (no 6813 entries); MAINTAINERS (`brcm,bcmbca.yaml`). `[MAIN]`
- Our cross-build (build host): `Image` 42 MB clean; `gt-be98.dtb` 6,024 B clean; runner built
  into vmlinux; DSA + flowtable as modules. `[BUILT]`
- DTS audit: `dts/bcm4916.dtsi`, `dts/gt-be98.dts` (placeholder compatibles for MAC/PCS/serdes/
  phy-mux have no mainline driver). `[MAIN]`
- Cross-refs: `re-notes/runner-microcode-and-cpuring.md` (datapath/microcode blocker),
  `re-notes/bcm4916-regmap.md` (reg bases + IRQ SPIs).
