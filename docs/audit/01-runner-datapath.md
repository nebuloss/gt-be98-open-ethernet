# Audit 01 — Runner base driver / open CPU datapath (the slow path / DSA conduit)

Subsystem files:

- `driver/runner/bcm4916_runner.c` — the driver (probe, bring-up, FPM pool, CPU RX/TX rings, NAT-C MMIO, SerDes loader).
- `driver/runner/bcm4916_runner.h` — register map, descriptor layout, FPM-token math.
- `driver/trial/bcm4916_runner_pdev.c` — a `platform_device` shim that binds the driver on a stock kernel with no DT node for it.

Reference RE notes cited throughout: `re-notes/xrdp-cpu-datapath.md` (feasibility / host-side ABI), `re-notes/realhw/10-runner-bringup-spec.md` (the 24-step bring-up, register-verified), `re-notes/realhw/11-route-a-egress-spec.md` (the CPU_TX→QM/TM→BBH_TX egress fix).

---

## 1. Purpose

The BCM4916 (a.k.a. BCM6813) is an **XRDP / "Runner"** SoC: the MAC ports (UNIMAC/XLMAC/XPORT) do **not** DMA into CPU-visible rings. They DMA into Runner-managed buffer pools (FPM/SBPM), and it is the **Runner firmware, executing on the packet-processor cores**, that classifies each frame, writes a CPU-RX descriptor into a host DDR ring on ingress, and on egress pulls the host's TX descriptor out of Runner SRAM and DMAs it to the MAC. There is no hardware bypass / "Runner idle" direct-DMA mode (`re-notes/xrdp-cpu-datapath.md` §1-2).

This driver is the **DSA conduit (master) netdev** for that integrated switch. Its job is the **slow path only**: move trapped / CPU-forwarded frames between the Runner and a Linux `sk_buff`, in both directions. It owns the ioremapped XRDP window and brings up the minimum hardware for one MAC port to move frames through the Runner to the CPU rings. A DSA switch driver (a b53/bcm_sf2 BCM4916 variant, a separate subsystem) is intended to attach its CPU port to this netdev.

Explicit non-goals stated in the file header (`bcm4916_runner.c:11-13`): the hardware **fast path** (flow offload / NAT-C / cmdlist) is not implemented here. The structs and bring-up are laid out so an offload layer can be added later; a Phase-1 L2/VLAN NAT-C offload MMIO path is stubbed in (owned here, driven from `flow_offload.c`).

Where it sits in the pipeline:

```
   MAC (UNIMAC/EGPHY) ── BBH_RX ──> SBPM/FPM buffer ──> [Runner core3/thread1: CPU_RX]
                                                              │ DMA frame into host FEED buffer
                                                              │ write CPU_RX_DESCRIPTOR into delivery ring
                                                              ▼
                                              host: NAPI poll (runner_rx_poll) ──> napi_gro_receive ──> Linux

   Linux ──> ndo_start_xmit ──> copy skb into FPM buffer ──> stage CPU_TX_DESCRIPTOR in TX ring
                                    │ bump write_idx in Runner SRAM + CFG_CPU_WAKEUP(core2/thread6)
                                    ▼
              [Runner core2/thread6: CPU_TX] ──> (stock blob) egress dispatcher ──> TM/QM core ──> BBH_TX ──> MAC
                                                                                     ▲
                                                                          Route A brings up this QM/TM hop
```

---

## 2. Architecture & data flow

### 2.1 Address windows the driver maps

`runner_probe()` maps three MMIO regions (`bcm4916_runner.c:1714-1738`):

| priv field | phys base | size | contents | mapped when |
|---|---|---|---|---|
| `p->xrdp` | `0x82000000` (`XRDP_WINDOW_BASE`) | `0x00caf004` | the whole rdpa datapath window: PSRAM, per-core RNR SRAM, DSPTCHR, SBPM, UBUS_SLV, BBH_RX/TX, UNIMAC, FPM, QM, NAT-C | always |
| `p->ethphytop` | `0x837f0000` (`ETHPHY_PHYS_BASE`) | `0x10000` | eth-phy-top / quad-EGPHY / mdiosf2 / xport (a **different** SoC region, not in the rdpa window) | HW only (`!runner_emulated`) |
| `p->serdes` | `0x837ff500` (`SERDES_PHYS_BASE`) | `0x300` | merlin16 10G SerDes indirect register window | HW only + `serdes_fw_load=1` |

The whole `p->xrdp` window is `devm_ioremap`'d **without** `request_mem_region` (`bcm4916_runner.c:1708-1719`), because the window hosts many sub-devices that already claim sub-regions; requesting the whole window would `-EBUSY`. Block handles are then computed as offsets into `p->xrdp`:

- `p->fpm  = p->xrdp + XRDP_OFF_FPM`  (`+0xa00000`)
- `p->natc = p->xrdp + XRDP_OFF_NATC` (`+0x950000`, from `flow_offload.h`)
- `p->rnr_mem[c] = p->xrdp + 0x700000 + c*0x20000` — per-core Runner **data SRAM** (8 cores)
- `p->rnr_regs[c] = p->xrdp + 0x800000 + c*0x1000` — per-core Runner **control regs**

The `0x82700000` RNR_MEM base (stride `0x20000`, 8 cores) is pinned from the device's own `rdpa.ko` `RNR_MEM_ADDRS[]` (`bcm4916_runner.h:42-47`); the header notes the 6837/6888 autogen proxy value `0x82600000` was wrong by `+0x100000` and was reverted. Per-core sub-regions: MEM+0, INST +`0x10000`, CNTXT +`0x18000`, PRED +`0x1c000`.

### 2.2 The two CPU host rings (RX) + one (TX), and where each index lives

The driver uses an **MPM-free first-light model** (`bcm4916_runner.c:322-336`, `re-notes/realhw/10` Wave-4/5): rather than the closed MPM DMA front-end, the host owns a contiguous RX buffer pool and hands empty buffers to the Runner via a FEED ring.

| ring | direction | descriptor | depth | resolution | where the control block lives |
|---|---|---|---|---|---|
| FEED | host → runner (empty buffers) | `runner_feed_desc` (8 B) | 256 | 64 (`>>6`) | core-3 SRAM @ `RDD_FEED_RING_DESC_TABLE 0x0f70` |
| RX delivery | runner → host (filled frames) | `runner_rx_desc` (16 B) | 256 | 32 (`>>5`) | core-3 SRAM @ `PSRAM_CPU_RING_DESC_TABLE 0x3000` |
| CPU TX | host → runner | `runner_tx_desc` (16 B) | 256 | 32 (`>>5`) | core-2 SRAM @ `PSRAM_CPU_TX_RING_DESC_TABLE 0x33e0` |

The ring **data arrays** live in coherent host DDR (`dmam_alloc_coherent`); only the **control blocks** and the **runtime indices** live in Runner core SRAM. Index locations within a control block (`bcm4916_runner.h:626-628`): `write_idx` = BE u16 @ +6 (runner advances on RX delivery / host advances on FEED), `read_idx` = BE u16 @ +12. The CPU-TX ring uses a separate 4-byte `CPU_TX_RING_INDICES_VALUES_TABLE` entry `{read_idx@+0, write_idx@+2}` at core-2 offset `0x29c8`.

RX delivery is detected by **polling write_idx vs the host's own read_idx** — NOT a per-descriptor ownership bit (`bcm4916_runner.c:328-330`; corrects an earlier "wave-1" assumption). This is the single most important ABI fact for RX.

### 2.3 Hardware blocks driven, with offsets

All offsets are relative to the block base unless noted; block bases are `p->xrdp + XRDP_OFF_*`.

- **FPM** (`+0xa00000`) — free-pool manager, the buffer source for TX (`FPM_CTL 0x0000`, `FPM_POOL1_CFG1 0x0040`, `FPM_POOL1_CFG2 0x0044`, `FPM_POOL1_STAT2 0x0054`, `FPM_SPARE 0x00c4`, `FPM_POOL0_ALLOC_DEALLOC 0x0400`). Token format: `VALID[31] | POOL[29] | INDEX[28:12] | SIZE[11:0]`.
- **RNR_REGS** (`+0x800000`, stride `0x1000`) — per-core control: `GLOBAL_CTRL 0x00` (EN b0), `CPU_WAKEUP 0x04` (THREAD_NUM[3:0]; the write starts the thread), `GEN_CFG 0x30` (zero-mem trigger + DONE bits), `DDR_CFG 0x40`, `PSRAM_CFG 0x44`, `SCH_CFG 0x4c`.
- **UBUS_SLV** (`+0x8a0000`) — address-decode windows (START/END register pairs, each a full 32-bit address).
- **SBPM** (`+0x8a1000`) — `INIT_FREE_LIST 0x000` (write `0x03FFC000`, poll RDY b31).
- **DSPTCHR** (`+0x880000`) — reorder engine + VIQ (virtual-input-queue) credit machine. Many RAM-indexed regs; see §4.
- **QM** (`+0xc00000`) — queue manager (Route A only): `ENABLE_CTRL 0x000`, `MEM_AUTO_INIT 0x138`/`_STS 0x13c`, `FPM_BASE_ADDR 0x034`, `DDR_SOP_OFFSET 0x03c`, `RUNNER_GRP` block @ `0x300` (stride `0x10`, 4 regs/group).
- **BBH_RX** (`+0x898000`, stride `0x400`, 12 ports) — per-port ingress DMA: `BBCFG 0x00`, `DISPVIQ 0x04`, `SDMAADDR 0x1c`, `SDMACFG 0x20`, `SOPOFFSET 0x30`, `ENABLE 0x3c`, `SBPMCFG 0x64`.
- **BBH_TX** (`+0x890000`, stride `0x2000`, 4 inst) — per-port egress: `MACTYPE 0x00`, `BBCFG_1 0x04`, `BBCFG_2 0x08`, `RNRCFG_2 0x60`, `Q2RNR_LAN 0x400`, `QMQ_LAN 0x4b0`, `QMQ_UNIFIED 0x7b0`.
- **UNIMAC** (`+0x8a8000`, stride `0x1000`, 4 inst) — `CMD 0x0008`, `FRM_LEN 0x0014`.
- **quad-EGPHY** (in `p->ethphytop`) — `QEGPHY_CTRL 0xf014`, `QEGPHY_STATUS 0xf018` (PLL lock b8).
- **NAT-C** (`+0x950000`) — flow-offload connection table; the driver stages key/ctx into PSRAM windows and issues an indirect command (Phase-1 stub).

### 2.4 Fixed block IDs (BB_IDs) used in wiring

`DISPATCHER_REORDER=18`, `FPM=23`, `SBPM=56`, `SDMA0=21`/`SDMA1=22`, `RX_BBH_n bb_id = 31 + 2*n` (`bcm4916_runner.h:191, 227-238`). "VIQ == bbh_id" is a firm platform rule.

### 2.5 RX data flow (MAC → CPU), end to end

1. A frame arrives on the 1G port; **BBH_RX** reassembles it into an SBPM/FPM buffer and enqueues a packet descriptor to the **dispatcher**, tagged with the port's VIQ (== bbh_id).
2. The **dispatcher** wakes the CPU_RX Runner thread (core3/thread1) — this is why `runner_dsptchr_cpu_rx_setup()` must place that core/thread in a runner group that consumes the VIQ and point the group's delivery address at the CPU_RX PD-table (`0x3940>>3`).
3. The CPU_RX thread pulls an empty host buffer off the **FEED ring**, DMAs the frame into it, writes a 16-byte `runner_rx_desc` into the **delivery ring** in host DDR, and advances the delivery ring's `write_idx` in core-3 SRAM.
4. Host side: an IRQ (if present) or a poll schedules NAPI. `runner_rx_poll()` reads `write_idx`, and for each new descriptor extracts the absolute 40-bit buffer pointer + length, copies the payload into a fresh skb, `napi_gro_receive`s it, and **recycles** the emptied buffer back to the FEED ring (re-post + feed doorbell). It advances the delivery ring `read_idx`.

### 2.6 TX data flow (CPU → MAC), end to end

1. `runner_start_xmit()` allocates an FPM token (a **read** of `FPM_POOL0_ALLOC_DEALLOC`), copies the skb into the token's 512-byte buffer, and `dma_sync`s it for the device.
2. It builds a 16-byte `runner_tx_desc` in host order and byte-swaps it into the TX ring slot: `is_egress`, packet length, `is_emac=1`, egress EMAC port = `RUNNER_FIRST_PORT`, `abs=0` (FPM-token mode), `word3 = fpm_bn0 | fpm_sop`. `do_not_recycle=0` so the Runner auto-frees the FPM buffer after transmit.
3. It advances `tx_write_idx`, then `tx_ring_doorbell()` publishes the new write_idx to core-2 SRAM `0x29c8+2` **and** issues a per-frame `CFG_CPU_WAKEUP(core2, thread6)`. The wakeup — not the index bump — is the real doorbell (`re-notes/realhw/10` Wave-8).
4. The **stock CPU_TX thread does not push directly to BBH_TX** — it hands the PD to the egress dispatcher, which routes it to a TM/QM Runner core that drains a QM queue into BBH_TX. Without that QM/TM hop the open slow-path TX freezes with `runner_read_idx` stuck at 3. **Route A** brings up exactly that hop (§2.7).

### 2.7 What Route A changes

Route A (opt-in `route_a=1`) is the fix for CPU_TX egress. Four coordinated changes (`bcm4916_runner.h:85-98`, `re-notes/realhw/11`):

1. **QM SRAM auto-init before enable** (`runner_qm_init` step 1): write `MEM_AUTO_INIT_EN` to `0x138`, poll `MEM_INIT_DONE` at `0x13c`. This is *the* step that makes `ENABLE_CTRL=0x307` safe — without it `fpm_prefetch` (bit0) reads uninitialised SRAM and hangs the SoC (the historical "0x307 hang"). The live stock oracle confirmed `MEM_AUTO_INIT_STS=0x01` (done=bit0) and `ENABLE_CTRL=0x0307` (`re-notes/realhw/11` "LIVE ORACLE RESULTS").
2. **Bind a QM queue range to the TM core via a RUNNER_GRP** (`runner_qm_init` step 3): `RUNNER_GRP[grp].QUEUE_CONFIG` = start=end=`route_a_queue`; `RUNNER_GRP[grp].RNR_CONFIG` = TM core `bb_id` | task | ENABLE. This is the TM-core wakeup path — QM issues credit/wakeup to the TM task through its UPDATE_FIFO; no extra DSPTCHR VIQ is needed for the TM hop.
3. **Point BBH_TX at the QM aggregator** (`runner_bbh_tx_route_a`): set `QMQ=1` (queue 0 of the LAN BBH_TX instance takes PDs from the QM aggregator instead of a Runner PD-FIFO), name the feeding TM core in `BBCFG_2` (PDRNR0SRC), set `RNRCFG_2` TASK.
4. **Mark the CPU_TX descriptor** `is_egress` + target `first_level_q` (`runner_start_xmit`, `bcm4916_runner.c:592-595`): when `route_a` is set, `word0` carries `first_level_q = route_a_queue` so the TM core enqueues the PD to the QM queue bound to BBH_TX.

The live oracle pinned `route_a_bbh_inst = 1` (BBH_TX[1] is the only QM-fed instance, MACTYPE=1) and gave two candidate queue/grp sets (B: queue≈80 grp0 tm_bb=7 tm_task=3 DS_TM; A: queue0 grp1 tm_bb=6 tm_task=4 US_TM). All Route-A values default to 0 (opt-in/safe) and are exposed as module params.

---

## 3. Data structures

### 3.1 `struct runner_priv` — `bcm4916_runner.c:185-230`

The per-device context (allocated inside the netdev's private area). Fields:

- MMIO handles: `xrdp`, `ethphytop`, `serdes`, `fpm`, `rnr_mem[8]`, `rnr_regs[8]`, `natc` (§2.1).
- FPM pool: `pool_vbase`/`pool_pbase` (coherent DDR base), `pool_size` (32 MB), `chunk_size` (512).
- RX rings: `rx_ring`/`rx_ring_phys` (delivery ring in DDR), `rx_head` (host read index), `feed_ring`/`feed_ring_phys`, `feed_widx` (host feed write index), `rx_pool_vbase`/`rx_pool_pbase` (contiguous RX buffer pool; phys↔virt is pure arithmetic).
- `rx_irq`, `napi`.
- TX ring: `tx_ring`/`tx_ring_phys`, `tx_write_idx`.
- `fw_loaded` — whether Runner microcode was actually loaded.
- Offload: `offload` (`struct xrdp_offload`), `natc`, `natc_next_idx` (simple slot allocator), `dbg` (debugfs dir).

### 3.2 Descriptor structs (all big-endian in DDR/SRAM; host byte-swaps every word)

**`struct runner_rx_desc`** (16 B, `bcm4916_runner.h:458-463`) — CPU RX delivery descriptor, validated against live silicon (`bs /Driver/cpUr/Vrpd`). Post-swap (host LE) field view:
- `word0` = abs buffer phys low 32.
- `word1` = ptr_hi[31:24]=phys[39:32] | `abs`(b16) | packet_length[15:2] | is_chksum_verified(b1). Length macros: `RXD_W1_PKT_LEN_SHIFT=2`, mask `0x3fff`; `RXD_W1_PTR_HI_SHIFT=24`.
- `word2` = reason[5:0] | data_offset[12:6] | source id | is_src_lan(b31).
- `word3` = wl_metadata[15:0] | is_ucast(b29) | is_rx_offload(b30) | is_exception(b31).

**`struct runner_feed_desc`** (8 B, `bcm4916_runner.h:514-524`) — an empty-buffer post. `ptr_low` = phys low 32; `w1` = valid | type/abs(`FEED_W1_ABS`=BIT(8), which is byte+6 bit0 after `cpu_to_be32`) | ptr_hi[7:0]. `ABS_TYPE=1` means an absolute DDR pointer.

**`struct runner_recycle_desc`** (8 B, `bcm4916_runner.h:533-540`) — defined for completeness (the runner posts freed skb tokens here); **not used** by the current MPM-free code, which recycles directly into the feed ring.

**`struct runner_tx_desc`** (16 B, `bcm4916_runner.h:557-588`) — CPU TX descriptor. Post-swap field view:
- `word0` = sk_buf_ptr_hi[7:0] | packet_length[21:8] | `egress_or_ingress_1`[30:22] (used as `first_level_q`, the 9-bit target QM queue) | is_egress(b31).
- `word1` = sk_buf_ptr_low / 1588 data.
- `word2` = pkt_buf_ptr_hi[7:0] | ssid[13:10] | abs(b16) | flow_or_port_id[26:20] | is_vport(b27) | is_emac(b28) | flag_1588(b29) | do_not_recycle(b30) | color(b31). Note `TXD_W2_PORT_MASK=0x7f` covers [26:20]; bit27 is `is_vport`, *not* part of the port field.
- `word3` = pkt_buf_ptr_low (abs=1) OR `fpm_bn0[19:0] | fpm_sop[29:20]` (abs=0).

**`struct runner_ring_cfg`** (16 B, `bcm4916_runner.h:594-599`) — the per-ring control block the host writes into Runner SRAM: `w0` = size_of_entry[byte0 top 5] | number_of_entries[10:0] (half@0) | interrupt_id (half@2); `w1` = drop_counter | write_idx; `base_addr_low` (word@8); `w3` = read_idx (half@c) | base_addr_high (byte@f).

### 3.3 `struct xrdp_offload` — `bcm4916_runner.h:653-657`

Bundles the flowtable `rhashtable`, a back-pointer `drv` to `runner_priv`, and `default_vport`. The NAT-C key/ctx types (`struct natc_key` = 4× BE u32; `struct fc_ucast_ctx` = 124-byte buffer + len) live in `flow_offload.h`.

### 3.4 Register-map macros (the load-bearing ones)

| macro | value | meaning |
|---|---|---|
| `XRDP_OFF_RNR_MEM0` / stride | `0x700000` / `0x20000` | per-core data SRAM |
| `XRDP_OFF_RNR_REGS0` / stride | `0x800000` / `0x1000` | per-core control regs |
| `XRDP_RNR_INST_OFF` / `_PRED_OFF` / `_CNTXT_OFF` | `0x10000` / `0x1c000` / `0x18000` | inst / prediction / context SRAM |
| `CPU_RX_RING_CORE` / `CPU_TX_RING_CORE` | 3 / 2 | which core owns each ring |
| `RNR_CPU_RX_THREAD` / `RNR_CPU_TX_THREAD` | 1 / 6 | CPU host thread numbers |
| `PSRAM_CPU_RING_DESC_TABLE` | `0x3000` | RX delivery ring cfg (core 3) |
| `RDD_FEED_RING_DESC_TABLE` | `0x0f70` | FEED ring cfg (core 3) |
| `PSRAM_CPU_TX_RING_DESC_TABLE` | `0x33e0` | TX ring cfg (core 2; BCM6813 project — 0x3360 was the wrong _FPI variant) |
| `CPU_TX_RING_INDICES_OFF` | `0x29c8` | TX `{read@0,write@2}` indices (core 2) |
| `CPU_TX_EGRESS_CREDIT_OFF` | `0x29d0` | egress dispatcher credit (3× u32) |
| `RING_RES_32` / `RING_RES_64` | 5 / 6 | number_of_entries = depth>>res_shift |
| `FPM_CHUNK_SIZE_DEFAULT` / `FPM_POOL_SIZE_512` | 512 / 32 MB | pool geometry |
| `QM_ENABLE_CTRL_STOCK` | `0x307` | full stock QM enable |
| `DSPTCHR_CPU_TX_EGRESS_VIQ` | 13 | the CPU_TX egress VIQ (verified live) |

Module params (all `0444`): `runner_emulated`, `mac_loopback`, `qm_enable` (default `0x307`), `serdes_fw_load`, `serdes_core`, `serdes_lane`, `route_a`, `route_a_grp`, `route_a_queue`, `route_a_tm_bb_id`, `route_a_tm_task`, `route_a_bbh_inst`.

---

## 4. Function reference (file order)

### Kernel-API compat shims (`< 5.0` vendor fork)

**`runner_compat_unregister_netdev(void *ndev)`** — `bcm4916_runner.c:85-88`. A devm-unwind trampoline that calls `unregister_netdev`. Used only by the synthesized `devm_register_netdev`.

**`devm_register_netdev(struct device *, struct net_device *)`** — `bcm4916_runner.c:89-98`. Synthesizes the mainline helper (absent in the 4.19 vendor fork) from `register_netdev()` + `devm_add_action_or_reset`. Also present as macros: `platform_get_irq_byname_optional` → `platform_get_irq_byname` (`:76`), and a `netif_napi_add` weight-argument shim (`:80-81`).

### FPM pool driver

**`fpm_alloc_token(struct runner_priv *p, int size)`** — `bcm4916_runner.c:238-245`. Allocates one FPM buffer by **reading** `FPM_POOL0_ALLOC_DEALLOC`; returns the token if `FPM_TOKEN_VALID` (bit31), else 0. **The `size` argument is ignored** (the pool is fixed 512-B chunks). Caller: `runner_start_xmit`. Ported from GPL `fpm_priv.h __fpm_alloc_token`.

**`fpm_free_token(struct runner_priv *p, u32 token)`** — `bcm4916_runner.c:247-250`, `__maybe_unused`. Frees a token by **writing** it back to the same register. Currently unused (TX sets `do_not_recycle=0`, so the Runner auto-frees).

**`fpm_token_to_buf` / `fpm_token_to_phys`** — `bcm4916_runner.c:252-260`. Compute the buffer virt/phys as `pool_base + FPM_TOKEN_INDEX(token) * chunk_size`.

**`fpm_token_to_bn(u32 token)`** — `bcm4916_runner.c:264-267`. Converts an FPM token to a Runner buffer number: `INDEX | (POOL<<17)`. Marked `[VERIFIED vs fpm_core.c:376]`. Used to fill TX `word3`.

**`readl_poll_drain(struct runner_priv *p)`** — `bcm4916_runner.c:270-279`. Bounded (1000× 10 µs) poll for `FPM_CTL_INIT_MEM` (bit4) to self-clear after a memory-init trigger. Returns silently on timeout — a **best-effort** poll, not error-checked.

**`fpm_pool_init(struct runner_priv *p)`** — `bcm4916_runner.c:287-320`. FPM bring-up (ABI bring-up step 3):
1. `dmam_alloc_coherent` a 32 MB pool (`pool_pbase`).
2. `FPM_POOL1_CFG1` = `FPM_FP_BUF_SIZE_512` (chunk = 512 B).
3. `FPM_POOL1_CFG2` = pool phys & `0xfffffffc` (4-byte aligned).
4. `FPM_SPARE` = `FPM_NET_BUF_HEAD_PAD(240) << 16`.
5. Read-modify-write `FPM_CTL |= INIT_MEM`; `readl_poll_drain`; then `FPM_CTL |= POOL1_ENABLE`.
Returns `-ENOMEM` on alloc failure. Callee: `readl_poll_drain`. Caller: `runner_probe`. **Note:** the base register is masked `& 0xfffffffc` and cast to `u32`, so only the low 32 bits of a 40-bit pool phys are programmed (see findings).

### CPU RX feed + delivery rings

**`rx_buf_phys` / `rx_buf_virt` / `rx_buf_index`** (inline) — `bcm4916_runner.c:340-351`. Arithmetic phys↔virt↔index over the contiguous RX buffer pool (`slot i` at `base + i*RX_BUF_SIZE`, `RX_BUF_SIZE=2048`).

**`feed_post(struct runner_priv *p, u32 slot, u32 buf_idx)`** — `bcm4916_runner.c:354-363`. Writes feed slot `slot` to point at buffer `buf_idx` as a 40-bit ABS pointer: `ptr_low = phys low32`, `w1 = FEED_W1_ABS | (phys_hi & 0xff)`, both `cpu_to_be32`. Callers: `rx_ring_alloc` (initial fill), `runner_rx_poll` (recycle).

**`feed_doorbell(struct runner_priv *p, u16 widx)`** — `bcm4916_runner.c:366-372`. `dma_wmb()` then writes the feed ring's `write_idx` (BE u16) to core-3 SRAM `RDD_FEED_RING_DESC_TABLE + 6`. This is the "feed doorbell" (`rdd_cpu_inc_feed_ring_write_idx`). Callers: `rx_ring_publish`, `runner_rx_poll`.

**`rx_deliv_write_idx` / `rx_deliv_set_read_idx`** — `bcm4916_runner.c:375-385`. Read the runner-advanced delivery `write_idx` (@+6) and write the host-advanced `read_idx` (@+12) in core-3 SRAM.

**`ring_publish(p, core, tbl_off, phys, depth, entry_sz, irq, res_shift)`** — `bcm4916_runner.c:402-413`. Writes a `runner_ring_cfg` control block into a core's data SRAM. Encoding: `w0 = (entry_sz<<27) | ((depth>>res_shift)<<16) | irq`; `base_addr_low = phys low32`; `w3 = phys_hi & 0xff`; all BE via `memcpy_toio`. **Critical ordering/encoding fact:** `res_shift` differs per ring — RX-delivery and TX rings are 32-resolution (`RING_RES_32=5`), the FEED ring is 64-resolution (`RING_RES_64=6`). Using `>>6` for the RX ring made the microcode think the ring was half-size and wrap math broke, so it never delivered (`bcm4916_runner.c:388-399`). `size_of_entry` is the entry size in **bytes**, not log2. Callers: `rx_ring_publish`, `tx_ring_alloc`.

**`rx_ring_alloc(struct runner_priv *p)`** — `bcm4916_runner.c:415-439`. Allocates three coherent regions: the delivery ring (256×16 B), the feed ring (256×8 B) and the RX buffer pool (256×2048 B). Sets `rx_head=0`, pre-posts `FEED_RING_DEPTH-1` (255) empty buffers (leaves one slot free so the ring is never "full == empty"), and sets `feed_widx=255`. Returns `-ENOMEM` on any alloc failure. Caller: `runner_probe`.

**`rx_ring_publish(struct runner_priv *p)`** — `bcm4916_runner.c:441-453`. Publishes both control blocks on core 3 (delivery ring 32-res, feed ring 64-res) then rings the feed doorbell so the runner sees the pre-posted buffers. Caller: `runner_probe`.

**`runner_rx_poll(struct napi_struct *napi, int budget)`** — `bcm4916_runner.c:455-517`. The NAPI RX handler. Loop while `done < budget` and `(u16)rx_head != write_idx`:
- Parse the descriptor: `bphys = word0 | (ptr_hi<<32)`, `len = word1[15:2]`, `bidx = rx_buf_index(bphys)`.
- If `bidx >= FEED_RING_DEPTH` the descriptor points outside our pool → count `rx_errors`, skip (cannot recycle).
- If `0 < len <= RX_BUF_SIZE`: `dma_sync_single_for_cpu`, `napi_alloc_skb`, `skb_put_data`, `eth_type_trans`, `napi_gro_receive`; else `rx_errors`.
- Recycle: `dma_sync_single_for_device`, `feed_post(feed_widx % DEPTH, bidx)`, `feed_widx++`.
- Advance `rx_head` mod `RX_RING_DEPTH`.
After the loop, if any frames were processed: ring the feed doorbell and write back `read_idx`. If `done < budget`: `napi_complete_done` and — **only if `rx_irq > 0`** — `enable_irq`. Callees: `rx_deliv_write_idx`, `rx_buf_index`, `dma_sync_*`, `feed_post`, `feed_doorbell`, `rx_deliv_set_read_idx`. Called by NAPI. See findings for the poll-mode self-sustain gap.

**`runner_rx_isr(int irq, void *dev_id)`** — `bcm4916_runner.c:519-526`. `disable_irq_nosync` + `napi_schedule`; the poll re-enables. Registered only when `rx_irq > 0`.

### CPU TX path

**`tx_ring_doorbell(struct runner_priv *p, u16 write_idx)`** — `bcm4916_runner.c:534-558`. Two-step doorbell:
1. `dma_wmb()`, then write the BE u16 `write_idx` to core-2 SRAM `CPU_TX_RING_INDICES_OFF + 2` (the `write_idx` slot; +0 is `read_idx`). Writing +0 on every core was an earlier bug that clobbered read_idx.
2. `wmb()`, then `writel(RNR_CPU_TX_THREAD, rnr_regs[core2] + RNR_CFG_CPU_WAKEUP)` — the per-frame edge wakeup that actually makes the thread run.

**`runner_start_xmit(struct sk_buff *skb, struct net_device *ndev)`** — `bcm4916_runner.c:560-624`. `ndo_start_xmit`:
- Drops (`tx_dropped`) if `len > chunk_size` (512).
- `fpm_alloc_token`; on failure `netif_stop_queue` + `NETDEV_TX_BUSY`.
- Copy skb into the FPM buffer, `dma_sync_single_for_device`.
- Build the descriptor: `word0 = IS_EGRESS | (len<<8) | (route_a ? first_level_q=route_a_queue : 0)`; `word1=0`; `word2 = IS_EMAC | (RUNNER_FIRST_PORT<<20)`; `word3 = fpm_bn0 | (sop=0)`. `abs=0` (FPM-token mode), `do_not_recycle=0`.
- Advance `tx_write_idx` mod 256, ring the doorbell, bump `tx_packets`/`tx_bytes`, `dev_consume_skb_any`.
The comment (`:597-609`) explains why the egress EMAC port field **must** name the port actually brought up (`RUNNER_FIRST_PORT`); leaving it 0 aimed frames at an unconfigured EMAC and stalled the TX thread after the BBH_TX FIFO filled (~3 frames). Callees: `fpm_alloc_token`, `fpm_token_to_buf/phys/bn`, `tx_ring_doorbell`.

**`tx_ring_alloc(struct runner_priv *p)`** — `bcm4916_runner.c:632-647`. Allocates the coherent TX ring (256×16 B), sets `tx_write_idx=0`, and publishes the control block on core 2 at `PSRAM_CPU_TX_RING_DESC_TABLE(0x33e0)` with `RING_RES_32`. Caller: `runner_probe`.

### NAT-C offload MMIO (owned here, called from `flow_offload.c`)

**`xrdp_natc_add(o, key, ctx, idx_out)`** — `bcm4916_runner.c:660-684`. Stages the 16-byte masked BE key at `PSRAM + NATC_STAGE_KEY(0x100)`, the variable-length context at `NATC_STAGE_CTX(0x120)`, writes the table index at `NATC_INDIR_INDEX(0x200)`, then `dma_wmb()` + writes `NATC_CMD_ADD(3)` to `NATC_INDIR_CMD(0x204)`. Allocates the index from a simple `natc_next_idx++` counter. The PSRAM staging offsets + command register are **contract placeholders** shared with the QEMU model (real RDD/NAT-C indirect offsets are an ABI unknown).

**`xrdp_natc_del(o, key, idx)`** — `bcm4916_runner.c:686-695`. Stages the key + index, issues `NATC_CMD_DEL(4)`.

**`xrdp_natc_stats(o, idx, pkts, bytes)`** — `bcm4916_runner.c:697-706`. Returns 0/0: per-flow CNPL counter read-back is not yet pinned; the flowtable still ages entries out by its own timeout.

### Microcode load

**`runner_load_microcode(struct runner_priv *p)`** — `bcm4916_runner.c:709-798`. In emulated mode (module param or `brcm,runner-emulated` DT prop) it skips the load, sets `fw_loaded=false`, returns 0. Otherwise `request_firmware("brcm/bcm4916-runner-microcode.bin")`; **absence is non-fatal** (warns, `fw_loaded=false`, returns 0) so the driver can still bind. On success it parses the `RFW1` container: a 32-byte little-endian header (magic, ver, num_cores, hdr_size, entry_size, total, rsvd[2]), then per-core `{inst_off, inst_len, pred_off, pred_len}`, then inst images (32 KB) + prediction images (u16-packed, 1 KB). For each core it validates extents, then — because the Runner SRAM is big-endian while the blob is native little-endian — writes each INST word via `iowrite32be(get_unaligned_le32(...))` to `rnr_mem[c] + XRDP_RNR_INST_OFF`, and each PRED u16 as a BE u32 (stride 4) to `rnr_mem[c] + XRDP_RNR_PRED_OFF`. Rejects on bad magic / short file / bad table / bad extent (`-EINVAL`). The firmware itself is proprietary and non-redistributable; the driver only loads it. Caller: `runner_probe`.

### Bring-up: UBUS, RNR cores, thread regfiles

**`runner_ubus_decode_init(struct runner_priv *p)`** — `bcm4916_runner.c:807-821`. Programs the five UBUS_SLV address-decode windows (each a START+END pair holding full 32-bit addresses, no mask/enable): dev0 FPM `[0x82a00000,0x82c00000)`, dev1 QM `[0x82c00000,0x82c80000)`, dev2 DQM `[0x82c80000,0x82d00000)`, vpb `[0x82700000,0x82900000)`, apb `[0x82900000,0x82a00000)`. Program order matches the SDK (dev0,dev1,dev2,vpb,apb). Marked `[PINNED vs SDK ubus_bridge_init]`.

**`runner_rnr_precfg(struct runner_priv *p)`** — `bcm4916_runner.c:833-868`. Per-core pre-microcode config. Computes the DDR DMA base from the FPM pool phys: `DMA_BASE[19:0] = (phys_hi<<12) | (phys_lo>>20)` (a 40-bit fold), OR'd with `BUF_SIZE_MODE(b23)`. For each core: trigger data+context-SRAM zeroing (`GEN_CFG = DIS_DMA_OLD_FC | ZERO_DATA_MEM | ZERO_CTX_MEM`), poll the DONE bits (b4+b5) — the zero bits do **not** self-clear (`re-notes/realhw/10` Wave-7); then write `SCH_CFG=4` (DRV_RNR_16SP), `PSRAM_CFG=0x820`, `DDR_CFG=ddr_cfg`. Runs **before** microcode (which targets separate INST/PRED SRAM). Caller: `runner_probe`.

**`runner_thread_regfile_init(struct runner_priv *p)`** — `bcm4916_runner.c:880-905`. Writes the initial register file for the CPU_RX (core3/thread1) and CPU_TX (core2/thread6) threads into the context SRAM (`rnr_mem[core] + XRDP_RNR_CNTXT_OFF(0x18000) + thread*128 + reg*4`, big-endian). Without this a woken thread runs on zeroed registers (no entry PC/stack/FIFO ptrs) and does nothing. RX writes R0=entry (`0x1624`, the `cpu_rx_wakeup_request` label), R8/R17=PD_FIFO_TABLE (`0x3940`), R9/R18=UPDATE_FIFO_TABLE (`0x3840`), R30=stack top (`0x2bd0`), R31=CONST_1(`1`). TX writes R0=entry (`0x0215`), R8=thread number(6), R30=stack top(`0x3340`), R31=1. **These register values are specific to the exact BCM6813 microcode image loaded** (`bcm4916_runner.c:876-878`) — re-derive if a different microcode is used. Must run **after** zero-mem + microcode load and **before** core enable/wakeup. Caller: `runner_probe`.

**`runner_rnr_enable(struct runner_priv *p)`** — `bcm4916_runner.c:907-932`. The **last** bring-up step: RMW `GLOBAL_CTRL |= EN(b0)` on every core, then `dma_wmb()` and issue the initial `CFG_CPU_WAKEUP` to start the CPU_RX (core3/thread1) and CPU_TX (core2/thread6) host threads. Caller: `runner_probe` (last MMIO step before netdev registration).

### Bring-up: SBPM, DSPTCHR, QM, BBH

**`runner_sbpm_init(struct runner_priv *p)`** — `bcm4916_runner.c:939-955`. Trigger the SBPM free-list init (`INIT_FREE_LIST = 0x03FFC000`, INIT_OFFSET `0xFFF<<14` → ~0x1000 buffers), poll RDY (b31). Timeout is non-fatal (warns, returns 0). Caller: `runner_probe`.

**`runner_dsptchr_cpu_rx_setup(struct runner_priv *p)`** — `bcm4916_runner.c:966-1003`. Wires the CPU_RX delivery path through the dispatcher for `RUNNER_FIRST_PORT`:
- `viq = RUNNER_FIRST_PORT` (VIQ==bbh_id), `grp = DSPTCHR_CPU_RX_GROUP(1)`, `bb_id = 31 + 2*RUNNER_FIRST_PORT`.
- Program the feeding VIQ credit config (`QUEUE_CRDT_CFG + 4*viq` = `bb_id | (NORMAL<<8)`), Q_DEST=0 (dispatcher), place core3/thread1 in the group (`MASK_MSK_TSK` word = `CPU_RX_RING_CORE/2` → yields offset `0x524`, value `0x20000`), make the group consume the VIQ (`MASK_MSK_Q + 4*grp`), map the task→group (3-bit field in `TSK_TO_RG_MAPPING`), set the group's available-task count, set the delivery address (`PD_DSPTCH_ADD + 4*core3` = `DSPTCHR_CPU_RX_PD_ADDR = 0x3940>>3 = 0x728`), and finally enable the VIQ (`VQ_EN`). Several field encodings are RE-derived but **unconfirmed on silicon** (`:963-964`). Caller: `runner_dsptchr_init`.

**`runner_dsptchr_cpu_tx_setup(struct runner_priv *p)`** — `bcm4916_runner.c:1013-1043`. Registers the CPU_TX **egress (delayed-credit) VIQ 13** so the dispatcher grants egress credits to the CPU_TX thread; without it the credit table (core-2 @0x29d0) stays 0 and the thread stalls after its initial buffers (observed `read_idx` frozen at 3). Writes `crdt_cfg[13] = (CPU_TX_CRDT_TGT<<16) | bb_id` (matches the live stock value `0x653A0002`), marks it a delayed queue (`MASK_DLY_Q`), sets ingress limits (`0x23ff` = CMN_MAX 0x3FF, GURNTD 8), seeds `EGRS_DLY_QM_CRDT=8`, enables the VIQ. `q_dest` is intentionally left unwritten (stock leaves it `0xDEADBEEF`). Caller: `runner_dsptchr_init`.

**`runner_qm_init(struct runner_priv *p)`** — `bcm4916_runner.c:1053-1093`. **Route A only.** Steps: (1) QM SRAM auto-init (`MEM_AUTO_INIT_EN` → poll `MEM_INIT_DONE`, warn on timeout); (2) `FPM_BASE_ADDR = pool_pbase>>8` (256-B res) + `DDR_SOP_OFFSET=18`; (3) bind `RUNNER_GRP[route_a_grp]`: QUEUE_CONFIG start=end=`route_a_queue`, RNR_CONFIG = `route_a_tm_bb_id | (route_a_tm_task<<8) | ENABLE`; (4) `ENABLE_CTRL = qm_enable ? qm_enable : 0x307`. The SRAM auto-init in step 1 is what makes 0x307 safe. Caller: `runner_probe` (only if `route_a`). See findings for omitted QM sub-steps.

**`runner_bbh_tx_route_a(struct runner_priv *p)`** — `bcm4916_runner.c:1100-1117`. **Route A only.** For BBH_TX instance `route_a_bbh_inst`: set `BBCFG_2` PDRNR0SRC = TM core bb_id (so BBH_TX accepts the TM core's PDs), set `RNRCFG_2` TASK = TM egress thread (PTRADDR left 0 — ★silicon, the QM RUNNER_GRP drives the wakeup regardless), and set `QMQ=1` for queue 0 in both register views (`QMQ_LAN 0x4b0` and `QMQ_UNIFIED 0x7b0`) so the port takes PDs from the QM aggregator. Caller: `runner_probe` (only if `route_a`).

**`runner_dsptchr_init(struct runner_priv *p)`** — `bcm4916_runner.c:1119-1160`. Orchestrates the dispatcher: calls `runner_dsptchr_cpu_rx_setup` + `runner_dsptchr_cpu_tx_setup`, then (non-route-A only) writes `qm_enable` to `QM_ENABLE_CTRL` if non-zero, then triggers the reorder engine (`REORDER_CFG = EN | AUTO_INIT`) and polls RDY (b8). When `route_a` is set it deliberately does **not** write QM enable here — `runner_qm_init` owns the ordered QM bring-up. Non-fatal on RDY timeout. Caller: `runner_probe`.

**`runner_bbh_init(struct runner_priv *p, int rx_port)`** — `bcm4916_runner.c:1163-1196`. Configures + enables one BBH_RX MAC port and the LAN BBH_TX (instance 0). BBH_RX: `BBCFG` = SDMABBID | DISPBBID(18)<<8 | SBPMBBID(56)<<16 (SDMA0=21 for ports ≤5, else SDMA1=22); `DISPVIQ` = bbh_id | bbh_id<<8; `SDMAADDR=0`; `SDMACFG=0x404`; `SOPOFFSET=0`; `SBPMCFG=0xf`; then `ENABLE = PKTEN|SBPMEN` **last**. BBH_TX: `MACTYPE=1` (GPON; 7 is invalid), `BBCFG_1 = FPM(23)<<24 | SBPM(56)<<16`. Caller: `runner_probe`. Note this always uses BBH_TX instance 0; Route A separately touches `route_a_bbh_inst`.

### 1G MAC/PHY bring-up

**`runner_mac_phy_init(struct runner_priv *p, int unimac_inst)`** — `bcm4916_runner.c:1207-1307`. HW-only (skipped under emulation). Brings up the internal quad-EGPHY block then the UNIMAC:
- EGPHY (`QEGPHY_CTRL` at `ethphytop + 0xf014`): compute the target value (clear IDDQ/pwr-down/reset/phyad/refclk fields; set base MDIO addr=1, refclk=50 MHz), then a three-phase power sequence — phase 1: all ports powered down + in reset (40 µs); phase 2: power applied, still in reset (100 µs); phase 3: release reset (1 ms), poll `QEGPHY_STATUS` PLL_LOCK (b8) up to 100×100 µs. Powers all four internal GPHYs (`ext_pwr_down=0`).
- UNIMAC (`mac = xrdp + 0x8a8000 + inst*0x1000`): assert `SW_RESET`, set `FRM_LEN=0x3fff`, write `CMD` = SW_RESET | speed 1G | PROMIS | CRC_FWD | PAUSE_FWD | CNTL_FRM_ENA | NO_LGTH_CHK (+ `LOOP_ENA` if `mac_loopback`), release reset, then set TX_ENA|RX_ENA.
- Under the `<5.0` vendor kernel only: a best-effort diagnostic PHY/link readback via the stock built-in SF2 MDIO accessor (resolved by kallsyms), reading BMCR/BMSR/PHYID at MDIO addr 2.
Caller: `runner_probe`.

### Offload self-test + ringstat (debugfs)

**`runner_selftest_write`** — `bcm4916_runner.c:1318-1343`. Parses `"<vlan_op> <vid>"` written to `.../offload_selftest`, calls `xrdp_offload_selftest` with fixed test DA (`ff:ff:ff:ff:ff:ff`) / SA (`02:00:00:00:00:01`) to program one NAT-C L2 flow. `fops` at `:1345-1348`.

**`runner_nat_selftest_write`** — `bcm4916_runner.c:1366-1388`. Writing any non-empty string to `.../offload_nat_selftest` programs one routed IPv4 SNAT+NAPT flow using TEST-NET / documentation-range addresses (192.0.2.10:4096 → 198.51.100.20:80 TCP, rewritten src 203.0.113.5:5000). `fops` at `:1390-1393`.

**`runner_ringstat_read`** — `bcm4916_runner.c:1401-1449`. Read `.../ringstat`: dumps host vs runner indices to prove the microcode is servicing the datapath — TX runner `read_idx` (@core2 `0x29c8+0`), RX runner `write_idx` (@core3 `0x3000+6`), FEED runner `read_idx` (@core3 `0x0f70+12`), the 3× egress credit words + sync-fifo ptrs (core 2), FPM tokens-available, and driver stats. `fops` at `:1451-1454`. This is the primary bring-up telemetry.

**`runner_debugfs_init`** — `bcm4916_runner.c:1456-1464`. Creates the `bcm4916-runner` debugfs dir with the three files above.

### netdev ops

**`runner_ndo_open`** — `bcm4916_runner.c:1467-1476`. `napi_enable`, `netif_start_queue`; if `rx_irq <= 0` (poll mode) `napi_schedule` once. **See findings: poll-mode RX is not self-sustaining.**

**`runner_ndo_stop`** — `bcm4916_runner.c:1478-1485`. `netif_stop_queue`, `napi_disable`.

**`runner_ndo_setup_tc`** — `bcm4916_runner.c:1492-1498`. Forwards TC_SETUP_FT/BLOCK to `xrdp_offload_setup_tc` (flowtable HW-offload block registration; modelled on `mtk_eth_setup_tc`).

`runner_netdev_ops` — `bcm4916_runner.c:1500-1507`: open/stop/start_xmit/setup_tc + `eth_mac_addr` + `eth_validate_addr`.

### 10G XPORT SerDes (merlin16) — opt-in first step

**`serdes_xfer(p, reg, mask, val, write, out)`** — `bcm4916_runner.c:1517-1547`. One indirect merlin16 transaction over the ADDR/MASK/CNTRL triplet at `serdes + serdes_core*0x100`: encode the lane register as `(PMD_DEV<<27)|(serdes_lane<<16)|reg`, write ADDR, set MASK (`~mask` for writes), issue CNTRL (`START_BUSY | DELAYED_ACK` + `RW` for reads + data for writes), poll `START_BUSY` clear (1000×1 µs), `-ETIMEDOUT` on stuck, return masked read data.

**`serdes_wr_reg` / `serdes_wr_f` / `serdes_rd`** (inline) — `bcm4916_runner.c:1549-1562`. Convenience wrappers (full-word write, field write with `{mask,shift}`, masked read).

**`serdes_poll_ra_initdone`** — `bcm4916_runner.c:1564-1576`. Poll `SRD_MICRO_AHB_STATUS` `RA_INITDONE` (250×1 µs).

**`runner_serdes_load(struct runner_priv *p)`** — `bcm4916_runner.c:1578-1670`. Step 1 of 10G bring-up: `request_firmware("brcm/merlin16-shortfin.bin")` (expected 31664 B), assert uC reset (clocks off, micro-reg block to defaults from a `zero_regs[]` table + three non-zero seeds), toggle subsystem reset, init code+data RAM (poll RA_INITDONE twice), set the program-RAM write port (autoinc, 16-bit words, addr 0), **stream the blob as raw 16-bit LE words** into `RA_WRDATA_LSW`, release/start the uC, and confirm `uc_active` (b15, 10000×1 µs). Returns `-EIO` if the uC never goes active, `-ETIMEDOUT` on RAM-init timeout, or the `request_firmware` error. The full PLL/lane/AN link is a larger follow-on. The SerDes firmware is proprietary/non-redistributable (loaded, not shipped). Caller: `runner_probe` (only if `serdes_fw_load`).

### Probe / remove / driver registration

**`runner_probe(struct platform_device *pdev)`** — `bcm4916_runner.c:1672-1835`. The full sequence:
1. `dma_set_mask_and_coherent(40)`, fall back to 32, warn-and-continue if refused.
2. `devm_alloc_etherdev`, wire `runner_priv`, `platform_set_drvdata`.
3. Fold the `brcm,runner-emulated` DT prop into `runner_emulated`.
4. `devm_ioremap` the XRDP window (no request_mem_region); map `ethphytop` (HW only) and `serdes` (HW + opt-in); compute `fpm`/`natc`/`rnr_mem[]`/`rnr_regs[]`.
5. `rx_irq = platform_get_irq_byname_optional(pdev, "queue0")`; negative → 0 (poll mode).
6. **Bring-up in order** (`:1759-1794`): `runner_ubus_decode_init` → `fpm_pool_init` → `runner_rnr_precfg` → `runner_load_microcode` → `runner_sbpm_init` → `runner_dsptchr_init` → (route_a) `runner_qm_init` → `rx_ring_alloc` → `tx_ring_alloc` → `rx_ring_publish` → (HW) `runner_mac_phy_init` → (HW+opt-in) `runner_serdes_load` → `runner_bbh_init` → (route_a) `runner_bbh_tx_route_a` → `runner_thread_regfile_init` → `runner_rnr_enable` (**last**).
7. `netif_napi_add`, set `netdev_ops`, `min_mtu=ETH_MIN_MTU`, `max_mtu = chunk_size - ETH_HLEN` (= 498), random MAC.
8. Init offload (`xrdp_offload_init`), advertise `NETIF_F_HW_TC`, create debugfs.
9. Force the netdev name to `rnr%d` (so it never collides with DSA user-port `eth%d` labels).
10. Request the RX IRQ if present, `devm_register_netdev`.
Returns 0 on success; propagates `-ENOMEM`/alloc/`request_firmware(-EINVAL)` failures. Any error after coherent allocs relies on devm unwind.

**`runner_remove(struct platform_device *pdev)`** — `bcm4916_runner.c:1839-1868`. Return type is version-gated (int `<6.11`, void ≥). **Quiesces the Runner before devm frees the DMA rings**: writes `GLOBAL_CTRL=0` on every core (the cores keep DMAing into the rings otherwise), `wmb()` + `mdelay(2)` to let in-flight DMA settle, then `debugfs_remove_recursive` + `xrdp_offload_deinit`.

**`runner_of_match[]`** — `bcm4916_runner.c:1870-1884`. Binds `brcm,bcm4916-runner` **and** the stock `brcm,rdpa` node (so on a trial boot where the stock datapath is not loaded, the driver can claim the real reg window as its own device resource, with the DT device's DMA config and the real `queue0` IRQ, with no DT change).

**`module_platform_driver(runner_driver)`** — `bcm4916_runner.c:1886-1894`. Registers the platform driver (`.name = "bcm4916-runner"`). `MODULE_FIRMWARE(RUNNER_FW_NAME)` at `:1899`.

### The platform_device shim (`driver/trial/bcm4916_runner_pdev.c`)

**`runner_pdev_init`** — `bcm4916_runner_pdev.c:37-65`. Registers a `platform_device` named `bcm4916-runner` with a single MEM resource (`0x82000000 + 0xcaf004`, verbatim from the stock rdpa node) and a 40-bit DMA mask, so the platform bus binds it to the driver **by name** (no of_node) on a stock kernel built without `CONFIG_OF_OVERLAY`. Critically it then calls `of_dma_configure(&pdev->dev, NULL, true)` — a bare non-DT platform_device gets no dma_ops otherwise and the driver's coherent allocs would fail. The CPU-RX IRQ (GIC SPI 75) is intentionally omitted → the driver runs poll mode.

**`runner_pdev_exit`** — `bcm4916_runner_pdev.c:67-71`. `platform_device_unregister`. The documented takeover flow: rmmod the stock datapath first (frees the window), `insmod bcm4916-runner.ko` then `insmod bcm4916-runner-pdev.ko`; unload in reverse to abort. No flash / boot-state change → a plain reboot restores stock.

---

## 5. Audit findings

### High severity

**H1 — Default (non-Route-A) QM enable writes `0x307` with no SRAM auto-init → documented SoC-hang risk on live silicon.** `qm_enable` defaults to `QM_ENABLE_CTRL_STOCK = 0x307` (`bcm4916_runner.c:127`). In the non-route-A path, `runner_dsptchr_init` writes that value straight to `QM_ENABLE_CTRL` (`bcm4916_runner.c:1142-1145`) **without** the `MEM_AUTO_INIT` step, which only runs inside the route-A-gated `runner_qm_init`. Both the header (`bcm4916_runner.h:106-108`) and `re-notes/realhw/11` §A state that enabling `fpm_prefetch` (bit0 of 0x307) on dirty silicon before SRAM auto-init makes QM read garbage and hangs the SoC. So a plain `insmod` (no `route_a`, default `qm_enable`) on real hardware risks the hang; the emulator masks this because it clears immediately. A safe live bring-up requires `qm_enable=0` or `route_a=1`. This default is silicon-unsafe.

**H2 — TX is limited to ~512-byte frames; standard-MTU traffic is impossible.** The FPM pool uses 512-byte chunks (`FPM_CHUNK_SIZE_DEFAULT=512`, `bcm4916_runner.h:416`), the driver copies the whole skb into a single chunk, and `runner_start_xmit` drops any frame with `len > chunk_size` (`bcm4916_runner.c:569-573`). `max_mtu` is set to `chunk_size - ETH_HLEN = 498` (`bcm4916_runner.c:1799`). There is no multi-chunk / SBPM buffer chaining, so the conduit cannot transmit a normal 1500-byte frame. RX_BUF_SIZE is 2048 (RX can receive larger), so the limitation is asymmetric.

**H3 — Poll-mode RX (the pdev-shim path) is not self-sustaining.** When `rx_irq <= 0`, `runner_ndo_open` schedules NAPI exactly once (`bcm4916_runner.c:1473-1474`). `runner_rx_poll` re-enables/re-arms only via `enable_irq`, and only when `rx_irq > 0` (`bcm4916_runner.c:511-515`); once it drains below budget and calls `napi_complete_done`, nothing reschedules the poll. Since the `bcm4916_runner_pdev.c` shim deliberately omits the IRQ (`:12-15`), that takeover path can service at most one NAPI pass of RX and then stops. A periodic poll (timer/hrtimer) or the IRQ is required for continuous RX in poll mode.

### Medium severity

**M1 — TX ring can be overrun; only FPM-token exhaustion provides backpressure, and the stop/wake handshake is broken.** `runner_start_xmit` never checks the Runner's TX `read_idx` before advancing `tx_write_idx` (mod 256) (`bcm4916_runner.c:617`). With ~65536 FPM tokens (32 MB / 512 B) but only 256 ring slots, if the Runner stalls the 256-slot ring wraps and clobbers undelivered descriptors long before tokens run out. Separately, on token exhaustion the code calls `netif_stop_queue` + returns `NETDEV_TX_BUSY` (`:576-580`) but **nothing ever calls `netif_wake_queue`** — no TX-completion path re-arms the queue — so after a single FPM exhaustion the queue can remain stopped permanently.

**M2 — Route A QM init omits queue-context / WRED / DQM-enable and the FIFO configs the spec requires.** `runner_qm_init` (`bcm4916_runner.c:1053-1093`) programs only MEM_AUTO_INIT, FPM_BASE/DDR_SOP, one RUNNER_GRP's QUEUE_CONFIG+RNR_CONFIG, and ENABLE. `re-notes/realhw/11` §A/§C additionally require: `FPM_CONTROL`, `FPM_COHERENT_BASE_ADDR`, `FPM_USR_GRP_*_THR`, clearing queue contexts, per-queue `qm_q_context_set` with a **pass** WRED profile (default is 15 = drop-all), `dqm_dqmol_cfgb_set(Q,1)` to enable the queue, and `RUNNER_GRP_{PDFIFO,UPDATE_FIFO}_CONFIG` (`0x308/0x30c`). As written, the target QM queue would default to drop-all / disabled, so Route A egress is unlikely to actually pass frames without these steps. (`QM_FPM_POOLS_THR`, `QM_GLOBAL_FPM_CONTROL`, `QM_GLOBAL_AGGREGATION_CTRL`, `QM_GLOBAL_FPM_COHERENT_BASE_ADDR` are defined in the header but never written.)

**M3 — Route A default queue/grp values will overwrite live stock QM state.** All `route_a_*` params default to 0. With `route_a=1` and defaults unchanged, `runner_qm_init` programs `RUNNER_GRP[0]` to queue range `[0,0]` and RNR_CONFIG bb_id=0/task=0 (`bcm4916_runner.c:1078-1084`), overwriting the stock grp0 mapping the oracle captured (queues 80-111 → core7/task3). The user must pass a valid candidate set; the defaults are not merely inert for route A (unlike the rest of the driver). Documented as "pass the candidate set on insmod and iterate" (`bcm4916_runner.c:155-157`) but the failure mode (clobbering stock QM) is not guarded.

**M4 — `RUNNER_FIRST_PORT`/bbh_id mapping is an unverified guess.** `RUNNER_FIRST_PORT=1` (`bcm4916_runner.c:114`) is used simultaneously as UNIMAC instance, BBH_ID, and dispatcher VIQ. `re-notes/realhw/10` Wave-9 says the same 1G port ("eth2"=port_gphy1) is **bbh_id 0, VIQ 0** while UNIMAC inst = 1, i.e. the emac→bbh_id map is not the identity. The code comments acknowledge the exact mapping lives in the closed rdpa.ko and is unconfirmed (`:113`, `:1782-1784`). If bbh_id is actually 0, the dispatcher VIQ wiring (`bb_id = 31 + 2*1 = 33`) and `runner_bbh_init(rx_port=1)` target the wrong port.

**M5 — Route A BBH_TX binding uses `route_a_bbh_inst` but the plain BBH_TX setup is hardwired to instance 0.** `runner_bbh_init` always programs BBH_TX **instance 0** (`tx = xrdp + XRDP_OFF_BBH_TX0`, `bcm4916_runner.c:1167`), while `runner_bbh_tx_route_a` programs instance `route_a_bbh_inst` (pinned to 1 by the oracle). So under Route A, MACTYPE/BBCFG_1 are set on instance 0 but the QM-fed binding is set on instance 1 — the two halves of BBH_TX setup land on different instances. Instance 1's MACTYPE/BBCFG_1 are never written by the open driver (they may survive from stock, but that is not guaranteed after a takeover).

### Low severity / cosmetic

**L1 — `XRDP_OFF_QM` is `#define`d twice** (`bcm4916_runner.h:72` and `:81`), both to `0x00c00000`. Same value so it compiles, but redundant. `XRDP_OFF_DQM = 0x00c80034` (`:73`) is defined but unused, and is a base+`0x34` register address, not the block base (noted in `re-notes/realhw/10` block table).

**L2 — EGPHY control/status register offset carries an unresolved +4 ambiguity vs the RE note.** The header uses `QEGPHY_CTRL @ 0x837ff014`, `STATUS @ 0x837ff018`, `TEST_CNTRL @ 0x837ff010` citing `bchp_eth_phy_top_reg.h` (`bcm4916_runner.h:289-308`), whereas `re-notes/realhw/10` Wave-9 lists `QEGPHY_CTRL 0x837ff010`, `STATUS 0x837ff014`, `TEST_CTRL 0x837ff00c` — a consistent +4 shift. Only one can be right; unverified on silicon. A wrong choice means the PHY power-up writes land on the wrong register.

**L3 — `fpm_alloc_token`'s `size` argument is ignored** (`bcm4916_runner.c:238-245`); harmless but misleading (the pool is fixed 512-B chunks).

**L4 — `xrdp_natc_stats` always returns 0/0** (`bcm4916_runner.c:697-706`): the CNPL per-flow counter read-back is not implemented, so offloaded-flow byte/packet stats are absent (flows still age out by the flowtable timeout). Explicitly a Phase-1.x TODO.

**L5 — NAT-C staging offsets are contract placeholders, not silicon-verified.** `NATC_STAGE_KEY/CTX/INDIR_*` and the `NATC_CMD_*` values (`flow_offload.h:56-62`) are agreed only between the driver and the QEMU model; the real RDD/NAT-C indirect-register interface is an ABI unknown (`bcm4916_runner.c:655-659`). Not a slow-path issue, but any real-HW offload will need these re-derived.

**L6 — Several polled bring-up steps swallow timeouts as non-fatal** (FPM `readl_poll_drain`, SBPM RDY, DSPTCHR RDY, QM MEM_INIT_DONE): they `dev_warn` and continue (`bcm4916_runner.c:270-279, 946-954, 1068-1069, 1151-1159`). Reasonable for first-light diagnostics, but a production driver should fail probe when a mandatory block never signals ready (especially QM MEM_INIT_DONE, given H1).

### Known deviations from the stock oracle (documented in-tree, flagged for completeness)

- `re-notes/realhw/10` §"CPU ring/descriptor ABI" line 93 says the RX empty/ownership test "must key on WORD2"; the current driver instead uses the **feed-ring write_idx-vs-read_idx** model (Wave-4/5 supersedes that earlier note). The header's ownership comment still mentions word2/word3 (`bcm4916_runner.h:496-504`) but is unused by the polling code — a stale comment, not a code bug.
- `re-notes/realhw/10` lines 94-95 flag the TX `word2` byte layout and `TXD_W3_FPM_BN0/SOP` as "wrong"/"unverified", while the later Wave-7 (lines 232-235) and the header (`bcm4916_runner.h:585-588`, `bcm4916_runner.c:604-609`) mark them verified. The in-tree notes contradict each other; the FPM-token TX word3 layout is the one place where the spec's own "validation diffs" and its later wave disagree — worth an on-silicon confirmation before trusting FPM-mode TX.

---

## 6. Open questions / unknowns

1. **The `emac → bbh_id / VIQ` map for port_gphy1** (M4). Is the 1G first-light port bbh_id 0 or 1? Everything downstream (dispatcher VIQ, BBH_RX/TX instance, `RUNNER_FIRST_PORT`) depends on it and it is only in the closed rdpa.ko. Resolve by live oracle capture.
2. **The logical→physical QM queue map for Route A** (`re-notes/realhw/11` G2/G4). The oracle pinned `route_a_bbh_inst=1` and the RUNNER_GRP table, but the exact physical QM queue for port_gphy1's CPU-TX egress (port-relative q0 → which global queue in 80-111) is unresolved; candidates B (queue≈80/grp0/DS_TM) and A (queue0/grp1/US_TM) must be tried empirically.
3. **CPU_TX descriptor path selection**: whether egress needs an explicit `first_level_q` or relies on the microcode's `VPORT_TX_FLOW_TABLE` resolution (which needs a QoS table whose format is unknown). The driver only sets `first_level_q`; the vport-resolution path (`is_vport`, `flow_or_port_id`) is not wired.
4. **Minimal safe QM enable mask** — is `<0x307` (e.g. `0x7`) sufficient to drain, and does SRAM auto-init alone make the full 0x307 safe on live silicon (H1)? The QM `MEM_AUTO_INIT` done-bit was confirmed = bit0, but the complete safe-enable recipe is untested against real egress.
5. **The QM per-queue context / WRED / DQM-enable steps** (M2) — are they truly required for the QM-fed BBH_TX slow path, and what are the correct WRED-pass / fpm_ug values for a CPU-egress queue?
6. **EGPHY register offset** (L2): the +4 ambiguity between the header and the RE note must be settled on silicon.
7. **DSPTCHR CPU_RX field encodings** (`crdt_cfg` layout, the `TSK_TO_RG` global task id, the `0x3940>>3` PD wake address) are RE-derived but unconfirmed; RX has not been exercised on hardware, only TX.
8. **Whether the microcode thread register-file values** (R0 entry points, PD/UPDATE-FIFO/stack addresses) match the microcode actually extracted on a given device — they are hardcoded for one specific BCM6813 image and must be re-derived if a different microcode is loaded.
9. **The full 10G SerDes link** — `runner_serdes_load` only streams the ucode and confirms `uc_active`; PLL/VCO, lane AFE, speed/AN and PMD lock are unimplemented, so 10G ports do not yet pass traffic.
