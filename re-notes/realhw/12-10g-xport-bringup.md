# BCM6813 (GT-BE98) 10G XPORT port bring-up — clean-room RE reference

**Purpose:** reimplement the stock-SDK bring-up of the two 10G Ethernet ports in the open
`bcm4916_runner` driver so the already-cabled `eth0`/`eth1` move packets. READ-ONLY RE; nothing
in the SDK was modified. SDK root (on the build machine):
`$SDK = ~/be98/gt-be98-firmware/vendor/asuswrt-merlin.ng/release/src-rt-5.04behnd.4916`.

Milestone context: the open driver already probes + runs the Runner on silicon (microcode loaded,
`rnr0` conduit up) via the datapath-skip + `insmod` path. This doc covers the remaining 10G port
plumbing.

## 0. Definitive port map (verified in `$SDK/kernel/dts/6813/6813.dtsi` + `GT-BE98.dts`)

**The two 10G ports use DIFFERENT PHY front-ends** (key correction):

| netdev | switch reg = port_id = BBH/VIQ = rdpa_emac | DTS node | mac-type | **mac-index = xport_port_id** | phy addr | front-end |
|---|---|---|---|---|---|---|
| **eth0** | **5** | port_xphy   | XPORT | **0** | EXT3 addr **9** | **internal 10G XPHY** (phy_drv_ext3 + eth-phy-top @0x837ff000) — self-contained, **no merlin16 blob for link** |
| **eth1** | **6** | port_sgmii1 | XPORT | **2** | 10GAE addr **7** | **merlin16_shortfin core0/lane0** (phy-fixed 10GBase-R) |
| eth2 | 1 | port_gphy1  | UNIMAC | 1 | EGPHY | 1G (the driver's current first-light port) |
| eth3 | 7 | port_sgmii2 | XPORT | 4 | 10GAE addr 8 | merlin16 core1/lane0 (+ BCM84891L @21) |

THREE distinct index spaces — do not confuse:
- **port_id** (5,6) = BBH_RX index = dispatcher VIQ = rdpa_emac index.
- **mac-index** (0,2) = XPORT/XLMAC MAC instance (register-block selector). Both 10G ports are on **XLMAC0**.
- **phy MDIO addr** (9,7).

**⇒ eth0 is the more tractable first target** (its XPHY handles its own SerDes; no merlin16 full-link needed).

## 1. Register-base map

| Block | Base | Notes |
|---|---|---|
| XRDP/Runner/rdpa window | 0x82000000 (0xCAF004) | BBH/QM/DMA/dispatcher all inside |
| BBH_TX LAN / LAN_1 | 0x82890000 / 0x82894000 | stride 0x2000 (DTS-authoritative) |
| BBH_RX id5 / id6 | 0x82899400 / 0x82899800 | base 0x82898000 stride 0x400 — **⚠ validate on silicon** (from BCM6888 same-family autogen; BCM6813's own BBH_RX_AG is build-generated/absent) |
| XLIF (10G runner-facing) | 0x828b2000 | channel 0x680, credits at +0x48 |
| MPCS | 0x828C4000 | |
| XPORT MAC window | 0x837f0000 (0x8000) | XLMAC0=0x837f0000, XLMAC1=0x837f4000 |
| XLMAC_CORE eth0 / eth1 | 0x837f0000 / 0x837f0800 | port stride 0x400 (both on XLMAC0) |
| XPORT TOP/MAB/PORTRESET (XLMAC0) | 0x837f2000 / 0x837f3300 / 0x837f3400 | |
| eth-phy-top (XPHY, eth0 addr9) | 0x837ff000 (0x1000) | |
| XPORT0_CLK_CTRL | 0x837ff1f8 | |
| merlin16 serdes indirect window (eth1) | 0x837ff500 (0x300) | core bank = base + core*0x100 |

## 2. XPORT / XLMAC 10G MAC init  (Area 2 — the new block; `$SDK/bcmdrivers/opensource/phy/xport/`, `xport_drv.c`, `XPORT_AG.h`)

`PID_XLMAC_NUM(id)=id>>2`, `PID_XPORT_NUM(id)=id&3`. eth0 xport_port_id 0 → XLMAC0/port0 @0x837f0000; eth1 id 2 → XLMAC0/port2 @0x837f0800. XLMAC_CORE regs are 64-bit but committed by a **single low-word 32-bit write** (fields > bit31 like TX_THRESHOLD@b38, PFC_STATS_EN@b35 are NOT delivered — do a true 64-bit write if needed).

**Phase A — init_driver (leave in reset), `xport_drv.c:718-752`:** PORTRESET.CONFIG(+0x10) LINK_DOWN_RST_EN/ENABLE_SM_RUN/TICK_TIMER_NDIV; PORTRESET Px_SIG_EN (P0+0x30 / P2+0x90) set XLMAC_RX/TX_DISAB(b9/b8), XLMAC_SOFT_RESET(b6), MAB_RX/TX_PORT_INIT(b5/b4), MAB_TX_CREDIT_DISAB(b3), MAB_TX_FIFO_INIT(b2), PORT_IS_UNDER_RESET(b1)=1, EP_DISCARD(b0)=0; udelay(5000); CONFIG.ENABLE_SM_RUN|=(1<<xport_num); udelay(5000); XLMAC_CORE.RX_LSS_CTRL(+0x50).LOCAL_FAULT_DISABLE=1; xport_reset (MAB.CONTROL RX/TX_PORT_RST b0/b4; CORE.CTRL(+0x00).SOFT_RESET(b6)=1; XLIF hold credits |=(1<<12)).

**Phase B — link-up, `xport_drv.c:628-669`:** PORTRESET Px_CTRL(P0+0x00 / P2+0x08).PORT_SW_RESET(b0)=1; XLIF release credits &=~(1<<12); **xport_xlmac_init (`:411-527`):** PFC_CTRL(+0x70).PFC_STATS_EN(b35)=1; **TOP.CONTROL(0x837f2000+0x00) Px_MODE = 1 (XGMII/10G)**; TX_CTRL(+0x20) CRC_MODE(b0)=3,PAD_EN(b4)=1,TX_THRESHOLD(b38)=2; RX_CTRL(+0x30) STRIP_CRC(b2)=0,RUNT_THRESHOLD(b4)=0x40,RX_PASS_CTRL(b13)=1; **MODE(+0x08).SPEED_MODE(b4,3b)=4 (10G)** → low word 0x40; release SOFT_RESET(b6)=0; xport_msbus_init (MAB weights + clear this port's TX_CREDIT_DISAB b12/TX_FIFO_RST b8/RX_TX_PORT_RST); poll TOP.STATUS(+0x04).LINK_STATUS bit(1<<xport_num); PORTRESET Px_CTRL.PORT_SW_RESET=0.

**Phase C — enable (`:980-991`):** CORE.CTRL(+0x00) RX_EN(b1)=1, TX_EN(b0)=1. MTU RX_MAX_SIZE(+0x40)=0x3fff. Pause PAUSE_CTRL(+0x68).

## 3. SerDes link  (Area 1 — eth1/merlin16 only; eth0's XPHY is EXT3/self-contained)

merlin16 indirect window 0x837ff500, per-core bank +core*0x100: ADDR@+0x004, MASK@+0x008, CONTROL@+0x00c, STATUS@+0x010, INDIR_ACC_CNTRL@+0x0f0. Addr encode `(dev<<27)|(lane<<16)|reg`. STATUS: rx_sigdet[0] cdr_lock[1] link_status[2] pll_lock[3]. (The driver already has this window + the ucode-load "step 1".)

Full-link add-ons (all reimplementable except the 31664-B blob):
- **PLL/VCO 50MHz→10.3125G** table `M1_merlin.h:118-135`: dev3 0x9100[15:12]=0x6 refclk50; dev1 0xD0F4[13]=0 hold; dev1 0xd0b8[9:0]=0x0ce ndiv_int=206; dev1 0xd0b6[11:10]=2,0xd0b7[13:0]=0x1000; dev1 0xd0b9[6:3]=0xF; dev1 0xd0b1[3:0]=3,0xd0b0[11:9]=1,[15:14]=3; dev1 0xD0B9[0] 0→1 toggle. Wait pll_lock (STATUS bit3).
- **TX-FIR** 10GBASE-R: pre=1,main=38,post1=1,post2=0 (`merlin16_tx_analog_functions.c:597`); regs main=0xd11e pre=0xd11d post1=0xd11c post2=0xd11b hpf=0xd0a2.
- **rate-select force_speed_10g_R** `M1_merlin.h:530-546`: dev3 0xC30B[11]/[6:0]=0x04f (speed 0x0f, force_en); 0xC433[10]=0 TX-FEC off; 0xC454[2:0]=1 RX-FEC bsync; datapath_reset_lane + udelay(200); 0xC457[0]=1; 0xC30B[7]=1 mac_creditenable; 0xC433[1:0]=3 tx rstb+enable. Wait link_status.
- **RX AFE/DFE/CTLE = BLOB-INTERNAL** (only dfe_on/media_type uC-RAM vars set from C).

## 4. BBH_RX / BBH_TX for ports 5/6  (Area 3 — extend the existing BBH code)

BBH_ID_5_10G=5, BBH_ID_6_10G=6. Invariant **BBH_ID == VIQ == port_id**. BB_ID_RX_BBH_n=31+2n → port5=41, port6=43. **1G→10G table deltas (all data-driven):**

| | 1G (bbh0-4) | port5 | port6 |
|---|---|---|---|
| SDMA engine (BBID) | SDMA0/1 | **SDMA0=21** | **SDMA1=22** |
| SDMA chunks (numofcd) | 4 | **12** | **10** |
| chunk offset | 0/4/8/12/16 | 20 | 0 |
| SDMA urgent thr in/out | 0x02/0x01 | **0x03/0x02** | 0x03/0x02 |
| MAC feeding BBH | UNIMAC3 0x828a8000 | XLIF 0x828b2000 | XLIF 0x828b2000 |
| BBH_TX RNR sources | 2 | **4** | 4 |

BBH_RX cfg (`data_path_init.c:332-382`): disp_bb_id=18, sbpm_bb_id=56, normal_viq=excl_viq=bbh_id, min_pkt=64, sop_offset=0, per_flow_th=255, max_otf_sbpm=0xF. Write SDMAADDR@0x1c (database=first_chunk), SDMACFG@0x20 (descbase/numofcd/exclth), BBCFG@0x00 (SDMABBID[5:0]/DISPBBID[13:8]/SBPMBBID[21:16]/RNRBBID[29:24]), DISPVIQ@0x04 (NORMALVIQ[4:0]=bbh_id, EXCLVIQ[12:8]=bbh_id), SBPMCFG@0x64=0xF, per-flow init. Enable (ENABLE@0x3c) deferred to rdp_block_enable (also clears XLIF RX/TX disable for the 10G channels). BBH_TX (`:451`): MACTYPE@0x00=GPON(1); BBCFG_1@0x04 fpmsrc=23/sbpmsrc=56; **BBCFG_2@0x08 = 4 RNR sources (BCM6813-specific)**; DMACFG@0x20/SDMACFG@0x24; egress-counter binding ptr_addr=DS_TM_BBH_TX_EGRESS_COUNTER_TABLE>>3; DFIFOCTRL@0x3c.

## 5. Runner / RDPA datapath mapping  (Area 4 — host control-plane; enet impl7/runner.c + rdd tables)

- port5→BBH5→VIQ5→rdpa_emac5; port6→BBH6→VIQ6→rdpa_emac6. DMA0 serves RX_BBH_0..5, DMA1 serves 6..11.
- **DSPTCHR VIQ** (`data_path_init.c:704-807`): per port, bb_id=BB_ID_RX_BBH_0+2*bbh_id, bbh_target=2(normal), queue_dest=0(disp), delayed_queue=1, viq_num=bbh_id, coherency_en=1, rnr_grp_num=2; `dsptchr_rnr_group_list[1].queues_mask = (1<<BBH_ID_NUM)-1` (all BBH VIQs→group1). No reassembly VIQ for plain LAN Ethernet.
- **reason→CPU trap** (`runner.c`): create_rdpa_port sets def_flow.action=host + sal/dal_enable=1 + sal/dal_miss_action=host (unknown-SA/DA + flow-miss → CPU). reason→TC→RXQ maps (`_rdpa_map_reasons_2_queue`): control reasons (ARP/DHCP/…/ip_flow_miss=41/ttl_expired=43) → HI/LOW queues.
- **CPU RX ring is Runner-owned** (`rdp_ring.c`): ring_head=NULL required ⇒ Runner allocates; host pulls via `rdpa_cpu_packet_get(rdpa_cpu_host, hw_q_id, info)` + NAPI. Descriptor ring format is microcode-internal (the open driver already implements a matching CPU-RX path for rnr0).
- **rdpa_port** (`runner.c:951-1090`): index=port_id, type=rdpa_port_emac, is_wan=0 (LAN), SAL+DAL learning on, bind CPU conduit, LAN filter group.

## 6. Reimplementable vs. proprietary
**Reimplementable (GPL-C decodable):** SerDes reset/power/PLL/TX-FIR/rate-select/status; the entire XPORT/XLMAC MAC init; all BBH_RX/TX regs + SDMA/DMA/DFIFO/credit tables; DSPTCHR VIQ + group mask; reason→TC→RXQ maps; rdpa_port creation + CPU-rxq cfg.
**Proprietary (load verbatim / match ABI):** the 31664-B merlin16 blob (RX DFE/CTLE/CDR + lock SM), the Runner core microcode (parsing/classification/reason codes, dispatcher scheduling, FPM mgmt, CPU-RX descriptor ring format/ownership).

## 7. Open item to validate on live silicon
BBH_RX per-instance base **0x82898000 / stride 0x400** (from BCM6888 same-family; BCM6813's own BBH_RX autogen is absent). BBH_TX base/stride are BCM6813-DTS-authoritative. Everything else is BCM6813-authoritative.

## 8. Implementation plan for the open driver (recommended order)
1. **XPORT MAC block** (§2) — new regmap in `bcm4916_runner.h` (XPORT window 0x837f0000, XLMAC_CORE/TOP/MAB/PORTRESET) + `runner_xport_init(port)` in the `.c`. Target **eth0 (xport_port_id 0, XLMAC0/port0)** first.
2. **eth0 link** — XPHY (EXT3) via eth-phy-top MDIO addr 9 (self-contained; check whether the boot-time link persists into the datapath-skip boot before writing a full XPHY bring-up).
3. **BBH_RX/TX for port5** (§4) — extend the existing 1G BBH code with the port5 table row (SDMA0, 12 chunks @off20, BBH_ID 5, VIQ 5).
4. **DSPTCHR VIQ 5 + rdpa_emac5 + reason map** (§5) — extend the CPU-RX wiring for port5.
5. **Trial:** `insmod bcm4916-runner.ko` (datapath-skip boot) → bring up port5 → check TOP.STATUS link + `rnr0` RX counters under traffic on the eth0 cable.
6. eth1 (merlin16 full-link §3) is the harder follow-on; do after eth0 proves the XPORT+BBH+Runner path.
