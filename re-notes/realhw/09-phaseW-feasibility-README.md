# Phase-W (Runner takeover) feasibility + plan (2026-06-22)

Pursued the deadman-guarded takeover ("free the Runner, our driver owns it, move a frame").
Investigation verdict: **a clean takeover is NOT feasible with the current driver** — it would
require implementing the entire real-HW Runner bring-up, which is currently stubbed because the
driver was validated against the QEMU emulator.

## Why (evidence)
- Stock datapath load order (SDK `targets/96813GW` fs): `bdmf → rdpa_gpl → rdpa → rdpa_usr →
  rdpa_mw → bcm_enet → pktrunner → rdpa_cmd`. **`bcm_enet` (the management Ethernet that gives
  us 10.0.0.x) depends on this stack** — so freeing the Runner inherently drops the mgmt link
  (hence the takeover MUST be deadman-guarded + breadcrumb-instrumented).
- Our driver's `data_path_init` is a **24-step bring-up** but the real-HW steps are stubbed:
  - **Microcode load** (`load_runner_fw`): "Runner microcode … loaded (HW path TODO)" — it does
    NOT copy fw into RNR_INST/RNR_PRED or enable the 8 cores.
  - **UBUS decode windows**: TODO (offsets not pinned; FPM/QM/DQM/VPB/APB windows).
  - **BBH RX/TX, dispatcher, QM, DMA, SBPM per-port** register values: not pinned, stubbed.
  Only FPM + the CPU rings are attempted, "for the emulator".
- The Runner is a microcoded 8-core engine; without microcode + the full bring-up it does
  nothing. Our driver cannot cold-init it today.
- Coexistence (stock inits the Runner, our driver pokes alongside) is unsafe: the stock CPU
  ring tables are in use; writing them clobbers the stock CPU path (mgmt/exceptions), and raw
  XRDP MMIO has hung the SoC before (xrdp_peek on the FPM block).

## What this session DID pin toward the bring-up (Phase-R, SDK oracle, safe)
The SDK-source-as-oracle method is proven and is exactly how to close the gap. Pinned so far:
- CPU ring RDD offsets: RX `0x3000` (core 3), TX `0x3360` (core 2), TX-indices `0x29c8`.
- Per-core RNR-MEM base `0x82600000`, stride `0x20000` → abs RX `0x82663000`, TX `0x82643360`.
- RNR INST SRAM = core_base + `0x10000`, PRED RAM = core_base + `0x1c000` (already in driver).
- Microcode is present in the SDK: `rdp/projects/BCM6813_FPI/firmware_bin/runner_fw_N.c` (per-core
  32 KB inst) + `predict_runner_fw_N.c` (1 KB pred). FPM pool default `0x01020408`.

## Concrete plan to reach a real "move a frame" takeover (large but well-scoped)
Implement + pin each real-HW bring-up step against the SDK oracle, in order, building the driver
each time (safe, dev-build only) and unit-checking vs the SDK values:
1. **Microcode load HW path**: extract the 8 core inst/pred blobs → firmware blob; implement the
   copy to RNR_INST[core]/RNR_PRED[core] + the RNR_REGS/RNR_QUAD core-enable handshake (pin the
   enable sequence from `xrdp_drv_rnr*` / `rdp_drv_rnr`).
2. **UBUS_SLV decode windows** (FPM/QM/DQM/VPB/APB) — pin offsets+values.
3. **FPM init** (pool cfg 0x01020408, buffer mgmt).
4. **BBH RX/TX, dispatcher, QM, SBPM, DMA** per-port config — pin from the BCM6813 autogen.
5. Only then: deadman-guarded slot1 trial (stock datapath disabled via an init patch; our driver
   cold-inits the Runner; breadcrumb at each of the 24 steps; auto-revert on hang).

This is the real remaining engineering for an open 10G datapath on silicon. It's incremental and
safe up to step 5; step 5 is the one device-risky trial and is deadman/watchdog recoverable.

## Decision (autonomous)
Did NOT run a destructive takeover trial — with the bring-up stubbed it would only hang on the
first real-HW step (no information gained, only risk). The productive path is closing the
bring-up gap step-by-step (Phase-R → implement), which is safe. Device left untouched this turn
(SDK-source RE on dev-build only); on committed slot2 (10.0.0.8).
