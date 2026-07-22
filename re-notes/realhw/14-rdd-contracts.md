# BCM6813 XRDP — RDD structures & datapath contracts (TX + RX)

Synthesis of four parallel SDK traces (2026-07-22). Purpose: pin the structures and
flows the open driver must reproduce, so the remaining work is implementation rather
than guessing.

**Confidence:** `[C]` confirmed from readable source · `[T]` from the stock CFE
register write-trace · `[I]` inferred · `[BLOB]` only inside `rdpa.ko`/microcode.

---

## 0. Source-tree facts that change everything

`$SDK = .../src-rt-5.04behnd.4916`

| Tree | Status |
|---|---|
| `$SDK/rdp/projects/BCM6813/target/**` | **dangling symlinks** — ignore |
| **`$SDK/rdp/projects/BCM6813/drivers/rdd/auto/**`** | **REAL — the authoritative non-FPI map** |
| **`$SDK/rdp/projects/BCM6813/firmware_bin/rdd_runner_labels.h`** | **REAL — production microcode entry points** |
| **`$SDK/rdp/projects/BCM6813/firmware_bin/runner_fw_0..7.c`** | **REAL — the Runner microcode as GPL-licensed C arrays** |
| `$SDK/rdp/projects/XRDP_CFE2/drivers/rdp_subsystem/BCM6813/data_path_init_basic_data.h` | literal {addr,value} trace of a working 6813 bring-up |
| `$SDK/rdp/projects/BCM6813_FPI/**` | **WRONG for this chip — 60/104 core-2 addresses differ** |

⚠ **Never use the FPI map.** Known traps it caused here:
`QUEUE_THRESHOLD_VECTOR` real **0x35c0** (FPI 0x3540 = `VPORT_TO_LOOKUP_PORT_MAPPING_TABLE`);
`FW_ERROR_VECTOR_TABLE` real **0x3760** (FPI 0x35a0 = `FPM_GLOBAL_CFG`);
`CPU_TX_RING_DESCRIPTOR_TABLE` real **0x33e0** (FPI 0x3360); `CPU_TX_SYNC_FIFO_TABLE` real **0x3780** (FPI 0x3580).

⚠ **`rdd_runner_labels.h` has two blocks**: byte addresses then instruction-word
addresses. `image_2_c_cpu_tx_wakeup_request` = `0x854` byte = **`0x215` word**.

★ **The microcode is shipped as GPL C arrays** (`runner_fw_0..7.c`, 8192 instr words
each, `DUAL/GPL` licence tag). This contradicts the long-standing assumption that the
4916 GPL SDK lacks the microcode — it has direct bearing on whether a fully-open
*and* redistributable datapath is possible. Worth diffing against what stock loads.

---

## 1. Core / image map `[C]`

core *N* runs image *N* (1:1, 8 cores). `rdd_map_auto.c:36`

| Core | Image | Roles |
|---|---|---|
| **2** | image_2 | processing2, **cpu_tx** |
| **3** | image_3 | processing3, **cpu_rx**, spu_request |
| 6 / 7 | image_6/7 | us_tm / ds_tm |

image_2 threads: general_timer 0, cpu_recycle 1, **CPU_TX_0 = 6**, **CPU_TX_1 = 7**, processing 8..15.
image_3: **CPU_RX = thread 1**, entry `image_3_c_cpu_rx_wakeup_request` = 0x5890 byte / **0x1624** word.

---

## 2. CPU_TX egress contract

### 2.1 Ring descriptor — `CPU_TX_RING_DESCRIPTOR_TABLE` @ core-2 **0x33e0**, 2 × 16 B
Entry 0 = high prio, entry 1 = low prio. `CPU_RING_DESCRIPTOR_STRUCT` (BE):

| Byte | Field |
|---|---|
| 0–1 | `size_of_entry`[31:27] , `number_of_entries`[26:16] |
| 2–3 | `interrupt_id` |
| 4–5 | `drop_counter` |
| 6–7 | `write_idx` |
| 8–11 | **`base_addr_low`** |
| 12–13 | `read_idx` |
| 15 | **`base_addr_high`** |

`base_addr_{high,low}` = plain **40-bit DDR physical** (not a token).
**`number_of_entries` is scaled: entries ≫ 5** (`CPU_RING_SIZE_32_RESOLUTION`).
stock `0x80400000` → 16 B entries, 64 → **64≪5 = 2048** = `RDPA_CPU_TX_RING_SIZE`.
ours `0x80080000` → 8 → 8≪5 = **256**, consistent with our `TX_RING_DEPTH 256`. ✔

### 2.2 Descriptor — `RING_CPU_TX_DESCRIPTOR_STRUCT`, 16 B BE
W0: `is_egress`[31] · `first_level_q`[30:22] (**the QM queue**) · `packet_length`[21:8] · `sk_buf_ptr_high`[7:0]
W1: `sk_buf_ptr_low` / `data_1588`[31:14] + `bufmng_cnt_id`[13:9]
W2: `color`[31] `do_not_recycle`[30] `flag_1588`[29] `is_emac`[28] **`is_vport`[27]** **`flow_or_port_id`[26:20]** `fpm_fallback`[19] `sbpm_copy`[18] `tgtmem/l3pkt`[17] **`abs`**[16] · `egress_dont_drop`/`ssid`/`lag_index`[15:8] · `pkt_buf_ptr_high`[7:0]
W3: `pkt_buf_ptr_low` **or** `fpm_sop`[29:20] + `fpm_bn0`[19:0]

**GPL vs proprietary split:** `rdd_cpu_tx_set_ring_descriptor()` (`firmware/cpu/rdd/xrdp/rdd_cpu_tx.h:91`) sets only
`abs, fpm_fallback, sbpm_copy, packet_length, target_mem_0/l3_packet` and the buffer fields.
**Everything that decides *where the packet goes* — `is_egress`, `first_level_q`, `is_vport`,
`flow_or_port_id`, `color`, `do_not_recycle`, `is_emac` — is set by the closed `rdpa_cpu_tx`** `[C]`.
Semantics recoverable from GPL `rdpa_cpu_tx_info_t` (`rdpa_cpu_basic.h:272`).

### 2.3 Egress-target resolution chain `[C]`
```
first_level_q ──► QM_QUEUE_TO_TX_FLOW_TABLE_PTR_TABLE[q]   core2 0x3600 (160 × BE u16)
                     value = address of the tx-flow table  = 0x0fc0
              ──► VPORT_TX_FLOW_TABLE[vport]               core2 0x0fc0 (64 × 1 B)
                     TX_FLOW_ENTRY: valid[7] | rsv[6:5] | qos_table_ptr[4:0]
```
`rdd_qm_queue_to_tx_flow_tbl_cfg()` blanket-fills all 160 pointers on first call.

### 2.4 Indices & doorbell `[C]`
Indices live in **SRAM**, not in the ring descriptor:
`CPU_TX_RING_INDICES_VALUES_TABLE` @ core-2 **0x29c8**, 2 × 4 B (`read_idx` u16 @+0, `write_idx` u16 @+2, BE).
Doorbell = write thread number (6 or 7) to core-2 `RNR_REGS + 0x04` (`THREAD_NUM[3:0]`).

### 2.5 Dispatcher credit `[C]`
`DISPATCHER_CREDIT_DESCRIPTOR_STRUCT` 12 B: `reserved0` u32 @0 (HW deposits credit), **`total`**[11:0] @+6, **`used`**[11:0] @+10.

| VIQ | table | core-2 addr | crdt_cfg |
|---|---|---|---|
| **13** `CPU_TX_EGRESS` | `CPU_TX_EGRESS_DISPATCHER_CREDIT_TABLE` | 0x29d0 | `(0x29d0>>3)|(6<<12)` = 0x653A, bb_id 2 → **0x653a0002** ✔ verified |
| **14** `CPU_TX_FORWARD` | `CPU_TX_INGRESS_DISPATCHER_CREDIT_TABLE` | 0x2b70 | → **0x656e0002** `[I]` — **not yet configured by us** |

### 2.6 Other image_2 CPU_TX state
`CPU_TX_SYNC_FIFO_TABLE` 0x3780 (2 × 8 B: `write_ptr`,`read_ptr`,`fifo`,`rsv` — **live pointers, zero them, never seed**) ·
`RING_CPU_TX_DESCRIPTOR_DATA_TABLE` 0x3380 · `CPU_TX_RNR_CTR_REPLY_TABLE` 0x2b80 ·
`CPU_TX_0_STACK` 0x3200 / `CPU_TX_1_STACK` 0x3400 · `CPU_TX_SCRATCHPAD` 0x0 ·
`FPM_GLOBAL_CFG` **0x35a0** (must be programmed if `abs=0`) · `FPM_POOL_NUMBER_MAPPING_TABLE` 0x29e0.

⚠ **`CPU_TX_DBG_CNTRS` is NOT instantiated in the 6813 image** — the struct exists
(`sbpm_no_next, sbpm_no_first, tx_flow_disable, no_fpm, task_exit, sync_wait,
no_dispatcher_scheduler, recycle_fifo_full, drop_pkt, task_start, no_fwd`) but there is
no address for it. **That is why a CPU_TX drop is silent** — expected, not our bug.

### 2.7 Per-thread context — the decisive mechanism `[C]`
`RNR_CNTXT[core] + (thread*32 + reg)*4`; core-2 = 0x82758000, thread 6 @0x82758300, thread 7 @0x82758380.
Stock seeds ~20 registers with every table pointer the task needs. Captured verbatim
(now in the driver): r3=0x33e0, r7=0x35c0, r8/r25=0x3788, r22=0x2b80/0x2ba0, r23=0x3780,
r28=0x29c8, r30=0x32c0/0x34c0, r0=0x00600221, r31=1.
**Safe to replay** — all RDD/SRAM offsets and scalars, no per-boot DDR pointers.

---

## 3. RX contract — ★our model is wrong

```
XLMAC → XLIF0 ch0 → BBH_RX 5 (BB_ID 41) → DISPATCHER VIQ 5
   → load-balancer picks a FREE TASK from the runner-group owning VIQ 5
   → dispatcher DMAs the PD into that core's DMEM at PD_DSPTCH_ADD[core] + task*offset
   → PROCESSING task (image_N thread 8..15) parses/classifies
   → forward (QM/BBH_TX)  OR  trap: build CPU_RX_DESCRIPTOR + wake CPU_RX
   → CPU_RX task core 3 / thread 1 → host ring
```

★ **There is no "CPU_RX group".** `DISP_REOR_VIQ` has no CPU_RX entry; CPU_RX is woken
*by a processing task* (`THREAD_WAKEUP_REQUEST(x) = (x<<4)+1`), not by the dispatcher.
**Routing a port VIQ straight at the CPU_RX thread — which our driver does — is wrong**:
thread 1 of image_3 starts at `c_cpu_rx_wakeup_request` and expects a CPU_RX trap
context, not a raw BBH PD. It won't consume it, dispatcher credits are never returned,
the VIQ fills and `BBH_RX PM_COUNTERS_DISPCONG` climbs while `INPKT` stays 0.

Two viable models:
* **(A) faithful** — VIQ5 → group whose task mask = processing tasks; requires the
  classification/trap tables too.
* **(B) minimal first-light** — VIQ5 → group with ONE ingress task. CFE uses
  `direct_processing`; production equivalent is **core 5 / thread 0
  `image_5_direct_flow_wakeup_request`**, PD landing zone
  `IMAGE_5_DIRECT_FLOW_PD_TABLE_ADDRESS = 0x2570` `[I]`.

**Dispatcher invariants** (missing any ⇒ silent stall):
VIQ index **must equal** BBH id · VIQ in `MASK_MSK_Q[grp]` (+0x600) · every task of the
group in `MASK_MSK_TSK_255_0[grp]` (+0x500, stride 0x20; task = core*16+thread) · each
task in `TSK_TO_RG_MAPPING` (+0x900) · counted in `RG_AVLABL_TSK_0_3` (+0x980) ·
`PD_DSPTCH_ADD[core]` (+0x480) programmed · `Q_INGRS_COHRENCY[q]` (+0x380) = **0x400**
(`CHRNCY_EN`, required for BBH queues) · `MASK_DLY_Q` (+0x620) bit set (BBH VIQs are
delayed) · `VQ_EN` (+0x004) set **last** · `REORDR_CFG` (+0x000) = **0x11** written last.

**BBH_RX per-port checklist** (base + bbh*0x400): SDMAADDR +0x1c = 0x1414 (chunk 20) ·
SDMACFG +0x20 = 0x0c0c (12) · BBCFG +0x00 = 0x00381215 (sbpm 56, disp 18, sdma 21) ·
DISPVIQ +0x04 = 0x0505 · MINPKT0 +0x24 = 0x40404040 · MAXPKT0/1 +0x28/+0x2c ·
SOPOFFSET +0x30 = 0 · **PERFLOWTH +0x44 = 0xff** (⚠ below `PER_FLOW_TH_MIN_VAL` 0x20
makes `drv_bbh_rx_configuration_set` return `BDMF_ERR_RANGE` **before** writing
flow-ctrl and SBPMCFG — a bad value silently skips the tail of the config) ·
MINPKTSEL/MAXPKTSEL/PERFLOWSETS = 0 · MACMODE +0x5c = 0 · **ENABLE +0x3c = 0x3 LAST**.

**Classification tables** (image_3 DMEM): `RX_FLOW_TABLE` 0x0a00 (202 × 2 B:
`cntr_id:8, flow_dest:1, virtual_port:5`) — index for a BBH source =
`RDD_WAN_FLOW_NUM(128) + BB_ID` ⇒ **eth0 = 169** `[I]` · `VPORT_CFG_TABLE` 0x2780 ·
`CPU_REASON_TO_TC` 0x3960 · `TC_TO_CPU_RXQ` 0x39a0 · `EXC_TC_TO_CPU_RXQ` 0x39e0 ·
`VPORT_TO_CPU_OBJ` 0x3bc0 · `CPU_RXQ_DATA_BUF_TYPE_TABLE` 0x3a40 ·
**`CPU_REDIRECT_MODE` 0x3a2e** (global "everything to CPU" — attractive for bring-up).

**CPU_RX ring**: `CPU_RING_DESCRIPTORS_TABLE` @ image3 **0x3000**, 24 × 16 B, same
`CPU_RING_DESCRIPTOR_STRUCT`. ⚠ **CPU_RX indices are BYTE-scaled** (wrap at
`number_of_entries << 5`), unlike CPU_TX which uses entry indices.

---

## 4. DDR vs SRAM

**All 945 BCM6813 RDD tables live in core-local SRAM** — zero use DDR/PSRAM segments.
PSRAM holds no RDD tables (packet scratch only). The only declared DDR table is IPTV
context. So a core-SRAM diff *does* cover the RDD table space.

What **is** in DDR: the ring *buffers* themselves (host `dma_alloc_coherent`), reached
via the 40-bit base in the ring descriptor; FPM packet pool (`bufmem`, 128 MB) and
`rnrmem` (12 MB) reserved regions; and per-boot write-back addresses.

⚠ **Never replay these from a stock dump** (per-boot physical addresses):
core2 0x33e0+8/+15 (TX ring bases) · 0x3170/0x31f0 (recycle) · 0xf98, 0x2368
(interrupt-id write-back) · core3 0x3000 (24 RX ring bases), 0xf70/0x2568 (feed) ·
`GDX_PARAMS_TABLE` · `RNR_REGS+0x40 DMA_BASE` · `BBH_TX +0x2c/+0x34` · QM/FPM/NATC bases.

`RNR_REGS CFG_DDR_CFG` (+0x40) is **only** the FPM-token→DDR expansion base — not a
general DDR window, and nothing to do with reaching the CPU_TX ring.

---

## 5. Ordering constraints `[C]`

`_data_path_init`: ubus → bbh profiles → **qm_init** → **runner_init** → bbh_rx_init →
bbh_tx_init → sbpm_init → dma_sdma → dispatcher_reorder → **rdp_block_enable**.

`runner_init`: `zero_data_mem|zero_context_mem` → **poll both DONE** → **load microcode**
→ `SCH_CFG` → DMA/PSRAM base → **`rdd_data_structures_init`** → freq → quad cfg → **RNR enable last**.
Context seeding must land *after* zero-context completes and *before* RNR enable —
and `image_N_context_set()` **read-modify-writes** context the ACE compiler may have
pre-seeded, so seeding before microcode load is clobbered.

`rdp_block_enable`: poll SBPM `init_free_list.rdy` → XLIF TX cfg → **XLIF RX IF_DIS=0**
→ **BBH_RX ENABLE=0x3** → **DSPTCHR REORDR_CFG=0x11** → QM `enable_ctrl` 0x307 →
UBUS_MSTR EN → **RNR enable last**.

---

## 6. Blob-only (valuable negatives)

`rdd_init.c` / `rdd_data_structures_init()` for 6813 · `rdd_cpu_tx.c` / `rdd_cpu_rx.c` ·
`rdd_ring_init()` XRDP impl (legacy RDP version at `firmware/common/rdd/rdp/rdd_cpu_ring.c:248`
is field-for-field the same contract → reimplementable) · `image_2_context_set()` ·
which forwarding bits `rdpa_cpu_tx` sets · the classification/trap decision logic ·
production `data_path_init.c` for 6813 · `data_path_natc_init()`.

---

## 7. Highest-value live measurements still outstanding

1. **DSPTCHR `PD_DSPTCH_ADD[0..7]` (+0x480), `MASK_MSK_TSK_255_0[0..7]` (+0x500 stride 0x20),
   `MASK_MSK_Q[0..7]` (+0x600) on stock** — settles which group owns VIQ 5, which tasks are
   in it, and where the PD lands. Single highest-value read for RX.
2. `RX_FLOW_TABLE` (core3 0x0a00, 202×2 B) on stock — find entries with `virtual_port != 31`
   to pin the per-port rx_flow index.
3. BBH5 `SBPMCFG` — AG says +0x60, CFE trace writes 0xF at +0x64. Read both.
4. `CPU_TX_INGRESS_DISPATCHER_CREDIT_TABLE` (0x2b70) + VIQ 14 config on stock.
