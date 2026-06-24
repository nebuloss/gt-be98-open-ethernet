// SPDX-License-Identifier: GPL-2.0-only
/*
 * rdpa_trace.c - read-only kprobe tracer for the LIVE stock rdpa stack, to learn
 * the proprietary control plane (Route A QM/TM egress + the HW flow-offload path)
 * for the open BCM4916 (XRDP/Runner) Ethernet driver effort.
 *
 * WHY KPROBES (vs the read-only xrdp_peek.ko / natc_dump.ko):
 *   xrdp_peek/natc_dump read register/DDR STATE. This captures stock FUNCTION
 *   CALLS + their ARGUMENTS as they happen - the rdpa.ko-only values a static
 *   snapshot can't get: the per-packet CPU_TX egress_queue, the flow-add /
 *   NAT-C / cmdlist programming args. rdpa ships as MODULES, so its symbols are
 *   in kallsyms once loaded and kprobe-able by name. (Needs a kernel built with
 *   CONFIG_KPROBES=y - the stock kernel has it OFF; use the kprobe-enabled
 *   kernel.) For a kernel with CONFIG_KPROBE_EVENTS, rdpa-trace-events.sh does
 *   the same via tracefs with no module build.
 *
 * SAFETY (this traces the LIVE datapath - see no-connectivity-break rule):
 *   - pre-handlers ONLY, no jprobes/kretprobe-on-hot-path, no writes anywhere.
 *   - struct fields read with probe_kernel_read() (bad ptr -> -EFAULT, no oops).
 *   - PER-PROBE HIT CAP (max_hits, default 16): hot paths (per-packet CPU_TX)
 *     would flood dmesg + add latency; after the cap the handler returns
 *     immediately without printing. Nothing is patched; rmmod unregisters all.
 *   - arm a single group at a time (grp=) to minimise datapath perturbation.
 *
 * BUILD: vermagic-exact 4.19 recipe, same as natc_dump/xrdp_peek (see Makefile).
 * LOAD examples (grep dmesg for "RDPATRACE"):
 *   insmod rdpa_trace.ko grp=1                 # Route A (CPU_TX egress + QM)
 *   insmod rdpa_trace.ko grp=2 max_hits=32     # offload control plane
 *   insmod rdpa_trace.ko grp=3                 # both
 * Then generate traffic (ping a LAN host / establish a flow) and read dmesg.
 *
 * The probe TABLE (symbols + arg/struct offsets) is filled from SDK RE; entries
 * marked TODO are pending the offload-path symbol map - safe to load meanwhile
 * (an absent symbol is skipped with a warning, never fatal).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/version.h>

#define RDPATRACE "RDPATRACE"

/* probe groups (bitmask via grp=) */
#define GRP_ROUTEA   0x1   /* CPU_TX egress + QM/BBH egress setup */
#define GRP_OFFLOAD  0x2   /* flow-add / NAT-C / cmdlist programming */

static int grp = GRP_ROUTEA;
module_param(grp, int, 0444);
MODULE_PARM_DESC(grp, "bitmask of probe groups to arm: 1=RouteA, 2=offload, 3=both");

static int max_hits = 16;
module_param(max_hits, int, 0444);
MODULE_PARM_DESC(max_hits, "per-probe print cap (hot paths flood dmesg); 0 = unlimited");

/* up to this many struct-deref dumps per probe entry */
#define MAX_DEREF 3
/* bytes to dump per struct deref (clamped) */
#define DEREF_MAX 64

struct deref_spec {
	int  arg;		/* which arg register holds the pointer (0..7) */
	int  off;		/* byte offset into the pointed-at struct */
	int  len;		/* bytes to read (<= DEREF_MAX) */
	const char *label;
};

struct trace_probe {
	const char *sym;	/* kallsyms function name */
	int   group;
	int   nregs;		/* how many arg regs x0..x{nregs-1} to print */
	const char *desc;	/* what this reveals */
	struct deref_spec deref[MAX_DEREF];
	/* runtime */
	struct kprobe kp;
	atomic_t hits;
	bool armed;
};

/*
 * ===================== PROBE TABLE =====================
 * arm64: args in x0..x7 = regs->regs[0..7]. Struct-by-value >16B is by hidden
 * pointer. Offsets below come from the SDK structs (cite in comments). Entries
 * with TODO offsets still print the arg registers (always useful); fill the
 * deref specs from the RE symbol map before the live run.
 */
static struct trace_probe probes[] = {
	/* ---- GRP_ROUTEA: CPU_TX egress (per-packet; THE egress_queue oracle) ---- */
	{
		/* f_rdpa_cpu_tx_port_enet_lan(sysb, egress_queue, port_obj, extra_info)
		 * [rdpa_gpl_handwritten.c EXPORT_SYMBOL]. x1 = egress_queue = the LAN
		 * QM queue number (route_a_queue!); x2 = port_obj. */
		.sym = "f_rdpa_cpu_tx_port_enet_lan", .group = GRP_ROUTEA, .nregs = 4,
		.desc = "CPU_TX LAN: x1=egress_queue (route_a_queue), x2=port_obj",
	},
	{
		/* rdpa_cpu_send_sysb(sysb, info) - the generic CPU_TX entry; x1=info
		 * points at rdpa_cpu_tx_info (port/queue/method). [EXPORT_SYMBOL] */
		.sym = "rdpa_cpu_send_sysb", .group = GRP_ROUTEA, .nregs = 2,
		.desc = "CPU_TX entry: x0=sysb, x1=rdpa_cpu_tx_info*",
		/* TODO: deref x1 -> {port, queue_id, method} offsets from rdpa_cpu_tx_info */
	},
	{
		/* ag_drv_qm_rnr_group_cfg_set(rnr_idx, qm_rnr_group_cfg *cfg)
		 * boot-time; x0=rnr_idx (route_a_grp), x1=cfg* (queue range + TM
		 * bb_id/task). Caught only if armed at boot - otherwise read the regs
		 * statically (route-a-oracle.sh). */
		.sym = "ag_drv_qm_rnr_group_cfg_set", .group = GRP_ROUTEA, .nregs = 2,
		.desc = "QM RUNNER_GRP set: x0=rnr_idx, x1=qm_rnr_group_cfg*",
		/* TODO: deref x1 -> {start_queue, end_queue, rnr_bb_id, rnr_task} */
	},

	/* ---- GRP_OFFLOAD: flow-add / NAT-C / cmdlist (fill from RE agent map) ---- */
	{
		/* f_rdpa_cpu_tx_flow_cache_offload(sysb, cpu_rx_queue, dirty)
		 * [EXPORT_SYMBOL] - the flow-cache offload inject path. */
		.sym = "f_rdpa_cpu_tx_flow_cache_offload", .group = GRP_OFFLOAD, .nregs = 3,
		.desc = "flow-cache offload: x0=sysb x1=cpu_rx_queue x2=dirty",
	},
	/* TODO(offload): flow create/add, drv_natc_*_ctx_add, cmdlist builder -
	 * symbols + arg/struct offsets pending the offload RE agent map. */
};

static int trace_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct trace_probe *tp = container_of(p, struct trace_probe, kp);
	int hits, i;

	hits = atomic_inc_return(&tp->hits);
	if (max_hits && hits > max_hits)
		return 0;	/* capped: stay cheap, no print */

	pr_emerg(RDPATRACE ": %s [#%d] x0=%px x1=%px x2=%px x3=%px x4=%px\n",
		 tp->sym, hits,
		 (void *)regs->regs[0], (void *)regs->regs[1],
		 (void *)regs->regs[2], (void *)regs->regs[3],
		 (void *)regs->regs[4]);

	for (i = 0; i < MAX_DEREF; i++) {
		struct deref_spec *d = &tp->deref[i];
		unsigned char buf[DEREF_MAX];
		unsigned long ptr;
		int len, j;
		char hex[3 * DEREF_MAX + 1];

		if (!d->len)
			continue;
		len = d->len > DEREF_MAX ? DEREF_MAX : d->len;
		ptr = regs->regs[d->arg] + d->off;
		if (probe_kernel_read(buf, (void *)ptr, len)) {
			pr_emerg(RDPATRACE ":   %s = <fault @%px>\n",
				 d->label ? d->label : "deref", (void *)ptr);
			continue;
		}
		for (j = 0; j < len; j++)
			snprintf(hex + 3 * j, 4, "%02x ", buf[j]);
		pr_emerg(RDPATRACE ":   %s (x%d+0x%x, %dB) = %s\n",
			 d->label ? d->label : "deref", d->arg, d->off, len, hex);
	}
	return 0;
}

static int __init rdpa_trace_init(void)
{
	int i, armed = 0;

	pr_emerg(RDPATRACE ": init grp=0x%x max_hits=%d\n", grp, max_hits);
	for (i = 0; i < ARRAY_SIZE(probes); i++) {
		struct trace_probe *tp = &probes[i];
		int ret;

		if (!(tp->group & grp))
			continue;
		atomic_set(&tp->hits, 0);
		tp->kp.symbol_name = tp->sym;
		tp->kp.pre_handler = trace_pre;
		ret = register_kprobe(&tp->kp);
		if (ret) {
			pr_warn(RDPATRACE ": skip %s (register_kprobe=%d - symbol absent?)\n",
				tp->sym, ret);
			continue;
		}
		tp->armed = true;
		armed++;
		pr_emerg(RDPATRACE ": armed %s @%px (%s)\n",
			 tp->sym, tp->kp.addr, tp->desc);
	}
	pr_emerg(RDPATRACE ": %d probe(s) armed. Generate traffic, then read dmesg.\n",
		 armed);
	return 0;	/* stay loaded even if 0 armed (informational) */
}

static void __exit rdpa_trace_exit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(probes); i++) {
		struct trace_probe *tp = &probes[i];

		if (tp->armed) {
			unregister_kprobe(&tp->kp);
			pr_emerg(RDPATRACE ": %s hits=%d\n", tp->sym,
				 atomic_read(&tp->hits));
			tp->armed = false;
		}
	}
	pr_emerg(RDPATRACE ": unloaded\n");
}

module_init(rdpa_trace_init);
module_exit(rdpa_trace_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only kprobe tracer for the stock rdpa control plane (open BCM4916 RE)");
