# GT-BE98 Wired-Ethernet / Switch / PHY / Serdes / MDIO — Live Device Recon

**Device:** ASUS GT-BE98 (`model=GT-BE98`, `productid=GT-BE98`, `build_name=GT-BE98`, `territory_code=EU/01`)
**SoC:** Broadcom BCM4916 (BCA / XRDP-Runner architecture; DT compatible `brcm,brcm-v8A`, "Broadcom-v8A")
**Firmware:** ASUSWRT-Merlin `firmver=3.0.0.6` / `buildno=102.6`
**Kernel:** Linux 4.19.294 #5 SMP PREEMPT aarch64, built by `<user>@dev-build`, gcc 10.3.0 (Buildroot 2021.02.4)
**Bootloader:** U-Boot 2019.07 (06/05/2026), loader build tag `50404p2@450432` (Sep 07 2023)
**Access used:** `ssh -p 2222 <device>` (dropbear, busybox userland; `id` absent)
**Recon date:** 2026-06-19. ALL commands READ-ONLY. No module load/unload, no ip/ifconfig/ethtool-set, no devmem, no nvram writes, no dd of mtd.

---

## TOPOLOGY SUMMARY (ports → driver → PHY → switch → serdes → MDIO)

The wired datapath is the Broadcom **BCA / XRDP "Runner"** offload stack. All four wired ports are presented as Linux netdevs `eth0..eth3` by **`bcm_enet`** (driver string "Broadcom Ethernet Interface", version 7.0, internal naming `bcm63xx_enet` family). They hang off the on-SoC **SF2 / "swblks" switch** with an embedded quad-GPHY (`egphy`) plus serdes lanes (`serdes1`/`mpcs`) feeding external multi-gig PHYs. There is **no Linux `phylib` / standard `mdio_bus`** — only a `Fixed MDIO bus.0` is registered in sysfs; the real MDIO/PHY access is proprietary inside `bcm_enet` (via `mdiosf2` controller).

| Netdev | Role (nvram) | MDIO addr (PHYAD) | PHY ID reg2/reg3 | Media | Max speed | Link now | PHY / attachment (confidence) |
|--------|--------------|-------------------|------------------|-------|-----------|----------|-------------------------------|
| **eth0** | **WAN** (`wan_ifname_x=eth0`) | **9** | `0x3590` / `0x50e1` | Copper, autoneg | 10G (10/5/2.5/1G/100M) | **UP @ 10GFD** | External 10GBASE-T copper PHY = **BCM84891** at MDIO 9 — matches DT `ethphytop/xphy0-addr=0x09`. (PHYID HW-confirmed; "BCM84891" inferred from ID family + 10G copper role + known GT-BE98 BOM) |
| **eth1** | LAN/wired (`wired_ifnames` incl.) | 7 | `0x0000` / `0x0000` (no C22 PHY) | **Serdes**, AN off | 10G (10/5/2.5/1G/100M/10M) | DOWN | **Serdes/USXGMII 10G port** — `media-type` reports "Serdes ... PHY Configured Speed Mode: 10GBase-R". No clause-22 PHY ID (serdes-attached, likely the SFP+/2nd-10G lane). (HW-confirmed serdes; exact endpoint inferred) |
| **eth2** | LAN | 2 | `0x3590` / `0x50e0` | Copper, autoneg | **1G only** (1G/100M/10M) | DOWN | Internal **EGPHY** (embedded quad-GbE, DT `egphy` / `brcm,egphy`). 1G-only copper LAN port. (HW-confirmed) |
| **eth3** | LAN/wired | 21 (0x15) | `0x3590` / `0x5081` | **Serdes + copper PHY**, autoneg | 10G (10/5/2.5/1G/100M) | DOWN | Serdes-fed multi-gig copper PHY at MDIO 21 — `media-type` shows "Serdes Auto Speed Detection: Enabled, ... 10G:5G:2.5G:1G". A 2.5G/multi-gig external PHY. (PHYID HW-confirmed; exact chip UNCONFIRMED — see open questions) |

Notes from nvram corroborating the map:
- `lanports=1 1 1 1 2 3`, `lan_ifnames=eth0 eth1 eth2 eth3 ...`
- `wired_ifnames=eth1 eth2 eth3`; `wan_ifname_x=eth0`; `eth_ifnames=eth0 vlan4094`; `autowan_ifnames=eth0 vlan4094`
- `autowan_brport_no_0=9`, `autowan_brport_no_1=8` (brport 9 = the external/WAN PHY)
- `led_extphy_gpio=255`, `led_10g_white_gpio=4143` (dedicated external-10G-PHY LED → confirms a discrete external 10G PHY)
- All four netdevs share MAC `60:cf:84:xx:xx:xx` (`et0macaddr=60:cf:84:xx:xx:xx`); switching done in HW (SF2), so per-port MAC is shared.

**MDIO bus map (inferred from DT + PHYADs):** one SF2-side MDIO bus (`mdiosf2`, `brcm,mdio-sf2`, base `0x837ffd00`). PHY addresses observed: 2 (egphy/eth2), 7 (serdes/eth1, no C22 ID), 9 (ext 10G/eth0), 21 (multi-gig/eth3). `swblks/phy_base=0x01` (SF2 internal PHY base = addr 1).

**Confidence legend:** PHYADs, PHY ID register values, link/speed, driver names, DT node names/regs, module list, MAC, nvram = **HW-confirmed**. Specific silicon part numbers (BCM84891 for the 10G, exact 2.5G PHY) = **inferred** (PHY ID family + role); not directly stamped in any read-only string found.

---

## 1. Loaded modules (`cat /proc/modules`)

Full Ethernet/switch/Runner/RDPA stack present:

```
firmware_class 16384 0 - Live
bcm_thermal 16384 0 - Live
bcmspu 212992 0 - Live
bcmflex 28672 1 bcmspu, Live
hs_uart_drv 20480 0 - Live
wfd 45056 0 - Live
bcm_pcie_hcd 90112 0 - Live
bcmmcast 122880 1 wfd, Live
pktrunner 73728 0 - Live (P)
rdpa_cmd 40960 0 - Live
pcap 20480 0 - Live
bcm_enet 163840 1 bcm_pcie_hcd, Live        <-- wired Ethernet/switch driver
sw_gso 20480 0 - Live (P)
gdx 327680 1 - Live (P)
cmdlist 102400 1 pktrunner, Live (P)
pktflow 413696 1 pktrunner, Live (P)
bcm_ingqos 229376 1 rdpa_cmd, Live
bcm_bp3drv 135168 0 - Live (P)
rdpa_mw 45056 2 pktrunner,rdpa_cmd, Live
rdpa_usr 77824 0 - Live
rdpa 4808704 0 - Live (P)                    <-- Runner/RDPA core
rdpa_gpl_ext 16384 1 rdpa_cmd, Live
bcmvlan 110592 0 - Live (P)
bcm_bpm 49152 0 [permanent], Live (P)
bcm_mpm 147456 2 rdpa,bcm_bpm, Live (P)
rdpa_gpl 32768 10 ...,bcm_enet,..., Live
bdmf 1290240 10 ...,bcm_enet,..., Live        <-- Broadcom Device Mgmt Framework
bcmlibs 36864 5 wfd,pktrunner,bcm_enet,pktflow,rdpa, Live
bcm_knvram 24576 2 bcm_pcie_hcd,rdpa_mw, Live
```

`bcm_enet` depends on `bcm_pcie_hcd` and links `rdpa_gpl`, `bdmf`, `bcmlibs`. No separate phy/serdes/mdio .ko — PHY/serdes/MDIO logic is statically built into `bcm_enet` (and rdpa impl libs).

---

## 2. dmesg

**Limitation:** the kernel ring buffer had wrapped — boot-time enet/switch/serdes/PHY init lines were already overwritten by WiFi/PCIe (`bca_pcie_ipc` / `wl0` BCM6726) console spam. Grepping `enet|switch|phy|mdio|serdes|...` returned only WiFi PCIe-IPC console output (e.g. `wl0: Broadcom BCM6726 802.11 Wireless Controller`), **no wired enet/PHY ID/serdes-init lines remained**. Topology below was therefore reconstructed from persistent sources (sysfs, device-tree, ethtool/ethctl, nvram) rather than dmesg. Boot logs would need a fresh boot capture over serial (`console=ttyAMA0,115200`) — not possible read-only post-boot.

---

## 3. Interfaces

`ls /sys/class/net`: `eth0 eth1 eth2 eth3 bcmsw` plus vlans `ethN.{20,30,50,70}`, bridges `br0/br20/br30/br50/br70`, and the usual virtual/tunnels (`dummy0 gre0 ifb0/1 imq0-2 lo sit0 ...`), `blogtcp_local`, `spu_ds_dummy`, `spu_us_dummy`.

sysfs per port:

```
eth0: address=60:cf:84:xx:xx:xx operstate=up   carrier=1 speed=10000   (WAN, 10G, LINKED)
eth1: address=60:cf:84:xx:xx:xx operstate=down carrier=0 speed=0
eth2: address=60:cf:84:xx:xx:xx operstate=down carrier=0 speed=0
eth3: address=60:cf:84:xx:xx:xx operstate=down carrier=0 speed=0
bcmsw: address=60:cf:84:xx:xx:xx operstate=down (switch master device)
```

`ethtool -i ethX` (all four identical): `driver: Broadcom Ethernet Interface`, `version: 7.0`, `firmware-version: N/A`, `bus-info:` (empty). No standard register-dump/eeprom support.

`ethtool ethX` link modes / PHYAD (key lines):
```
eth0: 100M..10000baseT, Speed 10000Mb/s, Link yes, PHYAD 9,  Transceiver internal, autoneg on
eth1: 10..10000baseT,   Speed Unknown,   Link no,  PHYAD 7,  Transceiver internal, autoneg off
eth2: 10..1000baseT,    Speed Unknown,   Link no,  PHYAD 2,  Transceiver internal, autoneg on
eth3: 100..10000baseT,  Speed Unknown,   Link no,  PHYAD 21, Transceiver internal, autoneg on
```

`ethctl ethX media-type` (read):
```
eth0: PHY Auto-Negotiation Enabled; Speed Caps 10G:5G:2.5G:1G:100M:100MHD; Link Up @ 10GFD
eth1: Serdes (AN off); Detecting 10GFD; Caps 10G:5G:2.5G:1G:100M:10M; PHY Configured 10GBase-R; Link Down
eth2: PHY Auto-Negotiation Enabled; Caps 1G:1GHD:100M:100MHD:10M:10MHD; Link Down   (1G-only EGPHY)
eth3: Serdes Auto Speed Detection Enabled; Caps 10G:5G:2.5G:1G:100M; Link Down
```

PHY ID registers (`ethctl ethX reg 2` / `reg 3`, clause-22 reads):
```
eth0 (addr 0x09): reg2=0x3590 reg3=0x50e1
eth1 (addr 0x07): reg2=0x0000 reg3=0x0000   (reg1 status = 0x6149; no C22 PHY -> serdes/USXGMII)
eth2 (addr 0x02): reg2=0x3590 reg3=0x50e0
eth3 (addr 0x15): reg2=0x3590 reg3=0x5081
```
`reg2=0x3590` is the common Broadcom upper-OUI; the lower nibble of reg3 distinguishes model/rev (e1 / e0 / 81). eth1 returns all-zero on C22 → its identity is in clause-45 MMD / serdes (not read in this pass to stay safe).

---

## 4. MDIO / PHY topology

- `/sys/bus/mdio_bus/devices/`: **empty**.
- `/sys/class/mdio_bus/`: only `fixed-0 -> ../../devices/platform/Fixed MDIO bus.0/mdio_bus/fixed-0`.
- `/sys/class/net/*/phydev`: **none** (proprietary driver does not register phylib devices).

So MDIO/PHY is entirely inside `bcm_enet`. Device-tree gives the controller + PHY layout:

Top-level DT nodes (`/proc/device-tree`, compatible `brcm,brcm-v8A`):
`egphy ephyled ethphytop mdiosf2 mpcs serdes wan_serdes unimac xlmac xport xrdp swblks ...`

Key node `compatible` strings + `reg` (hexdump via busybox):
```
egphy       compatible="brcm,egphy"        reg base 0x837ff00c size 0x20      (embedded quad GbE PHY)
ethphytop   compatible="brcm,eth-phy-top"  reg base 0x837ff000 size 0x1000
            status="okay"  xphy0-addr=0x00000009  xphy0-enabled (present)     (external 10G PHY @ MDIO 9)
mdiosf2     compatible="brcm,mdio-sf2"      reg base 0x837ffd00 ... + 0xff85a024  (SF2 switch MDIO ctrl)
serdes      compatible="brcm,serdes1"       reg base 0x837ff500 size 0x300
wan_serdes  compatible="brcm,pon-drv"       status="disabled"  reg base 0x837ff500  (PON unused)
mpcs        compatible="brcm,mpcs"          reg base 0x828c4000 size 0x100      (multi-port copper serdes / PCS)
unimac      compatible="brcm,unimac3"       reg base 0x828a8000 (+0x828b0000)    (UniMAC v3)
xlmac       compatible="brcm,xlmac1"        reg base 0x828b2000 (+0x82890000)    (10G XLMAC)
xport       compatible="brcm,xport"         reg base 0x837f0000 / 0x828b2010 / 0x837ff1f8  (10G XPORT)
swblks      compatible="brcm,swblks"        reg base 0x837ff000 / 0x837ff014
            reg-names="bcast-ctrl","qphy-ctrl"   phy_base=0x01                  (SF2 switch block, internal PHY base addr 1)
ephyled     compatible="brcm,ephy-led"      (per-port LED control)
```

Interpretation: SF2 switch (`swblks`) with embedded quad GPHY (`egphy`, internal PHY base 1) + `serdes1`/`mpcs`/`xlmac`/`xport` for the multi-gig/10G lanes; `unimac3` MAC for GbE ports. External 10G copper PHY declared by `ethphytop` at MDIO addr 9. `wan_serdes`(PON) disabled (this is a copper-WAN unit, not fiber).

**2.5G switch/PHY chip — UNCONFIRMED:** No discrete "2.5G switch chip" is evidenced read-only. The architecture is a single on-SoC SF2 switch; the 2.5G/multi-gig ports (eth1 serdes, eth3 PHYAD 21 ID 0x3590/0x5081) are driven via the SoC serdes + external multi-gig copper PHY(s). The exact external multi-gig PHY part (e.g. BCM5499x-class) could not be read out via clause-22 (only the OUI family + model nibble are confirmed). See open questions.

---

## 5. nvram (wired-net relevant keys)

```
et0macaddr=60:cf:84:xx:xx:xx
lan_ifname=br0
lan_ifnames=eth0 eth1 eth2 eth3 wl0 wl1 wl2 wl3 wl0.1 wl1.1 wl2.1 wl3.1 wl3.4
lanports=1 1 1 1 2 3
wired_ifnames=eth1 eth2 eth3
eth_ifnames=eth0 vlan4094
wan_ifname_x=eth0          wan_ifname=(empty)   wanports=0   wans_dualwan=wan none   wans_lanport=1
autowan_ifnames=eth0 vlan4094     autowan_brport_no_0=9   autowan_brport_no_1=8
amas_lldp_ifnames=eth0 eth3 vlan4094
led_extphy_gpio=255        led_10g_white_gpio=4143
sw_mode=3   vlan_enable=0   switch_wantag=none   switch_stb_x=0
wan0_phytype= / wan1_phytype= / wan_phytype= (all empty)
```
No `et1*`/`sw_*`/`serdes`/`xfi`/`usxgmii`/`mdio` topology keys present in nvram (driven from DT + driver, not nvram).

---

## 6. mtd / boot-slot / bootloader

`cat /proc/mtd`:
```
mtd0:  10000000  brcmnand.0   (whole 256MB NAND)
mtd1:  00200000  loader       (2 MB  -> U-Boot/CFE loader; bootloader lives here)
mtd2:  0fd00000  image        (UBI volume "image")
mtd3:  00000500  metadata1    (boot-slot metadata A)
mtd4:  00000500  metadata2    (boot-slot metadata B)
mtd5:  00cb2898  bootfs1
mtd6:  021e8000  rootfs1
mtd7:  00cb2898  bootfs2
mtd8:  021e8000  rootfs2
mtd9:  0141a000  data
mtd10: 0081d000  defaults
mtd11: 03203000  jffs2
```
Dual-image A/B layout (bootfs1/rootfs1 + bootfs2/rootfs2, metadata1/2 = slot selectors).

`chosen/bootargs`:
```
coherent_pool=4M cpuidle_sysfs_switch pci=pcie_bus_safe console=ttyAMA0,115200 rootwait
rng_core.default_quality=1024 mtdparts=brcmnand.0:2097152(loader),265289728@2097152(image)
root=/dev/ubiblock0_6 ubi.mtd=image ubi.block=0,6 rootfstype=squashfs cma=168M
```
Current root = ubiblock0_6 (UBI volume 6 inside mtd "image"). Bootloader = U-Boot 2019.07 in mtd1 "loader" (NOT dumped — small but left untouched per safety). DT `loader_info`: build_date "Sep 07 2023 - 15:41:48", build_tag "50404p2@450432", `loader_img_idx`/`tpl_min_compat` present.

**No `.dtb` file exists on the running rootfs** (`find / -name '*.dtb'` empty) — the FDT is supplied by U-Boot from the boot image. The live FDT was captured instead (see staged files).

---

## 7. RE-input binaries staged to the RE container (`/opt/re-bins/`)

All copied **device → (stream through dev-code, never landed on disk) → the RE container**. bcm_enet/rdpa/pktrunner sha256 cross-checked identical on device and the RE container (integrity verified). On-device source: `/lib/modules/4.19.294/extra/`. Live FDT: `tar c` of `/proc/device-tree`.

| File | bytes | sha256 |
|------|-------|--------|
| **bcm_enet.ko** | 269120 | `a18a5499d938e5c035c2e22482987a283445bbdeb13dcff65864b88e070fccba` |
| rdpa.ko | 7186008 | `ec2614918055e09085f47869fbd52f431d5bdfec81dc5c017a109876fc11fa16` |
| pktrunner.ko | 135992 | `3403c8c85a0eb54544903ef4467e326ab572713a3faf92df68e8b8443f19b821` |
| pktflow.ko | 525208 | `4a1aef36117adea3c202427fce9470e97d6f349e7ef334c4c00d702b17f74ad1` |
| cmdlist.ko | 222160 | `fc413b1428b0f475b6ff685f1f639fcfdc9256008f4f501db26a24412f3e3aff` |
| bdmf.ko | 412824 | `023aca2ac7deb959fcdf0a353f999974b8521a2d44374fc70207b6df306cf6c4` |
| rdpa_cmd.ko | 68744 | `5f4105716a1ec62835c6195d2edd27ce6da779d79052dfe63a31664d37b22fc7` |
| rdpa_mw.ko | 67608 | `39d2f67ab25f9237397ddb6025d996d2f809d35e0131d7fd405f3ac704667c15` |
| rdpa_usr.ko | 149376 | `46d64dadbba235e4f785398f3e75bde3dea24d34559b19e7359d561400901c5f` |
| rdpa_gpl.ko | 77344 | `2758dee6e3789e868180b10cc0ee104ece514e01d04f1e2b063fb076e115751e` |
| rdpa_gpl_ext.ko | 5696 | `9cf20e2f817e449a19ebf1be95b59121ac7927c74a30725283d3b34aeba67c6d` |
| bcm_mpm.ko | 126864 | `3f925cb884c6059a3ec8668dd757c40e834d5e67a3f4a4c5e4b19ee181290d95` |
| bcm_bpm.ko | 93848 | `4210544576e5a9b262de67f6678d0dc7086493e5bd33f0fed8ba86311f20fd2f` |
| bcm_bp3drv.ko | 212176 | `1b0358022a743009ca09a7613e898de31c1f57440c85ae5babb6e0c95e0c1d18` |
| bcmlibs.ko | 64808 | `b63afa61242e9c29938c05591ae50de9a9049e45fa7ac4ae2d739d985a6fbfa1` |
| wfd.ko | 65208 | `20d55f2e67ea830aee866f37084f5316da61d55e9ef0d8763fd67ed4d8464b1e` |
| sw_gso.ko | 23664 | `7fba345b7959ab34f7a0d39cafb572d1b8ecd58dc2f1cf8d2ee040e2ac8aeb52` |
| bcm_knvram.ko | 23704 | `302cda26fa6be36b5052b21e0a95c34db299300bdb74299eb8234f10763a28ea` |
| **live-fdt-device-tree.tar** (tar of `/proc/device-tree`) | 2999296 | `de1f164ea9b55e772cf89c48ecbc271fe12a7bcaab98024ccdaacc1e2f8807f1` |

(`wl-impl105` already pre-existing in /opt/re-bins, not from this pass.)

`bcm_enet.ko` vermagic = `4.19.294 SMP preempt mod_unload aarch64`; internal symbols use `bcm63xx_enet`/`bcm63xx_ethtool_*` naming; contains `crossbar_*`, `USXGMII`, `PHY_TYPE_158CLASS_SERDES`, `serdes_inter_phy_string` symbols.

---

## 8. Could NOT determine read-only (open questions)

1. **Boot-time enet/switch/serdes/PHY init log** — ring buffer wrapped (WiFi PCIe console flooded it). Need a serial-console capture at boot (`ttyAMA0,115200`).
2. **Exact external 10G PHY part number** — PHY ID `0x3590/0x50e1` @ MDIO 9 is the Broadcom 10G copper family; "BCM84891" is inferred (role + GT-BE98 BOM), not stamped in any read-only string. Confirm via clause-45 MMD device-ID read (safe read, not done this pass) or by RE of the impl phy lib.
3. **Exact eth3 multi-gig PHY (the "2.5G" PHY)** — ID `0x3590/0x5081` @ MDIO 21 confirmed; specific silicon UNCONFIRMED. No discrete external 2.5G *switch* chip found — the multi-gig ports are SoC-serdes + external copper PHY(s) behind the on-SoC SF2.
4. **eth1 PHY/serdes endpoint identity** — clause-22 reads all-zero (serdes/USXGMII 10GBase-R); identity lives in clause-45 / serdes regs, not probed to stay safe.
5. **Full port→switch-port index map** — `ethctl bcmsw phy-crossbar` returned empty (no crossbar on this SoC); precise SF2 unit/port numbering for each ethN is in the driver/DT impl (RE `bcm_enet` + the staged FDT). nvram `lanports=1 1 1 1 2 3` and brport 8/9 hints are the only read-only clues.
6. **CFE/U-Boot loader contents** — located in mtd1 "loader" (2 MB) and `loader_info` DT; intentionally NOT dd'd this pass.
