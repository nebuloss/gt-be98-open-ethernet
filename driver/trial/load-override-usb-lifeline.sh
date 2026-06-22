#!/bin/sh
# load-override.sh - bring up the USB-Eth lifeline, riding out the AX88179
# re-enumeration (device re-plugs itself once after MCU config). Defer the
# Runner takeover so we get a clean live SSH shell over USB.
set +e
LOG=/data/open-enet; BC=$LOG/trial.log; M=/lib/modules/4.19.294
mkdir -p "$LOG"
log(){ echo "$(cut -d. -f1 /proc/uptime 2>/dev/null)s $*" >> "$BC"; sync; }
: > "$BC"
log "=== load-override: USB lifeline (ride AX88179 re-enum); takeover deferred ==="
( while :; do dmesg 2>/dev/null | grep -iE "bcm4916-runner|bring-up|Runner|usb|ax88|xhci|eth" > "$LOG/dmesg.log.t"; mv -f "$LOG/dmesg.log.t" "$LOG/dmesg.log"; sync; sleep 1; done ) &

insmod $M/kernel/drivers/usb/common/usb-common.ko 2>>"$BC"
insmod $M/kernel/drivers/usb/core/usbcore.ko 2>>"$BC"
insmod $M/kernel/drivers/usb/host/xhci-hcd.ko 2>>"$BC"
insmod $M/kernel/drivers/usb/host/xhci-plat-hcd.ko 2>>"$BC"
insmod $M/extra/bcm_bca_usb.ko 2>>"$BC"
insmod $M/kernel/drivers/net/mii.ko 2>>"$BC"
insmod $M/kernel/drivers/net/usb/usbnet.ko 2>>"$BC"
insmod $M/kernel/drivers/net/usb/ax88179_178a.ko 2>>"$BC"
log "usb modules loaded; waiting for the AX88179 to settle (poll 120s)"

# Poll up to 120s. Re-check every 4s; require the SAME usb iface to persist for
# 2 consecutive checks (so we pick the post-re-enumeration, stable instance) and
# then keep (re)assigning the IP each loop until SSH-over-USB is confirmed.
UIF=""; prev=""; stable=0; i=0; armed=0
while [ $i -lt 120 ]; do
    cur=""
    for n in $(ls /sys/class/net 2>/dev/null); do
        if readlink "/sys/class/net/$n/device" 2>/dev/null | grep -q usb; then cur="$n"; fi
    done
    if [ -n "$cur" ] && [ "$cur" = "$prev" ]; then stable=$((stable+1)); else stable=0; fi
    prev="$cur"
    if [ -n "$cur" ] && [ "$stable" -ge 1 ]; then
        UIF="$cur"
        ip addr add 10.0.0.77/24 dev "$UIF" 2>/dev/null
        ip link set "$UIF" up 2>/dev/null
        if [ "$armed" = 0 ]; then
            mkdir -p /tmp/oe-db
            /usr/br/sbin/dropbearmulti dropbear -R -E -p 10.0.0.77:2222 \
                -d /tmp/oe-db/dss -r /tmp/oe-db/rsa 2>>"$BC" &
            log "USB iface $UIF up @10.0.0.77; dropbear started (ssh -p2222 admin@10.0.0.77)"
            armed=1
        fi
    fi
    sleep 4; i=$((i+4))
done
[ "$armed" = 1 ] && log "=== USB lifeline ARMED; takeover deferred (insmod manually) ===" \
                  || log "WARN: USB iface never stabilized in 120s; deadman will revert"
exit 0
