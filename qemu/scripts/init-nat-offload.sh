#!/bin/busybox sh
# NAT-offload (Phase 2) test init: bring rnr0 up, let the peer inject MISS
# frames, program a routed IPv4 SNAT+NAPT flow via the offload_nat_selftest
# debugfs trigger, then let the peer inject HIT frames (which the Runner model
# rewrites + forwards in HW, CPU bypassed).
/bin/busybox --install -s
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null
echo 1 > /proc/sys/net/ipv6/conf/all/disable_ipv6 2>/dev/null
echo "=== BE98 NAT-OFFLOAD TEST initramfs up ==="
ip link set rnr0 up 2>/dev/null
ip link set rnr0 promisc on 2>/dev/null
sleep 2
echo "NATOFF_PHASE=pre_program RXP=$(cat /sys/class/net/rnr0/statistics/rx_packets)"
sleep 6
echo "RXP_AFTER_MISS=$(cat /sys/class/net/rnr0/statistics/rx_packets)"
echo "--- PROGRAM NAT-C routed SNAT+NAPT flow (offload_nat_selftest) ---"
echo "go" > /sys/kernel/debug/bcm4916-runner/offload_nat_selftest
echo "NATOFF_PHASE=programmed"
sleep 9
echo "RXP_AFTER_HIT=$(cat /sys/class/net/rnr0/statistics/rx_packets)"
echo "RXB_AFTER_HIT=$(cat /sys/class/net/rnr0/statistics/rx_bytes)"
echo "=== NAT_OFFLOAD_TEST_DONE ==="
poweroff -f
