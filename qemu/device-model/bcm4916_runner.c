/*
 * BCM4916 / GT-BE98 XRDP "Runner" CPU-conduit model for QEMU (Stage 3 datapath)
 *
 * A QOM sysbus + network device that emulates JUST ENOUGH of the Broadcom
 * BCM4916 (BCM6813) XRDP "Runner" datapath for the OPEN mainline slow-path
 * conduit driver (driver/runner/bcm4916_runner.c) to move real Ethernet frames
 * between Linux and a QEMU netdev backend, with NO proprietary Runner microcode.
 *
 *   TYPE_BCM4916_RUNNER  - DT compatible "brcm,bcm4916-runner", reg = the whole
 *                          rdpa window (base 0x82000000, size 0x00caf004).
 *
 * This is the emulator-facing counterpart of
 *   qemu/device-model/runner-emulation-contract.md
 * and must stay bit-for-bit compatible with the driver's shared constants.
 *
 * WHAT IS MODELLED
 *   - FPM pool: alloc(read 0x400)/free(write 0x400) returning ABI-3.1 tokens;
 *     POOL1_CFG1 (chunk size), POOL1_CFG2 (pool DDR base), FPM_CTL INIT_MEM /
 *     POOL1_ENABLE, POOL1_STAT2 tokens-available.
 *   - Ring discovery: the driver memcpy_toio()'s a 16-B CPU_RING_DESCRIPTOR into
 *     PSRAM at 0x0000 (RX) and 0x0080 (TX); we parse them to learn ring DDR
 *     base + depth + entry size.
 *   - RX inject: a frame from the netdev backend is DMA'd into the FPM/DDR
 *     buffer the descriptor points at, word0/word2 are filled (ownership=HOST
 *     last), and the queue0 IRQ (SPI 75) is raised.
 *   - TX consume: the u16 BE index write into RNR_MEM[n]+0x0 is the doorbell;
 *     on it we read each newly-produced 16-B TX descriptor, resolve the FPM
 *     token in word3 to a guest buffer, emit the frame to the backend, and free
 *     the token.
 *
 * BUILD: copied to hw/net/bcm4916_runner.c on dev-build, added to
 * hw/net/meson.build, wired into hw/arm/virt.c create_bcm4916(). Never build on
 * dev-code.
 *
 * ALL multi-byte descriptor fields are BIG-ENDIAN in guest memory (the Runner is
 * BE). We use ldl_be / stl_be on a byte buffer DMA'd to/from guest RAM.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "system/dma.h"
#include "qemu/bswap.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"

/* ------------------------------------------------------------------ */
/* Shared contract constants (MUST match driver/runner/bcm4916_runner.*) */
/* ------------------------------------------------------------------ */
#define RUNNER_WINDOW_BASE   0x82000000ULL
#define RUNNER_WINDOW_SIZE   0x00caf004ULL

#define XRDP_OFF_PSRAM       0x00000000ULL
#define XRDP_OFF_RNR_MEM0    0x00700000ULL
#define XRDP_RNR_MEM_STRIDE  0x00020000ULL
#define XRDP_RNR_CORES       8
#define XRDP_OFF_FPM         0x00a00000ULL

/* FPM registers (offsets within the FPM block) */
#define FPM_CTL                  0x0000
#define  FPM_CTL_INIT_MEM        (1u << 4)
#define  FPM_CTL_SOFT_RESET      (1u << 14)
#define  FPM_CTL_POOL1_ENABLE    (1u << 16)
#define FPM_POOL1_CFG1           0x0040
#define  FPM_FP_BUF_SIZE_SHIFT   24
#define  FPM_FP_BUF_SIZE_MASK    (0x7u << FPM_FP_BUF_SIZE_SHIFT)
#define FPM_POOL1_CFG2           0x0044
#define  FPM_POOL_BASE_ADDR_MASK 0xfffffffcu
#define FPM_POOL1_STAT2          0x0054
#define FPM_SPARE                0x00c4
#define FPM_POOL0_ALLOC_DEALLOC  0x0400

/* FPM token bitfields (ABI 3.1) */
#define FPM_TOKEN_VALID          (1u << 31)
#define FPM_TOKEN_INDEX_SHIFT    12
#define FPM_TOKEN_INDEX_MASK     (0x1ffffu << FPM_TOKEN_INDEX_SHIFT)
#define FPM_TOKEN_INDEX(t)       (((t) & FPM_TOKEN_INDEX_MASK) >> FPM_TOKEN_INDEX_SHIFT)

/* PSRAM ring-config table offsets (contract placeholders) */
#define PSRAM_CPU_RX_RING_DESC   0x0000
#define PSRAM_CPU_TX_RING_DESC   0x0080
#define CPU_TX_RING_INDICES_OFF  0x0000   /* within each RNR_MEM[n] */

/* RX descriptor word2 (ABI 2.4) */
#define RXD_W2_OWNERSHIP_HOST    0x80000000u
#define RXD_W2_BUF_PTR_MASK      0x7fffffffu

/* defaults */
#define FPM_CHUNK_SIZE_DEFAULT   512
#define RX_BUF_MAX               2048
#define DESC_SIZE                16

/* IRQ: queue0 = SPI 75, fpm = SPI 107 (wired by virt.c create_bcm4916) */
#define RUNNER_IRQ_QUEUE0        0
#define RUNNER_IRQ_FPM           1
#define RUNNER_NUM_IRQ           2

#define TYPE_BCM4916_RUNNER "bcm4916-runner"
OBJECT_DECLARE_SIMPLE_TYPE(Bcm4916RunnerState, BCM4916_RUNNER)

typedef struct {
    bool     valid;        /* ring descriptor parsed and usable */
    uint64_t base;         /* ring DDR phys base */
    uint32_t depth;        /* number_of_entries */
    uint32_t entry_size;   /* bytes per entry (16) */
    uint32_t idx;          /* model's own producer/consumer index */
} RunnerRing;

struct Bcm4916RunnerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq     irq[RUNNER_NUM_IRQ];
    NICState    *nic;
    NICConf      conf;

    /* FPM pool */
    uint32_t fpm_ctl;
    uint32_t fpm_cfg1;
    uint32_t fpm_cfg2;
    uint32_t fpm_spare;
    uint32_t chunk_size;
    uint64_t pool_pbase;
    uint32_t pool_ntokens;     /* number of chunks the model tracks */
    uint8_t *tok_used;         /* per-index in-use bitmap (1 byte each) */
    uint32_t tok_avail;        /* count of free tokens */
    uint32_t alloc_hint;       /* round-robin search start */

    /* rings */
    RunnerRing rx;             /* CPU RX data ring (producer = rx.idx) */
    RunnerRing tx;             /* CPU TX ring (consumer = tx.idx) */

    bool irq_asserted;
    QEMUTimer *rx_timer;   /* polls ring drain to deassert level IRQ */
    uint32_t rx_cons;      /* model view of guest consumer index */

    /* captured ring-cfg bytes (driver memcpy_toio's word-by-word) */
    uint8_t rx_cfg[DESC_SIZE];
    uint8_t tx_cfg[DESC_SIZE];
};

/* DMA helpers operate on the system address space of this device. */
static AddressSpace *runner_dma_as(Bcm4916RunnerState *s)
{
    return &address_space_memory;
}

/* ===================== FPM pool ===================== */

#define FPM_MAX_TOKENS (64u * 1024u)   /* 32MB / 512B */

static void fpm_pool_reset(Bcm4916RunnerState *s)
{
    if (s->tok_used) {
        memset(s->tok_used, 0, FPM_MAX_TOKENS);
    }
    s->pool_ntokens = FPM_MAX_TOKENS;
    s->tok_avail = FPM_MAX_TOKENS;
    s->alloc_hint = 0;
}

static uint32_t fpm_alloc(Bcm4916RunnerState *s)
{
    uint32_t i, idx;

    if (!s->tok_avail || !s->tok_used) {
        return 0;   /* exhausted -> VALID clear, driver back-pressures */
    }
    for (i = 0; i < s->pool_ntokens; i++) {
        idx = (s->alloc_hint + i) % s->pool_ntokens;
        if (!s->tok_used[idx]) {
            s->tok_used[idx] = 1;
            s->tok_avail--;
            s->alloc_hint = (idx + 1) % s->pool_ntokens;
            return FPM_TOKEN_VALID | (idx << FPM_TOKEN_INDEX_SHIFT) |
                   (s->chunk_size & 0xfff);
        }
    }
    return 0;
}

static void fpm_free(Bcm4916RunnerState *s, uint32_t token)
{
    uint32_t idx = FPM_TOKEN_INDEX(token);

    if (!s->tok_used || idx >= s->pool_ntokens) {
        return;
    }
    if (s->tok_used[idx]) {
        s->tok_used[idx] = 0;
        s->tok_avail++;
    }
}

static uint64_t fpm_token_to_phys(Bcm4916RunnerState *s, uint32_t token)
{
    return s->pool_pbase + (uint64_t)FPM_TOKEN_INDEX(token) * s->chunk_size;
}

/* ===================== ring discovery ===================== */

/*
 * Parse a 16-B CPU_RING_DESCRIPTOR (big-endian) the driver published into PSRAM.
 * Layout (ABI 2.1, see contract section 3.1):
 *   w0 [31:27]=size_of_entry(log2 bytes), [26:16]=number_of_entries, [15:0]=irq
 *   w1 [31:16]=drop_counter, [15:0]=write_idx
 *   w2 base_addr_low
 *   w3 [15:0]=read_idx, [7:0]=base_addr_high
 */
static void runner_parse_ring(const uint8_t *d, RunnerRing *r)
{
    uint32_t w0 = ldl_be_p(d + 0);
    uint32_t w2 = ldl_be_p(d + 8);
    uint32_t w3 = ldl_be_p(d + 12);
    uint32_t size_code = (w0 >> 27) & 0x1f;
    uint32_t depth = (w0 >> 16) & 0x7ff;
    uint64_t base = ((uint64_t)(w3 & 0xff) << 32) | w2;

    r->entry_size = 1u << size_code;
    r->depth = depth;
    r->base = base;
    r->idx = 0;
    r->valid = (depth != 0 && base != 0 && r->entry_size == DESC_SIZE);
}

/* ===================== RX inject (backend -> guest) ===================== */

/*
 * Level-IRQ drain check. The contract says: keep queue0 asserted while the
 * model has produced RX slots the guest has not yet consumed, and deassert when
 * caught up. The guest re-arms a consumed slot by clearing the ownership bit
 * (word2 bit31) back to RUNNER. We advance our consumer view over slots that
 * are no longer HOST-owned, and lower the IRQ line(s) when consumer == producer.
 * Re-armed periodically while the line is high so the guest's NAPI drain is
 * eventually observed (there is no MMIO ack on this path).
 */
static void runner_rx_drain_check(void *opaque)
{
    Bcm4916RunnerState *s = opaque;
    AddressSpace *as = runner_dma_as(s);

    if (!s->rx.valid) {
        return;
    }
    while (s->rx_cons != s->rx.idx) {
        uint8_t desc[DESC_SIZE];
        hwaddr pa = s->rx.base + (uint64_t)s->rx_cons * DESC_SIZE;
        uint32_t w2;
        if (dma_memory_read(as, pa, desc, DESC_SIZE,
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            break;
        }
        w2 = ldl_be_p(desc + 8);
        if (w2 & RXD_W2_OWNERSHIP_HOST) {
            break;   /* guest has not consumed this slot yet */
        }
        s->rx_cons = (s->rx_cons + 1) % s->rx.depth;
    }

    if (s->rx_cons == s->rx.idx) {
        /* ring drained -> deassert level */
        if (s->irq_asserted) {
            qemu_set_irq(s->irq[RUNNER_IRQ_QUEUE0], 0);
            qemu_set_irq(s->irq[RUNNER_IRQ_FPM], 0);
            s->irq_asserted = false;
        }
    } else {
        /* still pending -> keep line high and re-check soon */
        timer_mod(s->rx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 200000); /* 200us */
    }
}


static bool runner_can_receive(NetClientState *nc)
{
    Bcm4916RunnerState *s = qemu_get_nic_opaque(nc);
    return s->rx.valid;
}

static ssize_t runner_receive(NetClientState *nc, const uint8_t *buf, size_t len)
{
    Bcm4916RunnerState *s = qemu_get_nic_opaque(nc);
    AddressSpace *as = runner_dma_as(s);
    hwaddr desc_pa;
    uint8_t desc[DESC_SIZE];
    uint32_t w2, w0;
    uint64_t buf_pa;
    uint32_t copylen;

    qemu_log("bcm4916-runner: RX recv len=%zu rx.valid=%d idx=%u\n", len, s->rx.valid, s->rx.idx);
    if (!s->rx.valid) {
        return -1;   /* ring not up yet; tell backend we couldn't take it */
    }
    if (len > RX_BUF_MAX) {
        len = RX_BUF_MAX;   /* truncate jumbo to one chunk for first light */
    }

    desc_pa = s->rx.base + (uint64_t)s->rx.idx * DESC_SIZE;
    if (dma_memory_read(as, desc_pa, desc, DESC_SIZE,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return -1;
    }

    w2 = ldl_be_p(desc + 8);
    if (w2 & RXD_W2_OWNERSHIP_HOST) {
        /* host hasn't re-armed this slot yet -> ring full, drop */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm4916-runner: RX ring full at idx %u, dropping\n",
                      s->rx.idx);
        return len;   /* consumed (dropped) */
    }

    buf_pa = w2 & RXD_W2_BUF_PTR_MASK;
    copylen = len;

    /* 1. DMA the frame into the host-provided buffer */
    if (dma_memory_write(as, buf_pa, buf, copylen,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return -1;
    }

    /* 2. word0 = packet_length | (src_port<<14); src_port 0 (single pipe) */
    w0 = copylen & 0x3fff;
    stl_be_p(desc + 0, w0);

    /* 3. publish: word2 ownership=HOST (bit31), same buffer pointer; LAST. */
    stl_be_p(desc + 8, (uint32_t)(buf_pa & RXD_W2_BUF_PTR_MASK) |
                       RXD_W2_OWNERSHIP_HOST);

    if (dma_memory_write(as, desc_pa, desc, DESC_SIZE,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return -1;
    }

    s->rx.idx = (s->rx.idx + 1) % s->rx.depth;

    /*
     * Raise queue0 (level, hold asserted). The driver's ISR does
     * disable_irq_nosync()+napi_schedule(); NAPI consumes ownership purely from
     * DDR and calls enable_irq() once the ring drains. We deassert the level
     * via a bottom half so the GIC reliably latches the edge first, then
     * lower it after the CPU has had a chance to take the IRQ. Holding it high
     * until the BH runs guarantees the interrupt is observed even though the
     * driver never issues an MMIO ack.
     */
    /*
     * Raise the RX interrupt. We pulse BOTH the queue0 (SPI 75) and fpm
     * (SPI 107) lines: depending on how of_irq resolves the named interrupts,
     * the driver may register either, so asserting both guarantees the one it
     * hooked schedules NAPI. (Harmless: the driver's only IRQ handler is the
     * RX NAPI scheduler.)
     */
    qemu_set_irq(s->irq[RUNNER_IRQ_QUEUE0], 1);
    qemu_set_irq(s->irq[RUNNER_IRQ_FPM], 1);
    s->irq_asserted = true;
    timer_mod(s->rx_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 200000);  /* 200us */

    return len;
}

/* ===================== TX consume (guest -> backend) ===================== */

static void runner_tx_kick(Bcm4916RunnerState *s, uint16_t new_widx)
{
    AddressSpace *as = runner_dma_as(s);

    if (!s->tx.valid) {
        return;
    }
    new_widx %= s->tx.depth;

    while (s->tx.idx != new_widx) {
        uint8_t desc[DESC_SIZE];
        hwaddr desc_pa = s->tx.base + (uint64_t)s->tx.idx * DESC_SIZE;
        uint32_t w0, token;
        uint32_t len;
        uint64_t buf_pa;
        g_autofree uint8_t *frame = NULL;

        if (dma_memory_read(as, desc_pa, desc, DESC_SIZE,
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            break;
        }
        w0 = ldl_be_p(desc + 0);
        token = ldl_be_p(desc + 12);

        /* packet_length is word0 bits [21:8] (ABI 2.2) */
        len = (w0 >> 8) & 0x3fff;
        if (len == 0 || len > RX_BUF_MAX) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm4916-runner: TX idx %u bad len %u\n",
                          s->tx.idx, len);
            s->tx.idx = (s->tx.idx + 1) % s->tx.depth;
            continue;
        }

        buf_pa = fpm_token_to_phys(s, token);
        frame = g_malloc(len);
        if (dma_memory_read(as, buf_pa, frame, len,
                            MEMTXATTRS_UNSPECIFIED) == MEMTX_OK) {
            qemu_log("bcm4916-runner: TX emit idx=%u len=%u token=0x%x buf=0x%" PRIx64 "\n", s->tx.idx, len, token, buf_pa);
            qemu_send_packet(qemu_get_queue(s->nic), frame, len);
        }

        /* free the FPM token back to the pool */
        fpm_free(s, token);

        s->tx.idx = (s->tx.idx + 1) % s->tx.depth;
    }
}

/* ===================== MMIO decode ===================== */

static uint64_t runner_read(void *opaque, hwaddr addr, unsigned size)
{
    Bcm4916RunnerState *s = opaque;

    /* FPM block */
    if (addr >= XRDP_OFF_FPM && addr < XRDP_OFF_FPM + 0x1000) {
        hwaddr off = addr - XRDP_OFF_FPM;
        switch (off) {
        case FPM_CTL:
            /* INIT_MEM completes instantly in the model */
            return s->fpm_ctl & ~FPM_CTL_INIT_MEM;
        case FPM_POOL1_CFG1:
            return s->fpm_cfg1;
        case FPM_POOL1_CFG2:
            return s->fpm_cfg2;
        case FPM_POOL1_STAT2:
            return s->tok_avail & 0x3ffff;
        case FPM_SPARE:
            return s->fpm_spare;
        case FPM_POOL0_ALLOC_DEALLOC:
            return fpm_alloc(s);   /* READ == allocate a token */
        default:
            return 0;
        }
    }

    /* everything else reads 0 (PSRAM/RNR_MEM reads are not used by the driver) */
    return 0;
}

static void runner_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Bcm4916RunnerState *s = opaque;

    /* FPM block */
    if (addr >= XRDP_OFF_FPM && addr < XRDP_OFF_FPM + 0x1000) {
        hwaddr off = addr - XRDP_OFF_FPM;
        switch (off) {
        case FPM_CTL:
            s->fpm_ctl = val;
            if (val & FPM_CTL_SOFT_RESET) {
                fpm_pool_reset(s);
            }
            if (val & FPM_CTL_INIT_MEM) {
                fpm_pool_reset(s);   /* (re)build the free pool */
            }
            return;
        case FPM_POOL1_CFG1:
            s->fpm_cfg1 = val;
            switch ((val & FPM_FP_BUF_SIZE_MASK) >> FPM_FP_BUF_SIZE_SHIFT) {
            case 0: s->chunk_size = 512; break;
            case 1: s->chunk_size = 256; break;
            default: s->chunk_size = 512; break;
            }
            return;
        case FPM_POOL1_CFG2:
            s->fpm_cfg2 = val;
            s->pool_pbase = val & FPM_POOL_BASE_ADDR_MASK;
            return;
        case FPM_SPARE:
            s->fpm_spare = val;
            return;
        case FPM_POOL0_ALLOC_DEALLOC:
            fpm_free(s, val);      /* WRITE == free a token */
            return;
        default:
            return;
        }
    }

    /* PSRAM ring-config publish (memcpy_toio from the driver) */
    if (addr >= XRDP_OFF_PSRAM && addr < XRDP_OFF_PSRAM + 0x10000) {
        hwaddr off = addr - XRDP_OFF_PSRAM;
        /*
         * The driver memcpy_toio's the 16-B ring-cfg word by word (32-bit
         * accesses). Capture the bytes for the RX (0x00..0x0f) and TX
         * (0x80..0x8f) descriptors, then (re)parse on the last word.
         */
        if (off >= PSRAM_CPU_RX_RING_DESC &&
            off < PSRAM_CPU_RX_RING_DESC + DESC_SIZE && size == 4) {
            /*
             * memcpy_toio() writes the BE-in-memory cfg word with a raw (no
             * swap) store, so on this LE guest the value reaching us is the
             * big-endian byte sequence reinterpreted as an integer. Store it
             * little-endian to reconstruct the original BE byte order, which
             * runner_parse_ring() then reads back with ldl_be_p().
             */
            stl_le_p(s->rx_cfg + off, (uint32_t)val);
            if (off == PSRAM_CPU_RX_RING_DESC + DESC_SIZE - 4) {
                runner_parse_ring(s->rx_cfg, &s->rx);
                qemu_log("bcm4916-runner: RX ring base=0x%" PRIx64
                         " depth=%u esz=%u valid=%d\n",
                         s->rx.base, s->rx.depth, s->rx.entry_size, s->rx.valid);
            }
            return;
        }
        if (off >= PSRAM_CPU_TX_RING_DESC &&
            off < PSRAM_CPU_TX_RING_DESC + DESC_SIZE && size == 4) {
            stl_le_p(s->tx_cfg + (off - PSRAM_CPU_TX_RING_DESC), (uint32_t)val);
            if (off == PSRAM_CPU_TX_RING_DESC + DESC_SIZE - 4) {
                runner_parse_ring(s->tx_cfg, &s->tx);
                qemu_log("bcm4916-runner: TX ring base=0x%" PRIx64
                         " depth=%u esz=%u valid=%d\n",
                         s->tx.base, s->tx.depth, s->tx.entry_size, s->tx.valid);
            }
            return;
        }
        return;
    }

    /* RNR_MEM[n] TX index doorbell: u16 BE write at offset 0x0 of each core */
    if (addr >= XRDP_OFF_RNR_MEM0 &&
        addr < XRDP_OFF_RNR_MEM0 + XRDP_RNR_CORES * XRDP_RNR_MEM_STRIDE) {
        hwaddr rel = addr - XRDP_OFF_RNR_MEM0;
        hwaddr in_core = rel % XRDP_RNR_MEM_STRIDE;
        if (in_core == CPU_TX_RING_INDICES_OFF) {
            /*
             * The driver does iowrite16(cpu_to_be16(widx)). On a LE guest the
             * value reaching the model's MMIO handler is therefore the
             * byte-swapped index; undo the swap to recover write_idx.
             */
            uint16_t widx;
            if (size == 2) {
                widx = bswap16((uint16_t)val);
            } else {
                widx = bswap16((uint16_t)(val & 0xffff));
            }
            runner_tx_kick(s, widx);
        }
        return;
    }

    /* all other XRDP writes ignored (BBH/QM/DSPTCHR/... not modelled) */
}

static const MemoryRegionOps runner_ops = {
    .read = runner_read,
    .write = runner_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    /*
     * arm64 memcpy_toio() issues up to 8-byte (STP/STR x) stores. Allow 8-byte
     * accesses at the "valid" layer but keep the impl at 4 bytes so QEMU splits
     * an 8-byte access into two ordered 32-bit handler calls (matching the
     * driver's word-by-word ring-cfg publish semantics).
     */
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* ===================== QOM plumbing ===================== */

static NetClientInfo net_runner_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = runner_can_receive,
    .receive = runner_receive,
};

static void bcm4916_runner_reset(DeviceState *dev)
{
    Bcm4916RunnerState *s = BCM4916_RUNNER(dev);

    s->fpm_ctl = 0;
    s->fpm_cfg1 = 0;
    s->fpm_cfg2 = 0;
    s->fpm_spare = 0;
    s->chunk_size = FPM_CHUNK_SIZE_DEFAULT;
    s->pool_pbase = 0;
    s->irq_asserted = false;
    memset(&s->rx, 0, sizeof(s->rx));
    s->rx_cons = 0;
    memset(&s->tx, 0, sizeof(s->tx));
    memset(s->rx_cfg, 0, sizeof(s->rx_cfg));
    memset(s->tx_cfg, 0, sizeof(s->tx_cfg));
    fpm_pool_reset(s);
    qemu_set_irq(s->irq[RUNNER_IRQ_QUEUE0], 0);
    qemu_set_irq(s->irq[RUNNER_IRQ_FPM], 0);
}

static void bcm4916_runner_realize(DeviceState *dev, Error **errp)
{
    Bcm4916RunnerState *s = BCM4916_RUNNER(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    int i;

    s->tok_used = g_malloc0(FPM_MAX_TOKENS);
    s->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, runner_rx_drain_check, s);
    s->chunk_size = FPM_CHUNK_SIZE_DEFAULT;
    fpm_pool_reset(s);

    memory_region_init_io(&s->iomem, OBJECT(dev), &runner_ops, s,
                          TYPE_BCM4916_RUNNER, RUNNER_WINDOW_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    for (i = 0; i < RUNNER_NUM_IRQ; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_runner_info, &s->conf,
                          object_get_typename(OBJECT(dev)),
                          dev->id, &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static const VMStateDescription vmstate_bcm4916_runner = {
    .name = TYPE_BCM4916_RUNNER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(fpm_ctl, Bcm4916RunnerState),
        VMSTATE_UINT32(fpm_cfg1, Bcm4916RunnerState),
        VMSTATE_UINT32(fpm_cfg2, Bcm4916RunnerState),
        VMSTATE_UINT32(chunk_size, Bcm4916RunnerState),
        VMSTATE_UINT64(pool_pbase, Bcm4916RunnerState),
        VMSTATE_END_OF_LIST()
    },
};

static Property bcm4916_runner_properties[] = {
    DEFINE_NIC_PROPERTIES(Bcm4916RunnerState, conf),
};

static void bcm4916_runner_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "BCM4916 XRDP Runner CPU-conduit (slow-path datapath)";
    dc->realize = bcm4916_runner_realize;
    dc->vmsd = &vmstate_bcm4916_runner;
    device_class_set_legacy_reset(dc, bcm4916_runner_reset);
    device_class_set_props(dc, bcm4916_runner_properties);
}

static const TypeInfo bcm4916_runner_types[] = {
    {
        .name          = TYPE_BCM4916_RUNNER,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Bcm4916RunnerState),
        .class_init    = bcm4916_runner_class_init,
    },
};

DEFINE_TYPES(bcm4916_runner_types)
