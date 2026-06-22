#!/bin/sh
# load.sh - boot-time open Runner takeover + USB-Eth LIVE lifeline (Path A).
#
# Runs from the bcm-base-drivers.sh guard (after `start)`) when /data/open_enet
# exists, INSTEAD of the stock datapath insmods (Runner stays pristine). It first
# brings up a USB-Ethernet dongle (a NIC independent of the Runner) with a static
# IP so we keep an SSH lifeline AFTER the 10G mgmt drops -> live iteration. Then
# it takes over the Runner via our driver (which binds the stock rdpa_drv DT node
# directly: real reg window + of_dma_configure DMA + queue0 IRQ, no shim).
#
# SAFETY: a slot1 trial is armed with /data/.trial-armed, so if the USB lifeline
# does NOT come up the scaffold auto-reverts to committed slot2 in ~240s. When the
# USB shell IS reachable, disarm from it (touch /tmp/deadman-disarm; wdtctl stop)
# to iterate without time pressure.
set +e
RO=/usr/lib/open-enet
[ -f /data/open-enet/bcm4916-runner.ko ] && RO=/data/open-enet	# /data override
LOG=/data/open-enet; BC=$LOG/trial.log
M=/lib/modules/4.19.294
mkdir -p "$LOG"
log(){ echo "$(cut -d. -f1 /proc/uptime 2>/dev/null)s $*" >> "$BC"; sync; }
: > "$BC"
log "=== boot-time takeover + USB lifeline (stock datapath SKIPPED) ==="

# breadcrumb snapshotter (survives a revert)
( while :; do dmesg 2>/dev/null | grep -iE "bcm4916-runner|bring-up|Runner|usb|xhci" > "$LOG/dmesg.log.t"; mv -f "$LOG/dmesg.log.t" "$LOG/dmesg.log"; sync; sleep 1; done ) &

# (A) USB-Eth LIFELINE -------------------------------------------------------
# core -> xhci -> BCA glue (binds brcm,bcmbca-xhci) -> usbnet drivers.
insmod $M/kernel/drivers/usb/common/usb-common.ko 2>>"$BC"
insmod $M/kernel/drivers/usb/core/usbcore.ko 2>>"$BC"
insmod $M/kernel/drivers/usb/host/xhci-hcd.ko 2>>"$BC"
insmod $M/kernel/drivers/usb/host/xhci-plat-hcd.ko 2>>"$BC"
insmod $M/extra/bcm_bca_usb.ko 2>>"$BC"
log "usb host modules loaded; waiting for enumeration"
sleep 4
insmod $M/kernel/drivers/net/usb/usbnet.ko 2>>"$BC"
insmod $M/kernel/drivers/net/usb/cdc_ether.ko 2>>"$BC"
insmod $M/kernel/drivers/net/usb/ax88179_178a.ko 2>>"$BC"
insmod $M/kernel/drivers/net/usb/rndis_host.ko 2>>"$BC"
sleep 5
# find the USB-backed net iface (its /sys .../device symlink points under usb)
UIF=""
for i in $(ls /sys/class/net 2>/dev/null); do
    if readlink "/sys/class/net/$i/device" 2>/dev/null | grep -q usb; then UIF="$i"; break; fi
done
if [ -n "$UIF" ]; then
    ip addr add 10.0.0.77/24 dev "$UIF" 2>>"$BC"
    ip link set "$UIF" up 2>>"$BC"
    log "USB LIFELINE UP: $UIF = 10.0.0.77/24 (ssh -p2222 admin@10.0.0.77)"
else
    log "WARN: no USB-Eth iface found (dongle/driver?); relying on deadman revert"
fi

# (B) RUNNER TAKEOVER --------------------------------------------------------
modprobe firmware_class 2>>"$BC"
echo "$RO/fw" > /sys/module/firmware_class/parameters/path 2>>"$BC"
[ -f "$RO/fw/brcm/bcm4916-runner-microcode.bin" ] && log "microcode present" || log "WARN microcode MISSING"
insmod "$RO/bcm4916-runner.ko"
log "insmod bcm4916-runner.ko ret=$? (binds stock rdpa_drv node)"
sleep 2
dmesg 2>/dev/null | grep -iE "bcm4916-runner|bring-up|Runner" >> "$BC"; sync
ip link set rnr0 up 2>>"$BC" || ifconfig rnr0 up 2>>"$BC"
log "=== takeover issued; SSH the USB lifeline (10.0.0.77) to iterate/disarm ==="
exit 0
