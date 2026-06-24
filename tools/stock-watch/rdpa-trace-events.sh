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
		# Route A: per-packet CPU_TX egress_queue (x1) + port (x2); QM grp set.
		add lan_tx   f_rdpa_cpu_tx_port_enet_lan 'egq=%x1:u32 port=%x2:x64'
		add cpu_send rdpa_cpu_send_sysb          'sysb=%x0:x64 info=%x1:x64'
		add qm_grp   ag_drv_qm_rnr_group_cfg_set 'rnr_idx=%x0:u32 cfg=%x1:x64'
		# TODO: + start=+0(%x1):u32 end=+4(%x1):u32 bb=+8(%x1):u32 task=... once
		#       the qm_rnr_group_cfg struct offsets are pinned from the RE map.
	fi
	if [ "$what" = offload ] || [ "$what" = all ]; then
		# Offload control plane (symbols/offsets filled from the RE agent map).
		add fc_off f_rdpa_cpu_tx_flow_cache_offload 'sysb=%x0:x64 rxq=%x1:u32 dirty=%x2:u32'
		# TODO(offload): flow create/add, drv_natc_*_ctx_add (key+ctx derefs),
		#                cmdlist builder - add p: lines from the RE map.
	fi
	echo "enabled. generate traffic, then: $0 show"
	;;
show)
	cat "$T/trace"
	;;
off)
	# disable + clear all our kprobes
	for e in lan_tx cpu_send qm_grp fc_off; do
		echo 0 > "$T/events/kprobes/$e/enable" 2>/dev/null
	done
	echo > "$KE" 2>/dev/null
	echo "removed all kprobe_events"
	;;
*)
	echo "usage: $0 {on [routea|offload|all]|show|off}"; exit 1;;
esac
