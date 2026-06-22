# BCM6813 XRDP Runner bring-up — register spec (pinned vs SDK oracle, 2026-06-22)

Synthesis of 6 parallel SDK-source RE passes. XRDP window phys base **0x82000000**; offsets below
are window-relative unless stated. Per-core RNR data-mem base = 0x82600000 + core*0x20000;
INST = +0x10000, PRED = +0x1c000. Provenance: SDK `src-rt-5.04behnd.4916/rdp/...` (CFE2
`data_path_init.c` is the readable bring-up oracle; per-block AG from BCM6888/6837 autogen,
offsets cross-checked vs BCM6813 `autogen/XRDP_AG.h`). ★Caveat: BCM6813 per-block autogen `.c`
are build-generated (dangling symlinks here); a few exact values live only in proprietary
`rdpa.ko`/`bcm_mpm.ko` — flagged below.

## Block bases (verified)
| block | window off | abs | note |
|---|---|---|---|
| PSRAM | 0x000000 | 0x82000000 | NAT-C staging etc. |
| RNR_MEM[core] | 0x600000 + core*0x20000 | 0x82600000+ | per-core data SRAM |
| RNR_REGS[core] | ~0x800000 + core*0x1000 | 0x82800000+ | ★base from 6888 proxy; re-confirm |
| DSPTCHR (DIS_REOR) | 0x880000 | 0x82880000 | ✓ |
| BBH_TX[i] | 0x890000 + i*0x2000 | 0x82890000 | 4 inst ✓ |
| BBH_RX[i] | 0x898000 + i*0x400 | 0x82898000 | 11 MAC inst (0-10) ✓ |
| UBUS_SLV | 0x8a0000 | 0x828a0000 | ✓ |
| SBPM | 0x8a1000 | 0x828a1000 | ✓ |
| FPM | 0xa00000 | 0x82a00000 | ✓ |
| QM | 0xc00000 | 0x82c00000 | ✓ |
| DQM | 0xc80000 | 0x82c80000 | ★driver had 0xc80034 (=base+0x34 reg), base is 0xc80000 |

## Bring-up order (from CFE2 data_path_init + rdp_block_enable)
1. RNR cores addr init; CFG_GEN_CFG zero-mem + poll; **load microcode** (+prediction); sched/dma cfg.
2. UBUS_SLV decode windows.  3. FPM/MPM init.  4. SBPM init (trigger + poll RDY).
5. DSPTCHR init (free-list, VIQ).  6. QM init.  7. BBH_RX/BBH_TX cfg.
8. **rdp_block_enable** (LAST, in order): SBPM ready-poll → XLIF rx/tx → BBH_RX ENABLE →
   DSPTCHR REORDER_CFG EN → QM_ENABLE_CTRL → UBUS-mstr → **RNR enable + cpu_wakeup**.

## 1. Microcode load + core enable  (rdp_drv_rnr.c)
- `fw_inst_binaries[8]` (32KB each), `fw_pred_binaries[8]` (2KB), index==core, **distinct per core**.
- Per core: MEMSET RNR_INST[core] = NOP 0xFC000000 (8192 words), then write inst words (skip NOPs);
  prediction: read u16 src → write as u32 to RNR_PRED[core] (1024 entries).
- CFG_GEN_CFG (RNR_REGS off 0x30): DISABLE_DMA_OLD_FLOW_CONTROL b0, ZERO_DATA_MEM b2, ZERO_CONTEXT_MEM b3; poll *_DONE.
- Enable (per core): RNR_REGS CFG_GLOBAL_CTRL **off 0x00** EN b0 = 1; then CFG_CPU_WAKEUP **off 0x04** THREAD_NUM[3:0] = `rnr_image_first_task[core]` (write-to-start).
- ★UNPINNED: BCM6813 RNR_REGS abs base; `rnr_image_first_task[core]` per-core start-thread (in rdpa.ko).

## 2. UBUS_SLV decode windows  (FULLY PINNED; ubus_bridge_init)
Each window = START reg + END reg, each a full 32-bit ADDRESS (no mask, no enable bit). Base 0x828a0000:
| window | START off | START val | END off | END val |
|---|---|---|---|---|
| VPB | 0x04 | 0x82700000 | 0x08 | 0x82900000 |
| APB | 0x0c | 0x82900000 | 0x10 | 0x82a00000 |
| dev0 FPM | 0x14 | 0x82a00000 | 0x18 | 0x82c00000 |
| dev1 QM | 0x1c | 0x82c00000 | 0x20 | 0x82c80000 |
| dev2 DQM | 0x24 | 0x82c80000 | 0x28 | 0x82d00000 |
Program order: dev0, dev1, dev2, vpb, apb.

## 3. FPM/MPM init  (base 0x82a00000)
- BCM6813 = MPM path (-DRUNNER_MPM_SUPPORT); HW sequence in proprietary `bcm_mpm.ko` (★no source).
- pool config 0x01020408 = pool_size{8,4,2,1} (software descriptor for RDD/QM, not one MMIO).
- Register map (offsets in FPM block): FPM_CTL 0x000 (INIT_MEM b4, BB_SOFT_RESET b14, POOL1_ENABLE b16,
  POOL2_ENABLE b17), FPM_CFG1 0x004, POOL1_CFG1 0x040 (FPM_BUF_SIZE [26:24]; 1=512B default),
  POOL1_CFG2 0x044 (POOL_BASE_ADDRESS [31:2] word-addr), 256K tokens.
- QM-side FPM coupling: QM_GLOBAL_CFG_FPM_CONTROL 0x00c, FPM_BASE_ADDR 0x034, MPM_ENHANCEMENT_POOL_SIZE 0x140/0x144/0x148.
- Minimal direct path if not using MPM: FPM_CTL BB reset → INIT_MEM pulse → POOL1_CFG1/2 → POOL1_ENABLE.

## 4. SBPM init  (base 0x828a1000)
- Defaults: SP_RNR_LOW 0x184, SP_RNR_HIGH 0x188, UG_MAP_LOW 0x18c, UG_MAP_HIGH 0x190.
- **INIT_FREE_LIST 0x000 = 0x03FFC000** (INIT_OFFSET=0xFFF<<14 → 0x1000 buffers); UG0_TRSH 0x050 = 0x400.
- Set egress Runner src-port bit (RMW SP_RNR_LOW/UG_MAP_LOW). Poll INIT_FREE_LIST RDY bit31.

## 5. DSPTCHR init  (base 0x82880000)
- Free linked list: seed 1024 nodes BDRAM_NEXT_DATA 0x3000 (node i←i+1) + FLL desc 0x2700 (minbuf=1024,head=0,tail=1023) — MUST precede VIQ cfg.
- Per-VIQ: QUEUE_MAPPING_CRDT_CFG 0x400, Q_DEST 0x4c0, INGRS limits 0x300, QDES head/tail 0x2000/0x200c. Congestion 0x080/0x100/0x184. Task→RG map 0x900.
- REORDER_CFG_VQ_EN 0x004 = VQ bitmask. **FINAL: REORDER_CFG_DSPTCHR_REORDR_CFG 0x000 = EN b0 + AUTO_INIT b4 (~0x11), poll RDY b8.**

## 6. QM init  (base 0x82c00000, DQM 0x82c80000)
- QM_GLOBAL_CFG_FPM_CONTROL 0x00c (RMW; FPM_PREFETCH_MIN_POOL_SIZE [9:8] from buf_size; reset 0x310).
- FPM_BASE_ADDR 0x034; DDR_SOP_OFFSET 0x03c. Quiesce queues QUEUE_CONTEXT_CONTEXT 0x800 (per-q +q*4).
- Per CPU egress q: passing WRED, aggregation_disable=1. DQM_DQMOL_CFGB[q] 0x1fd4 ENABLE b0.
- **FINAL: QM_GLOBAL_CFG_QM_ENABLE_CTRL 0x000 = 0x307 (FPM_PREFETCH|REORDER_CREDIT|DQM_POP|arb); min 0x007.**

## 7. BBH_RX (per MAC port, base 0x82898000+id*0x400) — CPU path via a real MAC port
BB_IDs: DISPATCHER_REORDER=18, FPM=23, SBPM=56, SDMA0=21/SDMA1=22, TX_LAN=32.
- SDMACFG 0x20 (NUMOFCD, EXCLTH); SDMAADDR 0x1c (DATABASE/DESCBASE); SDMA bb_id = 21/22.
- BBCFG 0x00: DISPBBID[15:8]=**18**, SBPMBBID[23:16]=**56**. DISPVIQ 0x04: NORMALVIQ/EXCLVIQ = bbh_id.
- MINPKT/MAXPKT 0x24/0x28; SOPOFFSET 0x30; SBPMCFG 0x64 MAXREQ=0xf.
- **ENABLE 0x3C: PKTEN b0=1, SBPMEN b1=1 (LAST, in rdp_block_enable).**

## 8. BBH_TX (base 0x82890000+id*0x2000, BBH_TX_ID_LAN=0)
- MACTYPE 0x00 = 7 (GPON unified); BBCFG_1_TX 0x04: FPMSRC[31:24]=**23**, SBPMSRC[23:16]=**56**.
- RNR_SRC_ID (runner core+CPU_TX task); DMACFG 0x20 / SDMACFG 0x24 (src=21, desc base/size 16);
  DDRTMBASEL/H 0x2c (FPM packet pool phys); LAN PDBASE/PDSIZE/Q2RNR per-queue FIFOs.

## CPU ring/descriptor ABI — validation diffs vs our driver (apply these)
- ✓ RX descriptor (`struct runner_rx_desc` + RXD_*): MATCHES the SDK on every field.
- ✓ Ring control block (`runner_ring_cfg`): byte layout MATCHES. (It lives in core-3 RNR SRAM @0x3000, not "PSRAM" — comment only.)
- ✓ TX doorbell: write u16 write_idx at **+2** of the 4-byte indices entry @0x29c8 core 2. (entry = {read_idx@0, write_idx@2}).
- ✗ **RX ownership/empty test must key on WORD2** (`rdpa_cpu_ring_not_empty`: word2 & 0x80000000 = is_src_lan as the producer-set bit; refill writes only word2 = swab(phys & 0x7fffffff)). Our header comment says word3 — FIX.
- ✗ **TX descriptor word2 (`TXD_W2_*`) byte layout is wrong**: color/do_not_recycle/flag_1588/is_emac in **byte8** (b7/b6/b5/b4); abs in **byte9 b0**; ssid in **byte10 [5:2]**; pkt_buf_ptr_high in **byte11**; wan_flow_source_port/flow_or_port_id = u16@+8 [11:4]/[10:4], is_vport byte8 b3. Rebuild from byte macros.
- ✗ **TX `TXD_W3_FPM_BN0/SOP` unverified** — no such fields in the SDK TX struct (word3 = pkt_buf_ptr_low). Likely carried from another chip; re-derive if FPM-mode TX needed.
- ⚠ sk_buf_ptr (word1/+4) and pkt_buf_ptr (word3/+12) are DISTINCT pointers in the SDK; confirm which the open TX fills.

## Remaining unpinned (proprietary blobs)
- `rnr_image_first_task[core]` (per-core start thread) — in rdpa.ko.
- Exact FPM/MPM ordered HW init — in bcm_mpm.ko (register map known; sequence not).
- BCM6813 RNR_REGS absolute base — re-confirm from build-generated XRDP_RNR_REGS_AG.c or live read.
- Several runtime values (fpm_base, buf_size, VQ bitmask, SOP_OFFSET) are p_dpi_cfg/enum-derived.
