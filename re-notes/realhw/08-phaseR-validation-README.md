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

### Remaining gap (next Phase-R step)
These are **RDD / per-core-data-memory offsets**, not absolute XRDP-window addresses. The RX
ring lives on Runner **core 3**, the TX ring on **core 2**. To turn them into the absolute
addresses our driver writes through the XRDP window (`XRDP_WINDOW_BASE 0x82000000`), we still
need the **per-core RNR data-memory base** in the XRDP map (the "RNR-MEM base" gap). Pin that
next from the same SDK source (rdp_drv / RU_BLK RNR_MEM block addresses), then the CPU-ring
publish path is fully addressed for the eventual deadman-guarded Phase-W takeover.

Method note: validating against the SDK source (the closed stack as oracle) is the cleanest
Phase-R lever here — authoritative, no device risk, and works even in AP mode with no live
flows. See [[wifi-style-strategy]], [[stock-driver-watch-lever]].
