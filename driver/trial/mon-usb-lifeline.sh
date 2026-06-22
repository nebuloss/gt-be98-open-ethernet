#!/bin/bash
U="ssh -o BatchMode=yes -o ConnectTimeout=5 -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -p 2222 admin@10.0.0.77"
G="ssh -o BatchMode=yes -o ConnectTimeout=5 -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -p 2222 admin@10.0.0.8"
seen_down=0
for i in $(seq 1 80); do
  if $U 'echo ok' >/dev/null 2>&1; then
    echo "$(date +%H:%M:%S) ★ USB LIFELINE UP (10.0.0.77) -> DISARM + go live"
    $U 'touch /tmp/deadman-disarm; wdtctl stop 2>/dev/null; echo "slot:$(grep -o ubi.block=0,[0-9] /proc/cmdline)"; echo "=== dmesg bring-up ==="; dmesg | grep -iE "bcm4916-runner|bring-up|Runner" | tail -40; echo "=== rnr0? ==="; ip link show rnr0 2>/dev/null || echo "no rnr0"; echo "=== trial.log ==="; cat /data/open-enet/trial.log'
    echo "$(date +%H:%M:%S) LIVE (disarmed; device staying on slot1 via USB)"
    exit 0
  fi
  if $G 'echo ok' >/dev/null 2>&1; then
    if [ $seen_down -eq 1 ]; then
      echo "$(date +%H:%M:%S) 10G back (reverted to slot2; USB lifeline did NOT come up)"
      $G 'touch /tmp/deadman-disarm; wdtctl stop 2>/dev/null; rm -f /data/.trial-armed /data/open_enet; grep -o ubi.block=0,[0-9] /proc/cmdline; echo "=== trial.log ==="; cat /data/open-enet/trial.log; echo "=== dmesg.log ==="; cat /data/open-enet/dmesg.log'
      echo "$(date +%H:%M:%S) DONE (reverted)"
      exit 0
    fi
    echo "$(date +%H:%M:%S) 10G up (pre-reboot)"
  else
    seen_down=1; echo "$(date +%H:%M:%S) both down (slot1 trial booting/takeover)"
  fi
  sleep 8
done
echo "timeout"
