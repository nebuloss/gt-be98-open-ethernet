#!/usr/bin/env bash
# run-virt-baseline.sh — boot the mainline aarch64 kernel under qemu-system-aarch64
# -machine virt as the BE98 host-side sanity loop. RUN ON dev-build.
#
# This proves toolchain + kernel + rootfs boot. It does NOT model any BCM4916
# device (no UNIMAC / SF2-MDIO / GPHY) — that is Stage 2+ (see qemu/README.md).
#
# Success criteria: the boot log contains "BE98_BOOT_OK".
#
# Usage:
#   ./run-virt-baseline.sh                 # interactive (Ctrl-A X to quit qemu)
#   TIMEOUT=90 ./run-virt-baseline.sh      # auto-exit after N seconds (CI/headless)
set -euo pipefail

KERNEL="${KERNEL:-$HOME/mainline/arch/arm64/boot/Image}"
INITRD="${INITRD:-$HOME/qemu-be98/initramfs.cpio.gz}"
LOG="${LOG:-$HOME/qemu-be98/boot.log}"
TIMEOUT="${TIMEOUT:-0}"   # 0 = no timeout (interactive)

# In timed/headless mode, ask init to poweroff after printing the marker so QEMU
# exits cleanly (rc 0) instead of being SIGTERM'd at the timeout (rc 124).
AUTO=""
[ "$TIMEOUT" -gt 0 ] && AUTO=" be98_auto=poweroff"

[ -f "$KERNEL" ] || { echo "missing kernel: $KERNEL (build with: cd ~/mainline && rtk make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j\$(nproc) Image)"; exit 1; }
[ -f "$INITRD" ] || { echo "missing initrd: $INITRD (build with: qemu/scripts/build-initramfs.sh)"; exit 1; }

QEMU=(qemu-system-aarch64
  -machine virt -cpu cortex-a53 -smp 4 -m 1024
  -kernel "$KERNEL"
  -initrd "$INITRD"
  -append "console=ttyAMA0 rdinit=/init panic=5${AUTO}"
  -netdev user,id=n0 -device virtio-net-device,netdev=n0
  -nographic -no-reboot)

mkdir -p "$(dirname "$LOG")"
if [ "$TIMEOUT" -gt 0 ]; then
  timeout "$TIMEOUT" "${QEMU[@]}" 2>&1 | tee "$LOG"
  echo "=== exit; log: $LOG ($(wc -l < "$LOG") lines) ==="
  grep -aq BE98_BOOT_OK "$LOG" && echo "RESULT: BOOT OK" || { echo "RESULT: BOOT FAILED (no BE98_BOOT_OK marker)"; exit 1; }
else
  "${QEMU[@]}" 2>&1 | tee "$LOG"
fi
