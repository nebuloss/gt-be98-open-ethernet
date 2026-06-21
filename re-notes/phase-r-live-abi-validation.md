# Phase-R live-silicon ABI validation (WiFi-style 4.19 graft + stock bdmf shell)

First on-silicon validation pass of the open BCM4916 datapath ABI, 2026-06-21. Method: since a
mainline kernel cannot boot on this unit (RSA secure-boot enforced in U-Boot + no kexec arch glue +
stock 4.19 — see `docs/`), we validate the **hardware ABI** WiFi-style: a vermagic-exact 4.19 graft
probe (`tools/stock-watch/xrdp_peek.c`) plus, decisively, the **stock driver's own bdmf shell `bs`**
as a *safe* oracle (the stock driver mediates all HW access).

## Safety model (proven)
- ★**Raw MMIO read of XRDP/Runner register space HANGS the SoC.** `xrdp_peek phys=0x82a00000
  allow_mmio=1` (FPM block, FDT-confirmed base) caused an unrecoverable external bus abort → device
  unreachable. **Never raw `ioremap`+`readl` XRDP/Runner/RDD space from a graft module** — that space
  must be reached through the stock driver's bus bridge/accessors. `xrdp_peek` keeps `allow_mmio` OFF
  for XRDP space.
- ★**HW-watchdog auto-revert recovered the wedge in ~74 s** (no serial, no WiFi, no human power-cycle):
  unpetted `wdtctl -t 240` (trial mode) fired → U-Boot booted committed stock **slot2** → Ethernet/SSH
  back. Nothing flashed → zero corruption, `/data` intact. Validates the serial-free recovery model for
  the worst case (host-wide wedge). NOTE: in trial mode the watchdog is left unpetted, so each boot
  auto-reboots at ~240 s unless you `touch /tmp/deadman-disarm && wdtctl stop` right after connecting.

## kallsyms constraint
CONFIG_KALLSYMS=y but **CONFIG_KALLSYMS_ALL=OFF** (1 data symbol total). Only FUNCTIONS (15644 T +
21701 t) and EXPORTED symbols are resolvable via kallsyms_lookup_name — stock data globals (incl.
`host_ring`) are NOT. So the read path is: call resolvable accessor functions, or use the `bs` shell.

## The bdmf `bs` shell oracle (`/bin/bs` = `bdmf_shell -c $(cat /var/bdmf_sh_id) -cmd $*`)
Tree: `/` → `Bdmf/ Driver/ Misc/`. `/Driver/` → `ru/ Bbh_rx/ bbH_tx/ Dma/ Sbpm/ Cnpl/ Psram/ qm/
dqM/ rNr_regs/ Tcam/ dsptchr/ nAtc/ sYstem/ hash/ drV_shell/ rdd/ cpUr/`. Key read-only dumps:
- `bs "/Driver/cpUr/Sar"` — all CPU ring descriptors (verbose: dumps EVERY entry, ~131k lines; grep it).
- `bs "/Driver/cpUr/Vrpd <ring_id 0..32> <desc_idx>"` — decode ONE ring packet descriptor (raw words
  unswapped+swapped + field interpretation). The authoritative descriptor oracle.
- `bs "/Driver/nAtc/cFg_get"`, `Dbg_get`, `natc_Key/Get`, `nAtc_tbl/Get` — NAT-C config/keys/table/stats.
- `bs "/Driver/Psram/{Get,Address,Set}"` — PSRAM access THROUGH the driver (safe; counter/MEMORY_DATA
  oriented). Use this instead of raw MMIO for PSRAM/RDD reads.

## CONFIRMED against live silicon
- **CPU RX data rings = 16 B/entry** (our `runner_rx_desc` size ✓). Rings 0,1,7 = 1024 entries; rings
  5,6 = 8192. **Feed ring id 24 = 8 B/entry, 4096 entries; Recycle ring id 25 = 8 B, 16384** (matches
  the RE host_ring special-ring-24 finding; feed/recycle carry 8 B FPM/buffer tokens).
- Ring base addrs are 0xffffffc05xxxxxxx (DDR virt uncached); per-ring write/read index shadow pairs.
- **NAT-C table[0] LIVE:** key_tbl phys = `0x0fa00000`, res_tbl phys = `0x0fa20010`; "variable context
  length: 1"; debug stats moving (hit 218 / miss 64 / ddr_request 64) → NAT-C active for the local flows.
  Cross-confirms the [[stock-driver-watch-lever]] natc_dump pinning. XRDP NATC block @ 0x82950000.
- Full XRDP block map + FPM token/register layout: confirmed by static RE of rdpa.ko (separate note).

## ★KEY DISCREPANCY FOUND — RX descriptor layout is WRONG in our header
`driver/runner/bcm4916_runner.h` `runner_rx_desc` was derived from the **416L05 SDK** (a DIFFERENT
XRDP generation, 63138/63148). The live 6813 absolute-address RX descriptor (host/swapped view) is:
- **word0 = packet DDR phys-low** (NOT packet_length as our header says)
- **word3 bit31 = ownership/host** (LAN samples: 0x80000000) (NOT word2 as our header says)
- word2 = a source/type constant (LAN `0x82800000`, WLAN `0x07400000`)
- word1 = packet length + reason; WLAN packs ssid/vport/is_ucast/tx_prio/color/metadata
Our QEMU model encoded the same (wrong) layout, so QEMU "passing" did NOT catch this — exactly why
on-silicon validation matters. Live samples (unswapped raw DDR / swapped host view):
```
LAN RX (port5) len66 : 4041095d 0a01a500 00008082 00000080  /  5d094140 00a5010a 82800000 80000000
LAN RX        len102: 4001095d 9a01a500 00008082 00000080  /  5d090140 00a5019a 82800000 80000000
LAN RX        len774: 40c1085d 1a0ca500 00008082 00000080  /  5d08c140 00a50c1a 82800000 80000000
WLAN RX vport14 ucast: 4041085d f2010100 00004007 02040020  /  5d084140 000101f2 07400000 20000402
```
Exact corrected word/bit layout (RX 16 B abs-mode, feed/recycle 8 B, and a TX-descriptor recheck) is
being derived from the 6813 rdpa.ko `Vrpd` decoder / dump_RDD function — apply to the header AND the
QEMU model, then re-validate in QEMU.

## Next
1. Apply corrected RX/TX/feed descriptor layout (RE in flight) to header + QEMU model; re-test slow path.
2. Per-core TX doorbell: pin `CPU_TX_RING_INDICES` (RE: per-core ~0x2368–0x29c8, not 0x0) from live.
3. Capture a live NAT-C entry (key+context+cmdlist) via `bs nAtc` for a real NAT flow (needs traffic
   through the device) to validate the offload cmdlist bytes (the open gap in [[stock-driver-watch-lever]]).
