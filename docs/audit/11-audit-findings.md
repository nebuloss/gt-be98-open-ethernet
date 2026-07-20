# 11 — Consolidated audit findings

Deduplicated, severity-ranked roll-up of the phase-1 subsystem audits:

- `01-runner-datapath.md` — Runner base driver / open CPU datapath (`driver/runner/bcm4916_runner.{c,h}`)
- `02-flow-offload.md` — XRDP HW flow-accelerator host side (`driver/runner/flow_offload.{c,h}`)
- `03-cmdlist.md` — XPE cmdlist byte-code emitter (`driver/runner/cmdlist.{c,h}`)
- `04-pcs-serdes.md` — XPORT 10G PCS / Merlin16-Shortfin SerDes (`driver/pcs/pcs-bcm-xport.{c,h}`)
- `05-firmware.md` — Runner microcode + Merlin16 SerDes microcode (extractors + driver loaders)
- `06-stock-re-oracle.md` — live stock-watch RE tooling (`tools/stock-watch/*`)
- `07-qemu-model.md` — QEMU device model / offline validation harness (`qemu/device-model/*`)
- `08-hw-abi-regmap.md` — hardware register map & ABI reference (`re-notes/*`)

Finding IDs are carried through from the source audits (`F*` flow-offload, `A-*` stock-oracle,
`H*/M*/L*` pcs-serdes; cmdlist findings are numbered per its §5). Where the same defect surfaced
in more than one subsystem it is stated once here and cross-referenced. The four subsystems added in
this revision carry their own IDs, disambiguated where they would collide: **runner-core** uses
`R-H*/R-M*/R-L*` (its native `H*/M*/L*` clash with pcs-serdes); **firmware** keeps its `§5.x`
numbers; **qemu-model**'s `F1..F14` are renamed `QF1..QF14` to avoid the flow-offload `F*` collision;
**hw-abi** uses `HW-*` from its §8/§9.

---

## Summary

### Counts by severity (post-dedup)

| Severity | flow-offload | cmdlist | pcs-serdes | stock-oracle | runner-core | firmware | qemu-model | hw-abi | Total |
|---|---|---|---|---|---|---|---|---|---|
| **High** | 2 | 3 | 3 | 0 | 3 | 2 | 0 | 2 | **15** |
| **Medium** | 6 | 3 | 4 | 2 | 5 | 5 | 5 | 6 | **36** |
| **Low** | 6 | 3 | 7 | 6 | 6 | 3 | 7 | 4 | **42** |
| **Info** | 3 | 2 | 1 | 3 | 2 | 1 | 2 | 2 | **16** |
| **Total** | 17 | 11 | 15 | 11 | 16 | 11 | 14 | 14 | **109** |

Two themes dominate. First, **unverified ABI**: most subsystems are self-consistent against a QEMU
model they themselves define, with the real-silicon contract (context bitfields, NAT-C key/registers,
XPE operand packing, SerDes/Runner microcode, the RX consumer model) still owed. Second — surfaced
now that runner-core, firmware, qemu and hw-abi are folded in — a cluster of **concrete
correctness/bring-up defects that would bite on first contact with silicon**: a default `insmod` that
hangs the SoC (R-H1), no committed tool that produces a loadable Runner blob (firmware 5.1), a
512-byte TX cap that blocks normal-MTU traffic (R-H2), and poll-mode RX that stops after one pass
(R-H3). The hw-abi reference independently corroborates the fast-path unknowns (FC_UCAST offsets
HW-16, ADD/COPY opcodes HW-18) and adds the still-unresolved RX consumer-model contradiction (HW-4).

### The 5 highest-impact issues

1. **No committed tool produces a loadable Runner microcode blob → the driver cannot bind on real
   hardware.** `tools/extract-runner-microcode.sh:37-47,129`, `bcm4916_runner.c:745`, `:1765-1766`.
   The shipped extractor emits a `B4916UC` container; the driver accepts **only** `RFW1` and
   `-EINVAL`-aborts probe on anything else. The canonical `RFW1` generator (`gen_runner_fw.py`) lives
   only on the build host, so as committed the repo cannot produce the microcode the datapath
   requires — and without the (proprietary, mandatory) Runner microcode no frame moves at all. This
   makes the shipped tree non-functional on silicon by tooling gap alone. (firmware 5.1.)

2. **A plain `insmod` hangs the SoC on live silicon.** `bcm4916_runner.c:127`, `:1142-1145`. The
   default `qm_enable=0x307` is written to `QM_ENABLE_CTRL` on the non-Route-A path with **no**
   preceding `MEM_AUTO_INIT`; enabling `fpm_prefetch` (bit0) on un-initialised QM SRAM makes QM read
   garbage and hangs the SoC (documented, `bcm4916_runner.h:106-108`, `re-notes/realhw/11` §A). The
   emulator masks it (clears instantly). A device-hanging landmine on the *default* code path — and a
   direct violation of the no-connectivity-break rule. (runner-core R-H1.)

3. **XPE cmdlist operand encoding + relocation are the open driver's own invention, never validated
   against silicon.** `cmdlist.c:100`, `cmdlist.c:207`, `re-notes/live-flow-dump.md:289`. Only
   `byte0=opcode<<2` and `byte1=(off>>1)+1` are pinned; every operand below that, and the
   `.text`/`.data` relocation the stock `xpe_cmd_end` performs, is matched only to the driver's own
   QEMU model (`byte2=0x94` is written as a dead pre-relocation `.data` reference real HW would
   mis-interpret). No live L2-accel or routed/NAT cmdlist was ever captured, so the emitted programs
   are unproven byte-for-byte — and the hw-abi reference independently flags the ADD/COPY opcode
   numbers as merely inferred (HW-18). The single largest fast-path (10G-offload) reimplementation
   gap. (cmdlist C1/C2/C3.)

4. **The offload context + NAT-C key ABI is a placeholder contract, not the real 6813 layout.**
   `flow_offload.h:79-119`, `:42-74`; independently confirmed UNKNOWN at
   `re-notes/xrdp-offload-abi.md:126-149` (hw-abi HW-16). `FC_UCAST_FLOW_CONTEXT_ENTRY` is a flat
   driver↔model buffer (only `XPE_CTX_CMDLIST_OFF=24` pinned; real HW needs `command_list_length_32`
   in WORD 1 plus RE-derived field offsets), and the NAT-C key packing, per-table mask and
   indirect-add registers (`0x100/0x120/0x200/0x204`, `NATC_CMD_DEL=4`) are invented. Blocks silicon
   bring-up of every offloaded flow. (flow-offload F3 + F19/F20, corroborated by hw-abi HW-16.)

5. **SerDes PMD microcode load is unimplemented → the 10G ports cannot link, and the tree carries
   two divergent SerDes loaders.** `pcs-bcm-xport.c:167-175` (warn-only stub) vs the full loader
   `bcm4916_runner.c:1578` (firmware 5.2). Without the ~31 KB Merlin16-Shortfin PRAM image no lane
   locks, so `link_status`/`pmd_rx_lock` never assert and `get_state` reports link-down forever; the
   two implementations even disagree on which cores to touch (`serdes_core` param vs hardcoded 0/1).
   Gates the entire 10G line side — the core "10G" deliverable. (pcs-serdes H1 + firmware 5.2.)

---

## Findings

### flow-offload (`driver/runner/flow_offload.{c,h}`)

#### High

- **F3 — context is a flat-byte contract, not the real bitfield struct.**
  `flow_offload.h:79-119` (builder `flow_offload.c:190-201`).
  *Why it matters:* real silicon needs `command_list_length_32` in 32-bit-word units in WORD 1 of
  the packed 124-byte BE `FC_UCAST_FLOW_CONTEXT_ENTRY`, plus RE-derived field byte offsets (ABI
  UNKNOWN #1). Only offset 24 is pinned. The current buffer would not be decoded by a real Runner.
  *Fix/next:* RE `rdpa.ko _ucast_prepare_rdd_*_result` / `_l2_ucast_prepare_rdd_*` (or the 6813
  `rdd_data_structures_auto.h`) and re-lay the struct with a real word-count length encoder. See O2.

- **F19/F20 — NAT-C key composition/mask + indirect-add registers are placeholders; `NATC_CMD_DEL=4`
  is invented.** `flow_offload.h:42-74`.
  *Why it matters:* the key is the open packing, not the stock `nat_cache_key0`/`natc_l2_vlan_key`
  layout; no per-table mask is applied here; the indirect-register offsets and delete opcode are
  driver↔model contracts only (stock RE pinned only `add=3`). Nothing can be programmed to real HW.
  *Fix/next:* RE `rdd_ag_natc_*_mask_*` + `data_path_natc_init` and
  `drv_natc_key_result_entry_var_size_ctx_add`, or take a read-only live NAT-C dump. See O3/O4.

#### Medium

- **F4/F5 — L3 key drops `ip_proto` and `ingress_vport`.** `flow_offload.c:239-261`.
  Class byte hardcoded `0x28` (live TCP constant), byte 15 hardcoded `0x68`. TCP vs UDP with the
  same addrs+ports collide; routed flows from different ingress ports collide.
  *Fix/next:* fold `ip_proto` and the real ingress vport into the key once O3 pins the layout.

- **F1 — UDP L4 checksum uses `apply_icsum_16`, not `apply_icsum_nz_16` (skip-if-zero).**
  `flow_offload.c:148-151`. A UDP datagram with checksum 0 (checksum disabled, legal in IPv4) would
  get a spurious non-zero checksum on real HW and be dropped by the receiver. `nz_16` is not
  implemented in `cmdlist.c`. Masked in QEMU (model does a full recompute).
  *Fix/next:* implement `xpe_cmd_apply_icsum_nz_16` and select it for UDP.

- **F6 — NAT cmdlist packet offsets assume an untagged IHL=5 frame.** `flow_offload.h:126-135`.
  TTL=22 / CSUM=24 / SADDR=26 / ports 34,36 / csums 40,50 are wrong for a VLAN-tagged (offsets shift
  by `VLAN_HLEN`) or IP-options (IHL>5) routed flow, corrupting the frame. Not guarded in code.
  *Fix/next:* parameterise the offset constants on tag/IHL, or reject tagged/optioned routed flows.

- **F7 — egress resolves to `o->default_vport`; the redirect netdev is ignored.**
  `flow_offload.c:508-514`. `FLOW_ACTION_REDIRECT`/`MIRRED` egress device is parsed but discarded,
  so every offloaded flow egresses on the conduit default vport — wrong port in a multi-port DSA
  topology. *Fix/next:* resolve egress vport from the redirect netdev (needs DSA port↔vport map). See O7.

- **F16 — the TC block handler rejects any binder type except `CLSACT_INGRESS`.**
  `flow_offload.c:637`. `xrdp_offload_setup_tc` routes `TC_SETUP_FT` (the nf_flow_table path — the
  whole point of the subsystem) here; if the flowtable core presents `FLOW_BLOCK_BINDER_TYPE_FT` the
  bind is refused before any flow is programmed. Mirrors mtk, but never exercised
  (`CONFIG_NF_FLOW_TABLE` unset) so the real binder type is unconfirmed. *Fix/next:* confirm and
  admit the flowtable binder type. See O1.

- **F13 — the per-op cmdlist byte encoding no longer matches the byte streams logged in the QEMU
  proof notes.** `cmdlist.c:100-319` vs `offload-phase1-status.md §3` / `offload-phase2-status.md §2`.
  The status-note "proven in QEMU" evidence predates the current pinned per-op pack (and the model
  "must be updated to decode the new layout" per `cmdlist.h:44`), so the proofs are stale, not
  necessarily wrong. *Fix/next:* re-run the offload/NAT self-tests against the current tree and
  refresh the notes. See O6, and cmdlist-High and cross-cutting §2.

#### Low

- **F8 — L2 ethertype hardcoded to `ETH_P_IP`.** `flow_offload.c:384`. Parser never reads
  `KEY_BASIC` n_proto for bridge flows; ethertype is in the L2 key, so bridged ARP/IPv6/PPPoE flows
  are mis-keyed as `0x0800` and collide. *Fix:* read n_proto into the L2 key.

- **F9 — L2 key encodes only a VLAN presence bit, not the VID.** `flow_offload.c:271`
  (`(vlan_in&0xfff)?1:0`). Two bridge flows between the same MAC pair on different VLANs collide.
  *Fix:* encode the VID once O3 confirms the L2 key layout.

- **F10 — VLAN_MANGLE drops PCP.** `flow_offload.c:481-484` stores/rewrites only the 12-bit VID; a
  PCP/prio remark is silently dropped (no error), so HW forwards with the original PCP.
  *Fix:* carry PCP and emit the PCP bits in the mangle.

- **F17 — deinit leaks programmed NAT-C entries.** `flow_offload.c:688-691`. `xrdp_flow_free_one`
  only `kfree()`s the rhashtable node; it does not call `xrdp_natc_del`, so live flows leave stale
  entries in the (real) NAT-C table at module unload. Harmless in QEMU. *Fix:* un-program NAT-C in
  the free callback. See cross-cutting §6.

- **F15 — HW stats always 0.** `flow_offload.c:587`. `xrdp_natc_stats` is a stub (CNPL counter read
  not wired), so `flow_stats_update` always reports 0 pkts/bytes and conntrack ageing relies solely
  on software flowtable GC. *Fix:* wire the CNPL per-flow counter read. (Also surfaces as runner-core
  R-L4.)

- **F12 — routed-flow NAT-C key evidence is stale.** `flow_offload.c:232-236`. The proof note's
  proto-in-byte12/trailer-`0x01` key predates the current ToS-byte12/class-`0x28`/trailer-`0x68`
  layout. L2 key evidence is still valid (L2 builder unchanged). *Fix:* documentation/proof refresh
  (see cross-cutting §2).

#### Info

- **F14 — misleading log string.** `flow_offload.c:544` hardcodes `(is_l2_accel)` in the success
  `pr_info` even for routed/NAT flows. Log-only.

- **F2 — `u16`→`u8` narrowing of the length fields.** `flow_offload.c:196-197`. Benign while
  `XPE_CMDLIST_MAX_BYTES=80`, silently truncates if a cmdlist ever exceeds 255 bytes. Facet of F3.

- **F18/F11 — self-tests bypass `xrdp_flow_mutex`; parser never sets `ip_tos`/`tcp_pure_ack`.**
  `flow_offload.c:707-836`. `xrdp_offload_selftest`/`_nat_selftest` mutate `o->flow_table` without
  the mutex the real `FLOW_CLS_*` path holds (safe single-threaded, racy if concurrent). The parser
  never populates `ip_tos`/`tcp_pure_ack`, so stock ToS/pure-ack HW-flow splitting is not reproduced.

---

### cmdlist (`driver/runner/cmdlist.{c,h}`)

#### High

- **C1 — no `.text`/`.data` relocation; `byte2=0x94` references are dead.** `cmdlist.c:207` (writes
  at `:138,267,276,289,293`). Every offset-addressed emitter writes `byte2=0x94` as a
  pre-relocation `.data` "from" reference that `xpe_cmd_end` never fixes up (it only records length
  and pads with `0xfc`). The open driver inlines operands after each command word instead, so buffers
  are self-consistent with the QEMU model only; on real silicon `0x94` would be read as a `.data`
  offset and the program would not run. *(Top-5 #3.)* *Fix/next:* RE the stock `xpe_cmd_end`
  relocation/concatenation math. See cmdlist-Q3.

- **C2 — operand bit-packing below the opcode field is the driver's own invention.** `cmdlist.c:100`.
  Only `byte0=opcode<<2` and `byte1=(off>>1)+1` are pinned; byte2/byte3 semantics, the
  inline-vs-relocated data split, `replace_32`/`replace_16` disambiguation by `byte3==4`, and the
  `replace_bits` pos/width nibble pack are all ABI UNKNOWN #3, matched only to the model.
  *Fix/next:* disassemble `xpe_api.o`/`cmdlist.ko` and correlate with matched-4916 GPL microcode
  arrays. See cmdlist-Q1, and hw-abi HW-18.

- **C3 — no live L2-accel or routed/NAT cmdlist was ever captured.** `re-notes/live-flow-dump.md:289`
  (`cmdlist.c:55-60`). The only live body is a GDX local-delivery program this driver does not emit;
  the word encoding+framing are validated but the driver's actual VLAN and NAT programs are **not**
  byte-for-byte validated against stock. *Fix/next:* capture a live routed/NAT and L2-accel flow's
  cmdlist without disturbing the management link. See cmdlist-Q4, and stock-oracle A-8. *(This absorbs
  flow-offload F13, stock-oracle A-8, and hw-abi HW-18 as the canonical "emitted programs unvalidated"
  high.)*

#### Medium

- **C4 — `decrement_8` ignores offset parity.** `cmdlist.c:250` hardcodes `byte0=0x6a` though the
  in-source note (`:246-247`) says odd offsets must set `byte0` bits[25:24]. Correct only for even
  offsets; sole caller uses even `IP4_OFF_TTL=22` so currently safe, wrong for odd-offset use.
  *Fix:* implement the odd-offset encoding or assert evenness. See cmdlist-Q7.

- **C5 — `xpe_to_idx` silently discards the low bit of an odd offset.** `cmdlist.c:115`
  (`(offset>>1)+1`). An odd offset targets the even byte below with no error/assertion. All current
  callers pass even offsets, but future/tagged-frame odd offsets would misfire silently.
  *Fix:* `WARN`/reject odd offsets. See cross-cutting §5.

- **C6 — `replace_bits_16` pos/width nibble pack overflows silently.** `cmdlist.c:139`. `position`
  and `(width-1)+position` are packed into 4-bit nibbles each masked `&0xf`; if
  `(width-1)+position > 15` the high nibble wraps and decoded width is wrong, and `width==0` makes
  `width-1` underflow to `0xff`. Only caller uses `position=0,width=12` (safe). *Fix:* guard width/pos.

#### Low

- **C7 — duplicate opcode macro value.** `cmdlist.h:53`. `XPE_OP_ICSUM` and `XPE_OP_GDMA` are both
  `0x1c`; `XPE_OP_MOVE=0x2c` is defined but never referenced. No functional bug (emitters use raw
  byte0) but a latent trap for any macro-based decoder. *Fix:* disambiguate or delete.

- **C8 — `XPE_OP_*` macros are dead in the emit path.** `cmdlist.h:47`. Words are assembled from
  hardcoded hex byte0 values, not the macros (which appear only in comments). Risks drift between the
  two encodings. *Fix:* wire emitters to the macros or clearly mark them decode-only.

- **C9 — context length fields stored as raw bytes, not the `command_list_length_32` bitfield.**
  `flow_offload.c:196`. The model reads `dlen` as one `uint8_t`; real silicon carries
  `round_up(clen,4)/4` in a WORD-1 bitfield of the 124-byte BE struct. Numerically safe today (max
  body 80 fits a byte). **Same root as flow-offload F3/F2** — a real-HW length encoder is owed.

- **C10 — `xpe_emit16` can leave a partially-written word in `buf` on overflow.** `cmdlist.c:79`.
  The write is dropped and `overflow` set, but a command word/immediate that overflows on its second
  half leaves `buf`/`len` mid-word. The three callers do check `cl.overflow` and return `-E2BIG`
  (`flow_offload.c:523,732,808`), so a truncated buffer is never programmed — brittle if the flag is
  ever unchecked. *Fix:* roll back `len` on overflow.

#### Info

- **C11 — `apply_icsum_16` emits a zero immediate.** `cmdlist.c:304`. Relies on the Runner
  recomputing the checksum delta; the stock emitter's low half carries a precomputed 16-bit csum
  (`bfxil`). If real silicon applies the inline immediate rather than recomputing, a zero would
  corrupt the checksum. Model recomputes so the pair is self-consistent; silicon behavior unverified.
  See cmdlist-Q2.

- **C12 — `XPE_CMDLIST_MAX_BYTES=80` is smaller than the real slot.** `cmdlist.h:70`. GPL
  `FC_UCAST` reserves `command_list[100]`; safe for short L2/NAT programs but truncates a longer
  tunnel/encap program (`xrdp_build_ctx` also clamps `clen` to 80 before memcpy).

---

### pcs-serdes (`driver/pcs/pcs-bcm-xport.{c,h}`)

#### High

- **H1 — SerDes PMD microcode load is unimplemented.** `pcs-bcm-xport.c:167-175`. Without the
  ~31 KB Merlin16-Shortfin PRAM image no lane locks on real HW, so `get_state` reports link-down
  forever; only the QEMU-faked path works. The stub returns 0 even on the not-implemented path, so
  callers cannot detect the missing blob. *(Top-5 #5; see firmware 5.2 for the divergent second
  loader.)* *Fix/next:* host the PRAM load mechanism (user-supplied non-redistributable blob) and
  return a real error when absent. See pcs-O1, cross-cutting §3/§11.

- **H2 — no PLL-lock / settling waits in the reset-release sequences.**
  `pcs-bcm-xport.c:131-141` and `:149-162`. `mpcs_reg_init` and `serdes_core_init` issue back-to-back
  `writel`s with no `udelay` and never poll `SS_PLL_LOCK`/`pmd_rx_lock` after release; the SDK oracle
  polls for lock. Deasserting core reset before PLL lock can leave the PMD indeterminate on silicon.
  *Fix/next:* add the SDK settling delays + lock polls. See pcs-O2.

- **H3 — `serdes_core_init` omits `PMD_setup_..._VCO` and `datapath_reset_core`.**
  `pcs-bcm-xport.c:149-162`. The SDK runs VCO/PLL setup and a datapath reset after reset release; the
  open driver does neither, so even with microcode loaded the PMD may not reach the operating rate.
  *Fix/next:* port the VCO setup + datapath reset. See pcs-O2.

#### Medium

- **M1 — USXGMII / 2500BASEX per-mode programming is stubbed.** `pcs-bcm-xport.c:191-212`.
  `pcs_config` accepts these modes but writes no `an_config` registers; only 10GBASE-R actually
  functions. `neg_mode`/`advertising`/`permit_pause_to_mac` are ignored. *Fix/next:* program the
  per-mode speed/AN registers. See pcs-O3.

- **M2 — `an_complete` conflated with `link`; AN status never read.** `pcs-bcm-xport.c:232`.
  `state->an_complete = link` and speed/pause come from fixed defaults; `SERDES_AN_STATUS` is defined
  but never read and there is no `.pcs_an_restart` op — wrong for the in-band-AN modes. *Fix:* derive
  `an_complete` from AN status and add `.pcs_an_restart`.

- **M3 — pause is hardcoded.** `pcs-bcm-xport.c:247`. `state->pause |= MLO_PAUSE_TX|MLO_PAUSE_RX`
  regardless of negotiation or `permit_pause_to_mac`. Flow control is always advertised symmetric.
  *Fix:* read pause from AN result.

- **M4 — `get_state` reads core 0 only; lane→port mux never programmed.** `pcs-bcm-xport.c:219`
  (`core=0` hardcoded; `SC_LN_OFFSET_SHIFT` unused). port7/eth3 (core 1 per the port map) is never
  reported from its own core; only the single-lane core-0 10GBASE-R path is real. *Fix:* select the
  per-port core and program the lane mux. See pcs-O5.

#### Low

- **L1 — register bases/offsets unverified against live silicon.** RE'd from the SDK 6813A0 arrays +
  QEMU model, never confirmed against a live regdump (read-only device policy). See pcs-O4.
- **L2 — many bit macros defined but never used** (`SERDES_INDIR_ACC_*`, `SERDES_AN_STATUS`,
  `SC_REFSEL/PRTAD/LN_OFFSET_SHIFT`, `SS_CDR_LOCK/PLL_LOCK/RX_SIGDET`, `MPCS_SIGNAL_DETECT/TX_CLK_VLD`,
  `SS1_10M/100M_SHIFT`) — documents how much of the block is not yet driven.
- **L3 — two independent copies of the supported-interface list.** `pcs-bcm-xport.c:271-275` array vs
  the `switch` in `pcs_config`; must be kept in sync by hand. *Fix:* single source of truth.
- **L4 — `emulated` flag under-used** (`:103`, read only at `:169`); correctness on QEMU depends
  entirely on the model matching the read paths.
- **L5 — `load_firmware` return value ignored** (`:186-187`); a real load that failed would still
  report bring-up success. See cross-cutting §4.
- **L6 — no `.pcs_disable` / reset re-assert;** cores are never returned to IDDQ/reset on link-down
  or unbind. See cross-cutting §6.
- **L7 — no `core` bounds check in `serdes_rd/serdes_wr`** (`:108-116`); safe today (callers pass
  0/1), out-of-window pointer for `core≥3`.

#### Info

- **L8 — doc/demo inconsistency.** The port map comment (`:118-123`) assigns eth3→core 1 but the
  QEMU boot shows `eth3` as 1G rgmii; the eth-name↔port↔core identity is not yet pinned on hardware.
  See pcs-O5.

---

### stock-oracle (`tools/stock-watch/*`)

#### Medium

- **A-1 — latent key-buffer overflow in `natc_dump`.** `natc_dump.c:141` vs `:174`. The `key` buffer
  is a fixed 16 B, but `drv_natc_key_entry_get` memcpy's `key_len` bytes from the stock per-table
  config *before* any valid check; the default `tables=0xf` scans all four tables and only table 0's
  16-byte stride is confirmed. If tables 1..3 have a longer key stride the stock memcpy overruns the
  kzalloc. *Fix:* default to scanning only table 0, or size the key buffer like the 256-B result buffer.

- **A-4 — safety-model contradiction in `route-a-oracle.sh`.** Phase-R records that a raw MMIO read
  of XRDP/Runner FPM space hung the SoC unrecoverably and concluded "never raw `ioremap`+`readl`
  XRDP/Runner space", yet the script does exactly that against QM/BBH_TX/TM RNR_MEM. Those reads
  empirically succeeded, but the tool trusts the human to know which sub-blocks are safe, and one
  wrong `phys=` already forced an automatic watchdog recovery. *Fix/next:* encode an allowlist of known
  side-effect-free registers; see stock-Q5.

#### Low

- **A-2 — `p_res_get` return discarded.** `natc_dump.c:184`. On a failed result getter, `res` stays
  zeroed and a zero CTX is dumped as if valid — could be misread as real data.
- **A-3 — redundant/contradictory kallsyms fallback.** `natc_dump.c:126-128`, `xrdp_peek.c:162-164`.
  The self-lookup is redundant on 4.19 (symbol still exported) and the direct-reference fallback
  cannot rescue the case its comment describes. Harmless; comment overstates portability.
- **A-5 — MMIO endianness left to the reader.** `xrdp_peek.c:153`. `readl` returns host-LE while
  Runner regs are big-endian in DDR; the tool dumps raw words with no swap, so every hand-mapping
  must byte-swap correctly — a foot-gun for the manual step.
- **A-6 — dead `nregs` field.** `rdpa_trace.c:73` vs `:203-207`. `trace_pre` always prints x0..x4
  regardless of `nregs`; probes needing x5..x7 in a deref would not get them printed.
- **A-10 — `route_a_queue` not fully resolved.** `re-notes/realhw/11` LIVE ORACLE RESULTS. The
  oracle pinned `route_a_bbh_inst=1` and the QM enable/auto-init values but not the logical→physical
  queue map; only candidates A and B are offered, to be tried empirically. See stock-Q1. **Same open
  item as runner-Q2 (queue map) and hw-abi HW-12.**
- **A-11 — `dmesg -c` clears the ring each iteration.** `route-a-oracle.sh:25`. Discards the entire
  kernel ring buffer before every `insmod`, destroying any unrelated messages an operator might need
  mid-run. *Fix:* grep without clearing, or snapshot first.

#### Info

- **A-7 — unverified/cross-tree probe offsets.** `rdpa_trace.c:163-192`
  (`drv_natc_key_result_entry_var_size_ctx_add` arg count, `rdd_connection_entry_add` layout). Honestly
  labelled placeholders, not silent guesses, but not yet silicon-validated. See stock-Q4.
- **A-8 — GRP_OFFLOAD path never captured live.** `re-notes/offload-live-validation.md §5`. The idle
  lab device had no routed/NAT or bridge-accel flow, so the NAT/VLAN operand math is READY but
  unverified. **Same gap as cmdlist-C3 / flow-offload F13 / hw-abi HW-18** — see cross-cutting §2.
- **A-9 — tracefs fallback non-functional today.** `rdpa-trace-events.sh:10-14`. The current kprobe
  kernel has `CONFIG_KPROBES=y` but `CONFIG_FTRACE` OFF, so `KPROBE_EVENTS`/tracefs is unavailable;
  committed as a forward-looking fallback only.
- **A-12 — stale index-range comment.** `natc_dump.c:80`. Comment says "max ~17408 entries" but live
  `hw_id`s range to 0xef58 (61272); the comment should not be trusted as the index bound.

---

### runner-core (`driver/runner/bcm4916_runner.{c,h}`)

The open CPU-conduit slow path / DSA conduit netdev. Findings from `01-runner-datapath.md §5`.

#### High

- **R-H1 — default (non-Route-A) QM enable writes `0x307` with no SRAM auto-init → documented
  SoC-hang risk on live silicon.** `bcm4916_runner.c:127` (default `qm_enable=QM_ENABLE_CTRL_STOCK`),
  write at `:1142-1145`; `MEM_AUTO_INIT` runs only inside the route-A-gated `runner_qm_init`.
  *Why it matters:* enabling `fpm_prefetch` (bit0 of 0x307) on un-initialised QM SRAM makes QM read
  garbage and hangs the SoC (`bcm4916_runner.h:106-108`, `re-notes/realhw/11` §A). A plain `insmod`
  (no `route_a`, default `qm_enable`) on real hardware risks the hang the emulator masks. *(Top-5 #2.)*
  *Fix/next:* gate the `0x307` write behind `MEM_AUTO_INIT`, or default `qm_enable=0` unless
  `route_a=1`. See cross-cutting §9, runner-Q4.

- **R-H2 — TX is capped at ~512-byte frames; standard-MTU traffic is impossible.**
  `FPM_CHUNK_SIZE_DEFAULT=512` (`bcm4916_runner.h:416`); the whole skb is copied into one chunk and
  `runner_start_xmit` drops `len>chunk_size` (`bcm4916_runner.c:569-573`); `max_mtu = chunk_size -
  ETH_HLEN = 498` (`:1799`).
  *Why it matters:* no multi-chunk / SBPM buffer chaining, so the conduit cannot transmit a normal
  1500-B frame (RX_BUF_SIZE=2048, so RX is asymmetric and can receive larger). The v1 slow path
  cannot carry real traffic. *Fix/next:* implement multi-chunk / SBPM TX buffer chaining. See
  cross-cutting §10, qemu-Q7.

- **R-H3 — poll-mode RX (the pdev-shim takeover path) is not self-sustaining.**
  `runner_ndo_open` schedules NAPI exactly once when `rx_irq<=0` (`bcm4916_runner.c:1473-1474`);
  `runner_rx_poll` re-arms only via `enable_irq`, and only when `rx_irq>0` (`:511-515`); the
  `bcm4916_runner_pdev.c` shim deliberately omits the IRQ.
  *Why it matters:* the actual on-silicon takeover path services at most one NAPI pass of RX and then
  stops. *Fix/next:* add a periodic timer/hrtimer poll, or wire the IRQ. See cross-cutting §10.

#### Medium

- **R-M1 — TX ring can be overrun; the stop/wake handshake is broken.** `bcm4916_runner.c:617`
  advances `tx_write_idx` (mod 256) without checking the Runner's TX `read_idx`; `:576-580` calls
  `netif_stop_queue`+`NETDEV_TX_BUSY` on FPM-token exhaustion but **nothing ever calls
  `netif_wake_queue`**. *Why:* 256 ring slots vs ~65536 tokens → a stalled Runner wraps and clobbers
  undelivered descriptors long before tokens run out; after one exhaustion the queue can stay stopped
  permanently. *Fix:* backpressure on TX `read_idx`; wake the queue on completion. See cross-cutting §4.

- **R-M2 — Route A QM init omits queue-context / WRED / DQM-enable and the FIFO configs.**
  `runner_qm_init` `bcm4916_runner.c:1053-1093` programs only MEM_AUTO_INIT, FPM_BASE/DDR_SOP, one
  RUNNER_GRP QUEUE/RNR config, and ENABLE. `re-notes/realhw/11` §A/§C also require `FPM_CONTROL`,
  `FPM_COHERENT_BASE_ADDR`, `FPM_USR_GRP_*_THR`, per-queue `qm_q_context_set` with a **pass** WRED
  profile (default 15 = drop-all), `dqm_dqmol_cfgb_set(Q,1)`, and `RUNNER_GRP_{PDFIFO,UPDATE_FIFO}
  _CONFIG` (`0x308/0x30c`). As written the target queue defaults to drop-all/disabled, so Route A
  likely passes no frames. *Fix:* program the missing QM sub-steps. See hw-abi HW-13, runner-Q5.

- **R-M3 — Route A default queue/grp values overwrite live stock QM state.** `bcm4916_runner.c:1078-1084`.
  All `route_a_*` params default 0, so `route_a=1` with defaults programs `RUNNER_GRP[0]` range
  `[0,0]` bb_id0/task0, clobbering the stock grp0 mapping the oracle captured (queues 80-111 →
  core7/task3). *Fix:* refuse `route_a` with unset params, or guard against clobbering stock grp0.

- **R-M4 — `RUNNER_FIRST_PORT` / bbh_id mapping is an unverified guess.** `bcm4916_runner.c:114`,
  used simultaneously as UNIMAC instance, BBH_ID and dispatcher VIQ. `re-notes/realhw/10` Wave-9 says
  the same 1G port (`port_gphy1`) is bbh_id 0 / VIQ 0 while UNIMAC inst = 1 — not the identity. If
  bbh_id is 0, the dispatcher VIQ wiring (`bb_id=31+2*1=33`) and `runner_bbh_init(rx_port=1)` target
  the wrong port. *Fix:* pin the emac→bbh_id map by live oracle. See runner-Q1.

- **R-M5 — Route A BBH_TX binding uses `route_a_bbh_inst`, but the plain BBH_TX setup is hardwired to
  instance 0.** `runner_bbh_init` always programs BBH_TX inst 0 (`bcm4916_runner.c:1167`) while
  `runner_bbh_tx_route_a` programs inst `route_a_bbh_inst` (=1). So MACTYPE/BBCFG_1 land on inst 0 but
  the QM-fed binding on inst 1; inst 1's MACTYPE/BBCFG_1 are never written by the open driver.
  *Fix:* program the base BBH_TX setup on the same instance Route A binds.

#### Low

- **R-L1 — `XRDP_OFF_QM` `#define`d twice** (`bcm4916_runner.h:72` and `:81`, both `0x00c00000`);
  `XRDP_OFF_DQM=0x00c80034` (`:73`) is unused and is a base+`0x34` register, not the block base.
  *Fix:* de-dup. (See also hw-abi HW-RESOLVED item 3, the DQM base-vs-accessor clarification.)
- **R-L2 — EGPHY control/status offset carries an unresolved +4 ambiguity vs the RE note.** Header
  uses `QEGPHY_CTRL 0x837ff014 / STATUS 0x837ff018 / TEST_CNTRL 0x837ff010` (`bcm4916_runner.h:289-308`)
  vs `re-notes/realhw/10` Wave-9 `0x837ff010 / 014 / 00c` — a consistent +4 shift; a wrong choice
  lands PHY power-up writes on the wrong register. **Same open item as hw-abi HW-20.** See runner-Q6.
- **R-L3 — `fpm_alloc_token`'s `size` arg is ignored** (`bcm4916_runner.c:238-245`); harmless (fixed
  512-B chunks) but misleading.
- **R-L4 — `xrdp_natc_stats` always returns 0/0** (`bcm4916_runner.c:697-706`). **Same defect as
  flow-offload F15** — CNPL per-flow counter read-back unwired; offloaded flows age out by the
  flowtable timeout.
- **R-L5 — NAT-C staging offsets are contract placeholders, not silicon-verified.** `flow_offload.h:56-62`,
  `bcm4916_runner.c:655-659`. **Same root as flow-offload F19/F20 and qemu QF7** — see cross-cutting §1.
- **R-L6 — several polled bring-up steps swallow timeouts as non-fatal** (FPM `readl_poll_drain`,
  SBPM RDY, DSPTCHR RDY, QM MEM_INIT_DONE): `dev_warn`+continue (`bcm4916_runner.c:270-279, 946-954,
  1068-1069, 1151-1159`). A production driver should fail probe when a mandatory block never signals
  ready — especially QM MEM_INIT_DONE, given R-H1. See cross-cutting §4.

#### Info

- **R-I1 — stale RX-ownership comment.** The header's word2/word3 ownership comment
  (`bcm4916_runner.h:496-504`) is unused by the polling code (feed-ring `write_idx`/`read_idx` model,
  Wave-4/5 supersedes the earlier word2 note); a stale comment, not a code bug. Tied to the
  RX-consumer-model conflict (hw-abi HW-4).
- **R-I2 — the TX `word2`/`word3` FPM-token layout has contradictory in-tree provenance.**
  `re-notes/realhw/10` flags `TXD_W3_FPM_BN0/SOP` "wrong/unverified" (line 95) while the later Wave-7
  (`:232-235`) and the header (`bcm4916_runner.h:585-588`) mark them verified; worth an on-silicon
  confirmation before trusting FPM-mode TX. **Same as hw-abi HW-7.**

---

### firmware (`tools/extract-*`, driver firmware loaders)

The two proprietary blobs (Runner microcode + Merlin16 SerDes microcode) and every extractor/loader
in-tree. Findings from `05-firmware.md §5` (numbers preserved).

#### High

- **5.1 — the committed extractor emits a `B4916UC` container the driver rejects; no committed tool
  produces a loadable `RFW1` Runner blob.** `tools/extract-runner-microcode.sh:37-47,129` vs
  `bcm4916_runner.c:745`; the `-EINVAL` aborts probe (`:1765-1766`).
  *Why it matters:* a blob from the shipped extractor fails the `"RFW1"` magic/table check and the
  driver fails to bind on real hardware; the canonical `RFW1` generator (`gen_runner_fw.py`) lives
  only on the build host (`re-notes/realhw/10-runner-bringup-spec.md:174`), so the repo cannot produce
  a working Runner blob as committed — and the Runner microcode is mandatory for *any* packet
  movement. *(Top-5 #1.)* *Fix/next:* update the extractor to emit `RFW1` (add the 128-B per-core
  table, drop `B4916UC`), or commit `gen_runner_fw.py`. See firmware-Q1.

- **5.2 — two divergent SerDes-firmware implementations for the same Merlin16 block.**
  `runner_serdes_load()` is a complete streaming PRAM loader (`bcm4916_runner.c:1578`) while
  `bcm_xport_pcs_load_firmware()` is a warn-only TODO that never loads anything (`pcs-bcm-xport.c:167`);
  they also disagree on which cores to touch (runner uses the `serdes_core` module param; PCS
  hardcodes cores 0 and 1, `:186-187`).
  *Why it matters:* only one can be the real bring-up path on silicon; today they contradict and
  neither is silicon-proven. **Directly reinforces pcs-serdes H1.** *(Top-5 #5.)* *Fix/next:* choose
  one authoritative load path. See firmware-Q6, cross-cutting §3/§11.

#### Medium

- **5.3 — `runner_serdes_load()` return value is ignored.** `bcm4916_runner.c:1788-1789`. A
  `request_firmware` failure, µC-init timeout, or `uc_active==0` (`-EIO`) never reaches probe, which
  reports success though the 10G line side never came up. *Fix:* propagate/log the failure. See
  cross-cutting §4.

- **5.4 — SerDes CRC 0x4949 is never verified by the driver.** `bcm4916_runner.c:1598-1600` checks
  **only** the size and streams whatever bytes it got, so a truncated/corrupt-but-right-sized image
  loads and is caught (if at all) only by the `uc_active` poll — even though the extractor computes
  the CRC and the header/TODO comments reference it. *Fix:* add the CRC-16/CCITT check before
  streaming. See cross-cutting §4.

- **5.6 — `RA_INITDONE` mask encodes an unresolved RE ambiguity.** `SRD_MICRO_RA_INITDONE=0x8001`
  "tolerate bit0|bit15" (`bcm4916_runner.h:359-360`); the poll (`bcm4916_runner.c:1564`) succeeds on
  either bit, so a spurious set of the wrong bit passes a RAM-init that did not complete. *Fix:* pin
  the real "done" bit on silicon. See firmware-Q7.

- **5.9 — SerDes poll timeouts + reset-recipe are hardcoded / guessed pending silicon.** The busy
  spin (1000 µs), `serdes_poll_ra_initdone` (250 µs) and the `uc_active` poll (10000 µs) are magic
  loop counts (`bcm4916_runner.c:1536,1569,1654`), and the µC reset-defaults recipe (`:1582-1611`) is
  a transcribed opaque SDK `uc_reset` sequence; none is validated against real Merlin timing. *Fix:*
  validate on silicon. See firmware-Q7, pcs-O2.

- **5.10 — the driver comment claims a GPL origin for the Runner microcode the RE verdict
  contradicts.** `bcm4916_runner.c:732-733` (and `10-runner-bringup-spec.md:178` "GPLv2 →
  MODULE_FIRMWARE ok") call the blob "built from the SDK's GPL per-core microcode arrays",
  contradicting `runner-microcode-and-cpuring.md` §A / `gpl-source-inventory.md`: the 4916 Runner
  microcode is absent from the GPL SDK and exists only inside the Proprietary `rdpa.ko`. *Why:* a
  misleading licensing story in shipped code. *Fix:* reconcile the comment/spec to the proprietary
  verdict. See cross-cutting §3.

#### Low

- **5.5 — no `MODULE_FIRMWARE` for the SerDes blob.** `bcm4916_runner.c:1899` declares it only for
  the Runner blob, so `modinfo`/initramfs tooling won't know to bundle `brcm/merlin16-shortfin.bin`.
- **5.7 — `sym_offsize()` ELF parse is fragile across binutils versions.**
  `tools/extract-runner-microcode.sh:110-116` uses positional `awk` heuristics for `readelf -SW`
  output; a mis-parse yields a wrong file offset and a silently corrupt extraction. *Fix:* use a
  structured reader (pyelftools / `objcopy --dump-section`).
- **5.8 — `extract-serdes-fw.py` writes the output even on size/CRC mismatch.**
  `tools/extract-serdes-fw.py:66-76` warns but writes the `.bin` regardless (exit 1), so a caller
  that ignores the exit code gets a bad blob on disk with a plausible name. *Fix:* refuse to write
  (or write a `.bad` name) on mismatch.

#### Info

- **5.11 — firmware absence is handled asymmetrically.** Runner-microcode absence is non-fatal (binds
  anyway, `bcm4916_runner.c:722-729`) while SerDes-fw absence returns an error the caller drops (5.3);
  document the intended bind-anyway-vs-feature-off contract so operators can tell a broken install
  from a slow-path-only run.

---

### qemu-model (`qemu/device-model/*`)

The offline validation harness (Runner datapath model + SF2/MDIO/XPORT control-plane model).
Findings from `07-qemu-model.md §5`, relabelled `QF*` to avoid the flow-offload `F*` collision.
Because this is a test harness rather than shipping/silicon code, most findings affect **test
fidelity**, not driver correctness on hardware.

#### Medium

- **QF1 — the contract markdown documents a superseded ring layout (stale spec).**
  `runner-emulation-contract.md:91-219,347-369` still describes the CPU rings in PSRAM `0x0000`(RX)/
  `0x0080`(TX) with a per-descriptor word2-ownership protocol and **no feed ring**; the implemented
  model and driver instead put the rings in per-core RNR_MEM (RX core3+0x3000, TX core2+0x33e0, FEED
  core3+0x0f70, TX indices core2+0x29c8) with a feed ring and `write_idx`/`read_idx` polling. Only the
  §6 Route A / §6b NAT-C parts are current. *Why:* the audited prose contract no longer matches the
  code it specifies. *Fix:* resync the contract. See cross-cutting §2/§11.

- **QF5 — `Property` array lacks a `DEFINE_PROP_END_OF_LIST()` terminator.** `bcm4916_runner.c:1587`
  is just `DEFINE_NIC_PROPERTIES(...)`; the classic NULL-name-terminated `Property`-array API would
  iterate past the array. *Why:* latent UB unless the QEMU 10.0 counted-props API tolerates it (it
  "reportedly builds and runs"). *Fix:* verify against the exact QEMU 10.0.0 `Property` API; add the
  terminator if needed.

- **QF7 — hardcoded / placeholder offsets, explicitly flagged.** The PSRAM NAT-C staging offsets
  (`0x0100/0x0120/0x0200/0x0204`), the add/del command values (3/4), the NAT-C key packing, and the
  context byte offsets (`CTX_OFF_*`) are driver↔model contract placeholders (`bcm4916_runner.c:132-152`);
  the L3-key `w3` layout (`0x28`/`0x68`/dir/ack, `:496-537`) is pinned from a single live capture; the
  Route A QM/BBH_TX offsets come from the RE notes. **Same root as flow-offload F3/F19/F20, runner
  R-L5, cross-cutting §1.** *Fix:* pin from silicon RE.

- **QF11 — both IRQ lines are pulsed on every RX (workaround, not fidelity).** `bcm4916_runner.c:1034`,
  `:850` assert/deassert **both** SPI 75 and SPI 107 because `platform_get_irq_byname("queue0")`
  resolves to the fpm SPI on this DT. *Why:* the real RDD interrupt id and the DT `interrupt-names`
  order remain unpinned. *Fix:* pin the IRQ contract. See qemu-Q5.

- **QF12 — `run-validate.sh` (the tx=4/rx=6 proof) is not in the repo.** The contract §6 asserts both
  the legacy and `route_a=1` variants reach `VALIDATE_DONE tx=4/rx=6` via `run-validate.sh`, and the
  README quotes evidence logs, but neither `run-validate.sh` nor the Phase-1 `init-offload.sh` exists
  under `qemu/scripts/` — they live on the build host only. *Why:* the published proof cannot be
  reproduced from the repo alone. *Fix:* commit the validation scripts. See cross-cutting §2.

#### Low

- **QF2 — vmstate is drastically incomplete.** `vmstate_bcm4916_runner` (`bcm4916_runner.c:1573`)
  migrates only 5 FPM scalars; rings (`rx/tx/feed`), `feed_rcons`, cfg buffers, `tx_indices`, the
  64-entry NAT-C table + staging, the `tok_used` bitmap + `tok_avail`, and the entire Route A block are
  not saved. Acceptable for a never-migrated harness; silent state loss otherwise.
- **QF3 — device reset does not clear Route A state.** `bcm4916_runner_reset` (`bcm4916_runner.c:1514`)
  leaves `qm_mem_init`, `qm_enable`, `qm_grp[]`, `bbh_qmq[]`, `route_a_egress` untouched; a warm
  reset/reboot keeps stale QM/QMQ config and can **mask a driver that fails to re-program Route A**.
- **QF4 — `runner_can_receive` blocks offload-HIT frames when the feed ring is empty.**
  `bcm4916_runner.c:877` requires an unconsumed feed buffer, but an offload HIT forwards without one;
  when the feed ring drains the backend stops delivering even HIT frames. Rarely bites (the driver
  keeps the feed ring filled).
- **QF6 — `word2` bit31 is overloaded (is_src_lan vs ownership).** `bcm4916_runner.c:1001`; the same
  bit serves is_src_lan and the stale contract's ownership role. The coincidence works but the intent
  is undocumented. Tied to QF1 and hw-abi HW-4.
- **QF8 — FPM pool is always sized to the 64K maximum, ignoring `POOL1_CFG2`.** `bcm4916_runner.c:306`.
  Harmless with the driver's 32 MB/512 B pool (exactly 64K) but not defensive; a smaller reserved pool
  could hand out an index past reserved DDR.
- **QF9 — jumbo RX is silently truncated.** `bcm4916_runner.c:903` clamps `len` to `RX_BUF_MAX`
  (2048) — a >2048-B frame is corrupted, not dropped. First-light limitation.
- **QF13 — MMD data writes are mis-latched as address phases.** `bcm4916_sf2.c:373` always treats an
  `MII_MMD_DATA` write as the register-address phase; benign because the modelled MMD regs are
  read-only status, but a driver that writes an MMD register would see no effect.

#### Info

- **QF10 — read-with-side-effect + no MMIO reentrancy guard on alloc.** `runner_read` of
  `FPM_POOL0_ALLOC_DEALLOC` mutates the pool (`fpm_alloc`); the intended alloc-on-read ABI, but any
  double/speculative read would consume two tokens. Acceptable in the single-threaded model.
- **QF14 — static link, no interrupt regions on the switch.** The SF2 `intrl2_0/1` are not decoded,
  link is static BMSR-up, and `CORE` reads default to 0, so switch ISRs never fire and PHY state never
  changes. Expected for a bind-only proof.

---

### hw-abi (`re-notes/*`)

The hardware/ABI reference the whole driver programs against; its "findings" are unverified /
placeholder / conflicting *values* that a driver author must not trust blindly. Findings from
`08-hw-abi-regmap.md §8`; several early flags are now **RESOLVED** and carried as info provenance.

#### High

- **HW-4 — the RX consumer model is contradictory in the notes (the biggest open ABI item).**
  `re-notes/xrdp-datapath-abi.md:191-210` (ownership = word2 bit31; host re-arms by setting it) vs
  `re-notes/realhw/10-runner-bringup-spec.md:182-184,243-247` (index-polled: host polls `read_idx`@0xc
  vs `write_idx`@6 of the `CPU_RING_DESCRIPTOR`). *Why it matters:* the driver committed to
  index-polling and the QEMU model validates that, but if silicon actually uses ownership bits the
  entire RX path is wrong. *Fix/next:* pin against live silicon (read-only). **Same open item as
  runner R-I1 / runner-Q2 / runner-datapath §5.3.** See hw-abi-Q1.

- **HW-16 — exact 6813 `FC_UCAST_FLOW_CONTEXT_ENTRY` byte offsets are UNKNOWN** (autogen header
  stripped from the GPL drop; only the RDP-impl2 template is available).
  `re-notes/xrdp-offload-abi.md:126-149`. **Same gap as flow-offload F3/O2, cmdlist-Q5, qemu QF7/OQ2**
  — the fast-path context ABI blocker. *(Corroborates Top-5 #4.)* *Fix/next:* RE `rdpa.ko
  _ucast_prepare_rdd_*` or the 6813 RDD header. See hw-abi-Q6.

#### Medium

- **HW-9 — per-port BBH RX/TX register VALUES are not individually disassembled.**
  `re-notes/xrdp-datapath-abi.md:562-564`, `re-notes/realhw/10-runner-bringup-spec.md:98-102`. The
  accessor set + call order are pinned, but the per-port immediates (`sdma_config`, `pkt_size`,
  `flow_ctrl_timer`, `min/max_pkt_sel_flows`, BBH-TX `q2rnr`/`rnrcfg`) need per-call disasm or a live
  regdump. *Fix:* disasm the per-port config or capture a live regdump. See hw-abi-Q3.

- **HW-10 — the DMA/SDMA base is stated two ways.** `DMA_ADDRS` triple `0x828a1800/1c00/2000`
  (`re-notes/xrdp-datapath-abi.md:57`) vs Wave-9 "DMA/SDMA base `0x828b2000` (DMA1 +0x800)"
  (`re-notes/realhw/10-runner-bringup-spec.md:186`). *Why:* must reconcile before coding DMA (the
  latter may be the XLMAC-adjacent SDMA view). *Fix:* reconcile. See hw-abi-Q4.

- **HW-12 — the Route A logical→physical QM queue map is not fully resolved.**
  `re-notes/realhw/11-route-a-egress-spec.md:230-241`. `route_a_bbh_inst=1` is solid (only QM-fed
  instance); the global QM queue for `port_gphy1` CPU-TX egress (candidate A vs B) and that BBH_TX[1]
  is that port's instance are unresolved. **Same as stock-oracle A-10 / stock-Q1 / runner-Q…** *Fix:*
  refined kprobe or empirical egress test.

- **HW-13 — CPU_TX `is_egress`/`first_level_q`/`flow_or_port_id` logic + QoS-table format are
  ★UNKNOWN.** `re-notes/realhw/11-route-a-egress-spec.md:146-152,187`; lives only in proprietary
  `rdpa_cpu_tx`. *Why:* needed for correct QM-fed egress addressing. **Overlaps runner R-M2 /
  runner-Q3.** *Fix:* RE `rdpa_cpu_tx`. See hw-abi-Q5.

- **HW-18 — the ADD/COPY XPE opcode numbers are compiler-grouped ranges `[INFER]`, not single pinned
  values.** `re-notes/xrdp-offload-abi.md:190-196,461`; the live cmdlist capture was GDX-local (it
  pins encoding/framing, not a byte-match of L2/NAT programs). **Same as cmdlist C2/C3/O5, qemu
  QF7/OQ3.** *Fix:* disasm `xpe_api.o` + a live L2/NAT capture. See hw-abi-Q6.

- **HW-19 — the SF2 switch-core register base is NOT in the FDT.** `re-notes/bcm4916-regmap.md:270-276`.
  The stock switch is exposed as "RUNNER_SW" with no `reg`; a `bcm_sf2`/DSA driver needs the SF2 core
  base, requiring deeper RE. *Fix:* RE the SF2 base. (Separate DSA subsystem.) See hw-abi-Q7.

#### Low

- **HW-7 — TX `word3` FPM fields were unverified / carried from another chip, now re-derived**
  (`fpm_bn0[19:0] | fpm_sop[29:20]`, `bn = index|(pool<<17)`).
  `re-notes/realhw/10-runner-bringup-spec.md:95,232-234`. Resolved, but worth on-silicon confirmation
  of FPM-mode TX. **Same as runner R-I2.**
- **HW-8 — `SBPM_INIT_FREE_LIST` value is project-variant.** `0x03FFC000` (rdpa) vs `0x5fc000` (CFE2
  bootloader pool). `re-notes/realhw/10-runner-bringup-spec.md:192,212-213`. Use the BCM6813 rdpa
  value; SBPM is slow-path-optional for the first CPU frame.
- **HW-17 — NAT-C HW hash polynomial / bucket selection is engine-internal / UNKNOWN.**
  `re-notes/xrdp-offload-abi.md:88,469`. Non-blocking: the host writes key+result and lets HW place.
- **HW-20 — CPU/IMP port index + `mdio-sf2` busy/start semantics + EGPHY/ethphytop mux sequence are
  guessed/`[INFER]` in the DTS skeleton.** `re-notes/bcm4916-regmap.md:249-266,282-288`. (The EGPHY
  mux part overlaps runner R-L2.) *Fix:* confirm on silicon.

#### Info

- **HW-15 — the MPM HW bring-up sequence has no GPL source** (register map recovered from `bcm_mpm.ko`
  disasm; bitfield names / per-ring strides are gaps). MPM lives outside the XRDP window
  (`0x80020000`/`0x4000`) and is skipped on the open MPM-free first-light path
  (`re-notes/realhw/10-runner-bringup-spec.md:162-170`). A single read-only `mpm_reg_dump` would
  resolve it.
- **HW-RESOLVED — now-resolved value provenance (historical).** Early proxy/variant values since
  pinned, retained only for provenance (doc §8 items 1,2,3,5,6,11,14): RNR_MEM base `0x82700000` (an
  early header used the 6837/6888 proxy `0x82600000`, wrong by +0x100000); RNR_REGS base `0x82800000`;
  DQM block base `0x82c80000` (not the `…0034` accessor base); CPU_TX ring desc `0x33e0` (BCM6813, not
  `_FPI` `0x3360`); ring resolution (RX/TX 32-res `>>5`, feed 64-res `>>6`); QM `MEM_AUTO_INIT` done =
  bit0; `rnr_image_first_task` = debug-only zeros.

---

## Cross-cutting concerns

**§1 — Placeholder ABI pending silicon RE (the dominant risk).** Most subsystems are validated only
against a self-consistent QEMU model that they themselves define, with the real contract still owed:
- flow-offload context bitfields (F3) and NAT-C key/mask/registers (F19/F20), the same offsets flagged
  independently UNKNOWN by the hw-abi reference (HW-16), and re-stated in the runner driver (R-L5) and
  the model (qemu QF7);
- cmdlist operand packing (C2) and `.text`/`.data` relocation (C1), with the ADD/COPY opcode numbers
  independently flagged inferred by hw-abi (HW-18);
- pcs-serdes register bases/offsets (L1) and the SerDes microcode load (H1);
- stock-oracle probe offsets (A-7); the DMA/SDMA base (HW-10); per-port BBH values (HW-9); the RX
  consumer model (HW-4).
These are honestly flagged in-source, but the driver cannot reach silicon until each is RE'd from
`rdpa.ko`/`xpe_api.o`/the 6813 RDD headers or confirmed by a read-only live dump. The stock-oracle
tooling (docs 06) is precisely the rig built to close them — `natc_dump` for the context/key layout,
`rdpa_trace` for the flow-add/cmdlist args — so the oracle and the placeholders are two ends of the
same open item.

**§2 — Oracle-vs-code / stale-and-absent proofs.** The "proven in QEMU" evidence predates the current
code in several places, the audited prose specs no longer match the code, and none of the emitted
fast-path programs are validated against real silicon:
- the per-op cmdlist encoding no longer matches the logged byte streams (F13), and the QEMU model
  "must be updated to decode the new layout" (`cmdlist.h:44`) — so whether the self-tests still pass
  is unverified (O6);
- the routed-flow NAT-C key evidence is stale (F12);
- no live L2-accel or routed/NAT cmdlist was ever captured (C3), and the GRP_OFFLOAD live-capture
  window never happened (A-8 / HW-18);
- the QEMU emulation contract still documents a superseded PSRAM/ownership ring layout (qemu QF1);
- the headline tx=4/rx=6 validation script (`run-validate.sh`) and the Phase-1 init script are not in
  the repo at all (qemu QF12), so the published proof cannot be reproduced from the tree.
Net: the byte-for-byte "proof" is stale for the emitter, absent for the actual VLAN/NAT programs, and
not reproducible from the repo. Action: re-run the offload/NAT self-tests against the current tree,
refresh the status notes and the emulation contract, commit the validation scripts, and schedule a
live offload-flow capture.

**§3 — Proprietary-blob boundary drawn (mostly) consistently.** The SerDes PMD microcode (H1 / firmware
5.2) and the Runner microcode are non-redistributable blobs the open driver deliberately does not
embed: it hosts the load mechanism and expects a user-supplied image, inheriting the same `P` taint
dependency as the stock module (hw-abi §4.3 / OQ8). Two frictions remain: the tree ships **two**
disagreeing SerDes loaders (firmware 5.2 / cross-cutting §11), and one driver comment still mislabels
the Runner microcode as GPL-derived (firmware 5.10) — contradicting the RE verdict. Documenting a
single blob-sourcing story (extract from the user's own vendor image; strictly non-redistributable)
and reconciling the 5.10 comment covers both.

**§4 — Error-handling pattern: silent success / narrowing / overflow.** A recurring class where a
failure or out-of-range value is swallowed rather than surfaced:
- return values ignored / success-on-failure: `load_firmware` stub returns 0 when not implemented
  (H1) and its PCS caller discards the return (pcs L5); `runner_serdes_load` return dropped by probe
  (firmware 5.3); the SerDes CRC is never checked (firmware 5.4); `p_res_get` return discarded (A-2);
- silent narrowing/rounding: length `u16`→`u8` (F2/C9), `xpe_to_idx` drops odd-offset low bit (C5),
  `decrement_8` ignores parity (C4), `replace_bits_16` nibble overflow/underflow (C6);
- swallowed timeouts / missing backpressure: mandatory bring-up polls warn-and-continue (R-L6);
  `netif_stop_queue` with no matching `netif_wake_queue` (R-M1);
- latent buffer overrun without a valid-check (A-1); partial mid-word buffer on overflow (C10).
Action: add `WARN_ON`/`-E*` guards at these narrowing/overflow points, propagate load failures, and
fail probe when a mandatory block never signals ready.

**§5 — Untagged-IHL=5 / even-offset assumption spans flow-offload and cmdlist.** The NAT offset
constants assume an untagged IPv4 IHL=5 frame (F6) and every offset-addressed emitter assumes even
offsets (C4/C5); a VLAN-tagged or IP-options routed flow would misfire silently. Action: parameterise
the offset constants on tag/IHL and assert offset evenness in the emitter. See cmdlist-Q6.

**§6 — Missing teardown / HW-state cleanup on the real-HW path.** flow-offload leaks programmed NAT-C
entries at module unload (F17), pcs-serdes never re-asserts SerDes/MPCS resets on link-down/unbind
(pcs L6), and the runner driver never un-programs the NAT-C table it stages (R-L4/F15 stub). All are
harmless in QEMU but leave real hardware in a stale state. Action: un-program NAT-C in the free
callback; add `.pcs_disable`.

**§7 — Concurrency (minor).** The debugfs self-tests mutate the flow rhashtable without the
`xrdp_flow_mutex` the real path holds (F18) — safe single-threaded, racy if ever driven concurrently.

**§8 — Live-capture operational safety.** The oracle's MMIO reads can hang the SoC (A-4) and it clears
the kernel ring buffer each pass (A-11); the only backstop is the automatic watchdog recovery. Action: an
address allowlist and non-destructive dmesg reads before any further live register work. (See §9 —
the driver itself carries the same class of hang risk.)

**§9 — SoC-hanging operations with only the watchdog as backstop (NEW, spans driver + oracle).** Two
independent code paths can hang the SoC on live silicon: the driver's *default* `insmod` writes QM
`0x307` without `MEM_AUTO_INIT` (R-H1), and the oracle's raw `ioremap`+`readl` of the wrong XRDP
sub-block already forced an automatic watchdog recovery (A-4). Both violate the no-connectivity-break
rule's spirit and rely on the watchdog rather than a safety gate. Action: gate the QM `0x307` write
behind SRAM auto-init (or default `qm_enable=0` unless `route_a=1`), and encode the oracle
side-effect-free register allowlist (stock-Q5).

**§10 — The slow path cannot yet carry real traffic on silicon end-to-end (NEW).** Independent of the
ABI unknowns, several defects mean the v1 conduit is not yet usable for ordinary traffic on hardware:
the TX path drops any frame >512 B (R-H2; conduit `max_mtu=498`, and the model corroborates the 512-B
FPM-chunk MTU cap in qemu-Q7); poll-mode RX (the pdev takeover path) stops after one NAPI pass (R-H3);
and without a loadable Runner blob no frame moves at all (firmware 5.1). Action: multi-chunk/SBPM TX
chaining, a self-sustaining RX poll (timer or IRQ), and a committed `RFW1` producer.

**§11 — Divergent / conflicting implementations of the same block (NEW).** Several blocks have more
than one source of truth that disagree: the SerDes microcode load (real streaming loader in
`bcm4916_runner.c` vs warn-only stub in `pcs-bcm-xport.c`, firmware 5.2), the RX consumer model
(word2-ownership vs index-polling, HW-4), and the emulation contract vs the implemented ring layout
(qemu QF1). Action: pick one authoritative implementation/spec per block and delete or reconcile the
other before silicon bring-up.

---

## Open questions register

Tagged **[offline]** = resolvable by RE/disassembly/source reading without touching the board;
**[silicon]** = needs a live board (read-only where possible per the no-connectivity-break rule);
**[mixed]** = an offline RE step that still needs live confirmation.

### flow-offload

- **O1 — which `flow_block` binder type does the `TC_SETUP_FT` path present?** Determines whether the
  `CLSACT_INGRESS` gate (F16) admits or rejects flowtable offload. **[mixed]** — the binder type is
  readable in the nf_flow_table core source; admit/reject on this driver needs the live path with
  `CONFIG_NF_FLOW_TABLE` on and a 2-port topology.
- **O2 — real 6813 `FC_UCAST_FLOW_CONTEXT_ENTRY` byte offsets + bitfield packing** (F3, hw-abi HW-16).
  **[offline]** — RE `rdpa.ko _ucast_prepare_rdd_*` / `_l2_ucast_prepare_rdd_*` or the 6813
  `rdd_data_structures_auto.h`.
- **O3 — real NAT-C key composition, per-table mask, table-id↔direction/class mapping** (F4/F5/F9/F19).
  **[mixed]** — RE `rdd_ag_natc_*_mask_*` + `data_path_natc_init` **or** a read-only live NAT-C dump
  (`natc_dump.ko`).
- **O4 — real NAT-C indirect-add register interface + HW hash/slot selection; does a delete opcode
  exist?** (F20, hw-abi HW-17). **[offline]** — RE `drv_natc_key_result_entry_var_size_ctx_add` /
  `drv_natc_key_idx_get`.
- **O5 — exact XPE operand bit-packing on silicon** (C2). **[mixed]** — disassemble `xpe_api.o`; the
  emitted L2-accel/routed-NAT programs still need a live byte-for-byte capture (C3).
- **O6 — has the QEMU Runner model been updated to the pinned per-op encoding?** (F13). **[offline]** —
  inspect `qemu/device-model/bcm4916_runner.c` and re-run the offload/NAT self-tests against the tree.
- **O7 — routed egress L2-header rewrite (next-hop dest MAC insert).** Not emitted by
  `xrdp_build_nat_cmdlist`; real port-to-port routing needs it plus the F7 vport resolution.
  **[mixed]** — design/impl offline, validate on multi-port silicon.

### cmdlist

- **cmdlist-Q1 — sub-opcode/operand bit encoding below byte0/byte1** (byte2/byte3 per family,
  `.data`-reference/relocation, `replace_16`-vs-`_32`, `replace_bits` pos/width). Same as O5.
  **[offline]** — disassemble `xpe_api.o`/`cmdlist.ko`, correlate with matched-4916 GPL microcode arrays.
- **cmdlist-Q2 — does silicon recompute the icsum delta or require the precomputed 16-bit immediate?**
  (C11). **[mixed]** — the stock emitter's `bfxil` hints "immediate"; confirm from the disasm and/or a
  live checksum test.
- **cmdlist-Q3 — the relocation/`.data` model of the real `xpe_cmd_end`** (C1). **[offline]** — RE how
  `byte2`'s `0x94` base is fixed up and where `.data` lands relative to `.text`.
- **cmdlist-Q4 — byte-for-byte validation of the emitted L2 and NAT programs** (C3). **[silicon]** —
  a dedicated capture of a bridged and a routed/NAT flow's cmdlist without disturbing the management link.
- **cmdlist-Q5 — true `command_list[]` slot size and the `cmd_list_length` vs `cmd_list_data_length`
  unit** (live showed 28 vs 40; GPL struct reserves 100). **[offline]** — pin from
  `rdpa.ko _ucast_prepare_rdd_*`.
- **cmdlist-Q6 — tagged-frame packet offsets** (F6, cross-cutting §5): interaction of a VLAN push/pop
  with the NAT offsets in the same cmdlist is untested. **[offline]** — design + self-test.
- **cmdlist-Q7 — `decrement_8` odd-offset encoding (byte0 bits[25:24]) and whether ADD is ever used
  for anything other than TTL/hop-limit −1** (C4). **[offline]** — from the emitter disasm.

### pcs-serdes

- **pcs-O1 — the SerDes microcode load mechanism** (H1): PRAM window sequence, CRC verify, post-load
  VCO/datapath steps, and whether it goes through the two mapped windows or the indirect-access path.
  **[mixed]** — known from the closed SDK (offline RE); blob is user-supplied and load must be validated
  on silicon.
- **pcs-O2 — real lock timing / settling requirements** (H2/H3, firmware 5.9). **[silicon]** — QEMU
  fakes instant lock; the actual delays and PLL/CDR wait loops need a live board.
- **pcs-O3 — USXGMII / 2500BASE-X `an_config` values + in-band AN handshake** (M1/M2). **[mixed]** —
  register values RE'd from the SDK; never linked past PCS-select on silicon.
- **pcs-O4 — register bases/offsets vs silicon** (L1). **[silicon]** — one read-only regdump pass
  validates or corrects them.
- **pcs-O5 — DSA-port ↔ XLMAC-core ↔ SerDes-core identity** (M4/L8, firmware-Q5). **[silicon]** —
  assumed 1:1, `get_state` hardcodes core 0, multi-core correctness untested.
- **pcs-O6 — is the third SerDes core unused on the GT-BE98 or wired to a port not yet brought up?**
  **[silicon]** — not stated in code+notes.

### stock-oracle

- **stock-Q1 — physical QM egress queue for the LAN CPU-TX path** (A-10, hw-abi HW-12, runner-Q2).
  **[silicon]** — logical `queue_id=0` is port-relative; the logical→physical map (candidate A vs B)
  needs a refined kprobe or an empirical egress test.
- **stock-Q2 — does BBH_TX[1] map to the specific first LAN port** the open driver brings up?
  **[silicon]** — BBH_TX[1] is proven the only QM-fed instance but not tied to that port.
- **stock-Q3 — QM `MEM_AUTO_INIT` done semantics** (exact wait/timeout). **[mixed]** —
  `MEM_AUTO_INIT_STS=0x01` observed; the stock poll is rdpa-internal, the open driver's is derived.
- **stock-Q4 — GRP_OFFLOAD ABI against silicon** (A-7/A-8): NAT-C-add / cmdlist-compile / RDD-commit
  arg layouts, and the XPE NOP-framing `0x3f<<8` vs `0x3f<<10`. **[mixed]** — needs the `xpe_api.o`
  disasm (offline) plus a live NAT/VLAN capture window.
- **stock-Q5 — which XRDP sub-blocks are MMIO-safe** (A-4, cross-cutting §9). **[silicon]** — QM/BBH/
  RNR_MEM read OK, FPM hung; no derived rule, so extending the oracle carries an unquantified SoC-hang
  risk bounded only by the watchdog.
- **stock-Q6 — full packed-bitfield map of the context entry beyond what `natc_dump` proved** (ABI
  UNKNOWN #1). **[mixed]** — cmdlist@24 and total=124 B are pinned; the named fields
  (`is_hw_cso`, `tx_adjust`, …) were never diffed as raw DDR bytes. Overlaps O2.

### runner-core

- **runner-Q1 — the `emac → bbh_id / VIQ` map for `port_gphy1`** (R-M4). Is the 1G first-light port
  bbh_id 0 or 1? Everything downstream (dispatcher VIQ, BBH_RX/TX instance, `RUNNER_FIRST_PORT`)
  depends on it and it lives only in the closed `rdpa.ko`. **[silicon]** — live oracle capture.
- **runner-Q2 — the RX consumer model: word2-ownership vs `read_idx`/`write_idx` index polling**
  (R-I1). The two RE passes disagree; the driver chose index polling. **[silicon]**. **Same as
  hw-abi-Q1.**
- **runner-Q3 — CPU_TX path selection: explicit `first_level_q` vs microcode `VPORT_TX_FLOW_TABLE`
  resolution** (which needs a QoS table of unknown format; the driver only sets `first_level_q`).
  **[mixed]**. **Overlaps hw-abi-Q5 / R-M2.**
- **runner-Q4 — minimal safe QM enable mask** (R-H1): is `<0x307` (e.g. `0x7`) enough to drain, and
  does SRAM auto-init alone make the full `0x307` safe on live silicon? **[silicon]**. Related to
  stock-Q3.
- **runner-Q5 — the QM per-queue context / WRED / DQM-enable steps** (R-M2): are they required for the
  QM-fed BBH_TX slow path, and what WRED-pass / fpm_ug values suit a CPU-egress queue? **[silicon]**.
- **runner-Q6 — the EGPHY register-offset +4 ambiguity** (R-L2 / hw-abi HW-20). **[silicon]**.
- **runner-Q7 — DSPTCHR CPU_RX field encodings** (`crdt_cfg` layout, the global task id, the
  `0x3940>>3` PD wake address) are RE-derived, unconfirmed; RX has not been exercised on hardware,
  only TX. **[silicon]**.
- **runner-Q8 — do the microcode thread register-file values** (R0 entry, PD/UPDATE-FIFO/stack
  addresses) match the microcode extracted on a given device? They are hardcoded for one BCM6813
  image. **[mixed]** — re-derive from the loaded image (offline) + confirm on silicon.
- **runner-Q9 — the full 10G SerDes link** (`runner_serdes_load` streams the ucode + confirms
  `uc_active` only; PLL/VCO/lane AFE/AN/PMD lock unimplemented). **[silicon]**. **Overlaps
  pcs-O1/O2, firmware-Q5.**

### firmware

- **firmware-Q1 — where is the canonical `RFW1` generator?** (5.1). The committed extractor emits
  `B4916UC`; `gen_runner_fw.py` lives only on the build host. Its exact packing (esp. how it sources
  the per-core inst/pred) must be recovered or rewritten. **[offline]**.
- **firmware-Q2 — Runner-microcode provenance/licensing, definitively** (5.10): are there *any* GPL
  per-core arrays for the 4916 (none found), or is `rdpa.ko` extraction the only route (strictly
  non-redistributable)? Determines whether any legal ship path exists. **[offline]**. **Overlaps
  hw-abi-Q8.**
- **firmware-Q3 — PRED image SRAM slot stride/endianness on silicon** (1 KB u16 → 512 BE-u32),
  inferred from the stock loader; needs a live RNR_PRED read-back. **[silicon]**.
- **firmware-Q4 — Runner INST SRAM endianness** (taken from the stock loader's `iowrite32be`),
  unverified against a live RNR_INST dump. **[silicon]**.
- **firmware-Q5 — SerDes core/lane mapping for the actual 10G port** (`serdes_core`/`serdes_lane`
  default 0; the lane→port mux is unprogrammed). **[silicon]**. **Same as pcs-O5.**
- **firmware-Q6 — which SerDes bring-up path is authoritative** — the runner `runner_serdes_load()` or
  the PCS `bcm_xport_pcs_load_firmware()` stub (5.2)? Both target the same block via different
  windows/register maps. **[silicon]**. See cross-cutting §11.
- **firmware-Q7 — the `RA_INITDONE` done bit (5.6), the µC reset-recipe correctness, and the poll
  timeouts (5.9)** — all unresolved without real Merlin hardware (QEMU fakes PMD/PCS lock and never
  exercises the PRAM load or µC timing). **[silicon]**. **Overlaps pcs-O2.**
- **firmware-Q8 — `uc_active` semantics** (bit15 of `0xd0f4`): is that bit alone sufficient evidence
  the PMD microcode is executing (vs merely clocked)? **[silicon]**.

### qemu-model

- **qemu-Q1 — true RDD table offsets** (`0x3000` RX-delivery, `0x33e0` TX, `0x0f70` FEED, `0x29c8` TX
  indices): are these the exact per-image RDD core-data offsets on live silicon (and for which core
  mapping)? Pinned "vs SDK oracle" but still named `PSRAM_CPU_*`. **[mixed]** — GPL RDD map (offline)
  + a live devmem confirm. **Overlaps hw-abi / stock-oracle.**
- **qemu-Q2 — NAT-C key + context byte layout on 6813** (key packing, the L3 `w3` bytes, `CTX_OFF_*`,
  `XPE_CMDLIST_MAX=80`, the indirect cmd/index offsets). **[offline]** — RE
  `FC_UCAST_FLOW_CONTEXT_ENTRY_STRUCT` + `xpe_api`. **Same as O2/O3, hw-abi-Q6.**
- **qemu-Q3 — XPE opcode encoding vs silicon** (byte0 values, `replace_bits` pos/width, the
  length-delimited framing + `0xfc` pad). **[mixed]** — the model agrees with the driver by
  construction; a live cmdlist capture confirms silicon decodes identically. **Same as O5,
  cmdlist-Q1/Q4.**
- **qemu-Q4 — QM/BBH_TX Route A egress semantics**: does silicon additionally require DSPTCHR
  egress-credit / reorder-credit sequencing beyond the model's "QM enabled + ≥1 RUNNER_GRP + any
  BBH_TX QMQ + `first_level_q` in range"? **[silicon]**. **Overlaps runner-Q5.**
- **qemu-Q5 — the IRQ contract**: which SPI actually carries `queue0` (75 vs 107), the
  `interrupt-names` order, and whether the real Runner raises an FPM IRQ at all (QF11). **[silicon]**.
- **qemu-Q6 — 10G is link-*state* only**: the serdes/MPCS "lock on reset-release" fakes the Merlin PMD
  microcode; real 10G needs the non-redistributable image + true AN, and the modelled 1G-class
  SF2/b53 MAC caps the copper 10G XPHY to GMII. **[silicon]**. **Overlaps pcs-serdes / firmware-Q5.**
- **qemu-Q7 — pool sizing / DSA user-port MTU**: the 512-B FPM chunk caps the conduit MTU (~498 B) so
  switch *user* ports can't be exercised end-to-end; is the real chunk 512 B or larger, and how is the
  conduit MTU meant to be raised? **[silicon]**. **Same root as runner R-H2, cross-cutting §10.**

### hw-abi

- **hw-abi-Q1 — RX consumer model: word2-ownership bit vs `read_idx`/`write_idx` index polling**
  (HW-4). The single most important RX ABI to settle on silicon; the two RE passes disagree.
  **[silicon]**. **Same as runner-Q2.**
- **hw-abi-Q2 — which TM image/thread owns LAN egress** (`RNRCFG_2.TASK` / `RUNNER_GRP.RNR_TASK`):
  the live map gives grp0=core7/task3 (DS_TM) and grp1=core6/task4 (US_TM); which a LAN CPU-TX frame
  lands in is the queue-map question. **[silicon]**. Tied to HW-12 / stock-Q1.
- **hw-abi-Q3 — per-port BBH register values + dispatcher credit/VIQ values** (HW-9): the immediates
  need per-call disasm or a live regdump. **[mixed]**.
- **hw-abi-Q4 — DMA/SDMA base reconciliation** (HW-10): `0x828a1800/1c00/2000` vs `0x828b2000+0x800`,
  or two different DMA views? **[offline]**.
- **hw-abi-Q5 — CPU_TX `is_egress`/`first_level_q` resolution + QoS-table format** (HW-13).
  **[mixed]**. **Same as runner-Q3.**
- **hw-abi-Q6 — exact 6813 `FC_UCAST` offsets, NAT-C hash, and the ADD/COPY opcode numbers**
  (HW-16/17/18): the fast-path blockers; a single read-only dump of a live NAT'd flow's NAT-C entry
  (key + context + cmdlist bytes) would collapse most of these. **[mixed]** — RE + one live dump.
  **Consolidates O2/O3/O5.**
- **hw-abi-Q7 — SF2 switch-core base + clocks/resets for the MAC/serdes nodes** (HW-19), not in the
  stock FDT. **[offline]** — deeper RE. (Control-plane / DSA subsystem input.)
- **hw-abi-Q8 — firmware licensing**: the Runner microcode is proprietary/non-redistributable, so a
  fully-open shippable datapath is not achievable today; any working datapath inherits a `P`-taint
  dependency on a user-supplied image. **[offline]** (settled verdict). **Same as firmware-Q2,
  cross-cutting §3.**
