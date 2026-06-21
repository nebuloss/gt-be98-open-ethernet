# Cycle 01 — first mainline on-silicon trial (2026-06-21)

**Image:** `GT-BE98-mainline-initramfs_nand_squashfs.pkgtb` (mainline Image + embedded initramfs +
breadcrumb init + driver-as-modules), flashed to **slot1** via `ubiupdatevol` (vol3 bootfs1 static
42.8 MiB, vol4 rootfs1 dynamic 372 KiB). slot2 (committed bench) + metadata + loader untouched.
Trial armed: `/data/.trial-armed TRIAL_SLOT=1 GOOD_SLOT=2 WINDOW=240`, `bcm_bootstate 6`
(BOOT_SET_PART1_IMAGE_ONCE). Pre-reboot gate PASSED (committed 2, Reboot Partition First).

## OUTCOME: auto-revert PROVEN ✓ ; mainline did NOT reach userspace ✗
- Rebooted; device returned as **committed slot2** (`ubi.block=0,6`, `4.19.294`) within ~60 s.
  **No power-cycle needed** — the reboot/auto-revert safety net works for a foreign-OS trial. slot2
  intact, `committed 2`. ★This is the key safety validation — iteration is now safe.★
- **No `/data` breadcrumb written** → the mainline init never ran (mainline never reached userspace).
- Diagnostics post-revert: `boot_failed_count = 0`, `reset_reason = 34` (normal, NOT a watchdog/crash
  code). A counted slot1 boot-FAILURE would bump boot_failed_count; it didn't.

## Most likely cause: U-Boot never selected slot1 (sequence-number)
slot1 seq = 52, slot2 seq = 97; U-Boot picks the higher seq. The `bcm_bootstate 6` ONCE flag set
"Reboot Partition: First", but it appears NOT to have overridden the higher-seq slot2 → U-Boot likely
re-booted slot2 directly (fast ~60 s revert + no breadcrumb + boot_failed_count=0 all fit "slot1 never
ran"). Less likely (but not excluded without serial): U-Boot tried slot1 and the FIT/kernel failed
very early. Cannot distinguish without serial console.

## Next: force slot1 selection (seq bump) — cycle 02
Build a tiny helper against `/lib/libbcm_flashutil.so` to `setImgValidStatus(1,1)` + `setImgSeqNum(1,98)`
(98 > slot2's 97) so U-Boot unambiguously selects slot1, keep `bcm_bootstate 6` ONCE for revert, retry.
If mainline still doesn't reach userspace after that → it's a real early-boot failure and **serial is
required** for further progress (no other observability into pre-userspace mainline on this no-serial unit).

★OBSERVABILITY BOTTLENECK: with no serial, each blind trial yields ~1 bit (reached-userspace or not).
A USB-serial UART adapter (GT-BE98 header undocumented) is the real enabler for mainline bring-up.★
