# Build environment — dev-build (10.0.50.21)

The toolchain + mainline-kernel setup for the open BCM4916 Ethernet driver.
**Builds happen ONLY on dev-build, never on dev-code (10.0.50.20).** Develop +
git on dev-code → push → pull + build on dev-build via `rtk`.

Verified 2026-06-19.

## Machine

- **dev-build** = `ssh guillaume@10.0.50.21` (key already authorized, no Claude
  Code on it). Debian 13 (trixie), x86_64, 12 cores, ~83 GB free on `/`.
- `rtk` at `/usr/local/bin/rtk` — wrap kernel builds in `rtk` so buildroot/kbuild
  spam is filtered: `rtk make ...`.

## Toolchain

- **aarch64-linux-gnu-gcc 14.2.0** (Debian package `gcc-aarch64-linux-gnu`,
  version `4:14.2.0-1`). Installed via:
  ```bash
  sudo apt-get install -y gcc-aarch64-linux-gnu   # sudo is NOPASSWD on dev-build
  ```
  Provides `aarch64-linux-gnu-{gcc,ld,objdump,...}` on PATH.
- For building arm64 user binaries / extracting arm64 .debs, the foreign arch is
  enabled: `sudo dpkg --add-architecture arm64 && sudo apt-get update`.
- Build prereqs present: `make`, `git`, `bc`, `flex`, `bison`, `cpio`, `gzip`.
  (`/usr/bin/time` is NOT installed — use shell `time` / `date` for timing.)

## Mainline kernel tree

- Path: **`~/mainline`** (on dev-build).
- Remote: `origin = https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git`
  (branch `master`). Pulled 2026-06-18.
- Version: **`make kernelversion` = 7.1.0**, HEAD `9ecfb2f72`
  (`NAME = Baby Opossum Posse`). To refresh: `cd ~/mainline && git pull`.
- BCM4916-relevant content present and confirmed:
  - `CONFIG_ARCH_BCMBCA` exists (`arch/arm64/Kconfig.platforms`), `=y` in arm64
    defconfig.
  - DTS dir `arch/arm64/boot/dts/broadcom/bcmbca/` exists. Closest relatives to
    our SoC: `bcm4908.dtsi`, `bcm4912.dtsi`, and notably
    `bcm4912-asus-gt-ax6000.dts` (a sibling ASUS BCM4912 router — good DTS
    reference). **No `bcm4916*` DTS in mainline — that is ours to author** under
    `dts/`.
  - Drivers present: `drivers/net/ethernet/broadcom/bcm4908_enet.c`,
    `drivers/net/dsa/bcm_sf2.c`, `drivers/net/dsa/b53/b53_common.c`,
    `drivers/net/mdio/mdio-bcm-unimac.c`.
  - In arm64 defconfig these build as modules: `CONFIG_BCM4908_ENET=m`,
    `CONFIG_NET_DSA_BCM_SF2=m`, `CONFIG_B53=m` (+`B53_SRAB_DRIVER=m`),
    `CONFIG_MDIO_BCM_UNIMAC=m`.

## Building

Always set ARCH + CROSS_COMPILE; wrap in `rtk`.

```bash
ssh guillaume@10.0.50.21
cd ~/mainline
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

# config (once)
rtk make defconfig

# kernel Image  (~167 s wall on 12 cores from clean)
rtk make -j$(nproc) Image
# -> arch/arm64/boot/Image  (~41 MB, "Linux kernel ARM64 boot executable")

# all DTBs (incl. bcmbca) if needed
rtk make -j$(nproc) dtbs

# in-tree modules (for the driver smoke loop)
rtk make -j$(nproc) modules
```

### Smoke-build result (recorded)
`make defconfig` + `make -j12 Image` succeeded clean; **Image = 41 MB**, built
in **~167 s**. The Image **boots under QEMU virt** (see `qemu/README.md`). No
blockers.

### Building an out-of-tree module (our `driver/`)
Standard kbuild external module against the configured tree:
```bash
cd ~/be98/gt-be98-open-ethernet/driver     # after git pull on dev-build
rtk make -C ~/mainline M=$PWD ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
```
(Requires `rtk make modules_prepare` in `~/mainline` first if not already done.)

## Fetching artifacts back to dev-code

Do **not** copy build outputs into this box's working tree / git. For inspection,
stream a single artifact over SSH (text or piped binary), e.g.:

```bash
# from dev-code:
ssh guillaume@10.0.50.21 'cat ~/mainline/.config'                       # text
ssh guillaume@10.0.50.21 'cat ~/qemu-be98/boot.log'                     # boot log
ssh guillaume@10.0.50.21 'file ~/mainline/arch/arm64/boot/Image'        # metadata
ssh guillaume@10.0.50.21 'aarch64-linux-gnu-objdump -d ~/.../foo.ko'    # disasm
```
Keep the Image / `.o` / `.ko` / qemu binaries on dev-build only.

## QEMU (for the host-side loop — details in `qemu/README.md`)

- `qemu-system-aarch64` **10.0.8** (Debian). Baseline machine: `-machine virt
  -cpu cortex-a53 -smp 4` (matches BCM4916 = 4× A53). No Broadcom bcmbca machine
  exists in QEMU; a custom device-model is Stage 2 (`qemu/device-model/`).
- QEMU source tree for custom builds: `~/qemu-src` (10.0 era) on dev-build.
- Working dir for the loop: `~/qemu-be98/` (initramfs + boot.log).
