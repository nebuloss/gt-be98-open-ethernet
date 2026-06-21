# Real-hardware trial-boot logs (open Ethernet driver on GT-BE98)

Persistent, per-cycle logs of on-silicon trial boots of our mainline open-Ethernet image,
mirroring the WiFi effort's `traces/realhw/` discipline. **Raw logs (may contain device
IPs/MACs) go to a gitignored local dir; only REDACTED summaries are committed here.**

## Safety model (no physical rescue readily available)
- Mainline trial image is flashed to the **INACTIVE slot only** (currently **slot1**) via
  `ubiupdatevol`; the **committed slot (currently slot2, the working custom
  4.19 bench) is the auto-revert target and is NEVER overwritten**, nor is `mtd1` (loader).
- Trial boot is **ONCE / revert-protected** (`bcm_bootstate` ONCE flag); `/data/.trial-armed`
  (WINDOW=240) + the HW watchdog stay **armed/unpetted** so a bad/unreachable boot
  auto-reverts to the committed slot. U-Boot itself arms the WD (strings: `WDT: Started with
  servicing`), so even a pre-userspace kernel failure reverts.
- **Worst case is power-cycle-recoverable, NOT a brick**: per the WiFi traces, a hard host-wide
  wedge that stops the watchdog just needs an unplug/replug (with `/data/.trial-armed` +
  committed safe slot intact it boots clean) — no Firmware Restoration. A true brick (needs
  rescue mode) only happens if a flash corrupts BOTH slots/loader → avoided by slot1-only
  per-volume `ubiupdatevol`.
- Never commit the trial slot unless it boots HEALTHY and is confirmed reachable.

## Observability (no serial)
Success signals, in order of reliability: (1) device becomes network-reachable via our driver
(SSH in within the WD window → hold + inspect); (2) breadcrumb written by the initramfs init to
the `/data` UBI volume (`/mnt/data/mainline-boot.log`) survives the revert → read it from the
committed slot afterward. On total silence, the cycle auto-reverts and we iterate.

## Per-cycle log convention
For each cycle `NN`: `NN-<desc>.runlog` (full host-side cycle log), `NN-<desc>-breadcrumb.log`
(pulled `/data/mainline-boot.log`), `NN-<desc>-LASTLINE.txt` (last action before any wedge),
`NN-<desc>-README.md` (redacted outcome). Commit the redacted set after each cycle.

## Status
- Mainline `.pkgtb` (non-initramfs) built + verified (Broadcom `dump_pkgtb --check` OK).
- Initramfs trial kernel (brcmnand/UBI=y, driver as modules, breadcrumb init, USB-Eth optional)
  — building.
- Exact `ubiupdatevol` slot-flash command sequence — being reconstructed from SDK
  `bcm_flashutil` (`writeImageToNand`/`setBootImageState`) since `open-flash.sh` is no longer on
  the build host.
- NO flash performed yet.
