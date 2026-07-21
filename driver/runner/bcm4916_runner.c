// SPDX-License-Identifier: GPL-2.0
/*
 * bcm4916_runner.c - open mainline slow-path datapath driver for the Broadcom
 * BCM4916 (BCM6813) XRDP "Runner" CPU-port conduit.
 *
 * This is the DSA conduit (master) netdev for the BCM4916 integrated switch:
 * it owns the XRDP CPU-port host ring and moves trapped / CPU-forwarded frames
 * between the Runner and the host. The DSA switch (b53/bcm_sf2 BCM4916 variant)
 * attaches its CPU port to this netdev.
 *
 * Scope: SLOW PATH ONLY (CPU-forwarded frames). The hardware fast path
 * (flow offload / NAT-C / cmdlist) is intentionally NOT implemented here; the
 * structs/bring-up are laid out so an offload layer can be added later.
 *
 * SOURCES
 *   - re-notes/xrdp-datapath-abi.md : THE register/descriptor/bring-up spec.
 *   - GPL FPM driver  (asuswrt-merlin.ng .../char/fpm/impl1/fpm_core.c,
 *     fpm_priv.h) : FPM pool init + token alloc(read)/free(write) + token->buf.
 *   - GPL enet ring  (asuswrt-merlin.ng .../net/enet/impl7/enet_ring.c) : the
 *     self-refilling CPU-RX data ring + ownership poll + rest_desc re-arm.
 *   - GPL 416L05 rdp_cpu_ring_defs.h/_inline.h : CPU_RX_DESCRIPTOR word layout.
 *
 * MICROCODE NOTE
 *   On REAL hardware the 8 Runner cores must be loaded with the proprietary
 *   Runner microcode before any packet moves (ABI sec 0). That blob is NOT
 *   redistributable. This driver requests it via request_firmware() but treats
 *   absence as non-fatal when the "emulated" mode is selected (module param
 *   runner_emulated=1, or DT property brcm,runner-emulated). Against the QEMU
 *   Runner model the emulator fakes Runner behaviour, so NO microcode is
 *   loaded and the driver runs the full ring datapath without the blob.
 */

#define pr_fmt(fmt) "bcm4916-runner: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/firmware.h>
#include <linux/fs.h>		/* filp_open/kernel_read: XPHY fw from /rom/etc/fw (4.19 trial) */
#include <linux/delay.h>
#include <linux/byteorder/generic.h>
#include <linux/bitops.h>	/* test_and_set_bit/clear_bit for the NAT-C slot map */
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
#include <asm/unaligned.h>	/* get_unaligned_le32() etc. (pre-6.12 location) */
#else
#include <linux/unaligned.h>
#endif
#include <linux/in.h>
#include <linux/debugfs.h>
#include <linux/mii.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
#include <linux/kallsyms.h>	/* resolve stock built-in MDIO accessor (trial) */
#endif

#include "bcm4916_runner.h"
#include "flow_offload.h"

/*
 * Kernel-API compat. This single source builds against two very different
 * trees: the on-silicon Broadcom vendor kernel (a 4.19 fork) for the live
 * trial, and current mainline (6.x/7.x) for the QEMU harness + upstreaming.
 * A few net/platform APIs differ. The vendor 4.19 fork additionally predates
 * (or strips) some 4.x helpers, so gate on < 5.0 rather than the exact add
 * version: the fork is < 5.0 and the mainline harness is >= 6.x, which splits
 * the two cleanly.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
/* platform_get_irq_byname_optional() (no error print on a missing entry)
 * landed in 5.4; the plain variant is functionally equivalent for us (we treat
 * any negative return as "no IRQ -> poll mode"). */
#define platform_get_irq_byname_optional platform_get_irq_byname

/* netif_napi_add() dropped its explicit weight argument in 6.1. The inner call
 * is not re-expanded (a macro is not recursively painted), so this is safe. */
#define netif_napi_add(dev, napi, poll) \
	netif_napi_add((dev), (napi), (poll), NAPI_POLL_WEIGHT)

/* devm_register_netdev() is absent in this vendor fork; synthesize it from
 * register_netdev() + a devm unwind action (devm_add_action_or_reset exists). */
static inline void runner_compat_unregister_netdev(void *ndev)
{
	unregister_netdev((struct net_device *)ndev);
}
static inline int devm_register_netdev(struct device *dev,
				       struct net_device *ndev)
{
	int ret = register_netdev(ndev);

	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, runner_compat_unregister_netdev,
					ndev);
}
#endif /* < 5.0.0 */

#define DRV_NAME		"bcm4916-runner"
#define RUNNER_FW_NAME		"brcm/bcm4916-runner-microcode.bin"

/* Ring geometry. Depth must fit number_of_entries[10:0] (<=2047). */
#define RX_RING_DEPTH		256	/* runner-written delivery ring */
#define FEED_RING_DEPTH		256	/* host-posted empty-buffer ring */
#define TX_RING_DEPTH		256
#define RX_BUF_SIZE		2048	/* one DDR buffer per RX frame */
#define RX_NAPI_BUDGET		64

/* First-light physical port: the 1G EGPHY port_gphy1 (eth2-class) = UNIMAC inst,
 * EGPHY addr+1, BBH_ID. The same index is used for UNIMAC inst, BBH_ID and the
 * dispatcher VIQ (VIQ == bbh_id). Tune on silicon (emac->bbh_id is in rdpa.ko). */
#define RUNNER_FIRST_PORT	1

static bool runner_emulated;
module_param(runner_emulated, bool, 0444);
MODULE_PARM_DESC(runner_emulated,
		 "Skip proprietary Runner microcode load (QEMU emulator path).");

static bool mac_loopback;
module_param(mac_loopback, bool, 0444);
MODULE_PARM_DESC(mac_loopback,
		 "Enable UNIMAC local loopback (TX->RX) to validate the full "
		 "datapath on silicon with no cable/link partner.");

static uint qm_enable = QM_ENABLE_CTRL_STOCK;
module_param(qm_enable, uint, 0444);
MODULE_PARM_DESC(qm_enable,
		 "QM GLOBAL_CFG_QM_ENABLE_CTRL value (0=skip). Stock=0x307; bit1 "
		 "REORDER_CREDIT gates the CPU_TX egress delayed-VIQ credit release.");

static bool serdes_fw_load;
module_param(serdes_fw_load, bool, 0444);
MODULE_PARM_DESC(serdes_fw_load,
		 "Load the merlin16_shortfin 10G XPORT SerDes firmware (" SERDES_FW_NAME
		 ") and start the uC. Opt-in (HW only); first step of 10G bring-up.");

static uint serdes_core;	/* which merlin16 core (0/1/2) the 10G port uses */
module_param(serdes_core, uint, 0444);
MODULE_PARM_DESC(serdes_core, "merlin16 SerDes core index for the 10G port (default 0).");

static uint serdes_lane;	/* lane within the core */
module_param(serdes_lane, uint, 0444);
MODULE_PARM_DESC(serdes_lane, "merlin16 SerDes lane index for the 10G port (default 0).");

static int port10g = -1;	/* xport_port_id of the 10G port to bring up (-1=off) */
module_param(port10g, int, 0444);
MODULE_PARM_DESC(port10g,
		 "Bring up a 10G XPORT MAC + BBH: xport_port_id 0 = eth0/port5 (internal "
		 "XPHY), 2 = eth1/port6 (merlin16). -1 = off (default). Opt-in, HW only.");

/* ---- Route A: QM + TM-core egress (opt-in; see bcm4916_runner.h + spec 11) --
 * route_a brings up the QM + a RUNNER_GRP + BBH_TX QMQ binding so an injected
 * CPU_TX PD egresses out a LAN port (the stock image_2 thread routes through the
 * TM/QM cores, not direct-to-BBH). The ★SILICON values below are rdpa.ko-only.
 *
 * ★LIVE-PINNED 2026-06-24 (stock oracle, re-notes/realhw/11 "LIVE ORACLE
 * RESULTS"): the QM-fed LAN egress instance is BBH_TX[1] -> route_a_bbh_inst=1.
 * Two queue/grp candidates (logical->physical queue map still TBD):
 *   B (preferred, LAN=DS_TM): route_a_queue~80 route_a_grp=0 tm_bb_id=7 tm_task=3
 *   A (literal q0,    US_TM): route_a_queue=0  route_a_grp=1 tm_bb_id=6 tm_task=4
 * Defaults stay 0 (opt-in/safe); pass the candidate set on insmod and iterate. */
static bool route_a;
module_param(route_a, bool, 0444);
MODULE_PARM_DESC(route_a,
		 "Bring up QM + TM-core egress so CPU_TX PDs reach BBH_TX (opt-in, "
		 "HW only). Needs the route_a_* values pinned from a stock oracle.");

static uint route_a_grp;	/* ★SILICON RUNNER_GRP index that drains the LAN queue */
module_param(route_a_grp, uint, 0444);
MODULE_PARM_DESC(route_a_grp, "QM RUNNER_GRP index for LAN egress (oracle).");

static uint route_a_queue;	/* ★SILICON QM queue number for the LAN egress */
module_param(route_a_queue, uint, 0444);
MODULE_PARM_DESC(route_a_queue, "QM queue number for LAN egress (oracle).");

static uint route_a_tm_bb_id;	/* ★SILICON BB_ID of the TM Runner core */
module_param(route_a_tm_bb_id, uint, 0444);
MODULE_PARM_DESC(route_a_tm_bb_id, "BB_ID of the TM Runner core draining the queue (oracle).");

static uint route_a_tm_task;	/* ★SILICON TM core egress thread number */
module_param(route_a_tm_task, uint, 0444);
MODULE_PARM_DESC(route_a_tm_task, "TM-core egress thread number (oracle).");

static uint route_a_bbh_inst;	/* ★SILICON BBH_TX instance for the LAN port */
module_param(route_a_bbh_inst, uint, 0444);
MODULE_PARM_DESC(route_a_bbh_inst, "BBH_TX instance index for the LAN egress port (default 0).");

/* ------------------------------------------------------------------------- */
struct runner_priv {
	struct platform_device	*pdev;
	struct device		*dev;
	struct net_device	*ndev;

	void __iomem		*xrdp;		/* whole rdpa window (0x82000000) */
	void __iomem		*ethphytop;	/* eth-phy-top/egphy/mdio region (0x837f0000) */
	void __iomem		*serdes;	/* merlin16 serdes indirect window (0x837ff500) */
	void __iomem		*fpm;		/* xrdp + XRDP_OFF_FPM */
	void __iomem		*rnr_mem[XRDP_RNR_CORES];	/* per-core data SRAM */
	void __iomem		*rnr_regs[XRDP_RNR_CORES];	/* per-core ctl regs */

	/* FPM pool (slow-path RX/TX buffers) */
	void			*pool_vbase;
	dma_addr_t		pool_pbase;
	u32			pool_size;
	u32			chunk_size;

	/* CPU-RX delivery ring (runner-written) + host-fed FEED ring (MPM-free) */
	struct runner_rx_desc	*rx_ring;	/* coherent DDR (runner fills) */
	dma_addr_t		rx_ring_phys;
	u32			rx_head;	/* host read_idx into rx_ring */

	struct runner_feed_desc	*feed_ring;	/* coherent DDR (host posts bufs) */
	dma_addr_t		feed_ring_phys;
	u32			feed_widx;	/* host feed write_idx */
	/* shared RX buffer pool fed to the runner; phys<->virt is arithmetic */
	void			*rx_pool_vbase;
	dma_addr_t		rx_pool_pbase;

	int			rx_irq;
	struct napi_struct	napi;

	/* CPU-TX ring (descriptors in Runner SRAM region of the window) */
	struct runner_tx_desc	*tx_ring;	/* coherent DDR staging */
	dma_addr_t		tx_ring_phys;
	u32			tx_write_idx;

	bool			fw_loaded;

	/* HW flow offload (Phase 1: L2 + VLAN) */
	struct xrdp_offload	offload;
	void __iomem		*natc;		/* xrdp + XRDP_OFF_NATC */
	DECLARE_BITMAP(natc_occ, NATC_TABLE_SLOTS); /* hash-slot occupancy (RE 01 §3) */
	struct dentry		*dbg;		/* debugfs dir */
};

/* ============================ FPM pool driver ============================ *
 * Ported from GPL fpm_core.c / fpm_priv.h. Alloc = READ the pool0
 * alloc/dealloc register; free = WRITE the token back. buffer = pool_vbase +
 * token_index * chunk_size. (fpm_priv.h __fpm_alloc_token / __fpm_free_token /
 * __fpm_token_to_buffer; fpm_core.c init_hw lines ~820-875.)
 */
static u32 fpm_alloc_token(struct runner_priv *p, int size)
{
	u32 token = readl(p->fpm + FPM_POOL0_ALLOC_DEALLOC);

	if (token & FPM_TOKEN_VALID)
		return token;
	return 0;
}

static void __maybe_unused fpm_free_token(struct runner_priv *p, u32 token)
{
	writel(token, p->fpm + FPM_POOL0_ALLOC_DEALLOC);
}

static void *fpm_token_to_buf(struct runner_priv *p, u32 token)
{
	return (u8 *)p->pool_vbase + FPM_TOKEN_INDEX(token) * p->chunk_size;
}

static dma_addr_t fpm_token_to_phys(struct runner_priv *p, u32 token)
{
	return p->pool_pbase + FPM_TOKEN_INDEX(token) * p->chunk_size;
}

/* FPM token -> runner buffer number (fpm_convert_fpm_token_to_rdp_token):
 * index[16:0] | pool<<17. [VERIFIED vs fpm_core.c:376] */
static u32 fpm_token_to_bn(u32 token)
{
	return FPM_TOKEN_INDEX(token) | (FPM_TOKEN_POOL(token) << 17);
}

/* small helper so we don't busy-loop forever if INIT_MEM never clears */
static void readl_poll_drain(struct runner_priv *p)
{
	int i;

	for (i = 0; i < 1000; i++) {
		if (!(readl(p->fpm + FPM_CTL) & FPM_CTL_INIT_MEM))
			return;
		udelay(10);
	}
}

/*
 * FPM pool bring-up. Mirrors fpm_core.c init_hw: reserve a 32MB/512B-chunk
 * coherent pool, program the pool base + chunk size, init + enable pool0.
 * (fpm_core.c: FPM_SET_POOL_INIT/ENABLE, FPM_SET_CHUNK_SIZE,
 * FPM_SET_POOL_BASE_ADDR.) ABI sec 3.1 / bring-up step 3.
 */
static int fpm_pool_init(struct runner_priv *p)
{
	u32 ctl;

	p->pool_size  = FPM_POOL_SIZE_512;
	p->chunk_size = FPM_CHUNK_SIZE_DEFAULT;

	p->pool_vbase = dmam_alloc_coherent(p->dev, p->pool_size,
					    &p->pool_pbase, GFP_KERNEL);
	if (!p->pool_vbase)
		return -ENOMEM;

	/* chunk size = 512B (FP_BUF_SIZE_512 == 0) */
	writel(FPM_FP_BUF_SIZE_512, p->fpm + FPM_POOL1_CFG1);

	/* pool base address (>>0, masked to 4-byte align per HW) */
	writel((u32)p->pool_pbase & FPM_POOL_BASE_ADDR_MASK,
	       p->fpm + FPM_POOL1_CFG2);

	/* store head/tail pad scratch like the GPL driver (FPM_SET_CTL_SPARE) */
	writel(FPM_NET_BUF_HEAD_PAD << 16, p->fpm + FPM_SPARE);

	/* init pool memory, then enable pool0 */
	ctl = readl(p->fpm + FPM_CTL);
	writel(ctl | FPM_CTL_INIT_MEM, p->fpm + FPM_CTL);
	/* HW clears INIT_MEM when done; emulator clears immediately */
	readl_poll_drain(p);
	ctl = readl(p->fpm + FPM_CTL);
	writel(ctl | FPM_CTL_POOL1_ENABLE, p->fpm + FPM_CTL);

	dev_info(p->dev, "FPM pool: %u MB, chunk %u, pbase %pad\n",
		 p->pool_size >> 20, p->chunk_size, &p->pool_pbase);
	return 0;
}

/* ====================== CPU RX feed + delivery rings ===================== *
 * MPM-free first-light model (spec Wave-4/5). The host owns a contiguous RX
 * buffer pool and posts empty buffers as 40-bit ABS pointers into a FEED ring;
 * the Runner pulls a feed entry, DMAs an incoming frame into that buffer and
 * writes a CPU_RX_DESCRIPTOR into the DELIVERY ring, advancing the delivery
 * ring's write_idx. The host polls write_idx vs its own read_idx (NOT a
 * per-descriptor ownership bit - wave-1 was wrong), consumes the frame, and
 * recycles the buffer back to the feed ring (re-post + feed doorbell).
 *
 *   feed ring  : host -> runner   (empty buffers, CPU_FEED_DESCRIPTOR 8B)
 *   delivery   : runner -> host   (filled CPU_RX_DESCRIPTOR 16B)
 *
 * Sources: CPU_FEED_DESCRIPTOR / CPU_RX_DESCRIPTOR (rdp_cpu_ring_defs.h),
 * rdd_cpu_inc_feed_ring_write_idx (feed doorbell), enet impl7 ring model.
 */
#define PSRAM_CPU_RING_DESC_TABLE	0x3000	/* RX delivery ring cfg, core 3 */

/* phys<->virt within the contiguous RX buffer pool (slot index == buffer #) */
static inline dma_addr_t rx_buf_phys(struct runner_priv *p, u32 i)
{
	return p->rx_pool_pbase + (dma_addr_t)i * RX_BUF_SIZE;
}
static inline void *rx_buf_virt(struct runner_priv *p, u32 i)
{
	return (u8 *)p->rx_pool_vbase + (size_t)i * RX_BUF_SIZE;
}
static inline u32 rx_buf_index(struct runner_priv *p, u64 phys)
{
	return (u32)((phys - p->rx_pool_pbase) / RX_BUF_SIZE);
}

/* write feed slot 'slot' to point at buffer 'buf_idx' (ABS 40-bit pointer) */
static void feed_post(struct runner_priv *p, u32 slot, u32 buf_idx)
{
	struct runner_feed_desc *fd = &p->feed_ring[slot];
	dma_addr_t bp = rx_buf_phys(p, buf_idx);
	u32 w1 = FEED_W1_ABS |
		 (((u32)(bp >> 32) & FEED_W1_PTR_HI_MASK) << FEED_W1_PTR_HI_SHIFT);

	fd->ptr_low = cpu_to_be32((u32)bp);
	fd->w1 = cpu_to_be32(w1);
}

/* feed doorbell: publish the feed ring's write_idx (BE u16 @ +6 of its cfg) */
static void feed_doorbell(struct runner_priv *p, u16 widx)
{
	dma_wmb();
	iowrite16(cpu_to_be16(widx),
		  p->rnr_mem[CPU_RX_RING_CORE] + RDD_FEED_RING_DESC_TABLE +
		  RING_CFG_WRITE_IDX_OFF);
}

/* delivery ring write_idx (runner-advanced) / read_idx (host-advanced) */
static u16 rx_deliv_write_idx(struct runner_priv *p)
{
	__be16 be = ioread16(p->rnr_mem[CPU_RX_RING_CORE] +
			     PSRAM_CPU_RING_DESC_TABLE + RING_CFG_WRITE_IDX_OFF);
	return be16_to_cpu(be);
}
static void rx_deliv_set_read_idx(struct runner_priv *p, u16 idx)
{
	iowrite16(cpu_to_be16(idx), p->rnr_mem[CPU_RX_RING_CORE] +
		  PSRAM_CPU_RING_DESC_TABLE + RING_CFG_READ_IDX_OFF);
}

/* publish a CPU_RING_DESCRIPTOR control block into a runner core's data SRAM.
 * HW field encodings [VERIFIED vs SDK rdd_ring_init disasm]:
 *  - number_of_entries field = depth >> res_shift. ★The RESOLUTION DIFFERS by
 *    ring: the RX delivery + TX rings are 32-resolution (res_shift=5,
 *    CPU_RING_SIZE_32_RESOLUTION; runner wraps at field<<5); the FEED ring is
 *    64-resolution (res_shift=6). Using >>6 for the RX ring told the microcode
 *    the ring was half-size -> wrap math broke -> it never delivered.
 *  - size_of_entry is the entry size in BYTES (sizeof desc), not log2.
 *  - base_addr = full DMA bus phys: low32 @+8, high byte @+15. No shift.
 *  - w0: half@0 = size_of_entry[15:11] | number_of_entries[10:0];
 *    half@2 = interrupt_id; w1 (drop_counter, write_idx)=0;
 *    w3 = read_idx[15:0]=0 | base_addr_high@f. All big-endian.
 */
#define RING_RES_32	5	/* RX delivery + TX rings */
#define RING_RES_64	6	/* FEED ring */
static void ring_publish(struct runner_priv *p, int core, u32 tbl_off,
			 dma_addr_t phys, u32 depth, u32 entry_sz, u16 irq,
			 u32 res_shift)
{
	struct runner_ring_cfg cfg = {};

	cfg.w0 = cpu_to_be32(((entry_sz & 0x1f) << 27) |
			     (((depth >> res_shift) & 0x7ff) << 16) | irq);
	cfg.base_addr_low = cpu_to_be32((u32)phys);
	cfg.w3 = cpu_to_be32((u32)(phys >> 32) & 0xff);
	memcpy_toio(p->rnr_mem[core] + tbl_off, &cfg, sizeof(cfg));
}

static int rx_ring_alloc(struct runner_priv *p)
{
	u32 i;

	p->rx_ring = dmam_alloc_coherent(p->dev,
					 RX_RING_DEPTH * sizeof(*p->rx_ring),
					 &p->rx_ring_phys, GFP_KERNEL);
	p->feed_ring = dmam_alloc_coherent(p->dev,
					   FEED_RING_DEPTH * sizeof(*p->feed_ring),
					   &p->feed_ring_phys, GFP_KERNEL);
	p->rx_pool_vbase = dmam_alloc_coherent(p->dev,
					       FEED_RING_DEPTH * RX_BUF_SIZE,
					       &p->rx_pool_pbase, GFP_KERNEL);
	if (!p->rx_ring || !p->feed_ring || !p->rx_pool_vbase)
		return -ENOMEM;

	/* delivery ring starts empty (runner writes it); host read_idx = 0 */
	p->rx_head = 0;

	/* post depth-1 empty buffers (leave one slot free => not "full"==empty) */
	for (i = 0; i < FEED_RING_DEPTH - 1; i++)
		feed_post(p, i, i);
	p->feed_widx = FEED_RING_DEPTH - 1;
	return 0;
}

static void rx_ring_publish(struct runner_priv *p)
{
	/* delivery ring (runner -> host), 16B descriptors, on core 3, 32-res */
	ring_publish(p, CPU_RX_RING_CORE, PSRAM_CPU_RING_DESC_TABLE,
		     p->rx_ring_phys, RX_RING_DEPTH,
		     sizeof(struct runner_rx_desc), p->rx_irq & 0xffff, RING_RES_32);
	/* feed ring (host -> runner), 8B descriptors, on core 3, 64-res */
	ring_publish(p, CPU_RX_RING_CORE, RDD_FEED_RING_DESC_TABLE,
		     p->feed_ring_phys, FEED_RING_DEPTH,
		     sizeof(struct runner_feed_desc), 0, RING_RES_64);
	/* ring the feed doorbell so the runner sees the pre-posted buffers */
	feed_doorbell(p, p->feed_widx);
}

static int runner_rx_poll(struct napi_struct *napi, int budget)
{
	struct runner_priv *p = container_of(napi, struct runner_priv, napi);
	u16 widx = rx_deliv_write_idx(p);
	int done = 0;

	while (done < budget && (u16)p->rx_head != widx) {
		struct runner_rx_desc *d = &p->rx_ring[p->rx_head];
		u32 w0 = be32_to_cpu(d->word0);
		u32 w1 = be32_to_cpu(d->word1);
		u64 bphys = w0 | ((u64)((w1 >> RXD_W1_PTR_HI_SHIFT) &
					RXD_W1_PTR_HI_MASK) << 32);
		u16 len = (w1 >> RXD_W1_PKT_LEN_SHIFT) & RXD_W1_PKT_LEN_MASK;
		u32 bidx = rx_buf_index(p, bphys);
		struct sk_buff *skb;

		if (bidx >= FEED_RING_DEPTH) {
			/* descriptor points outside our pool: cannot recycle */
			p->ndev->stats.rx_errors++;
			p->rx_head = (p->rx_head + 1) % RX_RING_DEPTH;
			done++;
			continue;
		}

		if (len && len <= RX_BUF_SIZE) {
			dma_sync_single_for_cpu(p->dev, bphys, len,
						DMA_FROM_DEVICE);
			skb = napi_alloc_skb(napi, len);
			if (skb) {
				skb_put_data(skb, rx_buf_virt(p, bidx), len);
				skb->protocol = eth_type_trans(skb, p->ndev);
				p->ndev->stats.rx_packets++;
				p->ndev->stats.rx_bytes += len;
				napi_gro_receive(napi, skb);
			} else {
				p->ndev->stats.rx_dropped++;
			}
		} else {
			p->ndev->stats.rx_errors++;
		}

		/* recycle this buffer back to the feed ring */
		dma_sync_single_for_device(p->dev, rx_buf_phys(p, bidx),
					   RX_BUF_SIZE, DMA_FROM_DEVICE);
		feed_post(p, p->feed_widx % FEED_RING_DEPTH, bidx);
		p->feed_widx++;

		p->rx_head = (p->rx_head + 1) % RX_RING_DEPTH;
		done++;
	}

	if (done) {
		feed_doorbell(p, p->feed_widx);
		rx_deliv_set_read_idx(p, p->rx_head);
	}

	if (done < budget) {
		napi_complete_done(napi, done);
		if (p->rx_irq > 0)
			enable_irq(p->rx_irq);
	}
	return done;
}

static irqreturn_t runner_rx_isr(int irq, void *dev_id)
{
	struct runner_priv *p = dev_id;

	disable_irq_nosync(irq);
	napi_schedule(&p->napi);
	return IRQ_HANDLED;
}

/* ============================== CPU TX path ============================== *
 * ndo_start_xmit: copy the skb into an FPM buffer, build a 16B big-endian
 * RING_CPU_TX_DESCRIPTOR (ABI 2.2) in FPM-token mode (abs=0), stage it into the
 * TX ring slot, advance write_idx and ring the SRAM index "doorbell" in every
 * serving Runner core (ABI 5bis-G2).
 */
static void tx_ring_doorbell(struct runner_priv *p, u16 write_idx)
{
	/* dmb oshst before publishing the index (ABI 5bis-G2 step 6) */
	dma_wmb();

	/*
	 * CPU_TX_RING_INDICES_VALUES_TABLE entry = { read_idx@+0, write_idx@+2 }
	 * (BE u16 each), on Runner core 2 [SDK + descriptor RE]. The runner reads
	 * write_idx from +2; the WRITE of write_idx IS the doorbell. (Previously
	 * this wrote +0 = the read_idx slot on every core -> the runner never saw
	 * a TX and our write_idx clobbered read_idx.)
	 */
	iowrite16(cpu_to_be16(write_idx),
		  p->rnr_mem[CPU_TX_RING_CORE] + CPU_TX_RING_INDICES_OFF + 2);

	/*
	 * The CPU_TX thread is edge-woken PER FRAME by a CFG_CPU_WAKEUP write to
	 * its thread number (core2/thread6), issued after the descriptor+index are
	 * published [SDK rdd_cpu_tx_ring.c:233]. The index bump alone does NOT make
	 * the thread run; this wakeup is the real doorbell.
	 */
	wmb();
	writel(RNR_CPU_TX_THREAD & RNR_CFG_CPU_WAKEUP_THREAD_MASK,
	       p->rnr_regs[CPU_TX_RING_CORE] + RNR_CFG_CPU_WAKEUP);
}

static netdev_tx_t runner_start_xmit(struct sk_buff *skb,
				     struct net_device *ndev)
{
	struct runner_priv *p = netdev_priv(ndev);
	struct runner_tx_desc *d;
	u32 token, len = skb->len;
	void *buf;
	dma_addr_t bphys;

	if (len > p->chunk_size) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	token = fpm_alloc_token(p, len);
	if (!token) {
		/* no buffer: stop and retry */
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}

	buf = fpm_token_to_buf(p, token);
	bphys = fpm_token_to_phys(p, token);
	skb_copy_from_linear_data(skb, buf, len);
	dma_sync_single_for_device(p->dev, bphys, len, DMA_TO_DEVICE);

	/* build descriptor in host order, then byte-swap into ring (ABI 2.2) */
	d = &p->tx_ring[p->tx_write_idx];
	/* word0: is_egress + length; when Route A is up also set first_level_q so the
	 * TM core enqueues the PD to the QM queue bound to BBH_TX (else the queue is
	 * resolved from the vport tx-flow table, which needs the QoS table - ★SILICON). */
	d->word0 = cpu_to_be32(TXD_W0_IS_EGRESS |
			       ((len & TXD_W0_PKT_LEN_MASK) << TXD_W0_PKT_LEN_SHIFT) |
			       (route_a ? ((route_a_queue & TXD_W0_FIRST_LEVEL_Q_MASK)
					   << TXD_W0_FIRST_LEVEL_Q_SHIFT) : 0));
	d->word1 = 0;
	/*
	 * is_emac=1 (UNIMAC egress for the dumb-pipe CPU port), abs=0 (FPM token
	 * mode). The egress EMAC port [27:20] MUST name the port we actually
	 * brought up (UNIMAC inst / BBH_TX = RUNNER_FIRST_PORT); leaving it 0
	 * directed frames at the unconfigured EMAC port 0, so the Runner TX
	 * thread stalled after the BBH_TX FIFO filled (~3 frames) and read_idx
	 * froze. (DSA tag-based egress selection is a later step.)
	 *
	 * abs=0 (FPM-token) egress: word3 = fpm_bn0[19:0] | fpm_sop[29:20]
	 * [VERIFIED vs RING_CPU_TX_DESCRIPTOR + fpm_core.c:376/rdd_cpu_tx.h:119].
	 * bn0 = fpm_convert_fpm_token_to_rdp_token(token); SOP = 0 (we copied the
	 * frame to the FPM buffer start). do_not_recycle stays 0 so the runner
	 * auto-frees the FPM buffer after transmit (no host reclaim needed).
	 */
	d->word2 = cpu_to_be32(TXD_W2_IS_EMAC |
			       (((u32)RUNNER_FIRST_PORT & TXD_W2_PORT_MASK)
				<< TXD_W2_PORT_SHIFT));
	d->word3 = cpu_to_be32((fpm_token_to_bn(token) & TXD_W3_FPM_BN0_MASK) |
			       ((0u & TXD_W3_FPM_SOP_MASK) << TXD_W3_FPM_SOP_SHIFT));

	p->tx_write_idx = (p->tx_write_idx + 1) % TX_RING_DEPTH;
	tx_ring_doorbell(p, p->tx_write_idx);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
}

/* [PINNED 2026-06-22: the GT-BE98 (96813GW) build uses rdp project BCM6813 (NOT _FPI; FPI is the
 * alt Flow-Provisioning accel — make.common:796), so CPU_TX_RING_DESCRIPTOR_TABLE = 0x33e0 on
 * Runner core 2 (BCM6813 rdd_runner_defs_auto.h:1422). 0x3360 was the wrong _FPI variant.
 * RDD/core-data-memory offset; abs = rnr_mem[2] + this. (RX 0x3000 + TX indices 0x29c8 agree.) */
#define PSRAM_CPU_TX_RING_DESC_TABLE	0x33e0	/* RDD off, core 2 [SDK BCM6813] */

static int tx_ring_alloc(struct runner_priv *p)
{
	p->tx_ring = dmam_alloc_coherent(p->dev,
					 TX_RING_DEPTH * sizeof(*p->tx_ring),
					 &p->tx_ring_phys, GFP_KERNEL);
	if (!p->tx_ring)
		return -ENOMEM;
	p->tx_write_idx = 0;

	/* TX ring descriptor table lives in RNR core-2 data memory (not PSRAM)
	 * [SDK RDD_CPU_TX_RING_DESCRIPTOR_TABLE_ADDRESS_ARR core 2]. */
	ring_publish(p, CPU_TX_RING_CORE, PSRAM_CPU_TX_RING_DESC_TABLE,
		     p->tx_ring_phys, TX_RING_DEPTH,
		     sizeof(struct runner_tx_desc), 0, RING_RES_32);
	return 0;
}

/* ============================ NAT-C offload I/O ========================== *
 * The open analog of drv_natc_key_result_entry_var_size_ctx_add. The install
 * sequence is RE-PINNED (re-notes/re-firmware/01-natc-abi.md §1):
 *   mask key -> hash-derived slot (open-addressed probe) -> stage key+result ->
 *   issue command 3 (there is NO command 4; delete = a cmd-3 write of an
 *   invalidated entry, §7).
 *
 * NOTE on staging offsets: real silicon stages a single 72-byte BE key+result
 * window at the NAT-C ENG sub-block (command @engine+0x10, key @+0x30) and copies
 * the full 124-byte context to the DDR result table at result_base+idx*entry_size
 * (§1/§5/§6). Those ABSOLUTE offsets are UNRESOLVED (§Unresolved: need a live
 * devmem/FDT read on silicon), so the PSRAM staging windows below remain a
 * driver<->QEMU-model contract; the pinned corrections applied here are the
 * hash-derived slot and the cmd-3 delete.
 */

/* XOR-fold hash of the masked key -> base slot (RE 01 §3, mode 0). */
static u32 natc_hash(const struct natc_key *key)
{
	u32 h = __swab32(NATC_HASH_SEED);	/* seed 0x4899b351, byteswapped */
	int i;

	for (i = 0; i < 4; i++)
		h ^= __swab32((__force u32)key->w[i]);	/* byteswap32(BE key word) */
	/* fold the 32-bit hash to NATC_IDX_BITS index bits (§3, N<=15 form) */
	return (((h ^ (h >> NATC_IDX_BITS)) & (NATC_TABLE_SLOTS - 1)) ^
		(h >> (2 * NATC_IDX_BITS))) & (NATC_TABLE_SLOTS - 1);
}

int xrdp_natc_add(struct xrdp_offload *o, const struct natc_key *key,
		  const struct fc_ucast_ctx *ctx, u32 *idx_out)
{
	struct runner_priv *p = o->drv;
	u32 base = natc_hash(key);
	u32 idx, probe;

	/* open-addressed probe from the hash slot for a free entry (§3-4) */
	for (probe = 0; probe < NATC_TABLE_SLOTS; probe++) {
		idx = (base + probe) & (NATC_TABLE_SLOTS - 1);
		if (!test_and_set_bit(idx, p->natc_occ))
			break;
	}
	if (probe == NATC_TABLE_SLOTS)
		return -ENOSPC;

	/* stage {key, context} + issue command 3 (add) */
	memcpy_toio(p->xrdp + XRDP_OFF_PSRAM + NATC_STAGE_KEY, key->w,
		    sizeof(key->w));
	memcpy_toio(p->xrdp + XRDP_OFF_PSRAM + NATC_STAGE_CTX, ctx->buf,
		    ctx->len);
	writel(idx, p->xrdp + XRDP_OFF_PSRAM + NATC_INDIR_INDEX);
	dma_wmb();
	writel(NATC_CMD_ADD, p->xrdp + XRDP_OFF_PSRAM + NATC_INDIR_CMD);

	*idx_out = idx;
	return 0;
}

void xrdp_natc_del(struct xrdp_offload *o, const struct natc_key *key, u32 idx)
{
	struct runner_priv *p = o->drv;

	if (idx >= NATC_TABLE_SLOTS)
		return;
	/*
	 * Delete = a command-3 write of an INVALIDATED entry (RE 01 §7: there is no
	 * command 4). We stage the key + index and re-issue cmd 3 with a zeroed
	 * context; the model treats a cmd-3 whose staged context has valid=0
	 * (FCU_VALID_BIT clear) as an invalidate of that slot.
	 */
	memcpy_toio(p->xrdp + XRDP_OFF_PSRAM + NATC_STAGE_KEY, key->w,
		    sizeof(key->w));
	memset_io(p->xrdp + XRDP_OFF_PSRAM + NATC_STAGE_CTX, 0, XPE_CTX_ENTRY_MAX);
	writel(idx, p->xrdp + XRDP_OFF_PSRAM + NATC_INDIR_INDEX);
	dma_wmb();
	writel(NATC_CMD_ADD, p->xrdp + XRDP_OFF_PSRAM + NATC_INDIR_CMD);

	clear_bit(idx, p->natc_occ);
}

void xrdp_natc_stats(struct xrdp_offload *o, u32 idx, u64 *pkts, u64 *bytes)
{
	/*
	 * Per-flow stats are CNPL counters (ABI sec 3.2). The CNPL read-back is
	 * not yet pinned; return 0 (the flowtable still ages the entry out by
	 * its own timeout). Phase 1.x: read the CNPL counter for this idx.
	 */
	*pkts = 0;
	*bytes = 0;
}

/* ============================ microcode load ============================ */
static int runner_load_microcode(struct runner_priv *p)
{
	const struct firmware *fw;
	int ret;

	if (runner_emulated ||
	    of_property_read_bool(p->dev->of_node, "brcm,runner-emulated")) {
		dev_info(p->dev,
			 "emulated mode: skipping proprietary Runner microcode\n");
		p->fw_loaded = false;
		return 0;
	}

	ret = request_firmware(&fw, RUNNER_FW_NAME, p->dev);
	if (ret) {
		dev_warn(p->dev,
			 "Runner microcode '%s' absent (%d); HW datapath will NOT move packets. Use runner_emulated=1 for QEMU.\n",
			 RUNNER_FW_NAME, ret);
		p->fw_loaded = false;
		return 0;	/* non-fatal: lets the driver bind for emulation */
	}

	/*
	 * Real-HW path: the firmware is the "RFW1" blob built from the SDK's GPL
	 * per-core microcode arrays. Layout [re-notes/realhw/10-runner-bringup-spec.md]:
	 *   32B header: 'R','F','W','1' | u32 ver | u32 num_cores | u32 hdr_size |
	 *               u32 entry_size | u32 total | u32 rsvd[2]   (all little-endian)
	 *   num_cores x { u32 inst_off, inst_len, pred_off, pred_len }  (entry i == core i)
	 *   payload: 8 inst images (32KB) then 8 pred images (1KB, u16-packed).
	 * Copy each core's inst image to RNR_INST[c] (=rnr_mem[c]+0x10000) and pred to
	 * RNR_PRED[c] (=rnr_mem[c]+0x1c000). Core enable/wakeup happens in runner_init.
	 */
	{
		const u8 *d = fw->data;
		u32 ncores, hdr_size, entry_size, c;

		if (fw->size < 32 || memcmp(d, "RFW1", 4)) {
			dev_err(p->dev, "runner fw: bad magic or short (%zu)\n", fw->size);
			release_firmware(fw);
			return -EINVAL;
		}
		ncores     = get_unaligned_le32(d + 8);
		hdr_size   = get_unaligned_le32(d + 12);
		entry_size = get_unaligned_le32(d + 16);
		if (ncores > XRDP_RNR_CORES || entry_size < 16 ||
		    (u64)hdr_size + (u64)ncores * entry_size > fw->size) {
			dev_err(p->dev, "runner fw: bad table (cores=%u)\n", ncores);
			release_firmware(fw);
			return -EINVAL;
		}
		for (c = 0; c < ncores; c++) {
			const u8 *e = d + hdr_size + (size_t)c * entry_size;
			u32 io = get_unaligned_le32(e + 0);
			u32 il = get_unaligned_le32(e + 4);
			u32 po = get_unaligned_le32(e + 8);
			u32 pl = get_unaligned_le32(e + 12);

			u32 j;

			if (!p->rnr_mem[c] ||
			    (u64)io + il > fw->size || (u64)po + pl > fw->size) {
				dev_err(p->dev, "runner fw: core %u bad extent\n", c);
				release_firmware(fw);
				return -EINVAL;
			}
			/*
			 * The Runner SRAM is BIG-ENDIAN (the stock loader byte-swaps
			 * every word - MWRITE_32). The RFW1 blob stores inst/pred as
			 * native LITTLE-ENDIAN, so a plain memcpy_toio would load
			 * byte-swapped instructions + a packed prediction RAM -> the
			 * cores execute garbage and idle. Match the stock loader:
			 *  - INST: each native u32 -> iowrite32be (== swap4) [rdp_drv_rnr.c]
			 *  - PRED: each native u16 -> a u32 RAM slot (stride 4), BE
			 *    (memcpyl_prediction: read u16, MWRITE_32; RNR_PRED is 512
			 *    u32 slots each holding a 16-bit pred value).
			 */
			for (j = 0; j + 4 <= il; j += 4)
				iowrite32be(get_unaligned_le32(d + io + j),
					    p->rnr_mem[c] + XRDP_RNR_INST_OFF + j);
			for (j = 0; j + 2 <= pl; j += 2)
				iowrite32be(get_unaligned_le16(d + po + j),
					    p->rnr_mem[c] + XRDP_RNR_PRED_OFF + (j / 2) * 4);
		}
		dev_info(p->dev, "Runner microcode loaded: %u cores, %zu bytes\n",
			 ncores, fw->size);
	}
	p->fw_loaded = true;
	release_firmware(fw);
	return 0;
}

/* ============================ data_path_init ============================ *
 * The 24-step Runner bring-up (ABI sec 4 / 5bis-G3). For the dumb-pipe slow
 * path against the emulator we program the minimum: UBUS decode windows, FPM,
 * the CPU rings and (real HW) microcode. The remaining BBH/DSPTCHR/QM/DMA/SBPM
 * per-port register *values* are not yet pinned (ABI sec 6) and are stubbed;
 * the emulator does not require them.
 */
static void runner_ubus_decode_init(struct runner_priv *p)
{
	/* UBUS_SLV address-decode windows [PINNED vs SDK ubus_bridge_init,
	 * XRDP_CFE2/.../BCM6813/data_path_init.c:405; offsets XRDP_AG.h:21360].
	 * Each window = START reg + END reg, each holding a FULL 32-bit address
	 * (no size-mask, no enable bit — active purely by [start,end) bounds).
	 * Program order matches the SDK: dev0, dev1, dev2, vpb, apb. */
	void __iomem *u = p->xrdp + XRDP_OFF_UBUS_SLV;

	writel(0x82a00000, u + 0x14); writel(0x82c00000, u + 0x18); /* dev0 FPM */
	writel(0x82c00000, u + 0x1c); writel(0x82c80000, u + 0x20); /* dev1 QM  */
	writel(0x82c80000, u + 0x24); writel(0x82d00000, u + 0x28); /* dev2 DQM */
	writel(0x82700000, u + 0x04); writel(0x82900000, u + 0x08); /* vpb      */
	writel(0x82900000, u + 0x0c); writel(0x82a00000, u + 0x10); /* apb      */
}

/* ========================= RNR core enable / wakeup ====================== *
 * Bring-up step 2 (spec sec 1 + Wave-3/6). Split in two:
 *   runner_rnr_precfg() - BEFORE microcode: zero each core's data+context SRAM
 *     (so a stale ring/feed table can't be mistaken for valid) and program the
 *     scheduler + PSRAM/DDR base config. Zeroing happens before the microcode
 *     load, which targets the separate INST/PRED SRAM (+0x10000/+0x1c000).
 *   runner_rnr_enable() - LAST (after all blocks + rings are published): set
 *     CFG_GLOBAL_CTRL.EN on every core, then start the CPU host threads via
 *     CFG_CPU_WAKEUP (the write IS the wake): RX core3/thread1, TX core2/thread6.
 */
static void runner_rnr_precfg(struct runner_priv *p)
{
	/* DDR DMA base: DMA_BASE[19:0] = (phys_hi<<12)|(phys_lo>>20), mode bit set.
	 * The runner DMAs RX/TX buffers from this 40-bit-phys region (our pool). */
	u64 phys = p->pool_pbase;
	u32 ddr_base = (((u32)(phys >> 32) << 12) |
			((u32)(phys & 0xffffffff) >> 20)) & RNR_CFG_DDR_CFG_BASE_MASK;
	u32 ddr_cfg = ddr_base | RNR_CFG_DDR_CFG_BUF_SIZE_MODE;
	const u32 zero = RNR_CFG_GEN_CFG_DIS_DMA_OLD_FC |
			 RNR_CFG_GEN_CFG_ZERO_DATA_MEM |
			 RNR_CFG_GEN_CFG_ZERO_CTX_MEM;
	const u32 done = RNR_CFG_GEN_CFG_ZERO_DATA_DONE |
			 RNR_CFG_GEN_CFG_ZERO_CTX_DONE;
	int c, i;

	for (c = 0; c < XRDP_RNR_CORES; c++) {
		void __iomem *r = p->rnr_regs[c];

		if (!r)
			continue;
		/* trigger data+context-mem zeroing, then poll the DONE bits to 1
		 * (the zero bits do NOT self-clear - data_path_init.c:631-647). */
		writel(zero, r + RNR_CFG_GEN_CFG);
		for (i = 0; i < 1000; i++) {
			if ((readl(r + RNR_CFG_GEN_CFG) & done) == done)
				break;
			udelay(10);
		}
		/* scheduler mode + PSRAM / DDR memory base config */
		writel(RNR_CFG_SCH_CFG_VAL,   r + RNR_CFG_SCH_CFG);
		writel(RNR_CFG_PSRAM_CFG_VAL, r + RNR_CFG_PSRAM_CFG);
		writel(ddr_cfg,               r + RNR_CFG_DDR_CFG);
	}
	dev_info(p->dev, "bring-up: RNR pre-cfg done (ddr_base=0x%05x)\n",
		 ddr_base);
}

/*
 * Per-thread INITIAL REGISTER FILE for the CPU RX/TX threads, written into the
 * RNR_CNTXT context SRAM (rnr_mem[core]+0x18000, thread*128 + reg*4, big-endian)
 * AFTER zero-mem + microcode load, BEFORE core enable/wakeup. Without this a woken
 * thread runs on zeroed registers (no entry PC / stack / FIFO ptrs) and does
 * nothing -> our rings never get serviced.
 * [RE rdpa.o rdd_data_structures_init + RNR_CNTXT_ADDRS; values are for THIS
 * BCM6813 microcode image (R0 = the *_cpu_*_wakeup_request label). re-derive R0
 * from firmware_bin/rdd_runner_labels.h if a different microcode is loaded.]
 */
static void runner_thread_regfile_init(struct runner_priv *p)
{
	void __iomem *rx = p->rnr_mem[CPU_RX_RING_CORE] + XRDP_RNR_CNTXT_OFF +
			   RNR_CPU_RX_THREAD * 128;
	void __iomem *tx = p->rnr_mem[CPU_TX_RING_CORE] + XRDP_RNR_CNTXT_OFF +
			   RNR_CPU_TX_THREAD * 128;

	/* CPU_RX: image_3 / core3 / thread1 */
	iowrite32be(0x00001624, rx + 0  * 4);	/* R0  entry: cpu_rx_wakeup_request */
	iowrite32be(0x00003940, rx + 8  * 4);	/* R8  PD_FIFO_TABLE */
	iowrite32be(0x00003840, rx + 9  * 4);	/* R9  UPDATE_FIFO_TABLE */
	iowrite32be(0x00003940, rx + 17 * 4);	/* R17 PD_FIFO_TABLE */
	iowrite32be(0x00003840, rx + 18 * 4);	/* R18 UPDATE_FIFO_TABLE */
	iowrite32be(0x00002bd0, rx + 30 * 4);	/* R30 stack top */
	iowrite32be(0x00000001, rx + 31 * 4);	/* R31 CONST_1 */

	/* CPU_TX: image_2 / core2 / thread6 */
	iowrite32be(0x00000215, tx + 0  * 4);	/* R0  entry: cpu_tx_wakeup_request */
	iowrite32be(0x00000006, tx + 8  * 4);	/* R8  thread number */
	iowrite32be(0x00003340, tx + 30 * 4);	/* R30 stack top */
	iowrite32be(0x00000001, tx + 31 * 4);	/* R31 CONST_1 */

	dev_info(p->dev, "bring-up: CPU thread regfiles initialized (RX c%d/t%d, TX c%d/t%d)\n",
		 CPU_RX_RING_CORE, RNR_CPU_RX_THREAD,
		 CPU_TX_RING_CORE, RNR_CPU_TX_THREAD);
}

static void runner_rnr_enable(struct runner_priv *p)
{
	int c;

	/* run all cores that carry microcode */
	for (c = 0; c < XRDP_RNR_CORES; c++) {
		void __iomem *r = p->rnr_regs[c];
		u32 v;

		if (!r)
			continue;
		v = readl(r + RNR_CFG_GLOBAL_CTRL);
		writel(v | RNR_CFG_GLOBAL_CTRL_EN, r + RNR_CFG_GLOBAL_CTRL);
	}

	/* wake the CPU host threads (the write to CFG_CPU_WAKEUP starts them) */
	dma_wmb();
	writel(RNR_CPU_RX_THREAD & RNR_CFG_CPU_WAKEUP_THREAD_MASK,
	       p->rnr_regs[CPU_RX_RING_CORE] + RNR_CFG_CPU_WAKEUP);
	writel(RNR_CPU_TX_THREAD & RNR_CFG_CPU_WAKEUP_THREAD_MASK,
	       p->rnr_regs[CPU_TX_RING_CORE] + RNR_CFG_CPU_WAKEUP);
	dev_info(p->dev,
		 "bring-up: RNR enabled + CPU threads woken (RX core%d/thr%d, TX core%d/thr%d)\n",
		 CPU_RX_RING_CORE, RNR_CPU_RX_THREAD,
		 CPU_TX_RING_CORE, RNR_CPU_TX_THREAD);
}

/* ========================= SBPM / DSPTCHR / BBH ========================== *
 * Bring-up step 3 (spec secs 4,5,7,8). Minimal first-light block init: just
 * enough to let one MAC port DMA frames through the Runner to the CPU rings.
 * Per-port BBH values still need on-silicon iteration (flagged in the spec).
 */
static int runner_sbpm_init(struct runner_priv *p)
{
	void __iomem *s = p->xrdp + XRDP_OFF_SBPM;
	int i;

	/* trigger the SBPM free-list init, then poll RDY (bit31) */
	writel(SBPM_INIT_FREE_LIST_VAL, s + SBPM_INIT_FREE_LIST);
	for (i = 0; i < 1000; i++) {
		if (readl(s + SBPM_INIT_FREE_LIST) & SBPM_INIT_FREE_LIST_RDY) {
			dev_info(p->dev, "bring-up: SBPM free-list ready\n");
			return 0;
		}
		udelay(10);
	}
	dev_warn(p->dev, "bring-up: SBPM free-list init did not signal RDY\n");
	return 0;	/* non-fatal for first-light */
}

/*
 * Wire the CPU_RX path through the dispatcher [spec Wave-8/9]: create the feeding
 * VIQ (== bbh_id of our ingress port), place the CPU_RX core/thread in a runner
 * group that consumes that VIQ, and point the group's delivery at the CPU_RX
 * PD-table address. Without this the BBH enqueues PDs but nothing wakes the
 * CPU_RX thread, so the RX delivery ring never fills.
 * ★Several field encodings (CRDT_CFG layout, the TSK_TO_RG global task id) are
 * RE-derived but unconfirmed on silicon - tune when RX is tested.
 */
static void runner_dsptchr_cpu_rx_setup(struct runner_priv *p)
{
	void __iomem *d = p->xrdp + XRDP_OFF_DSPTCHR;
	u32 viq   = RUNNER_FIRST_PORT;			/* VIQ == bbh_id */
	u32 grp   = DSPTCHR_CPU_RX_GROUP;
	u32 bb_id = BBH_BBID_RX_BBH0 + 2 * RUNNER_FIRST_PORT;
	u32 tword = CPU_RX_RING_CORE / 2;		/* MASK_MSK_TSK word for the core */
	u32 tval  = (1u << RNR_CPU_RX_THREAD) << ((CPU_RX_RING_CORE & 1) * 16);
	u32 task  = RNR_CPU_RX_THREAD;
	void __iomem *m;
	u32 cur;

	/* feeding VIQ: bb_id of the RX BBH + normal bbh target; dest = dispatcher */
	writel(bb_id | (DSPTCHR_VIQ_TARGET_NORMAL << 8),
	       d + DSPTCHR_QUEUE_CRDT_CFG + 4 * viq);
	writel(0, d + DSPTCHR_Q_DEST + 4 * viq);
	/* place CPU_RX core/thread into the group, and make the group consume VIQ */
	writel(tval, d + DSPTCHR_MASK_MSK_TSK + 4 * (grp * 8 + tword));
	writel(1u << viq, d + DSPTCHR_MASK_MSK_Q + 4 * grp);
	/* task -> runner-group (3-bit field) */
	m = d + DSPTCHR_TSK_TO_RG_MAPPING + 4 * (task / 8);
	cur = readl(m);
	cur &= ~(0x7u << ((task % 8) * 3));
	cur |= (grp & 0x7) << ((task % 8) * 3);
	writel(cur, m);
	/* available-task count for the group */
	cur = readl(d + DSPTCHR_RG_AVLABL_TSK_0_3);
	cur &= ~(0xffu << (grp * 8));
	cur |= (1u << (grp * 8));
	writel(cur, d + DSPTCHR_RG_AVLABL_TSK_0_3);
	/* THE wake target: where the dispatcher delivers PDs for the CPU_RX core */
	writel(DSPTCHR_CPU_RX_PD_ADDR & 0xffff,
	       d + DSPTCHR_PD_DSPTCH_ADD + 4 * CPU_RX_RING_CORE);
	/* enable the feeding VIQ */
	writel(1u << viq, d + DSPTCHR_VQ_EN);
	dev_info(p->dev, "bring-up: DSPTCHR CPU_RX wired (viq=%u grp=%u core%d/thr%d pd=0x%x)\n",
		 viq, grp, CPU_RX_RING_CORE, RNR_CPU_RX_THREAD, DSPTCHR_CPU_RX_PD_ADDR);
}

/*
 * Add an extra ingress port VIQ (e.g. a 10G XPORT port5/6) to the SAME CPU_RX
 * dispatcher group set up by runner_dsptchr_cpu_rx_setup(). The port's BBH_RX
 * already pushes to VIQ == bbh_id (runner_bbh_init); here we register that VIQ's
 * credit-cfg + dest, OR it into the group's consumed-queue mask, and enable it.
 * The CPU_RX core + task->group mapping are shared (already programmed for the
 * first-light port). Frames then reach the same rnr0 conduit; the Runner
 * microcode's flow-miss/unknown-SA default traps them to the CPU. If they do NOT
 * arrive on rnr0 under traffic, the reason->TC->RXQ + SAL/DAL tables (Area 5)
 * are the follow-up. [re-notes/realhw/12-10g-xport-bringup.md §4-5]
 */
static void runner_dsptchr_add_port_viq(struct runner_priv *p, u32 viq)
{
	void __iomem *d = p->xrdp + XRDP_OFF_DSPTCHR;
	u32 grp   = DSPTCHR_CPU_RX_GROUP;
	u32 bb_id = BBH_BBID_RX_BBH0 + 2 * viq;		/* port5 -> 41, port6 -> 43 */

	writel(bb_id | (DSPTCHR_VIQ_TARGET_NORMAL << 8),
	       d + DSPTCHR_QUEUE_CRDT_CFG + 4 * viq);
	writel(0, d + DSPTCHR_Q_DEST + 4 * viq);	/* dest = dispatcher */
	writel(readl(d + DSPTCHR_MASK_MSK_Q + 4 * grp) | (1u << viq),
	       d + DSPTCHR_MASK_MSK_Q + 4 * grp);
	writel(readl(d + DSPTCHR_VQ_EN) | (1u << viq), d + DSPTCHR_VQ_EN);
	dev_info(p->dev, "bring-up: DSPTCHR ingress VIQ%u added (bb_id=%u -> CPU_RX grp%u)\n",
		 viq, bb_id, grp);
}

/*
 * Register the CPU_TX EGRESS (delayed-credit) VIQ so the dispatcher GRANTS
 * egress credits to the CPU_TX thread. Without this the credit table (core-2
 * @0x29d0) stays 0 and the CPU_TX thread stalls after its initial buffers
 * (observed: runner_read_idx frozen at 3). The dispatcher is the credit
 * PRODUCER; it deposits credit at the target address and wakes thread 6.
 * [CFE2 data_path_init.c dispatcher_reorder_viq_init(DISP_REOR_VIQ_CPU_TX_EGRESS)].
 */
static void runner_dsptchr_cpu_tx_setup(struct runner_priv *p)
{
	void __iomem *d = p->xrdp + XRDP_OFF_DSPTCHR;
	u32 viq   = DSPTCHR_CPU_TX_EGRESS_VIQ;
	u32 bb_id = CPU_TX_RING_CORE;		/* BB_ID_RNR<core> == core index */
	u32 cur;

	/* crdt_cfg: target = (credit_addr>>3 | thread<<12) in [31:16], bb_id [7:0].
	 * The target tells the dispatcher WHERE to deposit credit + WHICH thread
	 * to wake; bb_id selects the runner core whose data memory holds it.
	 * Matches stock crdt_cfg[13] = 0x653A0002 exactly. */
	writel((DSPTCHR_CPU_TX_CRDT_TGT << 16) | (bb_id & 0xff),
	       d + DSPTCHR_QUEUE_CRDT_CFG + 4 * viq);
	/* mark it a delayed (egress) queue. q_dest is NOT written - stock leaves it
	 * uninitialized (0xDEADBEEF) for this VIQ; it is meaningless for a delayed
	 * credit queue. */
	cur = readl(d + DSPTCHR_MASK_DLY_Q);
	writel(cur | (1u << viq), d + DSPTCHR_MASK_DLY_Q);
	/* per-VIQ ingress limits: CMN_MAX=0x3FF, GURNTD_MAX=8 (match stock) */
	writel(DSPTCHR_TX_INGRS_LIMITS, d + DSPTCHR_INGRS_Q_LIMITS + 4 * viq);
	/* seed the dispatcher's delayed-egress QM credit pool (stock=8). Without a
	 * QM feeding this, the dispatcher cannot RELEASE delayed-queue credit. */
	writel(DSPTCHR_EGRS_DLY_QM_CRDT_VAL, d + DSPTCHR_EGRS_DLY_QM_CRDT);
	/* enable this VIQ alongside the CPU_RX one already set */
	cur = readl(d + DSPTCHR_VQ_EN);
	writel(cur | (1u << viq), d + DSPTCHR_VQ_EN);
	dev_info(p->dev,
		 "bring-up: DSPTCHR CPU_TX egress VIQ%u wired (crdt_tgt=0x%x bb=%u thr%d dly_crdt=%u)\n",
		 viq, DSPTCHR_CPU_TX_CRDT_TGT, bb_id, RNR_CPU_TX_THREAD,
		 DSPTCHR_EGRS_DLY_QM_CRDT_VAL);
}

/*
 * Route A QM bring-up (opt-in). Makes the QM safe to enable and binds one queue
 * to the TM core that drains it into BBH_TX. The gen-2 SRAM auto-init is THE step
 * that makes ENABLE_CTRL=0x307 safe: without it fpm_prefetch reads uninitialised
 * SRAM and hangs the SoC (the prior 0x307 hang). RUNNER_GRP / queue / TM-core
 * values are ★SILICON (module params from a stock devmem oracle).
 * [spec re-notes/realhw/11-route-a-egress-spec.md secs A+C; XRDP_AG.h offsets.]
 */
static void runner_qm_init(struct runner_priv *p)
{
	void __iomem *qm = p->xrdp + XRDP_OFF_QM;
	u32 grpoff = QM_RUNNER_GRP_REG(route_a_grp, 0);
	u32 en;
	int i;

	/* 1. gen-2 QM SRAM auto-init - MUST precede enable; poll MEM_INIT_DONE. */
	writel(QM_GLOBAL_MEM_AUTO_INIT_EN, qm + QM_GLOBAL_MEM_AUTO_INIT);
	for (i = 0; i < 1000; i++) {
		if (readl(qm + QM_GLOBAL_MEM_AUTO_INIT_STS) &
		    QM_GLOBAL_MEM_AUTO_INIT_DONE)
			break;
		udelay(10);
	}
	if (i == 1000)
		dev_warn(p->dev, "route_a: QM SRAM auto-init did not signal DONE\n");

	/* 2. FPM base = the DDR packet pool FPM manages (256B units) + DDR SOP. */
	writel((u32)(p->pool_pbase >> 8), qm + QM_GLOBAL_FPM_BASE_ADDR);
	writel(QM_GLOBAL_DDR_SOP_OFFSET_VAL, qm + QM_GLOBAL_DDR_SOP_OFFSET);

	/* 3. bind a RUNNER_GRP: queue range [Q,Q] -> TM core bb_id/task, enabled.
	 *    This IS the TM-core wakeup path (no extra DSPTCHR VIQ needed - the QM
	 *    issues the credit/wakeup to the TM task via its UPDATE_FIFO). */
	writel(((route_a_queue & QM_QUEUE_CFG_QUEUE_MASK) << QM_QUEUE_CFG_START_SHIFT) |
	       ((route_a_queue & QM_QUEUE_CFG_QUEUE_MASK) << QM_QUEUE_CFG_END_SHIFT),
	       qm + grpoff + QM_RUNNER_GRP_QUEUE_CONFIG);
	writel((route_a_tm_bb_id & QM_RNR_CFG_RNR_BB_ID_MASK) |
	       ((route_a_tm_task & QM_RNR_CFG_RNR_TASK_MASK) << QM_RNR_CFG_RNR_TASK_SHIFT) |
	       QM_RNR_CFG_RNR_ENABLE,
	       qm + grpoff + QM_RUNNER_GRP_RNR_CONFIG);

	/* 4. enable the QM now that SRAM/FPM/grp are configured (full stock 0x307). */
	en = qm_enable ? qm_enable : QM_ENABLE_CTRL_STOCK;
	writel(en, qm + QM_GLOBAL_QM_ENABLE_CTRL);
	dev_info(p->dev,
		 "route_a: QM up (enable=0x%x grp=%u queue=%u tm_bb=%u tm_task=%u fpm_base=0x%x)\n",
		 en, route_a_grp, route_a_queue, route_a_tm_bb_id, route_a_tm_task,
		 (u32)(p->pool_pbase >> 8));
}

/*
 * Route A BBH_TX binding (opt-in): make the LAN BBH_TX instance take its target
 * queue from the QM aggregator (QMQ=1) and name the feeding TM core. These are
 * the fields the plain slow-path BBH_TX setup omits. [spec sec B; XRDP_AG.h.]
 */
static void runner_bbh_tx_route_a(struct runner_priv *p)
{
	void __iomem *tx = p->xrdp + XRDP_OFF_BBH_TX0 +
			   (u32)route_a_bbh_inst * XRDP_BBH_TX_STRIDE;

	/* feeding TM-core BB-id (PDRNR0SRC) so BBH_TX accepts its PDs */
	writel(route_a_tm_bb_id & 0x3f, tx + BBH_TX_BBCFG_2);
	/* RNRCFG_2[0]: TASK = TM egress thread. PTRADDR (TM egress-counter table) is
	 * ★SILICON and left 0 - the QM RUNNER_GRP drives the wakeup regardless. */
	writel((route_a_tm_task & 0xf) << BBH_TX_RNRCFG_2_TASK_SHIFT,
	       tx + BBH_TX_RNRCFG_2_0);
	/* take queue 0 of this BBH from the QM aggregator (both register views) */
	writel(BBH_TX_QMQ_Q0, tx + BBH_TX_QMQ_LAN);
	writel(BBH_TX_QMQ_Q0, tx + BBH_TX_QMQ_UNIFIED);
	dev_info(p->dev,
		 "route_a: BBH_TX inst%u QM-fed (QMQ q0=1, tm_bb=%u task=%u)\n",
		 route_a_bbh_inst, route_a_tm_bb_id, route_a_tm_task);
}

static int runner_dsptchr_init(struct runner_priv *p)
{
	void __iomem *d = p->xrdp + XRDP_OFF_DSPTCHR;
	int i;

	/* configure the CPU_RX VIQ/group + CPU_TX egress credit VIQ BEFORE
	 * enabling the reorder engine */
	runner_dsptchr_cpu_rx_setup(p);
	runner_dsptchr_cpu_tx_setup(p);

	/*
	 * Enable the QM global credit path. The CPU_TX egress VIQ is a DELAYED
	 * queue whose credit RELEASE + thread wakeup is gated by DSPTCHR
	 * EGRS_DLY_QM_CRDT, which the QM only feeds when REORDER_CREDIT (bit1)
	 * is set. Without this the dispatcher deposits the one-time guaranteed
	 * grant but never re-arms, so CPU_TX stalls with credit pinned. Stock
	 * writes 0x307 here (verified). No QM queue/context config is needed for
	 * the runner-fed BBH_TX slow path. [CFE2 rdp_block_enable qm_enable_ctrl]
	 *
	 * When route_a is set, runner_qm_init() owns the QM (full SRAM auto-init +
	 * RUNNER_GRP + enable, in the correct order) - do NOT write enable here, or
	 * 0x307 lands before the SRAM auto-init and hangs the SoC.
	 */
	if (!route_a && qm_enable)
		writel(qm_enable, p->xrdp + XRDP_OFF_QM + QM_GLOBAL_QM_ENABLE_CTRL);
	if (!route_a)
		dev_info(p->dev, "bring-up: QM enable_ctrl=0x%x\n", qm_enable);

	/* let the HW auto-init the free linked list, then enable the reorder
	 * engine; poll RDY (bit8). (Hand-seeding 1024 nodes is the alternative.) */
	writel(DSPTCHR_REORDER_CFG_EN | DSPTCHR_REORDER_CFG_AUTO_INIT,
	       d + DSPTCHR_REORDER_CFG);
	for (i = 0; i < 1000; i++) {
		if (readl(d + DSPTCHR_REORDER_CFG) & DSPTCHR_REORDER_CFG_RDY) {
			dev_info(p->dev, "bring-up: DSPTCHR reorder ready\n");
			return 0;
		}
		udelay(10);
	}
	dev_warn(p->dev, "bring-up: DSPTCHR reorder init did not signal RDY\n");
	return 0;	/* non-fatal for first-light */
}

/* configure + enable one BBH_RX MAC port and the LAN BBH_TX instance */
static void runner_bbh_init(struct runner_priv *p, int rx_port)
{
	void __iomem *rx = p->xrdp + XRDP_OFF_BBH_RX0 +
			   (u32)rx_port * XRDP_BBH_RX_STRIDE;
	void __iomem *tx = p->xrdp + XRDP_OFF_BBH_TX0;	/* BBH_TX_ID_LAN = 0 */

	/* BBH_RX full per-port config [SDK bbh_rx_cfg / data_path_init.c]:
	 *  BBCFG = SDMABBID[5:0] | DISPBBID[15:8] | SBPMBBID[23:16]
	 *  DISPVIQ = NORMALVIQ | EXCLVIQ, both = bbh_id (★VIQ == bbh_id rule)
	 *  SDMAADDR=0, SDMACFG=4 chunks, SOPOFFSET=0, SBPMCFG MAXREQ, ENABLE last.
	 * (Previously only DISPBBID/SBPMBBID + ENABLE were written -> the BBH could
	 * not reassemble/enqueue a PD to the dispatcher.) */
	{
		u32 sdma_bb = (rx_port <= 5) ? BBH_BBID_SDMA0 : BBH_BBID_SDMA1;

		writel(sdma_bb | ((u32)BBH_BBID_DISPATCHER << 8) |
		       ((u32)BBH_BBID_SBPM << 16), rx + BBH_RX_BBCFG);
		writel(((u32)rx_port & 0xff) | (((u32)rx_port & 0xff) << 8),
		       rx + BBH_RX_DISPVIQ);
		writel(0, rx + BBH_RX_SDMAADDR);
		writel(BBH_RX_SDMACFG_VAL, rx + BBH_RX_SDMACFG);
		writel(0, rx + BBH_RX_SOPOFFSET);
		writel(BBH_RX_SBPMCFG_VAL, rx + BBH_RX_SBPMCFG);
		writel(BBH_RX_ENABLE_PKTEN | BBH_RX_ENABLE_SBPMEN,
		       rx + BBH_RX_ENABLE);	/* LAST */
	}

	/* BBH_TX: MAC type + FPM/SBPM source block IDs */
	writel(BBH_TX_MACTYPE_VAL, tx + BBH_TX_MACTYPE);
	writel(((u32)BBH_BBID_FPM << 24) | ((u32)BBH_BBID_SBPM << 16),
	       tx + BBH_TX_BBCFG_1);
	dev_info(p->dev, "bring-up: BBH_RX port %d + LAN BBH_TX configured\n",
		 rx_port);
}

/* ============================ 10G XPHY (eth0) bring-up =================== *
 * Release the internal 10G XPHY (eth0 = xphy0 @MDIO addr 9) from reset via the
 * eth-phy-top block. The stock eth_phy_top.c xphy_init() does this on a normal
 * boot but our guard skips it, so eth0's XPHY sits in reset (link=0). Mirrors
 * that sequence: clear iso/tmode in XPHY_TEST_CNTRL, then set phyad + clear
 * phy_reset/super_isolate in XPHY_CNTRL, 100 ms settle each. The XPHY is a
 * self-contained on-chip 10G PHY -> it should auto-link once out of reset (no
 * merlin16 blob). [SDK eth_phy_top.c CONFIG_BCM96813 xphy_init]
 */
static void runner_xphy_init(struct runner_priv *p, u32 xphy_id, u32 phyad)
{
	void __iomem *test = p->ethphytop +
		(xphy_id ? ETHPHY_OFF_XPHY_TEST_CNTRL1 : ETHPHY_OFF_XPHY_TEST_CNTRL0);
	void __iomem *cntrl = p->ethphytop +
		(xphy_id ? ETHPHY_OFF_XPHY_CNTRL1 : ETHPHY_OFF_XPHY_CNTRL0);
	u32 v;

	writel(1, p->ethphytop + ETHPHY_OFF_XPHY_MUX_SEL);	/* LED mux (stock=1) */
	v = readl(test) & ~(XPHY_TEST_ISO_ENABLE | XPHY_TEST_TMODE);
	writel(v, test);
	mdelay(100);
	v = readl(cntrl);
	v &= ~(XPHY_CNTRL_SUPER_ISOLATE | XPHY_CNTRL_PHY_RESET |
	       ((u32)XPHY_CNTRL_PHYAD_MASK << XPHY_CNTRL_PHYAD_SHIFT));
	v |= (phyad & XPHY_CNTRL_PHYAD_MASK) << XPHY_CNTRL_PHYAD_SHIFT;
	writel(v, cntrl);
	mdelay(100);
	dev_info(p->dev, "bring-up: XPHY%u out of reset (addr=%u) test=0x%08x cntrl=0x%08x\n",
		 xphy_id, phyad, readl(test), readl(cntrl));
}

/* ============================ eth0 XPHY firmware load ==================== *
 * Reimplements phy_drv_ext3.c load_blackfin() for the internal 10G XPHY (eth0,
 * C45 MDIO addr 9): halt the on-chip uC, stream /rom/etc/fw/xphy_firmware.bin
 * (179548 B ARM image) into on-chip RAM over C45 MDIO, reset the uC to run it,
 * and poll running. Without firmware the XPHY cannot link (link=0). C45 access =
 * the stock mdio_write/read_c45_register via kallsyms (4.19; same mechanism we
 * use to read the 1G PHY). Register set = default_load_reg (dev 0x01: ctrl
 * 0xa817, addr_low 0xa819/high 0xa81a, data_low 0xa81b/high 0xa81c).
 * [SDK phy_drv_ext3.c:3170 load_blackfin / :2597 _load_firmware_file]
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
static int runner_xphy_fw_load(struct runner_priv *p, u32 ad)
{
	int (*c45w)(u32, u16, u16, u16) =
		(void *)kallsyms_lookup_name("mdio_write_c45_register");
	int (*c45r)(u32, u16, u16, u16 *) =
		(void *)kallsyms_lookup_name("mdio_read_c45_register");
	const char *path = "/rom/etc/fw/xphy_firmware.bin";
	struct file *fp;
	loff_t pos = 0;
	u8 *buf;
	int len, i, total = 0;
	u16 rv = 0;

	if (!c45w || !c45r) {
		dev_warn(p->dev, "XPHY fw: mdio_*_c45_register absent - skip\n");
		return -ENOSYS;
	}
	c45r(ad, 1, 2, &rv);
	dev_info(p->dev, "XPHY fw: addr %u dev1.reg2(idhi)=0x%04x\n", ad, rv);

	/* halt the XPHY uC */
	c45w(ad, 0x1e, 0x4110, 0x0001); c45w(ad, 0x1e, 0x418c, 0x0000);
	c45w(ad, 0x1e, 0x4188, 0x48f0); c45w(ad, 0x01, 0xa81a, 0xf000);
	c45w(ad, 0x01, 0xa819, 0x3000); c45w(ad, 0x01, 0xa81c, 0x0000);
	c45w(ad, 0x01, 0xa81b, 0x0121); c45w(ad, 0x01, 0xa817, 0x0009);
	c45w(ad, 0x1e, 0x80a6, 0x0000); c45w(ad, 0x01, 0xa010, 0x0000);
	c45w(ad, 0x01, 0x0000, 0x8000); mdelay(1);
	c45w(ad, 0x1e, 0x4110, 0x0001); mdelay(1);

	fp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		dev_warn(p->dev, "XPHY fw: open %s failed (%ld)\n", path, PTR_ERR(fp));
		return PTR_ERR(fp);
	}
	buf = kmalloc(1024, GFP_KERNEL);
	if (!buf) { filp_close(fp, NULL); return -ENOMEM; }

	/* stream the image into on-chip RAM (addr=0, ctrl=0x38, per-u32 hi/lo) */
	c45w(ad, 0x01, 0xa819, 0x0000);		/* addr_low */
	c45w(ad, 0x01, 0xa81a, 0x0000);		/* addr_high */
	c45w(ad, 0x01, 0xa817, 0x0038);		/* ctrl: RAM write + autoinc */
	while ((len = kernel_read(fp, buf, 1024, &pos)) > 0) {
		for (i = 0; i + 3 < len; i += 4) {
			u32 data = get_unaligned_le32(buf + i);

			c45w(ad, 0x01, 0xa81c, data >> 16);	/* data_high */
			c45w(ad, 0x01, 0xa81b, data & 0xffff);	/* data_low */
			total += 4;
		}
	}
	c45w(ad, 0x01, 0xa817, 0x0000);		/* ctrl off */
	filp_close(fp, NULL);
	kfree(buf);
	dev_info(p->dev, "XPHY fw: streamed %d bytes\n", total);

	/* reset the uC to run the code, then poll running (dev1.reg0 == 0x2040) */
	c45w(ad, 0x01, 0xa81a, 0xf000); c45w(ad, 0x01, 0xa819, 0x3000);
	c45w(ad, 0x01, 0xa81c, 0x0000); c45w(ad, 0x01, 0xa81b, 0x0020);
	c45w(ad, 0x01, 0xa817, 0x0009); mdelay(2);
	for (i = 0; i < 1000; i++) {
		mdelay(2);
		rv = 0;
		c45r(ad, 0x01, 0x0000, &rv);
		if (rv == 0x2040)
			break;
	}
	dev_info(p->dev, "XPHY fw: uC %s (dev1.reg0=0x%04x, %d B)\n",
		 rv == 0x2040 ? "RUNNING" : "NOT running", rv, total);
	return rv == 0x2040 ? 0 : -EIO;
}
#else
static int runner_xphy_fw_load(struct runner_priv *p, u32 ad) { return -ENOSYS; }
#endif

/* ============================ 10G XPORT MAC bring-up ===================== *
 * Bring up one 10G XPORT/XLMAC MAC: eth0/port5 = xport_port_id 0, eth1/port6 =
 * id 2 (both on physical XLMAC0; xport_num = id & 3). All register blocks live
 * inside p->ethphytop (the 0x837f0000 region) - no extra ioremap. This configures
 * the MAC + msbus path only; the PHY link (eth0 internal XPHY / eth1 merlin16) is
 * a separate concern. ⚠ XLMAC_CORE regs are 64-bit but committed by one low-word
 * writel() (fields >bit31 rely on HW default). Sequence + offsets from the stock
 * xport_drv init_driver / xlmac_init / msbus_init.
 * [re-notes/realhw/12-10g-xport-bringup.md §2]  Values marked "TBD" are
 * RE-summary-derived and to be confirmed against a live-silicon dump.
 */
static void runner_xport_init(struct runner_priv *p, int xport_port_id)
{
	int num = xport_port_id & 3;			/* intra-XLMAC0 port 0..3 */
	void __iomem *core = p->ethphytop + XPORT_OFF_XLMAC0_CORE +
			     (u32)num * XPORT_XLMAC_PORT_STRIDE;
	void __iomem *top  = p->ethphytop + XPORT_OFF_TOP;
	void __iomem *mab  = p->ethphytop + XPORT_OFF_MAB;
	void __iomem *prst = p->ethphytop + XPORT_OFF_PORTRESET;
	u32 pctrl = (num == 0) ? XPORT_PORTRESET_P0_CTRL : XPORT_PORTRESET_P2_CTRL;
	u32 sigen = (num == 0) ? XPORT_PORTRESET_P0_SIG_EN : XPORT_PORTRESET_P2_SIG_EN;
	u32 v;
	int i;

	/*
	 * ★POWER/CLOCK FIRST. The XLMAC block is NOT powered on the datapath-skip
	 * boot: the stock mac_drv_xport port_xport_drv_init() calls
	 * pmc_xport_power_on(xlmac_instance) (-> bcm_rpc_ba_xport_set_state) per
	 * XLMAC, and our guard skips that -> the first XLMAC register access HANGS
	 * the SoC. Call the stock kernel's pmc_xport_power_on() via kallsyms before
	 * touching anything. xlmac instance = xport_port_id >> 2 (eth0/eth1 -> 0).
	 * (4.19 vendor kernel exports kallsyms_lookup_name; a mainline port needs a
	 * clk/reset/pmc driver instead.) [SDK misc/pmc/impl2/pmc_xport.c,
	 * phy/mac_drv_xport.c:490]
	 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
	{
		int (*xport_pwr)(int) =
			(void *)kallsyms_lookup_name("pmc_xport_power_on");

		if (!xport_pwr) {
			dev_warn(p->dev,
				 "XPORT: pmc_xport_power_on not found - block unpowered, ABORT to avoid SoC hang\n");
			return;
		}
		i = xport_pwr(xport_port_id >> 2);
		dev_info(p->dev, "XPORT: pmc_xport_power_on(xlmac%d) rc=%d\n",
			 xport_port_id >> 2, i);
		mdelay(1);
	}
#else
	dev_warn(p->dev, "XPORT: no pmc_xport_power_on path on this kernel - ABORT\n");
	return;
#endif

	/* --- Phase A: init_driver -> leave the port in reset --- */
	/* Px_SIG_EN: assert MAC/MAB reset+init strobes (rx/tx_disab b9/b8, xlmac
	 * soft_reset b6, mab rx/tx_port_init b5/b4, tx_credit_disab b3, tx_fifo_init
	 * b2, port_is_under_reset b1; ep_discard b0 left 0). */
	writel((1u<<9)|(1u<<8)|(1u<<6)|(1u<<5)|(1u<<4)|(1u<<3)|(1u<<2)|(1u<<1),
	       prst + sigen);
	writel(readl(prst + XPORT_PORTRESET_CONFIG) | (1u << num),
	       prst + XPORT_PORTRESET_CONFIG);		/* ENABLE_SM_RUN[num] */
	udelay(5000);
	writel(readl(core + XLMAC_CORE_RX_LSS_CTRL) | XLMAC_CORE_RX_LSS_LOCAL_FAULT_DIS,
	       core + XLMAC_CORE_RX_LSS_CTRL);
	/* xport_reset: MAB RX/TX port reset (per-port bit) + XLMAC soft reset */
	writel(readl(mab + XPORT_MAB_CONTROL) | (1u << (0 + num)) | (1u << (4 + num)),
	       mab + XPORT_MAB_CONTROL);
	writel(readl(core + XLMAC_CORE_CTRL) | XLMAC_CORE_CTRL_SOFT_RESET,
	       core + XLMAC_CORE_CTRL);

	/* --- Phase B: link-up config --- */
	writel(XPORT_PORTRESET_CTRL_SW_RESET, prst + pctrl);	/* assert port SW reset */
	/* TOP.CONTROL: this port's MODE = XGMII/10G (2 bits/port) */
	v = readl(top + XPORT_TOP_CONTROL);
	v = (v & ~(0x3u << (num * 2))) |
	    ((u32)XPORT_TOP_CONTROL_MODE_XGMII << (num * 2));
	writel(v, top + XPORT_TOP_CONTROL);
	writel(XLMAC_CORE_TX_CTRL_CRC_PERPKT | XLMAC_CORE_TX_CTRL_PAD_EN,
	       core + XLMAC_CORE_TX_CTRL);
	writel(XLMAC_CORE_RX_CTRL_RX_PASS, core + XLMAC_CORE_RX_CTRL); /* runt_thr TBD */
	writel((u32)XLMAC_CORE_MODE_SPEED_10G << XLMAC_CORE_MODE_SPEED_SHIFT,
	       core + XLMAC_CORE_MODE);			/* SPEED_MODE = 10G (0x40) */
	writel(XLMAC_CORE_RX_MAX_SIZE_VAL, core + XLMAC_CORE_RX_MAX_SIZE);
	writel(readl(core + XLMAC_CORE_CTRL) & ~XLMAC_CORE_CTRL_SOFT_RESET,
	       core + XLMAC_CORE_CTRL);			/* release XLMAC soft reset */
	/* msbus: clear this port's credit-disable + fifo/port resets */
	writel(readl(mab + XPORT_MAB_CONTROL) &
	       ~((1u<<(12+num)) | (1u<<(8+num)) | (1u<<(0+num)) | (1u<<(4+num))),
	       mab + XPORT_MAB_CONTROL);
	writel(0, prst + pctrl);			/* release port SW reset */

	/* --- Phase C: enable RX/TX --- */
	writel(readl(core + XLMAC_CORE_CTRL) |
	       XLMAC_CORE_CTRL_TX_EN | XLMAC_CORE_CTRL_RX_EN,
	       core + XLMAC_CORE_CTRL);

	for (i = 0; i < 3000; i++) {	/* up to 3s: XPHY PLL/PMD/AN link takes time */
		if (readl(top + XPORT_TOP_STATUS) & (1u << num))
			break;
		udelay(1000);
	}
	dev_info(p->dev,
		 "bring-up: XPORT id%d (xlmac0/p%d) 10G MAC enabled, link=%d "
		 "(top_sts=0x%08x ctrl=0x%08x)\n",
		 xport_port_id, num, !!(readl(top + XPORT_TOP_STATUS) & (1u << num)),
		 readl(top + XPORT_TOP_STATUS), readl(core + XLMAC_CORE_CTRL));
}

/* ============================ 1G MAC/PHY bring-up ======================== *
 * Bring up one 1G UNIMAC + internal EGPHY port (the eth2=port_gphy1 class) so a
 * physical link exists for the Runner to RX/TX through. No proprietary blob (the
 * 10G XPORT/serdes path needs one). [SDK phy_drv_egphy.c _phy_cfg +
 * unimac_drv_impl1.c]. The stock built-in unimac/egphy drivers won't START the
 * port without bcm_enet, so we replicate the register sequence directly.
 * NOTE: the per-PHY MDIO power-up/link-read (addr 2 on mdiosf2) is a follow-up;
 * the block EXT_PWR_DOWN=0 here powers the PHY and it auto-negotiates.
 */
static void runner_mac_phy_init(struct runner_priv *p, int unimac_inst)
{
	void __iomem *mac = p->xrdp + XRDP_OFF_UNIMAC0 +
			    (u32)unimac_inst * XRDP_UNIMAC_STRIDE;
	void __iomem *ctl = p->ethphytop + ETHPHY_OFF_QEGPHY_CTRL;
	void __iomem *sts = p->ethphytop + ETHPHY_OFF_QEGPHY_STATUS;
	u32 v;
	int i;

	/*
	 * --- internal quad-EGPHY block power-up (QPHY_CNTRL @ 0x837ff014) ---
	 * Replicates the stock 6813 sequence (phy_drv_dsl_phy.c qphy_ctrl_adjust /
	 * u-boot bcm_ethsw_phy.c qgphy_powerup): power everything down + assert
	 * reset, settle, apply power (still in reset), settle, then release reset
	 * and let the PLL lock. The PMC "ethtop" power domain is already ON
	 * (u-boot switch power-up + the built-in eth_phy_top probe's
	 * pmc_ethtop_power_up(ETHTOP_COMMON)); the stock GPHY power-up itself is
	 * tied to a MAC phy_connect via the stock datapath, which our trial SKIPS,
	 * so the block is left in POR and we drive it here. We enable all four
	 * internal GPHYs (ext_pwr_down = 0). Base phy_phyad = 1 (addrs 1..4).
	 */
	v = readl(ctl);
	v &= ~(QEGPHY_CTRL_IDDQ_BIAS |
	       ((u32)QEGPHY_CTRL_EXT_PWR_DOWN_MASK << QEGPHY_CTRL_EXT_PWR_DOWN_SHIFT) |
	       ((u32)QEGPHY_CTRL_IDDQ_GLOBAL_PWR_MASK << QEGPHY_CTRL_IDDQ_GLOBAL_PWR_SHIFT) |
	       QEGPHY_CTRL_CK25_DIS | QEGPHY_CTRL_PHY_RESET |
	       ((u32)QEGPHY_CTRL_PHYAD_MASK << QEGPHY_CTRL_PHYAD_SHIFT) |
	       ((u32)0x3 << QEGPHY_CTRL_REF_CLK_FREQ_SHIFT) |
	       QEGPHY_CTRL_PLL_CLK125_250_SEL);
	v |= (1u << QEGPHY_CTRL_PHYAD_SHIFT);			/* base MDIO addr = 1 */
	v |= ((u32)QEGPHY_CTRL_REF_CLK_50MHZ << QEGPHY_CTRL_REF_CLK_FREQ_SHIFT);
	/* phase 1: all ports powered down + in reset */
	writel(v | QEGPHY_CTRL_IDDQ_BIAS |
	       ((u32)0xf << QEGPHY_CTRL_EXT_PWR_DOWN_SHIFT) |
	       ((u32)0xf << QEGPHY_CTRL_IDDQ_GLOBAL_PWR_SHIFT) |
	       QEGPHY_CTRL_PHY_RESET, ctl);
	udelay(40);
	/* phase 2: power applied (iddq + ext_pwr_down cleared), still in reset */
	writel(v | QEGPHY_CTRL_PHY_RESET, ctl);
	udelay(100);
	/* phase 3: release reset (all four ports enabled) -> PLL starts */
	writel(v, ctl);
	udelay(1000);
	for (i = 0; i < 100; i++) {
		if (readl(sts) & QEGPHY_STATUS_PLL_LOCK)
			break;
		udelay(100);
	}
	dev_info(p->dev, "bring-up: EGPHY block PLL %s (qphy_cntrl=0x%08x status=0x%08x)\n",
		 (readl(sts) & QEGPHY_STATUS_PLL_LOCK) ? "locked" : "NOT locked",
		 readl(ctl), readl(sts));

	/* --- UNIMAC MAC config under sw_reset, then enable [unimac_drv_impl1.c] --- */
	writel(UNIMAC_CMD_SW_RESET, mac + UNIMAC_CMD);		/* assert reset */
	writel(UNIMAC_FRM_LEN_VAL, mac + UNIMAC_FRM_LEN);
	v = UNIMAC_CMD_SW_RESET |
	    ((u32)UNIMAC_CMD_SPEED_1G << UNIMAC_CMD_SPEED_SHIFT) |
	    UNIMAC_CMD_PROMIS | UNIMAC_CMD_CRC_FWD | UNIMAC_CMD_PAUSE_FWD |
	    UNIMAC_CMD_CNTL_FRM_ENA | UNIMAC_CMD_NO_LGTH_CHK;
	if (mac_loopback)
		v |= UNIMAC_CMD_LOOP_ENA;	/* TX->RX internal, no cable */
	writel(v, mac + UNIMAC_CMD);				/* configured, in reset */
	v &= ~UNIMAC_CMD_SW_RESET;
	writel(v, mac + UNIMAC_CMD);				/* release reset */
	v |= UNIMAC_CMD_TX_ENA | UNIMAC_CMD_RX_ENA;
	writel(v, mac + UNIMAC_CMD);				/* enable TX/RX */
	dev_info(p->dev, "bring-up: UNIMAC inst%d up (1G, tx/rx enabled%s)\n",
		 unimac_inst, mac_loopback ? ", LOCAL LOOPBACK" : "");

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
	/*
	 * Best-effort PHY/link readback via the stock built-in SF2 MDIO
	 * accessor (mdiosf2 @ 0x837ffd00; port_gphy1 = MDIO addr 2). Diagnostic
	 * only - a proper phylib hookup is a follow-up. (4.19 vendor kernel:
	 * kallsyms_lookup_name is exported; mainline uses phylib instead.)
	 */
	{
		int (*mdio_rd)(u32, u32, u16 *) =
			(void *)kallsyms_lookup_name("mdio_read_c22_register");

		if (mdio_rd) {
			u16 bmcr = 0, bmsr = 0, id1 = 0, id2 = 0;
			u32 a = QEGPHY_MDIO_ADDR;

			mdio_rd(a, MII_BMCR, &bmcr);
			mdio_rd(a, MII_BMSR, &bmsr);
			/* BMSR latches link-low; read twice for the live state */
			mdio_rd(a, MII_BMSR, &bmsr);
			mdio_rd(a, MII_PHYSID1, &id1);
			mdio_rd(a, MII_PHYSID2, &id2);
			dev_info(p->dev,
				 "bring-up: PHY@%u id=%04x%04x bmcr=0x%04x bmsr=0x%04x link=%d aneg=%d\n",
				 a, id1, id2, bmcr, bmsr,
				 !!(bmsr & BMSR_LSTATUS),
				 !!(bmsr & BMSR_ANEGCOMPLETE));
		} else {
			dev_info(p->dev, "bring-up: SF2 MDIO accessor not found (link unknown)\n");
		}
	}
#endif
}

/* ============================ offload self-test (debugfs) =============== *
 * Writing "<vlan_op> <vid>" to .../offload_selftest programs one NAT-C L2 flow
 * keyed on the fixed test DA/SA below (so frames the dgram peer injects HIT).
 * This drives the REAL builders + NAT-C add path - the QEMU offload proof.
 *   vlan_op: 0=plain L2, 1=push, 2=pop, 3=mangle ; vid: VLAN id for 1/2/3.
 */
static const u8 selftest_da[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const u8 selftest_sa[ETH_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

static ssize_t runner_selftest_write(struct file *file, const char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	struct runner_priv *p = file->private_data;
	char buf[32];
	int vlan_op = 0;
	unsigned int vid = 0;
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	sscanf(buf, "%d %u", &vlan_op, &vid);

	ret = xrdp_offload_selftest(&p->offload, selftest_da, selftest_sa,
				    vlan_op, (u16)vid);
	if (ret < 0) {
		dev_err(p->dev, "offload self-test failed: %d\n", ret);
		return ret;
	}
	dev_info(p->dev, "offload self-test OK (cmdlist %d data bytes programmed)\n",
		 ret);
	return count;
}

static const struct file_operations runner_selftest_fops = {
	.open	= simple_open,
	.write	= runner_selftest_write,
};

/*
 * Routed/NAT offload self-test (Phase 2). Writing any non-empty string to
 * .../offload_nat_selftest programs one routed IPv4 SNAT+NAPT flow whose
 * ORIGINAL 5-tuple matches the frames the NAT peer injects, so subsequent
 * frames HIT NAT-C and are rewritten + forwarded in HW. The flow models a
 * LAN->WAN masquerade: original src 192.0.2.10:4096 -> dst 198.51.100.20:80
 * (TCP), rewritten source 203.0.113.5:5000 (post-SNAT WAN side).
 * (Addresses are TEST-NET/documentation ranges - public-safe.)
 */
#define NAT_ST_ORIG_SIP		0xc000020aU	/* 192.0.2.10  */
#define NAT_ST_ORIG_DIP		0xc6336414U	/* 198.51.100.20 */
#define NAT_ST_ORIG_SPORT	4096
#define NAT_ST_ORIG_DPORT	80
#define NAT_ST_NAT_SIP		0xcb007105U	/* 203.0.113.5 */
#define NAT_ST_NAT_SPORT	5000

static ssize_t runner_nat_selftest_write(struct file *file,
					 const char __user *ubuf,
					 size_t count, loff_t *ppos)
{
	struct runner_priv *p = file->private_data;
	int ret;

	ret = xrdp_offload_nat_selftest(&p->offload,
					htonl(NAT_ST_ORIG_SIP),
					htonl(NAT_ST_ORIG_DIP),
					htons(NAT_ST_ORIG_SPORT),
					htons(NAT_ST_ORIG_DPORT),
					IPPROTO_TCP,
					htonl(NAT_ST_NAT_SIP),
					htons(NAT_ST_NAT_SPORT));
	if (ret < 0) {
		dev_err(p->dev, "NAT offload self-test failed: %d\n", ret);
		return ret;
	}
	dev_info(p->dev, "NAT offload self-test OK (cmdlist %d data bytes)\n",
		 ret);
	return count;
}

static const struct file_operations runner_nat_selftest_fops = {
	.open	= simple_open,
	.write	= runner_nat_selftest_write,
};

/*
 * ringstat: dump the host/runner ring indices so we can SEE whether the Runner
 * microcode is actually executing our CPU datapath (consuming TX descriptors,
 * pulling feed buffers, delivering RX). All reads go through the driver's normal
 * ioread paths (safe). The runner-owned indices are BE u16 in core-2/3 SRAM.
 */
static ssize_t runner_ringstat_read(struct file *file, char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct runner_priv *p = file->private_data;
	u16 tx_runner_rd, rx_runner_wr, feed_runner_rd;
	u32 ecred0, ecred1, ecred2;
	u16 sfifo_wr, sfifo_rd;
	char buf[640];
	int n;

	/* TX indices entry {read_idx@+0, write_idx@+2} on core 2: read_idx is
	 * advanced by the runner as it consumes our TX descriptors. */
	tx_runner_rd = be16_to_cpu(ioread16(p->rnr_mem[CPU_TX_RING_CORE] +
					    CPU_TX_RING_INDICES_OFF + 0));
	/* RX delivery ring write_idx@+6 (runner advances it on delivery). */
	rx_runner_wr = be16_to_cpu(ioread16(p->rnr_mem[CPU_RX_RING_CORE] +
				   PSRAM_CPU_RING_DESC_TABLE + RING_CFG_WRITE_IDX_OFF));
	/* FEED ring read_idx@+12 (runner advances it as it pulls buffers). */
	feed_runner_rd = be16_to_cpu(ioread16(p->rnr_mem[CPU_RX_RING_CORE] +
				     RDD_FEED_RING_DESC_TABLE + RING_CFG_READ_IDX_OFF));

	/* CPU_TX egress-dispatcher credits + sync-fifo ptrs (core 2): if the
	 * thread stalls after a few frames with credits==0, it is credit-starved
	 * (the QM/dispatcher replenish path is not wired in slow-path bring-up). */
	ecred0 = ioread32be(p->rnr_mem[CPU_TX_RING_CORE] + CPU_TX_EGRESS_CREDIT_OFF + 0);
	ecred1 = ioread32be(p->rnr_mem[CPU_TX_RING_CORE] + CPU_TX_EGRESS_CREDIT_OFF + 4);
	ecred2 = ioread32be(p->rnr_mem[CPU_TX_RING_CORE] + CPU_TX_EGRESS_CREDIT_OFF + 8);
	sfifo_wr = be16_to_cpu(ioread16(p->rnr_mem[CPU_TX_RING_CORE] + CPU_TX_SYNC_FIFO_OFF + 0));
	sfifo_rd = be16_to_cpu(ioread16(p->rnr_mem[CPU_TX_RING_CORE] + CPU_TX_SYNC_FIFO_OFF + 2));

	n = scnprintf(buf, sizeof(buf),
		"TX  : host_write_idx=%u  runner_read_idx=%u  %s\n"
		"RX  : runner_write_idx=%u host_read_idx=%u  %s\n"
		"FEED: host_write_idx=%u  runner_read_idx=%u  %s\n"
		"FPM : tokens_avail=%u\n"
		"TXCR: egress_credit=[%u %u %u] sync_fifo{wr=%u rd=%u}\n"
		"stats: tx=%lu rx=%lu txerr=%lu rxerr=%lu\n",
		p->tx_write_idx, tx_runner_rd,
		tx_runner_rd ? "(runner CONSUMING tx)" : "(runner not consuming tx)",
		rx_runner_wr, p->rx_head,
		rx_runner_wr ? "(runner DELIVERED rx)" : "(no rx delivered)",
		p->feed_widx, feed_runner_rd,
		feed_runner_rd ? "(runner PULLING feed bufs)" : "(feed untouched)",
		readl(p->fpm + FPM_POOL1_STAT2) & FPM_STAT2_TOKENS_AVAIL_MASK,
		ecred0, ecred1, ecred2, sfifo_wr, sfifo_rd,
		p->ndev->stats.tx_packets, p->ndev->stats.rx_packets,
		p->ndev->stats.tx_errors, p->ndev->stats.rx_errors);
	return simple_read_from_buffer(ubuf, count, ppos, buf, n);
}

static const struct file_operations runner_ringstat_fops = {
	.open	= simple_open,
	.read	= runner_ringstat_read,
};

static void runner_debugfs_init(struct runner_priv *p)
{
	p->dbg = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("offload_selftest", 0200, p->dbg, p,
			    &runner_selftest_fops);
	debugfs_create_file("offload_nat_selftest", 0200, p->dbg, p,
			    &runner_nat_selftest_fops);
	debugfs_create_file("ringstat", 0400, p->dbg, p, &runner_ringstat_fops);
}

/* ============================ netdev / probe ============================ */
static int runner_ndo_open(struct net_device *ndev)
{
	struct runner_priv *p = netdev_priv(ndev);

	napi_enable(&p->napi);
	netif_start_queue(ndev);
	if (p->rx_irq <= 0)	/* poll mode fallback for first light */
		napi_schedule(&p->napi);
	return 0;
}

static int runner_ndo_stop(struct net_device *ndev)
{
	struct runner_priv *p = netdev_priv(ndev);

	netif_stop_queue(ndev);
	napi_disable(&p->napi);
	return 0;
}

/*
 * TC_SETUP_FT / TC_SETUP_BLOCK: register the flowtable HW-offload block on the
 * conduit so nf_flow_table programs L2/VLAN flows into NAT-C (Phase 1). Modelled
 * on mtk_eth_setup_tc (mtk_ppe_offload.c:660).
 */
static int runner_ndo_setup_tc(struct net_device *ndev, enum tc_setup_type type,
			       void *type_data)
{
	struct runner_priv *p = netdev_priv(ndev);

	return xrdp_offload_setup_tc(&p->offload, ndev, type, type_data);
}

static const struct net_device_ops runner_netdev_ops = {
	.ndo_open	= runner_ndo_open,
	.ndo_stop	= runner_ndo_stop,
	.ndo_start_xmit	= runner_start_xmit,
	.ndo_setup_tc	= runner_ndo_setup_tc,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

/* ====================== 10G XPORT SerDes (merlin16) ===================== *
 * Minimal merlin16_shortfin firmware loader: stream the ucode blob into the
 * SerDes micro program-RAM and start the uC, then confirm it runs (uc_active).
 * This is STEP 1 of 10G XPORT bring-up; the full link (PLL/VCO, lane AFE,
 * speed/AN, PMD lock) is a larger follow-on (best served by reusing the open
 * SDK merlin16 driver). Access = the indirect ADDR/MASK/CNTRL window at
 * 0x837ff500 (per core). [merlin16_shortfin_config.c, serdes_access.c]
 */
static int serdes_xfer(struct runner_priv *p, u16 reg, u16 mask, u16 val,
		       bool write, u16 *out)
{
	void __iomem *b = p->serdes + serdes_core * SERDES_CORE_STRIDE;
	u32 addr = ((u32)SERDES_PMD_DEV << SERDES_DEV_TYPE_SHIFT) |
		   ((u32)serdes_lane << SERDES_LANE_SHIFT) | reg;
	u32 cntrl = 0;
	int i;

	writel(addr, b + SERDES_OFF_INDIR_ADDR);
	if (write) {
		writel((u16)~mask, b + SERDES_OFF_INDIR_MASK);
		cntrl = (u32)val | SERDES_CNTRL_START_BUSY | SERDES_CNTRL_DELAYED_ACK;
	} else {
		cntrl = SERDES_CNTRL_RW | SERDES_CNTRL_START_BUSY |
			SERDES_CNTRL_DELAYED_ACK;
	}
	writel(cntrl, b + SERDES_OFF_INDIR_CNTRL);

	for (i = 0; i < 1000; i++) {
		cntrl = readl(b + SERDES_OFF_INDIR_CNTRL);
		if (!(cntrl & SERDES_CNTRL_START_BUSY))
			break;
		udelay(1);
	}
	if (cntrl & SERDES_CNTRL_START_BUSY)
		return -ETIMEDOUT;
	if (out)
		*out = cntrl & mask;
	return 0;
}

static inline int serdes_wr_reg(struct runner_priv *p, u16 reg, u16 val)
{
	return serdes_xfer(p, reg, 0xffff, val, true, NULL);
}
/* write field {mask,shift} = v (v is the logical field value, pre-shift) */
static inline int serdes_wr_f(struct runner_priv *p, u16 reg, u16 mask,
			      u16 shift, u16 v)
{
	return serdes_xfer(p, reg, mask, (v << shift) & mask, true, NULL);
}
static inline int serdes_rd(struct runner_priv *p, u16 reg, u16 mask, u16 *out)
{
	return serdes_xfer(p, reg, mask, 0, false, out);
}

static int serdes_poll_ra_initdone(struct runner_priv *p)
{
	u16 v;
	int i;

	for (i = 0; i < 250; i++) {
		if (serdes_rd(p, SRD_MICRO_AHB_STATUS, SRD_MICRO_RA_INITDONE, &v) == 0 && v)
			return 0;
		udelay(1);
	}
	dev_warn(p->dev, "serdes: RAM init timeout\n");
	return -ETIMEDOUT;
}

static int runner_serdes_load(struct runner_priv *p)
{
	/* micro register block reset-to-default values [merlin16_shortfin_config.c
	 * uc_reset(enable=1)]; the few non-zero ones are spelled out below. */
	static const u16 zero_regs[] = {
		0xD200, 0xD201, 0xD202, 0xD204, 0xD205, 0xD206, 0xD207, 0xD208,
		0xD209, 0xD20A, 0xD20B, 0xD20C, 0xD20D, 0xD20E, 0xD211, 0xD212,
		0xD213, 0xD214, 0xD215, 0xD217, 0xD218, 0xD219, 0xD21A, 0xD21B,
		0xD220, 0xD221, 0xD224, 0xD226, 0xD229, 0xD22A,
	};
	const struct firmware *fw;
	int ret, i;
	u16 v = 0;

	ret = request_firmware(&fw, SERDES_FW_NAME, p->dev);
	if (ret) {
		dev_warn(p->dev, "serdes: firmware '%s' absent (%d); 10G XPORT skipped\n",
			 SERDES_FW_NAME, ret);
		return ret;
	}
	if (fw->size != SERDES_FW_SIZE)
		dev_warn(p->dev, "serdes: fw size %zu != expected %d\n",
			 fw->size, SERDES_FW_SIZE);
	dev_info(p->dev, "serdes: loading merlin16 uC core%u lane%u (%zu B)\n",
		 serdes_core, serdes_lane, fw->size);

	/* --- assert uC reset: clocks off, micro reg block to defaults --- */
	serdes_wr_f(p, SRD_MICRO_CLK_CTRL, SRD_MICRO_CLK_CORE, 1, 0);
	serdes_wr_f(p, SRD_MICRO_CLK_CTRL, SRD_MICRO_CLK_MASTER, 0, 0);
	for (i = 0; i < ARRAY_SIZE(zero_regs); i++)
		serdes_wr_reg(p, zero_regs[i], 0x0000);
	serdes_wr_reg(p, 0xD216, 0x0007);
	serdes_wr_reg(p, 0xD225, 0x8201);
	serdes_wr_reg(p, 0xD228, 0x0101);

	/* --- toggle subsystem reset, init code + data RAM --- */
	serdes_wr_f(p, SRD_MICRO_CLK_CTRL, SRD_MICRO_CLK_MASTER, 0, 1);
	serdes_wr_f(p, SRD_MICRO_RST_CTRL, SRD_MICRO_RST_MASTER, 0, 1);
	serdes_wr_f(p, SRD_MICRO_RST_CTRL, SRD_MICRO_RST_MASTER, 0, 0);
	serdes_wr_f(p, SRD_MICRO_RST_CTRL, SRD_MICRO_RST_MASTER, 0, 1);
	serdes_wr_f(p, SRD_MICRO_AHB_CTRL, SRD_MICRO_RA_INIT, SRD_MICRO_RA_INIT_SHIFT, 1);
	ret = serdes_poll_ra_initdone(p);
	if (ret)
		goto out;
	serdes_wr_f(p, SRD_MICRO_AHB_CTRL, SRD_MICRO_RA_INIT, SRD_MICRO_RA_INIT_SHIFT, 2);
	ret = serdes_poll_ra_initdone(p);
	if (ret)
		goto out;
	serdes_wr_f(p, SRD_MICRO_AHB_CTRL, SRD_MICRO_RA_INIT, SRD_MICRO_RA_INIT_SHIFT, 0);

	/* --- program-RAM write port: autoinc, 16-bit words, addr 0 --- */
	serdes_wr_f(p, SRD_MICRO_AHB_CTRL, SRD_MICRO_AUTOINC_WR, 12, 1);
	serdes_wr_f(p, SRD_MICRO_AHB_CTRL, SRD_MICRO_RA_WRDATASIZE, 0, 1);
	serdes_wr_reg(p, SRD_MICRO_RA_WRADDR_MSW, 0);
	serdes_wr_reg(p, SRD_MICRO_RA_WRADDR_LSW, 0);

	/* --- stream blob as 16-bit LE words (raw, no transform) --- */
	for (i = 0; i + 1 < fw->size; i += 2) {
		ret = serdes_wr_reg(p, SRD_MICRO_RA_WRDATA_LSW,
				    fw->data[i] | ((u16)fw->data[i + 1] << 8));
		if (ret)
			goto out;
	}
	if (fw->size & 1)
		serdes_wr_reg(p, SRD_MICRO_RA_WRDATA_LSW, fw->data[fw->size - 1]);
	serdes_wr_f(p, SRD_MICRO_AHB_CTRL, SRD_MICRO_RA_WRDATASIZE, 0, 2);
	serdes_wr_f(p, SRD_MICRO_CLK_CTRL, SRD_MICRO_CLK_CORE, 1, 1);

	/* --- release/start the uC --- */
	serdes_wr_f(p, SRD_MICRO_CLK_CTRL, SRD_MICRO_CLK_MASTER, 0, 1);
	serdes_wr_f(p, SRD_MICRO_RST_CTRL, SRD_MICRO_RST_MASTER, 0, 1);
	serdes_wr_f(p, SRD_MICRO_CLK_CTRL, SRD_MICRO_CLK_CORE, 1, 1);
	serdes_wr_f(p, SRD_MICRO_PMI_IF_CTRL, SRD_MICRO_PMI_HP_FAST, 0, 0);
	serdes_wr_f(p, SRD_MICRO_RST_CTRL, SRD_MICRO_RST_CORE, 1, 1);

	/* --- confirm uC running (uc_active) --- */
	for (i = 0; i < 10000; i++) {
		if (serdes_rd(p, SRD_UC_ACTIVE_REG, SRD_UC_ACTIVE, &v) == 0 && v)
			break;
		udelay(1);
	}
	if (v) {
		dev_info(p->dev, "serdes: merlin16 uC ACTIVE core%u (fw running)\n",
			 serdes_core);
		ret = 0;
	} else {
		dev_warn(p->dev, "serdes: uC NOT active after load (uc_active=0)\n");
		ret = -EIO;
	}
out:
	release_firmware(fw);
	return ret;
}

static int runner_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev;
	struct runner_priv *p;
	struct resource *res;
	int ret, i;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));
	if (ret) {
		/* a non-DT (shim) device may not honor 40-bit; try 32, and don't
		 * abort if even that is refused - the coherent allocs below will
		 * surface a real DMA failure with a clearer error. */
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret)
			dev_warn(dev, "DMA mask not honored (%d); continuing best-effort\n",
				 ret);
	}

	ndev = devm_alloc_etherdev(dev, sizeof(*p));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, dev);

	p = netdev_priv(ndev);
	p->pdev = pdev;
	p->dev = dev;
	p->ndev = ndev;
	platform_set_drvdata(pdev, p);

	/* The QEMU model advertises emulation via DT; fold it into runner_emulated
	 * so every HW-only step (microcode load, EGPHY/MAC-PHY) is skipped
	 * consistently (the module param is the other way to set it). */
	if (of_property_read_bool(dev->of_node, "brcm,runner-emulated"))
		runner_emulated = true;

	/*
	 * Map the XRDP window WITHOUT request_mem_region: the window hosts many
	 * sub-devices (MDIO/serdes/PHY under the xrdp simple-bus) that already
	 * claim sub-regions, so requesting the whole window returns -EBUSY. The
	 * stock rdpa shares the window the same way (ioremap, no request).
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	p->xrdp = devm_ioremap(dev, res->start, resource_size(res));
	if (!p->xrdp)
		return -ENOMEM;
	/*
	 * The eth-phy-top / quad-EGPHY / mdiosf2 / xport blocks sit in the
	 * 0x83000000 SoC region, OUTSIDE the rdpa window - map them separately.
	 * (Absolute phys base; the stock built-in eth-phy-top driver maps the
	 * same region with ioremap and no request_mem_region, so this coexists.)
	 * Skipped under emulation: the QEMU model only backs the rdpa window, so
	 * touching 0x837f0000 there would fault.
	 */
	if (!runner_emulated) {
		p->ethphytop = devm_ioremap(dev, ETHPHY_PHYS_BASE, ETHPHY_SIZE);
		if (!p->ethphytop)
			return -ENOMEM;
	}
	/* 10G XPORT SerDes indirect window (opt-in, HW only). */
	if (!runner_emulated && serdes_fw_load) {
		p->serdes = devm_ioremap(dev, SERDES_PHYS_BASE, SERDES_SIZE);
		if (!p->serdes)
			return -ENOMEM;
	}
	p->fpm = p->xrdp + XRDP_OFF_FPM;
	p->natc = p->xrdp + XRDP_OFF_NATC;
	for (i = 0; i < XRDP_RNR_CORES; i++) {
		p->rnr_mem[i] = p->xrdp + XRDP_OFF_RNR_MEM0 +
				i * XRDP_RNR_MEM_STRIDE;
		p->rnr_regs[i] = p->xrdp + XRDP_OFF_RNR_REGS0 +
				 i * XRDP_RNR_REGS_STRIDE;
	}

	p->rx_irq = platform_get_irq_byname_optional(pdev, "queue0");
	if (p->rx_irq < 0)
		p->rx_irq = 0;	/* poll mode */

	/*
	 * Runner bring-up (spec re-notes/realhw/10-runner-bringup-spec.md):
	 *   UBUS decode -> FPM pool -> RNR pre-cfg (zero mem) -> microcode load ->
	 *   SBPM -> DSPTCHR -> rings (alloc+publish) -> BBH -> RNR enable (LAST).
	 * All MMIO below only runs under a real probe (a DT node with our
	 * compatible), so it is inert on a stock kernel that lacks the node.
	 */
	runner_ubus_decode_init(p);
	ret = fpm_pool_init(p);
	if (ret)
		return ret;
	runner_rnr_precfg(p);		/* zero core SRAM + cfg, before microcode */
	ret = runner_load_microcode(p);
	if (ret)
		return ret;
	runner_sbpm_init(p);
	runner_dsptchr_init(p);
	/* Route A (opt-in): full QM bring-up (SRAM auto-init + RUNNER_GRP + enable)
	 * so the TM core can drain a queue into BBH_TX. FPM/SBPM are already up. */
	if (route_a)
		runner_qm_init(p);
	ret = rx_ring_alloc(p);
	if (ret)
		return ret;
	ret = tx_ring_alloc(p);
	if (ret)
		return ret;
	rx_ring_publish(p);
	/*
	 * First-light port = the 1G EGPHY port_gphy1 (eth2-class): UNIMAC inst 1,
	 * EGPHY MDIO addr 2, BBH_ID 1. ★The exact emac->bbh_id mapping is resolved
	 * in the closed rdpa.ko (BBH_ID 0 vs 1 for this port is unconfirmed) - tune
	 * RUNNER_FIRST_PORT on silicon. VIQ == bbh_id. No serdes blob (1G path).
	 */
	if (!runner_emulated)
		runner_mac_phy_init(p, RUNNER_FIRST_PORT);	/* 1G UNIMAC + EGPHY (HW only) */
	if (!runner_emulated && serdes_fw_load)
		runner_serdes_load(p);		/* 10G XPORT merlin16 fw (opt-in, step 1) */
	runner_bbh_init(p, RUNNER_FIRST_PORT);
	/*
	 * Opt-in 10G XPORT port (eth0/port5 = xport_port_id 0, eth1/port6 = id 2).
	 * eth1/merlin16 also needs serdes_fw_load=1 (loaded above); eth0's internal
	 * XPHY is self-contained. bbh_id: xport_port_id 0 -> port5, 2 -> port6.
	 * [re-notes/realhw/12-10g-xport-bringup.md]
	 */
	if (!runner_emulated && port10g >= 0) {
		u32 pviq = (port10g == 0) ? 5 : 6;	/* xport_port_id 0->port5, 2->port6 */

		if (port10g == 0) {
			runner_xphy_init(p, 0, XPHY0_MDIO_ADDR);	/* eth0 XPHY out of reset */
			runner_xphy_fw_load(p, XPHY0_MDIO_ADDR);	/* + load its uC firmware */
		}
		runner_xport_init(p, port10g);			/* 10G XPORT/XLMAC MAC */
		runner_bbh_init(p, pviq);			/* + its BBH_RX/TX */
		runner_dsptchr_add_port_viq(p, pviq);		/* + dispatcher VIQ -> CPU_RX */
	}
	if (route_a)
		runner_bbh_tx_route_a(p);	/* QMQ=1 binding so the TM core's queue egresses */
	runner_thread_regfile_init(p);	/* CPU thread initial registers (post-zero, pre-enable) */
	runner_rnr_enable(p);		/* CFG_GLOBAL_CTRL.EN + cpu_wakeup, LAST */

	netif_napi_add(ndev, &p->napi, runner_rx_poll);
	ndev->netdev_ops = &runner_netdev_ops;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = p->chunk_size - ETH_HLEN;
	eth_hw_addr_random(ndev);

	/* HW flow-offload (Phase 1: L2 + VLAN). Advertise NETIF_F_HW_TC so the
	 * flowtable/TC layer offers us a flow_block to bind. */
	p->offload.drv = p;
	p->offload.default_vport = 0;
	ret = xrdp_offload_init(&p->offload);
	if (ret)
		return ret;
	ndev->features |= NETIF_F_HW_TC;
	ndev->hw_features |= NETIF_F_HW_TC;
	runner_debugfs_init(p);

	/*
	 * Name the conduit "rnr%d" so it never collides with the DSA user-port
	 * netdev labels (eth0/eth2/...) the switch driver registers; otherwise
	 * the default "eth%d" allocation races the DSA ports (-EEXIST).
	 */
	strscpy(ndev->name, "rnr%d", IFNAMSIZ);

	if (p->rx_irq > 0) {
		ret = devm_request_irq(dev, p->rx_irq, runner_rx_isr, 0,
				       DRV_NAME, p);
		if (ret)
			return ret;
	}

	ret = devm_register_netdev(dev, ndev);
	if (ret)
		return ret;

	dev_info(dev, "BCM4916 Runner conduit ready (%s, irq %d, fw %s)\n",
		 runner_emulated ? "emulated" : "hw",
		 p->rx_irq, p->fw_loaded ? "loaded" : "absent");
	return 0;
}

/* platform_driver.remove() returns void only from 6.11 (the .remove_new
 * rename); older trees (incl. the 4.19 vendor fork) expect an int. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
static int runner_remove(struct platform_device *pdev)
#else
static void runner_remove(struct platform_device *pdev)
#endif
{
	struct runner_priv *p = platform_get_drvdata(pdev);
	int c;

	/*
	 * QUIESCE the Runner before devm frees the DMA rings/pool: the microcode'd
	 * cores were woken in probe and keep DMAing into the feed/RX/TX rings, so
	 * tearing those down (or re-probing) while the cores run hangs the SoC.
	 * Disable each core's run-enable (CFG_GLOBAL_CTRL.EN=0) so a clean re-probe
	 * starts the bring-up from a halted state.
	 */
	for (c = 0; c < XRDP_RNR_CORES; c++) {
		if (p->rnr_regs[c])
			writel(0, p->rnr_regs[c] + RNR_CFG_GLOBAL_CTRL);
	}
	/* let in-flight DMA settle before devm releases the coherent rings */
	wmb();
	mdelay(2);

	debugfs_remove_recursive(p->dbg);
	xrdp_offload_deinit(&p->offload);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	return 0;
#endif
}

static const struct of_device_id runner_of_match[] = {
	{ .compatible = "brcm,bcm4916-runner" },
	/*
	 * Also bind the STOCK rdpa_drv DT node ("brcm,rdpa") directly. On a trial
	 * boot where the stock datapath (rdpa.ko) is NOT loaded, that node is left
	 * unbound; binding it gives us the real XRDP reg window as our OWN device
	 * resource (request_mem_region succeeds - no -EBUSY from a duplicate),
	 * the of_dma_configure() DMA setup the DT device already got at boot (no
	 * -EIO), and the real "queue0" CPU-RX IRQ - with NO DT change and NO
	 * platform_device shim. (On a normal boot rdpa.ko owns this node first.)
	 */
	{ .compatible = "brcm,rdpa" },
	{ }
};
MODULE_DEVICE_TABLE(of, runner_of_match);

static struct platform_driver runner_driver = {
	.probe	= runner_probe,
	.remove	= runner_remove,
	.driver	= {
		.name		= DRV_NAME,
		.of_match_table	= runner_of_match,
	},
};
module_platform_driver(runner_driver);

MODULE_DESCRIPTION("Open BCM4916 XRDP Runner CPU-conduit slow-path datapath");
MODULE_AUTHOR("gt-be98-open-ethernet");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(RUNNER_FW_NAME);
