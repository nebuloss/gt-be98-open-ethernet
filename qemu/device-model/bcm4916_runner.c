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
 *   - Ring discovery: the driver memcpy_toio()'s 16-B CPU_RING_DESCRIPTORs into
 *     per-core RNR data SRAM (NOT PSRAM): RX delivery ring @ core3 +0x3000, TX
 *     ring @ core2 +0x33e0, FEED ring @ core3 +0x0f70; we parse them to learn
 *     each ring's DDR base + depth + entry size.
 *   - RX inject (FEED-ring model): the driver posts empty DDR buffers into the
 *     FEED ring (8-B CPU_FEED_DESCRIPTOR, 40-bit ABS ptr) and rings the feed
 *     doorbell (write_idx@+6). On a backend frame we take the next unconsumed
 *     feed buffer, DMA the frame into it, write a 16-B CPU_RX_DESCRIPTOR into the
 *     delivery ring, advance the delivery write_idx (@+6), advance the feed
 *     read_idx (@+12), and raise the queue0 IRQ (SPI 75).
 *   - TX consume: the u16 BE write_idx write into the TX indices entry
 *     (core2 +0x29c8 +2) is the doorbell; on it we read each newly-produced 16-B
 *     TX descriptor, resolve the FPM token in word3 to a guest buffer, emit the
 *     frame to the backend, free the token, and advance the runner read_idx
 *     (mirrored back into the indices entry @+0).
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
#define XRDP_OFF_RNR_REGS0   0x00800000ULL   /* per-core ctl regs, stride 0x1000 */
#define XRDP_RNR_REGS_STRIDE 0x00001000ULL
#define XRDP_OFF_FPM         0x00a00000ULL
#define XRDP_OFF_NATC        0x00950000ULL   /* NAT-C engine (ABI 1.1) */

/* CPU host rings live on specific Runner cores (matches driver header). */
#define CPU_RX_RING_CORE     3
#define CPU_TX_RING_CORE     2

/* RDD table offsets WITHIN a core's RNR_MEM data SRAM (match driver). */
#define RDD_RX_DELIV_RING_OFF   0x3000   /* core3: RX delivery CPU_RING_DESCRIPTOR */
#define RDD_FEED_RING_OFF       0x0f70   /* core3: FEED ring CPU_RING_DESCRIPTOR  */
#define RDD_TX_RING_OFF         0x33e0   /* core2: TX ring CPU_RING_DESCRIPTOR    */
#define CPU_TX_RING_INDICES_OFF 0x29c8   /* core2: {read_idx@+0, write_idx@+2}    */

/* field offsets within a 16-B CPU_RING_DESCRIPTOR control block (big-endian) */
#define RING_CFG_WRITE_IDX_OFF  6        /* runner advances on RX delivery */
#define RING_CFG_READ_IDX_OFF   12       /* host advances as it consumes   */

/* RNR_REGS per-core: the TX thread is edge-woken per frame by CFG_CPU_WAKEUP. */
#define RNR_CFG_CPU_WAKEUP      0x04

/* FEED descriptor (8-B CPU_FEED_DESCRIPTOR, big-endian; matches driver header):
 *   ptr_low@+0 = low32 of a 40-bit DMA phys; byte+6 bit0 = ABS/type; byte+7 =
 *   ptr_hi[39:32]. */
#define FEED_DESC_SIZE          8
#define FEED_W1_ABS             (1u << 8)     /* host word1 bit8 == byte+6 bit0 */
#define FEED_W1_PTR_HI_SHIFT    0             /* host word1 [7:0] == byte+7     */
#define FEED_W1_PTR_HI_MASK     0xff

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

/* -------- NAT-C offload contract (matches driver/runner/flow_offload.h) ---- */
#define PSRAM_NATC_STAGE_KEY     0x0100   /* 16-byte masked BE key */
#define PSRAM_NATC_STAGE_CTX     0x0120   /* FC_UCAST_FLOW_CONTEXT_ENTRY */
#define PSRAM_NATC_INDIR_INDEX   0x0200   /* table index (u32) */
#define PSRAM_NATC_INDIR_CMD     0x0204   /* command reg (u32): 3=add 4=del */
#define  NATC_CMD_ADD            3
#define  NATC_CMD_DEL            4

/* context-entry byte offsets (matches flow_offload.h CTX_OFF_*).
 * cmdlist offset corrected to 24 from the live capture (real silicon: GPL
 * FC_UCAST_FLOW_CONTEXT_ENTRY_STRUCT has command_list @ struct byte 24;
 * re-notes/stock-watch-capture.md sec 2). XPE_CMDLIST_MAX = 80. */
#define CTX_OFF_FLAGS            8
#define  CTX_FLAG_IS_L2_ACCEL    (1u << 4)
#define CTX_OFF_VPORT            12
#define CTX_OFF_CMDLIST_OFF      24
#define XPE_CMDLIST_MAX_M        80
#define CTX_OFF_CMDLIST_DLEN     (CTX_OFF_CMDLIST_OFF + XPE_CMDLIST_MAX_M + 0) /* 104 */
#define CTX_OFF_CMDLIST_LEN      (CTX_OFF_CMDLIST_OFF + XPE_CMDLIST_MAX_M + 1) /* 105 */
#define CTX_OFF_VALID            (CTX_OFF_CMDLIST_OFF + XPE_CMDLIST_MAX_M + 2) /* 106 */
#define CTX_ENTRY_MAX            124

/*
 * XPE opcodes. opcode = byte0 >> 2 (= cmd_word >> 26). These are PINNED
 * byte-for-byte from the driver's emitter (driver/runner/cmdlist.c, commit that
 * rewrote the encoding to the 6813 silicon layout). The per-op byte0 the driver
 * emits, and the opcode it decodes to, are:
 *
 *   replace_bits_16   byte0 0x50 -> op 0x14  (VLAN VID/PCP edit, ToS mangle)
 *   move_packet       byte0 0x4c -> op 0x13  (VLAN push/pop primitive)
 *   replace_16/_32    byte0 0x60 -> op 0x18  (full-field replace: NAT addr/port,
 *                                             4-byte VLAN tag write)
 *   decrement_8       byte0 0x6a -> op 0x1a  (= ADD(-1) on TTL, byte3 0xff)
 *   apply_icsum_16    byte0 0x70 -> op 0x1c  (IP/L4 incremental checksum fixup)
 *
 * (Was the OLD uniform packing: opcode<<26 | offset8<<18 | position<<13 |
 *  width<<8 | nbytes, with MCOPY/MOVE/ICSUM at 0x13/0x2c/0x36. The driver no
 *  longer emits that; the per-op byte layout below is the contract.)
 */
#define XPE_OP_REPLACE_BITS      0x14   /* replace_bits_16, byte0 0x50 */
#define XPE_OP_MOVE_PACKET       0x13   /* move_packet (VLAN push/pop), byte0 0x4c */
#define XPE_OP_REPLACE           0x18   /* replace_16/_32, byte0 0x60 */
#define XPE_OP_ADD               0x1a   /* decrement_8 = ADD(-1) on TTL, byte0 0x6a */
#define XPE_OP_ICSUM             0x1c   /* apply_icsum_16 (IP / L4 csum), byte0 0x70 */
/*
 * 0x3f is the opcode-switch default, NOT a terminator: the stock list is
 * length-delimited by cmd_list_data_length (xpe_api.o xpe_cmd_end emits no NOP
 * word and pads the trailing slot slack with the byte 0xfc, which lies past the
 * data-length and is never decoded). Kept only as a defensive early-out.
 */
#define XPE_OP_NOP               0x3f
#define XPE_PAD_BYTE             0xfc   /* stock slot pad fill (outside dlen) */

/* context flag bits (matches flow_offload.h CTX_FLAG_*) */
#define CTX_FLAG_IS_ROUTED       (1u << 5)
#define CTX_FLAG_IS_NAT          (1u << 3)

/* IPv4 packet byte offsets for an untagged frame (matches flow_offload.h) */
#define L2_HLEN_M                14
#define IP4_OFF_TTL_M            (L2_HLEN_M + 8)    /* 22 */
#define IP4_OFF_CSUM_M           (L2_HLEN_M + 10)   /* 24 */
#define IP4_OFF_SADDR_M          (L2_HLEN_M + 12)   /* 26 */
#define IP4_OFF_DADDR_M          (L2_HLEN_M + 16)   /* 30 */

#define NATC_MAX_ENTRIES         64
#define VLAN_HLEN_M              4

/* RX descriptor (BCM6813/XRDP CPU_RX_DESCRIPTOR, abs-address mode, host LE view).
 * word0 = buf phys low32; word1 = abs(b16)|packet_length[15:2]|ptr_hi[31:24];
 * word2 = is_src_lan(b31)|src_port/vport[29:25]; word3 = flags.
 * A filled slot has packet_length != 0; an empty (runner-owned) slot has it 0. */
#define RXD_W1_ABS               (1u << 16)
#define RXD_W1_PKT_LEN_SHIFT     2
#define RXD_W1_PKT_LEN_MASK      0x3fffu
#define RXD_W1_PTR_HI_SHIFT      24
#define RXD_W2_IS_SRC_LAN        0x80000000u
#define RXD_W2_SRC_PORT_SHIFT    25

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
    RunnerRing rx;             /* CPU RX *delivery* ring (runner-written, 16B) */
    RunnerRing tx;             /* CPU TX ring (consumer = tx.idx)             */
    RunnerRing feed;           /* host-posted FEED ring (8B empty-buf ptrs)   */
    uint32_t feed_rcons;       /* runner's feed read_idx (buffers consumed)   */

    bool irq_asserted;
    QEMUTimer *rx_timer;   /* polls ring drain to deassert level IRQ */

    /* captured ring-cfg bytes (driver memcpy_toio's word-by-word) */
    uint8_t rx_cfg[DESC_SIZE];
    uint8_t tx_cfg[DESC_SIZE];
    uint8_t feed_cfg[DESC_SIZE];
    /* TX indices entry {read_idx@+0, write_idx@+2} (BE u16 each), core2 SRAM.
     * The runner advances read_idx@+0 as it consumes; the driver reads it back
     * (ringstat) to confirm consumption. */
    uint8_t tx_indices[4];

    /* ---- NAT-C connection table (HW flow-offload model) ---- */
    uint8_t  natc_stage_key[16];          /* staged 16-byte BE key */
    uint8_t  natc_stage_ctx[CTX_ENTRY_MAX];
    uint32_t natc_stage_idx;              /* staged table index */
    struct {
        bool     valid;
        uint8_t  key[16];                 /* masked BE key */
        uint8_t  ctx[CTX_ENTRY_MAX];      /* FC_UCAST_FLOW_CONTEXT_ENTRY */
        uint32_t ctx_len;
    } natc[NATC_MAX_ENTRIES];
    uint64_t off_hits;                    /* frames forwarded in HW (bypassed CPU) */
    uint64_t off_misses;                  /* frames delivered to CPU (slow path) */
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
 * ★ ON-SILICON CONTRACT (resynced 2026-06-22) — matches driver/runner/ {c,h} ★
 * The ring contract was resynced from the live BCM6813 silicon driver:
 *   1. RINGS live in per-core RNR_MEM data SRAM (NOT PSRAM): RX delivery @ core3
 *      +0x3000, TX @ core2 +0x33e0, FEED ring @ core3 +0x0f70, TX indices entry
 *      @ core2 +0x29c8. The model watches those window offsets (see runner_write).
 *   2. number_of_entries field is in RESOLUTION units: depth = field<<5 for RX/TX
 *      (32-res), field<<6 for FEED (64-res). size_of_entry is in BYTES.
 *   3. FEED RING: RX buffers come from a host-posted feed ring (8-B 40-bit ABS
 *      ptrs) + a feed doorbell (write_idx@+6); the model consumes the feed ring
 *      to find a buffer to DMA each RX frame into (runner_receive).
 *   4. TX doorbell: the BE u16 write_idx at +2 of the indices entry (core2
 *      +0x29c8) is the kick; the per-frame CFG_CPU_WAKEUP write (RNR_REGS core2
 *      +0x04) is accepted and ignored (the index write already triggers consume).
 *   5. Microcode is loaded BE but the emulator IS the runner, so it is ignored.
 *
 * Parse a 16-B CPU_RING_DESCRIPTOR (big-endian). Layout:
 *   w0 [31:27]=size_of_entry(BYTES), [26:16]=number_of_entries(>>res), [15:0]=irq
 *   w1 [31:16]=drop_counter, [15:0]=write_idx ; w2 base_low ; w3 [15:0]=read_idx,
 *   [7:0]=base_high.
 */
#define RING_RES_32  5   /* RX delivery + TX rings */
#define RING_RES_64  6   /* FEED ring */
static void runner_parse_ring(const uint8_t *d, RunnerRing *r, uint32_t res_shift)
{
    uint32_t w0 = ldl_be_p(d + 0);
    uint32_t w2 = ldl_be_p(d + 8);
    uint32_t w3 = ldl_be_p(d + 12);
    uint32_t entry_sz = (w0 >> 27) & 0x1f;            /* BYTES, not log2 */
    uint32_t depth = ((w0 >> 16) & 0x7ff) << res_shift;
    uint64_t base = ((uint64_t)(w3 & 0xff) << 32) | w2;

    r->entry_size = entry_sz;
    r->depth = depth;
    r->base = base;
    r->idx = 0;
    r->valid = (depth != 0 && base != 0 && entry_sz != 0);
}

/* ===================== NAT-C connection table (offload) ================== */

/*
 * Add the staged {key, ctx} into a NAT-C slot. Mirrors the driver's
 * xrdp_natc_add (drv_natc_key_result_entry_var_size_ctx_add, ABI sec 1.1):
 * the driver staged the key + context into PSRAM, wrote the index, and issued
 * the add command; we copy the staging buffers into the indexed slot.
 */
static void natc_add(Bcm4916RunnerState *s)
{
    uint32_t idx = s->natc_stage_idx % NATC_MAX_ENTRIES;

    memcpy(s->natc[idx].key, s->natc_stage_key, 16);
    memcpy(s->natc[idx].ctx, s->natc_stage_ctx, CTX_ENTRY_MAX);
    s->natc[idx].ctx_len = CTX_ENTRY_MAX;
    s->natc[idx].valid = true;
    qemu_log("bcm4916-runner: NAT-C ADD idx=%u key=%02x%02x%02x%02x%02x%02x%02x%02x"
             "%02x%02x%02x%02x%02x%02x%02x%02x is_l2_accel=%d cmdlist_dlen=%u\n",
             idx,
             s->natc_stage_key[0], s->natc_stage_key[1], s->natc_stage_key[2],
             s->natc_stage_key[3], s->natc_stage_key[4], s->natc_stage_key[5],
             s->natc_stage_key[6], s->natc_stage_key[7], s->natc_stage_key[8],
             s->natc_stage_key[9], s->natc_stage_key[10], s->natc_stage_key[11],
             s->natc_stage_key[12], s->natc_stage_key[13], s->natc_stage_key[14],
             s->natc_stage_key[15],
             !!(s->natc_stage_ctx[CTX_OFF_FLAGS] & CTX_FLAG_IS_L2_ACCEL),
             s->natc_stage_ctx[CTX_OFF_CMDLIST_DLEN]);
    {
        /* dump the embedded cmdlist bytes (16-bit BE words) for validation */
        uint8_t dl = s->natc_stage_ctx[CTX_OFF_CMDLIST_DLEN];
        char hex[3 * 64 + 1];
        int i, n = 0;
        if (dl > 60) {
            dl = 60;
        }
        for (i = 0; i < dl; i++) {
            n += snprintf(hex + n, sizeof(hex) - n, "%02x ",
                          s->natc_stage_ctx[CTX_OFF_CMDLIST_OFF + i]);
        }
        qemu_log("bcm4916-runner: NAT-C ADD idx=%u cmdlist=[ %s]\n",
                 idx, hex);
    }
}

static void natc_del(Bcm4916RunnerState *s)
{
    uint32_t idx = s->natc_stage_idx % NATC_MAX_ENTRIES;

    s->natc[idx].valid = false;
    qemu_log("bcm4916-runner: NAT-C DEL idx=%u\n", idx);
}

/*
 * Compute the 16-byte masked BE key from a received frame, EXACTLY as the
 * driver's xrdp_build_key does (flow_offload.c), so a model lookup matches a
 * driver-programmed entry:
 *   w0 = DA[0..3]; w1 = DA[4..5]|SA[0..1]; w2 = SA[2..5];
 *   w3 = ethertype<<16 | ingress_vport<<4 | (vlan_in?1:0)
 * ingress_vport = 0 (single pipe, matches the driver's default_vport).
 * For an L2 key the ethertype used is the *inner* frame ethertype; on an
 * untagged frame that is bytes [12:13].
 */
static void natc_compute_key(const uint8_t *frame, size_t len, uint8_t key[16])
{
    uint16_t ethertype = 0;
    uint16_t vlan_in = 0;

    if (len >= 14) {
        ethertype = (frame[12] << 8) | frame[13];
    }
    if (ethertype == 0x8100 && len >= 18) {
        /* tagged: record inner ethertype + vid for the key */
        vlan_in = ((frame[14] << 8) | frame[15]) & 0xfff;
        ethertype = (frame[16] << 8) | frame[17];
    }

    /* w0 = DA[0..3] (big-endian byte order in the key buffer) */
    key[0] = frame[0]; key[1] = frame[1]; key[2] = frame[2]; key[3] = frame[3];
    /* w1 = DA[4..5] | SA[0..1] */
    key[4] = frame[4]; key[5] = frame[5]; key[6] = frame[6]; key[7] = frame[7];
    /* w2 = SA[2..5] */
    key[8] = frame[8]; key[9] = frame[9]; key[10] = frame[10]; key[11] = frame[11];
    /* w3 = ethertype<<16 | vport<<4 | vlan_present.  vport=0. */
    key[12] = (ethertype >> 8) & 0xff;
    key[13] = ethertype & 0xff;
    key[14] = 0x00;                       /* (vport[11:4]) high */
    key[15] = (vlan_in ? 1 : 0) & 0xff;   /* (vport[3:0]<<4)|present; vport 0 */
}

/*
 * Compute the 16-byte L3 IPv4 5-tuple key from a received frame, EXACTLY as the
 * driver's xrdp_build_key does for an is_routed flow (flow_offload.c). The w[3]
 * byte layout was PINNED from live silicon (re-notes/stock-watch-capture.md
 * sec 1): w[3] = ToS<<24 | 0x28<<16 | flags<<8 | 0x68, where ToS is the IP ToS
 * byte and flags bit7=direction(us), bit6=tcp_pure_ack.
 *   w0 = ORIGINAL src IP; w1 = ORIGINAL dst IP; w2 = sport<<16 | dport;
 *   key[12]=ToS, key[13]=0x28 (proto/key-class), key[14]=dir|ack, key[15]=0x68.
 * The key is the ORIGINAL (ingress) tuple - the lookup runs before the cmdlist
 * rewrites the packet. Returns false if the frame is not parseable IPv4 TCP/UDP.
 * Assumes an untagged IPv4 frame (ethertype 0x0800 at [12:13], IHL=5).
 */
#define NATC_L3_KEY_CLASS_BYTE_M  0x28
#define NATC_L3_KEY_TRAILER_M     0x68
#define NATC_L3_KEY_DIR_US_M      (1u << 7)
static bool natc_compute_l3_key(const uint8_t *frame, size_t len, uint8_t key[16])
{
    uint16_t ethertype;
    uint8_t ihl, proto;
    const uint8_t *ip, *l4;

    if (len < 14 + 20) {
        return false;
    }
    ethertype = (frame[12] << 8) | frame[13];
    if (ethertype != 0x0800) {
        return false;
    }
    ip = frame + L2_HLEN_M;
    if ((ip[0] >> 4) != 4) {
        return false;
    }
    ihl = (ip[0] & 0x0f) * 4;
    proto = ip[9];
    if (proto != 6 && proto != 17) {     /* TCP or UDP */
        return false;
    }
    if (len < (size_t)(L2_HLEN_M + ihl + 4)) {
        return false;
    }
    l4 = ip + ihl;

    /* w0 = src IP (ip[12..15]) */
    key[0] = ip[12]; key[1] = ip[13]; key[2] = ip[14]; key[3] = ip[15];
    /* w1 = dst IP (ip[16..19]) */
    key[4] = ip[16]; key[5] = ip[17]; key[6] = ip[18]; key[7] = ip[19];
    /* w2 = sport<<16 | dport (l4[0..3]) */
    key[8] = l4[0]; key[9] = l4[1]; key[10] = l4[2]; key[11] = l4[3];
    /* w3 = ToS<<24 | key-class<<16 | flags<<8 | trailer (live-pinned layout).
     * ToS = IP ToS byte (ip[1]); selftest has tcp_pure_ack=0, dir=us. */
    key[12] = ip[1];                      /* ToS/DSCP */
    key[13] = NATC_L3_KEY_CLASS_BYTE_M;   /* proto/key-class (0x28) */
    key[14] = NATC_L3_KEY_DIR_US_M;       /* dir=us, tcp_pure_ack=0 */
    key[15] = NATC_L3_KEY_TRAILER_M;      /* ingress-vport/valid trailer (0x68) */
    (void)proto;
    return true;
}

static int natc_lookup(Bcm4916RunnerState *s, const uint8_t key[16])
{
    int i;
    for (i = 0; i < NATC_MAX_ENTRIES; i++) {
        if (s->natc[i].valid && memcmp(s->natc[i].key, key, 16) == 0) {
            return i;
        }
    }
    return -1;
}

/* ones-complement 16-bit checksum over a byte range (for ICSUM modelling) */
static uint16_t ones_complement_csum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    size_t i;
    for (i = 0; i + 1 < n; i += 2) {
        sum += (p[i] << 8) | p[i + 1];
    }
    if (i < n) {
        sum += p[i] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return ~sum & 0xffff;
}

/*
 * Recompute the IPv4 header checksum in place. csum_off is the byte offset of
 * the 16-bit IP checksum field (IP4_OFF_CSUM_M for an untagged frame).
 */
static void fixup_ip_csum(uint8_t *frame, size_t len, uint8_t csum_off)
{
    uint8_t ip_off = csum_off - 10;     /* start of the IP header */
    uint8_t ihl;
    uint16_t c;

    if ((size_t)(ip_off + 20) > len) {
        return;
    }
    ihl = (frame[ip_off] & 0x0f) * 4;
    if ((size_t)(ip_off + ihl) > len) {
        return;
    }
    frame[csum_off] = 0;
    frame[csum_off + 1] = 0;
    c = ones_complement_csum(frame + ip_off, ihl);
    frame[csum_off] = (c >> 8) & 0xff;
    frame[csum_off + 1] = c & 0xff;
}

/*
 * Recompute the TCP/UDP checksum in place over the pseudo-header + L4 segment.
 * csum_off is the byte offset of the 16-bit L4 checksum field. The model assumes
 * an untagged IPv4 frame with IHL=5 (the Phase-2 fast-path shape).
 */
static void fixup_l4_csum(uint8_t *frame, size_t len, uint8_t csum_off)
{
    uint8_t ip_off = L2_HLEN_M;
    uint8_t ihl = (frame[ip_off] & 0x0f) * 4;
    uint8_t proto = frame[ip_off + 9];
    uint8_t l4_off = ip_off + ihl;
    uint16_t l4_len;
    uint32_t sum = 0;
    uint16_t c;
    size_t i;

    if ((size_t)(l4_off + 4) > len) {
        return;
    }
    l4_len = len - l4_off;

    /* zero the checksum field before summing */
    if ((size_t)(csum_off + 2) > len) {
        return;
    }
    frame[csum_off] = 0;
    frame[csum_off + 1] = 0;

    /* pseudo-header: src IP, dst IP, zero, proto, L4 length */
    for (i = 0; i < 4; i += 2) {
        sum += (frame[ip_off + 12 + i] << 8) | frame[ip_off + 12 + i + 1];
    }
    for (i = 0; i < 4; i += 2) {
        sum += (frame[ip_off + 16 + i] << 8) | frame[ip_off + 16 + i + 1];
    }
    sum += proto;
    sum += l4_len;

    /* L4 header + payload */
    for (i = 0; i + 1 < l4_len; i += 2) {
        sum += (frame[l4_off + i] << 8) | frame[l4_off + i + 1];
    }
    if (i < l4_len) {
        sum += frame[l4_off + i] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    c = ~sum & 0xffff;
    /* UDP: a computed checksum of 0 is transmitted as 0xffff */
    if (proto == 17 && c == 0) {
        c = 0xffff;
    }
    frame[csum_off] = (c >> 8) & 0xff;
    frame[csum_off + 1] = c & 0xff;
}

/*
 * Execute the embedded XPE cmdlist on a frame in-place, returning the new
 * length.
 *
 * NEW ENCODING (the byte-exact 6813 silicon layout — contract source is
 * driver/runner/cmdlist.c). Every command word is a 32-bit BIG-ENDIAN word
 * (emitted as two 16-bit BE half-words). The model reads 4 bytes at a time:
 *
 *   byte0 = (opcode << 2) | sub-flags        -> opcode = byte0 >> 2
 *   byte1 = "to" word index = (offset>>1)+1  -> offset = (byte1-1)*2  [offset ops]
 *   byte2/byte3 are op-specific (see each case).
 *
 * Per-op layout and inline-data consumed AFTER the command word:
 *   REPLACE_32 (0x60, op 0x18): b2=0x94(.data ref) b3=4. +4 inline bytes = the
 *     32-bit immediate (hi half then lo half, BE). Overwrite 4 bytes @offset.
 *   REPLACE_16 (0x60, op 0x18): b2=0x94 b3=2. +2 inline bytes = 16-bit immediate
 *     (BE). Overwrite 2 bytes @offset.  (disambiguated from _32 by b3 == nbytes)
 *   REPLACE_BITS_16 (0x50, op 0x14): b2=0x94 b3=(position&0xf)|(((width-1)+
 *     position)&0xf)<<4. +2 inline bytes = operand already pre-shifted to
 *     data16<<position. Replace 'width' bits at 'position' in the 16-bit word
 *     @offset (so position/width are recovered from b3, and the inline operand
 *     is masked back with the position/width window).
 *   MOVE_PACKET (0x4c, op 0x13): b1=from (RAW byte off), b2=to (RAW byte off),
 *     b3[6:0]=nbytes. NO inline data. insert = move(off -> off+n) opens a hole;
 *     delete = move(off+n -> off) closes one. We detect push vs pop by to>from.
 *   DECREMENT_8 (0x6a, op 0x1a): b1=b2=(offset>>1)+1, b3=0xff. NO inline data.
 *     Decrement the byte @offset by 1 (TTL).
 *   APPLY_ICSUM_16 (0x70, op 0x1c): b1=(offset>>1)+1, low half (b2/b3) = a 16-bit
 *     immediate INLINE in the command word (0 here). NO separate data word. We
 *     recompute the IP or L4 checksum at 'offset' (real HW applies the delta).
 *
 * FRAMING (matches the RE'd stock emitter, xpe_api.o xpe_cmd_end): the program
 * is LENGTH-DELIMITED by cmd_list_data_length (dlen) - there is NO NOP
 * terminator word. We walk command words until pos reaches dlen. The stock 0xfc
 * slot-pad bytes lie past dlen and are never reached; a 0xfc byte where a
 * command word is expected is also treated as the end (defensive early-out).
 */
static size_t natc_run_cmdlist(const uint8_t *ctx, uint8_t *frame, size_t len)
{
    uint8_t dlen = ctx[CTX_OFF_CMDLIST_DLEN];
    const uint8_t *cl = ctx + CTX_OFF_CMDLIST_OFF;
    int pos = 0;

    while (pos + 4 <= dlen) {
        uint8_t b0 = cl[pos];
        uint8_t b1 = cl[pos + 1];
        uint8_t b2 = cl[pos + 2];
        uint8_t b3 = cl[pos + 3];
        uint8_t opcode = b0 >> 2;
        /* offset-based ops: byte1 = (offset>>1)+1 -> offset = (byte1-1)*2 */
        uint8_t offset = (uint8_t)((b1 - 1) * 2);
        pos += 4;

        if (b0 == XPE_PAD_BYTE) {
            break;   /* hit the 0xfc slot pad (defensive; normally past dlen) */
        }

        switch (opcode) {
        case XPE_OP_MOVE_PACKET: {
            /* byte1=from (RAW), byte2=to (RAW), byte3[6:0]=nbytes; no inline. */
            uint8_t from = b1;
            uint8_t to = b2;
            uint8_t n = b3 & 0x7f;
            if (to > from) {
                /* insert: open a hole of n bytes at 'from' (VLAN push) */
                if (from <= len && len + n <= RX_BUF_MAX) {
                    memmove(frame + from + n, frame + from, len - from);
                    memset(frame + from, 0, n);
                    len += n;
                }
            } else if (from > to) {
                /* delete: close the hole, move [from..] back to 'to' (VLAN pop).
                 * removes (from - to) bytes; for delete_16 that equals 'n'. */
                if (from <= len) {
                    memmove(frame + to, frame + from, len - from);
                    len -= (from - to);
                }
            }
            break;
        }
        case XPE_OP_ADD:    /* decrement_8: byte3=0xff, no inline (TTL -1) */
            if (offset < len) {
                frame[offset] = (uint8_t)(frame[offset] - 1);
            }
            break;
        case XPE_OP_ICSUM:  /* apply_icsum_16: 16-bit imm inline in word, no data */
            if (offset == IP4_OFF_CSUM_M) {
                fixup_ip_csum(frame, len, offset);
            } else {
                fixup_l4_csum(frame, len, offset);
            }
            break;
        case XPE_OP_REPLACE:    /* byte0 0x60: full-field replace, b3 = nbytes */
            if (b3 == 4) {              /* REPLACE-32: IP SA/DA NAT, +4 inline */
                uint32_t d32 = 0;
                if (pos + 4 <= dlen) {
                    d32 = (cl[pos] << 24) | (cl[pos + 1] << 16) |
                          (cl[pos + 2] << 8) | cl[pos + 3];
                    pos += 4;
                }
                if ((size_t)(offset + 4) <= len) {
                    frame[offset]     = (d32 >> 24) & 0xff;
                    frame[offset + 1] = (d32 >> 16) & 0xff;
                    frame[offset + 2] = (d32 >> 8) & 0xff;
                    frame[offset + 3] = d32 & 0xff;
                }
            } else {                    /* REPLACE-16: L4 port NAPT, +2 inline */
                uint16_t d16 = 0;
                if (pos + 2 <= dlen) {
                    d16 = (cl[pos] << 8) | cl[pos + 1];
                    pos += 2;
                }
                if ((size_t)(offset + 2) <= len) {
                    frame[offset]     = (d16 >> 8) & 0xff;
                    frame[offset + 1] = d16 & 0xff;
                }
            }
            break;
        case XPE_OP_REPLACE_BITS: { /* byte0 0x50: bit-field replace, +2 inline */
            /* b3 = (position & 0xf) | (((width-1)+position) & 0xf) << 4.
             * Recover position and width; the inline operand is data16<<position
             * (already pre-shifted by the emitter), so mask it to the window. */
            uint8_t position = b3 & 0x0f;
            uint8_t hinib = (b3 >> 4) & 0x0f;     /* = (width-1)+position */
            uint8_t width = (uint8_t)(hinib - position + 1);
            uint16_t operand = 0;
            if (pos + 2 <= dlen) {
                operand = (cl[pos] << 8) | cl[pos + 1];
                pos += 2;
            }
            if (width >= 16 && (size_t)(offset + 2) <= len) {
                frame[offset] = (operand >> 8) & 0xff;
                frame[offset + 1] = operand & 0xff;
            } else if (width > 0 && (size_t)(offset + 2) <= len) {
                uint16_t cur = (frame[offset] << 8) | frame[offset + 1];
                uint16_t mask = (uint16_t)(((1u << width) - 1) << position);
                cur = (cur & ~mask) | (operand & mask);
                frame[offset] = (cur >> 8) & 0xff;
                frame[offset + 1] = cur & 0xff;
            }
            break;
        }
        case XPE_OP_NOP:
        default:
            return len;  /* defensive terminator (length-delimited normally) */
        }
    }
    return len;
}

/* ===================== RX inject (backend -> guest) ===================== */

/*
 * The runner-advanced indices live in the per-core RNR_MEM cfg blocks the driver
 * reads back: the RX delivery ring's write_idx @+6 of its cfg, and the FEED
 * ring's read_idx @+12 of its cfg. The driver polls these (BE u16). We keep them
 * in our captured cfg byte buffers and serve them from runner_read().
 */
static void rx_set_deliv_write_idx(Bcm4916RunnerState *s, uint16_t widx)
{
    stw_be_p(s->rx_cfg + RING_CFG_WRITE_IDX_OFF, widx);
}
static void rx_set_feed_read_idx(Bcm4916RunnerState *s, uint16_t ridx)
{
    stw_be_p(s->feed_cfg + RING_CFG_READ_IDX_OFF, ridx);
}

/* feed write_idx (host-advanced) is captured live in feed_cfg @+6 on every feed
 * doorbell publish; read it back here. */
static uint16_t feed_write_idx(Bcm4916RunnerState *s)
{
    return lduw_be_p(s->feed_cfg + RING_CFG_WRITE_IDX_OFF);
}

/*
 * Level-IRQ drain check. Keep queue0 asserted while the runner has delivered RX
 * slots the guest has not yet consumed (delivery write_idx != host read_idx),
 * deassert when caught up. The host read_idx is published into the delivery cfg
 * @+12; the driver writes it via the RNR_MEM window, which we capture live in
 * rx_cfg, so we just compare our write_idx against the captured read_idx.
 */
static void runner_rx_drain_check(void *opaque)
{
    Bcm4916RunnerState *s = opaque;
    uint16_t widx, hrd;

    if (!s->rx.valid) {
        /* ring not up yet - keep the heartbeat alive so we re-check */
        timer_mod(s->rx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000); /* 1ms */
        return;
    }

    widx = lduw_be_p(s->rx_cfg + RING_CFG_WRITE_IDX_OFF);
    hrd  = lduw_be_p(s->rx_cfg + RING_CFG_READ_IDX_OFF);

    if (widx == hrd) {
        /* ring drained -> deassert level */
        if (s->irq_asserted) {
            qemu_set_irq(s->irq[RUNNER_IRQ_QUEUE0], 0);
            qemu_set_irq(s->irq[RUNNER_IRQ_FPM], 0);
            s->irq_asserted = false;
        }
        /*
         * Re-enable the backend RX poll. A dgram/socket backend disables its
         * read handler once a receive is deferred; flushing here re-arms it so
         * queued frames keep flowing. Without this only the first injected
         * datagram is ever delivered.
         */
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    } else {
        /* still pending -> keep line high and re-check soon */
        timer_mod(s->rx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 200000); /* 200us */
        return;
    }

    /*
     * Keep a slow heartbeat so the backend RX poll is periodically re-armed and
     * the level IRQ is re-checked even when the guest is idle (1ms).
     */
    timer_mod(s->rx_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000); /* 1ms */
}


static bool runner_can_receive(NetClientState *nc)
{
    Bcm4916RunnerState *s = qemu_get_nic_opaque(nc);

    /* need the delivery ring up AND a posted (unconsumed) feed buffer */
    return s->rx.valid && s->feed.valid &&
           (uint16_t)s->feed_rcons != feed_write_idx(s);
}

static ssize_t runner_receive(NetClientState *nc, const uint8_t *buf, size_t len)
{
    Bcm4916RunnerState *s = qemu_get_nic_opaque(nc);
    AddressSpace *as = runner_dma_as(s);
    hwaddr desc_pa, feed_pa;
    uint8_t desc[DESC_SIZE];
    uint8_t feed[FEED_DESC_SIZE];
    uint32_t fw0, fw1;
    uint64_t buf_pa;
    uint32_t copylen;
    uint16_t fwidx, widx;

    qemu_log("bcm4916-runner: RX recv len=%zu rx.valid=%d feed.valid=%d\n",
             len, s->rx.valid, s->feed.valid);
    if (!s->rx.valid || !s->feed.valid) {
        return -1;   /* rings not up yet; tell backend we couldn't take it */
    }
    if (len > RX_BUF_MAX) {
        len = RX_BUF_MAX;   /* truncate jumbo to one chunk for first light */
    }

    /*
     * HW FAST PATH (offload): compute the NAT-C key for this frame and look it
     * up. On a HIT, the Runner runs the embedded cmdlist (VLAN edit) and
     * forwards the frame to the egress port WITHOUT delivering it to the CPU RX
     * ring - exactly the behaviour that proves offload (subsequent packets
     * bypass the A53). On a MISS, fall through to the slow path (deliver to
     * CPU), where the driver's flowtable learns the flow and programs NAT-C.
     */
    {
        uint8_t key[16];
        int hit;
        bool routed = false;

        /* Try the L2 bridge key first (Phase 1). */
        natc_compute_key(buf, len, key);
        hit = natc_lookup(s, key);

        /* Then the L3 IPv4 5-tuple key (Phase 2 routed/NAT). */
        if (hit < 0 && natc_compute_l3_key(buf, len, key)) {
            hit = natc_lookup(s, key);
            routed = (hit >= 0);
        }

        if (hit >= 0) {
            g_autofree uint8_t *fwd = g_malloc(RX_BUF_MAX);
            size_t newlen;
            bool is_routed_ctx =
                !!(s->natc[hit].ctx[CTX_OFF_FLAGS] & CTX_FLAG_IS_ROUTED);

            memcpy(fwd, buf, len);
            newlen = natc_run_cmdlist(s->natc[hit].ctx, fwd, len);
            s->off_hits++;
            qemu_log("bcm4916-runner: NAT-C HIT idx=%d (%s) -> HW forward "
                     "(offload), len %zu->%zu, hits=%" PRIu64 " (CPU bypassed)\n",
                     hit, is_routed_ctx ? "routed/NAT" : "L2",
                     len, newlen, s->off_hits);
            if (routed && newlen >= L2_HLEN_M + 20) {
                qemu_log("bcm4916-runner:   NAT rewrite: src %u.%u.%u.%u:%u "
                         "TTL=%u  ipcsum=0x%02x%02x l4csum=0x%02x%02x\n",
                         fwd[IP4_OFF_SADDR_M], fwd[IP4_OFF_SADDR_M + 1],
                         fwd[IP4_OFF_SADDR_M + 2], fwd[IP4_OFF_SADDR_M + 3],
                         (fwd[34] << 8) | fwd[35], fwd[IP4_OFF_TTL_M],
                         fwd[IP4_OFF_CSUM_M], fwd[IP4_OFF_CSUM_M + 1],
                         fwd[50], fwd[51]);
            }
            /* forward out the egress port; CPU RX ring untouched */
            qemu_send_packet(qemu_get_queue(s->nic), fwd, newlen);
            return len;
        }
        s->off_misses++;
        qemu_log("bcm4916-runner: NAT-C MISS -> CPU slow path, misses=%" PRIu64 "\n",
                 s->off_misses);
    }

    /*
     * 0. Take the next UNCONSUMED feed buffer. The host posts empty buffers into
     * the feed ring and bumps feed write_idx (@+6); we track feed_rcons (the
     * runner read_idx) and pull the buffer at that slot.
     */
    fwidx = feed_write_idx(s);
    if ((uint16_t)s->feed_rcons == fwidx) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm4916-runner: feed ring empty (rcons=%u widx=%u), drop\n",
                      s->feed_rcons, fwidx);
        return len;   /* consumed (dropped): no buffer to DMA into */
    }
    feed_pa = s->feed.base +
              (uint64_t)(s->feed_rcons % s->feed.depth) * FEED_DESC_SIZE;
    if (dma_memory_read(as, feed_pa, feed, FEED_DESC_SIZE,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return -1;
    }
    fw0 = ldl_be_p(feed + 0);     /* ptr_low[31:0] */
    fw1 = ldl_be_p(feed + 4);     /* type/abs(b8) | ptr_hi[7:0] */
    buf_pa = ((uint64_t)((fw1 >> FEED_W1_PTR_HI_SHIFT) & FEED_W1_PTR_HI_MASK)
              << 32) | fw0;
    copylen = len;

    /* 1. DMA the frame into the feed-provided buffer */
    if (dma_memory_write(as, buf_pa, buf, copylen,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return -1;
    }

    /*
     * 2. Write a 16-B CPU_RX_DESCRIPTOR into the DELIVERY ring at the runner
     * producer slot (s->rx.idx). word0 = buf phys low32; word1 = abs |
     * packet_length<<2 | ptr_hi<<24; word2 = is_src_lan; word3 = flags(0).
     */
    desc_pa = s->rx.base + (uint64_t)s->rx.idx * DESC_SIZE;
    stl_be_p(desc + 0, (uint32_t)buf_pa);
    stl_be_p(desc + 4, RXD_W1_ABS |
                       ((copylen & RXD_W1_PKT_LEN_MASK) << RXD_W1_PKT_LEN_SHIFT) |
                       (((uint32_t)(buf_pa >> 32) & 0xff) << RXD_W1_PTR_HI_SHIFT));
    stl_be_p(desc + 8, RXD_W2_IS_SRC_LAN);   /* LAN, source_port 0, single pipe */
    stl_be_p(desc + 12, 0);                  /* plain LAN unicast */
    if (dma_memory_write(as, desc_pa, desc, DESC_SIZE,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return -1;
    }

    /* 3. advance the runner producer (delivery write_idx) and feed read_idx */
    s->rx.idx = (s->rx.idx + 1) % s->rx.depth;
    s->feed_rcons = (uint16_t)(s->feed_rcons + 1);
    widx = (uint16_t)s->rx.idx;
    rx_set_deliv_write_idx(s, widx);   /* dma_wmb-equivalent: desc written first */
    rx_set_feed_read_idx(s, (uint16_t)s->feed_rcons);
    qemu_log("bcm4916-runner: RX deliver slot=%u buf=0x%" PRIx64
             " len=%u deliv_widx=%u feed_rcons=%u\n",
             (widx - 1) & 0xffff, buf_pa, copylen, widx, s->feed_rcons);

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

    /* mirror the runner read_idx back into the indices entry @+0 (BE u16) so the
     * driver's ringstat sees the runner consuming TX. */
    stw_be_p(s->tx_indices + 0, (uint16_t)s->tx.idx);
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

    /*
     * NAT-C debug counters (model-only; not real silicon registers). Lets a
     * test read offload hit/miss without parsing the QEMU log.
     *   NATC base + 0x00 : off_hits (frames HW-forwarded, CPU bypassed)
     *   NATC base + 0x08 : off_misses (frames sent to CPU slow path)
     */
    if (addr >= XRDP_OFF_NATC && addr < XRDP_OFF_NATC + 0x10) {
        hwaddr off = addr - XRDP_OFF_NATC;
        switch (off) {
        case 0x00: return (uint32_t)s->off_hits;
        case 0x08: return (uint32_t)s->off_misses;
        default:   return 0;
        }
    }

    /*
     * Per-core RNR_MEM data SRAM: serve the runner-advanced indices the driver
     * polls (BE u16 each). The driver does be16_to_cpu(ioread16(...)), so we
     * return the byte-swapped value (== cpu_to_be16 as the LE guest sees it).
     *   RX delivery write_idx : core3 +0x3000+6 (runner advances on delivery)
     *   FEED read_idx         : core3 +0x0f70+12 (runner advances pulling bufs)
     *   TX read_idx           : core2 +0x29c8+0 (runner advances consuming TX)
     */
    if (addr >= XRDP_OFF_RNR_MEM0 &&
        addr < XRDP_OFF_RNR_MEM0 + XRDP_RNR_CORES * XRDP_RNR_MEM_STRIDE) {
        hwaddr rel = addr - XRDP_OFF_RNR_MEM0;
        uint32_t core = rel / XRDP_RNR_MEM_STRIDE;
        hwaddr in_core = rel % XRDP_RNR_MEM_STRIDE;

        if (core == CPU_RX_RING_CORE &&
            in_core == RDD_RX_DELIV_RING_OFF + RING_CFG_WRITE_IDX_OFF) {
            return bswap16(lduw_be_p(s->rx_cfg + RING_CFG_WRITE_IDX_OFF));
        }
        if (core == CPU_RX_RING_CORE &&
            in_core == RDD_FEED_RING_OFF + RING_CFG_READ_IDX_OFF) {
            return bswap16(lduw_be_p(s->feed_cfg + RING_CFG_READ_IDX_OFF));
        }
        if (core == CPU_TX_RING_CORE &&
            in_core == CPU_TX_RING_INDICES_OFF + 0) {
            return bswap16(lduw_be_p(s->tx_indices + 0));
        }
        return 0;
    }

    /* everything else reads 0 (other RNR_MEM/PSRAM reads are not used) */
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

    /*
     * PSRAM block: only NAT-C offload staging lives here now. The CPU rings
     * moved to per-core RNR_MEM data SRAM (handled below).
     */
    if (addr >= XRDP_OFF_PSRAM && addr < XRDP_OFF_PSRAM + 0x10000) {
        hwaddr off = addr - XRDP_OFF_PSRAM;
        /*
         * NAT-C offload staging. The driver memcpy_toio's the 16-byte BE key
         * and the context entry (word-by-word, 32-bit), then writel's the index
         * and the command. memcpy_toio writes the BE-in-memory bytes with a raw
         * store, so on this LE guest each 32-bit write arrives as the BE byte
         * sequence reinterpreted as an int: store it LE to reconstruct the
         * original BE byte order in our staging buffer.
         */
        if (off >= PSRAM_NATC_STAGE_KEY &&
            off < PSRAM_NATC_STAGE_KEY + 16 && size == 4) {
            stl_le_p(s->natc_stage_key + (off - PSRAM_NATC_STAGE_KEY),
                     (uint32_t)val);
            return;
        }
        if (off >= PSRAM_NATC_STAGE_CTX &&
            off < PSRAM_NATC_STAGE_CTX + CTX_ENTRY_MAX) {
            hwaddr coff = off - PSRAM_NATC_STAGE_CTX;
            /* context is copied with memcpy_toio (variable length, may be
             * 1/2/4/8-byte chunks); store byte-wise to be size-agnostic. */
            unsigned i;
            for (i = 0; i < size && coff + i < CTX_ENTRY_MAX; i++) {
                s->natc_stage_ctx[coff + i] = (val >> (8 * i)) & 0xff;
            }
            return;
        }
        if (off == PSRAM_NATC_INDIR_INDEX && size == 4) {
            s->natc_stage_idx = (uint32_t)val;
            return;
        }
        if (off == PSRAM_NATC_INDIR_CMD && size == 4) {
            switch ((uint32_t)val) {
            case NATC_CMD_ADD: natc_add(s); break;
            case NATC_CMD_DEL: natc_del(s); break;
            default: break;
            }
            return;
        }
        return;
    }

    /*
     * Per-core RNR_MEM data SRAM: the CPU rings live here now (NOT PSRAM).
     *   core3 +0x3000 : RX delivery ring CPU_RING_DESCRIPTOR (16B)
     *   core3 +0x0f70 : FEED ring CPU_RING_DESCRIPTOR (16B; entry size 8B)
     *   core2 +0x33e0 : TX ring CPU_RING_DESCRIPTOR (16B)
     *   core2 +0x29c8 : TX indices entry {read_idx@+0, write_idx@+2} (BE u16)
     * The driver memcpy_toio's the cfg blocks (raw word stores: the BE-in-memory
     * bytes reinterpreted as a native int, so store byte-wise from the LE value
     * to reconstruct the original BE byte order). It then runtime-updates the
     * indices via iowrite16(cpu_to_be16()) at +6 (feed/RX) / +2 (TX), which on a
     * LE guest arrive byte-swapped.
     */
    if (addr >= XRDP_OFF_RNR_MEM0 &&
        addr < XRDP_OFF_RNR_MEM0 + XRDP_RNR_CORES * XRDP_RNR_MEM_STRIDE) {
        hwaddr rel = addr - XRDP_OFF_RNR_MEM0;
        uint32_t core = rel / XRDP_RNR_MEM_STRIDE;
        hwaddr in_core = rel % XRDP_RNR_MEM_STRIDE;
        unsigned i;

        /* capture a cfg control block (memcpy_toio raw bytes) into 'cfg', then
         * (re)parse it on the block-completing store. */
        #define CAPTURE_CFG(base_off, cfgbuf, ring, res) do {                  \
            hwaddr o = in_core - (base_off);                                   \
            for (i = 0; i < size && o + i < DESC_SIZE; i++) {                  \
                (cfgbuf)[o + i] = (val >> (8 * i)) & 0xff;                     \
            }                                                                  \
            if (o + size >= DESC_SIZE) {                                       \
                runner_parse_ring((cfgbuf), &(ring), (res));                   \
            }                                                                  \
        } while (0)

        /* RX delivery ring cfg (core3 +0x3000) */
        if (core == CPU_RX_RING_CORE &&
            in_core >= RDD_RX_DELIV_RING_OFF &&
            in_core < RDD_RX_DELIV_RING_OFF + DESC_SIZE) {
            CAPTURE_CFG(RDD_RX_DELIV_RING_OFF, s->rx_cfg, s->rx, RING_RES_32);
            if (in_core - RDD_RX_DELIV_RING_OFF + size >= DESC_SIZE) {
                s->rx.idx = 0;
                qemu_log("bcm4916-runner: RX deliv ring base=0x%" PRIx64
                         " depth=%u esz=%u valid=%d\n",
                         s->rx.base, s->rx.depth, s->rx.entry_size, s->rx.valid);
            }
            return;
        }
        /* FEED ring cfg (core3 +0x0f70) */
        if (core == CPU_RX_RING_CORE &&
            in_core >= RDD_FEED_RING_OFF &&
            in_core < RDD_FEED_RING_OFF + DESC_SIZE) {
            CAPTURE_CFG(RDD_FEED_RING_OFF, s->feed_cfg, s->feed, RING_RES_64);
            if (in_core - RDD_FEED_RING_OFF + size >= DESC_SIZE) {
                s->feed_rcons = 0;
                qemu_log("bcm4916-runner: FEED ring base=0x%" PRIx64
                         " depth=%u esz=%u valid=%d feed_widx=%u\n",
                         s->feed.base, s->feed.depth, s->feed.entry_size,
                         s->feed.valid, feed_write_idx(s));
            }
            return;
        }
        /* feed doorbell: BE u16 write_idx @ +6 of the feed cfg (runtime bump) */
        if (core == CPU_RX_RING_CORE &&
            in_core == RDD_FEED_RING_OFF + RING_CFG_WRITE_IDX_OFF && size == 2) {
            stw_be_p(s->feed_cfg + RING_CFG_WRITE_IDX_OFF, bswap16((uint16_t)val));
            /* a freshly posted buffer may unblock a deferred backend RX */
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
            return;
        }
        /* RX delivery read_idx @ +12 (host advances as it consumes) */
        if (core == CPU_RX_RING_CORE &&
            in_core == RDD_RX_DELIV_RING_OFF + RING_CFG_READ_IDX_OFF && size == 2) {
            stw_be_p(s->rx_cfg + RING_CFG_READ_IDX_OFF, bswap16((uint16_t)val));
            return;
        }

        /* TX ring cfg (core2 +0x33e0) */
        if (core == CPU_TX_RING_CORE &&
            in_core >= RDD_TX_RING_OFF &&
            in_core < RDD_TX_RING_OFF + DESC_SIZE) {
            CAPTURE_CFG(RDD_TX_RING_OFF, s->tx_cfg, s->tx, RING_RES_32);
            if (in_core - RDD_TX_RING_OFF + size >= DESC_SIZE) {
                s->tx.idx = 0;
                qemu_log("bcm4916-runner: TX ring base=0x%" PRIx64
                         " depth=%u esz=%u valid=%d\n",
                         s->tx.base, s->tx.depth, s->tx.entry_size, s->tx.valid);
            }
            return;
        }
        /* TX doorbell: BE u16 write_idx @ +2 of the indices entry (core2) */
        if (core == CPU_TX_RING_CORE &&
            in_core == CPU_TX_RING_INDICES_OFF + 2 && size == 2) {
            uint16_t widx = bswap16((uint16_t)val);
            stw_be_p(s->tx_indices + 2, widx);
            runner_tx_kick(s, widx);
            return;
        }
        /* TX indices read_idx slot @ +0 (host never writes it; ignore) */
        return;
        #undef CAPTURE_CFG
    }

    /*
     * Per-core RNR_REGS control block. The driver pokes many regs during
     * bring-up (GEN_CFG zero-mem, SCH/PSRAM/DDR cfg, GLOBAL_CTRL.EN) and a
     * per-frame CFG_CPU_WAKEUP for the TX thread. The emulator IS the runner, so
     * accept and ignore them (the TX index write already triggers consume).
     */
    if (addr >= XRDP_OFF_RNR_REGS0 &&
        addr < XRDP_OFF_RNR_REGS0 + XRDP_RNR_CORES * XRDP_RNR_REGS_STRIDE) {
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
    memset(&s->tx, 0, sizeof(s->tx));
    memset(&s->feed, 0, sizeof(s->feed));
    s->feed_rcons = 0;
    memset(s->rx_cfg, 0, sizeof(s->rx_cfg));
    memset(s->tx_cfg, 0, sizeof(s->tx_cfg));
    memset(s->feed_cfg, 0, sizeof(s->feed_cfg));
    memset(s->tx_indices, 0, sizeof(s->tx_indices));
    memset(s->natc, 0, sizeof(s->natc));
    memset(s->natc_stage_key, 0, sizeof(s->natc_stage_key));
    memset(s->natc_stage_ctx, 0, sizeof(s->natc_stage_ctx));
    s->natc_stage_idx = 0;
    s->off_hits = 0;
    s->off_misses = 0;
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

    /* start the RX heartbeat (re-arms the backend poll; see drain_check) */
    timer_mod(s->rx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
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
