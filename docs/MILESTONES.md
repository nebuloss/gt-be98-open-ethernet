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

## M1 — Recon + recovery  ✅ done
- [x] Read-only RE of live device: topology mapped → `re-notes/device-recon.md`
- [x] Gather RE inputs: 18 stock `.ko`s + live FDT tar staged to the RE container `/opt/re-bins` (bootloader=mtd1, located not dumped)
- [x] Mainline-source survey (v7.1) → `docs/mainline-survey.md` (reuse map + prime-directive change list)
- [x] Recovery path designed (internal notes, not published) — NOT exercised; needs user/physical action
- [x] Extract BCM4916 reg/IRQ/topology from live FDT + bcm_enet.ko → `re-notes/bcm4916-regmap.md` (+ XRDP pivot)
- [x] Mainline aarch64 cross-toolchain (gcc 14.2) + mainline v7.1 build on dev-build

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

## M2 — DTS + control-plane bring-up (in QEMU)  ✅ mostly done
Scope refined by the XRDP finding: the datapath (Runner) is M5; M2 = the CONTROL plane
(DTS + MDIO/PHY/switch management) proven under QEMU.
- [x] `dts/bcm4916.dtsi` + `dts/gt-be98.dts` — compiles clean (dtc, 5911B); SF2 core base
      pinned 0x837ff000 (SDK 6813_map_part.h)
- [x] QEMU SF2/MDIO device-model (`qemu/device-model/bcm4916_sf2.c` + virt.c patch) — chip-ID +
      UNIMAC-MDIO state machine + fake PHYs returning the real Broadcom IDs
- [x] **mainline b53/bcm_sf2 + phylib complete a full DSA bring-up under QEMU**, reading the real
      PHY IDs (0x359050e0/e1, 0x35905081) via the modeled MDIO — verified boot log (qemu/README)
- [x] Driver patches (`driver/mainline-patches/`): b53 `BCM4916_DEVICE_ID` + bcm_sf2
      `brcm,bcm4916-switch` of_match + broadcom.c EGPHY 0x359050e0 → **our DTS binds as BCM4916**
      under QEMU (`found switch: BCM4916`; eth2 PHY driver = "Broadcom BCM4916 EGPHY"). Verified.
- [ ] eth0 10G PHY (BCM84891, 0x359050e1) — real driver non-trivial, deferred (currently Generic PHY)
- [ ] (real datapath/ping deferred to the Runner work — needs M5; QEMU conduit is a stand-in GEM)

**M2 control plane: DONE under QEMU.** The open switch/MDIO/PHY management stack binds the real
GT-BE98 device tree end-to-end in emulation. Remaining wired work is the datapath (the Runner) = M5.

## M3 — Switch (DSA)
- [ ] b53/bcm_sf2 for the internal switch; LAN ports up + bridged

## M4 — Multi-gig / 10G PHY  ⏳ control-plane done
- [x] 10G PHY driver for eth0 (`driver/mainline-patches/0004`): ID 0x359050e1 = BCM4916 integrated
      **XPHY** ("XPHY4916_X"), not standalone BCM84891. Binds under QEMU as "Broadcom BCM4916 10G XPHY"
      (reads VEND1 0x400d per GPL SDK). eth3's real BCM84891L (0x35905081) already in bcm84881.c.
- [ ] Live 10G link not provable in QEMU (modeled MAC is 1G-class; real 10G needs the Runner/crossbar)
- [ ] 2.5G ports / serdes config (later)

## M5 — Runner/RDPA HW offload  ★ NOW THE GOAL (user re-scope 2026-06-19)
"Full features: 10G with no CPU overload" → the Runner MUST do fast-path forward/NAT/QoS in HW.
- [ ] Bring up the Runner: load microcode (⚠ 4916 microcode is PROPRIETARY/non-redistributable — see
      `re-notes/runner-microcode-and-cpuring.md`; user must extract from own rdpa.ko) + init FPM/SBPM/BBH
- [~] CPU host-ring slow-path — **ABI recovered** (`re-notes/xrdp-datapath-abi.md`): XRDP regmap +
      ring/FPM descriptors pinned (FPM + ring caller are GPL!). Ready to implement RX/TX + NAPI on SPI
      75–107. Remaining RE: host CPU_RX_DESCRIPTOR word0/1/3, CPU-TX doorbell, data_path_init values.
- [ ] Flow-acceleration control plane: flow learning → cmdlist actions → Runner tables (the hard part)
- [ ] 10G line-rate forward/NAT offloaded, CPU idle — the deliverable
- ★ CORRECTED: the 4916 Runner microcode is **PROPRIETARY/non-redistributable** (256KB inside rdpa.ko,
  license=Proprietary, taint P; absent from the 4916 GPL SDK). The Runner is mandatory (no HW bypass),
  so a fully-open AND shippable datapath is impossible today — realistic = open driver + user extracts
  microcode from their OWN rdpa.ko, or drive the closed Runner via mainline netfilter flowtable. The
  offload control plane (rdpa core / cmdlist) is also proprietary → clean-room = the multi-person-year core.

### Re-scope note
The Runner is now central, NOT out-of-scope. All M1–M4 foundation work still applies — it's the
substrate the offload fast-path sits on. The "CPU-forwarded ~1–3 Gbps" framing is downgraded to an
intermediate checkpoint, not the deliverable.

---
## Log
- 2026-06-19: Repo initialized (skeleton, README, this tracker). Connectivity confirmed to
  device (4.19.294), the RE container (dev-reverse running), dev-build (x86_64).
  Inherited WiFi-effort infra/recovery memory. User constraint added: no connectivity-breaking
  live tests for now.
