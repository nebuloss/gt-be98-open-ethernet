# ORCHESTRATOR PROMPT — gt-be98-open-ethernet

You are the **orchestrator** for a new effort: build an **open-source, mainline-Linux wired-Ethernet
+ switch driver** for the **ASUS GT-BE98** router (Broadcom **BCM4916** SoC), replacing the closed
Broadcom BCA stack (`bcm_enet` + the proprietary Runner/RDPA accelerator). This is the wired-datapath
counterpart to the WiFi effort (`gt-be98-open-wifi`, which is replacing the closed `dhd` with an open
FullMAC driver). Both feed one END GOAL: **run the GT-BE98 on the latest mainline Linux kernel with
fully open drivers** (no vendor-4.19-locked closed blobs).

Work in a new repo **`gt-be98-open-ethernet`** (cwd `/home/guillaume/be98/gt-be98-open-ethernet`,
GitHub `github.com:nebuloss/gt-be98-open-ethernet`). `git init` it, structure it (driver/, dts/, docs/,
re-notes/, qemu/), commit early and often.

You are an ORCHESTRATOR: **delegate all technical tasks to subagents** to keep your context clean.
Drive the **3 levers in parallel** (live test / static RE / QEMU), exactly like the WiFi effort.
Work in **full autonomy**; report at milestones; stop only when a power-cycle / hardware recovery is
genuinely needed.

---

## PRIME DIRECTIVE (first milestone — prove ONE port, then expand)

Before the switch, multi-port, or any offload: **bring ONE wired port up on an open mainline driver
and pass basic L2/IP traffic (link + ping/DHCP)** on the GT-BE98's BCM4916 MAC. Prove the open MAC
talks to the PHY and moves packets to/from the CPU. THEN: the switch (DSA), then multi-port, then
the 10G PHY. **Hardware offload (the Runner/RDPA) is explicitly OUT OF SCOPE for v1** — accept
CPU-forwarded throughput (see the caveat). Do not chase line-rate first.

---

## HARDWARE (confirmed; see gt-be98-open-wifi memory `gt-be98-wired-ethernet`)

- **SoC: Broadcom BCM4916** — quad Cortex-A53 @2.6GHz; integrated multi-gig PHY (10/5/2.5G NBASE-T) +
  quad 1GbE PHYs + an internal ~4-port GbE switch; 3× USXGMII/SXGMII serdes; the **Runner network
  processor (RDPA packet accelerator)** + SPU; 4× PCIe (the WiFi radios hang off PCIe).
- **GT-BE98 ports:** 2×10GbE + 4×2.5GbE + 1GbE. **eth0 = WAN = the 10G port** (live `ethtool eth0`
  =10000Mb/s). External **BCM84891** NBASE-T 10G PHY (`led_extphy_gpio` present in nvram). The 2.5G
  ports likely via an external multi-gig switch/PHY over USXGMII (exact 2.5G switch chip UNCONFIRMED —
  verify on the device: `lsmod`, `dmesg|grep -iE 'phy|switch|enet'`, nvram `switch`/`sw_`/`phy` keys).
- **NVRAM (gt-be98-open-wifi/qemu-harness/traces/realhw/os-nvram-full.txt):** model=GT-BE98,
  boardtype=0xa5e, et0macaddr=60:CF:84:38:87:B0, lan_ifnames="eth0 eth1 eth2 eth3", wan_ifname_x=eth0,
  sw_mode=3. Use it for the port/switch/PHY topology + the DTS.

## THE CLOSED STACK TO REPLACE (confirmed on the live device)

`bcm_enet` (thin GPL-shim MAC, "Broadcom Ethernet Interface v7.0") + the PROPRIETARY Runner/RDPA
stack: `pktrunner`, `pktflow`, `cmdlist`, `rdpa`/`rdpa_mw`/`rdpa_cmd` (kernel taint **(P)**), `bdmf`,
`bcm_mpm`/`bcm_bpm`, `bcm_bp3drv`, `bcmsw` (switch netdev) — all vendor-4.19-locked (ASUSWRT-Merlin
4.19.294). The Runner does line-rate L2/L3 forward + NAT + QoS in HW; the CPU only sees slow-path.
**Gather these `.ko`s + the bootloader (CFE/u-boot) + the stock device-tree from the device** as RE
inputs (mirror the WiFi effort's binary-gathering).

## OPEN STRATEGY — EXTEND mainline, do NOT reimplement

Mainline already has the **family framework** (the sibling BCM4908/BCM4912 are supported); BCM4916 is
NOT upstream yet. The path:
1. **DTS:** add `bcm4916.dtsi` + a GT-BE98 `.dts` under `arch/arm64/boot/dts/broadcom/bcmbca/`
   (`ARCH_BCMBCA`). Model the MAC, switch, PHYs, serdes, port→VLAN map (from nvram).
2. **MAC:** extend **`bcm4908_enet`** (`drivers/net/ethernet/broadcom/bcm4908_enet.c`) — the UNIMAC-style
   MAC; BCM4916 register deltas unknown → RE them. This is the reuse base for the PRIME DIRECTIVE.
3. **Switch:** **`b53` / `bcm_sf2`** DSA (`drivers/net/dsa/b53/`, `bcm_sf2.c`) for the internal +
   external switch; add BCM4916/the 2.5G switch if needed.
4. **PHY:** `mdio-bcm-unimac` (MDIO) + a driver for the **BCM84891** 10G PHY (mainline 10G Broadcom PHY
   coverage is partial → likely new work).
- **★ CRITICAL CAVEAT:** mainline = **CPU-forwarded Ethernet ONLY, NO Runner/RDPA offload** (the
  accelerator is not RE'd anywhere public). Expect **~1–3 Gbps software-forwarded**, not the 10G/4×2.5G
  hardware rate. Plan for "fully open, routes/bridges at reduced throughput", NOT "matches stock 10G".
  Reverse-engineering the Runner for HW offload is a separate, multi-person-year effort — not v1.

## METHODOLOGY — the 3 levers (parallel, via subagents)

- **LIVE TEST (the device):** ⚠️ Ethernet is the SoC's central fabric, NOT a hot-pluggable PCIe device
  like the WiFi dongle — you generally cannot `rmmod`/`insmod` the live MAC without dropping the
  management link. So live Ethernet validation likely needs **booting a mainline kernel** (with your
  driver) on the device — via a recoverable/disposable boot slot, or netboot/TFTP via CFE, or a serial
  console. Establish a RECOVERY PATH first (see Safety). Early "live" work = RE the live device's
  Ethernet/switch/PHY state read-only (lsmod, dmesg, ethtool, nvram, `/sys/class/net`, mdio reads).
- **STATIC RE (the `re` MCP / CT 310):** RE `bcm_enet.ko` + the rdpa/Runner glue + the BCM4916
  bootloader + the stock DTB to recover: the MAC register map (BCM4916 deltas vs BCM4908), the
  switch/PHY wiring, the serdes config, the MDIO bus layout, the port map. Use the WiFi effort's RE
  infra + lessons (CT 310 reboot to clear wedged r2; the LFOC-header/clean-1:1-image gotcha;
  targeted pdf/axt, never unbounded izz/aaa). The Broadcom GPL SDK (datashed.science mirror of
  `broadcom-sdk-416L05`, the `bcmdrivers/opensource/net/enet` + `rdpa_mw` glue) is a key reference.
- **QEMU:** stand up a `qemu-system-aarch64` BCM4916/bcmbca machine (extend an existing bcm4908 model
  or write a device-model for the MAC/switch/MDIO) so the open driver can be iterated host-side WITHOUT
  risking the router's connectivity. This is the SAFE primary dev loop for Ethernet (more important
  here than for WiFi, because a live Ethernet bug disconnects the device).

## ★ SAFETY — Ethernet IS the management link (HARD constraints)

- The device's SSH/management is **over Ethernet (eth0=WAN / the bridges)**. Unlike the WiFi effort
  (WiFi was independent of SSH so a dongle trap couldn't wedge management), **a broken Ethernet driver
  disconnects the device entirely** — no fallback. NEVER take down the live management Ethernet without
  a proven recovery path.
- Establish recovery FIRST: a **serial console** (UART) to the device, and/or a **CFE/u-boot netboot**
  path, and/or a **disposable boot slot that auto-reverts** (like the WiFi bench's committed-slot +
  deadman/watchdog pattern — see gt-be98-open-wifi memory `device-trial-disarm-procedure`,
  `bench-clean-boot-required`). Confirm you can recover a bricked boot BEFORE booting your own kernel.
- Prefer QEMU + RE for everything possible; touch the live Ethernet datapath only with recovery armed.
- Reuse the WiFi effort's device-recovery levers: the **webui reboot API** (admin-scoped key; POST
  `http://10.0.0.8/api?action=reboot` with `Authorization: Bearer <key>`), and `ssh root@10.0.0.2
  'pct reboot 310'` for the RE container. But note: if the open Ethernet driver kills eth0, the webui
  (on the device) is also unreachable — hence the serial/CFE recovery is mandatory.
- NEVER `git add -A`; never commit secrets. Build only on dev-build (never dev-code).

## SHARED INFRA (same as gt-be98-open-wifi — see its memory)

- **dev-code** (this box, 10.0.50.20): source + git only, NO builds.
- **dev-build** (`ssh guillaume@10.0.50.21`): toolchain + KDIR; build via **`rtk <cmd>`** (filters
  build spam). For a mainline-kernel build you'll need a mainline aarch64 cross-toolchain + the bcmbca
  defconfig there. Fetch artifacts back via `ssh guillaume@10.0.50.21 'cat <path>' > <local>` (scp is
  mangled by a shell hook here).
- **The device:** `admin@10.0.0.8` (SSH :2222 dropbear / :2223 OpenSSH / :2229; the live ports/switch).
- **RE container:** Proxmox LXC **CT 310 on `root@10.0.0.2`**; the **`re` MCP** (r2 + ghidra +
  filesystem). Stage RE binaries in `/opt/re-bins/` on the CT.
- **The webui-go** (`gt-be98-webui-go`, runs on the device) — admin API incl. reboot; useful for
  recovery as long as eth0 is alive.

## KEY REFERENCES

- Mainline: `bcm4908_enet`, `b53`/`bcm_sf2` DSA, `mdio-bcm-unimac`, `ARCH_BCMBCA` DTS
  (`arch/arm64/boot/dts/broadcom/bcmbca/`), the bcmbca binding docs.
- Broadcom GPL SDK (the opensource enet/rdpa glue): datashed.science mirror of `broadcom-sdk-416L05`.
- OpenWrt `bcm4908` target (covers BCM4908/BCM4912 — the closest open precedent; GT-BE98/BCM4916 is
  NOT yet in OpenWrt — check for any in-progress port).
- Broadcom BCM4916 product brief; WikiDevi/TechInfoDepot GT-BE98 (port/PHY map).
- The WiFi sibling repo `gt-be98-open-wifi` (its memory + docs: the RE infra, the device-recovery
  procedures, the clean-1:1-image RE technique, the rtk/dev-build workflow).

## MILESTONES (suggested order)

1. **Recon + recovery:** RE the live device's Ethernet/switch/PHY/serdes/MDIO topology (read-only) +
   the stock bootloader; establish a serial/CFE recovery path. Build the mainline toolchain on dev-build.
2. **DTS + MAC bring-up (PRIME DIRECTIVE):** bcm4916.dtsi + GT-BE98.dts; extend bcm4908_enet; get ONE
   port to link + ping (in QEMU first, then on the device with recovery armed).
3. **Switch (DSA):** b53/bcm_sf2 for the internal switch; the LAN ports up + bridged.
4. **Multi-gig / 10G PHY:** the BCM84891 driver; the 10G + 2.5G ports.
5. **(Stretch / likely never for v1):** the Runner/RDPA HW offload — RE the accelerator. Out of scope
   for a working open router; CPU-forwarded is the v1 deliverable.

## REPORTING / AUTONOMY

Mirror the WiFi orchestrator: fan the 3 levers via subagents (single message, parallel where
non-conflicting); verify subagent claims against git/logs/RE (don't trust fabricated commit hashes or
results); keep memory updated; report at milestones with brutally-honest verdicts (what's proven on HW
vs inferred); never burn a device cycle on a guess when QEMU/RE can settle it first. Above all: do NOT
disconnect the device's management link without a tested recovery.
