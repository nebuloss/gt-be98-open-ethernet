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
#define MAX_DEREF 4
/* bytes to dump per struct deref (clamped) */
#define DEREF_MAX 64

struct deref_spec {
	int  arg;		/* which arg register holds the base pointer (0..7) */
	int  off;		/* byte offset into the pointed-at struct */
	int  len;		/* bytes to read (<= DEREF_MAX) */
	bool chase;		/* if set: read a pointer at (arg+off), dump len from *that*
				 * (for a buffer reached via a pointer field, e.g. cmdlist) */
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
	/* ---- GRP_ROUTEA: CPU_TX egress (per-packet; THE egress_queue oracle) ----
	 * NB: the f_rdpa_cpu_tx_* / rdpa_cpu_tx_port_enet_lan names are DATA pointers
	 * or DSL-only -> NOT kprobe-able. The real kprobe-able worker is
	 * rdpa_cpu_send_pbuf (in rdpa.ko). Symbols/offsets verified vs rdpa.ko nm +
	 * rdpa_gpl/include headers (RE agent acff6c80). */
	{
		/* rdpa_cpu_send_pbuf - last kprobe-able fn before the (static-inline)
		 * ring-desc writer. ★LIVE-CONFIRMED 2026-06-24 the arg order is
		 * (pbuf_t *pbuf, const rdpa_cpu_tx_info_t *info) - i.e. x0=pbuf,
		 * x1=info (the RE's cross-tree (info,pbuf) was reversed; verified by
		 * x1 matching the info ptr rdpa_cpu_send_sysb passes). info->queue_id
		 * @ +24 = egress QM queue; pbuf: fpm_bn@0x10, len u16@0x16, flags@0x18. */
		.sym = "rdpa_cpu_send_pbuf", .group = GRP_ROUTEA, .nregs = 2,
		.desc = "CPU_TX worker: x0=pbuf(len@0x16), x1=info(queue_id@+24)",
		.deref = {
			{ .arg = 1, .off = 24,   .len = 8,  .label = "info.queue_id/wanflow@+24" },
			{ .arg = 1, .off = 8,    .len = 8,  .label = "info.port_obj@+8" },
			{ .arg = 0, .off = 0x10, .len = 10, .label = "pbuf.fpm_bn/off/len/flags@0x10" },
		},
	},
	{
		/* rdpa_cpu_send_sysb(bdmf_sysb sysb, const rdpa_cpu_tx_info_t *info) -
		 * generic CPU_TX entry trampoline; x1=info. [rdpa_gpl EXPORT_SYMBOL] */
		.sym = "rdpa_cpu_send_sysb", .group = GRP_ROUTEA, .nregs = 2,
		.desc = "CPU_TX entry: x0=sysb, x1=rdpa_cpu_tx_info*",
		.deref = {
			{ .arg = 1, .off = 24, .len = 8, .label = "info.queue_id@+24" },
			{ .arg = 1, .off = 8,  .len = 8, .label = "info.port_obj@+8" },
		},
	},
	{
		/* rdpa_cpu_tx_port_enet_or_dsl_wan(sysb, egress_queue, wanFlow,
		 * port_obj, extra) - WAN inject trampoline; w1=egress_queue directly.
		 * (The enet_lan equivalent is a data ptr -> not kprobe-able.) */
		.sym = "rdpa_cpu_tx_port_enet_or_dsl_wan", .group = GRP_ROUTEA, .nregs = 5,
		.desc = "CPU_TX WAN: w1=egress_queue w2=wanFlow x3=port_obj",
	},
	{
		/* ag_drv_qm_rnr_group_cfg_set(uint8_t rnr_idx, const qm_rnr_group_cfg*)
		 * boot-time; x0=rnr_idx (route_a_grp), x1=cfg. struct (verified): u16
		 * start_queue@0, end_queue@2, pd_fifo_base@4, pd_fifo_size@6,
		 * upd_fifo_base@8, upd_fifo_size@10, rnr_bb_id@11, rnr_task@12,
		 * rnr_enable@13. Caught only if armed at boot - else route-a-oracle.sh. */
		.sym = "ag_drv_qm_rnr_group_cfg_set", .group = GRP_ROUTEA, .nregs = 2,
		.desc = "QM RUNNER_GRP set: x0=rnr_idx, x1=qm_rnr_group_cfg(14B)",
		.deref = {
			{ .arg = 1, .off = 0, .len = 14, .label = "qm_rnr_group_cfg{start,end,fifos,bb_id@11,task@12,en@13}" },
		},
	},
	{	/* BBH_TX QM-fed queue -> runner mapping (boot): w0=bbh_id w1=idx w2=q0 w3=q1 */
		.sym = "ag_drv_bbh_tx_unified_configurations_q2rnr_set", .group = GRP_ROUTEA,
		.nregs = 4, .desc = "BBH_TX Q2RNR: w0=bbh_id w1=idx w2=q0 w3=q1",
	},
	{	/* BBH_TX enable QM queue (boot): w0=bbh_id w1=idx w2=q0 w3=q1 */
		.sym = "ag_drv_bbh_tx_unified_configurations_qmq_set", .group = GRP_ROUTEA,
		.nregs = 4, .desc = "BBH_TX QMQ: w0=bbh_id w1=qm_q_idx w2=q0 w3=q1",
	},

	/* ---- GRP_OFFLOAD: flow-add / NAT-C / cmdlist (verified vs rdpa.ko nm) ---- */
	{
		/* ucast_attr_flow_add(mo, bdmf_index *index, const rdpa_ip_flow_info_t*)
		 * x2=flow. struct: hw_flow_id u32@0, then rdpa_ip_flow_key_t (bdmf_ip_t
		 * src/dst are 20B each!). Dump raw 64B and decode offline vs .config. */
		.sym = "ucast_attr_flow_add", .group = GRP_OFFLOAD, .nregs = 3,
		.desc = "FC_UCAST flow-add: x2=rdpa_ip_flow_info_t* (5-tuple+result)",
		.deref = { { .arg = 2, .off = 0, .len = 64, .label = "rdpa_ip_flow_info(raw)" } },
	},
	{	/* l2_ucast_attr_flow_add(mo, index, const rdpa_l2_flow_info_t*) */
		.sym = "l2_ucast_attr_flow_add", .group = GRP_OFFLOAD, .nregs = 3,
		.desc = "L2 flow-add: x2=rdpa_l2_flow_info_t* (MAC+VLAN)",
		.deref = { { .arg = 2, .off = 0, .len = 64, .label = "rdpa_l2_flow_info(raw)" } },
	},
	{
		/* drv_natc_key_result_entry_var_size_ctx_add(tbl, hash_key, key,
		 * result, *entry_idx): w0=tbl x1=hash_key x2=key(16B) x3=result(ctx,124B
		 * w/ cmdlist@24). THE NAT-C add. [arg count cross-tree - confirm live] */
		.sym = "drv_natc_key_result_entry_var_size_ctx_add", .group = GRP_OFFLOAD,
		.nregs = 5, .desc = "NAT-C add: x2=key(16B) x3=result/ctx x4=entry_idx*",
		.deref = {
			{ .arg = 2, .off = 0, .len = 16, .label = "natc_key(16B BE)" },
			{ .arg = 3, .off = 0, .len = 64, .label = "fc_ucast_ctx(raw, cmdlist@24)" },
		},
	},
	{
		/* rdpa_cmd_list_update_context(cmd_list_update_params_t *p, int *ovf):
		 * p->rdpa_cmd_list_p @0x10 (ptr to bytes), len @0x1c, data_len @0x20,
		 * final_len_32 @0x2c (OUT). CHASE the buffer pointer. */
		.sym = "rdpa_cmd_list_update_context", .group = GRP_OFFLOAD, .nregs = 2,
		.desc = "cmdlist compile: x0=params (buf@0x10 chase, len@0x1c)",
		.deref = {
			{ .arg = 0, .off = 0x10, .len = 64, .chase = true, .label = "cmdlist bytes(*p+0x10)" },
			{ .arg = 0, .off = 0x1c, .len = 8,  .label = "cmdlist len@0x1c / data_len@0x20" },
		},
	},
	{	/* rdd_connection_entry_add - final RDD commit; rdd_fc_context_t internal
		 * offsets are proprietary -> dump regs + a best-effort raw blob, decode
		 * offline. (arg layout unconfirmed; raw regs are the safety net.) */
		.sym = "rdd_connection_entry_add", .group = GRP_OFFLOAD, .nregs = 4,
		.desc = "RDD commit: dump regs + raw x0 blob (rdd_fc_context_t)",
		.deref = { { .arg = 0, .off = 0, .len = 64, .label = "x0 blob(raw)" } },
	},
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
		if (d->chase) {
			/* read a pointer at (arg+off), then dump from *that* */
			unsigned long p2;

			if (probe_kernel_read(&p2, (void *)ptr, sizeof(p2)) || !p2) {
				pr_emerg(RDPATRACE ":   %s = <chase fault @%px>\n",
					 d->label ? d->label : "deref", (void *)ptr);
				continue;
			}
			ptr = p2;
		}
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
