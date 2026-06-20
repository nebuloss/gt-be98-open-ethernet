# bcm4916_runner — open BCM4916 XRDP Runner CPU-conduit (slow-path datapath)

Open, mainline-style Linux driver for the Broadcom **BCM4916 / BCM6813** XRDP
"Runner" **CPU-port conduit**. It is the **DSA conduit (master) netdev** for the
GT-BE98 integrated switch: it owns the XRDP CPU-port host ring and moves
trapped / CPU-forwarded Ethernet frames between the Runner and Linux. The DSA
switch driver (`b53`/`bcm_sf2`, BCM4916 variant — see
`driver/mainline-patches/`) attaches its CPU/IMP port to this netdev.

In the QEMU bring-up it **replaces the Cadence GEM stand-in** that played the
conduit role in the earlier control-plane harness with the real Runner
CPU-ring datapath.

## Scope

**Slow path** — CPU-forwarded frames (RX trap-to-host + host TX) — **plus
Phase-1 L2/VLAN HW flow-offload**. The offload fast path (NAT-C connection table
+ XPE cmdlist + `FC_UCAST_FLOW_CONTEXT_ENTRY`) is implemented for BOTH:
- **Phase 1 — L2 bridge + VLAN** (`is_l2_accel`): VLAN push/pop/mangle.
- **Phase 2 — L3 route + NAT/NAPT** (`is_routed`): the line-rate-NAT path —
  IPv4 TTL decrement, source/dest IP rewrite (SNAT/DNAT), L4 source/dest port
  rewrite (NAPT), and IP + TCP/UDP incremental checksum fixups, emitted as the
  RE'd `addIpv4Commands` cmdlist sequence (dec-TTL → IP-replace → IP-icsum →
  L4-replace → L4-icsum).

In both cases a flow-miss traps to the CPU, the driver learns it via the
flowtable and programs one NAT-C entry, and every subsequent packet hits NAT-C
and is forwarded by the Runner (Phase 2: with the packet rewritten) without the
CPU. See `re-notes/offload-phase1-status.md` and `offload-phase2-status.md`.
IPv6 NAT is still rejected (as in mtk).

### Offload files
- `cmdlist.{c,h}` — open XPE (XRDP Packet Engine) cmdlist builder: emits 16-bit
  big-endian command words (opcode = word>>26). Phase-1 L2 ops (VLAN
  push/pop/mangle + NOP) and Phase-2 L3/NAT ops (`decrement_8`, `replace_32`,
  `replace_16`, `apply_icsum_16`). Clean-room from the RE'd opcode encoding.
- `flow_offload.{c,h}` — the `FC_UCAST_FLOW_CONTEXT_ENTRY` builder (L2 + routed),
  the 16-byte masked BE NAT-C key builder (L2 tuple + L3 5-tuple), the L2 and
  NAT cmdlist builders, the `FLOW_ACTION_MANGLE`/5-tuple parse, and the
  nf_flow_table / `TC_SETUP_FT` offload block (flow_block_cb +
  FLOW_CLS_REPLACE/DESTROY/STATS), modelled on mainline `mtk_ppe_offload.c`.
- NAT-C connection-table I/O (`xrdp_natc_add/del/stats`) lives in
  `bcm4916_runner.c` (it owns the MMIO window); `.ndo_setup_tc` +
  `NETIF_F_HW_TC` register the offload block on the conduit.

Two debugfs triggers (the QEMU offload proofs):
- `…/bcm4916-runner/offload_selftest` (`echo "<vlan_op> <vid>"`) — one L2 flow.
  vlan_op: 0=plain L2, 1=push, 2=pop, 3=mangle.
- `…/bcm4916-runner/offload_nat_selftest` (`echo go`) — one routed IPv4
  SNAT+NAPT flow (LAN→WAN masquerade, TEST-NET addresses), through the real
  Phase-2 builders + NAT-C add path.

## What it does

- **FPM buffer pool** init + token alloc/free (`fpm_alloc_token` reads, and
  `fpm_free_token` writes, the pool0 alloc/dealloc register; buffer =
  `pool_base + index*chunk`). Ported from the GPL `fpm_core.c`/`fpm_priv.h`.
- **CPU RX**: a self-refilling data ring (the GPL `enet_ring.c` impl7 model).
  Each descriptor owns a DDR buffer; the Runner DMAs a frame in, sets the
  ownership bit (bit31 of `word2`, big-endian) and `packet_length`; a **NAPI**
  poll builds an skb, hands it up, and re-arms the slot in place. RX has **no
  MMIO doorbell** — the queue IRQ only wakes NAPI; ownership is pure DDR poll.
- **CPU TX**: `ndo_start_xmit` copies the skb into an FPM buffer, builds a 16-B
  big-endian `RING_CPU_TX_DESCRIPTOR` (FPM-token mode, `abs=0`), stages it into
  the TX ring, advances `write_idx` and **rings the SRAM index "doorbell"** in
  each serving Runner core (`CPU_TX_RING_INDICES_VALUES_TABLE`). There is **no
  per-packet MMIO doorbell** on TX either — the index write *is* the doorbell.

## Source map (what is ported vs new)

| Piece | Origin |
|---|---|
| FPM token bitfields, register layout, alloc(read)/free(write), token→buf math | **ported GPL** — `fpm_priv.h` / `fpm_core.c` (asuswrt-merlin.ng 4916 SDK) |
| FPM pool bring-up (init mem, chunk size, enable, base addr) | **ported GPL** — `fpm_core.c` init_hw |
| CPU_RX_DESCRIPTOR word0..3 layout + ownership poll + `swap4`/`rest_desc` | **ported GPL** — `rdp_cpu_ring_defs.h`/`_inline.h` (416L05) + `enet_ring.c` (4916) |
| XRDP block bases/offsets (FPM, RNR_MEM, PSRAM, BBH, …) | **new, RE-derived** — `re-notes/xrdp-datapath-abi.md` sec 1.1 (from `rdpa.ko`) |
| RING_CPU_TX_DESCRIPTOR bitfield packing | **new, RE-derived** — ABI sec 2.2 (from `rdpa.ko`) |
| CPU_RING_DESCRIPTOR (ring publish) packing | **new, RE-derived** — ABI sec 2.1 |
| TX SRAM index doorbell sequence | **new, RE-derived** — ABI sec 5bis-G2 (`rdpa_cpu_send_pbuf`) |
| Runner microcode loader | **new, RE-derived stub** — ABI sec 4 step 7 / 5bis-G3 step 11 |

## Files

- `bcm4916_runner.h` — register map, descriptor/token bitfields, offload glue (cited).
- `bcm4916_runner.c` — platform driver, FPM, RX NAPI, TX, microcode hook,
  NAT-C I/O, `.ndo_setup_tc`, debugfs self-test.
- `cmdlist.{c,h}` — open XPE cmdlist builder (Phase-1 L2/VLAN ops).
- `flow_offload.{c,h}` — context/key builders + nf_flow_table offload block.
- `Kbuild`, `Makefile` — out-of-tree module build. The module is the composite
  `bcm4916-runner.ko` (= `bcm4916_runner.o + cmdlist.o + flow_offload.o`).

## Build (on the dedicated build host only)

```sh
make -C ~/mainline M=$PWD ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
```

or with the provided wrapper Makefile:

```sh
make KDIR=~/mainline ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
```

The C compiles **clean** against mainline v7.1. (If the kernel tree has no
`Module.symvers` yet, modpost will warn about undefined exported symbols such
as `napi_enable` — those resolve at module load; build the kernel's
`Module.symvers` to silence them.)

## Device tree node

Compatible: **`brcm,bcm4916-runner`**. Defined in `dts/bcm4916.dtsi` as
`runner: runner@82000000`, enabled in `dts/gt-be98.dts`:

```dts
runner: runner@82000000 {
    compatible = "brcm,bcm4916-runner";
    reg = <0x0 0x82000000 0x0 0x00caf004>;   /* whole rdpa window */
    interrupt-names = "queue0", "fpm";
    interrupts = <GIC_SPI 75  IRQ_TYPE_LEVEL_HIGH>,   /* RX queue0 */
                 <GIC_SPI 107 IRQ_TYPE_LEVEL_HIGH>;   /* FPM refill */
    brcm,runner-emulated;   /* QEMU: skip proprietary microcode */
    status = "okay";
};
```

switch0's CPU/IMP port references it: `ethernet = <&runner>;`. The driver
derives all XRDP block bases (FPM, RNR_MEM, PSRAM, BBH, …) by offset from the
single `reg` window. If `interrupts` are absent the driver falls back to a
poll-mode NAPI schedule (fine for first light).

## Microcode: QEMU vs real hardware

On **real hardware** the 8 Runner cores must be loaded with the proprietary
Runner microcode (`request_firmware("brcm/bcm4916-runner-microcode.bin")`)
before any packet moves — that blob is **not redistributable** (it lives only
inside the stock `rdpa.ko`). The actual block-copy into RNR_INST/RNR_PRED + the
per-core enable handshake is a **structured TODO** (the blob sub-layout is not
pinned; ABI sec 6).

For **emulation** the QEMU Runner model fakes Runner behaviour, so **no
microcode is loaded**. Select that path with either:

- the DT property `brcm,runner-emulated`, or
- the module param `runner_emulated=1`.

In emulated mode `request_firmware` is skipped entirely and the driver runs the
full ring datapath against the emulator. Absence of the blob is also treated as
**non-fatal** in HW mode (a warning), so the driver always binds; it just won't
move packets on real silicon until a licensed image is present.

## Honest status / TODO

- **Offload Phase 1 (L2 + VLAN): implemented and proven in QEMU** — first packet
  misses NAT-C → CPU; driver programs the entry; subsequent packets hit NAT-C →
  HW-forwarded, CPU bypassed. cmdlist/context/key bytes validated against the
  RE'd opcode encoding. See `re-notes/offload-phase1-status.md`. Gaps: the full
  nf_flow_table FLOW_CLS_REPLACE trigger is proven via a debugfs self-test (same
  code path) rather than live conntrack (needs `NF_FLOW_TABLE` + a 2-port
  topology); context byte offsets / NAT-C key layout / indirect-register offsets
  are contract placeholders, not pinned 6813 silicon offsets; CNPL stats stubbed;
  Phase 2 (L3/NAT) not implemented.
- FPM, RX ring + NAPI poll, TX descriptor build + SRAM doorbell: implemented.
- **PSRAM RDD table offsets** (`CPU_RING_DESCRIPTORS_TABLE`,
  `CPU_TX_RING_DESCRIPTOR_TABLE`, `CPU_TX_RING_INDICES_VALUES_TABLE`) are
  placeholder constants — the exact RDD offsets are not yet pinned from the GPL
  RDD map. The QEMU contract uses these same constants, so driver↔emulator
  agree; real HW needs the pinned offsets.
- **UBUS decode windows / per-port BBH/DSPTCHR/QM/DMA/SBPM register values**
  are stubbed (ABI sec 6 "STILL UNKNOWN"). Not required by the emulator.
- **Feed/recycle rings** (the full rdpa.ko CPU path) are documented but not
  built; the impl7 self-refilling data ring is used for first light.
- TX `egress port` is left 0 (DSA tag selects egress); per-port TX needs the
  DSA tagger wired.
