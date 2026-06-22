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
#include <linux/delay.h>
#include <linux/byteorder/generic.h>
#include <asm/unaligned.h>
#include <linux/in.h>
#include <linux/debugfs.h>

#include "bcm4916_runner.h"
#include "flow_offload.h"

#define DRV_NAME		"bcm4916-runner"
#define RUNNER_FW_NAME		"brcm/bcm4916-runner-microcode.bin"

/* Ring geometry. Depth must fit number_of_entries[10:0] (<=2047). */
#define RX_RING_DEPTH		256	/* runner-written delivery ring */
#define FEED_RING_DEPTH		256	/* host-posted empty-buffer ring */
#define TX_RING_DEPTH		256
#define RX_BUF_SIZE		2048	/* one DDR buffer per RX frame */
#define RX_NAPI_BUDGET		64

static bool runner_emulated;
module_param(runner_emulated, bool, 0444);
MODULE_PARM_DESC(runner_emulated,
		 "Skip proprietary Runner microcode load (QEMU emulator path).");

/* ------------------------------------------------------------------------- */
struct runner_priv {
	struct platform_device	*pdev;
	struct device		*dev;
	struct net_device	*ndev;

	void __iomem		*xrdp;		/* whole rdpa window */
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
	u32			natc_next_idx;	/* simple slot allocator */
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
 * HW field encodings [VERIFIED vs SDK]:
 *  - number_of_entries is in 64-entry units: field = depth>>6. The runner wraps
 *    write_idx at (number_of_entries<<6)==depth (rdd_cpu_inc_feed_ring_write_idx,
 *    CPU_RING_SIZE_64_RESOLUTION=6). => depth MUST be a multiple of 64.
 *  - size_of_entry is the entry size in BYTES (rdp_cpu_ring.c:323
 *    size_of_entry = sizeof(CPU_RX_DESCRIPTOR)), not log2.
 *  - w0: half@0 = size_of_entry[15:11] | number_of_entries[10:0];
 *    half@2 = interrupt_id; w1 (drop_counter, write_idx)=0;
 *    w3 = read_idx[15:0]=0 | base_addr_high@f. All big-endian.
 */
static void ring_publish(struct runner_priv *p, int core, u32 tbl_off,
			 dma_addr_t phys, u32 depth, u32 entry_sz, u16 irq)
{
	struct runner_ring_cfg cfg = {};

	cfg.w0 = cpu_to_be32(((entry_sz & 0x1f) << 27) |
			     (((depth >> 6) & 0x7ff) << 16) | irq);
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
	/* delivery ring (runner -> host), 16B descriptors, on core 3 */
	ring_publish(p, CPU_RX_RING_CORE, PSRAM_CPU_RING_DESC_TABLE,
		     p->rx_ring_phys, RX_RING_DEPTH,
		     sizeof(struct runner_rx_desc), p->rx_irq & 0xffff);
	/* feed ring (host -> runner), 8B descriptors, on core 3 */
	ring_publish(p, CPU_RX_RING_CORE, RDD_FEED_RING_DESC_TABLE,
		     p->feed_ring_phys, FEED_RING_DEPTH,
		     sizeof(struct runner_feed_desc), 0);
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
	__be16 be = cpu_to_be16(write_idx);
	int core;

	/* dmb oshst before publishing the index (ABI 5bis-G2 step 6) */
	dma_wmb();

	/*
	 * Bump CPU_TX_RING_INDICES_VALUES_TABLE write_idx in each serving
	 * core's RNR_MEM SRAM. For the dumb-pipe v1 / emulator a single
	 * serving core is sufficient; real HW writes all cores whose address
	 * is not the 0xffffff "not-serving" sentinel.
	 */
	for (core = 0; core < XRDP_RNR_CORES; core++) {
		if (!p->rnr_mem[core])
			continue;
		iowrite16(be, p->rnr_mem[core] + CPU_TX_RING_INDICES_OFF);
	}
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
	d->word0 = cpu_to_be32(TXD_W0_IS_EGRESS |
			       ((len & TXD_W0_PKT_LEN_MASK) << TXD_W0_PKT_LEN_SHIFT));
	d->word1 = 0;
	/*
	 * is_emac=1 (UNIMAC egress for the dumb-pipe CPU port), abs=0 (FPM
	 * token mode), egress port left 0 - DSA tags select the real egress.
	 */
	/*
	 * abs=0 (FPM-token) egress: word3 = fpm_bn0[19:0] | fpm_sop[29:20]
	 * [VERIFIED vs RING_CPU_TX_DESCRIPTOR + fpm_core.c:376/rdd_cpu_tx.h:119].
	 * bn0 = fpm_convert_fpm_token_to_rdp_token(token); SOP = 0 (we copied the
	 * frame to the FPM buffer start). do_not_recycle stays 0 so the runner
	 * auto-frees the FPM buffer after transmit (no host reclaim needed).
	 */
	d->word2 = cpu_to_be32(TXD_W2_IS_EMAC);
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
		     sizeof(struct runner_tx_desc), 0);
	return 0;
}

/* ============================ NAT-C offload I/O ========================== *
 * The open analog of drv_natc_key_result_entry_var_size_ctx_add (ABI sec 1.1):
 * stage the 16-byte masked BE key + the FC_UCAST_FLOW_CONTEXT_ENTRY into PSRAM
 * staging windows, write the table index, then issue the indirect "add"
 * command (cmd=3). The QEMU model watches these PSRAM writes and the command
 * register to populate its modelled NAT-C table.
 *
 * The PSRAM staging offsets + indirect command register here are CONTRACT
 * placeholders (real RDD/NAT-C indirect-register offsets are ABI UNKNOWN #5);
 * driver and model agree on them.
 */
int xrdp_natc_add(struct xrdp_offload *o, const struct natc_key *key,
		  const struct fc_ucast_ctx *ctx, u32 *idx_out)
{
	struct runner_priv *p = o->drv;
	u32 idx = p->natc_next_idx++;

	/* 1. stage the 16-byte masked BE key */
	memcpy_toio(p->xrdp + XRDP_OFF_PSRAM + NATC_STAGE_KEY, key->w,
		    sizeof(key->w));

	/* 2. stage the variable-length context (result) */
	memcpy_toio(p->xrdp + XRDP_OFF_PSRAM + NATC_STAGE_CTX, ctx->buf,
		    ctx->len);

	/* 3. write the table index */
	writel(idx, p->xrdp + XRDP_OFF_PSRAM + NATC_INDIR_INDEX);

	/* 4. issue the add command (cmd=3) - this is the "doorbell" the model
	 *    keys on to copy {key, ctx} into its NAT-C table. */
	dma_wmb();
	writel(NATC_CMD_ADD, p->xrdp + XRDP_OFF_PSRAM + NATC_INDIR_CMD);

	*idx_out = idx;
	return 0;
}

void xrdp_natc_del(struct xrdp_offload *o, const struct natc_key *key, u32 idx)
{
	struct runner_priv *p = o->drv;

	memcpy_toio(p->xrdp + XRDP_OFF_PSRAM + NATC_STAGE_KEY, key->w,
		    sizeof(key->w));
	writel(idx, p->xrdp + XRDP_OFF_PSRAM + NATC_INDIR_INDEX);
	dma_wmb();
	writel(NATC_CMD_DEL, p->xrdp + XRDP_OFF_PSRAM + NATC_INDIR_CMD);
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

			if (!p->rnr_mem[c] ||
			    (u64)io + il > fw->size || (u64)po + pl > fw->size) {
				dev_err(p->dev, "runner fw: core %u bad extent\n", c);
				release_firmware(fw);
				return -EINVAL;
			}
			memcpy_toio(p->rnr_mem[c] + XRDP_RNR_INST_OFF, d + io, il);
			memcpy_toio(p->rnr_mem[c] + XRDP_RNR_PRED_OFF, d + po, pl);
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
		if (readl(s + SBPM_INIT_FREE_LIST) & SBPM_INIT_FREE_LIST_RDY)
			return 0;
		udelay(10);
	}
	dev_warn(p->dev, "SBPM free-list init did not signal RDY\n");
	return 0;	/* non-fatal for first-light */
}

static int runner_dsptchr_init(struct runner_priv *p)
{
	void __iomem *d = p->xrdp + XRDP_OFF_DSPTCHR;
	int i;

	/* let the HW auto-init the free linked list, then enable the reorder
	 * engine; poll RDY (bit8). (Hand-seeding 1024 nodes is the alternative.) */
	writel(DSPTCHR_REORDER_CFG_EN | DSPTCHR_REORDER_CFG_AUTO_INIT,
	       d + DSPTCHR_REORDER_CFG);
	for (i = 0; i < 1000; i++) {
		if (readl(d + DSPTCHR_REORDER_CFG) & DSPTCHR_REORDER_CFG_RDY)
			return 0;
		udelay(10);
	}
	dev_warn(p->dev, "DSPTCHR reorder init did not signal RDY\n");
	return 0;	/* non-fatal for first-light */
}

/* configure + enable one BBH_RX MAC port and the LAN BBH_TX instance */
static void runner_bbh_init(struct runner_priv *p, int rx_port)
{
	void __iomem *rx = p->xrdp + XRDP_OFF_BBH_RX0 +
			   (u32)rx_port * XRDP_BBH_RX_STRIDE;
	void __iomem *tx = p->xrdp + XRDP_OFF_BBH_TX0;	/* BBH_TX_ID_LAN = 0 */

	/* BBH_RX: point the dispatcher + SBPM block IDs, then enable last */
	writel(((u32)BBH_BBID_DISPATCHER << 8) | ((u32)BBH_BBID_SBPM << 16),
	       rx + BBH_RX_BBCFG);
	writel(BBH_RX_ENABLE_PKTEN | BBH_RX_ENABLE_SBPMEN, rx + BBH_RX_ENABLE);

	/* BBH_TX: MAC type + FPM/SBPM source block IDs */
	writel(BBH_TX_MACTYPE_VAL, tx + BBH_TX_MACTYPE);
	writel(((u32)BBH_BBID_FPM << 24) | ((u32)BBH_BBID_SBPM << 16),
	       tx + BBH_TX_BBCFG_1);
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

static void runner_debugfs_init(struct runner_priv *p)
{
	p->dbg = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("offload_selftest", 0200, p->dbg, p,
			    &runner_selftest_fops);
	debugfs_create_file("offload_nat_selftest", 0200, p->dbg, p,
			    &runner_nat_selftest_fops);
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

static int runner_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev;
	struct runner_priv *p;
	int ret, i;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));
	if (ret)
		return ret;

	ndev = devm_alloc_etherdev(dev, sizeof(*p));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, dev);

	p = netdev_priv(ndev);
	p->pdev = pdev;
	p->dev = dev;
	p->ndev = ndev;
	platform_set_drvdata(pdev, p);

	p->xrdp = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->xrdp))
		return PTR_ERR(p->xrdp);
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
	ret = rx_ring_alloc(p);
	if (ret)
		return ret;
	ret = tx_ring_alloc(p);
	if (ret)
		return ret;
	rx_ring_publish(p);
	/* one MAC port for first-light; the real port map needs on-silicon
	 * iteration (BBH_RX has 11 MAC instances) - flagged in the spec. */
	runner_bbh_init(p, 0);
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

static void runner_remove(struct platform_device *pdev)
{
	struct runner_priv *p = platform_get_drvdata(pdev);

	debugfs_remove_recursive(p->dbg);
	xrdp_offload_deinit(&p->offload);
}

static const struct of_device_id runner_of_match[] = {
	{ .compatible = "brcm,bcm4916-runner" },
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
