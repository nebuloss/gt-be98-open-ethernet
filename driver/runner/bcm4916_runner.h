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
#define XRDP_OFF_UNIMAC_RDP0	0x008a8004UL	/* 4, stride 0x1000 */
#define XRDP_OFF_FPM		0x00a00000UL
#define XRDP_OFF_QM		0x00c00000UL
#define XRDP_OFF_DQM		0x00c80034UL	/* [rdpa.ko DQM_ADDRS[0]=0x82c80034 — stock accessor base; revert] */

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
#define RNR_CFG_DDR_CFG		0x40
#define RNR_CFG_DDR_CFG_BASE_MASK	0x000fffff
#define RNR_CFG_DDR_CFG_BUF_SIZE_MODE	BIT(23)
#define RNR_CFG_PSRAM_CFG	0x44	/* DMA_BASE[19:0] = psram_base>>20 = 0x820 */
#define RNR_CFG_PSRAM_CFG_VAL	0x820
#define RNR_CFG_SCH_CFG		0x4c	/* scheduler cfg = 4 (DRV_RNR_16SP) */
#define RNR_CFG_SCH_CFG_VAL	0x4
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
/* BBH_TX */
#define BBH_TX_MACTYPE		0x00	/* = 1 (GPON; 7 is invalid) */
#define BBH_TX_MACTYPE_VAL	1
#define BBH_TX_BBCFG_1		0x04	/* FPMSRC[31:24], SBPMSRC[23:16] */

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
	__be32	word2;	/* pkt_buf_ptr_high[7:0] | ssid[13:10] | abs(b16) | wan_flow/src_port[27:20] | is_emac(b28) | flag_1588(b29) | do_not_recycle(b30) | color(b31) */
	__be32	word3;	/* pkt_buf_ptr_low (abs=1) OR fpm_bn0[19:0]|fpm_sop[29:20] (abs=0) */
};

/* word0 (host LE order, post-swap) */
#define TXD_W0_IS_EGRESS	BIT(31)			/* bit31 */
#define TXD_W0_PKT_LEN_SHIFT	8			/* bits [21:8], width 14 */
#define TXD_W0_PKT_LEN_MASK	0x3fff
#define TXD_W0_SKB_PTR_HI_MASK	0xff			/* bits [7:0] = sk_buf phys[39:32] */

/* word2 (host LE order, post-swap) */
#define TXD_W2_COLOR		BIT(31)			/* bit31 */
#define TXD_W2_DO_NOT_RECYCLE	BIT(30)			/* bit30 */
#define TXD_W2_FLAG_1588	BIT(29)			/* bit29 */
#define TXD_W2_IS_EMAC		BIT(28)			/* bit28 */
#define TXD_W2_PORT_SHIFT	20			/* bits [27:20] wan_flow / source_port (egress) */
#define TXD_W2_PORT_MASK	0xff
#define TXD_W2_IS_VPORT		BIT(27)			/* bit27 (when set, [26:20]=vport flow_or_port_id) */
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
#define CPU_TX_RING_INDICES_OFF		0x29c8	/* RDD off [SDK BCM6813_FPI] */

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

/* bcm4916_runner.c - NAT-C connection-table I/O (owns the MMIO window) */
int  xrdp_natc_add(struct xrdp_offload *o, const struct natc_key *key,
		   const struct fc_ucast_ctx *ctx, u32 *idx_out);
void xrdp_natc_del(struct xrdp_offload *o, const struct natc_key *key, u32 idx);
void xrdp_natc_stats(struct xrdp_offload *o, u32 idx, u64 *pkts, u64 *bytes);

#endif /* _BCM4916_RUNNER_H_ */
