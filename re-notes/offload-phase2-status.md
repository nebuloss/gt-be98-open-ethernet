# Offload Phase 2 — L3 routing + NAT/NAPT HW flow-offload: STATUS

Open BCM4916 (BCM6813) XRDP **L3 route + NAT/NAPT hardware flow-offload**,
implemented in the open conduit driver and proven against the extended QEMU
Runner model. This is the line-rate-NAT fast path — the user's actual goal — on
top of Phase 1 (L2 + VLAN, `re-notes/offload-phase1-status.md`).

**Result (proven in QEMU): the first packet of a routed IPv4 flow MISSES NAT-C
and goes to the CPU; the driver programs a NAT-C entry whose `is_routed=1`
context embeds the IPv4-NAT cmdlist; every subsequent packet HITS NAT-C and is
forwarded by the Runner model with the source IP + L4 port REWRITTEN, the TTL
DECREMENTED and the IP + TCP checksums RECOMPUTED — the CPU RX ring untouched.
An independent peer + tcpdump verified the rewritten 5-tuple and both
checksums on the egress pcap.**

---

## 1. What is implemented (driver, `driver/runner/`)

| Piece | File | ABI source |
|---|---|---|
| XPE L3/NAT cmdlist ops: `decrement_8` (TTL), `replace_32` (IP SA/DA), `replace_16` (L4 port), `apply_icsum_16` (IP+L4 csum) | `cmdlist.{c,h}` | xrdp-offload-abi.md §2.3/§2.4a |
| `xrdp_build_nat_cmdlist` — emits the addIpv4Commands sequence in stock order | `flow_offload.c` | §2.4a |
| `is_routed=1` (+ open `is_nat` model hint) context builder | `flow_offload.c::xrdp_build_ctx` | §1.3, live-flow-dump.md §3 |
| IPv4 5-tuple NAT-C key (original/ingress tuple) | `flow_offload.c::xrdp_build_key` | §1.1 (configurable key class) |
| `FLOW_ACTION_MANGLE` parse (IPv4 SA/DA + L4 sport/dport) + 5-tuple dissector parse | `flow_offload.c::xrdp_parse_mangle` / `xrdp_parse_flow` | mtk_ppe_offload.c precedent |
| routed/NAT debugfs self-test (`offload_nat_selftest`) | `bcm4916_runner.c` + `flow_offload.c::xrdp_offload_nat_selftest` | — (test harness) |

Phase 1 L2/VLAN path kept intact; the `-EOPNOTSUPP` on `FLOW_ACTION_MANGLE` and
on IPv4 `addr_type` is removed. The dispatch now branches:
`addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS` → routed L3/NAT; `addr_type == 0` →
L2 bridge (Phase 1). IPv6 NAT is still rejected (as in mtk).

### mtk precedent mapping (the NAT path is modelled on mainline)
- `xrdp_parse_mangle` IPv4 branch ≈ `mtk_flow_mangle_ipv4` —
  `mtk_ppe_offload.c:147-167` (switch on `mangle.offset` =
  `offsetof(struct iphdr, saddr|daddr)`, full-32-bit replace, `mask` ignored).
- `xrdp_parse_mangle` TCP/UDP branch ≈ `mtk_flow_mangle_ports` —
  `mtk_ppe_offload.c:124-145` (the netfilter mangle word packs src in the high
  16 bits, dst in the low 16; `mask == ~htonl(0xffff)` ⇒ dst at offset 0).
- The MANGLE dispatch `switch (act->mangle.htype)` ≈ `mtk_ppe_offload.c:449-462`.
- 5-tuple key read (KEY_BASIC→proto, KEY_IPV4_ADDRS, KEY_PORTS) ≈
  `mtk_ppe_offload.c:318-325 / 420-429 / 407-418`.
- **Direction handling:** like mtk, each `FLOW_CLS_REPLACE` = ONE NAT-C entry =
  ONE direction. The netfilter flowtable core
  (`net/netfilter/nf_flow_table_offload.c`) issues TWO separate REPLACE calls
  (orig + reply), each with its own cookie + pre-baked MANGLE list; the driver
  treats them as two independent unidirectional flows (mtk does the same — there
  is no orig/reply two-entry logic in the driver).

## 2. The NAT cmdlist — bytes produced + decoded (validated vs the opcode encoding)

Self-test flow (`offload_nat_selftest`, a LAN→WAN SNAT masquerade, all addresses
TEST-NET/documentation ranges — public-safe):

```
original 5-tuple : 192.0.2.10:4096 -> 198.51.100.20:80  TCP  TTL 64
SNAT + NAPT      : source rewritten to 203.0.113.5:5000
```

NAT-C key (16-byte masked BE, L3 5-tuple class — original/ingress tuple):
```
key = c0 00 02 0a  c6 33 64 14  10 00 00 50  06 00 00 01
      \__src IP__/  \__dst IP__/  \sport/dport  \proto + L3-class marker (0x..01)
      192.0.2.10    198.51.100.20  4096   80     TCP
```

Embedded cmdlist (28 data bytes; 16-bit BE words; opcode = byte0>>2):
```
68 58 08 00   ADD      off=22 width=8        ; decrement_8: IPv4 TTL  (offset 14+8)
60 68 00 04   REPLACE  off=26 nbytes=4 } data32=0xcb007105  ; SNAT src IP 203.0.113.5 (off 14+12)
cb 00 71 05            (32-bit immediate, BE: high half then low half)
d8 60 10 00   ICSUM    off=24 width=16       ; IP header checksum   (offset 14+10)
60 88 00 02   REPLACE  off=34 nbytes=2 } data16=0x1388 ; NAPT src port 5000 (off 14+20+0)
13 88                  (16-bit immediate, BE)
d8 c8 10 00   ICSUM    off=50 width=16       ; TCP checksum         (offset 14+20+16)
fc 00         NOP                            ; terminator (+pad to 4-byte multiple)
```

Opcodes decode EXACTLY to the RE'd table (xrdp-offload-abi.md §2.2): ADD=0x1a
(decrement_8 = ADD on an 8-bit field; the ADD numeric value is INFER in the ABI
doc and the driver+model agree on 0x1a), REPLACE=0x18, ICSUM=0x36, NOP=0x3f. The
ORDER matches the addIpv4Commands sequence §2.4a: **dec-TTL → IP-replace →
IP-icsum → L4-replace → L4-icsum**. REPLACE is disambiguated by the `nbytes`
operand (4 = replace_32 with two data words, 2 = replace_16 with one, else the
Phase-1 bit-field VLAN replace) — the model decodes the same packing.

Context entry: `is_routed=1` (`CTX_FLAG_IS_ROUTED`, +`CTX_FLAG_IS_NAT` open
model hint), `is_l2_accel=0`, two length fields (`cmd_list_data_length=28`,
`cmd_list_length=28`), cmdlist embedded at +16. Matches the live-confirmed
routed-flow shape (live-flow-dump.md §3: a routed flow sets `is_routed=1`).

## 3. What is modelled (QEMU, `qemu/device-model/bcm4916_runner.c`)

- **L3 5-tuple key compute on RX** (`natc_compute_l3_key`): parses an untagged
  IPv4 TCP/UDP frame and builds the SAME 16-byte key as the driver, so a model
  lookup matches a programmed routed entry. RX tries the L2 key first (Phase 1),
  then the L3 key (Phase 2).
- **cmdlist interpreter extended** (`natc_run_cmdlist`): ADD (TTL −1), REPLACE-32
  (IP SA/DA), REPLACE-16 (L4 port), ICSUM (full ones-complement recompute of the
  IP header csum and the TCP/UDP csum incl. pseudo-header; UDP 0→0xffff rule).
  Phase-1 ops (MCOPY/MOVE/REPLACE-bits/NOP) unchanged.
- **HIT → HW NAT forward (CPU bypass):** on a routed-entry hit, runs the NAT
  cmdlist on a copy of the frame and `qemu_send_packet`s the **rewritten** frame
  out the egress backend WITHOUT writing the CPU RX ring; logs the rewritten
  src/port/TTL + csums. On MISS, the slow path is unchanged.

## 4. QEMU NAT-offload test result — EVIDENCE

Driver built into the kernel (`CONFIG_BCM4916_RUNNER=y`), emulated mode, dgram
netdev peer (`qemu/scripts/nat_offload_peer.py`) injecting real IPv4/TCP frames
with the original 5-tuple, plus `qemu/scripts/init-nat-offload.sh` driving the
debugfs trigger. (dev-build `~/qemu-be98/nat-EVIDENCE.log`, `nat-peer.log`,
`nat-offload-tx.pcap`.)

```
# BEFORE programming: every frame MISSES -> CPU slow path
bcm4916-runner: NAT-C MISS -> CPU slow path, misses=1 .. 7
NATOFF_PHASE=pre_program RXP=0
RXP_AFTER_MISS=0

# driver programs the routed NAT-C entry (REAL Phase-2 builders + NAT-C add)
bcm4916-runner: NAT-C ADD idx=0 key=c000020ac63364141000005006000001 is_l2_accel=0 cmdlist_dlen=28
bcm4916-runner: NAT-C ADD idx=0 cmdlist=[ 68 58 08 00 60 68 00 04 cb 00 71 05 d8 60 10 00 60 88 00 02 13 88 d8 c8 10 00 fc 00 ]
bcm4916-offload: NAT-SELFTEST: programmed NAT-C idx 0 routed SNAT: 192.0.2.10:4096 -> [nat 203.0.113.5:5000] dst 198.51.100.20:80 proto 6, cmdlist 28/28 bytes
NATOFF_PHASE=programmed

# AFTER programming: every frame HITS -> HW NAT forward, CPU bypassed
bcm4916-runner: NAT-C HIT idx=0 (routed/NAT) -> HW forward (offload), len 71->71, hits=1 (CPU bypassed)
bcm4916-runner:   NAT rewrite: src 203.0.113.5:5000 TTL=63  ipcsum=0x14a8 l4csum=0x4cf9
... hits=2 ... 3 ... 4
RXP_AFTER_HIT=0          # conduit CPU rx_packets did NOT advance on HIT frames

# the peer received the rewritten HW-forwarded frames and VERIFIED them:
[nat-peer] RX-from-guest src=203.0.113.5:5000 dst=198.51.100.20:80 ttl=63 ipcsum=OK tcpcsum=OK  NAT-VERIFIED  (x4)
[nat-peer] TOTAL from guest=4  IPv4-forwarded=4  NAT-VERIFIED=4

# tcpdump -nr nat-offload-tx.pcap  (tcpdump only prints "IP" when the IP csum is valid):
IP 203.0.113.5.5000 > 198.51.100.20.80: Flags [.], ... length 17   (x4)
```

**The ordering is the proof:** all pre-program frames MISS→CPU; after the single
routed NAT-C ADD, every subsequent frame HITS→HW-forward with the **source IP
rewritten (192.0.2.10→203.0.113.5), source port rewritten (4096→5000), TTL
decremented (64→63), and the IP + TCP checksums correct**, the CPU RX ring
untouched. This is exactly the flow-miss→learn→program→HW-NAT-fast-path
behaviour of the stock stack (xrdp-offload-abi.md §4.1), reproduced with a fully
open driver — the line-rate NAT path.

### Regression: Phase-1 L2/VLAN path intact
Re-ran the Phase-1 L2 self-test (`offload_selftest`) on the SAME build: plain-L2
cmdlist `fc 00 00 00` (NOP), HIT→HW forward, `RXP_AFTER_HIT=0`, VLAN
push/pop/mangle variants still program. No regression from the shared REPLACE
interpreter / key-builder changes.

## 5. Build status

- Driver: **compiles CLEAN** (zero warnings) cross-aarch64 against mainline
  v7.1, in-tree (`drivers/net/ethernet/broadcom/`, `CONFIG_BCM4916_RUNNER=y`,
  linked into `Image`; new symbols `xrdp_build_nat_cmdlist`,
  `xrdp_offload_nat_selftest`, `xpe_cmd_{decrement_8,replace_32,replace_16,
  apply_icsum_16}` present in the objects). The Kbuild composite is unchanged
  (`bcm4916_runner.o + cmdlist.o + flow_offload.o`).
- QEMU model: builds clean (the two `-Wtype-limits` warnings are pre-existing,
  from `XRDP_OFF_PSRAM==0`/`PSRAM_CPU_RX_RING_DESC==0` comparisons in the
  Stage-3 code, NOT new).

## 6. Honest gaps / TODO

### Proven via the self-test path, NOT yet via live conntrack
The Phase-2 **builders + routed context + NAT-C program + model HIT/NAT/bypass**
are proven end-to-end. The full **nf_flow_table → flow_block → FLOW_CLS_REPLACE
with FLOW_ACTION_MANGLE** trigger is implemented and **compiles + registers**
(the `xrdp_parse_mangle` + routed `xrdp_parse_flow` path), but is exercised here
through a **debugfs self-test** (`offload_nat_selftest`) that calls the same
`xrdp_build_nat_cmdlist` + `xrdp_build_ctx` + `xrdp_build_key` + `xrdp_natc_add`
code `FLOW_CLS_REPLACE` drives — because a true conntrack-NAT'd, forwarded flow
that offers a flow_block needs a 2-port routing/NAT topology
(`MASQUERADE` + `NF_FLOW_TABLE` + ip_forward) the single-NIC emulation does not
host, and the test kernel has `CONFIG_NF_FLOW_TABLE` unset. **TODO:** enable
`NF_FLOW_TABLE` + nftables flowtable offload + a 2-backend QEMU topology and
confirm the flowtable offers `FLOW_CLS_REPLACE` (with the pre-baked NAT MANGLE
actions) to `xrdp_flow_replace` automatically.

### Inherited Phase-1 caveats (UNCHANGED — same contract for the proof)
- **Context byte offsets are a template** (RDP-impl2 + live XRDP fields), NOT the
  pinned 6813 offsets (ABI UNKNOWN #1). Real silicon needs RE of rdpa.ko
  `_ucast_prepare_rdd_ip_flow_*_result` (the routed/NAT result writer).
- **NAT-C key composition / table-id / mask** is the open IPv4 5-tuple packing,
  not the pinned stock `nat_cache_key0` layout (ABI UNKNOWN #5). The stock stack
  also splits one 5-tuple into multiple HW flows by ToS + TCP-pure-ack
  (`fc_tos_mflows`/`fc_tcp_ack_mflows`, live-flow-dump.md §1) — not modelled.
- **NAT-C indirect-register offsets** (PSRAM staging @0x100/0x120/0x200/0x204)
  are CONTRACT placeholders, not the real `ag_drv_natc_indir_*` registers.

### Phase-2-specific gaps
- **XPE operand bit-packing (sub-opcode)** is the open driver's packing, decoded
  identically by the model — the documented opcode field (bits[31:26]) is
  faithful (validated above), but the operand bit layout below bit 26 (and the
  exact ADD opcode value 0x1a) must be re-derived from `xpe_api.o` for real
  silicon (ABI UNKNOWN #3 / INFER).
- **Incremental checksum:** the driver emits `apply_icsum_16` (the stock op); the
  QEMU model implements it as a FULL recompute (correct, simpler) rather than a
  true RFC-1624 incremental delta. Real HW does the incremental delta; the
  on-wire result is identical, so the proof holds.
- **IP options:** the cmdlist assumes IHL=5 (no IP options) — the fast-path
  shape. Stock would compute offsets from IHL.
- **IPv6 NAT/NPT** not implemented (rejected, as in mtk). IPv6 *routing* (no
  addr rewrite, hop-limit dec only) would reuse `decrement_8` + a v6 key.
- **DNAT / dest-port NAPT** are wired in the builder/parser (`nat_dip`/
  `nat_dport`) and the model executes them, but the self-test exercises SNAT +
  source-NAPT (the masquerade case); a DNAT self-test variant is a trivial
  add.
- **CNPL per-flow stats** (`xrdp_natc_stats`) still stub to 0 (ABI §3.2 TODO,
  inherited from Phase 1). Flows age via the flowtable GC timeout.
- **Egress vport / DSA:** the self-test uses the conduit default vport; a full
  DSA route must resolve ingress/egress vports + rewrite the egress L2 MAC
  header (routed flows also need an L2-header insert for the next-hop MAC, ABI
  §4.2 "L2 MAC rewrite (routed egress)") — not yet emitted (the model forwards
  on the same backend, so the L2 rewrite is not exercised here).

## 7. Files

- `driver/runner/cmdlist.{c,h}` — XPE L3/NAT emitter primitives implemented
  (decrement_8 / replace_32 / replace_16 / apply_icsum_16; XPE_OP_ADD added).
- `driver/runner/flow_offload.{c,h}` — `xrdp_build_nat_cmdlist`, routed context +
  L3 5-tuple key, `FLOW_ACTION_MANGLE` + 5-tuple parse, `xrdp_offload_nat_selftest`.
- `driver/runner/bcm4916_runner.{c,h}` — `offload_nat_selftest` debugfs trigger.
- `qemu/device-model/bcm4916_runner.c` — L3 key compute + NAT cmdlist interpreter
  (ADD/REPLACE-32/REPLACE-16/ICSUM) + HIT-NAT-bypass forward (extended).
- `qemu/scripts/nat_offload_peer.py` — IPv4/TCP injector + rewrite/csum verifier.
- `qemu/scripts/init-nat-offload.sh` — NAT-offload test init.
- evidence on dev-build: `~/qemu-be98/nat-EVIDENCE.log`, `nat-peer.log`,
  `nat-offload-tx.pcap`.
