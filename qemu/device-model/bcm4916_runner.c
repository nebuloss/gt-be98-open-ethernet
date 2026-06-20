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
#define XRDP_OFF_NATC        0x00950000ULL   /* NAT-C engine (ABI 1.1) */

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

/* -------- NAT-C offload contract (matches driver/runner/flow_offload.h) ---- */
#define PSRAM_NATC_STAGE_KEY     0x0100   /* 16-byte masked BE key */
#define PSRAM_NATC_STAGE_CTX     0x0120   /* FC_UCAST_FLOW_CONTEXT_ENTRY */
#define PSRAM_NATC_INDIR_INDEX   0x0200   /* table index (u32) */
#define PSRAM_NATC_INDIR_CMD     0x0204   /* command reg (u32): 3=add 4=del */
#define  NATC_CMD_ADD            3
#define  NATC_CMD_DEL            4

/* context-entry byte offsets (matches flow_offload.h CTX_OFF_*) */
#define CTX_OFF_FLAGS            8
#define  CTX_FLAG_IS_L2_ACCEL    (1u << 4)
#define CTX_OFF_VPORT            12
#define CTX_OFF_CMDLIST_OFF      16
#define CTX_OFF_CMDLIST_DLEN     96
#define CTX_OFF_CMDLIST_LEN      97
#define CTX_OFF_VALID            98
#define CTX_ENTRY_MAX            128

/* XPE opcodes (matches cmdlist.h; opcode = cmd_word >> 26) */
#define XPE_OP_MCOPY             0x13   /* INSERT (VLAN push) */
#define XPE_OP_REPLACE           0x18   /* REPLACE bits/16/32 (VLAN edit, NAT addr/port) */
#define XPE_OP_ADD               0x1a   /* ADD imm; decrement_8 = ADD(-1) on TTL */
#define XPE_OP_MOVE              0x2c   /* DELETE (VLAN pop) */
#define XPE_OP_ICSUM            0x36   /* incremental checksum fixup (IP / L4) */
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
 * driver's xrdp_build_key does for an is_routed flow (flow_offload.c):
 *   w0 = ORIGINAL src IP; w1 = ORIGINAL dst IP;
 *   w2 = sport<<16 | dport; w3 = ip_proto<<24 | vport<<4 | 0x1.
 * The key is the ORIGINAL (ingress) tuple - the lookup runs before the cmdlist
 * rewrites the packet. Returns false if the frame is not parseable IPv4 TCP/UDP.
 * Assumes an untagged IPv4 frame (ethertype 0x0800 at [12:13], IHL=5).
 */
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
    /* w3 = proto<<24 | vport<<4 | 0x1  (vport=0) */
    key[12] = proto;
    key[13] = 0x00;
    key[14] = 0x00;
    key[15] = 0x01;
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
 * length. Decodes 16-bit BE words; a command word's opcode is bits[31:26].
 * Matches the driver's cmdlist.c packing:
 *   word0: [31:26]=opcode [25:18]=offset8 [17:13]=position [12:8]=width [7:0]=nbytes
 *
 * Phase-1 L2: MCOPY=insert (VLAN push), MOVE=delete (VLAN pop), REPLACE-bits
 *   (TPID/TCI/VID write; width set, nbytes=0, +1 data word), NOP=terminate.
 * Phase-2 L3/NAT: ADD=decrement_8 (TTL -1, nbytes/width=8), REPLACE-32 (IP
 *   SA/DA NAT; nbytes=4, +2 data words), REPLACE-16 (L4 port NAPT; nbytes=2,
 *   +1 data word), ICSUM (recompute IP or L4 checksum at offset).
 *
 * REPLACE is disambiguated by nbytes: 4 => 32-bit replace, 2 => 16-bit replace,
 * else => bit-field replace (the Phase-1 VLAN packing).
 *
 * FRAMING (matches the RE'd stock emitter, xpe_api.o xpe_cmd_end): the program
 * is LENGTH-DELIMITED by cmd_list_data_length (dlen) - there is NO NOP
 * terminator word. We walk command words until pos reaches dlen. The stock 0xfc
 * slot-pad bytes lie past dlen and are never reached. The XPE_OP_NOP/default
 * case below is a defensive early-out only.
 */
static size_t natc_run_cmdlist(const uint8_t *ctx, uint8_t *frame, size_t len)
{
    uint8_t dlen = ctx[CTX_OFF_CMDLIST_DLEN];
    const uint8_t *cl = ctx + CTX_OFF_CMDLIST_OFF;
    int pos = 0;

    while (pos + 4 <= dlen) {
        uint32_t cmd = (cl[pos] << 24) | (cl[pos + 1] << 16) |
                       (cl[pos + 2] << 8) | cl[pos + 3];
        uint8_t opcode = (cmd >> 26) & 0x3f;
        uint8_t offset = (cmd >> 18) & 0xff;
        uint8_t width  = (cmd >> 8) & 0x1f;
        uint8_t nbytes = cmd & 0xff;
        pos += 4;

        switch (opcode) {
        case XPE_OP_MCOPY:  /* INSERT nbytes at offset (VLAN push) */
            if (offset <= len && len + nbytes <= RX_BUF_MAX) {
                memmove(frame + offset + nbytes, frame + offset, len - offset);
                memset(frame + offset, 0, nbytes);
                len += nbytes;
            }
            break;
        case XPE_OP_MOVE:   /* DELETE nbytes at offset (VLAN pop) */
            if (offset + nbytes <= len) {
                memmove(frame + offset, frame + offset + nbytes,
                        len - offset - nbytes);
                len -= nbytes;
            }
            break;
        case XPE_OP_ADD:    /* decrement_8: ADD(-1) on an 8-bit field (TTL) */
            if (offset < len) {
                frame[offset] = (uint8_t)(frame[offset] - 1);
            }
            break;
        case XPE_OP_ICSUM:  /* recompute checksum at 'offset' (no inline data) */
            if (offset == IP4_OFF_CSUM_M) {
                fixup_ip_csum(frame, len, offset);
            } else {
                fixup_l4_csum(frame, len, offset);
            }
            break;
        case XPE_OP_REPLACE:
            if (nbytes == 4) {              /* REPLACE-32: IP SA/DA NAT */
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
            } else if (nbytes == 2) {       /* REPLACE-16: L4 port NAPT */
                uint16_t d16 = 0;
                if (pos + 2 <= dlen) {
                    d16 = (cl[pos] << 8) | cl[pos + 1];
                    pos += 2;
                }
                if ((size_t)(offset + 2) <= len) {
                    frame[offset]     = (d16 >> 8) & 0xff;
                    frame[offset + 1] = d16 & 0xff;
                }
            } else {                        /* REPLACE-bits: VLAN edit */
                uint16_t data16 = 0;
                if (pos + 2 <= dlen) {
                    data16 = (cl[pos] << 8) | cl[pos + 1];
                    pos += 2;
                }
                if (width >= 16 && (size_t)(offset + 2) <= len) {
                    frame[offset] = (data16 >> 8) & 0xff;
                    frame[offset + 1] = data16 & 0xff;
                } else if ((size_t)(offset + 2) <= len) {
                    uint16_t cur = (frame[offset] << 8) | frame[offset + 1];
                    uint16_t mask = (1u << width) - 1;
                    cur = (cur & ~mask) | (data16 & mask);
                    frame[offset] = (cur >> 8) & 0xff;
                    frame[offset + 1] = cur & 0xff;
                }
            }
            break;
        case XPE_OP_NOP:
        default:
            return len;  /* terminator */
        }
    }
    return len;
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
        /* ring not up yet - keep the heartbeat alive so we re-check */
        timer_mod(s->rx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000); /* 1ms */
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
        /*
         * Re-enable the backend RX poll. A dgram/socket backend disables its
         * read handler once a receive is deferred (or returns a short result);
         * flushing here re-arms it so queued frames keep flowing. Without this
         * only the first injected datagram is ever delivered.
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
