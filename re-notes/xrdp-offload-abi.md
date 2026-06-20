# BCM4916 / XRDP HW flow-offload FAST-PATH ABI — implementable spec

Scope: the **offload fast-path ABI** — the exact on-the-wire/in-DDR formats and the
flow-learn->program sequence needed to implement HW flow offload in an open mainline driver
(goal: 10G forward/NAT with the A53 idle). This extends `rdpa-offload-controlplane.md`
(module split + feasibility) and `xrdp-datapath-abi.md` (register map). It is the
"how to actually build it" layer.

**Confidence tags**
- `[XRDP-GPL]` = read from the **BCM6813 (=4916 XRDP) GPL drop**
  `asuswrt-merlin.ng/release/src-rt-5.04behnd.4916/` on dev-build. Matched generation.
- `[XRDP-BIN]` = disassembled/strings from the **6813-matched** stock objects:
  `cmdlist_ucast.armb53_6813.o_saved`, `cmdlist_l2_ucast.armb53_6813.o_saved`,
  `xpe_api.armb53_6813.o_saved` (aarch64, **NOT stripped** — the prize), and the device
  `rdpa.ko` (built for `.../rdp/projects/BCM6813/...`, confirmed by embedded path string).
- `[RDP-GPL]` = older RDP-impl2 (63138/63148) GPL `broadcom-sdk-416L05` — Rosetta stone for
  the *concepts*; exact bitfields differ on XRDP.
- `[INFER]` = deduced. `[UNKNOWN]` = not pinned.

Disasm addrs are the object's own vaddr (xpe_api.o/.text based at 0x08000000; rdpa.ko at
0x08000000). r2: `r2 -a arm -b 64 -e bin.relocs.apply=true`.

---

## 0. KEY ARCHITECTURAL CORRECTION vs the prior note

The prior note said XRDP "replaces the SRAM hash table with NAT-C". RE now shows the 4916 has
**both**, with distinct roles:

1. **NAT-C (NAT Cache) @ 0x82950000** — the **primary 5-tuple flow cache** in **DDR**. Each
   NAT-C entry = a 16-byte (masked) **key** + a **result** that **is literally the
   `FC_UCAST_FLOW_CONTEXT_ENTRY`** (which embeds the cmdlist). Main L3/NAT/L2-accel flow table. `[XRDP-BIN]`
2. **HASH/CAM engine @ 0x82920000** — a separate **CAM-backed hash + context-RAM** lookup block,
   used for **multicast/IPTV** and aux key lookups (`rdd_iptv_hash_key_entry_compose`,
   `_drv_hash_encode_cam_key`, `hash_context_ram_context_23_0`, `ds_hash_index_get`). NOT the
   main unicast path. `[XRDP-BIN]`
3. **CNPL (Counter aNd Policer Layer) @ 0x82948000** — token-bucket **policers** + **stat
   counters**. The context entry's `policer_id` indexes a CNPL policer; per-flow stats
   (`flow_hits`/`flow_bytes`/`q_bytes_cnt`) are CNPL counters. `[XRDP-BIN]`

Defining property unchanged: **flow-miss -> trap -> learn -> program one NAT-C entry; every
subsequent packet of that 5-tuple hits NAT-C in HW and runs the embedded cmdlist on egress,
never touching the A53.**

---

## 1. THE FLOW / CONNECTION TABLE (NAT-C, in DDR)

### 1.1 NAT-C entry = KEY + RESULT(=context) `[XRDP-BIN]`

Add path RE'd from `rdpa.ko sym.drv_natc_key_result_entry_var_size_ctx_add` (@0x080a8d70,
GLOBAL entry point; signature `(table_id, key_u32x4, result_ptr, result_len, ...)`):

```
drv_natc_key_result_entry_var_size_ctx_add(tbl, key[4], result, rlen):
  ag_drv_natc_key_mask_get(tbl, &mask)          ; per-table 16B key mask  (0x80a8df8)
  for i in 0..3:  key[i] &= ~rev32(mask[i])     ; mask + byte-swap (BE)   (0x80a8e04 loop)
  drv_natc_key_idx_get(tbl, key, &idx, result)  ; compute hash/slot       (0x80a8e3c)
  _xrdp__memcpy(stage, key, ...)                ; stage key               (0x80a8e5c)
  drv_natc_eng_key_result_write(tbl, idx, ...)  ; engine indirect write   (0x80a8e64)
  drv_natc_eng_command_write(tbl, 3)            ; issue "add"(cmd=3) op    (0x80a8e74)
  _xrdp__memcpy(stage2, result, rlen)           ; stage result/context    (0x80a8ed8)
  drv_natc_key_entry_add(tbl, ...)              ; bookkeeping             (0x80a8ee8)
```

Pinned:
- **Key = 16 bytes = 4x u32**, AND-masked with a **per-table key mask** (unused 5-tuple bytes
  zeroed), stored **big-endian** (rev32). `[XRDP-BIN]`
- Masks programmable per table: `rdd_ag_natc_nat_cache_key0_mask_*`,
  `rdd_ag_natc_natc_l2_vlan_key_mask_*`, `rdd_ag_natc_natc_l2_tos_mask_*`,
  `rdd_natc_pbit_key_mode_set` — **key composition is configurable**. `[XRDP-BIN]`
- **Result payload = the `FC_UCAST_FLOW_CONTEXT_ENTRY`** (variable length — "var_size_ctx").
- Engine programmed via **indirect** regs `ag_drv_natc_indir_addr_set/get`,
  `ag_drv_natc_indir_data_get`; `drv_natc_eng_command_write(...,3)` = add. `[XRDP-BIN]`

### 1.2 NAT-C DDR table geometry `[XRDP-BIN]`

From `xrdp_drv_natc_ddr_cfg_ag.c` accessors in rdpa.ko:
- DDR split into **buckets**, each with **bins** (`ddr_bins_per_bucket_0`,
  `ddr_bins_per_bucket_1` — two size classes; a bin holds one key+context; bucket =
  set-associative set). `[XRDP-BIN]`
- Per-table DDR size: `ag_drv_natc_ddr_cfg_natc_ddr_size_set(8 args)`,
  `ag_drv_natc_ddr_cfg_total_len_set(8 args)` -> **up to 8 NAT-C tables** (matches the
  NATC_TBL[0..7]/NATC_KEY[0..7] banks at 0x829502d0/0x829503b0 in the regmap note). Separate
  tables ~ separate flow classes/directions. `[XRDP-BIN]`
- Lookup hash: `ag_drv_natc_eng_hash_get` (NAT-C has its own HW hash over the masked key).
  Polynomial = `[UNKNOWN]` (HW-internal; host writes key+result, engine hashes — unlike old RDP
  where host computed CRC32).

### 1.3 The CONTEXT ENTRY = `FC_UCAST_FLOW_CONTEXT_ENTRY` (the result)

Heart of the offload: carries the per-flow **cmdlist** + egress metadata.

**XRDP/4916 field set** (complete, from rdpa.ko dump strings `FC_UCAST_FLOW_CONTEXT_ENTRY_*`) `[XRDP-BIN]`:

```
-- HW control header (NATC_CONTROL_ENTRY, first u32(s), HW-written) --
  done, natc_hit, cache_hit, has_iter, hash_val, hw_reserved0/1

-- software context (we build this) --
  valid                          flow validity
  multicast_flag
  is_routed                      L3 route (dec-TTL etc.) vs L2 bridge
  is_l2_accel                    pure L2 accelerate
  is_tos_mangle                  DSCP/ToS edit present
  is_hw_cso                      HW checksum offload
  command_list_length_32         <-- cmdlist length in 32-bit words (XRDP unit; old RDP used _64)
  connection_direction
  priority, wfd_prio, wfd_idx    QoS / WLAN fast-dispatch
  is_unicast_wfd_nic/any, dhd_flow_priority
  vport                          <-- egress virtual port (replaces old "egress_phy")
  service_queue_id               egress queue
  policer_id                     <-- CNPL policer binding
  coupled_classic_queue
  is_ingqos_high_prio, is_wred_high_prio
  max_pkt_len                    (~MTU / frag guard)
  tunnel_index_ref               GRE/VXLAN/MAP-T tunnel descriptor index
  pre_exception_actions_union / post_exception_actions_union
  cpu_reason                     trap reason if exception
  tx_adjust, pathstat_idx, spdtest_stream_id, tcpspdtest_is_upload
  q_bytes_cnt, flow_hits, flow_bytes  (stats; CNPL-backed)
  link_specific_union
  command_list[ ]                <-- the XPE byte-code, embedded inline (see section 2)
```

**Byte layout:** exact 6813 offsets `[UNKNOWN]` (autogen `rdd_data_structures_auto.h` for 6813
is stripped from the GPL drop). The **RDP-impl2** layout is the structural template `[RDP-GPL]`
(`broadcom-sdk-416L05/.../rdd/rdd_data_structures_auto.h:6111`):

```
RDD_FC_UCAST_FLOW_CONTEXT_ENTRY (RDP-impl2 TEMPLATE; XRDP adds vport/policer/tunnel):
  +0   flow_hits           u32
  +4   flow_bytes          u32
  +8   b7 multicast_flag | b6 overflow | b5 is_routed | b4 is_l2_accel | b0..10 mtu
  +10  rx_tos              u8
  +12  is_unicast_wfd_nic/any, priority:4, wfd_prio, wfd_idx:2 ...
  +13  egress_phy:2, ip_addresses_table_index:3 ...
  +14  link_specific_union u16
  +16  command_list[80]    <-- cmdlist bytes start at +16
  +96  valid               u8
  +97  reserved:4, command_list_length_64:4, connection_direction:1, connection_table_index:15
```

=> Implementation rule: build an `FC_UCAST_FLOW_CONTEXT_ENTRY` with the cmdlist embedded inline,
`command_list_length_32` set, `valid=1`, `vport`/`service_queue_id`/`policer_id`/`is_routed`/
`is_l2_accel` per the resolved flow, and hand it as the NAT-C result to section 1.1. **Exact 6813
field offsets must be re-derived** (RE rdpa.ko `_ucast_prepare_rdd_*` writers, or get the 6813
auto header).

---

## 2. THE CMDLIST (XPE byte-code) — opcode set, encoding, compiled examples

The **XPE (XRDP Packet Engine)** runs a compact byte-code per flow on egress. Builder =
cmdlist.ko; matched source = `xpe_api.armb53_6813.o_saved` (low-level emitter, NOT stripped) +
`cmdlist_ucast/l2_ucast.armb53_6813.o_saved` (compilers).

### 2.1 Command word format `[XRDP-BIN]`

`sym.xpe_api_opcode_name` (xpe_api.o @0x08000040) first insn is `lsr w0, w0, 0x1a` =>
**opcode = command_word >> 26**, i.e. **bits[31:26] = a 6-bit opcode**. Lower 26 bits carry
operands; many commands are followed by inline data words.

- Command/text words and data words are **16-bit, big-endian** — confirmed by the data copy loop
  in `xpe_api.o sym.__command_add.constprop.0` @0x08000818: `ldrh; rev16; strh`. `[XRDP-BIN]`
- `__command_add` lays out three regions, 4-byte-aligned: **.text** (command words), **.data**
  (inline operand data), **.gdma[0/1]** (GDMA descriptors for memcpy-style ops), padded to a
  multiple of 4 (assert `!(total_size % 4)`). Control globals `xpe_ctrl_g`/`target_name_g` track
  `text_size`/`data_size`/gdma sizes/`cmd_list_length_max`. `[XRDP-BIN]`
- Operands byte-granular: `offset8` (max 255), `words8`, `words16`, `words32`, plus bit-field ops
  with `position`/`width`. `[XRDP-BIN]` (debug fmt strings in xpe_api.o .rodata).
- **Targets** (`CMDLIST_CMD_TARGET_*`): `CL` = command-list/packet-header buffer (default, only
  one fully supported here), plus `PKTBUF`. An **input** source `PACKET_LENGTH` feeds
  length-relative adds. `XPE_OFFSET_APPEND` is the only append mode. `[XRDP-BIN]`

### 2.2 Opcode table (bits[31:26]) `[XRDP-BIN]`

Decoded from `xpe_api_opcode_name` switch (branch -> `.rodata.str1.8` `XPE_CMD_OPCODE_*`):

| opcode | name | meaning |
|---|---|---|
| 1 (0x01)  | CMP_JMP  | compare + conditional jump |
| 2 (0x02)  | JMPCOND  | conditional jump |
| 3 (0x03)  | JMPREG   | jump via register |
| 0x13      | MCOPY    | multi-word/memcpy copy (uses GDMA) |
| 0x18      | REPLACE  | replace N bytes/words with inline data |
| 0x1c      | GDMA     | GDMA descriptor (large block move) |
| 0x2c      | MOVE     | move bytes within packet |
| 0x36      | ICSUM    | incremental ones-complement checksum update |
| ~0x18 grp | ADD      | add immediate to a field (TTL dec = ADD -1; (op+0x27)&0x3f<=2 group) `[INFER]` |
| ~0x34 grp | COPY     | copy bytes packet->packet ((op+0x2c)&0x3f<=3 group) `[INFER]` |
| 0x3f      | NOP      | default/no-op (csel fallback) |

Explicit cmp values (1,2,3,0x13,0x18,0x1c,0x2c,0x36,0x3f) **pinned**; ADD/COPY are
compiler-grouped value ranges (exact members `[INFER]` — resolve via each emitter's opcode imm).

### 2.3 Emitter API -> opcode (the open builder's primitives) `[XRDP-BIN]`

xpe_api.o exports (all FUNC, not stripped):

```
xpe_cmd_end                  finalize (terminator / NOP)
xpe_cmd_replace_16 / _32     REPLACE: overwrite 16/32-bit field with immediate (NAT addr/port)
xpe_cmd_replace_bits_16      REPLACE bits: (offset, position, width, data16)  (VLAN/ToS edit)
xpe_cmd_replace_add_input    REPLACE field = field + input(PACKET_LENGTH)  (len-relative)
xpe_cmd_replace_pointer      REPLACE a pointer/offset
xpe_cmd_copy_bytes           COPY/MCOPY bytes packet->packet
xpe_cmd_copy_add_16          COPY 16b then add (length adjust)
xpe_cmd_copy_bits_16         COPY bitfield (src_pos,dst_pos,width)
xpe_cmd_insert_16            insert/expand header bytes (L2/VLAN push)
xpe_cmd_delete_16            delete/shrink header bytes (VLAN pop / strip)
xpe_cmd_decrement_8          ADD -1 to an 8-bit field -> IPv4 TTL / IPv6 hop-limit
xpe_cmd_apply_icsum_16       ICSUM: apply incremental checksum delta
xpe_cmd_apply_icsum_nz_16    ICSUM but skip if csum field == 0 (UDP)
xpe_cmd_compute_csum_16      full ones-complement checksum over a range
xpe_cmd_save_16 / restore_16 save/restore a field (around tunnel encap)
xpe_cmd_memcpy / copy_bytes  block copy (GDMA-backed)
xpe_cmd_offset               set current working offset
xpe_cmd_sop_push_replace     push bytes at Start-Of-Packet + replace (encap)
xpe_cmd_sop_pull_replace     pull/remove bytes at SOP + replace (decap)
```

Higher-level header builders in cmdlist_ucast.o: `addIpv4Commands`, `_build_l2gre_header`,
`_build_gre_v4/v6_del_header`, `_translate_mapt_v4tov6_header`,
`cmdlist_ucast_llc_snap_header_insert`, `cmdlist_ucast_wlan_eth_header_create`. `[XRDP-BIN]`

### 2.4 Compiled flow examples (ordered call traces) `[XRDP-BIN]`

**(a) IPv4 routed + NAPT** — `cmdlist_ucast.o sym.addIpv4Commands` (file off 0xe0, len 1804).
CALL26 relocs in order (log calls elided):

```
xpe_cmd_decrement_8      @0x204   ; decrement IPv4 TTL
xpe_cmd_replace_16       @0x2a8   ; replace half of IP addr / part of NAT
xpe_cmd_replace_32       @0x3fc   ; replace full 32-bit IP SA or DA (SNAT/DNAT)
xpe_cmd_apply_icsum_16   @0x45c   ; fix IP header checksum (incremental)
xpe_cmd_replace_16       @0x56c   ; replace L4 SPort/DPort (NAPT)
xpe_cmd_apply_icsum_16   @0x694   ; fix TCP/UDP checksum (incremental)
... (further replace/icsum + L2 header insert)
```

Whole-function `xpe_cmd_*` usage profile (addIpv4Commands + helpers): copy_add_16 x36,
copy_bits_16 x25, compute_csum_16 x21, decrement_8 x12, apply_icsum_16 x7, apply_icsum_nz_16 x6,
replace_bits_16 x6, replace_32 x2, replace_16 x2, sop_push/pull_replace, replace_add_input x2.

**(b) L2 bridge** — `cmdlist_l2_ucast.o sym.cmdlist_l2_ucast_create_bin`. Profile: copy_add_16 x4,
replace_bits_16 x3, decrement_8 x2, copy_bits_16 x2, compute_csum_16 x2, apply_icsum_16 x2,
sop_push_replace x1, sop_pull_replace x1, save_16/restore_16 x1.
=> L2 path dominated by **VLAN edit** (replace_bits_16/copy_bits_16) and **push/pull**
(sop_push/pull_replace), no IP rewrite. Short program -> realistic Phase-1 target.

### 2.5 Command-word bit-packing + list framing — PINNED from xpe_api.o disasm `[XRDP-BIN]`

Resolves UNKNOWN #3. Addrs are file offsets in `xpe_api.armb53_6813.o` (r2:
`-a arm -b 64 -e io.va=false -e bin.cache=true`).

**Command word = 32-bit, stored BIG-ENDIAN.** The shared low-level emitter
`sym.__command_add` @0x740 builds each command word in a register, then `rev w5, w22`
(byte-swap) + `str w5, [.text]` -> the word lands MSB-first in the buffer. Inline data
words are `rev16`'d (16-bit BE) into a separate `.data` region; `xpe_cmd_end` later
relocates the data "from" offsets and concatenates `.text` + `.data`. So the on-wire word
is read with `opcode = word>>26` (bits[31:26]) exactly as live-confirmed (REPLACE 0x18 ->
top byte 0x60).

**Byte structure of the 32-bit word** (from the clean single-purpose emitters):

| field | bits | source |
|---|---|---|
| opcode | [31:26] | `>>26`; byte0 = opcode<<2 \| sub-flags |
| sub-flags | [25:24] | size class etc. (decrement: even=0b00 in 0x6a; odd path sets 1) |
| "to" word-count / dst offset | [23:16] | `(offset>>1)+1`, `bfi w*, w*, 0x10, 8` |
| "from" offset (data ref) OR inline immediate | [15:8] / [15:0] | data-region ref (0x94-relative, relocated by xpe_cmd_end) OR a 16-bit immediate (icsum16) |
| sentinel / mask | [7:0] | 0xff for "all bytes / to end" ops (decrement_8) |

Per-emitter base words (the `mov w*, 0x........` seed before the `bfi` inserts):

| emitter | addr | base word | opcode (>>26) | notes |
|---|---|---|---|---|
| xpe_cmd_replace_bits_16 | 0x1e60 | 0x50000000 | 0x14 | inline data = `data16<<position`; position/width packed into byte0/low byte via bfi/bfxil |
| xpe_cmd_decrement_8 | 0x2c90 | 0x64000000 -> byte0 set 0x6a | 0x1a (=ADD) | TTL/hop-limit -1; no inline data; byte3=0xff |
| xpe_cmd_apply_icsum_16 | 0x2da0 | 0x70000000 | 0x1c | `bfxil w22, icsum16, 0, 16` -> the 16-bit immediate is INLINE in the command word's low half |
| xpe_cmd_replace_16/_32 | 0x1d60 / 0x1de0 | via 0xae0/0x310 | 0x18 | nbytes in [30:24], from-offset (0x94-based) in [23:16]; data appended to .data as words16/words32 |

(The 6-bit `>>26` opcode values emitted differ from the §2.2 switch *cmp* values for
ADD/ICSUM groups - §2.2 reads the opcode-name LUT which buckets ranges; the **emitted**
values above are what real silicon decodes. REPLACE=0x18 and the byte0=opcode<<2 placement
are the two facts also confirmed live.)

**LIST FRAMING — there is NO NOP terminator; the list is LENGTH-DELIMITED.**
`sym.xpe_cmd_end` @0x1450: walks the `.text` words to relocate the "from" offsets, then
(@0x16e0) `mov w2, 0xfc` + a `str w2, [tail, x0, lsl 2]` loop **fills the trailing slack of
the context's `command_list[]` slot with the byte 0xfc**. That 0xfc pad lies *past*
`cmd_list_data_length` and is never decoded. The Runner executes commands until it reaches
`cmd_list_data_length`. The final SOP/GDMA descriptor for L2 header insert is emitted by
the helper @0xa40 as words `0x08800021` / `0xb080....` (these are the `08 80 00 21 b0 80
c1 88 b0 80 ...` bytes seen trailing the live body - they are part of the cmdlist's GDMA
descriptor region, NOT context pad as previously assumed).

**Worked decode of the captured live 28-byte body** (GDX-local-delivery flow; see
offload-live-validation.md). As BE 32-bit command words:
```
60 14 eb 98 | 3f 00 60 14 | 00 00 00 00 | XX 06 00 20 | 00 14 18 04 | 7c 01 00 00 | 18 94 ff ff
W0=0x6014eb98  op=0x18 REPLACE, to-byte=0x14, ref/data 0xeb98
W1=0x3f006014  0x3f00 = a relocated "from" half-word (NOT a NOP), 0x6014 = REPLACE op repeat
W2=0x00000000  (zeroed slot / cleared ref)
W3=0xXX060020  XX = per-flow GDX/SOP selector immediate (56..59 across the 4 flows)
W4=0x00141804  to/ref bytes; the 0x18 low byte = relocated REPLACE ref tag (orr ...,0x18 @0x15a4)
W5=0x7c010000  from-offset / length descriptor
W6=0x1894ffff  0x18 ref tag + 0x94 (the 0x94 data-region base) + 0xffff "to end" sentinel
```
The two facts that are direction-independent and match our builder: **opcode in bits[31:26]
(REPLACE=0x18)** and **byte0 = opcode<<2** (live 0x60). The "3f 00" is conclusively NOT a
terminator. This is a *structural / framing* validation - a byte-for-byte program match
needs a live L2-accel or routed-NAT flow (none were captured; forbidden to generate).

---

## 3. HASH @0x82920000 and CNPL @0x82948000 — roles & registers `[XRDP-BIN]`

### 3.1 HASH/CAM engine @ 0x82920000
CAM-backed hash with a **context RAM** — multicast/IPTV/aux lookups, NOT main unicast NAT-C.
Host surface (ag_drv_hash_*): `cam_base_addr_set`, `cam_configuration_tm_cfg_set`,
`context_ram_context_23_0_*` (24-bit context slices), `cam_indirect_key_in_*`/`_rslt_*`/`_vlid_out_*`,
`drv_hash_init`, `drv_hash_set_aging`, `ds_hash_index_get`, `_drv_hash_encode_cam_key`,
`rdd_iptv_hash_key_entry_compose` (IPTV main consumer). Open-driver: **skip for v1**; only for
HW multicast/IPTV.

### 3.2 CNPL @ 0x82948000 (Counter aNd Policer Layer)
`ag_drv_cnpl_counter_cfg_set/get` (per-flow stats group/id), `ag_drv_cnpl_policer_cfg_set/get` +
`policers_configurations_*` (per_up, pl_calc_type token-bucket), `cnpl_buf_mng_counters_*`,
generic `rdp_drv_counter_read/clear/set_cli`. Context `policer_id` -> CNPL policer; flow stats
(flow_hits/flow_bytes/q_bytes_cnt) read from CNPL for conntrack ageing. v1: policing optional,
counters needed for flowtable stats.

---

## 4. FLOW-LEARN -> PROGRAM SEQUENCE and the MAINLINE mapping

### 4.1 Stock sequence (oracle) `[XRDP-BIN]`/`[XRDP-GPL]`

```
1st pkt misses NAT-C -> trap to CPU (CPU_RX ring, cpu_reason) -> Linux fwd + conntrack
   blog.c captures resolved flow (Blog_t: 5-tuple, NAT xlate, VLANs, qos)        [GPL]
   pktflow.ko (fcache): established & offloadable? -> blog_notify                 [PROPRIETARY]
   pktrunner.ko: L2L3ParseBlogFlowParams -> fhwPktRunnerActivate                  [PROPRIETARY]
        builds rdpa {key,result}, calls pktrunner_ucast_cmdlist_create
   cmdlist.ko: cmdlist_ucast_create_bin / cmdlist_l2_ucast_create_bin            [PROPRIETARY]
        -> emits XPE byte-code into command_list[] (section 2)
   rdpa.ko ucast/l2_ucast/ip_class object:                                       [PROPRIETARY core]
        _ucast_prepare_rdd_ip_flow_key_params  -> 16B NAT-C key
        _ucast_prepare_rdd_ip_flow_*_result    -> FC_UCAST_FLOW_CONTEXT_ENTRY
        drv_natc_key_result_entry_var_size_ctx_add(tbl,key,context)  (section 1.1)
   NAT-C engine writes key+context into DDR bucket/bin (indirect regs)
2nd+ pkt: NAT-C HIT -> run command_list (section 2) on egress -> vport/queue/policer -> DONE in HW
```

### 4.2 Mapping to mainline netfilter-flowtable HW offload

Mainline `flow_offload` hands the driver a resolved 5-tuple + FLOW_ACTION_* list (mtk_ppe /
`mtk_ppe_offload.c` precedent):

| mainline flow_action | cmdlist op(s) | context field |
|---|---|---|
| 5-tuple (key) | — | -> 16B NAT-C key (1.1), mask per direction |
| FLOW_ACTION_MANGLE IPv4 SA/DA | replace_32 + apply_icsum_16 | is_routed=1 |
| FLOW_ACTION_MANGLE L4 sport/dport | replace_16 + apply_icsum_16/nz_16 | — |
| TTL/hoplimit dec (routed) | decrement_8 + apply_icsum_16 | is_routed=1 |
| FLOW_ACTION_VLAN_PUSH | sop_push_replace / insert_16 + replace_bits_16 | — |
| FLOW_ACTION_VLAN_POP | sop_pull_replace / delete_16 | — |
| FLOW_ACTION_VLAN_MANGLE (prio/vid) | replace_bits_16 / copy_bits_16 | — |
| FLOW_ACTION_CSUM | compute_csum_16 | is_hw_cso |
| L2 MAC rewrite (routed egress) | insert L2 header (replace_16/copy_bytes) | is_routed=1 |
| egress dev -> port/queue | — | vport, service_queue_id |
| FLOW_ACTION_POLICE | — | policer_id -> CNPL (3.2) |
| stats / ageing | read CNPL counters | flow_hits/bytes/q_bytes_cnt |
| FLOW_ACTION_TUNNEL_* | _build_gre_* / mapt / sop_push | tunnel_index_ref |

Hook point: register a **flowtable offload block** (`.ndo_setup_tc(TC_SETUP_FT)` / `nf_flow_table`
callbacks). On FLOW_OFFLOAD_ADD compile actions -> cmdlist+context, call NAT-C add. On DEL/ageing
delete the entry and read counters. Structurally identical to mtk_ppe_offload.c.

---

## 5. OPEN OFFLOAD ARCHITECTURE — design, GPL-vs-clean-room, phasing

### 5.1 Layering (do NOT port blog/pktflow/pktrunner)

```
 Linux conntrack + nf_flow_table  -- resolved {5-tuple, FLOW_ACTION_*}        [MAINLINE GPL]
        |  flowtable offload block (.flow_offload add/del/stats)
        v
 OPEN flow translator  -- map actions -> cmdlist ops + context fields (4.2)   [NEW, clean]
        v
 OPEN cmdlist (XPE) builder  -- emit byte-code (section 2 opcodes/encoding)    [CLEAN-ROOM RE]
        v
 OPEN context builder  -- fill FC_UCAST_FLOW_CONTEXT_ENTRY (1.3)              [port RDD layout + RE 6813 offsets]
        v
 OPEN NAT-C writer  -- key mask + indirect engine write (1.1)                 [RE drv_natc_*]
        v
 NAT-C DDR table  -- HW hit runs cmdlist on egress                            [HW + GPL microcode blob]
```
MAC/switch/PHY = mainline mdio-bcm-unimac + b53/bcm_sf2 DSA(4916) + phylink. Runner microcode =
ship Broadcom GPLv2 image via request_firmware.

### 5.2 GPL-portable vs clean-room

| Piece | Status | Source |
|---|---|---|
| Flow learning | FREE / mainline | nf_flow_table, TC-flower; mtk_ppe |
| Context layout (concepts + RDP offsets) | GPL-portable template | [RDP-GPL] 416L05 auto header |
| Context exact 6813 offsets | RE (autogen stripped) | RE rdpa.ko _ucast_prepare_rdd_* |
| NAT-C add/mask/indirect-write seq | RE'd, re-implementable | [XRDP-BIN] 1.1 |
| NAT-C DDR geometry | RE'd | [XRDP-BIN] 1.2 |
| ag_drv_natc/cnpl/hash accessors | autogen pattern GPL; 6813 vals RE | pattern [RDP-GPL], vals [XRDP-BIN] |
| XPE cmdlist opcode set + encoding | RE'd to buildable spec (sec 2) | [XRDP-BIN] xpe_api.o (NOT stripped) |
| XPE cmdlist *compiler* (action->bytes) | CLEAN-ROOM RE — the hard piece | read cmdlist_ucast.o, write fresh |
| Runner microcode (interpreter) | ship as GPL firmware blob | device fw partition |

Biggest upgrade vs prior note: the 6813-matched `xpe_api.armb53_6813.o_saved` is **NOT stripped**,
so opcode field (>>26, 6-bit), BE 16-bit word format, .text/.data/.gdma layout, and every
emitter's semantics are recoverable to a clean buildable spec without copying proprietary code.
The cmdlist *compiler* (which emitter sequence for which action, exact operand math) is still
clean-room but now well-scoped.

### 5.3 Effort / risk / phasing

| Phase | Deliverable | Effort | Risk |
|---|---|---|---|
| 0 | CPU dumb pipe (slow path, no accel) | done-able | med (FW blob) |
| 1 | HW L2-bridge offload (forward + VLAN push/pop/mangle) via flowtable->cmdlist_l2->NAT-C | weeks-months | med — short cmdlist (2.4b), L2 layout templated |
| 2 | HW L3 route + NAT/NAPT (dec-TTL, addr/port replace, icsum) | months-quarters | high — full XPE compiler + 6813 offsets |
| 3 | tunnels (GRE/VXLAN/MAP-T), QoS/policer, IPv6, multicast (HASH/CAM) | long tail | high |

**Verdict:** Phase 1 (L2 HW offload) is now a concrete, sourced design. Phase 2 ("full NAT at
10G") remains **VERY-HARD** — needs the clean-room XPE cmdlist compiler + exact 6813
context-entry offsets — but is **no longer a black box**: opcode encoding, NAT-C add ABI, and the
action->op mapping are all pinned. Legally the cmdlist compiler + context builder must be
clean-room (blobs cannot ship); microcode ships as a GPLv2 firmware blob.

---

## 6. PINNED / INFERRED / UNKNOWN

### Pinned [XRDP-BIN]/[XRDP-GPL]
- NAT-C = primary unicast flow cache (DDR, <=8 tables, bucket/bin), key=16B masked BE,
  result=FC_UCAST_FLOW_CONTEXT_ENTRY; add via drv_natc_key_result_entry_var_size_ctx_add ->
  mask -> key_idx_get -> indirect eng_key_result_write + eng_command_write(cmd=3).
- NAT-C result IS the context entry (string ...NATC_RESULT_FC_UCAST_FLOW_CONTEXT_ENTRY...); the
  context embeds the cmdlist.
- Complete XRDP context field set (1.3); command_list_length_32, vport, service_queue_id,
  policer_id, tunnel_index_ref are 4916-specific additions.
- XPE command word = opcode in bits[31:26] (lsr #26); 16-bit big-endian words; .text/.data/.gdma
  regions, 4-byte aligned; byte-granular offset8/words{8,16,32}.
- Opcode values: CMP_JMP=1, JMPCOND=2, JMPREG=3, MCOPY=0x13, REPLACE=0x18, GDMA=0x1c, MOVE=0x2c,
  ICSUM=0x36, NOP=0x3f.
- Full emitter API (2.3); ordered IPv4-NAT compiled program (2.4a); L2 profile (2.4b).
- HASH=CAM/IPTV-multicast aux; CNPL=counters+policers; context policer_id->CNPL.
- rdpa.ko built for BCM6813 (path string) = exact 4916 match.

### Inferred [INFER]
- ADD/COPY exact numeric opcodes (compiler-grouped ranges, not single cmps).
- decrement_8 = ADD(-1); apply_icsum_nz = ICSUM-skip-if-zero (UDP). Tunnel ops map to
  tunnel_index_ref + sop_push/save/restore.
- Context byte offsets follow RDP-impl2 template with XRDP fields inserted.

### UNKNOWN — and what resolves each
1. Exact 6813 FC_UCAST_FLOW_CONTEXT_ENTRY byte offsets/bitfields (autogen header stripped).
   -> RE rdpa.ko _ucast_prepare_rdd_ip_flow_*_result / _l2_ucast_prepare_rdd_* writers (fixed
   MWRITE offsets), or obtain 6813 rdd_data_structures_auto.h.
2. NAT-C HW hash polynomial / bucket selection (engine-internal). -> RE drv_natc_key_idx_get +
   ag_drv_natc_eng_hash_get; or rely on the engine (write key+result, let HW place).
3. ~~Exact operand bit-packing of each XPE opcode below bit 26.~~ **RESOLVED** (sec 2.5):
   32-bit BE command word, byte0=opcode<<2|flags, byte1="to" word-count, byte2/3="from"
   data-ref OR inline immediate (icsum16), byte3=0xff sentinel; list is LENGTH-DELIMITED by
   cmd_list_data_length (no NOP terminator; trailing slot padded with 0xfc bytes by
   xpe_cmd_end @0x16e0). Disassembled from xpe_api.armb53_6813.o (__command_add @0x740,
   decrement_8 @0x2c90, apply_icsum_16 @0x2da0, replace_bits_16 @0x1e60, xpe_cmd_end @0x1450).
   Caveat: the live capture was GDX-local so this pins the ENCODING/FRAMING, not a byte-match
   of our L2/NAT programs.
4. cmdlist<->microcode version coupling. -> confirm against device fw partition Runner image gen.
5. Which NAT-C table id / key-mask maps to which direction/flow-class. -> RE rdd_ag_natc_*_mask_*
   callers + data_path_natc_init.

### What a READ-ONLY device observation would add (ground truth)
Dump an active NAT'd flow's NAT-C entry (key + context + cmdlist bytes) and a few variants (plain
L2, routed, NAT, VLAN, tunnel) via stock CLI (bcm_natc_* cli, /proc flow tables, the ...CONTEXT_ENTRY...
dump path). That single capture would: (a) confirm exact 6813 context byte offsets (UNKNOWN #1),
(b) give real cmdlist byte sequences to validate the opcode encoding (UNKNOWN #3), (c) reveal
table-id/mask mapping (UNKNOWN #5) — collapsing most unknowns. Read-only only; never write/reset
NAT-C on the live device.

---

## SOURCE INDEX (this pass)

XRDP-matched GPL (dev-build ~/re-sdk/asuswrt-merlin.ng/release/src-rt-5.04behnd.4916/):
- bcmdrivers/broadcom/char/cmdlist/impl1/xpe_api.armb53_6813.o_saved — XPE emitter (NOT
  stripped): opcode word format, opcode table, emitter API, .text/.data/.gdma layout
- .../cmdlist_ucast.armb53_6813.o_saved — addIpv4Commands (L3+NAT compile sequence), tunnels
- .../cmdlist_l2_ucast.armb53_6813.o_saved — cmdlist_l2_ucast_create_bin (L2/VLAN compile)

XRDP device binary (CT 310 /opt/re-bins/):
- rdpa.ko (built .../rdp/projects/BCM6813/...): drv_natc_key_result_entry_var_size_ctx_add
  @0x080a8d70, drv_natc_eng_key_result_write, drv_natc_eng_command_write,
  drv_natc_find_empty_hash_key_entry, ag_drv_natc_*, ag_drv_cnpl_*, ag_drv_hash_*,
  FC_UCAST_FLOW_CONTEXT_ENTRY_* dump strings, xrdp_drv_natc_ddr_cfg_ag.c accessors

RDP-impl2 template (dev-build ~/re-sdk/broadcom-sdk-416L05/):
- shared/broadcom/rdp/impl2/rdd/rdd_data_structures_auto.h:6111 — RDD_FC_UCAST_FLOW_CONTEXT_ENTRY
  byte layout (template; cmdlist[80] @ +16, valid @ +96, length_64 @ +97)
- rdd_l2_ucast.c, rdd_router.c, rdd_lookup_engine.c — connection/context write pattern

Mainline reuse: net/netfilter/nf_flow_table_offload.c, drivers/net/ethernet/mediatek/mtk_ppe*.c
(reference flowtable HW-NAT driver), drivers/net/dsa/{b53,bcm_sf2}, drivers/net/mdio/mdio-bcm-unimac.
