# BCM4916 XRDP/Runner HW flow-offload control plane — open-reconstruction feasibility

Scope: the **FAST path** — how a forwarded connection (L2 bridge / L3 route + NAT + VLAN +
QoS) gets pushed into the Runner so the quad-A53 stays idle at line rate. Companion to
`xrdp-cpu-datapath.md` (which covers the SLOW path / trap-to-CPU). Goal: 10G with no CPU
overload ⇒ the Runner must do fast-path forward/NAT/QoS in HW.

**Confidence tags:** `[GPL-SRC]` = read directly from Broadcom GPL SDK `broadcom-sdk-416L05`
(on dev-build `~/re-sdk/broadcom-sdk-416L05/`). `[BIN]` = confirmed against the 4916 stock
binaries on the RE container `/opt/re-bins/*` (symbols/strings; r2). `[FDT]` = live device tree.
`[INFER]` = deduced. Note the SDK is **RDP impl2 (BCM63138/63148)** = the Rosetta stone for
the architecture; the 4916 is **XRDP** = the binaries `[BIN]` are the ground truth for the
exact 4916 layer split.

---

## OPEN-OFFLOAD FEASIBILITY VERDICT

### **VERY-HARD — effectively NEEDS-BLOB-WE-CANT-SHIP for the full Broadcom-style offload.**

For *full feature parity with the stock fast path* (L2+L3 forward, NAT/NAPT, VLAN edit,
tunnels, per-flow QoS, all in HW), the verdict is **VERY-HARD bordering on
NEEDS-A-BLOB-WE-CANNOT-LEGALLY-SHIP**, because the three modules that actually *make the
offload decision and emit the HW program* are **proprietary** on the 4916 and have **no GPL
source** in any drop we have:

| Module (4916 stock) | License `[BIN]` | Role in the fast path |
|---|---|---|
| `pktflow.ko` (525 KB) | **Proprietary** | The flow cache (fcache): hooks Linux fwd path, builds `Blog_t`, decides "this flow is offloadable" |
| `pktrunner.ko` (136 KB) | **Proprietary** | `fhw` accelerator: `L2L3ParseBlogFlowParams`, `fhwPktRunnerActivate`, `pktrunner_ucast_cmdlist_create` — turns a Blog into a Runner flow |
| `cmdlist.ko` (222 KB) | **Proprietary** | Builds the per-flow **XPE cmdlist** byte-code (the header-edit/NAT/VLAN/csum program the Runner executes) |
| `rdpa.ko` (7.2 MB) | **Proprietary** (taint P) | bdmf `ucast`/`l2_ucast`/`ip_class` objects + **NAT-C** table mgmt + all XRDP `ag_drv_*` register accessors |

The make-or-break point: **the flow→cmdlist translation and the NAT-C/flow-table layout — the
literal heart of the offload — are NOT shipped as GPL.** `[BIN]` Unlike the Runner *microcode*
(which is GPLv2-redistributable, see CPU-datapath notes), the cmdlist builder and rdpa flow
core are ordinary proprietary kernel modules: we may **not** redistribute them, and they are
not request_firmware-able blobs.

### What IS GPL and reusable (the good news)

- The **RDD layer** (Runner Data Definition — the code that writes flow/context entries into
  Runner SRAM/DDR tables): `rdd_l2_ucast.c`, `rdd_router.c`, `rdd_lookup_engine.c`,
  `rdd_bridge.c`, `rdd_cpu.c` are **full GPL source** `[GPL-SRC]`. This is the layer *below*
  the proprietary rdpa core. It is the single most valuable open asset: it shows the exact
  hash-bucketed connection-table format, the context-entry/cmdlist binary layout, and the
  MWRITE programming.
- The **Linux→rdpa glue** (`rdpa_mw_blog_parse.c`, `rdpa_mw_*`, `rdpa_cmd_*`) is GPL
  `[GPL-SRC]`. `bdmf.ko`, `rdpa_mw.ko`, `rdpa_cmd.ko`, `bcm_enet.ko`, `wfd.ko` are GPL `[BIN]`.
- The **`ag_drv_*` register accessors** (FPM/BBH/DQM/NAT-C MMIO) are auto-generated from
  register XML; the *pattern* is GPL in the SDK (`rdp_drv_bbh.c` etc.) `[GPL-SRC]`, though the
  4916-XRDP-specific autogen currently only exists inside the proprietary `rdpa.ko` `[BIN]`.
- The **`blog` framework itself** (the kernel flow-tracking hook `pktflow` plugs into) is GPL
  in Broadcom kernels (`net/core/blog.c`); `pktflow`/`pktrunner` only *consume* its exported
  symbols (`blog_parse_*`, `blog_notify`, `blog_request`) `[BIN]`.

### Bottom line on ratings

- **CPU-forwarded "dumb pipe" (no accel):** YES-but-needs-GPL-microcode-blob (prior note).
- **HW L2 bridge offload only:** **FEASIBLE-WITH-MAJOR-RE.** The L2 path is the simplest:
  the GPL RDD `rdd_l2_connection_entry_add` shows the whole table format; the cmdlist for a
  pure L2-forward flow is short (often just "forward + maybe VLAN edit"). This is the
  realistic first open milestone.
- **HW L3 + NAT/NAPT + tunnels + QoS (full stock parity):** **VERY-HARD / NEEDS-BLOB-WE-
  CANT-SHIP.** Requires reconstructing the proprietary cmdlist byte-code compiler and the
  NAT-C management from RE of `cmdlist.ko`/`rdpa.ko`, then re-deriving the XRDP table layout.
  Large, fragile, and legally must be a clean-room reimplementation (the blobs can't ship).

---

## END-TO-END HW OFFLOAD DATA FLOW

```
                         ┌──────────────── SLOW PATH (first packet of a flow) ────────────────┐
 packet in
   │
   ▼
 MAC (UNIMAC/XPORT/XLMAC)
   │  BBH DMAs frame into an FPM buffer (free-pool mgr)            [GPL ag_drv_fpm/bbh + blob FW]
   ▼
 Runner cores (microcode) classify ──► look up flow:
   │                                       │
   │  FLOW MISS (no entry)                 │  FLOW HIT (entry present)
   ▼                                       ▼
 trap to CPU (CPU_RX_DESCRIPTOR,       HW FAST PATH — CPU IDLE:
 reason→queue, host ring)               execute the flow's cmdlist (XPE byte-code):
   │   [GPL host ring/ABI]                 - decrement TTL, replace IpSa/IpDa (NAT),
   ▼                                         replace SPort/DPort (NAPT), recompute
 Linux stack forwards it normally          IP/TCP/UDP iCsum, insert/replace L2 hdr,
 (bridge / ip_forward + conntrack)         add/edit VLAN tags, GRE/VXLAN/MAP-T,
   │                                        strip Broadcom tag
   ▼                                       then forward to egress BBH/MAC + QoS queue
 blog framework captures the flow          → all in Runner HW, no A53 involvement
 (Blog_t: keys, xlate, vtags, qos)             [microcode + per-flow cmdlist + NAT-C]
   │   [GPL blog.c]
   ▼
 pktflow.ko (fcache): flow "established"? offloadable? → blog_notify
   │   [PROPRIETARY]
   ▼
 pktrunner.ko: L2L3ParseBlogFlowParams → fhwPktRunnerActivate
   │   builds rdpa flow key + result, calls cmdlist_*_create
   │   [PROPRIETARY]
   ▼
 cmdlist.ko: compile Blog actions → XPE cmdlist byte-code (≤80 B ucast)
   │   [PROPRIETARY]
   ▼
 rdpa.ko ucast/l2_ucast/ip_class bdmf object:
   │   build RDD flow key + context entry (cmdlist embedded),
   │   manage NAT-C (NAT cache) DDR table
   │   [PROPRIETARY core]   ──calls──►   RDD layer  [GPL]
   ▼                                       │
 RDD: rdd_*_connection_entry_add           ▼
   compute CRC32 hash of flow key,     write CONNECTION_ENTRY (key) + CONTEXT_ENTRY
   set-associative bucket insert,      (cmdlist + egress + NAT idx + qos) into
   write Runner SRAM/DDR tables        Runner Private SRAM / DDR hash tables
   [GPL rdd_l2_ucast.c / rdd_router.c]      └────────────────► subsequent packets = FLOW HIT
                                                                 (the fast path above)
```

The defining property: **flow-miss → learn → program; all subsequent packets of that 5-tuple
hit in HW and never touch the A53.** That's how 10G stays off the CPU. The CPU only ever sees
the *first* packet of each flow plus aged-out/control traffic.

---

## FLOW-PROGRAMMING ABI (as concrete as the sources allow)

### A. The rdpa flow API surface (what the proprietary core exposes) `[GPL-SRC headers][BIN]`

GPL headers in `rdpa_gpl/impl1/include/`: `rdpa_ucast.h`, `rdpa_l2_ucast.h`,
`rdpa_ip_class.h`, `rdpa_ip_class_basic.h`, `rdpa_mcast.h`, `rdpa_cmd_list.h`.

The flow *result* struct carries the cmdlist directly (`rdpa_ucast.h`):
```c
typedef struct {                 /* rdpa_ucast flow result */
    ...
    uint8_t  drop;               /* 1=drop, 0=forward */
    union { uint32_t wl_metadata; ... };
    uint8_t  cmd_list_length;    /* bytes */
    uint16_t cmd_list[RDPA_CMD_LIST_UCAST_LIST_SIZE_16]; /* 40 x u16 = 80 bytes */
} ...;
```
`rdpa_cmd_list.h` (GPL) only defines **sizes**, NOT opcodes:
`RDPA_CMD_LIST_UCAST_LIST_SIZE = 80`, `MCAST_L2 = 64`, `MCAST_L3 = 20`.

The object API is GPL *shims only* — function-pointer trampolines set by the proprietary
core (`rdpa_gpl_ip_class.c`): `rdpa_ip_class_drv()` returns `f_rdpa_ip_class_drv()`, where
`f_*` is filled in by `rdpa.ko`. **The real bdmf object impl (which builds the flow key,
calls cmdlist, manages NAT-C, calls RDD) is NOT in any GPL source.** `[GPL-SRC][BIN]`

rdpa.ko fast-path internals confirmed by symbol RE `[BIN]`:
`_ucast_prepare_rdd_ip_flow_key_params`, `_ucast_prepare_rdd_ip_flow_tunnel_result`,
`_l2_ucast_prepare_rdd_flow_params`, `_l2_ucast_prepare_rdpa_ip_flow_key`,
`_rdpa_mcast_common_flow_add_to_natc`, `_natc_tbl_ddr_size_enum`, `_natc_tbl_entry_size_enum`.
⇒ XRDP uses a **NAT-C (NAT Cache) DDR table** for the flow lookup, distinct from the RDP-impl2
SRAM hash table — a fundamental XRDP-vs-RDP difference.

### B. The cmdlist (XPE) opcode model — RE'd from `cmdlist.ko` `[BIN]`

`cmdlist.ko` (proprietary) public API:
`cmdlist_ucast_create`, `cmdlist_ucast_create_bin`, `cmdlist_l2_ucast_create`,
`cmdlist_l3_mcast_create`, `cmdlist_l2_mcast_create`, `cmdlist_ucast_fc_context_add_bin`,
`cmdlist_buffer_alloc/free`, `cmdlist_get_length`, `cmdlist_bind` (binds rdpa+rdd handles),
`cmdlist_ucast_llc_snap_header_insert`, `cmdlist_ucast_wlan_eth_header_create`.

The **XPE (XRDP Packet Engine) opcode set** (emitter symbols + log strings) `[BIN]`:
```
xpe_cmd_sop_push_replace   xpe_cmd_sop_pull_replace   xpe_cmd_replace_pointer
xpe_cmd_replace_16         xpe_cmd_replace_32         xpe_cmd_replace_bits_16
xpe_cmd_replace_add_input  xpe_cmd_copy_add_16        xpe_cmd_copy_bits_16
xpe_cmd_insert_16          xpe_cmd_apply_icsum_16     xpe_cmd_apply_icsum_nz_16
xpe_cmd_compute_csum_16    (+ __cmd_sop_replace_ddr/_sram, __cmd_move_packet, __gdma_*)
```
Higher-level builders that emit those: `addIpv4Commands`, `addTcpUdpIcsumCommands`,
`insertL2Header`, `updateVlanTags`, `_build_l2gre_header`, `_build_gre_v4/v6_del_header`,
`_translate_mapt_v4tov6_header`, `__buildBrcmTagType2`.

Concrete action repertoire (log strings) `[BIN]`: *Decrement TTL · Replace IpSa · Replace
IpDa · Replace SPort/DPort · Apply IP/TCP/UDP iCsum · Insert L2 Header (+LLC/SNAP) · VLAN
Header edit · Remove IPv4 Header · Remove Broadcom Tag · GRE/6in4/6in6 del-header ·
VXLAN tunnel · IPv4↔IPv6 ToS/Traffic-Class copy (MAP-T).* The cmdlist is a compact byte-code
"micro-program" (≤80 B for unicast) the Runner runs per-flow on egress = NAT + routing edits
+ encap in HW.

### C. The Runner flow/context tables — RDD binary layout (RDP impl2, GPL) `[GPL-SRC]`

The exact table format the open driver must produce (from `rdd_l2_ucast.c`,
`rdd_data_structures_auto.h`):
- **Connection (key) table:** set-associative hash. Key built into a 16-byte `entry_bytes`,
  hashed with `crcbitbybit(...,RDD_CRC_TYPE_32)`; `hash_index = crc & (TABLE/SET_SIZE-1)`,
  bucketed (`LILAC_RDD_CONNECTION_TABLE_SET_SIZE`), overflow chained via a
  `BUCKET_OVERFLOW_COUNTER` in the last entry of the previous bucket. For L2 the key =
  {protocol=0x7F, tos, CRC32(src_mac+vtags+ethtype), CRC32(dst_mac)} — the MAC CRCs are
  computed by sending a `LILAC_RDD_CPU_TX_MESSAGE_IPV6_CRC_GET` message to FAST_RUNNER_A
  (the Runner computes the CRC; host reads it back). Separate US/DS tables.
- **Context (result) table:** `RDD_FC_UCAST_FLOW_CONTEXT_ENTRY` (offsets from `..._auto.h`):
  ```
  +0  flow_hits (u32)         +4  flow_bytes (u32)
  +8  [b7 multicast_flag][b6 overflow][b5 is_routed][b4 is_l2_accel][b0..10 mtu]
  +10 rx_tos                  +12 [is_unicast_wfd_nic/any][priority:4]...
      connection_direction, connection_table_index, ip_addresses_table_index,
      command_list_length_64 (4b), egress_phy, traffic_class, rate_controller (QoS),
      command_list[80]  ◄── the cmdlist bytes live INSIDE the context entry
  ```
  `f_rdd_l2_context_entry_write()` writes every field via `RDD_..._WRITE` macros
  (`MWRITE`/`FIELD_MWRITE` into Runner Private SRAM). `rdd_l2_connection_entry_add()` /
  `rdd_add_connection()` (router) are the GPL entry points — **but their only callers are the
  proprietary rdpa core** (grep shows no GPL caller) `[GPL-SRC]`.
- **XRDP caveat:** the 4916 replaces this SRAM hash-table scheme with **NAT-C** (a HW
  NAT-cache lookup engine over DDR) + a separate context/cmdlist store. The *concepts* map
  1:1 (key hash + context-with-cmdlist) but the exact 4916 table addresses/bitfields must be
  re-derived from `rdpa.ko` `_natc_*` / `_ucast_prepare_rdd_*` and the XRDP register XML.
  `[BIN][INFER]`

---

## PROPOSED OPEN ARCHITECTURE FOR THE OFFLOAD CONTROL PLANE

Recommendation: **do NOT port Broadcom's blog/pktflow/pktrunner stack.** Drive the Runner
from **mainline's netfilter flowtable HW-offload framework** instead, and put the
Broadcom-specific knowledge only in the table/cmdlist driver underneath.

### Layer plan

1. **Flow learning — REUSE MAINLINE, do not port pktflow/fcache.**
   Mainline already has the equivalent of blog+fcache: the **netfilter flowtable** offload
   (`nf_flow_table`, `flow_offload_add`) and **TC-flower HW offload**, both of which call a
   driver's `.ndo_setup_tc` / `flow_block` / flowtable `.flow_offload` ops with a fully
   resolved 5-tuple, NAT mangle, VLAN/dec-ttl action list. This is *conceptually identical*
   to what `pktrunner`'s `L2L3ParseBlogFlowParams` does — it hands you exactly the
   {key, actions} the cmdlist needs. Mediatek (`mtk_ppe`), mlx5, and others already use this
   path for HW NAT offload. ⇒ The Linux-side flow learner is **free, GPL, upstream, and
   maintained** — this is the single biggest reduction in scope vs. the Broadcom stack.
   The open driver registers a flowtable offload block; the kernel feeds it established
   conntrack flows; the driver translates each into a Runner flow.

2. **MAC / switch / PHY plane — REUSE MAINLINE** (as in the CPU-datapath note):
   `mdio-bcm-unimac`, `b53`/`bcm_sf2` DSA (new 4916 variant), `phylink`. No blob. `[GPL-SRC]`

3. **Runner table programming (RDD-equivalent) — PORT FROM GPL SDK + RE for XRDP deltas.**
   Reimplement clean from `rdd_l2_ucast.c`/`rdd_router.c`/`rdd_lookup_engine.c` `[GPL-SRC]`:
   key-hash, connection-table insert/delete/age, context-entry write. Re-derive the XRDP
   NAT-C layout + addresses from `rdpa.ko` `_natc_*`/`_ucast_prepare_rdd_*` `[BIN]`. The
   `ag_drv_fpm/bbh/dqm/natc` register accessors are autogen-pattern (GPL) — regenerate from
   the 4916 register XML if obtainable, else RE from rdpa.ko.

4. **cmdlist (XPE) compiler — the one genuinely hard RE / clean-room piece.**
   Must reimplement `cmdlist_ucast_create` from scratch: given the flowtable action list
   (dec-ttl, SNAT/DNAT addr+port, csum, VLAN push/pop, L2 rewrite), emit XPE byte-code using
   the opcode set RE'd above. The opcode *names/semantics* are recoverable from `cmdlist.ko`
   `[BIN]`, but the exact opcode *encodings* (byte values, operand layout, the Runner's
   cmdlist interpreter contract) must be reversed from `cmdlist.ko` disassembly +
   cross-checked against what the GPL microcode expects. **This is clean-room: you may read
   the opcode behavior but must not copy the proprietary code.** Highest risk item.

5. **Runner microcode — SHIP Broadcom's GPL image as firmware** (unchanged from CPU-datapath
   note). The microcode is the cmdlist *interpreter*; the cmdlist encoding must match the
   shipped microcode version exactly.

### Phasing / effort

| Phase | Deliverable | Effort | Risk |
|---|---|---|---|
| 0 | CPU "dumb pipe" (prior note) | done-able | med (needs FW blob) |
| 1 | **HW L2-bridge offload** via flowtable→RDD-L2 (no NAT, no cmdlist edits, just forward+maybe VLAN) | ~weeks–months | med — L2 table fully GPL-documented |
| 2 | **HW L3 forward + NAT/NAPT** (requires XPE cmdlist compiler: dec-ttl, addr/port replace, icsum) | ~months–quarters | **high** — cmdlist RE + NAT-C layout |
| 3 | tunnels (GRE/VXLAN/MAP-T), per-flow QoS, IPv6, multicast | long tail | high |

A realistic open project reaches **Phase 1 (L2 HW offload)** with major-but-bounded RE.
**Phase 2+ (the "full features, NAT at 10G") is VERY-HARD** and is the part that justifies the
overall NEEDS-BLOB/VERY-HARD verdict: it hinges on clean-room-reversing the XPE cmdlist
compiler and the NAT-C table format, neither of which exists as GPL source for XRDP.

---

## BIGGEST UNKNOWNS / BLOCKERS — and what source would resolve them

1. **XPE cmdlist opcode ENCODING (not just names).** Names/actions are RE'd `[BIN]`; the
   binary encoding + the microcode's interpreter contract are not. **Blocker for Phase 2.**
   *Resolved by:* targeted disasm of `cmdlist.ko` `xpe_cmd_*` + correlating against the GPL
   microcode arrays in a 4916-matched GPL SDK.
2. **XRDP NAT-C table layout & addresses on 4916** (entry format, hash, DDR base, eviction).
   The 416L05 GPL only documents the *older* RDP SRAM hash table, not NAT-C. *Resolved by:*
   RE of `rdpa.ko` `_natc_*`/`_ucast_prepare_rdd_*`, or a **4916-matched GPL SDK drop**.
3. **4916 XRDP register map for FPM/BBH/DQM/NAT-C/Runner** — autogen accessors live inside
   proprietary `rdpa.ko` `[BIN]`; the GPL drop we have is RDP-impl2 register XML, wrong gen.
   *Resolved by:* the 4916 register XML (data-model) from a matched GPL SDK, or RE of the
   `ag_drv_*` functions.
4. **Exact microcode version ↔ cmdlist/table ABI coupling.** The shipped GPL microcode,
   cmdlist encoding, and table offsets must all be the same generation. Mismatched versions =
   silent corruption. *Resolved by:* a single self-consistent 4916 GPL SDK (microcode + rdd +
   register XML from one release).

**The one source that collapses most of this:** a **GPL source drop matched to the BCM4916
/ XRDP generation** (e.g. an ASUS GPL archive for the GT-BE98, or a Broadcom 4912/4916-era
"impl"). It would likely contain the GPL **RDD/`ag_drv` for XRDP** and the **GPL microcode**,
turning blockers #2–#4 from "RE the proprietary blob" into "read the source." It would **not**
contain the cmdlist compiler or rdpa flow core as source (those are proprietary in every drop
we've seen — `cmdlist.ko`/`rdpa.ko`/`pktflow.ko`/`pktrunner.ko` are all license:Proprietary
on the 4916 `[BIN]`), so blocker #1 (clean-room cmdlist compiler) remains regardless.

---

## SOURCE INDEX

GPL SDK (`~/re-sdk/broadcom-sdk-416L05/`, RDP impl2) `[GPL-SRC]`:
- `shared/broadcom/rdp/impl2/rdd/rdd_l2_ucast.c` — L2 flow add/del, hash, context+cmdlist write
- `shared/broadcom/rdp/impl2/rdd/rdd_router.c` — L3/NAT flow + layer4 filters
- `shared/broadcom/rdp/impl2/rdd/rdd_lookup_engine.c` — hash lookup engine
- `shared/broadcom/rdp/impl2/rdd/rdd_data_structures_auto.h` — `RDD_FC_UCAST_FLOW_CONTEXT_ENTRY` (cmdlist[80] @ offset, all bitfields)
- `bcmdrivers/opensource/char/rdpa_mw/impl1/rdpa_mw_blog_parse.c` — GPL Blog→rdpa_ic_result bridge
- `bcmdrivers/opensource/char/rdpa_gpl/impl1/rdpa_gpl_ip_class.c` — GPL shim (proves core is proprietary)
- `bcmdrivers/opensource/char/rdpa_gpl/impl1/include/rdpa_ucast.h`, `rdpa_cmd_list.h` — flow result + cmdlist sizes
- `bcmdrivers/broadcom/char/pktflow/impl1/` — **prebuilt `.o_save` only, no source** (proprietary even here)

4916 stock binaries (the RE container `/opt/re-bins/`) `[BIN]`:
- licenses: `cmdlist.ko`=Proprietary, `pktflow.ko`=Proprietary, `pktrunner.ko`=Proprietary,
  `rdpa.ko`=Proprietary, `bcm_mpm.ko`/`bcm_bpm.ko`/`sw_gso.ko`=Proprietary; `rdpa_gpl/mw/cmd`,
  `bdmf`, `bcm_enet`, `wfd`=GPL
- `cmdlist.ko` symbols: `cmdlist_ucast_create*`, `cmdlist_l2_ucast_create*`, `xpe_cmd_*` opcode emitters
- `pktrunner.ko` symbols: `L2L3ParseBlogFlowParams`, `fhwPktRunnerActivate`, `pktrunner_ucast_cmdlist_create`, `runnerL2Ucast_activate`; imports `blog_*`, `cmdlist_*`, `rdpa_ucast_drv/get`, `rdpa_l2_ucast_drv/get`
- `pktflow.ko` imports: `blog_bind`, `blog_notify`, `blog_request`, conntrack/skb syms (Linux fwd hook)
- `rdpa.ko` symbols: `_natc_tbl_*`, `_ucast_prepare_rdd_ip_flow_*`, `_l2_ucast_prepare_rdd_*`, `ag_drv_fpm_*`, `ag_drv_bbh_rx_*`, `ag_drv_dqm_fpm_*`

Mainline reuse targets: `net/netfilter/nf_flow_table_offload.c` (flowtable HW offload),
TC-flower `flow_block`, `drivers/net/ethernet/mediatek/mtk_ppe*` (reference HW-NAT-offload
driver using flowtable), `drivers/net/dsa/{b53,bcm_sf2}`, `drivers/net/mdio/mdio-bcm-unimac`.
