# Offload Phase 1 — L2 + VLAN HW flow-offload: STATUS

Open BCM4916 (BCM6813) XRDP **L2 + VLAN hardware flow-offload**, implemented in
the open conduit driver and proven against the extended QEMU Runner model. This
is the first offload phase on top of the slow-path conduit
(`open-datapath-driver` memory / `qemu/README.md` Stage 3).

**Result (proven in QEMU): the first packet of an L2 flow MISSES NAT-C and goes
to the CPU; the driver programs a NAT-C entry; every subsequent packet HITS
NAT-C and is forwarded by the Runner model WITHOUT touching the CPU RX ring.**

---

## 1. What is implemented (driver, `driver/runner/`)

| Piece | File | ABI source |
|---|---|---|
| Open XPE cmdlist builder (16-bit BE words, opcode=word>>26) | `cmdlist.{c,h}` | xrdp-offload-abi.md §2.1/2.2/2.3, live-flow-dump.md §3 |
| L2 cmdlist ops: VLAN push / pop / mangle + NOP terminator | `cmdlist.c`, `flow_offload.c::xrdp_build_l2_cmdlist` | §2.4b (L2 profile), §4.2 (action→op map) |
| `FC_UCAST_FLOW_CONTEXT_ENTRY` builder (is_l2_accel=1, embeds cmdlist, two length fields) | `flow_offload.c::xrdp_build_ctx` | §1.3 + live-flow-dump.md §3 "CORRECTS" |
| 16-byte masked BE NAT-C key builder (L2 tuple) | `flow_offload.c::xrdp_build_key` | §1.1 |
| NAT-C connection-table add/del/stats (open analog of `drv_natc_key_result_entry_var_size_ctx_add`) | `bcm4916_runner.c::xrdp_natc_*` | §1.1 |
| nf_flow_table / TC_SETUP_FT offload block (flow_block_cb, FLOW_CLS_REPLACE/DESTROY/STATS, dissector + VLAN action parse) | `flow_offload.c` | mtk_ppe_offload.c precedent |
| `.ndo_setup_tc` on the conduit + `NETIF_F_HW_TC` | `bcm4916_runner.c` | mtk_eth_setup_tc |
| debugfs offload self-test trigger (drives the real builders + NAT-C add) | `bcm4916_runner.c` | — (test harness) |

Phase-1 SCOPE: **L2 bridge + VLAN** (`is_l2_accel=1`). L3/NAT is explicitly
rejected (`-EOPNOTSUPP` for IPv4/IPv6 addr_type and for `FLOW_ACTION_MANGLE`).
The cmdlist Phase-2 primitives (`xpe_cmd_decrement_8` / `_replace_32` /
`_apply_icsum_16`) are present as stubs so the `addIpv4Commands` sequence
(xrdp-offload-abi.md §2.4a) slots in without restructuring.

### mtk precedent mapping (architecture modelled on mainline)
- `xrdp_setup_tc_block` / `xrdp_setup_tc_block_cb` ≈ `mtk_eth_setup_tc_block` /
  `_cb` — `mtk_ppe_offload.c:593,613`.
- `xrdp_flow_cmd` (REPLACE/DESTROY/STATS) ≈ `mtk_flow_offload_cmd` —
  `mtk_ppe_offload.c:567`.
- `xrdp_parse_flow` bridge branch ≈ `mtk_flow_offload_replace` addr_type 0
  (`FLOW_DISSECTOR_KEY_ETH_ADDRS` + `KEY_VLAN`) — `mtk_ppe_offload.c:328-349`;
  the `FLOW_ACTION_VLAN_PUSH/POP` loop ≈ `mtk_ppe_offload.c:374-385`.
- `xrdp_flow_stats` → `flow_stats_update` ≈ `mtk_flow_offload_stats` —
  `mtk_ppe_offload.c:540`.

## 2. What is modelled (QEMU, `qemu/device-model/bcm4916_runner.c`)

- **NAT-C connection table**: watches the driver's PSRAM staging writes (16-byte
  key @0x100, context @0x120, index @0x200) and the indirect command register
  (@0x204, cmd=3 add / 4 del), and populates a 64-entry modelled NAT-C table.
- **Key compute on RX**: builds the same 16-byte key from a received frame as
  the driver's `xrdp_build_key` (so model lookup matches a programmed entry).
- **cmdlist interpreter**: decodes the embedded XPE byte-code (MCOPY=insert,
  MOVE=delete, REPLACE bits, NOP) and applies VLAN edits to the frame in place.
- **HIT → HW forward (CPU bypass)**: on a NAT-C hit, runs the cmdlist and
  `qemu_send_packet`s the frame out the egress backend **without writing the CPU
  RX ring**. On MISS, delivers to the CPU ring as before (slow path).
- Debug counters at `NATC base +0x00` (hits) / `+0x08` (misses).

## 3. Cmdlist + context bytes produced (validated against the opcode encoding)

NAT-C key (live-confirmed shape `FHW→RDPA 1:1`, ABI §1.1): for the self-test
flow `DA=ff:ff:ff:ff:ff:ff SA=02:00:00:00:00:01 ethertype=0x0800`:

```
key = ff ff ff ff ff ff 02 00 00 00 00 01 08 00 00 00   (plain L2, vlan_in=0)
key = ... 08 00 00 01                                    (pop/mangle: vlan_in present bit set)
```

Embedded cmdlists (16-bit BE words; **opcode = byte0 >> 2** = bits[31:26]):

| flow | cmdlist bytes | decode |
|---|---|---|
| plain L2 forward | `fc 00 00 00` | `0xfc>>2=0x3f` **NOP** (no edit) |
| VLAN **push** vid100 | `4c 30 00 04  60 30 10 00 81 00  60 38 10 00 00 64  fc 00 00 00` | `0x4c>>2=0x13` **MCOPY/insert** nbytes=4; `0x60>>2=0x18` **REPLACE** data `0x8100` (TPID); **REPLACE** data `0x0064`=VID 100; **NOP** |
| VLAN **pop** | `b0 30 00 04  fc 00 00 00` | `0xb0>>2=0x2c` **MOVE/delete** nbytes=4; **NOP** |
| VLAN **mangle** vid200 | `60 38 0c 00 00 c8  fc 00` | `0x60>>2=0x18` **REPLACE** width=0x0c(12) data `0x00c8`=VID 200; **NOP** |

Opcodes match the RE'd table exactly (REPLACE=0x18, MCOPY=0x13, MOVE=0x2c,
NOP=0x3f — xrdp-offload-abi.md §2.2; 0x18/0x3f also seen live, live-flow-dump.md
§3). Context entry carries `is_l2_accel=1`, the two length fields
(`cmd_list_data_length` = bytes emitted, `cmd_list_length` = 4-byte-aligned),
and the cmdlist embedded at +16.

## 4. QEMU test result — EVIDENCE

Test: bring the conduit up, inject frames matching the self-test key before AND
after programming the NAT-C entry. Driver built into the kernel
(`CONFIG_BCM4916_RUNNER=y`), emulated mode. Two-phase dgram peer injects MISS
frames (pre-program) and HIT frames (post-program) and captures any HW-forwarded
frames bounced back. (dev-build `~/qemu-be98/offload-EVIDENCE.log`,
`offload-peer.log`, `offload-tx.pcap`.)

```
# BEFORE programming: every frame MISSES -> CPU slow path
bcm4916-runner: NAT-C MISS -> CPU slow path, misses=1
... misses=2 ... 3 ... 4 ... 5 ... 6
OFFLOAD_PHASE=pre_program RXP=0
RXP_AFTER_MISS=0

# driver programs the NAT-C entry (REAL builders + NAT-C add MMIO path)
bcm4916-runner: NAT-C ADD idx=0 key=ffffffffffff02000000000108000000 is_l2_accel=1 cmdlist_dlen=4
bcm4916-runner: NAT-C ADD idx=0 cmdlist=[ fc 00 00 00 ]
bcm4916-offload: SELFTEST: programmed NAT-C idx 0 (vlan_op=0 vid=0): cmdlist 4/4 bytes
OFFLOAD_PHASE=programmed

# AFTER programming: every frame HITS -> HW forward, CPU bypassed
bcm4916-runner: NAT-C HIT idx=0 -> HW forward (offload), len 60->60, hits=1 (CPU bypassed)
... hits=2 ... 3 ... 4
RXP_AFTER_HIT=0          # conduit CPU rx_packets did NOT advance on HIT frames

# the peer received the HW-forwarded frames bounced out the egress port:
[peer] TOTAL from guest=4 offloaded-HW-forwarded(HIT echoed back)=4
# tcpdump -r offload-tx.pcap : 4 frames, 0xAB-padded payload (the injected HITs)
```

Counts for the run: `RX_recv=10  MISS=6  HIT=4`. **The ordering is the proof:
all pre-program frames MISS→CPU; after the single NAT-C ADD, all subsequent
frames HIT→HW-forward with the CPU RX ring untouched.** This is exactly the
flow-miss→learn→program→HW-fast-path behaviour of the stock stack
(xrdp-offload-abi.md §4.1), reproduced with a fully open driver.

## 5. Build status

- Driver: **compiles CLEAN** (zero warnings) cross-aarch64 against mainline
  v7.1, both out-of-tree (`bcm4916-runner.ko`, composite of
  `bcm4916_runner.o + cmdlist.o + flow_offload.o`) and in-tree
  (`drivers/net/ethernet/broadcom/`, `CONFIG_BCM4916_RUNNER=y`, linked into
  `Image` — `xrdp_build_l2_cmdlist`/`xrdp_natc_add`/`xrdp_offload_selftest`
  present in `vmlinux`).
- QEMU model: builds clean (the two `-Wtype-limits` warnings are pre-existing,
  from `XRDP_OFF_PSRAM==0` comparisons in the Stage-3 code, not new).

## 6. Honest gaps / TODO

### Proven via the self-test path, NOT yet via live conntrack
The offload **builders + context + NAT-C program + model HIT/bypass** are proven
end-to-end. The full **nf_flow_table → flow_block → FLOW_CLS_REPLACE** trigger is
implemented and **compiles + registers** (the `.ndo_setup_tc(TC_SETUP_FT)` path,
modelled on mtk), but is exercised here through a **debugfs self-test** that
calls the same `xrdp_build_*` + `xrdp_natc_add` code FLOW_CLS_REPLACE drives —
because a true bridge-forwarded conntrack flow that offers a flow_block needs a
2-port forwarding topology the single-NIC emulation does not host, and the test
kernel has `CONFIG_NF_FLOW_TABLE` unset. **TODO:** enable `NF_FLOW_TABLE` +
`bridge` software flowtable, build a 2-backend QEMU topology, and confirm the
flowtable offers FLOW_CLS_REPLACE to `xrdp_flow_replace` automatically.

### Other gaps
- **Context byte offsets are a template** (RDP-impl2 + live XRDP fields), NOT the
  pinned 6813 offsets (ABI UNKNOWN #1). Driver and model agree on the contract,
  so the proof is valid; real silicon needs `_ucast_prepare_rdd_*` RE.
- **NAT-C key composition / table-id / mask** is the open L2 packing, not the
  pinned stock `natc_l2_vlan_key` layout (ABI UNKNOWN #5). Same caveat.
- **NAT-C indirect-register offsets** (PSRAM staging @0x100/0x120/0x200/0x204)
  are CONTRACT placeholders, not the real `ag_drv_natc_indir_*` registers.
- **The XPE sub-opcode operand bit-packing** (below bit 26: offset/position/
  width/nbytes) is the open driver's own packing, decoded identically by the
  model — the documented opcode field (bits[31:26]) is faithful, the operand
  packing must be re-derived from `xpe_api.o` for real silicon (ABI UNKNOWN #3).
- **CNPL per-flow stats** (`xrdp_natc_stats`) stub to 0; flows still age out via
  the flowtable GC timeout. TODO: wire the CNPL counter read (ABI §3.2).
- **Egress vport / DSA**: the self-test uses the conduit default vport; a full
  DSA integration must resolve ingress/egress vports from the flow's netdevs
  (the `FLOW_ACTION_REDIRECT` dev).
- **Phase 2 (L3 + NAT)** not implemented: the cmdlist stubs + the
  `-EOPNOTSUPP` rejections mark exactly where dec-TTL / IP+port replace / icsum
  (xrdp-offload-abi.md §2.4a) and `is_routed=1` slot in next.

## 7. Files

- `driver/runner/cmdlist.{c,h}` — open XPE cmdlist builder (new).
- `driver/runner/flow_offload.{c,h}` — context/key builders + flowtable glue (new).
- `driver/runner/bcm4916_runner.{c,h}` — conduit driver extended: NAT-C I/O,
  `.ndo_setup_tc`, `NETIF_F_HW_TC`, debugfs self-test, offload init/deinit.
- `driver/runner/Kbuild` — composite `bcm4916-runner.ko`.
- `qemu/device-model/bcm4916_runner.c` — NAT-C table + key compute + cmdlist
  interpreter + HIT/bypass forward + debug counters (extended).
- evidence on dev-build: `~/qemu-be98/offload-EVIDENCE.log`,
  `offload-peer.log`, `offload-tx.pcap`.
