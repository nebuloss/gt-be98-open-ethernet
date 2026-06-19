# BCM4916 XRDP slow-path datapath ABI — implementable spec (host↔Runner CPU ring)

First concrete coding target for the open mainline driver: how the host sets up and runs the
Runner CPU datapath (RX/TX of trapped/CPU-forwarded packets). This note PINS the 4916-specific
register bases/offsets and the descriptor/ring/FPM-token bitfields from the stock binaries, and
gives a numbered bring-up checklist plus an open-driver datapath plan.

Companion notes: `xrdp-cpu-datapath.md` (architecture/feasibility, 63138 Rosetta-stone),
`runner-microcode-and-cpuring.md` (microcode license = Proprietary, the blocker),
`rdpa-offload-controlplane.md` (fast path / cmdlist), `bcm4916-regmap.md` (FDT bases).

**Confidence tags**
- `[KO]` recovered directly from stock **`rdpa.ko`** (license=Proprietary; RE = ELF symbol/
  reloc decode + targeted disasm of the auto-generated `dump_RDD_*` pretty-printers; no
  `aaa`/`izz`). Disasm offsets are relative to each function's `.text` value.
- `[SDK]` read from the **4916 "behnd" GPL SDK** (asuswrt-merlin.ng src-rt-5.04behnd.4916),
  file:line cited. NB: this SDK ships a **GPL FPM driver and GPL enet CPU-ring caller** — more
  of the host side is GPL than the earlier notes assumed.
- `[SDK-OLD]` from the old GPL `broadcom-sdk-416L05` (RDP impl2 / 63138) — structural template.
- `[FDT]` live device tree. `[INFER]` deduced.

Everything below is **pinned-from-binary or GPL-source** unless marked `[INFER]`/UNKNOWN.

---

## 0. The one unavoidable blocker (unchanged)

Moving even one packet requires the **Runner microcode**, which on 4916 exists only inside
`rdpa.ko` as `fw_binary_0..7` (8x32 KB) + `fw_predict_0..7` (8x1 KB), **license=Proprietary,
non-redistributable** (see `runner-microcode-and-cpuring.md`). This spec recovers the *host*
ABI so the open driver is ready the moment a loadable microcode path exists (vendor module on
device -> taint P; or a future licensed image). The host ABI is now well-pinned; the microcode
license is the remaining make-or-break.

---

## 1. XRDP REGISTER MAP (4916 / BCM6813) — CONFIRMED from rdpa.ko

All bases are absolute SoC addresses inside the `brcm,rdpa` window (`0x82000000`, size
`0x00caf004` `[FDT]`). Recovered by decoding the `*_ADDRS` relocated pointer arrays in
`.data` and the `*_REG` register-descriptor structs in `.rodata` of `rdpa.ko` `[KO]`.

### 1.1 Block base table (all `[KO]` unless noted)

| Block | Base(s) | Count / stride | Purpose |
|---|---|---|---|
| **PSRAM** | `0x82000000` | 1 | Runner Private-SRAM (RDD tables: rings, reason table, contexts) |
| **RNR_REGS[0..7]** | `0x82800000` + n*`0x1000` | 8 cores | per-core Runner control regs (enable/reset/pc) |
| **RNR_MEM[0..7]** (data) | `0x82700000` + n*`0x20000` | 8, stride 0x20000 | per-core data/scratch SRAM |
| **RNR_INST[0..7]** | core_base+`0x10000` (`0x82710000`...) | 8 | per-core **instruction SRAM**; 8192 words x4B = 32 KB (`ents-1=0x1fff`, stride 4) => matches `fw_binary_n` 32768 B |
| **RNR_CNTXT[0..7]** | core_base+`0x18000` (`0x82718000`...) | 8 | per-core context SRAM (512 x4B) |
| **RNR_PRED[0..7]** | core_base+`0x1c000` (`0x8271c000`...) | 8 | per-core **prediction RAM**; 512 x2B = 1 KB => matches `fw_predict_n` 1024 B |
| **RNR_QUAD** | `0x82808400` | 1 | quad/common Runner config |
| **DSPTCHR** (dispatcher) | `0x82880000` | 1 | ingress dispatcher / queue->core scheduling |
| **SBPM** | `0x828a1000` | 1 | SRAM buffer-pool mgr (short bufs / headers) |
| **UBUS_SLV** | `0x828a0000` | 1 | UBUS slave (address-decode window for Runner) |
| **DMA[0..2]** | `0x828a1800`,`0x828a1c00`,`0x828a2000` | 3 | the BBH<->DDR DMA/SDMA engines |
| **BBH_RX[0..11]** | `0x82898000` + n*`0x400` | 12 ports | per-port RX Buffer/Burst Handler (MAC->pool DMA) |
| **BBH_TX[0..3]** | `0x82890000` + n*`0x2000` | 4 | TX Buffer/Burst Handler (pool->MAC DMA) |
| **UNIMAC_RDP[0..3]** | `0x828a8004` + n*`0x1000` | 4 | UNIMAC datapath glue (1G ports; cf FDT `/unimac`) |
| **HASH** | `0x82920000` | 1 | hash/lookup engine (flow keys — fast path) |
| **CNPL** | `0x82948000` | 1 | counter/policer block |
| **NATC** | `0x82950000` | 1 | NAT-cache engine (fast path) |
| **NATC_TBL[0..7]** | `0x829502d0` + n*0x10 | 8 | NAT-C per-table cfg (fast path) |
| **NATC_KEY[0..7]** | `0x829503b0` + n*0x20 | 8 | NAT-C key regs (fast path) |
| **NATC_DDR_CFG** | `0x8295038c` | 1 | NAT-C DDR table cfg (fast path) |
| **FPM** | `0x82a00000` | 1 | **Free-Pool Mgr** (the main RX/TX buffer pool) |
| **QM** | `0x82c00000` | 1 | Queue Manager (egress queues/schedulers) |
| **DQM** | `0x82c80034` | 1 | DQM (descriptor/doorbell queue mgr; CPU-TX path) |
| XLIF0/1_* (RX_IF/TX_IF/EEE/FLOW_CONTROL/Q_OFF) | various `0x82...` | per-XLMAC | 10G XLMAC datapath interface (eth0/1/3) |
| BAC_IF, TCAM, PSRAM1, UBUS_MSTR, UNIMAC_MISC | (decodable same way if needed) | | aux |

Source arrays (in `rdpa.ko` `.data`, resolved via `.rela.data`) `[KO]`:
`FPM_ADDRS`, `SBPM_ADDRS`, `BBH_RX_ADDRS` (12), `BBH_TX_ADDRS` (4), `DSPTCHR_ADDRS`,
`QM_ADDRS`, `DQM_ADDRS`, `DMA_ADDRS` (3), `NATC_ADDRS`/`NATC_TBL_ADDRS`/`NATC_KEY_ADDRS`,
`HASH_ADDRS`, `PSRAM_ADDRS`, `UNIMAC_RDP_ADDRS` (4), `UBUS_SLV_ADDRS`, `CNPL_ADDRS`,
`RNR_REGS_ADDRS` (8), `RNR_INST_ADDRS`/`RNR_MEM_ADDRS`/`RNR_CNTXT_ADDRS`/`RNR_PRED_ADDRS`
(8 each), `RNR_QUAD_ADDRS`.

### 1.2 Register-descriptor struct (the `*_REG` objects, 32 B, `.rodata`) `[KO]`

Each `ag_drv_*` accessor reads a 32-byte descriptor: `+0x00`=name ptr (reloc), `+0x08`=
**register offset within the block**, `+0x10`=array-count-1, `+0x14`=entry stride (bytes),
`+0x18`=block id. Confirmed offsets:

| Register | Block-relative offset | notes |
|---|---|---|
| `FPM_FPM_CTL_REG` | `+0x000` | FPM master control |
| `FPM_FPM_CFG1_REG` | `+0x004` | FPM config (token size/shift live in RDD `FPM_GLOBAL_CFG`, sec 3.2) |
| `FPM_POOL1_CFG1_REG` | `+0x040` | pool0 cfg |
| `FPM_POOL1_STAT1_REG` | `+0x050` | pool0 status (tokens available etc.) |
| `FPM_POOL1_ALLOC_DEALLOC_REG` | `+0x400` | **alloc (read) / dealloc (write) token** |
| `BBH_RX_*_BBCFG_REG` | `+0x000` | per-port (base = `BBH_RX_ADDRS[port]`) |
| `BBH_RX_*_DISPVIQ_REG` | `+0x004` | dispatcher VIQ binding |
| `BBH_RX_*_FLOWCTRL_REG` | `+0x034` | flow control |
| `BBH_RX_*_ENABLE_REG` | `+0x03c` | per-port RX enable |
| `RNR_INST_MEM_ENTRY_REG` | `+0x000` of INST base | 8192 x4B window (write microcode here) |
| `RNR_PRED_MEM_ENTRY_REG` | `+0x000` of PRED base | 512 x2B window (write prediction here) |

Full BBH_RX / BBH_TX / FPM / SBPM / DSPTCHR / QM register sets are present as `*_REG`
objects and can be decoded the same way on demand (the `+0x08` field is the offset). Accessor
surface in `rdpa.ko`: `ag_drv_unimac_rdp_*`(365), `ag_drv_rnr_quad_*`(191),
`ag_drv_bbh_tx_*`(178), `ag_drv_bbh_rx_*`(113), `ag_drv_sbpm_regs_*`(64),
`ag_drv_rnr_regs_*`(51), `ag_drv_fpm_pool*`/`ag_drv_fpm_fpm_*`, `ag_drv_dsptchr_*`,
`ag_drv_qm_*`, `ag_drv_dqm_*`, `ag_drv_natc_*`, `ag_drv_dma_*` `[KO]`.

---

## 2. RING / DESCRIPTOR BITFIELDS — CONFIRMED

The XRDP CPU datapath uses **separate rings** (vs the single inline-refill ring of 63138):
- **data ring** — Runner writes RX packet descriptors here (host DDR); host polls.
- **feed ring** — host posts FREE FPM buffers to the Runner to refill RX.
- **recycle ring** — Runner hands spent buffers back to the host.
- **CPU-TX ring** — host pushes TX descriptors (in Runner SRAM, indices in PSRAM).

RDD table objects present in `rdpa.ko` `[KO]`: `CPU_RING_DESCRIPTORS_TABLE`,
`CPU_FEED_RING_DESCRIPTOR_TABLE`/`_CACHE_TABLE`/`_INDEX_DDR_ADDR_TABLE`/`_RSV_TABLE`,
`CPU_RECYCLE_RING_DESCRIPTOR_TABLE` (+ shadow rd/wr idx, next-ptr, stack, sram pd fifo),
`CPU_TX_RING_DESCRIPTOR_TABLE`/`CPU_TX_RING_INDICES_VALUES_TABLE`,
`CPU_REASON_TO_TC`/`CPU_REASON_TO_METER_TABLE`, `CPU_RX_METER_TABLE`.

### 2.1 `CPU_RING_DESCRIPTOR` — per-ring control descriptor (16 B) `[KO]`

The control block the host fills to register a ring with the Runner. Decoded from
`dump_RDD_CPU_RING_DESCRIPTOR` (rdpa.ko `.text` 0x7df20). **Big-endian in SRAM** (dump does
`rev16/rev32` before masking). Layout (byte offsets within the 16-B entry):

| field | location | width | meaning |
|---|---|---|---|
| `size_of_entry` | byte0 bits[7:3] (`ubfx 3,5`) | 5 | descriptor size code |
| `number_of_entries` | half@0 bits[10:0] | 11 | ring depth |
| `interrupt_id` | half@2 | 16 | IRQ/queue id |
| `drop_counter` | half@4 | 16 | drops |
| `write_idx` | half@6 | 16 | producer index (Runner-written) |
| `base_addr_low` | word@8 | 32 | ring DDR base, low |
| `read_idx` | half@0xc | 16 | consumer index (host-written) |
| `reserved0` | byte@0xe | 8 | — |
| `base_addr_high` | byte@0xf | 8 | ring DDR base, high (40-bit phys) |

Same struct shape (`read_idx`/`write_idx`) reused by `CPU_TX_RING_INDICES` (only those two
fields). `[KO]`

### 2.2 `RING_CPU_TX_DESCRIPTOR` — host TX descriptor (16 B) `[KO]`

The descriptor the host writes to push a packet to the Runner for egress. Decoded from
`dump_RDD_RING_CPU_TX_DESCRIPTOR` (rdpa.ko `.text` 0x81920) by mapping the load/`ubfx`/`and`
sequence to the printed field names. **Big-endian** in SRAM. Bit positions (MSB-first within
each big-endian word):

| field | byte/word | bits | width | meaning |
|---|---|---|---|---|
| `is_egress` | byte0 | [7] | 1 | egress vs ingress descriptor |
| `egress_or_ingress_1` | half@0 | [13:5] (`ubfx 6,9` post-rev16) | 9 | union (egress params / ingress) |
| `packet_length` | word@0 | [21:8] (`ubfx 8,14` post-rev32) | 14 | payload length |
| `sk_buf_ptr_high` | byte3 | [7:0] | 8 | host skb ptr high (abs/host-buffer mode) |
| `sk_buf_ptr_low_or_data_1588` | word@4 | — | 32 | skb ptr low, or 1588 timestamp data |
| `color` | byte8 | [7] | 1 | QoS color |
| `do_not_recycle` | byte8 | [6] | 1 | don't return buffer to pool |
| `flag_1588` | byte8 | [5] | 1 | 1588/PTP |
| `is_emac` | byte8 | [4] | 1 | EMAC (UNIMAC) vs XPORT egress |
| `wan_flow_source_port_union` | half@8 | [11:4] | 8 | egress port / wan flow id |
| `fpm_fallback` | byte9 | [3] | 1 | use FPM fallback alloc |
| `sbpm_copy` | byte9 | [2] | 1 | copy via SBPM |
| `tgtmem_or_l3pkt` | byte9 | [1] | 1 | target-mem / L3 packet |
| `abs` | byte9 | [0] | 1 | **absolute-address mode** (host buffer) vs token mode |
| `egress_or_ingress_2` | byte0xa | [7:0] | 8 | union part 2 |
| `pkt_buf_ptr_high` | byte0xb | [7:0] | 8 | packet buffer ptr high |
| `pkt_buf_ptr_low_or_fpm_bn0` | word@0xc | — | 32 | packet buf ptr low, **or FPM buffer-number (token)** |

Two TX modes are encoded: `abs=1` => host gives an absolute DDR buffer addr
(`sk_buf_ptr_*`/`pkt_buf_ptr_*`); `abs=0` => host gives an FPM token in
`pkt_buf_ptr_low_or_fpm_bn0`. The GPL TX helpers `rdpa_cpu_send_sysb_fpm` /
`f_rdpa_cpu_send_raw_from_fpm` (exported by `rdpa_gpl.ko`) build this in FPM mode.

### 2.3 `PROCESSING_RX_DESCRIPTOR` — the packet descriptor (Runner PD) `[KO]`

Decoded from `dump_RDD_PROCESSING_RX_DESCRIPTOR` (rdpa.ko `.text` 0x84a90). This is the
Runner's packet descriptor; the CPU-RX data-ring entry is this PD shape (the host reads
`packet_length`, `ingress_vport_or_flow`, the buffer pointer/token, `payload_offset_sop`).
Fields (print order): `pd_info, serial_num, ploam, ingress_cong, abs_or_dsl, l3_packet,
error_type_or_cpu_tx, packet_length, error, target_mem_1, cong_state, is_emac,
ingress_vport_or_flow, bn1_last_or_abs1, agg_pd, target_mem_0, payload_offset_sop`. `[KO]`

For a slow-path RX read the host needs: `packet_length`, `ingress_vport_or_flow`
(-> which netdev/port), `payload_offset_sop` (offset into the buffer where data starts),
`abs_or_dsl`/`bn1_last_or_abs1` (buffer = absolute addr vs FPM token), and the reason
(programmed via the reason->queue table, sec 3.5, so a packet that lands in a given ring already
carries its trap reason implicitly).

### 2.4 Host `CPU_RX_DESCRIPTOR` ownership model — CONFIRMED `[SDK]`

The host-facing RX data-ring entry and its ownership poll are GPL-confirmed in
`runner_wfd_inline.h` `[SDK]` (`bcmdrivers/opensource/include/bcm963xx/runner_wfd_inline.h`):
```c
volatile CPU_RX_DESCRIPTOR *pTravel = pDescriptor->base;   /* DDR ring */
if (pTravel->word2) { ... }                  /* slot populated */
/* little-endian host: ownership = MSb of the LSB after byte-swap */
pTravel->word2 = swap4bytes(pTravel->word2 | 0x80);        /* set OWNERSHIP_HOST */
/* big-endian: */ pTravel->ownership = OWNERSHIP_HOST;
bdmf_sysb_databuf_free(phys_to_virt(pTravel->host_buffer_data_pointer), 0);
```
So: **ownership bit = MSB of `word2`** (the word holding the buffer pointer); host re-arms by
setting ownership = HOST after consuming. `host_buffer_data_pointer` is the buffer phys addr.
Runner is big-endian, ARM host LE => `swap4bytes()` every word. **RX is pure DDR
ownership-bit polling — no MMIO doorbell on RX** (an IRQ just wakes the poll). The exact
`CPU_RX_DESCRIPTOR` word0/word1/word3 bitfields are NOT in the 4916 GPL SDK
(`rdp_cpu_ring_defs.h` absent); use the 63138 layout `[SDK-OLD]` as the starting template and
confirm against `PROCESSING_RX_DESCRIPTOR` sec 2.3 / `rdpa_cpu_rx_pd_get` in `rdpa.ko`.
**Partially UNKNOWN** — see sec 6.

---

## 3. FPM POOL + RINGS + REASON TABLE — bring-up data

### 3.1 FPM token format — CONFIRMED `[SDK]`

The 4916 SDK ships a **GPL FPM driver**: `bcmdrivers/opensource/char/fpm/impl1/`
(`fpm_core.c`, `fpm_priv.h`, `fpm.h`). The token is a 32-bit value:

| bits | field | macro (`fpm_priv.h`) |
|---|---|---|
| [31] | VALID | `FPM_TOKEN_VALID_MASK (0x1<<31)` |
| [30:29] | pool (1 bit used) | `FPM_TOKEN_POOL_SHIFT (29)`, `FPM_TOKEN_POOL_MASK (0x1<<29)` |
| [28:12] | token index (17b) | `FPM_TOKEN_INDEX_SHIFT (12)`, `WIDTH (17)`, `MASK (0x1ffff<<12)` |
| [11:0] | token size (bytes, 12b) | `FPM_TOKEN_SIZE_SHIFT (0)`, `MASK (0xfff)` |

- **buffer addr = pool_pbase[pool] + token_index * chunk_size**
  (`__fpm_token_to_buffer`, `fpm_core.c`; `chunk_size` = minimum allocation unit).
- **alloc = READ** the pool's `ALLOC_DEALLOC` register (`FPM_POOL1_ALLOC_DEALLOC_REG`
  @ FPM_base+0x400); **free = WRITE** the token back. `fpm_alloc_token(size)`,
  `fpm_free_token`, `fpm_token_to_buffer`/`fpm_buffer_to_token` are EXPORTED GPL symbols.
- pool sizing: `FPM_POOL_SIZE_FOR_512B_TOKEN = 32 MB`, `...256B = 16 MB` (`fpm_core.c:80`).
- pool status: `FPM_POOL1_STAT*` (tokens-available = `FPMCTRL_POOL_STAT2_NUM_OF_TOKENS_AVAL`,
  mask `0x3ffff`). Token-recovery/expiry control in `FPMCTRL_TOKEN_RECOVER_CTL_*`.

This means the **FPM pool driver is fully GPL-portable** (unlike what the earlier notes
feared) — a big de-risk for the slow path.

### 3.2 `FPM_GLOBAL_CFG` (RDD) — pool geometry the Runner needs `[KO]`

From `dump_RDD_FPM_GLOBAL_CFG` (rdpa.ko `.text` 0x7ecc0). Fields the host programs so the
Runner can decode tokens: `fpm_base_low`, `fpm_base_high` (40-bit pool DDR base),
`fpm_token_size`, `fpm_token_shift`, `fpm_token_add_shift`, `fpm_token_inv_mant`,
`fpm_token_inv_exp` (the size<->token math), `ddr_token_info_low/high`,
`fpm_token_num_pool0..3` (per-pool token counts). These mirror the GPL `fpm_priv.h` constants
sec 3.1 and must agree between host FPM driver and the Runner. `[KO]`

### 3.3 `FPM_RING_CFG_ENTRY` (RDD) `[KO]`

From `dump_RDD_FPM_RING_CFG_ENTRY` (rdpa.ko `.text` 0x83f00): `clr_tail_offset`,
`clr_tail_size`, `is_valid`, `prio`, `buffer_nums`, `buffer_size`. Per-ring FPM buffer
geometry. `[KO]`

### 3.4 Feed / recycle rings `[KO]+[SDK]`

Host posts free FPM buffers via the **feed ring** so the Runner can refill RX; spent buffers
return on the **recycle ring**. Driver entry points confirmed in `rdpa.ko` `[KO]`:
`rdpa_feed_ring_refill_kick`, `rdpa_get_feed_ring_size`, `_rdpa_recycle_thread_handler`,
`rdp_cpu_feed_ring.c` (baked source path). The recycle ring has shadow rd/wr idx, a next-ptr
table, a stack and an SRAM PD FIFO (all `CPU_RECYCLE_*` RDD tables). `[KO]`

### 3.5 Reason -> CPU-queue table (the trap config) `[KO]`

To trap traffic to the host, program the reason table. Functions in `rdpa.ko` `[KO]`:
`cpu_reason_cfg_rdd_ex`, `rdd_cpu_reason_per_port_set`, `rdd_cpu_reason_to_cpu_rx_meter`,
`rdd_ag_cpu_rx_cpu_reason_to_tc_set[_core]`; RDD tables `CPU_REASON_TO_TC`,
`CPU_REASON_TO_METER_TABLE`. RXQ config: `cpu_rxq_cfg_params_init_ex`,
`cpu_rxq_cfg_size_validate_ex`, `cpu_attr_rxq_cfg_write`, `cpu_rdd_rxq_idx_get`. For a
"dumb-pipe" v1, set every reason (and `per_port`) to one (or per-port) CPU RX queue with no
flow/ucast lookup => everything traps to the host. `[KO][INFER]`

---

## 4. SLOW-PATH BRING-UP CHECKLIST (init order)

Reconstructed from rdpa.ko baked source paths + symbols `[KO]` and the GPL FPM/enet
drivers `[SDK]`. Top-level driver: `_data_path_init` (rdpa.ko, from
`rdp_subsystem/data_path_init.c`).

1. **Map register windows.** ioremap the rdpa window `0x82000000`/`0x00caf004`; derive block
   bases from sec 1.1 (PSRAM, RNR_*, FPM, SBPM, BBH_RX/TX, DSPTCHR, QM, DQM, UBUS_SLV, DMA,
   UNIMAC_RDP). `[KO]`
2. **UBUS / address decode.** Program UBUS_SLV (`0x828a0000`) so the Runner can reach DDR +
   register windows. `[KO][INFER]`
3. **FPM pool init (GPL).** Reserve the pool DDR region (32 MB @512 B tokens), program
   `FPM_FPM_CTL`/`FPM_POOL1_CFG*`, fill `FPM_GLOBAL_CFG` (base/token math, sec 3.2). Port the
   GPL `fpm_core.c`. `[SDK]`
4. **SBPM init.** Program SBPM (`0x828a1000`) for header/short-buffer pool (`ag_drv_sbpm_*`).
   `[KO]`
5. **Dispatcher (DSPTCHR) init.** `0x82880000` — configure ingress queues, credits, RNR
   pool sizes/limits (`DSPTCHR_POOL_SIZES_RNR_POOL_*`, `ag_drv_dsptchr_*`), and the
   queue->core grouping (`DSPTCHR_QUEUE_MAPPING_*`). `[KO]`
6. **Per-port BBH RX/TX DMA.** For each active port: set `BBH_RX[port]` BBCFG(+0x0),
   DISPVIQ(+0x4), FLOWCTRL(+0x34), then ENABLE(+0x3c); configure the matching `BBH_TX`
   (`*_Q2RNR`, `*_RNRCFG`) and the DMA/SDMA engines (`DMA_ADDRS` 3x). `[KO]`
7. **Load Runner microcode (BLOCKED by license).** For each of 8 cores: block-write
   `fw_binary_n` (32 KB) into `RNR_INST[n]` and `fw_predict_n` (1 KB) into `RNR_PRED[n]`
   via the `RNR_INST_MEM_ENTRY`/`RNR_PRED_MEM_ENTRY` windows; init `RNR_CNTXT`/`RNR_MEM`;
   set version words. Loaders: `drv_rnr_load_microcode/instructions/prediction`. Then enable
   cores via `RNR_REGS[n]`. `[KO]` *(microcode = Proprietary; see sec 0.)*
8. **CPU rings.** Allocate non-cacheable DDR for the **data ring**, **feed ring**, **recycle
   ring**; fill a `CPU_RING_DESCRIPTOR` (sec 2.1) per ring (depth, DDR base hi/lo, idx) into the
   `CPU_RING_DESCRIPTORS_TABLE` / `CPU_FEED_RING_*` / `CPU_RECYCLE_*` tables in PSRAM. Seed
   the feed ring with fresh FPM tokens. (`rdp_cpu_feed_ring.c`, `create_ring` in GPL
   `enet_ring.c`.) `[KO]+[SDK]`
9. **CPU-TX ring.** Set up `CPU_TX_RING_DESCRIPTOR_TABLE` + `CPU_TX_RING_INDICES_VALUES_TABLE`
   (`_rdp_cpu_tx_ring_indices_alloc`); the DQM (`0x82c80034`) carries the CPU-TX doorbell/
   indices. `[KO]`
10. **Reason->queue table (trap-everything).** Program `CPU_REASON_TO_TC` + per-port reason so
    all ingress traps to the chosen CPU RX queue(s) with no flow lookup (sec 3.5). `[KO]`
11. **IRQ -> NAPI.** Wire GIC SPIs: `queue0..31` = SPI 75..106, `fpm` = SPI 107 `[FDT]`. Each
    data-ring queue IRQ schedules a NAPI poll; the FPM IRQ drives refill. `[FDT]`
12. **Enable MACs / link (control plane, GPL/mainline).** UNIMAC/XPORT/XLMAC + MDIO + PHY via
    `mdio-bcm-unimac` + b53/bcm_sf2 (4916 variant) + phylink. `[SDK]`

---

## 5. OPEN-DRIVER DATAPATH PLAN (v1 CPU-forwarded)

### 5.1 Structs (host side)
```c
struct xrdp_ring {                  /* one per CPU RX queue */
    void        *base;              /* non-cacheable DDR, CPU_RX_DESCRIPTOR[] */
    dma_addr_t   base_phys;         /* handed to Runner via CPU_RING_DESCRIPTOR */
    u32          ndesc;             /* number_of_entries */
    u32          head;              /* host read_idx (consumer) */
    int          irq;              /* GIC SPI 75..106 */
    struct napi_struct napi;
};
struct xrdp_feed_ring  { void *base; dma_addr_t base_phys; u32 ndesc, wr; };
struct xrdp_recycle_ring { void *base; dma_addr_t base_phys; u32 ndesc, rd; };
/* FPM token helpers (port of GPL fpm_core.c) */
static inline u32  fpm_alloc_token(int size);          /* read POOL_ALLOC_DEALLOC */
static inline void fpm_free_token(u32 token);          /* write POOL_ALLOC_DEALLOC */
static inline void *fpm_token_to_buf(u32 tok);         /* pbase + idx*chunk */
```

### 5.2 RX NAPI poll loop (ownership polling, sec 2.4)
```
napi_poll(budget):
  for n in 0..budget:
    d = ring->base + ring->head*sizeof(CPU_RX_DESCRIPTOR)
    w2 = swap4bytes(d->word2)
    if !(w2 & OWNERSHIP_HOST_BIT):  break          # MSB of word2; nothing new
    len   = field(d, packet_length)                # from PROCESSING_RX / CPU_RX layout
    port  = field(d, ingress_vport_or_flow)        # -> netdev
    off   = field(d, payload_offset_sop)
    buf   = phys_to_virt(d->host_buffer_data_pointer)   # or fpm_token_to_buf()
    dma_inv(buf, len); skb = build_skb(buf+off, len); netif_receive_skb(skb)
    newtok = fpm_alloc_token(buf_size)             # refill
    feed_ring_post(newtok)                          # post on FEED ring
    d->word2 = 0; ring->head = (ring->head+1) % ring->ndesc   # release slot
  if processed < budget: napi_complete(); enable_irq()
  rdpa_feed_ring_refill_kick()                      # nudge Runner
```
RX has **no MMIO doorbell**: the Runner sets ownership=HOST in DDR; the queue IRQ only wakes
NAPI. Re-arm = post a fresh FPM buffer on the feed ring + clear the slot. `[SDK sec 2.4][KO sec 3.4]`

### 5.3 TX path (sec 2.2)
```
xmit(skb, port):
  tok = fpm_alloc_token(skb->len)        # or abs mode with skb phys
  copy/map skb payload into fpm_token_to_buf(tok)
  build RING_CPU_TX_DESCRIPTOR (big-endian):
     is_egress=1, packet_length=len, is_emac=(port is UNIMAC),
     wan_flow_source_port_union=egress_port, abs=0, pkt_buf_ptr_low_or_fpm_bn0=tok
  write desc into CPU_TX ring slot at write_idx; advance write_idx (DQM doorbell)
```
TX **does** use an MMIO write (CPU-TX ring indices / DQM). The Runner consumes the descriptor
and programs the egress BBH/MAC DMA. GPL helper to model: `rdpa_cpu_send_sysb_fpm`. `[KO]`

### 5.4 What's reusable vs new
- **Reuse (mainline):** MDIO (`mdio-bcm-unimac`), switch (`b53`/`bcm_sf2` 4916 variant),
  phylink. **Reuse (port GPL SDK):** FPM token driver (`fpm_core.c`), enet ring caller
  (`enet_ring.c`/`runner_*.c`), `runner_get_pkt_from_ring`/`create_ring` shape.
- **New GPL (RE-derived from rdpa.ko):** the XRDP register pokes (sec 1), the ring/desc bitfield
  packing (sec 2), FPM_GLOBAL_CFG/dispatcher/BBH init, reason-table config, the microcode loader.
- **Blob:** the Runner microcode (Proprietary; sec 0).

---

## 6. STATUS: pinned vs inferred vs UNKNOWN

**PINNED from binary `[KO]`:** all block bases/strides (sec 1.1); register-descriptor struct +
key FPM/BBH/RNR offsets (sec 1.2); `CPU_RING_DESCRIPTOR` full layout (sec 2.1); `RING_CPU_TX_DESCRIPTOR`
full bitfield layout (sec 2.2); `PROCESSING_RX_DESCRIPTOR` field set (sec 2.3); `FPM_GLOBAL_CFG` /
`FPM_RING_CFG_ENTRY` field sets (sec 3.2/3.3); ring/feed/recycle/reason RDD tables + driver entry
points (sec 2/sec 3); the 8x32 KB inst + 8x1 KB pred microcode geometry (sec 1.1).

**PINNED from GPL source `[SDK]`:** FPM token bitfields + alloc(read)/free(write) + token->buf
math + pool sizes (sec 3.1); RX ownership-bit poll model + endian swap + re-arm (sec 2.4); host ring
caller API (`runner_get_pkt_from_ring`, `create_ring`, `rdpa_cpu_ring_rest_desc`,
`rdpa_cpu_ring_not_empty`).

**INFERRED:** UBUS/address-decode + dispatcher exact init order (step 2/5); "trap-everything"
reason-table contents (step 10); TX DQM doorbell exact register (the DQM base is pinned at
`0x82c80034`, the doorbell field within it is not yet decoded).

**STILL UNKNOWN / needs deeper RE:**
1. **Host `CPU_RX_DESCRIPTOR` exact word0/1/3 bitfields on 4916** — `rdp_cpu_ring_defs.h` is
   absent from the 4916 GPL SDK; only the ownership bit + buffer ptr are GPL-confirmed (sec 2.4).
   Decode the rest from `rdpa_cpu_rx_pd_get` in `rdpa.ko` (or map onto `PROCESSING_RX_DESCRIPTOR`
   sec 2.3). *Resolves: exact RX field offsets for the poll loop.*
2. **CPU-TX doorbell register** within DQM (`0x82c80034`) — decode `ag_drv_dqm_*` /
   `_rdp_cpu_tx_ring_indices_alloc`.
3. **Per-port BBH RX/TX full init sequence** — many `*_REG` offsets are decodable (the `+0x08`
   field) but the ordered write sequence + values come from `xrdp_drv_bbh_rx_ag.c` /
   `data_path_init.c` in rdpa.ko (disasm the init functions).
4. **Dispatcher credit/VIQ values** per port/queue.
5. **Microcode load sequence details** (CNTXT/MEM seeding, core enable order) — disasm
   `drv_rnr_load_microcode`.

**Offload entry points seen in passing (NOT this pass — for the offload-ABI follow-up):**
NAT-C engine @ `0x82950000` (`NATC_TBL`/`NATC_KEY`/`NATC_DDR_CFG`), HASH @ `0x82920000`,
CNPL @ `0x82948000`, QM @ `0x82c00000`; rdpa.ko fast-path syms `_ucast_prepare_rdd_*`,
`_natc_tbl_*`; cmdlist/XPE in `cmdlist.ko` (see `rdpa-offload-controlplane.md`).

---

## SOURCE INDEX
- **rdpa.ko `[KO]`** (staged stock module, license=Proprietary): `.data` `*_ADDRS` arrays (block
  bases, via `.rela.data`); `.rodata` `*_REG` 32-B descriptors (`+0x08`=reg offset); disasm of
  auto-gen pretty-printers `dump_RDD_CPU_RING_DESCRIPTOR` (.text 0x7df20),
  `dump_RDD_RING_CPU_TX_DESCRIPTOR` (0x81920), `dump_RDD_PROCESSING_RX_DESCRIPTOR` (0x84a90),
  `dump_RDD_FPM_GLOBAL_CFG` (0x7ecc0), `dump_RDD_FPM_RING_CFG_ENTRY` (0x83f00); strings in
  `.rodata.str1.8`; symbols `drv_rnr_load_*`, `rdpa_feed_ring_*`, `cpu_reason_cfg_rdd_ex`,
  `cpu_rxq_cfg_*`, `ag_drv_*`.
- **4916 GPL SDK `[SDK]`** (asuswrt-merlin.ng src-rt-5.04behnd.4916): `bcmdrivers/opensource/char/fpm/
  impl1/{fpm_core.c,fpm_priv.h,fpm.h}` (FPM token + alloc/free, GPL);
  `bcmdrivers/opensource/net/enet/impl7/enet_ring.c` + `runner_*.c` (ring caller, GPL);
  `bcmdrivers/opensource/include/bcm963xx/runner_wfd_inline.h` (`CPU_RX_DESCRIPTOR` ownership
  poll + swap4bytes, GPL).
- **416L05 GPL SDK `[SDK-OLD]`**: `rdp_cpu_ring_defs.h`/`rdp_cpu_ring_inline.h` (63138
  `CPU_RX_DESCRIPTOR` word0..3 template).
- **FDT `[FDT]`**: `bcm4916-regmap.md` — rdpa window `0x82000000`, queue SPI 75..106, fpm SPI 107.
