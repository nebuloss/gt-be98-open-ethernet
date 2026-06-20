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
#include <linux/debugfs.h>

#include "bcm4916_runner.h"
#include "flow_offload.h"

#define DRV_NAME		"bcm4916-runner"
#define RUNNER_FW_NAME		"brcm/bcm4916-runner-microcode.bin"

/* Ring geometry. Depth must fit number_of_entries[10:0] (<=2047). */
#define RX_RING_DEPTH		256
#define TX_RING_DEPTH		256
#define RX_BUF_SIZE		2048	/* one FPM/DDR chunk per RX buffer */
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
	void __iomem		*rnr_mem[XRDP_RNR_CORES];

	/* FPM pool (slow-path RX/TX buffers) */
	void			*pool_vbase;
	dma_addr_t		pool_pbase;
	u32			pool_size;
	u32			chunk_size;

	/* CPU-RX data ring (self-refilling, enet impl7 model) */
	struct runner_rx_desc	*rx_ring;	/* coherent DDR */
	dma_addr_t		rx_ring_phys;
	void			**rx_buf;	/* virt of each posted buffer */
	dma_addr_t		*rx_buf_phys;
	u32			rx_head;	/* host consumer index */
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

/* ============================ CPU RX data ring =========================== *
 * Self-refilling data ring, enet impl7 model (enet_ring.c create_ring /
 * runner_get_pkt_from_ring / rdpa_cpu_ring_rest_desc). Each descriptor owns a
 * DDR buffer; word2 = swap4(buf_phys & 0x7fffffff) with ownership bit cleared
 * hands the slot to the Runner. The Runner DMAs a frame in, sets ownership and
 * packet_length, the host consumes and re-arms in place.
 */

/* hand descriptor 'd' a fresh buffer at buf_phys, ownership -> RUNNER */
static void rx_rest_desc(struct runner_rx_desc *d, dma_addr_t buf_phys)
{
	/* rdpa_cpu_ring_rest_desc: word2 = swap4(phys & 0x7fffffff) (own=RUNNER) */
	d->word0 = 0;
	d->word1 = 0;
	d->word3 = 0;
	/* store big-endian; clearing bit31 == OWNERSHIP_RUNNER */
	d->word2 = cpu_to_be32((u32)buf_phys & RXD_W2_BUF_PTR_MASK);
}

static int rx_ring_alloc(struct runner_priv *p)
{
	int i;

	p->rx_ring = dmam_alloc_coherent(p->dev,
					 RX_RING_DEPTH * sizeof(*p->rx_ring),
					 &p->rx_ring_phys, GFP_KERNEL);
	if (!p->rx_ring)
		return -ENOMEM;

	p->rx_buf = devm_kcalloc(p->dev, RX_RING_DEPTH, sizeof(void *),
				 GFP_KERNEL);
	p->rx_buf_phys = devm_kcalloc(p->dev, RX_RING_DEPTH, sizeof(dma_addr_t),
				      GFP_KERNEL);
	if (!p->rx_buf || !p->rx_buf_phys)
		return -ENOMEM;

	for (i = 0; i < RX_RING_DEPTH; i++) {
		void *buf = dmam_alloc_coherent(p->dev, RX_BUF_SIZE,
						&p->rx_buf_phys[i], GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		p->rx_buf[i] = buf;
		rx_rest_desc(&p->rx_ring[i], p->rx_buf_phys[i]);
	}
	p->rx_head = 0;
	return 0;
}

/*
 * Publish the RX ring to the Runner via a CPU_RING_DESCRIPTOR in PSRAM.
 * ABI sec 2.1 / bring-up step 8. The PSRAM table offset for the data ring
 * (CPU_RING_DESCRIPTORS_TABLE) is an RDD symbol not yet pinned from the GPL
 * map; the emulator accepts the ring-cfg at the contract offset.
 */
#define PSRAM_CPU_RING_DESC_TABLE	0x0000	/* TODO: pin RDD offset */

static void rx_ring_publish(struct runner_priv *p)
{
	struct runner_ring_cfg cfg = {};
	u8 size_code = ilog2(sizeof(struct runner_rx_desc)); /* 16B -> 4 */
	u64 phys = p->rx_ring_phys;

	/* w0: byte0[7:3]=size_of_entry, half@0[10:0]=number_of_entries,
	 *     half@2=interrupt_id. Big-endian. */
	cfg.w0 = cpu_to_be32(((u32)(size_code & 0x1f) << 27) |
			     ((RX_RING_DEPTH & 0x7ff) << 16) |
			     (p->rx_irq & 0xffff));
	cfg.w1 = 0;					/* drop_counter, write_idx */
	cfg.base_addr_low = cpu_to_be32((u32)phys);
	/* w3: half@c read_idx=0, byte@f = base_addr_high */
	cfg.w3 = cpu_to_be32((u32)(phys >> 32) & 0xff);

	memcpy_toio(p->xrdp + XRDP_OFF_PSRAM + PSRAM_CPU_RING_DESC_TABLE,
		    &cfg, sizeof(cfg));
}

static int runner_rx_poll(struct napi_struct *napi, int budget)
{
	struct runner_priv *p = container_of(napi, struct runner_priv, napi);
	int done = 0;

	while (done < budget) {
		struct runner_rx_desc *d = &p->rx_ring[p->rx_head];
		u32 w2 = be32_to_cpu(d->word2);
		u32 w0;
		u16 len;
		struct sk_buff *skb;
		void *buf;

		/* ABI 2.4: ownership = bit31 of word2 post-swap. */
		if (!(w2 & RXD_W2_OWNERSHIP_HOST))
			break;

		w0  = be32_to_cpu(d->word0);
		len = w0 & RXD_W0_PKT_LEN_MASK;
		buf = p->rx_buf[p->rx_head];

		dma_sync_single_for_cpu(p->dev, p->rx_buf_phys[p->rx_head],
					RX_BUF_SIZE, DMA_FROM_DEVICE);

		if (len && len <= RX_BUF_SIZE) {
			skb = napi_alloc_skb(napi, len);
			if (skb) {
				skb_put_data(skb, buf, len);
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

		/* re-arm slot in place (same buffer), ownership -> RUNNER */
		dma_sync_single_for_device(p->dev, p->rx_buf_phys[p->rx_head],
					   RX_BUF_SIZE, DMA_FROM_DEVICE);
		rx_rest_desc(d, p->rx_buf_phys[p->rx_head]);

		p->rx_head = (p->rx_head + 1) % RX_RING_DEPTH;
		done++;
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
	d->word2 = cpu_to_be32(TXD_W2_IS_EMAC);
	d->word3 = cpu_to_be32(token);

	p->tx_write_idx = (p->tx_write_idx + 1) % TX_RING_DEPTH;
	tx_ring_doorbell(p, p->tx_write_idx);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
}

#define PSRAM_CPU_TX_RING_DESC_TABLE	0x0080	/* TODO: pin RDD offset */

static int tx_ring_alloc(struct runner_priv *p)
{
	struct runner_ring_cfg cfg = {};
	u64 phys;

	p->tx_ring = dmam_alloc_coherent(p->dev,
					 TX_RING_DEPTH * sizeof(*p->tx_ring),
					 &p->tx_ring_phys, GFP_KERNEL);
	if (!p->tx_ring)
		return -ENOMEM;
	p->tx_write_idx = 0;

	phys = p->tx_ring_phys;
	cfg.w0 = cpu_to_be32(((ilog2(sizeof(struct runner_tx_desc)) & 0x1f) << 27) |
			     ((TX_RING_DEPTH & 0x7ff) << 16));
	cfg.base_addr_low = cpu_to_be32((u32)phys);
	cfg.w3 = cpu_to_be32((u32)(phys >> 32) & 0xff);
	memcpy_toio(p->xrdp + XRDP_OFF_PSRAM + PSRAM_CPU_TX_RING_DESC_TABLE,
		    &cfg, sizeof(cfg));
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
	 * Real-HW path (ABI bring-up step 7 / 5bis-G3 step 11): block-copy
	 * fw_binary_n (32KB) into RNR_INST[n] and fw_predict_n (1KB) into
	 * RNR_PRED[n] for each of 8 cores, then enable cores via RNR_REGS[n].
	 * Left as a structured TODO: the exact blob sub-layout + per-core
	 * enable handshake are not pinned (ABI sec 6, "Microcode load fine
	 * detail"). Not needed for the emulator.
	 */
	dev_info(p->dev, "Runner microcode %zu bytes loaded (HW path TODO)\n",
		 fw->size);
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
	/* ABI 5bis-G3 step 2: UBUS_SLV address-decode windows. The exact
	 * register offsets within UBUS_SLV are not individually pinned; the
	 * window *values* are (FPM/QM/DQM/VPB/APB). Emulator ignores these,
	 * real HW needs them. Stubbed with the documented values for clarity. */
	/* TODO: pin UBUS_SLV per-window register offsets, then write:
	 *   dev0 0x82A00000..0x82C00000 (FPM), dev1 0x82C00000..0x82C80000 (QM),
	 *   dev2 0x82C80000..0x82D00000 (DQM), vpb 0x82700000..0x82900000,
	 *   apb 0x82900000..0x82A00000. */
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

static void runner_debugfs_init(struct runner_priv *p)
{
	p->dbg = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("offload_selftest", 0200, p->dbg, p,
			    &runner_selftest_fops);
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
	for (i = 0; i < XRDP_RNR_CORES; i++)
		p->rnr_mem[i] = p->xrdp + XRDP_OFF_RNR_MEM0 +
				i * XRDP_RNR_MEM_STRIDE;

	p->rx_irq = platform_get_irq_byname_optional(pdev, "queue0");
	if (p->rx_irq < 0)
		p->rx_irq = 0;	/* poll mode */

	/* bring-up (ABI sec 4) */
	runner_ubus_decode_init(p);
	ret = fpm_pool_init(p);
	if (ret)
		return ret;
	ret = runner_load_microcode(p);
	if (ret)
		return ret;
	ret = rx_ring_alloc(p);
	if (ret)
		return ret;
	ret = tx_ring_alloc(p);
	if (ret)
		return ret;
	rx_ring_publish(p);

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
