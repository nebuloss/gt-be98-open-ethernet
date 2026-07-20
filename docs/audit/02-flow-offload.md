# Audit 02 — Flow-offload control plane (HW-acceleration host side)

Subsystem files audited:

- `driver/runner/flow_offload.c` (837 lines)
- `driver/runner/flow_offload.h` (201 lines)

Read for accuracy / cross-referenced (not re-audited here):

- `driver/runner/cmdlist.{c,h}` — the XPE byte-code emitter this subsystem calls
- `driver/runner/bcm4916_runner.h` — `struct xrdp_offload`, the `xrdp_natc_*` MMIO I/O prototypes
- `re-notes/rdpa-offload-controlplane.md`, `re-notes/xrdp-offload-abi.md`,
  `re-notes/offload-phase1-status.md`, `re-notes/offload-phase2-status.md`

All `file:line` references are to the two audited files unless another path is given.

---

## 1. Purpose

This is the **host side of the hardware flow-accelerator** — the code that turns a Linux
forwarding decision into a Runner (XRDP) fast-path flow so that, after the first packet, an
L2-bridge / L3-routed / NAT'd connection is forwarded entirely in hardware and the quad-A53
CPU never sees it again.

Where it sits in the datapath (from `re-notes/xrdp-offload-abi.md` sec 4.1):

```
1st packet of a flow  -> MISS in NAT-C -> trap to CPU (slow path)
                         Linux bridge / ip_forward + conntrack resolves it
                         nf_flow_table / TC-flower offers the resolved flow to the driver
                         >>> flow_offload.c compiles it into a NAT-C entry <<<
2nd..Nth packet        -> HIT in NAT-C -> Runner runs the embedded cmdlist on egress
                         (NAT / VLAN / TTL / csum edits) -> forwarded in HW, CPU idle
```

`flow_offload.c` is the open analog of the closed Broadcom control plane
(`pktflow.ko` + `pktrunner.ko` + `cmdlist.ko` + `rdpa.ko` ucast core). Instead of porting that
proprietary stack it plugs into **mainline's netfilter-flowtable / TC-flower HW-offload
framework**, modelled line-for-line on `drivers/net/ethernet/mediatek/mtk_ppe_offload.c`
(the only mainline HW-NAT-offload precedent). Its job for each offloaded flow is:

1. Parse the resolved flow (5-tuple / L2 tuple + action list) into a flat `struct xrdp_flow`.
2. Compile the header edits into an **XPE cmdlist** (via `cmdlist.c`).
3. Build the **`FC_UCAST_FLOW_CONTEXT_ENTRY`** ("result" / context) that embeds that cmdlist
   plus egress metadata (vport, service queue, flags).
4. Build the **16-byte masked big-endian NAT-C key** from the flow tuple.
5. Hand {key, context} to the conduit driver's `xrdp_natc_add()` which stages them into PSRAM
   and issues the indirect NAT-C "add" command.
6. Track the entry in an rhashtable keyed by the flowtable cookie so DESTROY/STATS can find it.

The **fast-path vs slow-path decision is made in hardware**, not here: NAT-C HIT = fast path,
NAT-C MISS = trap to CPU. This subsystem only *populates* the NAT-C table; the first-packet
trap and the CPU forwarding are the slow-path conduit (audit 01 territory). What is genuinely
implemented in the open driver, and what still leans on RE'd-but-unverified contracts with the
stock stack, is spelled out in section 5.

**Scope split.** Phase 1 = L2 bridge + VLAN (`is_l2_accel`). Phase 2 = L3 route + NAT/NAPT
(`is_routed`, optionally `is_nat`). Both are implemented in this file; the dispatch is by
`addr_type` in `xrdp_parse_flow`. IPv6, tunnels, multicast, and per-flow QoS/policing are not
implemented.

---

## 2. Architecture & data flow

### 2.1 Control-plane entry points

Two ways a flow reaches this code:

1. **Real flowtable path (compiles + registers, never exercised live):**
   `ndo_setup_tc` on the conduit netdev → `xrdp_offload_setup_tc()` (`flow_offload.c:671`) →
   `xrdp_setup_tc_block()` (`:631`) registers a `flow_block_cb`. The kernel then calls
   `xrdp_setup_tc_block_cb()` (`:618`) for each `TC_SETUP_CLSFLOWER` command, which funnels into
   `xrdp_flow_cmd()` (`:595`) → `xrdp_flow_replace / _destroy / _stats`.
2. **Debugfs self-test path (the path actually proven in QEMU):**
   `xrdp_offload_selftest()` (`:707`) and `xrdp_offload_nat_selftest()` (`:774`) build a
   synthetic `struct xrdp_flow` and drive the *same* builder + `xrdp_natc_add` code, skipping
   the TC dissector. Per `offload-phase1-status.md` §6 and `offload-phase2-status.md` §6, the
   flowtable trigger was never fired against a live conntrack flow (test kernel had
   `CONFIG_NF_FLOW_TABLE` unset, single-NIC emulation has no 2-port forwarding topology).

### 2.2 Per-flow build pipeline (`xrdp_flow_replace`, `:497`)

```
flow_cls_offload
   │  xrdp_parse_flow()            -> struct xrdp_flow  (tuple + actions, flat)
   ▼
struct xrdp_flow
   │  flow.is_routed ? xrdp_build_nat_cmdlist() : xrdp_build_l2_cmdlist()
   ▼                               -> struct xpe_cmdlist (XPE byte-code)
   │  xrdp_build_ctx()             -> struct fc_ucast_ctx  (cmdlist embedded @ +24)
   │  xrdp_build_key()             -> struct natc_key  (16B masked BE)
   ▼
   xrdp_natc_add(o, key, ctx, &idx)   [conduit driver: PSRAM staging + indirect add]
   │
   rhashtable_insert_fast(cookie -> {natc_idx, key})
```

### 2.3 Hardware blocks driven (indirectly)

This file never touches MMIO directly — it produces byte buffers and calls the conduit
driver's `xrdp_natc_*` helpers, which own the ioremapped XRDP window. The blocks involved
(bases from `re-notes/xrdp-offload-abi.md` and `bcm4916_runner.h`):

| Block | Base (rel. rdpa window `0x82000000`) | Role in the offload |
|---|---|---|
| **NAT-C** (NAT Cache, DDR) | `0x82950000` — `XRDP_OFF_NATC` = `0x00950000` (`flow_offload.h:46`) | primary 5-tuple flow cache; entry = 16B key + `FC_UCAST_FLOW_CONTEXT_ENTRY` result |
| **PSRAM** staging | `XRDP_OFF_PSRAM` = `0` (`bcm4916_runner.h:41`) | driver stages key/ctx/index/cmd here for the indirect add (contract offsets, §5) |
| **CNPL** (counters/policers) | `0x82948000` | per-flow `flow_hits`/`flow_bytes`; **not read** — stats stub to 0 |
| **HASH/CAM** | `0x82920000` | multicast/IPTV only; unused here |

The NAT-C indirect-add sub-ABI (contract placeholders in `flow_offload.h:56-61`):
`NATC_STAGE_KEY 0x0100`, `NATC_STAGE_CTX 0x0120`, `NATC_INDIR_INDEX 0x0200`,
`NATC_INDIR_CMD 0x0204`; `NATC_CMD_ADD 3` (matches the stock
`drv_natc_eng_command_write(...,3)`), `NATC_CMD_DEL 4` (open extension — stock has no del=4;
see §5).

### 2.4 The two on-wire artifacts this file produces

**(a) The cmdlist** — a length-delimited stream of 32-bit big-endian XPE command words (each
emitted as two 16-bit BE half-words by `cmdlist.c`). Opcode = word `>>26` = `byte0>>2`. Built
by `xrdp_build_l2_cmdlist` (VLAN edits) or `xrdp_build_nat_cmdlist` (dec-TTL / addr+port
replace / icsum). Length-delimited by `cmd_list_data_length`; trailing slot slack is padded
with the byte `0xfc` (no NOP terminator — `re-notes/xrdp-offload-abi.md` §2.5).

**(b) The context / result** — `struct fc_ucast_ctx`, a flat 124-byte buffer that is the
driver↔QEMU-model contract stand-in for the real bitfield `FC_UCAST_FLOW_CONTEXT_ENTRY`. The
cmdlist is memcpy'd inline at **byte +24** (`XPE_CTX_CMDLIST_OFF`), which is the one offset
pinned to real silicon (`re-notes/stock-watch-capture.md` sec 2); every other `CTX_OFF_*` is a
private contract offset (§3, §5).

---

## 3. Data structures

### 3.1 `struct xrdp_flow` (`flow_offload.h:138-181`)

The flat, HW-agnostic description of a resolved flow, produced by the parser and consumed by
all three builders. Fields:

| Field | Meaning |
|---|---|
| `bool is_routed` | L3 route + NAT/NAPT (Phase 2) vs pure L2 bridge (Phase 1). Selects cmdlist builder, context flags, and key class. |
| `u8 mac_da[6] / mac_sa[6]` | L2 key (bridge path). |
| `__be16 ethertype` | L2 key ethertype. **Hardcoded to `ETH_P_IP`** by the parser (see finding F8). |
| `u16 ingress_vport` | source vport; enters the **L2** key only. |
| `u16 vlan_in` | match VID; in the L2 key only a *presence bit* is derived, not the VID (finding F9). |
| `u8 ip_proto` | `IPPROTO_TCP`/`UDP`; selects the L4 csum offset. **Does not enter the key** (finding F5). |
| `u8 ip_tos` | live NAT-C key byte 12 (splits HW flows by DSCP). Only set via TC parse if present. |
| `bool tcp_pure_ack` | live key byte 14 bit6; never set by the parser (finding F11). |
| `__be32 ip_sa / ip_da` | original (pre-NAT) 5-tuple addrs — the L3 key. |
| `__be16 l4_sport / l4_dport` | original 5-tuple ports — the L3 key. |
| `bool nat_sip / nat_dip` + `__be32 *_val` | SNAT/DNAT rewrite target IP (from FLOW_ACTION_MANGLE). |
| `bool nat_sport / nat_dport` + `__be16 *_val` | SNAPT/DNAPT rewrite target port. |
| `u16 egress_vport / service_queue_id` | egress context fields. |
| `bool vlan_push / vlan_pop / vlan_mangle` + vid/pcp | Phase-1 VLAN actions. |
| `bool is_hw_cso` | HW checksum-offload flag → context byte. |

### 3.2 `struct natc_key` (`flow_offload.h:75-77`)

`__be32 w[4]` — the 16-byte masked big-endian NAT-C key. Two key *classes* are packed by
`xrdp_build_key` depending on `is_routed` (see that function, §4). Stored big-endian per ABI
§1.1 (`cpu_to_be32` on each word at the end of the builder).

### 3.3 `struct fc_ucast_ctx` (`flow_offload.h:100-103`)

```c
struct fc_ucast_ctx { u8 buf[XPE_CTX_ENTRY_MAX /*124*/]; u16 len; };
```

The context ("result") buffer. `len` is the total meaningful bytes (`= CTX_OFF_VALID + 1`,
i.e. 107 with the current offsets).

**Context byte offsets** — a driver↔QEMU-model *contract*, NOT the real 6813 bitfield layout
(`flow_offload.h:97-119`):

| Macro | Value | Meaning |
|---|---|---|
| `XPE_CTX_CMDLIST_OFF` | `24` | cmdlist embedded here. **Real-silicon-pinned** (was 16 in RDP-impl2 template). |
| `XPE_CTX_ENTRY_MAX` | `124` | real `FC_UCAST_FLOW_CONTEXT_ENTRY` size. |
| `CTX_OFF_FLAGS` | `8` | flag byte: `CTX_FLAG_MCAST` BIT7, `CTX_FLAG_IS_ROUTED` BIT5, `CTX_FLAG_IS_L2_ACCEL` BIT4, `CTX_FLAG_IS_NAT` BIT3 (open model hint). |
| `CTX_OFF_VPORT` | `12` | egress vport. |
| `CTX_OFF_SERVICE_Q` | `13` | service_queue_id. |
| `CTX_OFF_IS_HW_CSO` | `14` | bit0 = is_hw_cso. |
| `CTX_OFF_CMDLIST_DLEN` | `24+80+0 = 104` | `cmd_list_data_length` (byte count) — **contract**; real HW uses `command_list_length_32` (word count) in WORD 1. |
| `CTX_OFF_CMDLIST_LEN` | `105` | `cmd_list_length` (4-byte-aligned byte count) — contract. |
| `CTX_OFF_VALID` | `106` | `valid = 1`. |

Note the cmdlist region is a fixed 80-byte window `[24..104)` (= `XPE_CMDLIST_MAX_BYTES`); the
length fields sit immediately after it at fixed offsets regardless of the actual cmdlist size.

### 3.4 Packet byte-offset macros (`flow_offload.h:126-135`)

For an **untagged** IPv4 frame with **no IP options** (IHL=5): `L2_HLEN 14`, `IP4_OFF_TTL 22`,
`IP4_OFF_CSUM 24`, `IP4_OFF_SADDR 26`, `IP4_OFF_DADDR 30`, `IP4_HLEN 20`, `L4_OFF_SPORT 34`,
`L4_OFF_DPORT 36`, `TCP_OFF_CSUM 50`, `UDP_OFF_CSUM 40`. These are what the NAT cmdlist builder
feeds to the XPE emitters; they are wrong for tagged or IP-option frames (finding F6).

### 3.5 `struct xrdp_flow_entry` (`flow_offload.c:289-294`)

The rhashtable node: `rhash_head node`, `unsigned long cookie` (hash key = the flowtable
cookie), `u32 natc_idx` (slot returned by `xrdp_natc_add`), `struct natc_key key` (kept so
DESTROY can re-issue the masked key to the delete path). Hash params `xrdp_flow_ht_params`
(`:296-301`) key on `cookie` with automatic shrinking.

### 3.6 NAT-C add/del constants (`flow_offload.h:45-62`) — see §2.3.

### 3.7 `struct xrdp_offload` (defined in `bcm4916_runner.h:653-657`)

`{ struct rhashtable flow_table; void *drv; u16 default_vport; }`. `drv` back-points to the
conduit `runner_priv`; `default_vport` is the egress/ingress vport used by the self-test and
(currently) by *every* real flow too (finding F7).

---

## 4. Function reference (file order)

### `xrdp_build_l2_cmdlist(const struct xrdp_flow *f, struct xpe_cmdlist *cl)` — `flow_offload.c:72`

Compiles the Phase-1 L2/VLAN cmdlist. Calls `xpe_cmdlist_init` then, depending on the VLAN
action, emits at most one edit and terminates with `xpe_cmd_end`:

- `f->vlan_push`: builds `tci = (pcp&7)<<13 | (vid&0xfff)`, `tag = ETH_P_8021Q<<16 | tci`, then
  `xpe_cmd_insert_16(cl, 12, VLAN_HLEN)` to open a 4-byte hole at offset 12 and
  `xpe_cmd_replace_32(cl, 12, tag)` to write the full 802.1Q tag.
- `f->vlan_pop`: `xpe_cmd_delete_16(cl, 12, VLAN_HLEN)` strips the 4-byte tag.
- `f->vlan_mangle`: `xpe_cmd_replace_bits_16(cl, 14, 0, 12, vid&0xfff)` rewrites only VID bits
  [11:0] of the TCI at offset 14.
- plain L2 forward (no VLAN op): **emits nothing** → the cmdlist is a **0-byte body** (the
  forwarding decision lives in the context, not the cmdlist). The header comment
  (`:49-70`) argues this is correct: the stock L2-forward compiler emits a near-empty program.

**Why:** the offset-12 tag position follows from the 12-byte MAC DA+SA; TCI at offset 14.
**HW ordering:** insert-then-replace for push (make room before writing). No other constraint.
**Callers:** `xrdp_flow_replace` (`:522`), `xrdp_offload_selftest` (`:731`).
**Callees:** `xpe_cmdlist_init`, `xpe_cmd_insert_16/replace_32/delete_16/replace_bits_16`,
`xpe_cmd_end` (all `cmdlist.c`).
**Note (finding F13):** the produced bytes differ from the byte streams logged in
`offload-phase1-status.md` §3 — that evidence predates the pinned per-op encoding in `cmdlist.c`.

### `xrdp_build_nat_cmdlist(const struct xrdp_flow *f, struct xpe_cmdlist *cl)` — `flow_offload.c:120`

Compiles the Phase-2 L3/NAT cmdlist in the exact RE'd `addIpv4Commands` order
(`xrdp-offload-abi.md` §2.4a). After `xpe_cmdlist_init`:

1. `xpe_cmd_decrement_8(cl, IP4_OFF_TTL)` — always (routed ⇒ TTL−1).
2. `xpe_cmd_replace_32(cl, IP4_OFF_SADDR, be32_to_cpu(f->nat_sip_val))` if `nat_sip`;
   `...IP4_OFF_DADDR...` if `nat_dip`. (`be32_to_cpu` because the emitter re-emits big-endian.)
3. `xpe_cmd_apply_icsum_16(cl, IP4_OFF_CSUM)` — always (covers TTL dec + IP addr replaces).
4. `xpe_cmd_replace_16(cl, L4_OFF_SPORT, be16_to_cpu(f->nat_sport_val))` if `nat_sport`;
   `...L4_OFF_DPORT...` if `nat_dport`.
5. `xpe_cmd_apply_icsum_16(cl, l4_csum_off)` **only if** any of `nat_sport/dport/sip/dip` is set,
   where `l4_csum_off = UDP ? UDP_OFF_CSUM : TCP_OFF_CSUM`.
6. `xpe_cmd_end`.

**Why the coalescing:** stock interleaves replace+icsum per field; this emits **one** IP icsum
and **one** L4 icsum. The header comment (`:100-118`) justifies it: ones-complement incremental
checksum is commutative over the deltas, so one fixup after all replaces is equivalent and
shorter. The L4 icsum is gated on the IP replaces too because the L4 pseudo-header covers the
IP addrs — correct. Pure-routing (TTL dec, no NAT) correctly emits IP icsum but **no** L4 icsum
(TTL is not in the L4 pseudo-header).
**HW ordering constraint (load-bearing):** the L4 checksum fixup must follow *all* IP-addr and
port replaces; the code guarantees this by emitting it last.
**Deviation (finding F1):** uses `apply_icsum_16` for UDP; stock uses `apply_icsum_nz_16`
(skip-if-zero) so a UDP datagram with checksum 0 ("no checksum") is not given a spurious one.
`xpe_cmd_apply_icsum_nz_16` is not implemented at all.
**Callers:** `xrdp_flow_replace` (`:520`), `xrdp_offload_nat_selftest` (`:807`).

### `xrdp_build_ctx(const struct xrdp_flow *f, const struct xpe_cmdlist *cl, struct fc_ucast_ctx *ctx)` — `flow_offload.c:158`

Builds the `FC_UCAST_FLOW_CONTEXT_ENTRY` contract buffer. Steps:

1. `dlen = xpe_cmdlist_data_len(cl)` (executable bytes), `clen = xpe_cmdlist_len(cl)`
   (4-byte-aligned bytes). `memset(ctx,0,...)`.
2. Flags at `CTX_OFF_FLAGS` (byte 8): if `f->is_routed` → `CTX_FLAG_IS_ROUTED`, plus
   `CTX_FLAG_IS_NAT` if any NAT field set; else `CTX_FLAG_IS_L2_ACCEL`.
3. `buf[12] = egress_vport & 0xff`, `buf[13] = service_queue_id & 0xff`,
   `buf[14] = BIT(0)` if `is_hw_cso`.
4. Clamp `clen` to `XPE_CMDLIST_MAX_BYTES` (80), `memcpy(&buf[24], cl->buf, clen)` — copies the
   padded buffer including the trailing `0xfc` slack so the slot matches stock fill; the Runner
   only decodes the first `dlen` bytes.
5. `buf[104] = dlen`, `buf[105] = clen`, `buf[106] = 1` (valid). `ctx->len = 107`.

**Why:** produces exactly the byte pattern the QEMU Runner model decodes.
**Buffer safety:** cmdlist region `[24..104)` is 80 bytes; DLEN/LEN/VALID at 104/105/106;
`ctx->len` 107 ≤ 124 — no overflow.
**Findings:** `dlen`/`clen` are `u16` narrowed into single bytes (F2 — benign, max 80 but
silently truncates >255); the two length fields are byte counts, not the real
`command_list_length_32` word count (F3, acknowledged in the comment `:190-198`).
**Callers:** `xrdp_flow_replace`, both self-tests. **Callees:** `xpe_cmdlist_data_len/len`.

### `xrdp_build_key(const struct xrdp_flow *f, struct natc_key *key)` — `flow_offload.c:243`

Packs the 16-byte NAT-C key, two classes selected by `f->is_routed`:

- **L3 (routed):** original/ingress 5-tuple, byte layout pinned from live silicon
  (`stock-watch-capture.md` sec 1). `w0 = ip_sa`, `w1 = ip_da`,
  `w2 = sport<<16 | dport`, `w3 = ip_tos<<24 | NATC_L3_KEY_CLASS_BYTE(0x28)<<16 | k14<<8 |
  NATC_L3_KEY_TRAILER(0x68)`, where `k14 = DIR_US(bit7) | (tcp_pure_ack ? PURE_ACK(bit6) : 0)`.
  The class byte `0x28` and trailer `0x68` are **hardcoded live-observed constants**
  (`:239-242`), captured from a single stock eth0 upstream-TCP table.
- **L2 (bridge):** `w0 = DA[0..3]`, `w1 = DA[4..5]|SA[0..1]`, `w2 = SA[2..5]`,
  `w3 = ethertype<<16 | (ingress_vport&0xfff)<<4 | (vlan_in&0xfff ? 1 : 0)`.

All four words `cpu_to_be32`'d at the end (big-endian per ABI §1.1).

**Why:** lookup happens on ingress before the cmdlist rewrites the packet, so the key is the
*original* tuple. **Callers:** `xrdp_flow_replace` (`:532`), both self-tests.
**Findings:** L3 key omits `ip_proto` (F5) and `ingress_vport` (byte 15 is the fixed constant
`0x68`, F4); the class byte is hardcoded TCP (F5); L2 key encodes only a VLAN *presence* bit,
not the VID (F9). The L3 layout also no longer matches the key logged in
`offload-phase2-status.md` §2 (that evidence used an older `proto`-in-MSB layout — F12).

### `xrdp_parse_mangle(const struct flow_action_entry *act, struct xrdp_flow *out)` — `flow_offload.c:309` (static)

Translates one `FLOW_ACTION_MANGLE` into the NAT rewrite fields, modelled on
`mtk_flow_mangle_ipv4` / `mtk_flow_mangle_ports`.

- `FLOW_ACT_MANGLE_HDR_TYPE_IP4`: `offset == offsetof(iphdr,saddr)` → `nat_sip` + copy val;
  `daddr` → `nat_dip`; else `-EOPNOTSUPP`. Mask ignored (full 32-bit replace, as in mtk).
- `FLOW_ACT_MANGLE_HDR_TYPE_TCP/UDP`: `val = ntohl(act->mangle.val)`. offset 0 with
  `mask == ~htonl(0xffff)` → `nat_dport = cpu_to_be16(val)`; offset 0 otherwise →
  `nat_sport = cpu_to_be16(val >> 16)`; offset 2 → `nat_dport = cpu_to_be16(val)`; else
  `-EOPNOTSUPP`. This mirrors the netfilter mangle word packing (src high 16, dst low 16).
- default htype → `-EOPNOTSUPP`.

**Caller:** `xrdp_parse_flow` (`:428`). No hardware touched.

### `xrdp_parse_flow(struct flow_cls_offload *f, struct xrdp_flow *out, u16 ingress_vport, u16 egress_vport)` — `flow_offload.c:373` (static)

Resolves a `FLOW_CLS_REPLACE` into `struct xrdp_flow`. `memset`s `out`, seeds
ingress/egress vport and `ethertype = htons(ETH_P_IP)`. Reads `FLOW_DISSECTOR_KEY_CONTROL` for
`addr_type`, then branches:

- `addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS` → **Phase 2 routed L3/NAT**: sets `is_routed`;
  requires `KEY_BASIC` (rejects non-TCP/UDP), reads `ip_proto`; reads `KEY_IPV4_ADDRS`
  (`ip_sa/ip_da`); requires `KEY_PORTS` (`l4_sport/dport`). Iterates the action list:
  REDIRECT/MIRRED ignored, CSUM → `is_hw_cso`, MANGLE → `xrdp_parse_mangle`, else
  `-EOPNOTSUPP`. Returns.
- `addr_type != 0` → `-EOPNOTSUPP` (IPv6 etc. not offloaded).
- `addr_type == 0` → **Phase 1 L2 bridge**: requires `KEY_ETH_ADDRS` (`mac_da/mac_sa`);
  optional `KEY_VLAN` (rejects non-802.1Q TPID, stores `vlan_in`). Action loop:
  REDIRECT/MIRRED (egress resolved by caller), CSUM → `is_hw_cso`,
  VLAN_PUSH (rejects non-802.1Q, stores vid+pcp), VLAN_POP, VLAN_MANGLE (stores vid), else
  `-EOPNOTSUPP`. Finally validates both MACs (`-EINVAL` otherwise).

**Why:** mirrors `mtk_flow_offload_replace`. **Caller:** `xrdp_flow_replace` (`:514`).
**Findings:** `ethertype` is forced to IP and the L2 path never reads `KEY_BASIC` n_proto
(F8); VLAN_MANGLE ignores PCP (F10); IPv6 rejected (documented limitation).

### `xrdp_flow_replace(struct xrdp_offload *o, struct flow_cls_offload *f)` — `flow_offload.c:497` (static)

The add path. Rejects a duplicate cookie (`-EEXIST`). Parses the flow with **both** vports =
`o->default_vport` (F7). Builds cmdlist (routed vs L2), checks `cl.overflow` (`-E2BIG`), builds
context. `kzalloc`s an entry, sets `cookie`, builds the key. Calls `xrdp_natc_add`; on failure
`goto free`. Inserts into the rhashtable; on failure `goto del` (un-programs NAT-C) then `free`.
On success logs and returns 0.

**HW ordering:** program NAT-C **before** publishing the rhashtable entry; on rhashtable
failure the NAT-C entry is rolled back. **Concurrency:** all commands serialized by
`xrdp_flow_mutex` (held by `xrdp_flow_cmd`), so the lookup→insert window is safe.
**Finding F14:** the success `pr_info` (`:544`) hardcodes the string `(is_l2_accel)` even for
routed flows — cosmetic/log-only.
**Callees:** `rhashtable_lookup_fast`, `xrdp_parse_flow`, `xrdp_build_{nat_,l2_}cmdlist`,
`xrdp_build_ctx`, `xrdp_build_key`, `xrdp_natc_add`, `xrdp_natc_del`.

### `xrdp_flow_destroy(struct xrdp_offload *o, struct flow_cls_offload *f)` — `flow_offload.c:556` (static)

Looks up by cookie (`-ENOENT` if absent), calls `xrdp_natc_del(o, &entry->key, natc_idx)`,
removes from the rhashtable, frees. Serialized by `xrdp_flow_mutex`.

### `xrdp_flow_stats(struct xrdp_offload *o, struct flow_cls_offload *f)` — `flow_offload.c:572` (static)

Looks up by cookie (`-ENOENT`), calls `xrdp_natc_stats(o, natc_idx, &pkts, &bytes)` and
`flow_stats_update(&f->stats, bytes, pkts, 0, 0, FLOW_ACTION_HW_STATS_DELAYED)`.
**Finding F15:** `xrdp_natc_stats` is a stub returning 0 (CNPL counter read not wired — comment
`:582-586`), so HW stats are always 0; flows age out only via the flowtable GC timeout.

### `xrdp_flow_cmd(struct xrdp_offload *o, struct flow_cls_offload *cls)` — `flow_offload.c:595` (static)

Takes `xrdp_flow_mutex`, dispatches `FLOW_CLS_REPLACE/DESTROY/STATS` to the three handlers
(else `-EOPNOTSUPP`), unlocks, returns. `xrdp_flow_mutex` is a single **global** static mutex
(`:593`).

### `xrdp_setup_tc_block_cb(enum tc_setup_type type, void *type_data, void *cb_priv)` — `flow_offload.c:618` (static)

The registered `flow_setup_cb_t`. Accepts only `TC_SETUP_CLSFLOWER` (else `-EOPNOTSUPP`),
casts `cb_priv` to `struct xrdp_offload *`, forwards to `xrdp_flow_cmd`.

### `xrdp_setup_tc_block(struct xrdp_offload *o, struct net_device *dev, struct flow_block_offload *f)` — `flow_offload.c:631` (static)

Manages the `flow_block_cb`. **Rejects any `binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS`**
(finding F16). Sets `f->driver_block_list = &xrdp_block_cb_list` (a global static list, `:629`).
On `FLOW_BLOCK_BIND`: reuse+incref an existing cb, else alloc/incref/add and append to the
list. On `FLOW_BLOCK_UNBIND`: lookup (`-ENOENT` if absent), decref, and on last ref remove +
list-del. Mirrors `mtk_eth_setup_tc_block`.

### `xrdp_offload_setup_tc(struct xrdp_offload *o, struct net_device *dev, enum tc_setup_type type, void *type_data)` — `flow_offload.c:671`

The `ndo_setup_tc` entry. Routes both `TC_SETUP_BLOCK` and `TC_SETUP_FT` to
`xrdp_setup_tc_block` (else `-EOPNOTSUPP`). Exported (non-static). Called by the conduit's
`ndo_setup_tc`.

### `xrdp_offload_init(struct xrdp_offload *o)` — `flow_offload.c:683`

`rhashtable_init(&o->flow_table, &xrdp_flow_ht_params)`. Called at conduit probe.

### `xrdp_flow_free_one(void *ptr, void *arg)` — `flow_offload.c:688` (static)

rhashtable free callback: `kfree(ptr)`. Note it does **not** un-program NAT-C (finding F17).

### `xrdp_offload_deinit(struct xrdp_offload *o)` — `flow_offload.c:693`

`rhashtable_free_and_destroy(&o->flow_table, xrdp_flow_free_one, NULL)`.

### `xrdp_offload_selftest(struct xrdp_offload *o, const u8 *mac_da, const u8 *mac_sa, int vlan_op, u16 vid)` — `flow_offload.c:707`

Debugfs L2 self-test. Builds a synthetic `xrdp_flow` (MACs, `ethertype=ETH_P_IP`, both vports =
`default_vport`), maps `vlan_op` (0 plain / 1 push / 2 pop / 3 mangle) to the VLAN fields, runs
`xrdp_build_l2_cmdlist` → `xrdp_build_ctx`, `kzalloc`s an entry with a **synthetic cookie =
(unsigned long)entry**, builds the key, `xrdp_natc_add`, inserts into the rhashtable (rollback +
`kfree` on failure). Returns the cmdlist data length. **Not serialized by `xrdp_flow_mutex`**
(finding F18).

### `xrdp_offload_nat_selftest(struct xrdp_offload *o, __be32 ip_sa, __be32 ip_da, __be16 l4_sport, __be16 l4_dport, u8 ip_proto, __be32 nat_sip, __be16 nat_sport)` — `flow_offload.c:774`

Debugfs Phase-2 routed SNAT+NAPT self-test. Builds a routed `xrdp_flow` (original 5-tuple as
key; `nat_sip`/`nat_sport` as the post-NAT source), runs `xrdp_build_nat_cmdlist` →
`xrdp_build_ctx` → synthetic cookie → `xrdp_build_key` → `xrdp_natc_add` → rhashtable insert
(same rollback pattern). Returns the cmdlist data length. Also not mutex-serialized (F18).
Never sets `ip_tos`/`tcp_pure_ack`, so exercises only the ToS=0 key path.

---

## 5. Audit findings

Severity is my judgement for a driver whose stated goal is real-silicon bring-up. "Contract"
findings are safe for the QEMU proof but block real HW.

### Correctness / behavioral

- **F1 (medium) — UDP checksum uses `apply_icsum_16`, not `apply_icsum_nz_16`.**
  `flow_offload.c:149-151`. Stock (`xrdp-offload-abi.md` §2.3) uses the skip-if-zero variant for
  UDP so a datagram with checksum 0 (checksum disabled, legal in IPv4 UDP) stays 0. Here a plain
  incremental fixup would compute a non-zero checksum over a "no-checksum" field. `nz_16` is not
  implemented in `cmdlist.c`. Undetectable in QEMU because the model does a full recompute.
  *Failure:* a real UDP flow whose endpoints send checksum-0 datagrams gets a bogus L4 checksum
  and is dropped by the receiver.

- **F4 (medium) — L3 key drops `ingress_vport`.** `flow_offload.c:258-261`. Key byte 15 is the
  hardcoded constant `NATC_L3_KEY_TRAILER = 0x68`; the flow's actual ingress vport never enters
  the routed key. Two routed flows with the same 5-tuple arriving on different ingress ports
  collide.

- **F5 (medium) — L3 key drops `ip_proto`; class byte hardcoded to TCP.**
  `flow_offload.c:239,259`. `NATC_L3_KEY_CLASS_BYTE = 0x28` is a live-captured TCP-table
  constant and is written unconditionally; `f->ip_proto` only picks the L4 csum offset, never
  enters the key. A TCP flow and a UDP flow with identical addresses+ports produce the *same*
  16-byte key → NAT-C collision. The code comment (`:232-236`) acknowledges the encoding is
  per-table and unverified.

- **F6 (medium) — NAT cmdlist offsets assume untagged, IHL=5.**
  `flow_offload.h:126-135`, used by `xrdp_build_nat_cmdlist`. A VLAN-tagged routed flow (offsets
  shift by `VLAN_HLEN`) or an IP-options packet (IHL>5) would have TTL/csum/addr/port edits
  applied at the wrong byte offsets, corrupting the frame. Documented for the self-test
  (`offload-phase2-status.md` §6 "IP options: assumes IHL=5") but not guarded in code.

- **F8 (low) — L2 `ethertype` hardcoded to `ETH_P_IP`.** `flow_offload.c:384`; never overwritten
  in the L2 branch (the parser never reads `KEY_BASIC` n_proto for bridge flows). Since ethertype
  is part of the L2 key (`w3` high half), a bridged ARP / IPv6 / PPPoE flow is mis-keyed as
  `0x0800`. Two different-ethertype L2 flows between the same MAC pair collide.

- **F9 (low) — L2 key encodes only a VLAN presence bit, not the VID.** `flow_offload.c:271`:
  `(vlan_in & 0xfff) ? 1 : 0`. Two bridge flows between the same MACs on different VLANs collide.

- **F10 (low) — VLAN_MANGLE ignores PCP.** `flow_offload.c:481-484` stores only `vlan.vid`;
  `xrdp_build_l2_cmdlist` rewrites only VID bits [11:0]. A prio-remark action is silently
  dropped (no error), so the HW forwards with the original PCP.

### Placeholder / unverified-vs-silicon (all acknowledged in comments)

- **F3 (high for real HW) — context length + layout is a flat-byte contract, not the 6813
  bitfield struct.** `flow_offload.h:79-119`, `flow_offload.c:190-201`. Real silicon needs
  `command_list_length_32` in *32-bit-word* units inside WORD 1 of the packed
  `FC_UCAST_FLOW_CONTEXT_ENTRY`, and the real field byte offsets (ABI UNKNOWN #1), re-derived
  from `rdpa.ko _ucast_prepare_rdd_*`. Only `XPE_CTX_CMDLIST_OFF = 24` is pinned. The current
  builder writes raw byte counts at contract offsets 104/105 — a real Runner would not decode it.

- **F19 (high for real HW) — NAT-C key composition / table-id / mask is the open packing, not
  the stock `nat_cache_key0` / `natc_l2_vlan_key` layout** (ABI UNKNOWN #5). No per-table key
  mask is applied here at all (the stock add path does
  `key[i] &= ~rev32(mask[i])`, `xrdp-offload-abi.md` §1.1); masking, if any, must happen in
  `xrdp_natc_add` (conduit driver, out of this file's scope). Stock also splits one 5-tuple into
  multiple HW flows by ToS + TCP-pure-ack; the fields exist (`ip_tos`, `tcp_pure_ack`) but the
  parser never populates them (F11).

- **F20 (contract) — NAT-C indirect-register offsets are placeholders.** `flow_offload.h:56-61`
  (`0x0100/0x0120/0x0200/0x0204`, cmd add=3/del=4). They only have to agree with the QEMU model;
  real `ag_drv_natc_indir_*` registers differ. `NATC_CMD_DEL = 4` is an open invention — stock
  RE only pinned add=3.

- **F13 (medium) — the XPE command-word byte encoding in `cmdlist.c` no longer matches the byte
  streams the QEMU proofs logged.** `offload-phase1-status.md` §3 and `offload-phase2-status.md`
  §2 show cmdlists built with the *old uniform* pack (`opcode<<26|offset8<<18|position<<13|...`),
  e.g. L2 push logged as `4c 30 00 04 60 30 10 00 81 00 ...` and plain-L2 as `fc 00 00 00`.
  Current `cmdlist.c` uses the pinned per-op pack (`byte0=op<<2`, `byte1=(off>>1)+1`), which for
  the same push emits `4c 0c 10 04 60 07 94 04 81 00 00 64` and for plain-L2 emits a **0-byte**
  body (no NOP). Likewise the NAT self-test now emits a 26/28-byte cmdlist, not the logged
  28/28. `cmdlist.h:44-46` flags that the QEMU model "must be updated to decode this real byte
  layout instead of the old uniform split." *Consequence:* the "proven in QEMU" evidence in both
  status notes was produced against an earlier encoding and has not been re-validated against the
  code as it stands — the proofs are stale, not necessarily wrong, but not current.

- **F12 (low) — the routed-flow NAT-C key evidence is also stale.**
  `offload-phase2-status.md` §2 logs key `...1000005006000001` (proto `0x06` in byte 12,
  trailer `0x01`). Current `xrdp_build_key` produces `...10 00 00 50 00 28 80 68` for the same
  self-test (ToS byte 12, class `0x28`, dir `0x80`, trailer `0x68`). The comment at
  `flow_offload.c:232-236` documents the layout change ("earlier the driver packed ip_proto in
  the MSB… the live capture shows the MSB is ToS"). The L2 key evidence is *not* stale — the L2
  builder is unchanged and still produces the logged `ff ff ff ff ff ff 02 00 00 00 00 01 08 00
  00 00`.

### Robustness / minor

- **F7 (medium) — real flows use `default_vport` for both ingress and egress.**
  `flow_offload.c:514`. The `FLOW_ACTION_REDIRECT`/`MIRRED` egress device is parsed but ignored;
  the egress vport is never resolved from the redirect netdev. Every offloaded flow egresses on
  the conduit default vport, so a real multi-port DSA topology would forward to the wrong port.
  Documented as the DSA-integration gap (`offload-phase1-status.md` §6).

- **F14 (info) — misleading log string.** `flow_offload.c:544` prints `(is_l2_accel)`
  unconditionally, including for routed/NAT flows.

- **F15 (low) — HW stats always 0.** `xrdp_natc_stats` stub (`flow_offload.c:587`); the CNPL
  counter read (ABI §3.2) is not wired. `flow_stats_update` reports 0 pkts/bytes, so conntrack
  ageing relies solely on the software flowtable GC.

- **F16 (medium, needs verification) — the block handler rejects non-`CLSACT_INGRESS` binders.**
  `flow_offload.c:637`. `xrdp_offload_setup_tc` routes `TC_SETUP_FT` (the nf_flow_table path,
  which is the whole point of the subsystem) into `xrdp_setup_tc_block`, which returns
  `-EOPNOTSUPP` unless `binder_type == FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS`. If the flowtable
  core presents `FLOW_BLOCK_BINDER_TYPE_FT`, the offload block bind would be refused before any
  flow is programmed. This mirrors `mtk_eth_setup_tc_block`, so it may be correct in practice —
  but per `offload-phase1-status.md` §6 this path was **never exercised** (`CONFIG_NF_FLOW_TABLE`
  unset), so the binder type the kernel actually presents here is unconfirmed. See open
  question O1.

- **F17 (low) — deinit does not un-program NAT-C.** `xrdp_flow_free_one` (`flow_offload.c:688`)
  only `kfree`s the node; it does not call `xrdp_natc_del`. On module unload with live flows,
  stale entries are left in the (modelled/real) NAT-C table. Harmless in QEMU (torn down with
  the model) but leaks HW state on real silicon.

- **F18 (low) — self-tests bypass `xrdp_flow_mutex`.** `xrdp_offload_selftest` /
  `_nat_selftest` (`:707`, `:774`) mutate `o->flow_table` without the mutex the real command path
  holds. Fine for the single-threaded debugfs trigger, racy if ever called concurrently with a
  real `FLOW_CLS_*`.

- **F2 (info) — `u16`→`u8` narrowing of the length fields.** `flow_offload.c:196-197`. Benign
  while `XPE_CMDLIST_MAX_BYTES = 80`, but silently truncates if the cmdlist ever exceeds 255
  bytes.

- **F11 (info) — `ip_tos` / `tcp_pure_ack` are never set by the parser.** The fields feed the L3
  key (`flow_offload.c:251-259`) but `xrdp_parse_flow` reads neither `KEY_IP` (ToS) nor any TCP
  flags, so the stock ToS/pure-ack HW-flow splitting (`fc_tos_mflows`/`fc_tcp_ack_mflows`) is not
  reproduced; every routed flow keys with ToS=0, dir=upstream, pure_ack=0.

---

## 6. Open questions / unknowns

- **O1 — Which `flow_block` binder type does the nf_flow_table path present on this driver?**
  Determines whether the `FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS` gate at `flow_offload.c:637`
  admits or rejects `TC_SETUP_FT` offload. Never exercised live (F16). Resolvable only by
  building a 2-port forwarding topology with `CONFIG_NF_FLOW_TABLE` enabled and observing the
  bind — the explicit TODO in `offload-phase1-status.md` §6 / `offload-phase2-status.md` §6.

- **O2 — Real 6813 `FC_UCAST_FLOW_CONTEXT_ENTRY` byte offsets and bitfield packing** (ABI
  UNKNOWN #1). The current context is a flat contract buffer (F3). Needs RE of `rdpa.ko`
  `_ucast_prepare_rdd_ip_flow_*_result` / `_l2_ucast_prepare_rdd_*` writers, or the 6813
  `rdd_data_structures_auto.h`. Until then the context builder cannot target real silicon.

- **O3 — Real NAT-C key composition, per-table key mask, and table-id ↔ direction/flow-class
  mapping** (ABI UNKNOWN #5). The open key packing (F4/F5/F9/F19) is unverified against the
  stock `nat_cache_key0` / `natc_l2_vlan_key` masks. Needs RE of `rdd_ag_natc_*_mask_*` and
  `data_path_natc_init`, or a read-only live NAT-C dump of an active stock flow.

- **O4 — Real NAT-C indirect-add register interface** (ABI UNKNOWN, `flow_offload.h:42`). The
  `xrdp_natc_add/del` staging offsets are contract placeholders (F20); the true
  `ag_drv_natc_indir_addr/data` sequence, the HW hash/slot selection
  (`drv_natc_key_idx_get`), and whether a delete opcode even exists must be RE'd from
  `rdpa.ko drv_natc_key_result_entry_var_size_ctx_add` and friends.

- **O5 — Exact XPE sub-opcode operand bit-packing on silicon** (ABI UNKNOWN #3, partially
  resolved in `xrdp-offload-abi.md` §2.5). `cmdlist.c` now claims a byte-for-byte-pinned pack,
  but the only live capture was a GDX-local-delivery program, not an L2-accel or routed-NAT
  flow, so the *exact* programs this subsystem emits are still unvalidated byte-for-byte against
  hardware. A read-only capture of a live stock L2/NAT flow's cmdlist would close this.

- **O6 — Has the QEMU Runner model been updated to the pinned per-op encoding?** `cmdlist.h:44`
  says it "must be updated." The status-note evidence (F13) predates the change. Whether the
  model in `qemu/device-model/bcm4916_runner.c` currently decodes the new encoding — and thus
  whether the offload proof still passes as the code stands — was not verifiable from the audited
  files (the QEMU model is outside this subsystem). Needs a re-run of the offload/NAT self-tests
  against the current tree.

- **O7 — Routed egress L2-header rewrite.** A real routed flow must also insert/rewrite the
  next-hop destination MAC on egress (ABI §4.2 "L2 MAC rewrite (routed egress)").
  `xrdp_build_nat_cmdlist` emits no L2-header edit, and the self-test forwards on the same
  backend so it is never needed. Real routing between two ports will require this plus the vport
  resolution of F7/O3.
