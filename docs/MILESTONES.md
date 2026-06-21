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

## M4 — Multi-gig / 10G PHY + XPORT/serdes  ✅ 10G links in emulation
- [x] 10G PHY driver for eth0 (`driver/mainline-patches/0004`): ID 0x359050e1 = BCM4916 integrated
      **XPHY** ("XPHY4916_X"). eth3's real BCM84891L (0x35905081) already in bcm84881.c.
- [x] **XPORT/XLMAC/MPCS/serdes drivers** (`driver/pcs/pcs-bcm-xport.*` + patches 0005/0006): phylink PCS
      over serdes+MPCS + XLMAC 10G MAC enable + bcm_sf2 mac_select_pcs + real 10G crossbar. **eth1 (XPORT)
      links at 10Gbps/Full in QEMU** (`re-notes/xport-serdes-bringup.md`).
- [ ] serdes PMD needs ~31KB non-redistributable Merlin microcode (stubbed; QEMU fakes lock) — extract
      from device like the Runner ucode
- [ ] USXGMII per-mode + eth0/eth3 (ext-PHY) paths only to PCS-select stage; lane-mux RE'd not written

## M5 — Runner/RDPA HW offload  ★ THE GOAL — ALL PHASES PROVEN IN EMULATION
"Full features: 10G with no CPU overload" → the Runner MUST do fast-path forward/NAT/QoS in HW.
- [ ] Bring up the Runner: load microcode (⚠ 4916 microcode is PROPRIETARY/non-redistributable — see
      `re-notes/runner-microcode-and-cpuring.md`; user must extract from own rdpa.ko) + init FPM/SBPM/BBH
- [x] CPU host-ring slow-path — **DONE in emulation**: open driver `driver/runner/bcm4916_runner.ko`
      (FPM pool + CPU RX NAPI + CPU TX index-doorbell) + QEMU Runner model → **a real Ethernet frame
      moves MAC↔CPU both directions, packet-proven** (tcpdump). ABI fully recovered
      (`re-notes/xrdp-datapath-abi.md`). Gaps: DSA user-port needs ≥2KB FPM chunk (512B caps MTU);
      IRQ-name quirk; PSRAM/RNR-MEM offsets still shared placeholders (pin from device).
- [x] Flow-acceleration control plane — **ABI RE'd + live-confirmed** (`re-notes/xrdp-offload-abi.md`,
      `live-flow-dump.md`).
- [x] **Offload Phase 1 (L2+VLAN) — WORKS in emulation**: open cmdlist builder + context + NAT-C writer
      + nf_flow_table hook; QEMU proves first-pkt→CPU→program→subsequent pkts HW-forwarded, CPU bypassed
      (`re-notes/offload-phase1-status.md`).
- [x] **Offload Phase 2 (L3+NAT/NAPT) — WORKS in emulation**: TTL-dec + IP/port rewrite + csum cmdlist;
      QEMU proves a NAT'd flow HW-forwarded with correct SNAT+csum, CPU bypassed, pcap-verified
      (`re-notes/offload-phase2-status.md`).
- Remaining for HW: exact 6813 context byte-offsets + NAT-C key/hash + ADD-opcode operand packing are
  driver↔model PLACEHOLDERS (pin from device/xpe_api.o RE); live conntrack/MASQUERADE trigger needs a
  2-port topo; IPv6 + next-hop MAC rewrite + CNPL stats TODO.
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
- 2026-06-21: **On-silicon ABI validation (WiFi-style graft, exclusive window).** Mainline kernel
  CANNOT boot on this unit (RSA secure-boot enforced + no kexec + stock 4.19) → validate the HW ABI
  via the stock driver's bdmf `bs` shell (safe oracle) + a 4.19 graft probe (`tools/stock-watch/
  xrdp_peek.c`). Proven: HW-watchdog auto-revert recovers a host-wide wedge in ~74 s (serial-free).
  CAUGHT+FIXED real silicon mismatches: (1) RX/TX descriptor layout was from the wrong XRDP gen
  (416L05) — corrected to 6813, proven vs live `bs Vrpd` samples; (2) XPE cmd_list emitter byte-
  encoding was a wrong uniform packing — rewrote each emitter byte-exact from xpe_api.armb53_6813.o,
  live-confirmed. Offload key (incl. vtag_num + multi-flow tcp_ack/tos keying), FC_UCAST context
  fields, cmd_list encoding, and VLAN action (vtag_num in key, tx_adjust +4/tag) all silicon-
  confirmed via `bs /Bdmf/Examine ucast`. Driver + QEMU model synced to the corrected encoding;
  **QEMU offload regression PASSES end-to-end** (slow-path + L2/VLAN + L3/NAT, CPU bypassed) with the
  corrected code. Commits 2298c77/eb931da/875a57e/246e4ad. Scope refined: device is an AP → NAT
  deferred; deliverable = 10G L2/VLAN HW forward, CPU idle. REMAINING for on-silicon 10G-accel proof:
  capture a real `is_l2_accel=1` forward flow (CPU-bypassed) — needs a 2nd active port (only eth0 is
  cabled) or WiFi up. Note: kernel builds the driver IN-TREE (CONFIG_BCM4916_RUNNER=y); keep
  drivers/net/ethernet/broadcom + drivers/net/pcs copies synced with driver/runner + driver/pcs.
