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
 *   - CPU RX descriptor (word0..3) layout:
 *       broadcom-sdk-416L05 .../shared/opensource/include/rdp/rdp_cpu_ring_defs.h
 *       (the "Rosetta stone"; cross-confirmed with rdpa.ko
 *        dump_RDD_PROCESSING_RX_DESCRIPTOR, ABI 2.3/5bis-G1)
 *   - TX descriptor layout: rdpa.ko dump_RDD_RING_CPU_TX_DESCRIPTOR, ABI 2.2
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

#define XRDP_OFF_PSRAM		0x00000000UL	/* RDD tables live here */
#define XRDP_OFF_RNR_MEM0	0x00700000UL	/* per-core data SRAM, stride 0x20000 */
#define XRDP_RNR_MEM_STRIDE	0x00020000UL
#define XRDP_RNR_CORES		8
#define XRDP_OFF_RNR_REGS0	0x00800000UL	/* per-core ctl, stride 0x1000 */
#define XRDP_RNR_REGS_STRIDE	0x00001000UL
#define XRDP_RNR_INST_OFF	0x00010000UL	/* core_base + this = inst SRAM */
#define XRDP_RNR_PRED_OFF	0x0001c000UL	/* core_base + this = pred RAM */
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
#define XRDP_OFF_DQM		0x00c80034UL

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
 * Layout = the GPL 416L05 CPU_RX_DESCRIPTOR (rdp_cpu_ring_defs.h), which is the
 * host view of the Runner PROCESSING_RX_DESCRIPTOR (ABI 2.3/5bis-G1).
 *
 * IMPORTANT (ABI 2.4 / 5bis-G1, GPL-confirmed): for the host CPU-RX *ring*,
 * word2 carries BOTH the buffer phys pointer (low 31 bits) AND the ownership
 * bit (bit31, MSB post-swap). This is the abs-address ring mode the open
 * driver uses; it does not depend on FPM-token decode on the RX path (the
 * Runner DMAs the frame straight into the host-provided buffer).
 * -------------------------------------------------------------------------- */
struct runner_rx_desc {
	__be32	word0;	/* packet_length[13:0], source_port[18:14], ... */
	__be32	word1;	/* descriptor_type/reason/dst_ssid (unused slow-path) */
	__be32	word2;	/* ownership(bit31) | host_buffer_data_pointer[30:0] */
	__be32	word3;	/* wl/1588 metadata (unused slow-path) */
};

/* word0 fields (LE host order, post-swap) - 416L05 little-endian struct */
#define RXD_W0_PKT_LEN_MASK	0x3fff		/* bits [13:0]  */
#define RXD_W0_SRC_PORT_SHIFT	14		/* bits [18:14] */
#define RXD_W0_SRC_PORT_MASK	0x1f

/* word2: ownership bit + 31-bit buffer pointer */
#define RXD_W2_OWNERSHIP_HOST	0x80000000U	/* MSB post-swap; set => host owns */
#define RXD_W2_BUF_PTR_MASK	0x7fffffffU

/* ----------------------------------------------------------------------------
 * CPU-TX ring descriptor: 16 bytes, big-endian. ABI 2.2.
 * Built by the host in FPM-token mode (abs=0): packet copied into an FPM
 * buffer, token placed in word3 (pkt_buf_ptr_low_or_fpm_bn0).
 * -------------------------------------------------------------------------- */
struct runner_tx_desc {
	__be32	word0;	/* is_egress | egress_or_ingress_1 | packet_length | sk_buf_ptr_high */
	__be32	word1;	/* sk_buf_ptr_low_or_data_1588 */
	__be32	word2;	/* color|recycle|1588|is_emac | wan_flow/src_port | flags | pkt_buf_ptr_high */
	__be32	word3;	/* pkt_buf_ptr_low_or_fpm_bn0 (the FPM token in abs=0 mode) */
};

/* word0 (host order, post-swap). ABI 2.2 byte/bit map. */
#define TXD_W0_IS_EGRESS	BIT(31)			/* byte0 bit7 */
#define TXD_W0_PKT_LEN_SHIFT	8			/* bits [21:8], width 14 */
#define TXD_W0_PKT_LEN_MASK	0x3fff

/* word2 (host order, post-swap) */
#define TXD_W2_IS_EMAC		BIT(28)			/* byte8 bit4 */
#define TXD_W2_PORT_SHIFT	20			/* half@8 bits[11:4], egress port */
#define TXD_W2_PORT_MASK	0xff
#define TXD_W2_ABS		BIT(8)			/* byte9 bit0: 1=abs addr, 0=FPM token */

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
#define CPU_TX_RING_INDICES_OFF		0x0000	/* TODO: pin RDD table offset */

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

/* bcm4916_runner.c - NAT-C connection-table I/O (owns the MMIO window) */
int  xrdp_natc_add(struct xrdp_offload *o, const struct natc_key *key,
		   const struct fc_ucast_ctx *ctx, u32 *idx_out);
void xrdp_natc_del(struct xrdp_offload *o, const struct natc_key *key, u32 idx);
void xrdp_natc_stats(struct xrdp_offload *o, u32 idx, u64 *pkts, u64 *bytes);

#endif /* _BCM4916_RUNNER_H_ */
