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

## Offload: live flow context + cmd_list capture (`bs /Bdmf/Examine ucast`)
The BEST offload oracle: `bs "/Bdmf/Examine ucast"` dumps EVERY accelerated flow's full key +
result context INCLUDING the cmd_list bytes — no graft module needed. Captured 4 live flows (SSH to
the device's own IP, so CPU/gdx-delivery flows, NOT NAPT-rewrite):

- **Key fields (confirm our NAT-C key):** `src_ip,dst_ip,prot=6,src_port,dst_port,dir=us,
  tcp_pure_ack,vtag_num=0,tos,ctx_ext,client_idx,port_ingress_obj={eth0}`. Multi-flow keying is LIVE:
  `fc_global_cfg={fc_accel_mode=layer23,fc_tcp_ack_mflows=1,fc_tos_mflows=1,fc_pbit_key_mode=0}` → the
  SAME 5-tuple yields SEPARATE NAT-C entries per `tcp_pure_ack`(0/1) and per `tos`(0xb8/0x20) — our
  builder/key must replicate this or keys won't match silicon.
- **Result/context fields (confirm FC_UCAST_FLOW_CONTEXT_ENTRY):** `port_egress_obj={gdx0},queue_id,
  is_routed=0,is_l2_accel=0,ip_addresses_table_index=4,rx_max_pkt_len=10258,tos,wl_metadata,
  cmd_list_data_length=28,tx_adjust=-10,cmd_list_length=40,tunnel_index_ref=16,is_gdx_tx=yes,
  gdx_ctx_data=1026,is_hw_cso=yes`.
- **Live cmd_list (redacted flow, ingress eth0 → gdx0, dir=us):** cmd_list_length=40, data_length=28:
  `6014 eb98 3f00 6014 00 00000000 3100 0020 0014 1804 8d00 000000 1894 ffff 0880 0021 b080 c188 b080 00 <zero pad>`
  - op[0] `0x6014eb98 >> 26 = 0x18` = **REPLACE** ✓ (matches our pinned opcode set). Length-delimited
    (cmd_list_data_length=28), the remaining context bytes are 0x00 padded (NOT 0xfc — the 0xfc framing
    is a within-cmdlist pad nuance; here the trailing CONTEXT region is zero).
  - This is a CPU/gdx-delivery cmd_list (is_routed=0/is_l2_accel=0), so it validates the cmd_list
    ENCODING/framing + the context layout, not a NAPT IP/port rewrite.

## Next
1. ✅ Corrected RX/TX/feed descriptor layout applied to header + QEMU model (commit 2298c77).
2. Per-core TX doorbell: pin `CPU_TX_RING_INDICES` (RE: per-core ~0x2368–0x29c8, not 0x0) — RE'd from
   rdpa.ko; live-pin needs a safe RNR_MEM read path (NOT raw MMIO).
3. NAPT-rewrite cmd_list: capture a FORWARDED flow (through the box to another host, ideally a routed/
   NAT topology) via `bs /Bdmf/Examine ucast` to validate the IP/port rewrite + csum ops of
   flow_offload.c — the remaining open gap. The encoding/framing + key/context are now silicon-confirmed.
