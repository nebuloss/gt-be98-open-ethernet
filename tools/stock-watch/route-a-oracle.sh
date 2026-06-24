#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# route-a-oracle.sh - capture the rdpa.ko-only Route A egress values from a LIVE
# stock device (slot2, working CPU_TX egress), to pin the route_a_* module-param
# defaults for the open driver. READ-ONLY: uses xrdp_peek.ko MMIO read mode (the
# stock kernel has CONFIG_STRICT_DEVMEM=y, so userspace /dev/mem / busybox devmem
# is blocked). No kprobes, no writes, nothing to undo. See:
#   re-notes/realhw/11-route-a-egress-spec.md   (what each value means)
#   tools/stock-watch/README.md                 (xrdp_peek build/vermagic recipe)
#
# Run ON THE DEVICE (stock slot2) with the matching-vermagic xrdp_peek.ko present:
#   /bin/busybox sh route-a-oracle.sh [/path/to/xrdp_peek.ko]
# (use /bin/busybox sh - /usr/sbin/sh is a devmem tool that shadows the shell.)
#
# These are all side-effect-free config registers / SRAM (NOT FIFO/clear-on-read),
# so MMIO reads are safe. Output: grep "XRDPEEK" lines; map them with spec 11.

KO="${1:-./xrdp_peek.ko}"
[ -f "$KO" ] || { echo "xrdp_peek.ko not found at $KO"; exit 1; }

# peek <phys> <rlen> <label>  - one ioremap'd read window, dumped to dmesg.
peek() {
	rmmod xrdp_peek 2>/dev/null
	dmesg -c >/dev/null 2>&1
	insmod "$KO" phys="$1" rlen="$2" allow_mmio=1 2>/dev/null
	echo "---- $3  ($1 +$2) ----"
	dmesg | grep XRDPEEK
	rmmod xrdp_peek 2>/dev/null
}

echo "================ Route A oracle (stock slot2) ================"

# (A) QM global: ENABLE_CTRL@0x000, FPM_CONTROL@0x00c, FPM_BASE@0x034,
#     DDR_SOP@0x03c, MEM_AUTO_INIT@0x138, MEM_AUTO_INIT_STS@0x13c.
peek 0x82c00000 0x140 "QM global (ENABLE_CTRL .. MEM_AUTO_INIT_STS)"

# (C) QM RUNNER_GRP: 15 groups x 4 regs, base 0x82c00300 stride 0x10.
#     Find the group with RNR_ENABLE(bit16)=1 -> its QUEUE_CONFIG gives the
#     LAN egress queue [START_QUEUE[8:0], END_QUEUE[24:16]] and RNR_CONFIG gives
#     the TM core RNR_BB_ID[5:0] + RNR_TASK[11:8]. (route_a_grp/queue/tm_bb_id/
#     tm_task.)
peek 0x82c00300 0xf0 "QM RUNNER_GRP[0..14] (RNR_CONFIG/QUEUE_CONFIG/PDFIFO/UPDFIFO)"

# (B) BBH_TX per instance (LAN base 0x82890000, stride 0x2000). For each, read:
#   +0x000 MACTYPE/BBCFG_1/BBCFG_2/BBCFG_3, +0x050 RNRCFG_1/2 (TM ptraddr/task),
#   +0x400 Q2RNR(LAN) +0x4b0 QMQ(LAN), +0x700 Q2RNR(unified) +0x7b0 QMQ(unified).
# The instance whose QMQ has a bit set is the QM-fed LAN egress -> route_a_bbh_inst.
# (LITERAL addresses: busybox ash arithmetic overflows 32-bit on >0x80000000, so
#  we never compute them in-shell. inst i base = 0x82890000 + i*0x2000.)
bbh_one() {	# $1=instance base addr  $2=instance#
	peek "$1" 0x10 "BBH_TX[$2] MACTYPE/BBCFG_1..3 (+0x000)"
	# RNRCFG_1/2 are at +0x050; pass the precomputed addr as $3
	peek "$3" 0x20 "BBH_TX[$2] RNRCFG_1/2 (TM ptraddr/task, +0x050)"
	# window 0xc0 from +0x400 reaches QMQ at +0x4b0 (0x400..0x4bf); same +0x700.
	peek "$4" 0xc0 "BBH_TX[$2] LAN Q2RNR(+0x400=word0)/QMQ(+0x4b0=word0x2c)"
	peek "$5" 0xc0 "BBH_TX[$2] UNIFIED Q2RNR(+0x700=word0)/QMQ(+0x7b0=word0x2c)"
}
#       base         #   rnrcfg(+50)  lan(+400)    uni(+700)
bbh_one 0x82890000 0 0x82890050 0x82890400 0x82890700
bbh_one 0x82892000 1 0x82892050 0x82892400 0x82892700
bbh_one 0x82894000 2 0x82894050 0x82894400 0x82894700
bbh_one 0x82896000 3 0x82896050 0x82896400 0x82896700

# (D)/(E) RDD FIRST_QUEUE_MAPPING in the TM cores' RNR_MEM SRAM (big-endian):
#   DS_TM = core7 @ 0x82700000 + 7*0x20000 + 0x2d1c = 0x827e2d1c
#   US_TM = core6 @ 0x82700000 + 6*0x20000 + 0x36bc = 0x827c36bc
# Cross-check the queue number the RUNNER_GRP covers against these.
peek 0x827e2d1c 0x20 "DS_TM(core7) FIRST_QUEUE_MAPPING (RDD 0x2d1c)"
peek 0x827c36bc 0x20 "US_TM(core6) FIRST_QUEUE_MAPPING (RDD 0x36bc)"

echo "================ done. Map values via spec 11 sec C/B. ================"
echo "Then run the open driver with, e.g.:"
echo "  insmod bcm4916-runner.ko route_a=1 route_a_grp=<G> route_a_queue=<Q> \\"
echo "    route_a_tm_bb_id=<BB> route_a_tm_task=<T> route_a_bbh_inst=<I>"
