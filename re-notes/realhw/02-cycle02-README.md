# Cycle 02 — shrunk kernel (2026-06-21)

**Change vs cycle 01:** kernel shrunk 42.8 MB → **22.4 MB** decompressed (focused config: dropped
DEBUG_INFO/KALLSYMS_ALL + ~50 non-Broadcom SoC platforms + DRM/sound/media/etc.), lzo-compressed, our
drivers as modules. This resolved the cycle-01 U-Boot FIT-overlap (`BOOTM_ERR_RESET`): load extent now
ends ~23.3 MB, well below the 32 MB FIT. `dump_pkgtb --check` PASS.

Flashed to slot1 (ubiupdatevol, slot2 committed+intact), armed ONCE (`bcm_bootstate 6`), rebooted.

## OUTCOME: still no userspace signal ✗ (auto-revert clean ✓)
- Device auto-reverted to committed slot2 (`ubi.block=0,6`, 4.19.294) in ~60 s — clean, no power-cycle.
- **No `/data` breadcrumb**; `boot_failed_count=0`; `reset_reason=34` (normal); `old_reset_reason=0`.
- ★Workflow fix: cycle 01 left `/data/.trial-armed` set → slot2 reboot-looped (~240 s, watchdog
  unpetted in trial-mode) and wiped staged /tmp. Now the poll **disarms + removes `/data/.trial-armed`
  on return to slot2** every cycle → no loop. (Verified stable, uptime grows.)

## Interpretation: lost observability
The overlap is fixed, so mainline *should* now execute — but we get ZERO signal. The breadcrumb
depends on mainline mounting the `/data` NAND/UBI volume, which mainline may not manage (the feasibility
pass flagged: mainline lacks a bcmbca NAND pinctrl). So "no breadcrumb" could mean either (a) mainline
panics very early, or (b) mainline reaches userspace but can't write `/data` AND our driver doesn't
bring up the network → invisible. Cannot distinguish without serial, a working `/data`, or a
driver-independent shell.

## Cycle 03 hypothesis (merlin-comparison, observability-free): missing reserved-memory
Our mainline `gt-be98.dts` declares `memory@0 = <0 0x80000000>` (full 2 GB) with **no
`reserved-memory`**. The vendor reserves ATF/BL31 (the secure monitor that services PSCI), bootloader,
and XRDP regions. If mainline uses the ATF/BL31 RAM, the first PSCI CPU_ON / SMP bringup faults *very*
early → exactly "no signal, fast clean revert". Fix in progress: add vendor-matching `reserved-memory`
(esp. ATF/BL31 `no-map`) to our DTS, rebuild DTB, repackage. If cycle 03 still gives no signal, the
real unlock is a **driver-independent shell**: a USB-Ethernet dongle (mainline xhci+cdc_ether) gives a
reachable shell even when /data, our driver, and serial are all unavailable — the right observability
tool for mainline bring-up here.
