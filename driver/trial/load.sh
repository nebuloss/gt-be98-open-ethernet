#!/bin/sh
# load.sh - open BCM4916 Runner takeover trial loader.
#
# Runs SYNCHRONOUSLY inside rc3.d/S45bcm-base-drivers (start) when the arm-flag
# /data/open_enet exists, INSTEAD of the stock datapath stack (bdmf/rdpa/bcm_enet/
# pktrunner). Because the stock stack is skipped, the management Ethernet is DOWN
# for the duration -- so this trial is observed OUT OF BAND via a breadcrumb on
# /data (a slot-independent UBI volume that survives the auto-revert), and a
# two-layer deadman returns the box to the committed GOOD slot (slot2).
#
# Payload is baked read-only into the trial rootfs at $RO; logs go to writable /data.
set +e
RO=/usr/lib/open-enet			# baked-in: load.sh, .ko, fw/
LOG=/data/open-enet			# writable, survives revert
BC=$LOG/trial.log
WINDOW=240				# deadman / watchdog window (s)

mkdir -p "$LOG"
log() { echo "$(cut -d. -f1 /proc/uptime 2>/dev/null)s $*" >> "$BC"; sync; }
: > "$BC"
log "=== open Runner takeover trial start (mgmt WILL drop; expect auto-revert) ==="

# (1) SOFTWARE DEADMAN: commit GOOD slot2 + reboot unless confirmed within WINDOW.
#     mgmt is down so a confirm is normally impossible -> it always reverts (the
#     intended first-light flow; read $BC from slot2 afterwards). The confirm hook
#     exists for when our own datapath can eventually carry a login.
(
	i=0
	while [ $i -lt $WINDOW ]; do
		[ -f /tmp/open-enet-confirm ] && { echo "confirmed; software deadman cancelled" >> "$BC"; sync; exit 0; }
		sleep 5; i=$((i + 5))
	done
	echo "SW-DEADMAN ${WINDOW}s elapsed -> bcm_bootstate 7 (commit slot2) + reboot" >> "$BC"; sync
	/bin/bcm_bootstate 7 >> "$BC" 2>&1		# defeat any late auto-commit of slot1
	/bin/sync
	/sbin/reboot -f 2>/dev/null || reboot -f
) &
log "sw-deadman armed (${WINDOW}s); confirm via: touch /tmp/open-enet-confirm"

# (2) HW WATCHDOG backstop for a hard SoC hang (unpetted -- no '-d' daemon).
#     Same tool/primitive the proven buildroot deadman uses; fires the watchdog
#     reset if the takeover wedges the core so the sw-deadman can't run.
/bin/wdtctl -t $WINDOW start >> "$BC" 2>&1
log "hw watchdog armed ${WINDOW}s unpetted (hard-hang backstop)"

# (3) CONTINUOUS BREADCRUMB: snapshot the driver's per-step dev_info to /data
#     every 1s, so if insmod hangs mid-bring-up we still keep the steps reached.
(
	while :; do
		dmesg 2>/dev/null | grep -iE "bcm4916-runner|bring-up|Runner" > "$LOG/dmesg.log.tmp"
		mv -f "$LOG/dmesg.log.tmp" "$LOG/dmesg.log"; sync
		sleep 1
	done
) &
log "dmesg breadcrumb snapshotter started -> $LOG/dmesg.log"

# (4) firmware loader + custom search path (the microcode blob is baked at $RO/fw)
modprobe firmware_class 2>> "$BC"
echo "$RO/fw" > /sys/module/firmware_class/parameters/path 2>> "$BC"
log "fw search path -> $RO/fw (blob: $RO/fw/brcm/bcm4916-runner-microcode.bin)"
[ -f "$RO/fw/brcm/bcm4916-runner-microcode.bin" ] && log "microcode blob present" || log "WARN: microcode blob MISSING"

# (5) THE TAKEOVER: load the open driver. It probes the runner@82000000 DT node
#     and cold-inits the Runner (microcode -> RNR enable/wakeup -> SBPM/DSPTCHR/
#     BBH -> feed/RX/TX rings). Each step logs a 'bring-up:' breadcrumb (step 3).
log "insmod bcm4916-runner.ko ..."
insmod "$RO/bcm4916-runner.ko"; R=$?
log "insmod returned ret=$R"

# (6) bring the conduit netdev up (poll mode; no IRQ confirmation required)
ip link set rnr0 up 2>> "$BC" || ifconfig rnr0 up 2>> "$BC"
log "rnr0 up attempted"
sleep 2
dmesg 2>/dev/null | grep -iE "bcm4916-runner|bring-up|Runner" >> "$BC"; sync
log "=== takeover issued; awaiting deadman revert; read $BC + $LOG/dmesg.log on slot2 ==="
exit 0
