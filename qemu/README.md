# QEMU host-side iteration loop — BCM4916 / GT-BE98

Safe, device-free dev loop for the open mainline Ethernet driver. **Live device
testing is OFF the table for now** (Ethernet is the management link — see the
repo root README "Safety" section). QEMU is the primary loop.

All builds + QEMU runs happen on **dev-build** (`ssh guillaume@10.0.50.21`),
**never** on dev-code. Artifacts (Image, cpio, qemu binaries) stay on dev-build;
only scripts/docs/skeletons live in this repo.

---

## TL;DR — run the baseline boot loop (on dev-build)

```bash
ssh guillaume@10.0.50.21
# 1. (once) build the kernel — see docs/build-env.md
cd ~/mainline
rtk make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image

# 2. (once) build the busybox initramfs
~/be98/gt-be98-open-ethernet/qemu/scripts/build-initramfs.sh

# 3. boot it under qemu virt (auto-exit + pass/fail in 90s)
TIMEOUT=90 ~/be98/gt-be98-open-ethernet/qemu/scripts/run-virt-baseline.sh
# -> "RESULT: BOOT OK" when the log contains the BE98_BOOT_OK marker
```

Interactive (no timeout): `~/.../qemu/scripts/run-virt-baseline.sh` then
`Ctrl-A X` to quit QEMU.

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
- `device-model/bcm4916_mdio_sf2.c` — Stage 2 SF2-MDIO + fake-GPHY **skeleton**
  (design only; not built / not wired into a machine yet).

On dev-build the working dir is `~/qemu-be98/` (kernel `Image` lives in
`~/mainline/arch/arm64/boot/`, initramfs at `~/qemu-be98/initramfs.cpio.gz`,
last boot log `~/qemu-be98/boot.log`). QEMU source for Stage 2 builds:
`~/qemu-src`.
