#!/bin/sh
# load.sh - boot-time open Runner takeover loader (Path A).
#
# Runs from the bcm-base-drivers.sh guard (after `start)`) ONLY when the arm-flag
# /data/open_enet exists, INSTEAD of the stock datapath insmods. So the Runner is
# PRISTINE (rdpa/pktrunner/bcm_mpm never loaded) and our driver cold-inits it.
#
# AUTO-REVERT is owned by the firmware scaffold, not this script: a slot1 trial
# is armed with /data/.trial-armed, so deadman-early forks /sbin/trial-deadman
# and watchdog-disarm leaves the HW watchdog UNPETTED -> with mgmt down (datapath
# skipped) the box auto-reverts to committed slot2 in ~WINDOW s. We therefore do
# NOT touch wdtctl/bcm_bootstate here; we just take over and breadcrumb to /data.
set +e
# Modules/microcode load from the baked rootfs path, but a /data override (if
# present) wins -> iterate the driver via a /data push WITHOUT re-flashing.
RO=/usr/lib/open-enet			# baked into the trial rootfs
[ -f /data/open-enet/bcm4916-runner.ko ] && RO=/data/open-enet
LOG=/data/open-enet			# writable, survives the revert
BC=$LOG/trial.log
mkdir -p "$LOG"
log(){ echo "$(cut -d. -f1 /proc/uptime 2>/dev/null)s $*" >> "$BC"; sync; }
: > "$BC"
log "=== boot-time open Runner takeover (stock datapath SKIPPED; pristine Runner) ==="

# continuous breadcrumb: snapshot the driver's per-step dev_info every 1s so a
# mid-bring-up hang still leaves the steps reached on /data (read after revert).
( while :; do
      dmesg 2>/dev/null | grep -iE "bcm4916-runner|bring-up|Runner" > "$LOG/dmesg.log.t"
      mv -f "$LOG/dmesg.log.t" "$LOG/dmesg.log"; sync; sleep 1
  done ) &
log "dmesg breadcrumb snapshotter started -> $LOG/dmesg.log"

# firmware loader + custom search path (microcode blob baked at $RO/fw)
modprobe firmware_class 2>>"$BC"
echo "$RO/fw" > /sys/module/firmware_class/parameters/path 2>>"$BC"
[ -f "$RO/fw/brcm/bcm4916-runner-microcode.bin" ] && log "microcode blob present" || log "WARN microcode MISSING"

# THE TAKEOVER: register the driver, then the platform_device (-> probe -> cold
# init: microcode + RNR enable/wakeup + SBPM/DSPTCHR/BBH + feed/RX/TX rings).
insmod "$RO/bcm4916-runner.ko";       log "insmod bcm4916-runner.ko ret=$?"
insmod "$RO/bcm4916-runner-pdev.ko";  log "insmod pdev shim ret=$? (triggers probe)"
sleep 2
dmesg 2>/dev/null | grep -iE "bcm4916-runner|bring-up|Runner" >> "$BC"; sync
ip link set rnr0 up 2>>"$BC" || ifconfig rnr0 up 2>>"$BC"
log "rnr0 up attempted"
log "=== takeover issued; scaffold deadman will revert to slot2; read $BC after ==="
exit 0
