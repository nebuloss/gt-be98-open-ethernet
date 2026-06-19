# LIVE READ-ONLY device dump — GT-BE98 / BCM4916 stock stack (ground-truth for the offload + datapath ABIs)

Captured read-only over the management SSH link (Ethernet — link stayed up the whole time;
`echo OK`/watchdog confirmed alive between every read group, and a final `LINK_STILL_UP` after the
last command). Device: `Linux (none) 4.19.294 ... aarch64 ASUSWRT-Merlin` (stock Broadcom/ASUS
BCA stack). All commands were `cat`/`head`/`ls` of /proc, plus the read-only `bdmf_shell`
**examine** (`bs /Bdmf/e <obj>`) and `fc status` — no writes, no `*clr*` reads, no `cmd` writes,
no devmem.

PUBLIC-SAFE: device LAN IPs redacted as `<ip>` / `<peer-ip>`; MACs reduced to OUI where Broadcom
(`60:cf:84:xx:xx:xx`) or fully masked (`xx:xx:xx:xx:xx:xx`); kernel pointers kept (not sensitive,
not stable). Protocol/port/structure preserved.

The **3-4 active accelerated flows are the management/SSH flows themselves** (the only traffic on
this otherwise-idle lab device): TCP `<peer-ip>:<port> -> <ip>:2222`, locally terminating
(`br0_rx -> blogtcp_local_tx`). So we captured **locally-terminated L3 ucast (non-routed, non-NAT)
flows** — still extremely informative for the context-entry + cmdlist layout, but NOT a routed/NAT
or VLAN/tunnel variant (none were active to capture).

---

## 1. fcache flow tables (`/proc/fcache/*`)

### nflist — EMPTY (no IP-forwarded/NAT flows active; the SSH flows are bridge-local -> brlist)

Header (column definitions, authoritative):
```
Flow K/U U/M M/C idle:+swhit TotalHits TotalBytes SW_TotHits: SW_TotalBytes HW_tpl Fhw_idx
  HW_Hits HW_TotHits HW_TotalBytes L1-Info Prot SourceIpAddress:Port DestinIpAddress:Port
  Vlan0 Vlan1 tag# ToS IqPrio ClientId SkbMark TCP_PURE_ACK LLC MAC_SA MAC_DA RxDev TxDev
  nf_conn[0..3]
```

### brlist — 4 flows (the live SSH/mgmt flows; bridge/local-terminated)
```
   901  K U -   0:1     9    3942   2:1700  0x00100000 0000000000  7 7 2242  EPHY 5  6
        <peer-ip:sport> <ip:02222>  0x0 0x0 0 b8 0 ... TCP_PURE_ACK=0
        <bc:24:11:xx:xx:xx> <60:cf:84:xx:xx:xx>  RxDev=eth0  TxDev=blogtcp_local
   902  K U -   0:0     1      66   1:66    0x00100001 0000000001  0 0 0  EPHY 5 6 ... ToS=b8 ACK=1
   903  K U -   0:0     1     102   1:102   0x00100002 0000000002  0 0 0  EPHY 5 6 ... ToS=20 ACK=0
   904  K U -   0:0     1      66   1:66    0x00100003 0000000003  0 0 0  EPHY 5 6 ... ToS=20 ACK=1
```
Key observations:
- **`HW_tpl` = `0x00100000` + idx**, **`Fhw_idx` = 0,1,2,3** (decimal, zero-padded).
- 4 brlist rows but only flows 901-903 have `HW_Hits>0`/are in pktrunner (see §2): the 4 rows are
  the same 5-tuple split by **TCP_PURE_ACK (0/1)** and **ToS (0xb8 vs 0x20)** — i.e. the stack
  makes **separate HW flows per ToS and per pure-ack** (matches `fc_tcp_ack_mflows=1`,
  `fc_tos_mflows=1` in §3).
- `RxDev=eth0` => **eth0 is the ingress port carrying the WAN/uplink+mgmt** (consistent with M1).
- `TxDev=blogtcp_local` => locally terminated (no egress port rewrite -> minimal cmdlist).

### l2list — header only (0 L2-accel flows active)
```
Flow K/U M/C idle:+swhit ... HW_tpl Fhw_idx ... MAC_SA MAC_DA EthType Vlan0 Vlan1 tag# ToS
  IqPrio ClientId SkbMark ACK_PRIO LLC RxDev TxDev
```

---

## 2. pktrunner (Runner-accelerated) — `/proc/pktrunner/accel0/*`

### stats
```
PKT_RUNNER[0]: status=1  activates=807 deactivates=804 failures=0
  l3_errors=0 l2_errors=0 mc_errors=0 hash_collision=0 no_free_idx=0 flushes=0
  active=3  ucast_active=3  mcast_active=0  max_flows=17408  Flow_idx_in_use=3
HOST_IP_ADDR_TABLE: ipv4_add_fail=0 ipv6_add_fail=0 ipv4_del_fail=0 ipv6_del_fail=0
CMDLIST: Success=808 Errors=0 Unsupported=0 Overflow=0 InvalidTarget=0
```

### flows/L3 — the FHW-Key -> RDPA-Key MAPPING (ground truth)
```
[Index : FHW-Key   ] -> [RDPA-Key  ]
[    0 : 0x00100000] -> [0x00000000]
[    1 : 0x00100001] -> [0x00000001]
[    2 : 0x00100002] -> [0x00000002]
Total Flows with valid RDPA Key = 3 ; matching = 3 ; displayed = 3
```
flows/L2 and flows/mcast: 0 matching displayed (the "valid RDPA key = 4" line is the pool size,
not active flows).

`var/max_disp` = `Max Display = 128`.

**=> CONFIRMED: FHW-Key `0x00100000 + idx` maps to RDPA-Key `0x00000000 + idx` (1:1, idx-aligned).**
The `HW_tpl` in fcache == the pktrunner FHW-Key. Flow 904 (Fhw_idx 3) is brlist-only / not yet a
valid RDPA HW entry (HW_Hits=0) — SW-tracked, HW-deferred (`Pkt-HW Activate Deferral rate=1`, §3).

---

## 3. bdmf_shell `examine` of the `ucast` object — THE CONTEXT ENTRY + RAW CMDLIST (the prize)

Read-only path discovered via `/usr/sbin/runner_dump.sh` (`rdpa_flow_dump()` does
`bs /Bdmf/e ucast` — examine/print, **clear is NOT wired** in that script). `bs` =
`bdmf_shell -c <id> -cmd $*`. `/Bdmf/e` = examine (read-only). Confirmed by running it: it printed
state and changed nothing.

```
Object: ucast  (Owned by: system)   nflows : 3
flow[0]: hw_id=0x48c9
  key={ src_ip=<peer-ip> dst_ip=<ip> prot=6 src_port=<sport> dst_port=2222
        dir=us tcp_pure_ack=0 vtag_num=0 tos=0xb8 ctx_ext=0 client_idx=0
        port_ingress_obj={port/name=eth0} }
  result={ port_egress_obj={port/name=gdx0}
        queue_id=0 service_queue_id=0 is_service_queue=0 wan_flow=0 wan_flow_mode=0
        is_routed=0 is_l2_accel=0 is_tcpspdtest=0 spdtest_stream_id=0 tcpspdtest_is_upload=0
        is_hit_trap=0 is_wred_high_prio=0 is_ingqos_high_prio=0 is_mapt_us=0 is_tunnel=0
        policer=null drop=0 ip_addresses_table_index=4 rx_max_pkt_len=10258 tos=0x0
        wl_metadata={0x00000000} tc=0 pathstat_idx=1 spdsvc=0 cntxt_ext=0
        cmd_list_data_length=28 tx_adjust=-10 cmd_list_length=40
        cmd_list=6014eb983f00601400[4]bd03002000141804170100001894ffff08800021b080c188b08000[162]
        is_group_master=0 tunnel_type=0 tunnel_inner_packet_offset=0 tunnel_key_offset=0
        tunnel_key=00[4] tunnel_index_ref=16 esp_o_udp=0 is_spu=0 crypto_session_id=0
        is_gdx_tx=yes gdx_prio=no gdx_ctx_data=1026 is_flood=no is_hw_cso=yes }
flow[1]: hw_id=0x2485  key={... tcp_pure_ack=1 tos=0xb8 ...}
  result={... cmd_list=6014eb983f00601400[4]be03002000141804170100001894ffff08800021b080c188b08000[162] ...}
flow[2]: hw_id=0xc07d  key={... tcp_pure_ack=0 tos=0x20 ...}
  result={... cmd_list=6014eb983f00601400[4]bf03002000141804170100001894ffff08800021b080c188b08000[162] ...}
flow_stat[0]={packets=7,bytes=2270}  flow_stat[1]={0,0}  flow_stat[2]={0,0}
fc_global_cfg={fc_accel_mode=layer23, fc_tcp_ack_mflows=1, fc_tos_mflows=1, fc_pbit_key_mode=0}
```
Notes on the `cmd_list` notation: `XX[4]` = byte `XX` repeated 4 times; trailing `[162]` =
remaining 162 bytes (the context-entry pad after the 28-byte cmdlist body — `cmd_list_data_length=28`).
So the real cmdlist body for flow[0] is:
`60 14 eb 98 3f 00 60 14 00 00 00 00 bd 03 00 20 00 14 18 04 17 01 00 00 18 94 ff ff 08 80 00 21 b0 80 c1 88 b0 80 00` ... (28 data bytes per the field; `cmd_list_length=40` is in the XPE word/byte
unit used by the entry — see "corrects" below).

The **only byte that differs between the 3 flows** is at offset 12: `bd`/`be`/`bf` — i.e. a
per-flow immediate (the `gdx_ctx_data`/SOP-relative selector or per-flow tag), everything else
identical because the 3 flows share egress (`gdx0`, local) and differ only in key (ack/tos).

### bdmf object types present (`bs /Bdmf/types`)
`system -> {filter, rate_limit, vlan_action, policer, mcast_whitelist, dhd_helper, cpu,
egress_tm, port, ingress_class, l2_ucast, ucast, mcast, tcont, ct_class, gem}`.
`l2_ucast` examine: `nflows=0` (no L2-accel flow to capture right now).

---

## 4. RDPA system config — `bs /Bdmf/e system` (data_path_init ground truth)

```
init_cfg = { enabled_emac = emac1+emac5+emac6+emac7,   <-- the 4 wired ports
             switching_mode=none, operation_mode=fc, runner_ext_sw={enabled=no},
             us_ddr_queue_enable=no, iptv_table_size=256, dhd_offload_bitmask=0x7 }
cfg = { car_mode=yes, def_max_pkt_size=10272, qos_mapping_mode=egress_pbit,
        rx_cpu_redirect=disable, dynamic_buffer_management_mode=no, ... }
sw_version = { rdpa 4.4.3 sw_rev=512995,
        firmware_revision = i0:340982 i1:340982 i2:340982 i3:341568 i4:340982
                            i5:340982 i6:325816 i7:325816 (latest:341568) }   <-- 8 Runner cores
stat.common (all 0 except): rx_error_drop=103, cpu_tx_egress_packets=9343
dbg_stat: parser_exception_path=12803, vport_exception_path=0
cpu_reason_to_tc[oam/omci/flow/mcast/bcast]=0 ...
tpid_detect[0x8100], [0x88A8] = both otag+itag+triple enabled
```
**=> CONFIRMS:** 8 Runner cores each with its own firmware image (i0..i7) — matches the
`fw_binary_0..7`/`fw_predict_0..7` geometry in the datapath ABI §1.1/§5bis. `operation_mode=fc`
(flow-cache/forwarding). `enabled_emac=emac1+5+6+7` = the 4 GbE/10G eth ports.

## 4b. `fc status` (fcctl, read-only) — flow-cache policy
```
Acceleration Mode: <L2 & L3>   HW Acceleration <Enabled>
TCP idle 60s / UDP 120s / L2 120s / Mcast 120s
Flow Refresh Timer=10000ms  Pkt-HW Activate Deferral rate=1  Pkt-HW Idle Deactivate=0
TCP Ack Prioritization <Enabled>   ToS Multi Flow <Enabled>   IPv6/L2TP/GRE Learning <Enabled>
Flow Ucast Learning: Max<16383> Active<4> Cumulative[972-968]
Flow Mcast Learning: Max<1152> Active<0>
```
Explains the 4-vs-3 split: TCP-ack + ToS multi-flow are on, so one 5-tuple becomes up to 4 HW flows.

---

## 5. fcache misc / stats (read-only; `*clr*` files SKIPPED)

### misc/host_dev_mac — port<->MAC table (all share the base MAC 60:cf:84:xx:xx:xx)
```
0 eth0   1 mac 60:cf:84:xx:xx:xx   ;  1 eth1 ;  2 eth2 ;  3 eth3 ;  4 br0
5 eth0.30  6 eth0.20  7 eth0.50  8 eth0.70  9 br20  10 br30  11 br50  12 br70
```
### misc/host_netdev — full netdev list (eth0..eth3, eth{0..3}.{20,30,50,70}, br0/20/30/50/70,
gretap0/gre0/erspan0/ip6gre0/ip6tnl0/sit0/blogtcp_local/bcmsw, imq/ifb/dummy...). Confirms the
VLAN-per-service topology (VIDs 20/30/50/70 on every eth port) and tunnel netdevs registered.

### misc/fdblist — bridge FDB
```
281 0x20000119 bc:24:11:xx:xx:xx  (the SSH peer)
282 0x2000011a 60:cf:84:xx:xx:xx  (device)
```
### misc/slice_info: num_slices=2000, slice_period=5ms, num_slice_ent=9 (flow-ageing slicer).
### misc/npelist, misc/unknown_ucast_info: empty (no NPE chains; 0 unknown-ucast groups).

### stats/fhw (global):
```
Flow Activation Success=819 / Failure=0 ; Deactivation=816 ; Refresh Success=2004 / Fail=0
Host MAC Add=5/0  Delete=4/0
```
### stats/path: one active path
```
<Path_65> key=0xffffff80bd961ff3 active_flows=4 sw_pathstat_idx=65 hw_pathstat_idx=1
  sw_pkt=5/1934   hw_counter_valid=1 hw_pkt=8/2308
  br0_rx [-14] -> blogtcp_local_tx [0] -> End
```
### stats/path_usage: Max#Path=128, Max#HWACC cntr=64, Active Path=1, Active vt dev=2,
auto-refresh slice_idx_step=3, path_num_per_slice=1.
### stats/flow_bmap: SW/HW bitmaps printed empty (sparse).

---

## 6. driver state — `/proc/driver/*`

`/proc/driver = {enet, hs_uart, phy, ubus, xport}`.
- `/proc/driver/enet` = only `cmd` (write-only control; **NOT touched**).
- `/proc/driver/phy`  = only `cmd` (**NOT touched**).
- `/proc/driver/xport` = only `cmd` (**NOT touched**).
- `/proc/driver/ubus` = read-only `decode_cfg` + `tokens`:

### ubus/decode_cfg — UBUS master-port decode windows (block base 0x83xxxxxx)
```
Master Port (addr)  | Cache:Enable:CD:Strict
BIU   (83020000) 24:1   PER  (83010000) 24:1   USB  (83018000) 4:0
PCIE0 (83030000) 24:1   PCIE1(83038000) 24:1   PCIE2(83040000) 24:1  PCIE3(83048000) 24:1
DMA0  (83058000) 24:1   DMA1 (83060000) 24:1   DMA2 (83070000) 24:1
RQ0   (83078000) 24:1   RQ1  (83080000) 24:1   QM   (83050000) 24:1
SPU   (83028000) 4:0    MPM  (83088000) 24:1
```
### ubus/tokens — NxN UBUS arbitration token matrix (BIU/PER/USB/PCIE0-3/DMA0-2/RQ0-1/QM/SPU/MPM).
(e.g. BIU->BIU=4, RQ1->BIU=14, QM->BIU=18.)

NB: these `0x83xxxxxx` master-port registration addresses are the **UBUS master/registration
block** — distinct from the `data_path_init` UBUS_SLV *device decode windows* (0x82A00000 FPM /
0x82C00000 QM / 0x82C80000 DQM / 0x82700000 VPB / 0x82900000 APB) in the datapath ABI §5bis G3.
They are complementary, not contradictory.

---

## WHAT THIS CONFIRMS / CORRECTS (mapped to the RE'd ABI docs)

### CONFIRMS (xrdp-offload-abi.md)
1. **FC_UCAST_FLOW_CONTEXT_ENTRY field set (§1.3) — confirmed live, name-for-name.** Every field
   the note listed appears in the `ucast result={...}` dump: `is_routed`, `is_l2_accel`,
   `is_hw_cso`, `vport` (shown as `port_egress_obj`), `service_queue_id`, `policer` (`policer_id`),
   `tunnel_index_ref`, `pathstat_idx`, `max_pkt_len` (`rx_max_pkt_len`), `tx_adjust`,
   `spdtest_stream_id`, `tcpspdtest_is_upload`, `is_ingqos_high_prio`, `is_wred_high_prio`,
   `cmd_list` (embedded inline). The 4916-specific additions the note flagged (`vport`,
   `service_queue_id`, `policer_id`, `tunnel_index_ref`) are ALL present and real.
2. **The context embeds the cmdlist inline** (`cmd_list=...` inside `result`) — confirms §1.1/§1.3
   "result IS the context entry, which embeds the cmdlist."
3. **NAT-C key composition is configurable / multi-flow by ToS & pure-ack** — `fc_tos_mflows=1`,
   `fc_tcp_ack_mflows=1`, `fc_pbit_key_mode=0` (§1.1 "key composition is configurable":
   `rdd_natc_pbit_key_mode_set` etc.). Live key shows `tos` and `tcp_pure_ack` ARE part of the key
   (one 5-tuple -> 4 entries).
4. **Real cmdlist bytes captured** to validate the XPE opcode encoding (§2). The cmdlist is a byte
   stream of 16-bit BE words: `0x6014 0xeb98 0x3f00 0x6014 0x0000 0x0000 0xbd03 0x0020 0x0014
   0x1804 0x1701 0x0000 0x1894 0xffff 0x0880 0x0021 0xb080 0xc188 0xb080 0x00..`. `0x18` = REPLACE
   (pinned opcode); `0x3f00` carries the pinned NOP=0x3f in the high bits. The minimal program
   (no TTL-dec/NAT) is consistent with `is_routed=0` local delivery. Exact per-16-bit-word opcode
   bit position still needs the emitter disasm — but we now have a real byte stream to validate.
5. **8 Runner cores, per-core firmware** (`i0..i7` revisions) — confirms datapath ABI §1.1
   8x inst + 8x pred microcode geometry, and that microcode is versioned per core.
6. **FHW-Key -> RDPA-Key 1:1 idx mapping** (pktrunner L3) — confirms the offload-ABI note's
   observed `idx0-3, FHW 0x00100000+, RDPA 0x0+` mapping exactly.
7. **CNPL-backed per-flow stats** — `flow_stat[i]={packets,bytes}` separate from the context, and
   `policer=null` per flow (policer_id optional) — matches §3.2 (CNPL counters + optional policer).
8. **`port_egress_obj`/`port_ingress_obj` = the `vport` abstraction** (§1.3 "vport replaces old
   egress_phy"): here ingress `eth0`, egress `gdx0` (the GDX/local-delivery vport;
   `is_gdx_tx=yes`).

### CORRECTS / REFINES
- **cmdlist length unit:** the note (§1.3) said `command_list_length_32` (32-bit-word unit). Live
  entry shows BOTH `cmd_list_data_length=28` (bytes of actual command data) AND `cmd_list_length=40`
  (larger) plus a 162-byte tail pad. So the entry carries a **byte data-length AND a separate
  (word-ish/aligned) length field** — refines the single-`command_list_length_32` picture.
- **`tx_adjust=-10`** is a live, real per-flow field (egress length delta) — concrete value now.
- **GDX path:** these flows egress to `gdx0` with `is_gdx_tx=yes`, `gdx_ctx_data=1026` — local
  delivery still gets a full HW context + cmdlist (good Phase-0/1 reference, NOT a port-to-port
  forward).

### CONFIRMS (xrdp-datapath-abi.md)
- 8 Runner cores w/ independent firmware (§1.1 / §5bis).
- `operation_mode=fc`, `enabled_emac=emac1+5+6+7` (the 4 wired MACs).
- UBUS decode infrastructure exists and is inspectable (ubus/decode_cfg) — complements the
  data_path_init UBUS_SLV device windows in §5bis G3 (0x83xxxxxx master ports vs 0x82xxxxxx device
  windows).
- `cpu_tx_egress_packets=9343`, `parser_exception_path=12803` — live CPU-TX + exception/trap
  counters, consistent with the slow-path trap model (§3.5/§4).

---

## STILL NOT OBTAINABLE READ-ONLY (this session)

1. **A routed / NAT / NAPT cmdlist** (with `is_routed=1`, dec-TTL, IP/port replace, icsum) — the
   only active flows are **local-terminated, non-routed** (`is_routed=0,is_l2_accel=0`). Capturing
   the §2.4a IPv4-NAT program live needs an actual forwarded/NAT'd flow (would require generating
   LAN<->WAN traffic = NOT allowed / could disturb mgmt).
2. **An L2-bridge-accel cmdlist** (`l2_ucast`) — `l2_ucast nflows=0`; no VLAN push/pop/mangle to
   capture.
3. **A tunnel (GRE/MAP-T) cmdlist** — tunnel netdevs exist but no active accelerated tunnel flow.
4. **Exact 6813 FC_UCAST_FLOW_CONTEXT_ENTRY *byte offsets*** — the bdmf examine prints fields by
   NAME (driver-decoded), NOT the raw DDR byte layout. Raw NAT-C bytes would need devmem of
   0x82950000/DDR (FORBIDDEN). Offsets still need RE of rdpa.ko `_ucast_prepare_rdd_*` (offload-ABI
   UNKNOWN #1 stands).
5. **NAT-C HW hash polynomial / bucket placement** — engine-internal; not surfaced (UNKNOWN #2).
6. **Per-opcode operand bit-packing below the opcode field** — we have real cmdlist BYTES now, but
   pinning the sub-opcode bit layout still needs the `xpe_api.*.o` emitter disasm (UNKNOWN #3
   partially advanced).
7. **Raw `/dev/mem` register/DDR reads** — out of scope by the read-only rule.

---

## SAFETY LEDGER
- Commands run: only `cat`/`head`/`ls` of /proc, `bs /Bdmf/e <obj>` (examine = read-only print),
  `bs /Bdmf/types`, `bs /Bdmf/help`, `fc status`, and `uname`/`echo`. Read the `runner_dump.sh` /
  `bs` / `runner` scripts but did NOT execute runner_dump.sh or `runner`/`fc enable|disable|flush`.
- NEVER touched: `/proc/driver/{enet,phy}/cmd`, `/proc/driver/xport/cmd`, any `*clr*`/`clr_on_read`
  /`fc_tx_clr` file, no devmem, no writes/echo/redirection, no module load/unload, no set/flush.
- Link watchdog passed before/after every read group; final `LINK_STILL_UP` confirmed. Management
  Ethernet was never disturbed.
