#!/bin/sh
# takeover.sh - LIVE, rmmod-recoverable open BCM4916 Runner takeover trial.
#
# ★ PRIMARY trial path: no firmware flash, no boot-state change. We free the
#   Runner from the stock datapath at runtime, load the open driver via the
#   platform_device shim, capture per-step breadcrumbs to /data, then reboot.
#   Because boot state is never touched, a reboot (or power-cycle on a hard hang)
#   restores the committed stock slot completely -> maximally safe.
#
# Run DETACHED from an SSH session (mgmt WILL drop when bcm_enet is removed):
#   nohup /data/open-enet/takeover.sh >/data/open-enet/takeover.out 2>&1 &
#   (then the SSH link dies; read /data/open-enet/trial.log after the reboot.)
#
# Payload expected under $P: bcm4916-runner.ko, bcm4916-runner-pdev.ko,
#   fw/brcm/bcm4916-runner-microcode.bin
set +e
P=/data/open-enet
BC=$P/trial.log
WINDOW=240
log(){ echo "$(cut -d. -f1 /proc/uptime 2>/dev/null)s $*" >> "$BC"; sync; }
mkdir -p "$P"; : > "$BC"
log "=== live Runner takeover start (mgmt WILL drop; reboot restores stock) ==="

# (1) SW DEADMAN: unconditional reboot after WINDOW. Boot state is unchanged
#     (committed=slot2), so a plain reboot returns to the known-good slot + mgmt.
#     This is a SEPARATE process, so it fires even if the main flow blocks on a
#     soft insmod hang. A HARD SoC hang is covered by the firmware's EXISTING HW
#     watchdog (deadman-early armed wdtctl -t 240; watchdog-disarm's petting_loop
#     freezes on a hard hang -> reset -> committed slot2). We deliberately do NOT
#     touch wdtctl here, to avoid disturbing that managed watchdog.
( i=0
  while [ $i -lt $WINDOW ]; do
      [ -f /tmp/open-enet-confirm ] && { echo "confirmed; sw deadman cancelled" >> "$BC"; sync; exit 0; }
      sleep 5; i=$((i + 5))
  done
  echo "SW-DEADMAN ${WINDOW}s -> reboot (restore committed slot2)" >> "$BC"; sync
  /bin/sync
  reboot -f 2>/dev/null || /sbin/reboot -f 2>/dev/null
  sleep 10
  echo b > /proc/sysrq-trigger 2>/dev/null   # ultimate fallback
) &
log "sw deadman armed ${WINDOW}s (reboot->slot2); HW watchdog left to the firmware scaffold"

# (3) continuous breadcrumb: snapshot driver bring-up dev_info to /data every 1s.
( while :; do
      dmesg 2>/dev/null | grep -iE "bcm4916-runner|bring-up|Runner" > "$P/dmesg.log.t"
      mv -f "$P/dmesg.log.t" "$P/dmesg.log"; sync; sleep 1
  done ) &
log "dmesg breadcrumb snapshotter started -> $P/dmesg.log"

# (4) free the Runner: bring stock netdevs down, then rmmod the stock datapath
#     (reverse of the load order; wfd/WiFi first as it depends on rdpa).
for i in br0 eth0 eth1 eth2 eth3 eth4; do ip link set $i down 2>/dev/null; done
log "stock netdevs down; rmmod stock datapath (dependency order) ..."
# Order from the live dep tree (remove users first). Leave the permanent libs
# bdmf/rdpa_gpl/bcmlibs/bcm_mpm/bcm_bpm loaded -- we only need rdpa (the Runner
# control plane, usecount 0) and its datapath peers out to free the Runner.
for m in bcm_pcie_hcd bcm_enet wfd bcmmcast pktrunner cmdlist pktflow \
         rdpa_cmd bcm_ingqos rdpa_gpl_ext rdpa_usr rdpa_mw rdpa gdx; do
    if rmmod "$m" 2>>"$BC"; then log "rmmod $m ok"; else log "rmmod $m FAILED (continuing)"; fi
done
log "rmmod sweep done; remaining datapath modules:"; lsmod 2>/dev/null | grep -iE "rdpa|enet|runner|pktflow|pktrunner|bdmf|mpm" >> "$BC"; sync

# (5) firmware loader + custom search path (microcode blob on /data)
modprobe firmware_class 2>>"$BC"
echo "$P/fw" > /sys/module/firmware_class/parameters/path 2>>"$BC"
[ -f "$P/fw/brcm/bcm4916-runner-microcode.bin" ] && log "microcode blob present" || log "WARN microcode MISSING"

# (6) THE TAKEOVER: register the driver, then the platform_device (-> probe fires
#     -> microcode load + RNR enable/wakeup + SBPM/DSPTCHR/BBH + feed/RX/TX rings).
insmod "$P/bcm4916-runner.ko";       log "insmod bcm4916-runner.ko ret=$?"
insmod "$P/bcm4916-runner-pdev.ko";  log "insmod pdev shim ret=$? (triggers probe)"
sleep 2
dmesg 2>/dev/null | grep -iE "bcm4916-runner|bring-up|Runner" >> "$BC"; sync

# (7) bring the conduit netdev up (poll mode)
ip link set rnr0 up 2>>"$BC" || ifconfig rnr0 up 2>>"$BC"
log "rnr0 up attempted"
log "=== takeover issued; deadman will reboot in <=${WINDOW}s; read $BC after ==="
exit 0
