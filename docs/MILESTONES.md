# Milestones & status — gt-be98-open-ethernet

Brutally-honest tracker. Mark each fact as **[HW]** proven on hardware, **[QEMU]** proven in
emulation, **[RE]** inferred from reverse-engineering, or **[REF]** from mainline/docs.

## Operating constraints (HARD)
- **No live tests that break connectivity** (user, 2026-06-19). Device work is **read-only**:
  `lsmod`, `dmesg`, `ethtool`, `nvram`, `/sys/class/net`, mdio reads. **No** rmmod/insmod of the
  live MAC, **no** flashing, **no** booting our own kernel on the device — until a tested
  serial/CFE recovery path exists AND the user clears it.
- Builds only on dev-build via `rtk`. Never build on dev-code.
- Never `git add -A`; never commit secrets/firmware blobs.

## M1 — Recon + recovery  ⏳ in progress
- [x] Read-only RE of live device: topology mapped → `re-notes/device-recon.md`
- [x] Gather RE inputs: 18 stock `.ko`s + live FDT tar staged to CT 310 `/opt/re-bins` (bootloader=mtd1, located not dumped)
- [x] Mainline-source survey (v7.1) → `docs/mainline-survey.md` (reuse map + prime-directive change list)
- [x] Recovery path designed → `docs/recovery-plan.md` (NOT exercised; needs user/physical action)
- [ ] **Extract BCM4916 reg/IRQ/topology from live FDT + bcm_enet.ko** (RE) — unblocks the DTS  ⏳ NEXT
- [ ] Mainline aarch64 cross-toolchain + bcmbca defconfig on dev-build

### M1 key findings
- **Datapath:** `bcm_enet` v7.0 presents eth0..eth3 off an **on-SoC SF2 switch** w/ embedded quad-GPHY +
  serdes lanes (serdes1/mpcs/xlmac/xport). No standard phylib/mdio_bus — proprietary `mdiosf2`.
  eth0=WAN 10G (ext PHY @mdio9, ID 0x359050e1), eth2=internal GPHY 1G (mdio2, 0x359050e0),
  eth3=serdes multigig (mdio21, 0x35905081), eth1=10GBase-R serdes. **No discrete 2.5G switch chip.**
- **Mainline reuse:** extend `bcm4908_enet` (MAC) + `bcm_sf2`/`b53` (switch) + `mdio-bcm-unimac`.
  **BCM84891 10G PHY already in mainline `bcm84881.c`** (ID 0x35905080 — matches eth3/eth0 family). The
  10G **serdes/port-7 crossbar is stubbed even for BCM4908** (`bcm_sf2.c:524 if(0) FIXME`) → the one
  genuinely-new switch chunk. BCM4916 reg/IRQ bases are **not derivable from mainline** → must come from
  RE of the live FDT + bcm_enet.ko + vendor SDK.
- **Prime-directive shortcut:** bring up an **internal-GPHY port (eth2) with a fixed-link IMP/CPU port**
  to dodge MDIO/serdes for first light.
- **Recovery (must be GREEN before any live test):** U-Boot 2019.07 (not CFE), `bootdelay=5`, TFTP env
  present → netboot viable; serial `ttyAMA0,115200` (UART header pinout undocumented → find on board);
  dual A/B slots, running slot2. **⚠ slot1 commit flag = 0 (UNCOMMITTED)** — no known-good fallback yet.

## M2 — DTS + MAC bring-up (PRIME DIRECTIVE)
- [ ] bcm4916.dtsi + gt-be98.dts
- [ ] bcm4908_enet extended for BCM4916 MAC deltas
- [ ] ONE port: link + ping  (QEMU first, then device with recovery armed + user go-ahead)

## M3 — Switch (DSA)
- [ ] b53/bcm_sf2 for the internal switch; LAN ports up + bridged

## M4 — Multi-gig / 10G PHY
- [ ] BCM84891 10G PHY driver; 10G + 2.5G ports

## M5 — (stretch, likely never for v1) Runner/RDPA HW offload
- Out of scope. CPU-forwarded is the v1 deliverable.

---
## Log
- 2026-06-19: Repo initialized (skeleton, README, this tracker). Connectivity confirmed to
  device (4.19.294), Proxmox host (CT 310 dev-reverse running), dev-build (x86_64).
  Inherited WiFi-effort infra/recovery memory. User constraint added: no connectivity-breaking
  live tests for now.
