#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# rdpa-trace-events.sh - kprobe the stock rdpa control plane via tracefs, no
# module build. Alternative to rdpa_trace.ko for a kernel built with
# CONFIG_KPROBE_EVENTS=y (+ CONFIG_KPROBES=y). Read-only: dynamic kprobes that
# only record function args; remove with `$0 off`. See rdpa_trace.c for the
# rationale + the no-connectivity-break safety notes.
#
# ★ NOTE: the SDK kernel has CONFIG_KPROBES=y but CONFIG_FTRACE is OFF, and
#   CONFIG_KPROBE_EVENTS needs the ftrace/tracing core - so on the current
#   kprobe kernel this tracefs path is probably UNAVAILABLE (no $T below).
#   Use rdpa_trace.ko (register_kprobe, needs only CONFIG_KPROBES) instead. This
#   script is the fallback for if/when CONFIG_FTRACE+KPROBE_EVENTS are enabled.
#
# Run ON THE DEVICE (kprobe-enabled kernel, stock rdpa loaded), as:
#   /bin/busybox sh rdpa-trace-events.sh on   [routea|offload|all]   # default all
#   /bin/busybox sh rdpa-trace-events.sh show                        # dump trace
#   /bin/busybox sh rdpa-trace-events.sh off                         # remove all
#
# arm64 4.19 kprobe_events arg syntax: register fetch %x0..%x7; struct field via
# +OFFSET(%xN):type. egress_queue is the 2nd arg of f_rdpa_cpu_tx_port_enet_lan
# (route_a_queue oracle). Struct-field derefs (+OFF) are filled from the RE map.

set -u
T=/sys/kernel/debug/tracing
[ -d "$T" ] || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -d "$T" ] || { echo "no tracefs at $T (need CONFIG_KPROBE_EVENTS)"; exit 1; }

KE="$T/kprobe_events"
what="${2:-all}"

add() {	# add <name> <symbol> <args...>
	# tolerate absent symbols (echo fails) - report and continue
	if printf 'p:%s %s %s\n' "$1" "$2" "$3" >> "$KE" 2>/dev/null; then
		echo "added $1 ($2)"
		echo 1 > "$T/events/kprobes/$1/enable" 2>/dev/null
	else
		echo "SKIP $1 ($2) - symbol absent or arg syntax rejected"
	fi
}

case "${1:-on}" in
on)
	echo > "$T/trace" 2>/dev/null
	if [ "$what" = routea ] || [ "$what" = all ]; then
		# Route A CPU_TX: the real worker is rdpa_cpu_send_pbuf (the f_rdpa_* /
		# enet_lan names are data ptrs / DSL-only -> not kprobe-able). ★LIVE-
		# CONFIRMED arg order is (pbuf, info): x0=pbuf, x1=info. queue_id is in
		# info @+24 (route_a_queue); pbuf.length @0x16.
		add cpu_pbuf rdpa_cpu_send_pbuf 'qid=+24(%x1):u32 len=+22(%x0):u16 fpmbn=+16(%x0):u32'
		add cpu_send rdpa_cpu_send_sysb 'sysb=%x0:x64 info=%x1:x64 qid=+24(%x1):u32'
		add qm_grp   ag_drv_qm_rnr_group_cfg_set \
			'idx=%x0:u32 start=+0(%x1):u16 end=+2(%x1):u16 bb=+11(%x1):u8 task=+12(%x1):u8 en=+13(%x1):u8'
		add bbh_qmq  ag_drv_bbh_tx_unified_configurations_qmq_set 'bbh=%x0:u8 idx=%x1:u8 q0=%x2:u8 q1=%x3:u8'
	fi
	if [ "$what" = offload ] || [ "$what" = all ]; then
		# Offload control plane (symbols verified vs rdpa.ko nm).
		add ucast_add ucast_attr_flow_add 'mo=%x0:x64 flow=%x2:x64'
		add natc_add  drv_natc_key_result_entry_var_size_ctx_add \
			'tbl=%x0:u8 key=%x2:x64 ctx=%x3:x64 idxp=%x4:x64'
		add cmdlist   rdpa_cmd_list_update_context 'params=%x0:x64 len=+28(%x0):u32'
		add rdd_add   rdd_connection_entry_add 'a0=%x0:x64 a1=%x1:x64'
		# struct-buffer bytes (natc key/ctx, cmdlist) are easier via rdpa_trace.ko
		# (probe_kernel_read hexdumps); kprobe_events scalars shown here.
	fi
	echo "enabled. generate traffic, then: $0 show"
	;;
show)
	cat "$T/trace"
	;;
off)
	# disable + clear all our kprobes
	for e in cpu_pbuf cpu_send qm_grp bbh_qmq ucast_add natc_add cmdlist rdd_add; do
		echo 0 > "$T/events/kprobes/$e/enable" 2>/dev/null
	done
	echo > "$KE" 2>/dev/null
	echo "removed all kprobe_events"
	;;
*)
	echo "usage: $0 {on [routea|offload|all]|show|off}"; exit 1;;
esac
