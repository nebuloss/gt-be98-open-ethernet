# Cycle 04 — enable NAND + ★ONCE-boot PROVEN★ (2026-06-21)

## Change: enabled the NAND controller in our DTS
Upstream `bcm6813.dtsi` ships `nand-controller@1800` as `status="disabled"`, and our
board DTS never enabled it — so a mainline boot had **no MTD device at all** → no UBI →
the `/data` breadcrumb the trial init writes was *physically impossible* (it silently fell
back to a tmpfs log that dies at reset). Enabled `nand_controller` + `nandcs`
(`nand-on-flash-bbt`, ECC inherited from the bootloader exactly like the stock GT-BE98 DT)
+ a `fixed-partitions` node (loader/image) so `/proc/mtd` carries the `image` label the
init greps to `ubiattach` the UBI holding `/data`. DTB rebuilt (7171 B), bootfs FIT
repackaged (`cycle04_bootfs.itb`, 11.6 MB), flashed slot1, ONCE-booted.

## OUTCOME: still no breadcrumb ✗ (clean revert ✓)
Reverted to committed slot2 in ~60 s; `/data/mainline-boot.log` absent. Same ~60 s as
cycles 01–03.

## ★ THE DECISIVE TEST: is U-Boot even booting slot1? — YES ★
After 4 identical-looking failures I stopped *assuming* `bcm_bootstate 6`
(PART1_IMAGE_ONCE) boots slot1 and tested it directly: copied the **known-good stock
4.19 bootfs2+rootfs2 into slot1** (vol3/vol4, on-device `cat /dev/ubi0_5|6`), armed the
same ONCE, rebooted.

**RESULT: device came up on `ubi.block=0,4` (SLOT1) in ~26 s, stock 4.19, fully
reachable.** → `bcm_bootstate 6` ONCE **works**; slot1 boot path, slot1 rootfs, NAND/UBI
`/data`, and networking all work. Reverted cleanly to committed slot2 with one reboot
(slot1 left uncommitted, committed=2).

## What this proves / rules out
- The flash + ONCE + auto-revert mechanism is **fully proven** (no longer a hypothesis).
- In cycles 01–04 **U-Boot WAS booting our mainline slot1**; mainline genuinely produced
  zero observable signal. It is NOT a slot-selection / seq-number artifact.
- The failure is isolated to **mainline itself**: either (a) it panics before `init` runs,
  or (b) it reaches userspace but no output channel works — `/data` needs mainline
  `brcmnand` to bind (our node is the upstream-blessed one, so it *should*), and network
  needs our (unproven) driver or USB-Eth (host bring-up unproven).

## Next: a mainline-side signal that doesn't depend on our driver
The user has plugged a **USB-Ethernet dongle**. Stock has USB host compiled out entirely
(xhci platform node present but no driver bound, `/sys/bus/usb` empty) so it can't be
pre-tested on stock, but mainline has the host stack built-in (`xhci`/`ehci`/`ohci`
platform + `PHY_BRCM_USB=y`) and BCM4908 (the 4916's sibling) already runs USB in
mainline via `generic-xhci` + `brcm,bcm4908-usb-phy`. 4916 USB reg map captured from
silicon: xhci `usb-xhci`@0x800c0000(0x484)+`xhci-gbl`@0x800cc100(0x544),
usb_ctrl@0x800cf200(0x128). Plan: add USB nodes (adapt BCM4908) + a `pmb` power node, make
cdc_ether/r8152/ax88179 built-in, and have the init bring up the dongle (static IP +
dropbear + netconsole). Dual-channel cycle (NAND breadcrumb + USB shell): if EITHER fires
→ mainline reached userspace + we get a shell/log; if NEITHER → strong evidence of an
early pre-userspace panic → serial UART becomes the required instrument.
