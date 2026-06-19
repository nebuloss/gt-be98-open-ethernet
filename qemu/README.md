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
brcm-sf2 ... eth0: PHY [a900000.mdio--1:09] driver [Generic PHY]
DSA: tree 0 setup
brcm-sf2 ...: Starfighter 2 top: 0.53, core: 0.06, IRQs: 19, 20
```
(`found switch: BCM4916` and `driver [Broadcom BCM4916 EGPHY]` are the new
results vs the earlier stock-kernel boot, which showed `BCM4908` and
`Generic PHY`.)
sysfs confirms phylib read the REAL Broadcom PHY IDs through the modelled MDIO:
```
a900000.mdio--1:02 phy_id=0x359050e0   # eth2 internal EGPHY
a900000.mdio--1:09 phy_id=0x359050e1   # eth0 ext 10G BCM84891
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
  returns the real Broadcom PHY IDs (addr 2/9/21) and BMSR link-up.
- Wiring lives in `hw/arm/virt.c::create_bcm4916()` (a local patch on dev-build):
  maps both devices, emits the matching FDT (switch + ports + mdio + PHYs), and
  adds a **Cadence GEM** (`macb`) as the DSA CPU-port conduit so
  `dsa_register_switch()` completes (DSA mandates a conduit netdev).

### Kernel config deltas needed (built-in, set on dev-build `~/mainline/.config`)
`NET_DSA`, `NET_DSA_BCM_SF2`, `B53`, `NET_DSA_TAG_BRCM`, `MDIO_BCM_UNIMAC`,
`BROADCOM_PHY`, `MACB` all `=y`. To make `NET_DSA=y` stick, `BRIDGE=y`,
`VLAN_8021Q=y`, and `HSR` must be `n` (DSA `depends on HSR || HSR=n` caps it to
`m` while `HSR=m`).

### Honest gaps / TODO
- The internal 1G EGPHY (`0x359050e0`, eth2) now binds the **Broadcom BCM4916
  EGPHY** driver (patch 0003). The external 10G PHY (`0x359050e1`, BCM84891,
  eth0) still attaches as **Generic PHY** — supporting it is a non-trivial C45
  PHY effort, deliberately deferred (see `driver/mainline-patches/README.md`).
  Enumeration succeeds either way, which is the Stage-2 goal.
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
