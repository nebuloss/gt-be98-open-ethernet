# Route A — CPU_TX → QM/TM → BBH_TX LAN egress bring-up spec (BCM6813 / 96813GW)

RE'd 2026-06-24 (agent a8cc94cdcf545b186). Register offsets verified against
`src-rt-5.04behnd.4916/rdp/drivers/rdp_subsystem/BCM6813/autogen/XRDP_AG.h`; behavioural order
against CFE2 `…/BCM6813/data_path_init.c`; QM logic against gen-1
`src-rt-5.02axhnd/…/xrdp/rdp_drv_qm.c`. Items marked **★UNKNOWN** live only in proprietary
`rdpa.ko` and must be read from live silicon (devmem on the stock slot2, same technique that pinned
DSPTCHR VIQ 13) or dumped from the binary.

## Why this is needed
The stock **image_2 CPU_TX thread (core2/thr6) does NOT push directly to BBH_TX.** It hands PDs to
the egress dispatcher → a **TM/QM Runner core** (image_6 US_TM @0x2600 / image_7 DS_TM @0x3600)
which drains a QM queue into BBH_TX. The direct-to-BBH model (BBH_TX_RING_TABLE + BB_DESTINATION)
exists only in CFE2/image_0, which image_2 lacks. So an open slow-path TX with the stock blob MUST
bring up QM + a TM-core egress queue. (This is the confirmed cause of "read_idx freezes at 3,
sync_fifo stays 0".)

BCM6813 is `CHIP_VER=RDP_GEN_61` (gen-2); `XRDP_BBH_PER_LAN_PORT` is NOT defined.

---

## (A) QM init — making `QM_ENABLE_CTRL=0x307` safe

**Root cause of the earlier hang:** CFE2 `qm_init()` is empty yet 0x307 works there because QM
SRAM/queue/FPM-prefetch state is at clean reset. On live stock-Linux silicon that state is dirty,
so enabling `fpm_prefetch(bit0)` makes QM read garbage. Fix = fully (re)init QM before enable.

QM offsets (within QM base 0x82c00000), verified XRDP_AG.h:

| Reg | Off | line |
|---|---|---|
| GLOBAL_CFG_QM_ENABLE_CTRL | 0x000 | 19024 |
| GLOBAL_CFG_QM_SW_RST_CTRL | 0x004 | 19027 |
| GLOBAL_CFG_FPM_CONTROL | 0x00c | 19033 |
| GLOBAL_CFG_AGGREGATION_CTRL | 0x02c | 19057 |
| GLOBAL_CFG_FPM_BASE_ADDR | 0x034 | 19063 |
| GLOBAL_CFG_FPM_COHERENT_BASE_ADDR | 0x038 | 19066 |
| GLOBAL_CFG_DDR_SOP_OFFSET | 0x03c | 19069 |
| GLOBAL_CFG_MEM_AUTO_INIT | 0x138 | 19115 |
| GLOBAL_CFG_MEM_AUTO_INIT_STS | 0x13c | 19118 |
| FPM_POOLS_THR | 0x200 | 19133 |
| FPM_USR_GRP_LOWER_THR (per-UG) | 0x280 | 19138 |
| FPM_USR_GRP_MID_THR (per-UG) | 0x284 | 19143 |
| FPM_USR_GRP_HIGHER_THR (per-UG) | 0x288 | 19148 |
| RUNNER_GRP_RNR_CONFIG (per-grp) | 0x300 | 19158 |
| RUNNER_GRP_QUEUE_CONFIG (per-grp) | 0x304 | 19163 |
| RUNNER_GRP_PDFIFO_CONFIG (per-grp) | 0x308 | 19168 |
| RUNNER_GRP_UPDATE_FIFO_CONFIG (per-grp) | 0x30c | 19173 |

Pre-enable order:
1. **QM SRAM auto-init (gen-2, mandatory):** write `MEM_AUTO_INIT(0x138)` MEM_INIT_EN[0]=1
   (MEM_SEL_INIT[10:8]=0 → all banks); poll `MEM_AUTO_INIT_STS(0x13c)` done. **★UNKNOWN exact
   done bit/value (rdpa.ko only) — prime suspect for the hang.**
2. FPM + SBPM already initialized (FPM init_mem, SBPM init_free_list polled ready) BEFORE QM.
3. `FPM_BASE_ADDR(0x034)` = 256-B-resolution phys of the RDP DDR pool (`(hi<<24)|(lo>>8)`).
4. `FPM_CONTROL(0x00c)`: prefetch min-pool-size + buf-size; leave BP enables at reset unless tuning.
5. `DDR_SOP_OFFSET(0x03c)` = 18 (gen-1 value).
6. `FPM_USR_GRP_{LOWER,MID,HIGHER}_THR` per UG — gen-1: UG0=20K, UG1=40K, UG2=0, UG3=64K-1.
7. Clear all queue contexts to drop-all (WRED 15) + clear counters (gen-1 drv_qm_init loop).
8. ≥1 RUNNER_GRP (section C) pointing at the TM egress task, covering the target queue.
9. **Then** `QM_ENABLE_CTRL(0x000)=0x307` (fpm_prefetch[0] reorder_credit[1] dqm_pop[2]
   rmt_fixed_arb[8] dqm_push_fixed_arb[9]).

Minimal drain subset likely `fpm_prefetch|dqm_pop|reorder_credit`(=0x7) but **★untested** — silicon-tune.

---

## (B) BBH_TX LAN runner/QM-fed register sequence

Offsets within the BBH_TX instance (LAN base 0x82890000, instance stride 0x2000, **LAN1 = id 1**).
COMMON block identical across instances; both LAN_CONFIGURATIONS (single-port) and
UNIFIED_CONFIGURATIONS (pair-indexed) are written by stock.

| Field | Off | Layout / value | line |
|---|---|---|---|
| MACTYPE | 0x000 | TYPE[2:0]; GPON=1 | 20751 |
| BBCFG_1_TX (fpm/sbpm src) | 0x004 | SBPMSRC[21:16], FPMSRC[29:24] | 20754 |
| **BBCFG_2_TX (rnr src)** | 0x008 | PDRNR0SRC[5:0] PDRNR1SRC[13:8] PDRNR2SRC[21:16] PDRNR3SRC[29:24] = BB_ID of feeding TM/QM core(s) | 20757 |
| BBCFG_3_TX | 0x00c | MSGRNRSRC[5:0] STSRNRSRC[13:8] | 20760 |
| DFIFOCTRL | 0x03c | PSRAMSIZE[9:0] DDRSIZE[19:10] PSRAMBASE[29:20] REORDER_PER_Q_EN[30] | 20782 |
| **RNRCFG_1** (idx0..3 stride 8) | 0x050 | TCONTADDR[15:0] SKBADDR[31:16] | 20794 |
| **RNRCFG_2** (idx0..3 stride 8) | 0x060 | PTRADDR[15:0]=TM egress-counter-table>>3, TASK[19:16]=TM thread | 20799 |
| PERQTASK | 0x0a0 | TASK0..7 ×4 bits | 20804 |
| GPR | 0x0bc | stock writes 3 | 20816 |
| **Q2RNR** (LAN single) | 0x400 | Q0[1:0] Q1[3:2] = runner-core select | 20896 |
| QPROF (LAN single) | 0x450 | Q0[0] Q1[1] DIS0[2] DIS1[3] | 20899 |
| **QMQ** (LAN single) | 0x4b0 | Q0[0] Q1[1] — **1=QM-fed, 0=runner-fed** | 20911 |
| **Q2RNR** (unified idx) | 0x700 | Q0[1:0] Q1[3:2] | 20929 |
| QPROF (unified idx) | 0x750 | | 20939 |
| **QMQ** (unified idx) | 0x7b0 | Q0[0] Q1[1] | 20953 |

Fields the open driver currently OMITS, required for QM-fed LAN egress:
- **QMQ=1** for the target queue (both LAN 0x4b0 and unified 0x7b0 for that pair) — THE switch to
  take PDs from the QM aggregator instead of a Runner PD-FIFO. (CFE leaves all 0 = runner-fed.)
- **Q2RNR**(0x400/0x700) — TM core that services the queue.
- **RNRCFG_2**(0x060 idx0) — PTRADDR = TM egress-counter-table>>3, TASK = TM egress thread.
- **BBCFG_2**(0x008) — PDRNRnSRC = BB_ID of the TM/QM Runner core(s) (6813 = 4-source).
- **MACTYPE**(0x000)=1.

**A LAN BBH_TX exposes 8 queues = 4 pairs** (`BBH_TX_NUM_OF_LAN_QUEUES=8`).

★Correction on "BB_DESTINATION 0x60": that is the **direct-BBH (CFE2/image_0)** per-frame encoding
`BB_ID_TX_LAN + (tx_port<<6)` (BB_ID_TX_LAN=0x20 → port1=0x60). It is NOT a register we write for
the QM path; for QM egress the destination is selected by the RUNNER_GRP binding (C) + the vport
tx-flow table.

---

## (C) QM queue ↔ BBH_TX LAN binding = the QM **RUNNER_GRP** registers

(NOT a `QM_QUEUE_TO_TX_FLOW` table — that one is `#if BCM_DSL_XRDP`, a no-op on 6813.)

`ag_drv_qm_rnr_group_cfg_set(group,…)` per group:
- `RUNNER_GRP_QUEUE_CONFIG(0x304)`: START_QUEUE[8:0], END_QUEUE[24:16] — contiguous QM queue range.
- `RUNNER_GRP_RNR_CONFIG(0x300)`: RNR_BB_ID[5:0] (TM core BB-id), RNR_TASK[11:8] (thread),
  RNR_ENABLE[16].
- `RUNNER_GRP_{PDFIFO,UPDATE_FIFO}_CONFIG(0x308/0x30c)`: PD/update FIFO base+size.

Per-queue QM context (gen-1 minimal): `qm_q_context_set(Q)` = pass WRED profile (NOT init default
15=drop-all) + valid fpm_ug; `dqm_dqmol_cfgb_set(Q,1)` enables the queue.

**★UNKNOWN (rdpa.ko):** the numeric LAN1 QM queue, and RUNNER_GRP START/END/RNR_BB_ID/RNR_TASK.
Recover live: QM 0x300–0x30c per group; `IMAGE_7_DS_TM_FIRST_QUEUE_MAPPING`(RDD 0x2d1c),
`IMAGE_6_US_TM_FIRST_QUEUE_MAPPING`(RDD 0x36bc); `GENERAL_QUEUE_DYNAMIC_MNG_TABLE`.
(`MAX_TX_QUEUES=160`; `QM_QUEUE_DROP=0xFF`.)

---

## (D) CPU_TX descriptor changes (QM/TM path)

image_2 uses `RING_CPU_TX_DESCRIPTOR_STRUCT` (16B, `rdd_data_structures_auto.h:1994`):

| W.byte | field | shift,w | for QM egress |
|---|---|---|---|
| W0+0 | **is_egress** | b7,1 | **=1** (path selector) |
| W0+0 | first_level_q | b6,9 | target QM queue (from C), if explicit |
| W0+0 | packet_length | b8,14 | len |
| W2+8 | is_emac | b4,1 | 1 if LAN is EMAC |
| W2+8 | **flow_or_port_id** | b4,7 | target LAN vport/flow |
| W2+8 | **is_vport** | b3,1 | =1 |
| W2+9 | abs | b0,1 | 1=abs host / 0=FPM bn |
| W2+10 | egress_dont_drop | b1,1 | drop override |
| W3+12 | fpm_bn0/sop/pool/num | u32 | buffer fields |

GPL builder `rdd_cpu_tx_set_ring_descriptor()` (`firmware/cpu/rdd/xrdp/rdd_cpu_tx.h:89`) sets only
abs/fpm_fallback/sbpm_copy/packet_length/buffer + (cond) egress_dont_drop. It does **NOT** set
`is_egress / first_level_q / flow_or_port_id / is_vport / is_emac` — those are set by proprietary
`rdpa_cpu_tx`. **Open driver must add: is_egress=1, is_vport=1, flow_or_port_id=LAN vport, is_emac,
and either first_level_q OR rely on the microcode VPORT_TX_FLOW_TABLE resolution** (image_2 RDD
0x0fc0; entry valid[1]+qos_table_ptr[5]); the latter needs `rdd_tx_flow_enable(vport)`
(`rdd_common.c:703`); for non-PON/DSL `tx_flow = port` directly (`rdd_common.c:662`).
**★UNKNOWN (rdpa.ko): first_level_q vs vport-resolution, and the QoS-table format.**

image_2 RDD addrs: CPU_TX_RING_DESCRIPTOR_TABLE=0x33e0, CPU_TX_RING_INDICES_VALUES=0x29c8,
CPU_TX_EGRESS_DISPATCHER_CREDIT=0x29d0, CPU_TX_SYNC_FIFO=0x3780; threads CPU_TX_0=6, _1=7; core2.

---

## (E) DSPTCHR / TM wakeup

Our existing CPU_TX egress VIQ 13 (crdt_cfg=0x653A0002, MASK_DLY_Q bit13, EGRS_DLY_QM_CRDT=8) =
CFE `DISP_REOR_VIQ_CPU_TX_EGRESS` (data_path_init.c:763) — it carries the PD CPU_TX→reorder. KEEP it.

**The TM core is woken by the QM RUNNER_GRP, not a DSPTCHR VIQ:** once `QM_ENABLE.dqm_pop` is set and
a RUNNER_GRP (RNR_ENABLE=1) binds the queue range to the TM core/task, QM HW issues the
wakeup/credit to that task via the UPDATE_FIFO when the queue is non-empty. So **no extra DSPTCHR
VIQ for TM egress.** Reorder enable (`dsptchr_reordr_cfg={1,1,0,0,0}`) before QM enable.
**★UNKNOWN: which TM image/thread owns LAN egress on 96813GW — read RUNNER_GRP_RNR_CONFIG live.**

---

## (F) Init order (CFE2 `_data_path_init` + folded-in QM steps)
1. ubus windows → 2. bbh_rx/tx profiles → **3. QM init (section A)** → 4. runner_init (ucode, RDD,
RNR DMA; RNR not enabled) → 5. bbh_rx_init → **6. bbh_tx_init(LAN, LAN1) incl QMQ=1/RNRCFG_2** →
7. sbpm_init → 8. dma/sdma → 9. dispatcher_reorder (VIQs incl CPU_TX egress) → **10. rdp_block_enable
in order:** poll SBPM ready → XLIF tx/rx en → BBH_RX en → DSPTCHR reorder en → **QM_ENABLE=0x307** →
UBUS master en → **RNR enable LAST**.

Rule: FPM+SBPM ready and QM SRAM/FPM/UG/RUNNER_GRP done BEFORE QM enable; QM enable BEFORE RNR; RNR last.

---

## (G) Open items → resolve by live oracle capture on stock slot2
1. **★QM MEM_AUTO_INIT(0x138) done-poll** bit/value — prime hang suspect.
2. **★LAN1 QM queue number + RUNNER_GRP values** (0x300–0x30c; FIRST_QUEUE_MAPPING RDD 0x2d1c/0x36bc).
3. **★Which TM image/thread** owns LAN egress (RNRCFG_2.TASK / RUNNER_GRP.RNR_TASK).
4. **★CPU_TX is_egress/first_level_q/flow_or_port_id logic** + QoS-table format.
5. Minimal QM enable mask (<0x307?).
6. FPM base / UG thresholds / ddr_sop numeric values (gen-1 starting points above).
7. DDR vs FPM residency for the QM-accepted PD (abs=0 vs 1).

**Implementation approach:** code the verified-offset structure now; expose the ★UNKNOWNs as module
params (like `qm_enable`), default to gen-1/CFE values, and pin them by devmem oracle capture on the
live stock slot2 (the technique that pinned VIQ 13) before the first real egress test.
