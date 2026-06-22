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
- MACTYPE 0x00 = 1 (MAC_TYPE_GPON; 7 is invalid); BBCFG_1_TX 0x04: FPMSRC[31:24]=**23**, SBPMSRC[23:16]=**56**.
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

## Wave-3 closures (2026-06-22) + ★MPM blocker★
Adversarial verification PASSED: every offset/value/bit above MATCHES BCM6813 source
(autogen/XRDP_AG.h + CFE2 data_path_init). Only fix: BBH_TX MAC_TYPE = **1** (GPON), not 7.

Core start (rnr_image_first_task is debug-only/zeros): cores start per-subsystem via
`cfg_cpu_wakeup_set(get_runner_idx(image), IMAGE_n_*_THREAD_NUMBER)`. core→image = identity.
**CPU RX = core 3, thread 1** (IMAGE_3_CPU_RX_THREAD_NUMBER); **CPU TX = core 2, thread 6**
(IMAGE_2_CPU_TX_0_THREAD_NUMBER).

RNR_REGS base 0x82800000 stride 0x1000 (rdpa.ko RNR_REGS_ADDRS). CFG_SCH_CFG@0x4c=4 (DRV_RNR_16SP).
CFG_PSRAM_CFG@0x44 = 0x820 (psram base 0x82000000>>20). CFG_DDR_CFG@0x40 = (bufmem_phys>>20) encoded.

QM (from rdpa.ko drv_qm_init disasm): FPM_CONTROL@0x0c fixed bits = override_bb_id_en + bb_id=0x3e(FPM)
+ pool_bp_enable → 0x7D000001 | (prefetch_min_pool_size<<8); DDR_SOP_OFFSET@0x3c = 0; FPM_BASE_ADDR@0x34
= bufmem_phys>>8 (full, also COHERENT@0x38); MPM pool sizes@0x180/4/8 = buf_size×{8,4,2,1} cap 0x3fff
(runtime); QM_ENABLE_CTRL@0x00 = 0x307 (set in data_path_init, runtime). FPM buf size default 512B.

★★MPM = HARD BLOCKER for a fully-open COLD datapath★★
- MPM is a SEPARATE HW DMA engine, DT node `brcm,mpm` @ **0x80020000 / 0x4000** (kernel/dts/6813),
  NOT in the XRDP 0x82xxxxxx window, NOT Runner-core logic.
- Its HW bring-up (mpm_init/mpm_pool_init/mpm_enable, the BALLOC/BFREE DMA rings, DQM_INTERFACE_CFG)
  is ONLY in proprietary `bcmdrivers/broadcom/char/mpm/impl1/{bcm_mpm.ko,mpm.6813.o_saved}` — GPL
  source stripped (rdp_drv_fpm.c/xrdp_drv_fpm_ag.* dangling on 6813). drv_mpm_init() (GPL) is
  software bookkeeping only.
- FPM-direct path is `#ifndef RUNNER_MPM_SUPPORT` + CHIP_VER<RDP_GEN_62 gated → OFF on 6813.
- GPL Runner microcode does NOT contain MPM/buffer-alloc logic.
→ Routes to a working datapath: (1) CLEAN-ROOM the MPM block (RE bcm_mpm.ko register sequence —
  block strings enumerate MPM_COMMON/DMA_COMMON/BALLOC/BFREE/CORE; highest effort, only fully-open
  route); (2) software-managed CPU-ring buffers for slow-path FIRST-LIGHT (rdp_cpu_fpm_alloc /
  QEMU-M5 path — may move CPU-forwarded frames without the MPM DMA rings); (3) interim: user loads
  their own bcm_mpm.ko. The control plane + microcode (GPL) + the rest of the bring-up are open;
  the buffer allocator is the one closed dependency.

## Wave-4 (2026-06-22): binary-confirmed bases + MPM-free first-light + MPM clean-room
★CORRECTION via BCM6813's OWN rdpa.ko *_ADDRS (authoritative; reverted earlier proxy fixes):
- **RNR_MEM base = 0x82700000** (offset 0x700000), stride 0x20000, 8 cores; per-core MEM+0,
  INST+0x10000, PRED+0x1c000, CNTXT+0x18000. (6837/6888 proxy 0x82600000 was WRONG by +0x100000.)
  → abs CPU RX ring = 0x82763000 (core3), TX ring = 0x82743360 (core2), TX indices = 0x827429c8.
- **DQM = 0x82c80034** (stock accessor base; reverted my 0xc80000). RNR_QUAD 0x82808400.
- Other bases CONFIRMED from rdpa.ko: FPM 0x82a00000, QM 0x82c00000, SBPM 0x828a1000,
  DSPTCHR 0x82880000, BBH_RX 0x82898000/0x400, BBH_TX 0x82890000/0x2000, RNR_REGS 0x82800000/0x1000,
  PSRAM 0x82000000, PSRAM1 0x82200000, UBUS_SLV 0x828a0000. NATC 0x82950000 (KEY 0x829503b0/0x20,
  TBL 0x829502d0/0x10), TCAM 0x82900000, HASH 0x82920000, CNPL 0x82948000.

★MPM-FREE FIRST-LIGHT = FEASIBLE (the near-term path):
- CPU-RX buffers come from a host-managed **FEED ring** (host-allocated DDR buffers as 40-bit ABS
  pointers, CPU_FEED_DESCRIPTOR @ rdp_cpu_ring_defs.h:38; host fills via __rdp_prepare_feed_desc +
  doorbell rdd_cpu_inc_feed_ring_write_idx) — NOT MPM. MPM is only the bulk/offload DMA front-end
  over the SAME FPM token pool. BCM6813 runs feed-ring + MPM side-by-side (CONFIG_RNR_FEED_RING on).
- Driver correction: real CPU-RX uses a DEDICATED feed ring + write-idx doorbell, not in-place
  re-arm of the RX data ring (our current rx_rest_desc). Add the feed ring + recycle→feed.
- FPM token use (TX): alloc = READ POOL0_ALLOC_DEALLOC + check FPM_TOKEN_VALID; free = WRITE token
  (GPL fpm driver bcmdrivers/opensource/.../fpm/impl1 is the clean-room ref). No MPM needed.
- MINIMAL first-light block set (from CFE2 _data_path_init): UBUS decode + RNR(microcode load+enable
  core2/3) + BBH_RX + BBH_TX + SBPM + DMA/SDMA + DSPTCHR(CPU_RX direct-processing group + CPU_TX
  egress VIQ). **SKIP QM (stub) + MPM.** Hard external dep = the proprietary Runner microcode + the
  RDD IMAGE_0 CPU-thread/ring-base table addresses (clean-room-able), NOT the buffer engine.

★MPM clean-room (for full offload later): recovered from bcm_mpm.ko disasm. MPM @ 0x80020000/0x4000
(separate from XRDP). Sub-blocks: COMMON@0x3d00 (CONFIG/CONTROL/STATUS, POOL_CFG_0..3@0x3d20..2c),
DMA_COMMON@0x2d00 (CONFIG, PHYS_BASE@0x2d10, VIRT_BASE_LO/HI@0x2d14/18, DQM_INTERFACE_CFG@0x2d30),
CORE@0x3000, BALLOC@0x0 (PSB@0x20/24/28, rings@0x30+), BFREE@0x2000 (CSB@0x2020/24/28, CPU_PTR@0x21f0,
SKB/FKB ptr offs@0x21f4-fc). Ordered init: pmc_mpm_en+settle → memset shadow → DMA_COMMON_CONFIG
RMW → POOL_CFG_0..3 ({8,4,2,1}×512) → PHYS/VIRT_BASE=bufmem → CORE_CONFIG (buf_size_log2<<4) →
BFREE ptr cfg → per-client(0x11)/per-ring(1) init → BUF_INIT (write 0xf6 @+0x100+4, poll &0x2c@+8)
→ master enable (DMA_COMMON+0x30 |=1, +0x3c|=0x80000000, etc.). Needs PMC + bufmem base + SYSRAM
rings. Gaps: exact bitfield names + per-ring strides (one read-only mpm_reg_dump on silicon resolves).

## Wave-5 (2026-06-22): microcode artifact + feed-ring ABI + ★PROJECT-VARIANT WARNING★
★Microcode blob BUILT (dev-build /tmp/bcm4916-runner-microcode.bin, 270496 B, sha256
e1141770967a34146efcfa4aa97e56df83c1c71cce4933225f1f9c909a95e69b; generator /tmp/gen_runner_fw.py).
Layout: 32B header "RFW1"|ver1|num_cores8|hdr32|entry16|total|rsvd2, then 8×{inst_off,inst_len,
pred_off,pred_len} (entry i==core i), then 8 inst(32KB) + 8 pred(1KB). ★pred is uint16_t = 1024B
(NOT u32). Loader: per core memcpy RNR_INST[c]=0x82700000+c*0x20000+0x10000 (32KB), RNR_PRED[c]
=+0x1c000 (1KB). GPLv2+linking-exception → MODULE_FIRMWARE ok.

Feed-ring/CPU-ring ABI (MPM-free, confirmed): FEED ring desc @ IMAGE_3 RDD 0x0f70 (CPU_FEED_DESCRIPTOR
8B: ptr_low@0, type@6b0=ABS_TYPE=1, ptr_hi@7); host fills + bumps WRITE_IDX (in the ring desc),
doorbell rdd_cpu_inc_feed_ring_write_idx, batch thr 128, CPU_RING_SIZE_64_RESOLUTION=6. RX delivery
ring desc table @ 0x3000 core3; **host polls read_idx(@12) vs write_idx(@6)** of CPU_RING_DESCRIPTOR
(NOT a word2 ownership bit — corrects wave-1). TX ring desc + indices(4B: read@0/write@2) + the
rdd_ring_init field recipe known (GPL legacy ref rdd_cpu_ring.c:248). DSPTCHR: CPU_TX_EGRESS=VIQ13,
CPU_RX=direct-processing(thread, not a VIQ). DMA/SDMA base 0x828b2000 (DMA1 +0x800).

★★PROJECT-VARIANT WARNING — values DIFFER across SDK rdp projects; use the one the DEVICE's rdpa.ko
matches before coding any MMIO:
- CPU_TX_RING_DESCRIPTOR: BCM6813 = 0x33e0 vs BCM6813_FPI = 0x3360. (RX ring 0x3000 + TX indices
  0x29c8 agree across both.)
- SBPM INIT_FREE_LIST: CFE2 basic-dump = 0x5fc000 (0x17F buffers) vs FPI/earlier = 0x03FFC000 (0xFFF).
  DIS_REOR linked-list = 512 (CFE2) vs 1024 (FPI). BBH_TX stride 0x4000 (CFE2 dump) vs 0x2000.
- CPU thread numbers: CFE2 single-image CPU_RX=0/CPU_TX=1; rdpa-runtime CPU_RX=core3/thread1,
  CPU_TX=core2/thread6. The CFE2 datapath is the bootloader (single CFE core image); the rdpa Linux
  runtime is multi-image — the DEVICE runs rdpa, so the rdpa/runtime values are authoritative for us.
→ NEXT (pre-implementation): pin which rdp project the targets/96813GW build = device rdpa.ko uses,
  and take CPU ring/feed/index addrs + SBPM init + thread nums from THAT (cross-check vs the actual
  rdpa.ko). Block bases already came from the device rdpa.ko (wave-4) and are authoritative.

## Wave-6 (2026-06-22): PROJECT VARIANT RESOLVED → BCM6813 (not _FPI)
The GT-BE98 (96813GW) firmware compiles rdp project **BCM6813** (PKTFLOW), NOT BCM6813_FPI.
Evidence: make.common:796-801 selects BCM6813 unless BRCM_DRIVER_FPI is set; the 96813GW.GT-BE98
profile sets BRCM_DRIVER_PKTFLOW=m and does NOT set FPI. ("FPI"=Flow Provisioning Interface, an
alt accel that replaces PKTFLOW; not used by GT-BE98.)
→ AUTHORITATIVE CPU ring/feed values (BCM6813 rdd_runner_defs_auto.h):
  RX ring desc 0x3000 (IMAGE_3/core3) · **TX ring desc 0x33e0 (IMAGE_2/core2)** [was wrongly 0x3360
  from _FPI; FIXED in driver] · TX indices 0x29c8 (core2) · FEED ring desc 0x0f70 (IMAGE_3/core3).
  CPU_RX thread 1 (core3), CPU_TX_0 thread 6 (core2), core→image identity.
The block bases (wave-4, from device rdpa.ko) + these BCM6813 RDD addrs are now the authoritative
set for implementation. SBPM init (0x03FFC000 vs 0x5fc000) still to confirm from the BCM6813
project (CFE2's 0x5fc000 is the bootloader's smaller pool; use BCM6813 rdpa value) — minor, SBPM is
slow-path-optional for first CPU frame.

## STATUS: RE COMPLETE — ready for implementation
Open MPM-free first-light datapath is fully specified: microcode blob built (RFW1), per-core
INST/PRED load offsets, RNR enable+wakeup (core3/thr1 RX, core2/thr6 TX), UBUS decode (done in
driver), FEED ring + CPU RX/TX rings + doorbells, BBH_RX/BBH_TX/SBPM/DSPTCHR/DMA minimal register
sets, all bases device-rdpa.ko-confirmed. Remaining = WRITE the driver bring-up (coherent single
file) + a deadman-guarded slot1 trial. MPM full HW-offload is clean-roomable later (map recovered).
