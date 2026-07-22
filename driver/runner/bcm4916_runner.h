/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bcm4916_runner.h - register map, descriptor layout and FPM token math for the
 * open BCM4916 (BCM6813) XRDP slow-path CPU-conduit datapath driver.
 *
 * All register bases/offsets are pinned in re-notes/xrdp-datapath-abi.md
 * (sections cited inline as "ABI 1.1", "ABI 2.x", "ABI 3.x"). The descriptor
 * and FPM-token bitfields are ported from the GPL Broadcom SDK:
 *   - FPM token format / register struct:
 *       asuswrt-merlin.ng .../bcmdrivers/opensource/char/fpm/impl1/fpm_priv.h
 *   - CPU RX/FEED/RECYCLE/TX descriptor layout (BCM6813/XRDP, authoritative):
 *       src-rt-5.04behnd.4916/rdp/projects/XRDP_CFE2/drivers/rdp_subsystem/cpu/
 *         rdp_cpu_ring_defs.h          (CPU_RX_DESCRIPTOR, LE variant)
 *       src-rt-5.04behnd.4916/rdp/drivers/rdpa_gpl/include/rdpa_cpu_helper.h
 *         (rdpa_cpu_rx_pd_get / rdpa_cpu_ring_rest_desc parse + refill)
 *       src-rt-5.04behnd.4916/rdp/drivers/rdp_subsystem/xrdp/rdp_cpu_ring_defs.h
 *         (CPU_FEED_DESCRIPTOR)
 *       src-rt-5.04behnd.4916/rdp/projects/BCM6813/target/rdd/
 *         rdd_data_structures_auto.h  (RING_CPU_TX_DESCRIPTOR, CPU_RECYCLE_DESCRIPTOR)
 *     All cross-validated against LIVE silicon descriptors captured via the stock
 *     bdmf shell ("bs /Driver/cpUr/Vrpd"). The earlier 416L05 (63138/63148) layout
 *     was the WRONG XRDP generation and is replaced; see the per-field notes below.
 *
 * Runner is BIG-ENDIAN; the ARM host is little-endian, so every 32-bit
 * descriptor word is byte-swapped (swap4) before/after the host touches it.
 */
#ifndef _BCM4916_RUNNER_H_
#define _BCM4916_RUNNER_H_

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/io.h>

/* ----------------------------------------------------------------------------
 * XRDP block bases, relative to the rdpa window base (0x82000000). ABI 1.1.
 * The driver ioremaps the whole window and indexes blocks by these offsets.
 * -------------------------------------------------------------------------- */
#define XRDP_WINDOW_BASE	0x82000000UL
#define XRDP_WINDOW_SIZE	0x00caf004UL

#define XRDP_OFF_PSRAM		0x00000000UL	/* PSRAM (NAT-C staging etc.) */
/* [PINNED 2026-06-22 vs BCM6813's OWN rdpa.ko RNR_MEM_ADDRS[] = 0x82700000 base, stride 0x20000,
 * 8 cores (per-core: MEM+0, INST+0x10000, PRED+0x1c000, CNTXT+0x18000). The BCM6837/6888 autogen
 * proxy (0x82600000) is WRONG for 6813 by +0x100000 — caught by adversarial binary verification.
 * The CPU RX/TX ring descriptor tables live in per-core RNR data memory, NOT PSRAM. */
#define XRDP_OFF_RNR_MEM0	0x00700000UL	/* per-core data SRAM base [rdpa.ko RNR_MEM_ADDRS] */
#define XRDP_RNR_MEM_STRIDE	0x00020000UL
#define XRDP_RNR_CORES		8
/* CPU host rings live on specific Runner cores per the RDD ADDRESS_ARR:
 * RX descriptors core 3, TX descriptors core 2 [SDK BCM6813_FPI rdd_data_structures_auto.c]. */
#define CPU_RX_RING_CORE	3
#define CPU_TX_RING_CORE	2
#define XRDP_OFF_RNR_REGS0	0x00800000UL	/* per-core ctl, stride 0x1000 */
#define XRDP_RNR_REGS_STRIDE	0x00001000UL
#define XRDP_RNR_INST_OFF	0x00010000UL	/* core_base + this = inst SRAM */
#define XRDP_RNR_PRED_OFF	0x0001c000UL	/* core_base + this = pred RAM */
#define XRDP_RNR_CNTXT_OFF	0x00018000UL	/* core_base + this = per-thread context regfile SRAM
						 * [RE rdpa.o RNR_CNTXT_ADDRS: core0=0x82718000,
						 * stride 0x20000; == rnr_mem[core]+0x18000].
						 * thread T reg R -> +T*128 + R*4, big-endian. */
#define XRDP_OFF_DSPTCHR	0x00880000UL
#define XRDP_OFF_SBPM		0x008a1000UL
#define XRDP_OFF_UBUS_SLV	0x008a0000UL
#define XRDP_OFF_DMA0		0x008a1800UL	/* 3 engines, stride 0x400 */
#define XRDP_DMA_STRIDE		0x00000400UL
#define XRDP_OFF_BBH_RX0	0x00898000UL	/* 12 ports, stride 0x400 */
#define XRDP_BBH_RX_STRIDE	0x00000400UL
#define XRDP_OFF_BBH_TX0	0x00890000UL	/* 4, stride 0x2000 */
#define XRDP_BBH_TX_STRIDE	0x00002000UL
/* XLIF0 = the XLMAC<->Runner interface for the 10G ports (abs 0x828b2000).
 * [RE rdpa.o XLIF0_{RX,TX}_IF_ADDRS + XRDP_AG.h *_REG_OFFSET]: 4 channels,
 * stride 0x200; RX_IF sub-block @ chan+0x00, TX_IF sub-block @ chan+0x40.
 * eth0 = channel 0, eth1 = channel 2 (channel == XLMAC mac index). */
#define XRDP_OFF_XLIF0		0x008b2000UL	/* XLIF0 ch0 RX_IF base */
#define XLIF0_CHANNEL_STRIDE	0x00000200UL
#define XLIF0_RX_IF_OFF		0x00000000UL	/* RX_IF sub-block within a channel */
#define   XLIF_RX_IF_IF_DIS	0x00000000UL	/*   bit0 = RX disable */
#define XLIF0_TX_IF_OFF		0x00000040UL	/* TX_IF sub-block within a channel */
#define   XLIF_TX_IF_IF_ENABLE	0x00000000UL	/*   bit0 dis_w_credits, bit1 dis_wo_credits */
#define   XLIF_TX_IF_URUN_PORT_EN 0x00000010UL	/*   bit0 = underrun port enable */
#define   XLIF_TX_IF_TX_THRESHOLD 0x00000014UL	/*   bits[3:0] = credit threshold (=12) */
#define XRDP_OFF_UNIMAC_RDP0	0x008a8004UL	/* 4, stride 0x1000 */
#define XRDP_OFF_FPM		0x00a00000UL
#define XRDP_OFF_QM		0x00c00000UL
#define XRDP_OFF_DQM		0x00c80034UL	/* [rdpa.ko DQM_ADDRS[0]=0x82c80034 — stock accessor base; revert] */
/*
 * QM block (base 0x82c00000 per our UBUS decode dev1). GLOBAL_CFG_QM_ENABLE_CTRL
 * @ +0x0: FPM_PREFETCH[0]|REORDER_CREDIT[1]|DQM_POP[2]|RMT_FIXED_ARB[8]|
 * DQM_PUSH_FIXED_ARB[9]. The CPU_TX EGRESS delayed VIQ's credit RELEASE+wakeup is
 * gated by DSPTCHR EGRS_DLY_QM_CRDT, fed only when REORDER_CREDIT (bit1) is set.
 * Stock slot2 reads 0x307 here (verified live via devmem). [XRDP_QM_AG.h:1535-1584]
 */
#define XRDP_OFF_QM		0x00c00000UL	/* QM block base (rel rdpa window) */
#define QM_GLOBAL_QM_ENABLE_CTRL 0x000		/* GLOBAL_CFG_QM_ENABLE_CTRL */
#define QM_ENABLE_CTRL_STOCK	0x307		/* full stock enable (matches silicon) */

/* ----------------------------------------------------------------------------
 * Route A (opt-in, module param route_a=1): QM + TM-core egress so an injected
 * CPU_TX PD actually reaches BBH_TX. The stock image_2 CPU_TX thread routes PDs
 * through the egress dispatcher to a TM/QM Runner core, NOT directly to BBH_TX
 * (that direct path is CFE2/image_0 only) - so the open slow-path TX freezes at
 * read_idx=3 until we (1) init the QM incl. the gen-2 SRAM auto-init that makes
 * ENABLE_CTRL=0x307 safe, (2) bind a QM queue range to the TM core via a
 * RUNNER_GRP, (3) set BBH_TX to take that queue from the QM aggregator (QMQ=1),
 * and (4) mark the CPU_TX descriptor is_egress + target queue.
 *
 * Offsets + field bits VERIFIED vs BCM6813/autogen/XRDP_AG.h. Values marked
 * ★SILICON live only in rdpa.ko and are exposed as module params, to be pinned
 * by devmem oracle capture on the live stock slot2 (the technique that pinned
 * DSPTCHR VIQ 13). Full spec: re-notes/realhw/11-route-a-egress-spec.md.
 * -------------------------------------------------------------------------- */
#define QM_GLOBAL_SW_RST_CTRL		0x004
#define QM_GLOBAL_FPM_CONTROL		0x00c
#define QM_GLOBAL_AGGREGATION_CTRL	0x02c
#define QM_GLOBAL_FPM_BASE_ADDR		0x034	/* 256B-res phys of the RDP DDR pool */
#define QM_GLOBAL_FPM_COHERENT_BASE_ADDR 0x038
#define QM_GLOBAL_DDR_SOP_OFFSET	0x03c
#define QM_GLOBAL_DDR_SOP_OFFSET_VAL	18	/* gen-1 starting value */
/* gen-2 QM SRAM auto-init - MUST run before ENABLE_CTRL or fpm_prefetch reads
 * garbage and hangs the SoC (the prior 0x307 hang). [XRDP_AG.h:693/19115] */
#define QM_GLOBAL_MEM_AUTO_INIT		0x138
#define QM_GLOBAL_MEM_AUTO_INIT_EN	BIT(0)	/* MEM_INIT_EN */
#define QM_GLOBAL_MEM_AUTO_INIT_STS	0x13c
#define QM_GLOBAL_MEM_AUTO_INIT_DONE	BIT(0)	/* MEM_INIT_DONE [XRDP_AG.h:723] */
#define QM_FPM_POOLS_THR		0x200
/* RUNNER_GRP: 15 groups (REG_RAM_CNT 0xf), stride 0x10, 4 regs/group.
 * [XRDP_AG.h:19157-19173] */
#define QM_RUNNER_GRP_BASE		0x300
#define QM_RUNNER_GRP_STRIDE		0x10
#define QM_RUNNER_GRP_RNR_CONFIG	0x00	/* RNR_BB_ID[5:0] RNR_TASK[11:8] EN[16] */
#define QM_RUNNER_GRP_QUEUE_CONFIG	0x04	/* START_QUEUE[8:0] END_QUEUE[24:16] */
#define QM_RUNNER_GRP_PDFIFO_CONFIG	0x08
#define QM_RUNNER_GRP_UPDATE_FIFO_CONFIG 0x0c
#define QM_RNR_CFG_RNR_BB_ID_MASK	0x3f	/* [5:0] */
#define QM_RNR_CFG_RNR_TASK_SHIFT	8	/* [11:8] */
#define QM_RNR_CFG_RNR_TASK_MASK	0xf
#define QM_RNR_CFG_RNR_ENABLE		BIT(16)
#define QM_QUEUE_CFG_START_SHIFT	0	/* [8:0] */
#define QM_QUEUE_CFG_END_SHIFT		16	/* [24:16] */
#define QM_QUEUE_CFG_QUEUE_MASK		0x1ff	/* 9-bit queue id */
/* per-group register address helper (relative to the QM block base) */
#define QM_RUNNER_GRP_REG(grp, reg) \
	(QM_RUNNER_GRP_BASE + (u32)(grp) * QM_RUNNER_GRP_STRIDE + (reg))

/* ---- per-queue QM context (spec 11 sec C: qm_q_context_set + queue enable) --
 * Without these a queue keeps its init-default WRED profile 15 = DROP-ALL and
 * stays disabled, so every enqueued PD is silently discarded (observed:
 * CPU_TX accepted but XLMAC gtxpkt=0 with zero errors on every counter).
 * ⚠ Field layout is 6813-SPECIFIC — BCM6888 shifts FPM_UG/EXCL differently.
 * [BCM6813 XRDP_AG.h QM_QUEUE_CONTEXT_CONTEXT_* + ru_reg_rec RAM steps] */
#define QM_QUEUE_CONTEXT	0x800		/* RAM, 160 queues, step 4 */
#define QM_QUEUE_CONTEXT_STEP	4
#define   QM_QCTX_WRED_PROFILE_SHIFT	0	/* [3:0]  15 = drop-all default */
#define   QM_QCTX_WRED_PROFILE_MASK	0xf
#define   QM_QCTX_COPY_DEC_PROFILE_SHIFT 4	/* [6:4] */
#define   QM_QCTX_DDR_COPY_DISABLE	BIT(8)
#define   QM_QCTX_AGGREGATION_DISABLE	BIT(9)
#define   QM_QCTX_FPM_UG_SHIFT		10	/* [12:10] */
#define   QM_QCTX_FPM_UG_MASK		0x7
#define   QM_QCTX_EXCLUSIVE_PRIORITY	BIT(13)
#define   QM_QCTX_RES_PROFILE_SHIFT	17	/* [19:17] */
/* WRED profile RAM: 16 profiles, step 48; thresholds are 24-bit */
#define QM_WRED_PROFILE_BASE	0x1000
#define QM_WRED_PROFILE_STEP	48
#define   QM_WRED_MIN_THR_0	0x00		/* MIN_THR [23:0], FLW_CTRL_EN b24 */
#define   QM_WRED_MIN_THR_1	0x04
#define   QM_WRED_MAX_THR_0	0x10		/* MAX_THR [23:0] */
#define   QM_WRED_MAX_THR_1	0x14
#define   QM_WRED_THR_MAX	0x00ffffffu	/* max threshold = never drop */
#define QM_WRED_PROFILE_PASS	0		/* the profile we program to pass */
/* DQM per-queue output-logic enable (block base XRDP_OFF_DQM) */
#define DQM_DQMOL_CFGB		0x1fd4		/* RAM, 160 queues, step 32 */
#define DQM_DQMOL_CFGB_STEP	32
#define   DQM_DQMOL_CFGB_ENABLE	BIT(31)

/* ----------------------------------------------------------------------------
 * RNR_REGS per-core control block (base XRDP_OFF_RNR_REGS0 + core*stride).
 * Offsets/values pinned vs BCM6813 rdpa.ko + CFE2 data_path_init / rdp_drv_rnr.c
 * (re-notes/realhw/10-runner-bringup-spec.md secs 1, Wave-3/6). Bring-up step 2.
 * -------------------------------------------------------------------------- */
#define RNR_CFG_GLOBAL_CTRL	0x00	/* EN b0 (core run enable) */
#define RNR_CFG_GLOBAL_CTRL_EN	BIT(0)
#define RNR_CFG_CPU_WAKEUP	0x04	/* THREAD_NUM[3:0]; the WRITE starts that thread */
#define RNR_CFG_CPU_WAKEUP_THREAD_MASK	0xf
#define RNR_CFG_GEN_CFG		0x30	/* trigger data/context-mem zeroing */
#define RNR_CFG_GEN_CFG_DIS_DMA_OLD_FC	BIT(0)
#define RNR_CFG_GEN_CFG_ZERO_DATA_MEM	BIT(2)
#define RNR_CFG_GEN_CFG_ZERO_CTX_MEM	BIT(3)
/* [VERIFIED vs data_path_init.c:631-647 / XRDP_AG.h: completion is NOT a
 * self-clear of the zero bits (that poll is #if 0'd); poll these DONE bits=1. ] */
#define RNR_CFG_GEN_CFG_ZERO_DATA_DONE	BIT(4)
#define RNR_CFG_GEN_CFG_ZERO_CTX_DONE	BIT(5)
/* CFG_DDR_CFG: DMA_BASE[19:0] = (phys_hi<<12)|(phys_lo>>20); BUF_SIZE_MODE b23.
 * [VERIFIED vs data_path_init.c:654-657 + rdp_drv_rnr.c:62 (mode arg = 1).] */
/* CFG_DDR_CFG: DMA_BASE[19:0] | DMA_BUF_SIZE[22:20] | DMA_BUF_SIZE_MODE[23] |
 * DMA_STATIC_OFFSET[31:24]. ★Live stock core2 reads 0x004006d0 = BUF_SIZE 4,
 * MODE 0. We were writing BUF_SIZE 0 with MODE 1, i.e. telling the Runner a
 * different DDR buffer geometry than the one its DMA actually uses. */
#define RNR_CFG_DDR_CFG		0x40
#define RNR_CFG_DDR_CFG_BASE_MASK	0x000fffff
#define RNR_CFG_DDR_CFG_BUF_SIZE_SHIFT	20		/* [22:20] */
#define RNR_CFG_DDR_CFG_BUF_SIZE_VAL	4		/* stock */
#define RNR_CFG_DDR_CFG_BUF_SIZE_MODE	BIT(23)		/* stock leaves this 0 */
#define RNR_CFG_PSRAM_CFG	0x44	/* DMA_BASE[19:0] = psram_base>>20 = 0x820 */
#define RNR_CFG_PSRAM_CFG_VAL	0x820
/* ★scheduler mode [2:0]: live stock core2 = 2; we were writing 4. */
#define RNR_CFG_SCH_CFG		0x4c
#define RNR_CFG_SCH_CFG_VAL	0x2
/* ★CFG_GEN_CFG upper bits live stock core2 has and we never set:
 * BBTX_TCAM_DEST_SEL b16 (BBH_TX destination select) + b17 + b22 = 0x00430000. */
#define RNR_CFG_GEN_CFG_STOCK_HI	0x00430000
/* ★CFG_EXT_ACC_CFG: ADDR_BASE[12:0] | ADDR_STEP_0[19:16] | ADDR_STEP_1[23:20] |
 * START_THREAD[27:24]. Live stock core2 = 0x08881000 (base 0x1000, steps 8/8,
 * start_thread 8); ours was 0 (external-access windows unconfigured). */
#define RNR_CFG_EXT_ACC_CFG	0x60
#define RNR_CFG_EXT_ACC_CFG_VAL	0x08881000
/* CPU-host threads (rdpa runtime, core->image identity): RX core3/thr1, TX core2/thr6. */
#define RNR_CPU_RX_THREAD	1	/* IMAGE_3_CPU_RX_THREAD_NUMBER (core 3) */
#define RNR_CPU_TX_THREAD	6	/* IMAGE_2_CPU_TX_0_THREAD_NUMBER (core 2) */

/* ----------------------------------------------------------------------------
 * SBPM (base XRDP_OFF_SBPM). Spec sec 4: trigger the free-list init, poll RDY.
 * -------------------------------------------------------------------------- */
#define SBPM_INIT_FREE_LIST	0x000
#define SBPM_INIT_FREE_LIST_VAL	0x03FFC000	/* INIT_OFFSET 0xFFF<<14 -> 0x1000 bufs */
#define SBPM_INIT_FREE_LIST_RDY	BIT(31)

/* ----------------------------------------------------------------------------
 * DSPTCHR / reorder block (base XRDP_OFF_DSPTCHR). Spec sec 5: for first-light
 * we trigger the HW auto-init of the free linked list rather than hand-seeding
 * 1024 nodes, then enable the reorder engine and poll RDY.
 * -------------------------------------------------------------------------- */
#define DSPTCHR_REORDER_CFG		0x000
#define DSPTCHR_REORDER_CFG_EN		BIT(0)
#define DSPTCHR_REORDER_CFG_AUTO_INIT	BIT(4)
#define DSPTCHR_REORDER_CFG_RDY		BIT(8)
/* CPU_RX wake config [spec Wave-8/9; data_path_init.c dispatcher_reorder_*].
 * RAM regs: addr = base + REG + 4*index. The Runner CPU_RX thread is woken by
 * the dispatcher, so a VIQ (== bbh_id) must be created, put in a runner group,
 * and the group pointed at the CPU_RX PD-table address. */
#define DSPTCHR_VQ_EN			0x004	/* bitmask of enabled VIQs */
#define DSPTCHR_QUEUE_CRDT_CFG		0x400	/* +4*viq: bb_id + bbh target */
#define DSPTCHR_PD_DSPTCH_ADD		0x480	/* +4*core: base_add[15:0]|offset_add[31:16] */
#define DSPTCHR_Q_DEST			0x4c0	/* +4*viq: 0=disp,1=reor */
#define DSPTCHR_MASK_MSK_TSK		0x500	/* +4*(grp*8+word): 8 words/grp, word0=tasks[255:224] */
#define DSPTCHR_MASK_MSK_Q		0x600	/* +4*grp: VIQ bitmask for the group */
#define DSPTCHR_TSK_TO_RG_MAPPING	0x900	/* +4*(task/8): 8x3-bit group idx */
#define DSPTCHR_RG_AVLABL_TSK_0_3	0x980	/* 4x8-bit available-task counts grp0-3 */
#define BBH_BBID_RX_BBH0		31	/* RX_BBH_n bb_id = 31 + 2*n */
#define DSPTCHR_VIQ_TARGET_NORMAL	2	/* bbh target = normal */
#define DSPTCHR_CPU_RX_GROUP		1	/* runner group for the LAN->CPU_RX path */
/*
 * CPU_TX EGRESS (delayed-credit) VIQ. The dispatcher is the credit PRODUCER:
 * it deposits credit into the CPU_TX credit table (core-2 @0x29d0) and wakes
 * thread 6. Without this VIQ registered the table stays 0 and CPU_TX stalls
 * after its initial buffers. [rdd_data_structures_auto DISP_REOR_VIQ_*, CFE2
 * dispatcher_reorder_viq_init]. Crdt-cfg target word = (credit_addr>>3) |
 * (thread<<12) in [31:16], bb_id (runner core) in [7:0].
 */
#define DSPTCHR_INGRS_Q_LIMITS		0x300	/* +4*viq: CMN_MAX[9:0] GURNTD_MAX[19:10] CREDIT_CNT[31:20] */
#define DSPTCHR_MASK_DLY_Q		0x620	/* bitmap: VIQ is a delayed (egress) queue */
#define DSPTCHR_EGRS_DLY_QM_CRDT	0x630	/* dispatcher's delayed-egress QM credit pool */
/*
 * ★VIQ NUMBER: the stock driver on THIS silicon uses VIQ 13 for the CPU_TX_0
 * egress (core2/thread6/credit-table 0x29d0) — read live: crdt_cfg[13]=0x653A0002,
 * exactly our target. (DISP_REOR_VIQ_CPU_TX_EGRESS=13; the "MCORE=30" enum is a
 * SECOND instance on core5/thread4/0x2970 — crdt_cfg[30]=0x452E0005.) Match stock:
 * ingrs_lim = CMN_MAX 0x3FF | GURNTD 8 (0x23ff); q_dest is unused (stock=0xDEADBEEF);
 * EGRS_DLY_QM_CRDT seeded to 8.
 */
#define DSPTCHR_CPU_TX_EGRESS_VIQ	13	/* DISP_REOR_VIQ_CPU_TX_EGRESS (stock, verified live) */
/* CMN_MAX[9:0]=0x3FF, GURNTD_MAX[19:10]=8, ★CREDIT_CNT[31:20]=224.
 * ★CREDIT_CNT was 0 here, which is what stalled open-driver TX: the CPU_TX
 * egress thread consumes its one initial descriptor and then parks forever
 * waiting for egress credit that is never granted (observed on silicon:
 * read_idx=1 vs write_idx=35, credit word core2+0x29d0 = 0, and the QM never
 * saw a single PD - occupancy 0 AND drop counters 0). Stock VIQ13 reads
 * 0x0e0023ff; match it exactly. [live DSPTCHR oracle 2026-07-22] */
#define DSPTCHR_TX_INGRS_LIMITS		0x0e0023ff
#define DSPTCHR_EGRS_DLY_QM_CRDT_VAL	8	/* match stock EGRS_DLY_QM_CRDT */
#define DSPTCHR_CPU_TX_CRDT_TGT		(((CPU_TX_EGRESS_CREDIT_OFF >> 3) | \
					  (RNR_CPU_TX_THREAD << 12)) & 0xffff)
/* CPU_RX PD-table the dispatcher delivers to (full-SDK IMAGE_3_PD_FIFO_TABLE
 * 0x3940 >> 3); matches the CPU_RX thread regfile R8/R17. */
#define DSPTCHR_CPU_RX_PD_ADDR		(0x3940 >> 3)

/* ----------------------------------------------------------------------------
 * BBH_RX / BBH_TX per-port config (bases XRDP_OFF_BBH_RX0 / XRDP_OFF_BBH_TX0).
 * Spec secs 7/8. Fixed BB (block) IDs on BCM6813:
 *   DISPATCHER_REORDER=18, FPM=23, SBPM=56, SDMA0=21/SDMA1=22.
 * -------------------------------------------------------------------------- */
#define BBH_BBID_DISPATCHER	18
#define BBH_BBID_FPM		23
#define BBH_BBID_SBPM		56
/* BBH_RX */
#define BBH_RX_BBCFG		0x00	/* DISPBBID[15:8], SBPMBBID[23:16] */
#define BBH_RX_ENABLE		0x3c	/* PKTEN b0, SBPMEN b1 (LAST) */
#define BBH_RX_ENABLE_PKTEN	BIT(0)
#define BBH_RX_ENABLE_SBPMEN	BIT(1)
/* BBH_RX extended per-port config [SDK data_path_init.c bbh_rx_cfg / XRDP_AG.h].
 * BBCFG also carries SDMABBID[5:0] (=SDMA0 21 for ports 0-5, SDMA1 22 for 6-11). */
#define BBH_BBID_SDMA0		21
#define BBH_BBID_SDMA1		22
#define BBH_RX_DISPVIQ		0x04	/* NORMALVIQ[7:0], EXCLVIQ[15:8] = bbh_id */
#define BBH_RX_SDMAADDR		0x1c	/* DATABASE[7:0], DESCBASE[15:8] */
#define BBH_RX_SDMACFG		0x20	/* NUMOFCD[7:0], EXCLTH[15:8] */
#define BBH_RX_SDMACFG_VAL	0x00000404	/* 4 chunk descriptors, excl thr 4 */
#define BBH_RX_SOPOFFSET	0x30
#define BBH_RX_SBPMCFG		0x64	/* MAXREQ[3:0] */
#define BBH_RX_SBPMCFG_VAL	0x0000000f
/* Packet-size window: without these the reset-default MAXPKT (0) drops every
 * frame as TOOLONG. 4 size profiles; per-flow selectors -> profile 0.
 * [SDK bbh_rx_cfg -> drv_bbh_rx_pkt_size{0..3}_set / XRDP_AG.h fields] */
#define BBH_RX_MINPKT0		0x24	/* MINPKT{0..3}[7:0 each] = min eth size 64 */
#define BBH_RX_MAXPKT0		0x28	/* MAXPKT0[13:0], MAXPKT1[29:16] */
#define BBH_RX_MAXPKT1		0x2c	/* MAXPKT2[13:0], MAXPKT3[29:16] */
#define BBH_RX_PERFLOWTH	0x44	/* per-flow group divider = 255 */
#define BBH_RX_MINPKTSEL0	0x50	/* flow->min-profile sel (2b/flow); 0 */
#define BBH_RX_MINPKTSEL1	0x54
#define BBH_RX_MAXPKTSEL0	0x58
#define BBH_RX_MAXPKTSEL1	0x5c
#define BBH_RX_MIN_ETH_PKT	64
#define BBH_RX_MAX_PKT		2048	/* accepts std+VLAN+QinQ; MAC caps upstream */
/* BBH_RX PM counters (per-port) — RX-path diagnostics [XRDP_AG.h PM_COUNTERS] */
#define BBH_RX_PM_INPKT		0x100	/* incoming packets (pre size-filter) */
#define BBH_RX_PM_TOOSHORT	0x10c
#define BBH_RX_PM_TOOLONG	0x110
#define BBH_RX_PM_CRCERROR	0x114
#define BBH_RX_PM_DISPCONG	0x11c	/* dispatcher-congestion drops */
#define BBH_RX_PM_NOSBPMSBN	0x124	/* no SBPM buffer -> drop */
#define BBH_RX_PM_NOSDMACD	0x12c	/* no SDMA chunk descriptor -> drop */
#define BBH_RX_PM_RUNTERROR	0x148
/* BBH_TX */
#define BBH_TX_MACTYPE		0x00	/* = 1 (GPON; 7 is invalid) */
#define BBH_TX_MACTYPE_VAL	1
#define BBH_TX_BBCFG_1		0x04	/* FPMSRC[31:24], SBPMSRC[23:16] */
/* Route A BBH_TX runner/QM-fed binding (the fields the slow-path TX omits).
 * [offsets/fields vs BCM6813/autogen/XRDP_AG.h:20757..20953; spec sec B] */
#define BBH_TX_BBCFG_2		0x08	/* PDRNR{0..3}SRC = BB_ID of feeding TM core(s) */
#define BBH_TX_RNRCFG_2_0	0x60	/* PTRADDR[15:0]=TM egr-cnt-tbl>>3, TASK[19:16] */
#define BBH_TX_RNRCFG_2_TASK_SHIFT 16
#define BBH_TX_Q2RNR_LAN	0x400	/* LAN single: Q0[1:0] Q1[3:2] runner-core sel */
#define BBH_TX_QMQ_LAN		0x4b0	/* LAN single: Q0[0] Q1[1] - 1=QM-fed */
#define BBH_TX_Q2RNR_UNIFIED	0x700	/* unified pair-indexed */
#define BBH_TX_QMQ_UNIFIED	0x7b0	/* unified pair-indexed: Q0[0] Q1[1] */
#define BBH_TX_QMQ_Q0		BIT(0)
#define BBH_TX_QMQ_Q1		BIT(1)

/* ----------------------------------------------------------------------------
 * 1G MAC/PHY: UNIMAC + internal EGPHY (the "eth2"=port_gphy1 first-light port).
 * UNIMAC inst N conf base = 0x828a8000 + N*0x1000; EGPHY power block @ 0x837ff00c.
 * No proprietary blob (the 10G XPORT/serdes path does need one). [SDK
 * unimac_drv_impl1.c / phy_drv_egphy.c / 6813.dtsi]
 * -------------------------------------------------------------------------- */
#define XRDP_OFF_UNIMAC0	0x008a8000UL	/* UNIMAC conf inst0; stride 0x1000 */
#define XRDP_UNIMAC_STRIDE	0x00001000UL
#define UNIMAC_CMD		0x0008		/* MAC command/config */
#define UNIMAC_CMD_TX_ENA	BIT(0)
#define UNIMAC_CMD_RX_ENA	BIT(1)
#define UNIMAC_CMD_SPEED_SHIFT	2		/* eth_speed[3:2]: 1G = 2 */
#define UNIMAC_CMD_SPEED_1G	2
#define UNIMAC_CMD_PROMIS	BIT(4)
#define UNIMAC_CMD_PAD_EN	BIT(5)
#define UNIMAC_CMD_CRC_FWD	BIT(6)
#define UNIMAC_CMD_PAUSE_FWD	BIT(7)
#define UNIMAC_CMD_SW_RESET	BIT(13)
#define UNIMAC_CMD_LOOP_ENA	BIT(15)		/* GMII/MII local loopback (TX->RX) */
#define UNIMAC_CMD_CNTL_FRM_ENA	BIT(23)
#define UNIMAC_CMD_NO_LGTH_CHK	BIT(24)
#define UNIMAC_FRM_LEN		0x0014
#define UNIMAC_FRM_LEN_VAL	0x3fff
/*
 * Internal quad-EGPHY (1G) block in the eth-phy-top register region.
 * Authoritative map: bchp_eth_phy_top_reg.h (68880/6813) + bcmethsw.h
 * (CONFIG_BCM96813 branch). Three adjacent registers:
 *   0x837ff010 QPHY_TEST_CNTRL  (leave 0; phy_test_en/iddq_test_mode only)
 *   0x837ff014 QPHY_CNTRL       (THE control register: reset/iddq/pwr/phyad)
 *   0x837ff018 QPHY_STATUS      (pll_lock = bit 8)
 * iddq_global_pwr[9:6] and ext_pwr_down[4:1] are 4-bit per-port fields
 * (one bit per internal GPHY, 1 = powered down). phy_reset is active-high
 * (POR = 1 = in reset); clear to release. Base phy_phyad = 1 -> the four
 * GPHYs answer at MDIO addr 1,2,3,4 (port_gphy1/eth2 = addr 2).
 */
/*
 * The eth-phy-top / quad-EGPHY / mdiosf2 / xport blocks live in the 0x83000000
 * SoC register region, NOT in the 0x82000000 XRDP/rdpa datapath window. They
 * need their OWN mapping (p->ethphytop); offsets below are relative to
 * ETHPHY_PHYS_BASE, NOT to p->xrdp. (UNIMAC at 0x828a8000 IS in the rdpa
 * window, so it stays on p->xrdp.)
 */
#define ETHPHY_PHYS_BASE	0x837f0000UL	/* xport/ephytop/egphy/mdiosf2 region */
#define ETHPHY_SIZE		0x00010000UL	/* covers 0x837f0000..0x837fffff */
#define ETHPHY_OFF_QEGPHY_TEST_CNTRL 0x0000f010UL /* abs 0x837ff010 (unused, keep 0) */
#define ETHPHY_OFF_QEGPHY_CTRL	0x0000f014UL	/* abs 0x837ff014 QPHY_CNTRL */
#define ETHPHY_OFF_QEGPHY_STATUS 0x0000f018UL	/* abs 0x837ff018 QPHY_STATUS */
#define QEGPHY_CTRL_IDDQ_BIAS		BIT(0)
#define QEGPHY_CTRL_EXT_PWR_DOWN_SHIFT	1	/* [4:1] per-port power-down (1=down) */
#define QEGPHY_CTRL_EXT_PWR_DOWN_MASK	0xf
#define QEGPHY_CTRL_IDDQ_GLOBAL_PWR_SHIFT 6	/* [9:6] per-port iddq (1=down) */
#define QEGPHY_CTRL_IDDQ_GLOBAL_PWR_MASK 0xf
#define QEGPHY_CTRL_CK25_DIS		BIT(10)
#define QEGPHY_CTRL_PHY_RESET		BIT(11)	/* active-high; POR=1; clear to release */
#define QEGPHY_CTRL_PHYAD_SHIFT		12	/* [16:12] base MDIO addr */
#define QEGPHY_CTRL_PHYAD_MASK		0x1f
#define QEGPHY_CTRL_REF_CLK_FREQ_SHIFT	17	/* [18:17]; 0x2 = 50 MHz (stock) */
#define QEGPHY_CTRL_REF_CLK_50MHZ	0x2
#define QEGPHY_CTRL_PLL_CLK125_250_SEL	BIT(19)	/* leave 0 = 125 MHz */
#define QEGPHY_STATUS_PLL_LOCK		BIT(8)
/* port_gphy1 (eth2-class, UNIMAC inst 1) answers at MDIO addr 2 on mdiosf2 */
#define QEGPHY_MDIO_ADDR		2

/* ----------------------------------------------------------------------------
 * Internal 10G XPHY (eth0 = xphy0 @MDIO addr 9) control in the eth-phy-top block
 * (base ETH_PHY_TOP @0x837ff000 = ETHPHY_PHYS_BASE + 0xf000; offsets rel to
 * p->ethphytop). The stock eth_phy_top.c xphy_init() (SKIPPED on the datapath-
 * skip boot) releases the XPHY reset + sets its MDIO addr; without it eth0's
 * XPHY stays in reset (link=0). 6813 offsets [eth_phy_top.c CONFIG_BCM96813].
 * -------------------------------------------------------------------------- */
#define ETHPHY_OFF_XPHY_TEST_CNTRL0	0x0000f234UL	/* abs 0x837ff234 */
#define ETHPHY_OFF_XPHY_CNTRL0		0x0000f238UL	/* abs 0x837ff238 */
#define ETHPHY_OFF_XPHY_TEST_CNTRL1	0x0000f240UL
#define ETHPHY_OFF_XPHY_CNTRL1		0x0000f244UL
#define ETHPHY_OFF_XPHY_MUX_SEL		0x0000f1fcUL	/* abs 0x837ff1fc (LED mux, =1) */
#define XPHY_TEST_ISO_ENABLE		BIT(4)
#define XPHY_TEST_TMODE			BIT(5)
#define XPHY_CNTRL_SUPER_ISOLATE	BIT(0)
#define XPHY_CNTRL_PHY_RESET		BIT(1)
#define XPHY_CNTRL_PHYAD_SHIFT		2		/* [6:2] 5-bit MDIO addr */
#define XPHY_CNTRL_PHYAD_MASK		0x1f
#define XPHY0_MDIO_ADDR			9		/* eth0 xphy0-addr (DTS) */

/* ----------------------------------------------------------------------------
 * 10G XPORT SerDes (merlin16_shortfin). Reached via an indirect register window
 * "brcm,serdes1" @ 0x837ff500 size 0x300 (separate ioremap, like the EGPHY).
 * Per-core block (core N at +N*0x100): ADDR @ +0x04, MASK @ +0x08, CNTRL @ +0xf0.
 * A merlin "lane register" is encoded as (dev<<27)|(lane<<16)|reg and driven via
 * the ADDR/MASK/CNTRL triplet. CNTRL bits: reg_data[15:0], r_w[16], start_busy[17],
 * delayed_ack[18]. The firmware (merlin16_shortfin_ucode_image, 31664 B, CRC 0x4949)
 * is streamed as 16-bit LE words into the micro program RAM. [serdes_access.c,
 * serdes_access_6813.h, merlin16_shortfin_config.c]
 * -------------------------------------------------------------------------- */
#define SERDES_PHYS_BASE	0x837ff500UL
#define SERDES_SIZE		0x00000300UL
#define SERDES_CORE_STRIDE	0x100UL
#define SERDES_OFF_INDIR_ADDR	0x004	/* +core*0x100 */
#define SERDES_OFF_INDIR_MASK	0x008
#define SERDES_OFF_INDIR_CNTRL	0x0f0
#define SERDES_DEV_TYPE_SHIFT	27	/* merlin C45-style dev in the encoded addr */
#define SERDES_LANE_SHIFT	16
#define SERDES_PMD_DEV		1	/* micro/PMD device */
#define SERDES_CNTRL_RW		BIT(16)
#define SERDES_CNTRL_START_BUSY	BIT(17)
#define SERDES_CNTRL_DELAYED_ACK BIT(18)
/* micro subsystem registers (merlin dev1 reg space) + field {mask,shift} */
#define SRD_MICRO_CLK_CTRL	0xd200	/* master_clk_en[0], core_clk_en[1] */
#define SRD_MICRO_CLK_MASTER	0x0001
#define SRD_MICRO_CLK_CORE	0x0002
#define SRD_MICRO_RST_CTRL	0xd201	/* master_rstb[0], core_rstb[1] */
#define SRD_MICRO_RST_MASTER	0x0001
#define SRD_MICRO_RST_CORE	0x0002
#define SRD_MICRO_AHB_CTRL	0xd202	/* ra_wrdatasize[1:0], ra_init[9:8], autoinc_wraddr_en[12] */
#define SRD_MICRO_RA_WRDATASIZE	0x0003
#define SRD_MICRO_RA_INIT	0x0300
#define SRD_MICRO_RA_INIT_SHIFT	8
#define SRD_MICRO_AUTOINC_WR	0x1000
#define SRD_MICRO_AHB_STATUS	0xd203	/* ra_initdone (bit0 per exc_; poll nonzero) */
#define SRD_MICRO_RA_INITDONE	0x8001	/* tolerate bit0|bit15 (RE ambiguity) */
#define SRD_MICRO_RA_WRADDR_LSW	0xd204
#define SRD_MICRO_RA_WRADDR_MSW	0xd205
#define SRD_MICRO_RA_WRDATA_LSW	0xd206
#define SRD_MICRO_PMI_IF_CTRL	0xd228	/* pmi_hp_fast_read_en[0] */
#define SRD_MICRO_PMI_HP_FAST	0x0001
#define SRD_UC_ACTIVE_REG	0xd0f4	/* uc_active[15] - the "uC running" gate */
#define SRD_UC_ACTIVE		0x8000
#define SERDES_FW_NAME		"brcm/merlin16-shortfin.bin"
#define SERDES_FW_SIZE		31664

/* ----------------------------------------------------------------------------
 * XPORT / XLMAC 10G MAC (ports 5/6 = eth0/eth1). The whole XPORT window
 * 0x837f0000 size 0x8000 lives INSIDE the already-mapped p->ethphytop region
 * (ETHPHY_PHYS_BASE 0x837f0000, size 0x10000) -> all offsets below are relative
 * to ETHPHY_PHYS_BASE / p->ethphytop, no extra ioremap.
 *
 * MAC instance is chosen by DT mac-index (=xport_port_id), NOT the switch port:
 *   eth0 = xport_port_id 0 -> XLMAC0 / port 0 -> XLMAC_CORE @ +0x0000
 *   eth1 = xport_port_id 2 -> XLMAC0 / port 2 -> XLMAC_CORE @ +0x0800
 * Both 10G ports are on physical XLMAC0 (TOP/MAB/PORTRESET @ +0x2000/3300/3400).
 * xport_num = xport_port_id & 3 (0 and 2). [re-notes/realhw/12-10g-xport-bringup.md
 * §2; SDK xport/xport_drv.c, XPORT_AG.h]  ⚠ XLMAC_CORE regs are 64-bit committed
 * by a single low-word 32-bit write: fields >bit31 (TX_THRESHOLD b38, PFC_STATS
 * b35) are NOT delivered by writel() and rely on HW default.
 * -------------------------------------------------------------------------- */
#define XPORT_OFF_XLMAC0_CORE	0x00000000UL	/* + xport_num*0x400 (port stride) */
#define XPORT_XLMAC_PORT_STRIDE	0x00000400UL
#define XPORT_OFF_TOP		0x00002000UL	/* XLMAC0 TOP */
#define XPORT_OFF_MAB		0x00003300UL	/* XLMAC0 MAB */
#define XPORT_OFF_PORTRESET	0x00003400UL	/* XLMAC0 PORTRESET */
/* XPORT MIB core: per-port RX/TX stat counters. abs 0x837f1000 (mac0), stride
 * 0x400 -> in p->ethphytop at +0x1000 + mac_index*0x400. Counters are 40-bit
 * (lo32 @ offset, hi8 @ offset+4); the lo32 read suffices to detect activity.
 * [SDK bcm6813_xport_mib_core_ag / XPORT_MIB_CORE_ADDRS] */
#define XPORT_MIB_CORE_OFF	0x00001000UL	/* + mac_index*0x400, in ethphytop */
#define XPORT_MIB_STRIDE	0x00000400UL
#define XPORT_MIB_GRX64		0x00		/* good rx 0-64B */
#define XPORT_MIB_GRXPKT	0x58		/* good rx packets (total) */
#define XPORT_MIB_GRXPOK	0x110		/* good rx packets ok */
#define XPORT_MIB_GTXPKT	0x228		/* good tx packets */
/* XLMAC_CORE per-port (base XPORT_OFF_XLMAC0_CORE + port*0x400) */
#define XLMAC_CORE_CTRL		0x00		/* TX_EN b0, RX_EN b1, SOFT_RESET b6 */
#define XLMAC_CORE_CTRL_TX_EN	BIT(0)
#define XLMAC_CORE_CTRL_RX_EN	BIT(1)
#define XLMAC_CORE_CTRL_SOFT_RESET BIT(6)
#define XLMAC_CORE_CTRL_LOCAL_LPBK BIT(2)	/* TX->RX fold-back (self-test) */
#define XLMAC_CORE_MODE		0x08		/* SPEED_MODE[6:4]; 10G = 4 -> 0x40 */
#define XLMAC_CORE_MODE_SPEED_SHIFT 4
#define XLMAC_CORE_MODE_SPEED_10G 4
#define XLMAC_CORE_TX_CTRL	0x20		/* CRC_MODE[1:0]=3, PAD_EN b4 */
#define XLMAC_CORE_TX_CTRL_CRC_PERPKT 0x3
#define XLMAC_CORE_TX_CTRL_PAD_EN BIT(4)
#define XLMAC_CORE_RX_CTRL	0x30		/* STRIP_CRC b2=0, RUNT_THR[10:4]=0x40, RX_PASS_CTRL b13 */
#define XLMAC_CORE_RX_CTRL_RX_PASS BIT(13)
#define XLMAC_CORE_RX_CTRL_RUNT_SHIFT 4		/* RUNT_THRESHOLD field [10:4] */
#define XLMAC_CORE_RX_MAX_SIZE	0x40
#define XLMAC_CORE_RX_MAX_SIZE_VAL 0x3fff
#define XLMAC_CORE_RX_LSS_CTRL	0x50		/* LOCAL_FAULT_DISABLE */
#define XLMAC_CORE_RX_LSS_LOCAL_FAULT_DIS BIT(0)
#define XLMAC_CORE_PAUSE_CTRL	0x68
#define XLMAC_CORE_PFC_CTRL	0x70
/* XPORT TOP (base XPORT_OFF_TOP) */
#define XPORT_TOP_CONTROL	0x00		/* Px_MODE (2b/port); 10G/XGMII = 1 */
#define XPORT_TOP_CONTROL_MODE_XGMII 1
#define XPORT_TOP_STATUS	0x04		/* LINK_STATUS bit (1<<xport_num) */
/* XPORT MAB (base XPORT_OFF_MAB) */
#define XPORT_MAB_CONTROL	0x00		/* RX_PORT_RST b(0+num), TX_PORT_RST b(4+num), TX_CREDIT_DISAB b(12+num), TX_FIFO_RST b(8+num) */
#define XPORT_MAB_TX_WRR_CTRL	0x04
/* XPORT PORTRESET (base XPORT_OFF_PORTRESET) */
#define XPORT_PORTRESET_CONFIG	0x10		/* LINK_DOWN_RST_EN, ENABLE_SM_RUN[per-port], TICK_TIMER_NDIV */
#define XPORT_PORTRESET_P0_CTRL	0x00		/* port0 SW reset; PORT_SW_RESET b0 */
#define XPORT_PORTRESET_P2_CTRL	0x08		/* port2 */
#define XPORT_PORTRESET_CTRL_SW_RESET BIT(0)
#define XPORT_PORTRESET_P0_SIG_EN 0x30		/* port0 */
#define XPORT_PORTRESET_P2_SIG_EN 0x90		/* port2 */

/* ----------------------------------------------------------------------------
 * FPM register block. Layout from the GPL FpmControl struct (fpm_priv.h):
 *   ctrl  @ 0x0000, pool(broadcast) @ 0x0200, pool0 @ 0x0400, pool1 @ 0x0600.
 * The alloc/dealloc register is at +0x000 of the pool0 management block,
 * i.e. FPM_base + 0x400. ABI 1.2 / 3.1.
 * -------------------------------------------------------------------------- */
#define FPM_CTL			0x0000	/* FpmCtrl.fpm_ctl */
#define FPM_CFG1		0x0004
#define FPM_POOL1_CFG1		0x0040	/* chunk-size select (FP_BUF_SIZE) */
#define FPM_POOL1_CFG2		0x0044	/* pool base addr (>>2, masked) */
#define FPM_POOL1_STAT2		0x0054	/* tokens-available etc. */
#define FPM_SPARE		0x00c4	/* head/tail pad scratch */
#define FPM_POOL0_ALLOC_DEALLOC	0x0400	/* read=alloc token, write=free token */

/* fpm_ctl bits (fpm_priv.h FpmCtrl) */
#define FPM_CTL_INIT_MEM	BIT(4)
#define FPM_CTL_SOFT_RESET	BIT(14)
#define FPM_CTL_POOL1_ENABLE	BIT(16)

/* pool1_cfg1 chunk-size (fpm_priv.h FPMCTRL_FP_BUF_SIZE_*) */
#define FPM_FP_BUF_SIZE_SHIFT	24
#define FPM_FP_BUF_SIZE_512	(0x0 << FPM_FP_BUF_SIZE_SHIFT)
#define FPM_FP_BUF_SIZE_256	(0x1 << FPM_FP_BUF_SIZE_SHIFT)
#define FPM_FP_BUF_SIZE_MASK	(0x7 << FPM_FP_BUF_SIZE_SHIFT)

/* pool1_cfg2 base-addr (fpm_priv.h FPMCTRL_FPM_POOL_BASE_ADDRESS_MASK) */
#define FPM_POOL_BASE_ADDR_MASK	0xfffffffcUL

/* pool1_stat2 tokens-available (fpm_priv.h) */
#define FPM_STAT2_TOKENS_AVAIL_MASK	0x3ffff

/* FPM token bitfields - fpm_priv.h FpmPoolMgmt. ABI 3.1. */
#define FPM_TOKEN_VALID		BIT(31)
#define FPM_TOKEN_POOL_SHIFT	29
#define FPM_TOKEN_POOL_MASK	(0x1 << FPM_TOKEN_POOL_SHIFT)
#define FPM_TOKEN_INDEX_SHIFT	12
#define FPM_TOKEN_INDEX_MASK	(0x1ffffU << FPM_TOKEN_INDEX_SHIFT)
#define FPM_TOKEN_SIZE_SHIFT	0
#define FPM_TOKEN_SIZE_MASK	0xfff

#define FPM_TOKEN_POOL(t)	(((t) & FPM_TOKEN_POOL_MASK) >> FPM_TOKEN_POOL_SHIFT)
#define FPM_TOKEN_INDEX(t)	(((t) & FPM_TOKEN_INDEX_MASK) >> FPM_TOKEN_INDEX_SHIFT)
#define FPM_TOKEN_SIZE(t)	(((t) & FPM_TOKEN_SIZE_MASK) >> FPM_TOKEN_SIZE_SHIFT)

/* pool sizing - fpm_core.c:80-85. We request a 512B-chunk, 32MB pool. */
#define FPM_CHUNK_SIZE_DEFAULT	512
#define FPM_POOL_SIZE_512	(32U << 20)
#define FPM_NET_BUF_HEAD_PAD	240	/* fpm_priv.h: reserved head pad */

/* ----------------------------------------------------------------------------
 * Host CPU RX data-ring descriptor: 16 bytes / 4 words, big-endian in DDR.
 *
 * AUTHORITATIVE SOURCE (BCM6813/XRDP, NOT 416L05/63138):
 *   CPU_RX_DESCRIPTOR in the 6813 GPL SDK
 *   src-rt-5.04behnd.4916/rdp/projects/XRDP_CFE2/drivers/rdp_subsystem/cpu/
 *     rdp_cpu_ring_defs.h  (little-endian variant, lines ~143-235)
 *   parsed by rdpa_cpu_rx_pd_get() in
 *   src-rt-5.04behnd.4916/rdp/drivers/rdpa_gpl/include/rdpa_cpu_helper.h
 *   and consumed by the GPL conduit in
 *   bcmdrivers/opensource/net/enet/impl7/enet_ring.c (e.g. line 305:
 *   "swap4bytes(p->word2) & 0x80000000" = ownership test).
 *
 * The Runner writes the descriptor big-endian in DDR; the LE host byte-swaps
 * each 32-bit word (swap4bytes) before reading, so the field positions below
 * are in the HOST (post-swap) little-endian word view.
 *
 * VALIDATED against LIVE silicon (bdmf "bs /Driver/cpUr/Vrpd", abs-addr mode):
 *   LAN  len=66  host words: 5d094140 00a5010a 82800000 80000000
 *     -> word0=ptr_low=0x5d094140, word1.packet_length=66, word1.abs=1,
 *        word2.is_src_lan=1, word2.source_port=1, word3.is_exception=1.
 *   WLAN/CPU len=124: 5d084140 000101f2 07400000 20000402
 *     -> word0=ptr_low=0x5d084140, word1.packet_length=124, word1.abs=1,
 *        word2.is_src_lan=0, word2.vport=3, word2.ssid=10,
 *        word3.is_ucast=1, word3.wl_metadata=0x402.
 *   (len=102/774 LAN samples also decode exactly; see RE notes.)
 *
 * NOTE on word numbering: this XRDP layout puts the abs buffer phys pointer in
 * word0/word1 and length+abs in word1 - it is NOT the legacy DSL-RDP layout
 * (word0=len, word2=ptr) that the old 416L05 header used. The abs phys pointer
 * is the FULL 40-bit address: word0 = ptr_low[31:0], word1.host_buffer_data_ptr_hi
 * = ptr[39:32]. Ownership/valid is signalled by word3.is_exception's byte (the
 * runner sets word2/word3 high byte non-zero); the conduit's "packet present"
 * test in the GPL enet uses (swap4bytes(word2) & 0x80000000) on this same ring
 * for the *legacy* layout, but on XRDP the abs.abs bit (word1 bit16) marks abs
 * mode and the runner clears word1/word2 to 0 on a free slot. The open driver
 * follows rdpa_cpu_ring_rest_desc(): on refill it writes word2 only.
 * -------------------------------------------------------------------------- */
struct runner_rx_desc {
	__be32	word0;	/* abs: host_buffer_data_ptr_low[31:0] (packet DDR phys, low 32) */
	__be32	word1;	/* abs: ptr_hi[31:24] | abs(b16) | packet_length[15:2] | is_chksum_verified(b1) */
	__be32	word2;	/* reason[5:0] | data_offset[12:6] | {LAN:src_port|CPU:ssid,vport} | is_src_lan(b31) */
	__be32	word3;	/* wl_metadata/dst_ssid_vector[15:0] | is_ucast(b29)|is_rx_offload(b30)|is_exception(b31) */
};

/* --- word0/word1: abs (absolute-address) buffer pointer + length --- */
/* word0 = host_buffer_data_ptr_low[31:0] (no shift) */
#define RXD_W1_PKT_LEN_SHIFT	2		/* word1 bits [15:2], width 14 */
#define RXD_W1_PKT_LEN_MASK	0x3fff
#define RXD_W1_ABS		BIT(16)		/* word1 bit16: 1 = abs-address mode */
#define RXD_W1_IS_CHKSUM_VERIF	BIT(1)		/* word1 bit1 */
#define RXD_W1_PTR_HI_SHIFT	24		/* word1 bits [31:24] = phys[39:32] */
#define RXD_W1_PTR_HI_MASK	0xff

/* --- word2: reason / data_offset / source identification --- */
#define RXD_W2_REASON_MASK	0x3f		/* bits [5:0]   rdpa_cpu_reason */
#define RXD_W2_DATA_OFFSET_SHIFT 6		/* bits [12:6]  headroom offset */
#define RXD_W2_DATA_OFFSET_MASK	0x7f
#define RXD_W2_IS_SRC_LAN	BIT(31)		/* bit31: 1=LAN bridge port, 0=CPU/WLAN vport */
/* LAN / WAN sub-decode (is_src_lan==1): */
#define RXD_W2_LAN_SRC_PORT_SHIFT 25		/* bits [29:25] source bridge port */
#define RXD_W2_LAN_SRC_PORT_MASK  0x1f
#define RXD_W2_WAN_FLOW_ID_SHIFT  13		/* bits [24:13] WAN flow id (WAN only) */
#define RXD_W2_WAN_FLOW_ID_MASK   0xfff
/* CPU/WLAN vport sub-decode (is_src_lan==0): */
#define RXD_W2_VPORT_SHIFT	25		/* bits [29:25] originating vport */
#define RXD_W2_VPORT_MASK	0x1f
#define RXD_W2_SSID_SHIFT	21		/* bits [24:21] WLAN ssid */
#define RXD_W2_SSID_MASK	0xf

/* --- word3: WLAN/offload metadata + flags --- */
#define RXD_W3_WL_METADATA_MASK	0xffff		/* bits [15:0] dst_ssid_vector / wl_metadata */
#define RXD_W3_IS_UCAST		BIT(29)		/* bit29 */
#define RXD_W3_IS_RX_OFFLOAD	BIT(30)		/* bit30 */
#define RXD_W3_IS_EXCEPTION	BIT(31)		/* bit31 (set for trapped/exception frames) */

/*
 * Ownership / refill: rdpa_cpu_ring_rest_desc() (rdpa_cpu_helper.h) hands a slot
 * back to the runner by writing word2 = swap4bytes(phys & 0x7fffffff) - i.e. it
 * stages the next buffer's low-31 phys in *word2* and clears the top bit so the
 * runner owns it. On the XRDP abs ring the runner then DMAs the frame and rewrites
 * word0/word1 (abs ptr + len) and word2/word3 (source + flags). The conduit's
 * empty test is byte-wise (word3 high byte / is_exception byte != 0 once filled).
 */
#define RXD_REFILL_PTR_MASK	0x7fffffffU	/* low-31 phys staged into word2 on refill */

/* ----------------------------------------------------------------------------
 * Host CPU FEED-ring descriptor: 8 bytes, big-endian.
 * Source: CPU_FEED_DESCRIPTOR in the 6813 GPL SDK
 *   src-rt-5.04behnd.4916/rdp/drivers/rdp_subsystem/xrdp/rdp_cpu_ring_defs.h
 * Each feed entry is just an abs buffer token the host posts so the runner has
 * somewhere to DMA the next RX frame. Live feed ring 24 entries look like
 * 0xFFFFFF800E01FFF2 (an FPM/abs token, two 32-bit halves). LE host word view:
 * -------------------------------------------------------------------------- */
struct runner_feed_desc {
	__be32	ptr_low;	/* host_buffer_data_ptr_low[31:0] (abs phys, low 32) */
	__be32	w1;		/* valid_flag[31:9] | type/abs(b8) | ptr_hi[7:0] */
};
/* [VERIFIED vs CPU_FEED_DESCRIPTOR, rdd_data_structures_auto.h:17168: the type
 * (ABS) bit is byte+6 bit0 == host bit8; ptr_hi[39:32] is byte+7 == host[7:0].
 * ABS_TYPE=1 => an absolute DDR pointer (vs FPM token). The host w1 value below
 * is cpu_to_be32()'d, so host bit8 -> byte6 bit0 and host[7:0] -> byte7. ] */
#define FEED_W1_ABS		BIT(8)		/* type/abs bit (byte+6 bit0) */
#define FEED_W1_PTR_HI_SHIFT	0		/* ptr_hi[39:32] at byte+7 */
#define FEED_W1_PTR_HI_MASK	0xff

/* ----------------------------------------------------------------------------
 * Host CPU RECYCLE-ring descriptor: 8 bytes, big-endian.
 * Source: CPU_RECYCLE_DESCRIPTOR in
 *   src-rt-5.04behnd.4916/rdp/projects/BCM6813/target/rdd/rdd_data_structures_auto.h
 *   (lines ~513-561; field byte/bit offsets from the RDD_CPU_RECYCLE_* macros).
 * The runner posts freed skb tokens here for the host to reclaim. LE host view:
 * -------------------------------------------------------------------------- */
struct runner_recycle_desc {
	__be32	skb_ptr_low;	/* skb pointer / phys low 32 */
	__be32	w1;		/* skb_ptr_hi[31:24] | abs(b16) | from_feed_ring(b17) | rsvd */
};
#define RECYCLE_W1_ABS		BIT(16)		/* word1 bit16 (byte6 bit0 BE) */
#define RECYCLE_W1_FROM_FEED	BIT(17)		/* word1 bit17 (byte6 bit1 BE) */
#define RECYCLE_W1_PTR_HI_SHIFT	24		/* word1 bits [31:24] = skb_ptr_hi (byte7 BE) */
#define RECYCLE_W1_PTR_HI_MASK	0xff

/* ----------------------------------------------------------------------------
 * CPU-TX ring descriptor: 16 bytes, big-endian.
 * Source: RING_CPU_TX_DESCRIPTOR in the 6813 GPL SDK
 *   src-rt-5.04behnd.4916/rdp/projects/BCM6813/target/rdd/rdd_data_structures_auto.h
 *   (struct at line ~1994; field byte/bit offsets from the RDD_RING_CPU_TX_* macros,
 *    e.g. PACKET_LENGTH @+0 lsb8 w14, IS_EMAC @+8 bit4, ABS @+9 bit0, PKT_BUF_PTR_LOW
 *    @+12). Filled by the closed rdpa_cpu_send_sysb(); the open driver writes the
 *    same wire layout. Positions below are the HOST (post-swap) LE word view.
 *
 * Two payload modes:
 *   abs=1  : word3 = pkt_buf_ptr_low (host buffer phys low-32), word2.pkt_buf_ptr_high
 *            = phys[39:32]. The runner DMAs directly from host DDR.
 *   abs=0  : word3 = FPM token (fpm_bn0[19:0] | fpm_sop[29:20]); the host first
 *            copied the packet into an FPM buffer.
 * -------------------------------------------------------------------------- */
struct runner_tx_desc {
	__be32	word0;	/* sk_buf_ptr_high[7:0] | packet_length[21:8] | egress_or_ingress_1[30:22] | is_egress(b31) */
	__be32	word1;	/* sk_buf_ptr_low_or_data_1588[31:0] */
	__be32	word2;	/* pkt_buf_ptr_high[7:0] | ssid[13:10] | abs(b16) | flow_or_port_id[26:20] | is_vport(b27) | is_emac(b28) | flag_1588(b29) | do_not_recycle(b30) | color(b31) */
	__be32	word3;	/* pkt_buf_ptr_low (abs=1) OR fpm_bn0[19:0]|fpm_sop[29:20] (abs=0) */
};

/* word0 (host LE order, post-swap) */
#define TXD_W0_IS_EGRESS	BIT(31)			/* bit31 */
#define TXD_W0_FIRST_LEVEL_Q_SHIFT 22			/* bits [30:22] egress_or_ingress_1 */
#define TXD_W0_FIRST_LEVEL_Q_MASK  0x1ff		/* 9-bit target QM queue (Route A) */
#define TXD_W0_PKT_LEN_SHIFT	8			/* bits [21:8], width 14 */
#define TXD_W0_PKT_LEN_MASK	0x3fff
#define TXD_W0_SKB_PTR_HI_MASK	0xff			/* bits [7:0] = sk_buf phys[39:32] */

/* word2 (host LE order, post-swap) */
#define TXD_W2_COLOR		BIT(31)			/* bit31 */
#define TXD_W2_DO_NOT_RECYCLE	BIT(30)			/* bit30 */
#define TXD_W2_FLAG_1588	BIT(29)			/* bit29 */
#define TXD_W2_IS_EMAC		BIT(28)			/* bit28 */
#define TXD_W2_IS_VPORT		BIT(27)			/* bit27: 1=[26:20] is a vport, 0=emac port */
#define TXD_W2_PORT_SHIFT	20			/* bits [26:20] flow_or_port_id (7-bit egress emac port) */
#define TXD_W2_PORT_MASK	0x7f			/* [26:20], NOT [27:20] - bit27 is is_vport */
#define TXD_W2_ABS		BIT(16)			/* bit16: 1=abs addr (word3=phys), 0=FPM token */
#define TXD_W2_SSID_SHIFT	10			/* bits [13:10] WLAN ssid */
#define TXD_W2_SSID_MASK	0xf
#define TXD_W2_PKT_BUF_PTR_HI_MASK 0xff			/* bits [7:0] = pkt_buf phys[39:32] (abs mode) */

/* word3 FPM-token sub-fields (abs=0 mode) */
#define TXD_W3_FPM_BN0_MASK	0xfffff			/* bits [19:0] */
#define TXD_W3_FPM_SOP_SHIFT	20			/* bits [29:20] */
#define TXD_W3_FPM_SOP_MASK	0x3ff

/* ----------------------------------------------------------------------------
 * CPU_RING_DESCRIPTOR - the per-ring control block the host fills in PSRAM so
 * the Runner knows where the ring lives. 16 B, big-endian. ABI 2.1.
 * -------------------------------------------------------------------------- */
struct runner_ring_cfg {
	__be32	w0;	/* byte0: size_of_entry[7:3]; half@0: number_of_entries[10:0]; half@2: interrupt_id */
	__be32	w1;	/* half@4: drop_counter; half@6: write_idx */
	__be32	base_addr_low;	/* word@8 */
	__be32	w3;	/* half@c: read_idx; byte@e: rsvd; byte@f: base_addr_high */
};

/*
 * CPU_TX_RING_INDICES_VALUES_TABLE entry, written into EACH serving Runner
 * core's RNR_MEM SRAM. The host bumps write_idx here (big-endian u16) after
 * staging a TX descriptor; the Runner polls it - "the index write IS the
 * doorbell" (ABI 5bis-G2). The exact RDD table offset within RNR_MEM is not
 * yet pinned from the GPL RDD map; it is a fixed contract offset the emulator
 * honours (see qemu/device-model/runner-emulation-contract.md).
 */
/* [PINNED 2026-06-22 vs SDK oracle: RDD_CPU_TX_RING_INDICES_VALUES_TABLE_ADDRESS_ARR = 0x29c8
 * (BCM6813_FPI). RDD/core-data-memory offset; + per-core RNR base for the absolute addr. */
#define CPU_TX_RING_INDICES_OFF		0x29c8	/* RDD off, core 2 [SDK BCM6813] */
/* CPU_TX credit/sync state (core-2 data mem) - the egress dispatcher credit is
 * replenished by the QM/dispatcher; if it stays 0 the CPU_TX thread stalls after
 * its initial credits. [rdd_runner_reg_dump_addrs.c core 2] */
#define CPU_TX_EGRESS_CREDIT_OFF	0x29d0	/* 3 x u32 */
/* VPORT_TX_FLOW_TABLE (image_2 / core 2, RDD 0x0fc0): TX_FLOW_ENTRY_STRUCT
 * entry[64], ONE BYTE each, indexed directly by vport. Firmware is big-endian:
 * valid:1 (bit7) | reserved:2 | qos_table_ptr:5. An all-zero table means every
 * vport is INVALID, so a CPU_TX descriptor with is_vport=1 is consumed and then
 * dropped by the microcode - which is exactly what we measured (read_idx
 * advances, QM occupancy AND drop counters both stay 0).
 * [rdd_data_structures_auto.h TX_FLOW_ENTRY_STRUCT; spec 11 sec D
 *  rdd_tx_flow_enable(); for non-PON/DSL tx_flow = port] */
/* ★VPORT_CFG_TABLE (image_2 / core 2, RDD 0x2580): VPORT_CFG_ENTRY_STRUCT,
 * 4 B per vport, firmware BIG-ENDIAN field order (MSB first):
 *   exception:1 | congestion_flow_control:1 | ingress_filter_profile:6 |
 *   natc_tbl_id:3 | viq:4 | port_dbg_stat_en:1 | mcast_whitelist_skip:1 |
 *   is_lan:1 | bb_rx_id:6 | cntr_id:8
 * Live stock entry for vport 5 (eth0) is 0x000ae903, which decodes to
 * viq=5, is_lan=1, bb_rx_id=41, mcast_whitelist_skip=1, cntr_id=3 -- i.e.
 * exactly this port's own dispatcher VIQ and BBH_RX bb_id, so we build it from
 * those rather than hardcoding. Ours was 0 (vport unconfigured).
 * [rdd_data_structures_auto.h VPORT_CFG_ENTRY_STRUCT; live stock dump] */
#define RDD_VPORT_CFG_TABLE		0x2580
#define   VPORT_CFG_VIQ_SHIFT		17	/* [20:17] */
#define   VPORT_CFG_PORT_DBG_STAT_EN	BIT(16)
#define   VPORT_CFG_MCAST_WL_SKIP	BIT(15)
#define   VPORT_CFG_IS_LAN		BIT(14)
#define   VPORT_CFG_BB_RX_ID_SHIFT	8	/* [13:8] */
#define   VPORT_CFG_CNTR_ID_SHIFT	0	/* [7:0] */
/* QUEUE_THRESHOLD_VECTOR (core 2, RDD 0x3540): one byte per vport; live stock
 * holds value == index for every CONFIGURED vport and 0xff elsewhere. */
#define RDD_QUEUE_THRESHOLD_VECTOR	0x3540

/* ★QM_QUEUE_TO_TX_FLOW_TABLE_PTR_TABLE (image_2 / core 2, RDD 0x3600):
 * 160 x BYTES_2 (one big-endian u16 per QM tx queue, MAX_TX_QUEUES=160).
 * THE missing indirection for CPU_TX egress: the microcode resolves a QM queue
 * to its TX-flow table through this pointer, then indexes that table by vport.
 * Live stock fills EVERY entry with 0x0fc0 == RDD_VPORT_TX_FLOW_TABLE. We left
 * it all-zero, so a queue resolved to a NULL flow-table pointer, resolution
 * failed and the PD was discarded inside the microcode -- before the QM, which
 * is why QM occupancy AND the QM drop counters were both 0.
 * (NB spec 11 sec C calls this table "#if BCM_DSL_XRDP, a no-op on 6813" --
 * live silicon contradicts that: it is populated and required.)
 * [rdd_data_structures_auto.{h,c}; live stock core-2 RDD dump 2026-07-22] */
#define RDD_QM_QUEUE_TO_TX_FLOW_PTR	0x3600
#define RDD_QM_QUEUE_TO_TX_FLOW_ENTRIES	160

/* CPU_TX_SYNC_FIFO (image_2 / core 2, RDD 0x3780): 2 x
 * CPU_TX_SYNC_FIFO_ENTRY_STRUCT, 8 B each, one per CPU_TX thread (6 and 7).
 * Big-endian: write_ptr:u16 | read_ptr:u16 | fifo:u16 | reserved:u16.
 * ★These are LIVE microcode pointers, NOT init constants - two reads of the
 * same running stock box gave e1 = 0x378d378d then 0x378c378c. Do not write
 * them: seeding left the FIFO inconsistent on silicon (write != read). Kept
 * only as a read-only diagnostic address.
 * ⚠ The BCM6813_FPI rdd address map lists CPU_TX_SYNC_FIFO_TABLE at 0x3580 and
 * CPU_TX_RING_DESCRIPTOR_TABLE at 0x3360, but on THIS image both stock and we
 * use 0x3780 / 0x33e0 - that map is only partially applicable (it IS correct
 * for VPORT_TX_FLOW_TABLE 0xfc0, ring indices 0x29c8 and egress credit 0x29d0,
 * all three confirmed on silicon). Trust silicon over the FPI map. */
#define RDD_CPU_TX_SYNC_FIFO		0x3780

#define RDD_VPORT_TX_FLOW_TABLE		0x0fc0
#define RDD_VPORT_TX_FLOW_ENTRIES	64
#define RDD_TX_FLOW_ENTRY_VALID		0x80	/* valid=1, qos_table_ptr=0 */
#define CPU_TX_INGRESS_CREDIT_OFF	0x2b70	/* 3 x u32 */
#define CPU_TX_SYNC_FIFO_OFF		0x3780	/* {write_ptr@+0, read_ptr@+2} */

/*
 * Field offsets WITHIN a runner_ring_cfg control block (16B, big-endian) for the
 * indices the host and runner exchange at runtime (CPU_RING_DESCRIPTOR):
 *   write_idx : u16 @ +6  (runner advances it as it delivers RX frames)
 *   read_idx  : u16 @ +12 (host advances it as it consumes)
 * Spec Wave-5: RX delivery is detected by polling write_idx vs the host read_idx,
 * NOT a per-descriptor word2 ownership bit.
 */
#define RING_CFG_WRITE_IDX_OFF		6
#define RING_CFG_READ_IDX_OFF		12

/*
 * FEED ring (MPM-free first-light): the host posts empty DDR buffers as 40-bit
 * ABS pointers (CPU_FEED_DESCRIPTOR, 8B) so the runner has somewhere to DMA the
 * next RX frame. Feed-ring control block lives in IMAGE_3 (core3) RDD @ 0x0f70;
 * the host bumps its write_idx (the "feed doorbell", rdd_cpu_inc_feed_ring_write_idx).
 * [SDK BCM6813 rdd_runner_defs_auto.h FEED ring; Wave-4/5.]
 */
#define RDD_FEED_RING_DESC_TABLE	0x0f70	/* RDD off, core 3 [SDK BCM6813] */

/* ----------------------------------------------------------------------------
 * OFFLOAD (Phase 1: L2 + VLAN HW flow-offload). The offload context bundles the
 * flowtable rhashtable, the NAT-C MMIO handle and a default egress vport. The
 * conduit driver owns the ioremapped window, so the NAT-C add/del/stats helpers
 * (which poke PSRAM + the indirect command register) live in the conduit driver
 * and are called by flow_offload.c. See flow_offload.{c,h}.
 * -------------------------------------------------------------------------- */
#include <linux/rhashtable.h>
#include <linux/netdevice.h>	/* enum tc_setup_type (by value below) */

struct net_device;
struct natc_key;
struct fc_ucast_ctx;

struct xrdp_offload {
	struct rhashtable	flow_table;
	void			*drv;		/* struct runner_priv * (conduit) */
	u16			default_vport;	/* egress vport for the self-test */
};

/* flow_offload.c */
int  xrdp_offload_init(struct xrdp_offload *o);
void xrdp_offload_deinit(struct xrdp_offload *o);
int  xrdp_offload_setup_tc(struct xrdp_offload *o, struct net_device *dev,
			   enum tc_setup_type type, void *type_data);
int  xrdp_offload_selftest(struct xrdp_offload *o, const u8 *mac_da,
			   const u8 *mac_sa, int vlan_op, u16 vid);
int  xrdp_offload_nat_selftest(struct xrdp_offload *o,
			       __be32 ip_sa, __be32 ip_da,
			       __be16 l4_sport, __be16 l4_dport, u8 ip_proto,
			       __be32 nat_sip, __be16 nat_sport);

/*
 * NAT-C slot allocation (RE-PINNED, re-notes/re-firmware/01-natc-abi.md §3):
 * the stock stack does NOT use a monotonic counter — the table slot is
 * HASH-DERIVED (XOR-fold of the masked key, seed 0x4899b351) then open-addressed
 * probed. Real HW folds the 32-bit hash to N = 13 + ddr_size_enum index bits; we
 * use a fixed-size open table for the driver<->model contract.
 */
#define NATC_HASH_SEED		0x4899b351U	/* XOR-fold seed (RE 01 §3 mode 0) */
#define NATC_IDX_BITS		12		/* open table index width (real: 13+enum) */
#define NATC_TABLE_SLOTS	(1U << NATC_IDX_BITS)

/* bcm4916_runner.c - NAT-C connection-table I/O (owns the MMIO window) */
int  xrdp_natc_add(struct xrdp_offload *o, const struct natc_key *key,
		   const struct fc_ucast_ctx *ctx, u32 *idx_out);
void xrdp_natc_del(struct xrdp_offload *o, const struct natc_key *key, u32 idx);
void xrdp_natc_stats(struct xrdp_offload *o, u32 idx, u64 *pkts, u64 *bytes);

#endif /* _BCM4916_RUNNER_H_ */
