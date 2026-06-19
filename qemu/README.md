# QEMU host-side iteration loop — BCM4916 / GT-BE98

Safe, device-free dev loop for the open mainline Ethernet driver. **Live device
testing is OFF the table for now** (Ethernet is the management link — see the
repo root README "Safety" section). QEMU is the primary loop.

All builds + QEMU runs happen on **dev-build** (`ssh <dev-build>`),
**never** on dev-code. Artifacts (Image, cpio, qemu binaries) stay on dev-build;
only scripts/docs/skeletons live in this repo.

---

## TL;DR — run the baseline boot loop (on dev-build)

```bash
ssh <dev-build>
# 1. (once) build a recent mainline aarch64 kernel Image
cd ~/mainline
rtk make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image

# 2. (once) build the busybox initramfs
~/gt-be98-open-ethernet/qemu/scripts/build-initramfs.sh

# 3. boot it under qemu virt (auto-exit + pass/fail in 90s)
TIMEOUT=90 ~/gt-be98-open-ethernet/qemu/scripts/run-virt-baseline.sh
# -> "RESULT: BOOT OK" when the log contains the BE98_BOOT_OK marker
```

Interactive (no timeout): `~/.../qemu/scripts/run-virt-baseline.sh` then
`Ctrl-A X` to quit QEMU.

---

## Stage 2 STATUS: DONE — DSA probe binds as **BCM4916** + EGPHY driver match

A custom `qemu-system-aarch64` (built from `~/qemu-src/qemu-10.0.0` on dev-build)
models the BCM4916 SF2 switch + UNIMAC MDIO control plane well enough that a
mainline kernel **fully probes the switch via `bcm_sf2`/`b53` and phylib
enumerates the PHYs** — no external `-dtb`.

As of the `driver/mainline-patches/` work the harness now emits OUR
`brcm,bcm4916-switch` compatible and the kernel binds it **as BCM4916** (was
BCM4908). The internal 1G EGPHY also matches a real Broadcom PHY driver. This
requires the three mainline patches in `driver/mainline-patches/` applied to the
kernel; an unpatched stock kernel would not bind `brcm,bcm4916-switch`.

Verified boot log (patched mainline v7.1/7.2-era, `QEMU_BCM4916=1`):
```
unimac-mdio a900000.mdio: Broadcom UniMAC MDIO bus
brcm-sf2 a800000.ethernet-switch: found switch: BCM4916, rev 0
macb a700000.ethernet eth1: Cadence GEM ... (DSA CPU-port conduit)
brcm-sf2 ...: Link is Up - 1Gbps/Full
brcm-sf2 ... eth2: PHY [a900000.mdio--1:02] driver [Broadcom BCM4916 EGPHY]
brcm-sf2 ... eth0: PHY [a900000.mdio--1:09] driver [Broadcom BCM4916 10G XPHY]
DSA: tree 0 setup
brcm-sf2 ...: Starfighter 2 top: 0.53, core: 0.06, IRQs: 19, 20
```
(`found switch: BCM4916`, `driver [Broadcom BCM4916 EGPHY]` on eth2, and
`driver [Broadcom BCM4916 10G XPHY]` on eth0 are the new results vs the
earlier stock-kernel boot, which showed `BCM4908` and `Generic PHY`.)
sysfs confirms phylib read the REAL Broadcom PHY IDs through the modelled MDIO:
```
a900000.mdio--1:02 phy_id=0x359050e0   # eth2 internal EGPHY
a900000.mdio--1:09 phy_id=0x359050e1   # eth0 BCM4916 integrated 10G XPHY
a900000.mdio--1:15 phy_id=0x35905081   # eth3 ext multigig (addr 21)
```

### Run command (on dev-build)
```bash
# kernel needs DSA/B53/SF2/UNIMAC-MDIO/MACB built-in (see "kernel config" below)
# AND the driver/mainline-patches/ applied (so brcm,bcm4916-switch binds).
QEMU_BCM4916=1 ~/qemu-src/qemu-10.0.0/build/qemu-system-aarch64 \
  -machine virt -cpu cortex-a53 -smp 4 -m 1024 \
  -kernel ~/mainline/arch/arm64/boot/Image \
  -initrd ~/qemu-be98/initramfs.cpio.gz \
  -append "console=ttyAMA0 rdinit=/init panic=5 loglevel=8 be98_auto=poweroff" \
  -nographic -no-reboot
```
The `QEMU_BCM4916=1` env var gates the device on the `virt` machine (off by
default, so plain `virt` is undisturbed). Note: this custom build has the slirp
`user` netdev disabled — drop `-netdev user,...` (the baseline script's NIC) when
using it, or rebuild QEMU with `--enable-slirp`.

### What is modelled (device source: `device-model/bcm4916_sf2.c`)
- **SF2 switch core** (`TYPE_BCM4916_SF2`, DT `brcm,bcm4916-switch`): the 6
  reg regions (core/reg/intrl2_0/intrl2_1/fcb/acb) per mainline `bcm4908.dtsi`
  (the 4916 SF2 core is register-compatible with 4908). Backs
  `REG_SWITCH_REVISION`/`REG_PHY_REVISION` and `CORE_IMP0_PRT_ID`; all other CORE
  page-register accesses read 0 / accept writes (enough for `b53_switch_init`).
  The chip ID is NOT register-read — `bcm_sf2` hardcodes it from the DT
  compatible (`pdata->chip_id = BCM4916_DEVICE_ID` via the of_match added in
  `driver/mainline-patches/`), so `b53` skips `b53_switch_detect`.
- **UNIMAC MDIO master** (`TYPE_BCM4916_MDIO`, DT `brcm,unimac-mdio`): the
  CMD/CFG register block + C22 state machine + a fake-PHY register file that
  returns the real Broadcom PHY IDs (addr 2/9/21) and BMSR link-up. It also
  models the **MMD-over-C22 indirect** access (regs `MII_MMD_CTRL` 0x0d /
  `MII_MMD_DATA` 0x0e, per `drivers/net/phy/phy-core.c::mmd_phy_indirect`) so the
  Clause-45 `Broadcom BCM4916 10G XPHY` driver (mainline-patches/0004) can read
  the proprietary VEND1 (0x1e) status reg `0x400d` and AN (0x07) `0xfff9`. For
  addr 9 (eth0) these preload to "link up, 10G". NOTE: `mdio-bcm-unimac` has no
  native `read_c45`, so the driver reaches the MMDs via the C22 indirect path —
  which is why the eth0 PHY is discovered as a C22 device (matched by exact C22
  PHY ID) rather than a native C45 device.
- Wiring lives in `hw/arm/virt.c::create_bcm4916()` (a local patch on dev-build):
  maps both devices, emits the matching FDT (switch + ports + mdio + PHYs), and
  adds a **Cadence GEM** (`macb`) as the DSA CPU-port conduit so
  `dsa_register_switch()` completes (DSA mandates a conduit netdev).

### Kernel config deltas needed (built-in, set on dev-build `~/mainline/.config`)
`NET_DSA`, `NET_DSA_BCM_SF2`, `B53`, `NET_DSA_TAG_BRCM`, `MDIO_BCM_UNIMAC`,
`BROADCOM_PHY`, `BCM84881_PHY`, `MACB` all `=y`. To make `NET_DSA=y` stick, `BRIDGE=y`,
`VLAN_8021Q=y`, and `HSR` must be `n` (DSA `depends on HSR || HSR=n` caps it to
`m` while `HSR=m`).

### Honest gaps / TODO
- The internal 1G EGPHY (`0x359050e0`, eth2) binds the **Broadcom BCM4916
  EGPHY** driver (patch 0003); the external 10G PHY (`0x359050e1`, eth0) now
  binds the new **Broadcom BCM4916 10G XPHY** driver (patch 0004) via the
  MMD-over-C22 path above. CAVEAT: the modelled SF2/b53 DSA MAC is 1G-class
  (offers only MII/GMII), so phylink caps eth0 to GMII even though the XPHY
  `read_status` reads 10G from VEND1 0x400d. True 10G operation runs through the
  Runner/crossbar datapath, which the harness does not model — so the harness
  proves *bind + read_status parse*, not a live 10G link.
- No real datapath: the CPU conduit is a stand-in Cadence GEM, not the real
  UNIMAC/XPORT/Runner. No frames traverse the switch (that is Stage 3+).
- The SF2 core base on the real 4916 is still unknown (FDT hides it behind
  RUNNER_SW). We model the mainline `bcm4908-switch` interface, not 4916 silicon
  offsets — fine for driver bring-up, not a silicon-accurate map.
- `intrl2_*` IRQ regions read 0 (ISRs never fire); link is static (BMSR-up).

---

## State of the world (verified 2026-06-19, on dev-build)

- `qemu-system-aarch64` **10.0.8** (Debian). `-machine help`: `virt` (alias of
  `virt-10.0`), `sbsa-ref`, `raspi*` (bcm283x — **unrelated** to BCM4916),
  `xlnx-versal-virt`. **No Broadcom bcmbca / bcm49xx machine exists** in QEMU,
  and none is on the roadmap. We get nothing for BCM4916 for free.
- Cross toolchain: `aarch64-linux-gnu-gcc 14.2.0` (Debian package).
- Mainline kernel tree `~/mainline` (stable.git, v7.1-era, gcc-built `Image`
  41 MB) **boots under `-machine virt -cpu cortex-a53 -smp 4`** with our busybox
  initramfs. Log evidence: `Run /init as init process` ->
  `BE98 QEMU baseline initramfs - kernel up` -> `BE98_BOOT_OK`, 4× A53
  (`0x410fd034`) brought up, virtio `eth0` enumerated. See
  `~/qemu-be98/boot.log` on dev-build.

So: the host-side toolchain+kernel+rootfs loop is **proven working**. What is
NOT yet modeled: any BCM4916 device (UNIMAC / SF2-MDIO / GPHY / XPORT / Runner).

---

## Approach decision: virt baseline first, custom device-model later

Two options were considered:

**(a) `-machine virt` baseline** — boot mainline as-is, no BCM4916 devices. Proves
toolchain + kernel + rootfs + (later) that our driver *module loads / probe runs*
against a DT we inject. Zero QEMU C work. **Done, working.**

**(b) custom BCM4916 device-model** — a QEMU sysbus device-model (C, in a QEMU
fork on dev-build) that maps the real control-plane regs (SF2 MDIO 0x837ffd00,
UNIMAC 0x828a8000, a fake GPHY) so the driver's probe/MDIO/PHY path executes
host-side. Real engineering, but the only way to exercise driver logic without
the device.

**These are not either/or — they stage.** Recommended plan:

### Stage 0 — baseline boot (DONE)
`run-virt-baseline.sh`. Establishes the iteration loop the user asked for. Use it
as a smoke test after every kernel rebuild.

### Stage 1 — load the driver under virt
Build `bcm4908_enet` / `bcm_sf2` / `b53` / `mdio-bcm-unimac` (defconfig already
=m) and our out-of-tree `driver/` as modules, drop them into the initramfs, and
`modprobe` them under virt. Goal: confirm the module loads, symbol-resolves, and
its `probe` is *reached*. It will fail to find hardware (expected) — that already
flushes out Kconfig/build/DT-binding mistakes cheaply. No QEMU C needed.
Inject a partial BCM4916 DT via `-dtb` (a stripped `bcm4916.dtsi` with just the
mdio-sf2 + gphy + one MAC node) to drive `of_*` matching.

### Stage 2 — minimal control-plane device-model (the real work)
Add a sysbus device-model on a custom QEMU build (`~/qemu-src` on dev-build):
1. **SF2 MDIO controller** at `0x837ffd00` modeling the `mdio-bcm-unimac`
   register block (CMD/CFG, start-busy, C22 read/write). Skeleton started:
   `device-model/bcm4916_mdio_sf2.c`.
2. **A fake GPHY** behind it that answers PHYID1/2 + BMSR(link-up) so
   `phy_device` probe + genphy succeed → our driver sees "link up". (Skeleton
   includes a trivial inline fake PHY.)
3. Wrap as a **`bcm4916-virt` machine** (or `-device` add-ons on virt) that maps
   these at the FDT-confirmed SoC bases and provides a matching DT.

Success at Stage 2 = the driver's MDIO read/write + PHY attach path runs against
emulated regs. This is the first point where we're *testing driver logic*, not
just plumbing.

### Stage 3 — minimal UNIMAC MAC datapath
Model just enough of UNIMAC (`0x828a8000`, conf_offset 0x1000, mib 0x400) +
XPORT (`0x837f0000`) for `bcm4908_enet` open/up and a loopback/injected RX so a
single frame can traverse driver TX/RX. Stop short of the Runner.

### Stage 4 (out of scope for v1) — Runner/RDPA datapath
The `0x82000000` rdpa window is a separate multi-person-year RE effort. Not
modeled. Mainline is CPU-forwarded only anyway (see root README caveat).

---

## FDT-confirmed register bases (from `re-notes/bcm4916-regmap.md`)

| Block      | SoC base     | size    | role / Stage |
|------------|--------------|---------|--------------|
| GIC        | `0x81001000` | dist    | (virt provides its own GIC) |
| rdpa       | `0x82000000` | 0xcaf004| Runner datapath — Stage 4, out of scope |
| UNIMAC     | `0x828a8000` | 0x5000  | 1G MAC core — Stage 3 |
| XLMAC      | `0x828b2000` | 0x680   | 10G MAC — later |
| XPORT      | `0x837f0000` | 0x8000  | 10G MAC/PCS wrapper — Stage 3 |
| mdio-sf2   | `0x837ffd00` | 0x10    | **SF2 MDIO controller — Stage 2** |
| egphy      | `0x837ff00c` | 0x20    | internal quad-GPHY power — Stage 2 helper |

SoC = 4× Cortex-A53 (matches `-cpu cortex-a53 -smp 4`).

---

## Files

- `scripts/build-initramfs.sh` — build the busybox aarch64 initramfs (dev-build).
- `scripts/run-virt-baseline.sh` — Stage 0 baseline boot + pass/fail check.
- `device-model/bcm4916_sf2.c` — Stage 2 device model: SF2 switch core
  (`brcm,bcm4916-switch`) + UNIMAC MDIO master (`brcm,unimac-mdio`) with fake
  PHYs. **Built + working** (probes under the custom QEMU; see Stage 2 STATUS).
- `device-model/bcm4916-qemu-virt.patch` — the `hw/arm/virt.c` +
  `hw/arm/Kconfig` + `hw/net/meson.build` changes that wire the device into the
  `virt` machine (gated by `QEMU_BCM4916=1`) and add the Cadence GEM conduit.
  Apply with `patch -p1` inside the qemu-10.0.0 tree, drop `bcm4916_sf2.c` into
  `hw/net/`, rebuild.

On dev-build the working dir is `~/qemu-be98/` (kernel `Image` lives in
`~/mainline/arch/arm64/boot/`, initramfs at `~/qemu-be98/initramfs.cpio.gz`,
last boot log `~/qemu-be98/boot.log`). QEMU source for Stage 2 builds:
`~/qemu-src`.

---

## Stage 3 STATUS: DONE — datapath / **first frame moves MAC↔CPU both ways**

A second QEMU device, `TYPE_BCM4916_RUNNER` (`device-model/bcm4916_runner.c`,
DT `brcm,bcm4916-runner`), models the XRDP "Runner" CPU-conduit datapath to the
contract in `device-model/runner-emulation-contract.md`. Wired into
`create_bcm4916()` it **replaces the Stage 2 Cadence GEM stand-in** as the DSA
CPU-port conduit, and attaches as a QEMU NIC so a `-netdev` backend can
inject/emit real Ethernet frames. The open driver `driver/runner/bcm4916_runner.c`
(built **into the kernel**, `CONFIG_BCM4916_RUNNER=y`, emulated mode via
`brcm,runner-emulated`) drives it unmodified.

**Result: a real Ethernet frame moves MAC↔CPU through the open driver in
emulation, both RX and TX.** (Goals a + b + c achieved.)

### What is modelled (`device-model/bcm4916_runner.c`)
- **rdpa window** at `0x82000000` size `0x00caf004` (one MMIO region; sub-blocks
  decoded by offset). FPM `@0xa00000`, RNR_MEM[0..7] `@0x700000+n*0x20000`,
  PSRAM `@0x0`.
- **FPM pool**: `POOL0_ALLOC_DEALLOC` (read=alloc token / write=free token,
  ABI-3.1 bit layout), `POOL1_CFG1` (chunk size), `POOL1_CFG2` (pool DDR base),
  `FPM_CTL` INIT_MEM (clears instantly) + POOL1_ENABLE, `POOL1_STAT2`
  (tokens-available). `buf_phys = pool_pbase + index*chunk`.
- **Ring discovery**: parses the 16-B big-endian `CPU_RING_DESCRIPTOR` the driver
  `memcpy_toio`'s into PSRAM at `0x0000` (RX) / `0x0080` (TX) to learn ring DDR
  base + depth (256) + entry size (16).
- **RX inject**: backend frame → `dma_memory_write` into the descriptor's DDR
  buffer → fill word0 (len) + word2 (ownership=HOST, set **last**) → advance
  producer → raise the RX IRQ (level; a virtual-clock timer scans the ring and
  deasserts when the guest re-arms, matching the contract's level semantics).
- **TX consume**: the `iowrite16(cpu_to_be16(write_idx))` into `RNR_MEM[n]+0x0`
  is the doorbell → read each new 16-B TX descriptor → resolve word3 FPM token →
  `dma_memory_read` payload → `qemu_send_packet` to the backend → free the token.

### Run command (on dev-build)
The custom QEMU has slirp `user` disabled, so use a **`dgram`** backend (each UDP
datagram payload is one raw Ethernet frame — trivial to inject/sniff). The
`qemu/scripts/dgram_peer.py`-style helper captures guest TX (→ pcap) and injects
guest RX.
```bash
# peer: capture guest TX on :15402, inject guest RX to :15401
PEER_DUR=50 INJECT_DELAY=9 INJECT_N=6 python3 dgram_peer.py 15402 15401 inject tx.pcap &
QEMU_BCM4916=1 ~/qemu-src/qemu-10.0.0/build/qemu-system-aarch64 \
  -machine virt -cpu cortex-a53 -smp 4 -m 1024 \
  -kernel ~/mainline/arch/arm64/boot/Image -initrd <stage3-initramfs> \
  -append "console=ttyAMA0 rdinit=/init panic=5 cma=128M" \
  -netdev dgram,id=rnet,remote.type=inet,remote.host=127.0.0.1,remote.port=15402,\
local.type=inet,local.host=127.0.0.1,local.port=15401 \
  -nic none -nographic -no-reboot
```
Two kernel-cmdline requirements found during bring-up:
- **`cma=128M`** — the driver's 32 MB FPM coherent pool needs CMA larger than the
  default 32 MB area (else `dmam_alloc_coherent` returns `-ENOMEM`).
- **keep `-m 1024`** — guest RAM must end below `0x82000000` or it collides with
  the rdpa MMIO window (`request_region` `-EBUSY`). Do **not** use `-m 2048`.

### Evidence (verified 2026-06-19, `~/qemu-be98/stage3-*` on dev-build)
```
bcm4916-runner 82000000.runner: FPM pool: 32 MB, chunk 512, pbase 0x0000000077d00000
bcm4916-runner: RX ring base=0x43507000 depth=256 esz=16 valid=1
bcm4916-runner: TX ring base=0x43e4d000 depth=256 esz=16 valid=1
bcm4916-runner 82000000.runner: BCM4916 Runner conduit ready (emulated, irq 17)
--- TX ---
bcm4916-runner: TX emit idx=3 len=64 token=0x80003200 buf=0x77d00600   (per frame)
TXP_BEFORE=3  TXP_AFTER=12                       # driver tx_packets advanced
# peer pcap (tcpdump -r), the guest's own txgen frame reached the backend:
02:00:00:00:00:02 > ff:ff:ff:ff:ff:ff, ethertype 0x88b5, length 64: "BE98-TX-TES..."
--- RX ---
bcm4916-runner: RX recv len=71 rx.valid=1 idx=0..3       # model placed frames in ring
RX ISR irq=17 -> napi_schedule ; poll w2=0xc3ee4000 own=1 # driver NAPI saw ownership=HOST
RXP_BEFORE=0  RXP_AFTER=4  RXB_AFTER=284          # driver rx_packets/bytes advanced
```

### Honest gaps / what is stubbed
- **DSA user-port path (goal d) not completed.** The conduit netdev (`rnr0`) is
  proven RX+TX; DSA enumerates `eth0/eth1/eth2` over it and `DSA: tree 0 setup`
  succeeds, but `dsa_register_switch()` cannot raise the conduit MTU to 1504
  (`-EINVAL`: the FPM chunk is 512 B so the conduit `max_mtu` is ~498). The
  Stage-3 proof therefore runs directly on the `rnr0` conduit netdev with small
  frames. To exercise a switch *user* port end-to-end, the conduit needs a
  ≥2 KB FPM chunk (the contract pins 512) or DSA's overhead check relaxed.
- **IRQ naming quirk:** `platform_get_irq_byname("queue0")` resolves to the fpm
  SPI (107) on this DT, not queue0 (75). The model works around it by pulsing
  **both** SPI 75 and 107 on RX; the real RDD interrupt id should be pinned and
  the DT `interrupts`/`interrupt-names` order audited.
- **No fast path / offload** (unchanged from driver scope): slow-path conduit only.
- The placeholder PSRAM ring-cfg offsets (`0x0`/`0x80`) and the RNR_MEM TX-index
  offset (`0x0`) are shared driver↔model constants, **not** the real RDD offsets
  (still to be pinned from the GPL RDD map).

### Files added/changed for Stage 3
- `device-model/bcm4916_runner.c` — the Runner datapath QEMU model (new).
- `device-model/bcm4916-qemu-virt.patch` — `create_bcm4916()` now instantiates
  the runner, wires its queue0/fpm IRQs, emits the `brcm,bcm4916-runner` FDT
  node, points the SF2 CPU port at it (`ethernet=&runner`), drops the GEM, and
  binds the runner NIC to `-netdev id=rnet`; meson.build adds the new source.
- driver built in-tree: `drivers/net/ethernet/broadcom/bcm4916_runner.{c,h}` +
  Kconfig `BCM4916_RUNNER` + Makefile; `CONFIG_BCM4916_RUNNER=y`.
- conduit netdev renamed `rnr%d` (was `eth%d`) so it never collides with DSA
  user-port labels.
