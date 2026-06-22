# Phase-R validation — pin driver ABI vs the SDK oracle (2026-06-22)

User chose Phase-R (safe, read-only ABI validation) before any HW takeover. Two channels:

## Channel 1 — live stock stack (natc_dump.ko) : NAT-C empty (AP mode)
Built `tools/stock-watch/natc_dump.ko` with the working 4.19 module setup and loaded it on the
live device (read-only, kallsyms-resolved stock accessors, no register pokes). It loaded fine —
`key_get`/`res_get` resolved, tables=0xf, max_index=64 — but found **0 valid NAT-C entries**:
the device runs in **AP mode (NAT deferred)** so the connection table is empty. So NAT-C
cmdlist validation has nothing to diff against right now. (Capture mechanism is proven for when
NAT flows exist.)

## Channel 2 — SDK source as oracle (authoritative, cleaner than live reads) ★
The closed stack's RDD address map IS in the SDK source on dev-build for our exact chip:
`rdp/projects/BCM6813_FPI/drivers/rdd/auto/rdd_data_structures_auto.c`. The CPU-ring table
addresses there are the authoritative values the stock microcode/driver use — so our driver's
"TODO: pin RDD offset" placeholders were validated/corrected against them:

| driver macro | was (guess) | **PINNED (SDK oracle)** | Runner core |
|---|---|---|---|
| `PSRAM_CPU_RING_DESC_TABLE` (RX) `RDD_CPU_RING_DESCRIPTORS_TABLE_ADDRESS_ARR` | 0x0000 | **0x3000** | core 3 |
| `PSRAM_CPU_TX_RING_DESC_TABLE` `RDD_CPU_TX_RING_DESCRIPTOR_TABLE_ADDRESS_ARR` | 0x0080 | **0x3360** | core 2 |
| `CPU_TX_RING_INDICES_OFF` `RDD_CPU_TX_RING_INDICES_VALUES_TABLE_ADDRESS_ARR` | 0x0000 | **0x29c8** | — |

All three guesses were wrong — exactly what Phase-R is for. Driver updated
(`driver/runner/bcm4916_runner.{c,h}`) with the pinned values + provenance comments.
Also captured: `FPM_POOL_SET_0 = 0x01020408` (default FPM pool config, all chips).

### Step 1 DONE — per-core RNR-MEM base pinned (+ a second bug found)
Pinned the per-core RNR data-memory base from the SDK oracle: `RNR_MEM_ADDRS[]` (BCM6837 &
BCM6888 autogen, same XRDP generation) = base **0x82600000**, stride **0x20000**; and the
BCM6813_FPI `runner_fw` addresses (0x82610000 = core0+inst, 0x82648000 = core2+data,
0x826E8000 = core7+data) confirm base 0x82600000 for the 6813 specifically.

★Second placeholder bug found: the driver had `XRDP_OFF_RNR_MEM0 = 0x700000` — wrong (that is
core-8 in the 14-core 6888 map, past the 6813's 8 cores). Corrected to **0x600000**. Also the
RX/TX ring-descriptor publishes were writing to **PSRAM (offset 0)**; the rings actually live
in **per-core RNR data memory**, so they were retargeted to `rnr_mem[core]` (RX core 3, TX
core 2). Absolute XRDP-window addresses now:
  - RX ring descriptors  = 0x82600000 + 3*0x20000 + 0x3000 = **0x82663000**
  - TX ring descriptors  = 0x82600000 + 2*0x20000 + 0x3360 = **0x82643360**
  - TX ring indices      = 0x82600000 + 2*0x20000 + 0x29c8 = **0x826429c8**
Driver updated (`XRDP_OFF_RNR_MEM0`, `CPU_RX_RING_CORE`/`CPU_TX_RING_CORE`, the two publishes)
and rebuilds clean as the 4.19 module. The CPU-ring publish path is now fully addressed.

### Remaining for the takeover (Phase-W)
Per-core RNR base done; next is the deadman-guarded takeover itself: build a dev-build firmware
variant with the stock RDPA/Runner drivers disabled (so the Runner is free), boot it
(auto-revert), then probe our driver to own the Runner and move real frames. (Verify the TX
ring-indices per-core target — the indices loop currently writes all cores; the RDD ADDRESS_ARR
says the indices table is on the TX core only.)

Method note: validating against the SDK source (the closed stack as oracle) is the cleanest
Phase-R lever here — authoritative, no device risk, and works even in AP mode with no live
flows. See [[wifi-style-strategy]], [[stock-driver-watch-lever]].
