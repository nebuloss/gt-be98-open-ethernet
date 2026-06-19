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
- [ ] Read-only RE of live device: enet/switch/PHY/serdes/MDIO topology, loaded `.ko`s, port map
- [ ] Gather RE inputs (bcm_enet.ko, rdpa/Runner .ko's, bootloader, stock DTB) → CT 310 /opt/re-bins
- [ ] Mainline-source survey: bcm4908_enet, b53/bcm_sf2, mdio-bcm-unimac, bcmbca DTS deltas
- [ ] Recovery path designed (serial/CFE/disposable slot) — NOT yet exercised
- [ ] Mainline aarch64 cross-toolchain + bcmbca defconfig on dev-build

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
