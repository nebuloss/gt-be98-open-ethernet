# Offload builders — LIVE-SILICON validation (our cmdlist/context/key vs the stock NAT-C entry)

Validates the OPEN offload builders (`driver/runner/cmdlist.c` + the `xrdp_build_*` functions in
`driver/runner/flow_offload.c`) against what the **stock Runner actually programmed** for a live
flow on the GT-BE98 (BCM4916). Goal: pin the XPE cmdlist operand bit-packing (ABI UNKNOWN #3) and
catch builder bugs by diffing our emitted bytes against real silicon.

**Method:** read-only capture from the device over the mgmt SSH link (link stayed up — `LINK_OK`
before and `LINK_STILL_UP` after every read group); a userspace harness on dev-build that compiles
the **REAL** builder `.c` code (with light kernel shims) and feeds it the captured flow inputs;
byte-for-byte diff.

PUBLIC-SAFE: device/peer IPs shown as the lab `10.0.0.x` private range; MACs are the lab device's
Broadcom OUI `60:cf:84:xx` and the peer `bc:24:11:xx`. No internal infra / credentials.

---

## 0. SAFETY LEDGER

- Device commands (read-only only): `cat`/`head` of `/proc/fcache/{nflist,brlist,l2list}` and
  `/proc/pktrunner/accel0/{stats,flows/L3}`; `bs /Bdmf/e ucast`, `bs /Bdmf/e l2_ucast` (bdmf-shell
  EXAMINE = read-only print); `fc status`. No writes, no `*clr*` reads, no `*cmd*` writes, no
  devmem, no module load/unload, no traffic generation.
- `echo LINK_STILL_UP` confirmed alive after the last read. Management Ethernet never disturbed.
- The harness builds + runs on **dev-build** only (never on dev-code). The driver module build also
  ran on dev-build via `rtk`.

---

## 1. CAPTURED LIVE FLOWS (read-only)

The device is an idle lab unit: the **only** accelerated traffic is the mgmt/SSH flow itself. Same
picture as the prior `live-flow-dump.md` capture.

- `/proc/fcache/nflist` — **empty** (no IP-forwarded / NAT flows).
- `/proc/fcache/l2list` — **empty** (0 L2-accel flows).
- `bs /Bdmf/e l2_ucast` — `nflows=0` (no L2 bridge-accel flow to capture).
- `/proc/fcache/brlist` — **4 flows**, all the SAME 5-tuple
  `<peer-ip>:<sport> -> <device-ip>:2222` TCP, `RxDev=eth0 TxDev=blogtcp_local` (locally terminated),
  split by ToS (0xb8 / 0x20) and TCP_PURE_ACK (0/1) — `fc_tcp_ack_mflows=1`, `fc_tos_mflows=1`.
- `bs /Bdmf/e ucast` — **4 ucast flows** (this session), all:
  `is_routed=0, is_l2_accel=0`, `port_egress_obj=gdx0` (local/GDX delivery), `is_gdx_tx=yes`,
  `is_hw_cso=yes`, `policer=null`, `tx_adjust=-10`, `cmd_list_data_length=28`, `cmd_list_length=40`.

### The stock cmdlist (the prize) — 28-byte body, identical across all 4 flows except 1 byte

```
60 14 eb 98 3f 00 60 14 00 00 00 00 XX 06 00 20 00 14 18 04 7c 01 00 00 18 94 ff ff
```
`XX` at byte offset 12 = `56`/`57`/`58`/`59` per flow (a per-flow immediate — the GDX/SOP selector,
consistent with `gdx_ctx_data=1026`; everything else identical because the 4 flows share egress and
differ only in the key). The trailing `08 80 00 21 b0 80 c1 88 b0 80 00 ...[162]` in the bdmf dump
is the **context-entry pad after the 28-byte cmdlist** (`cmd_list_data_length=28`), NOT cmdlist.

### Honest coverage of the live capture

| flow class | live? | validatable |
|---|---|---|
| locally-terminated L3 ucast (is_routed=0, gdx0) | YES (4 flows) | opcode/offset placement only |
| L2 bridge-accel (is_l2_accel, VLAN push/pop/mangle) | NO (l2_ucast nflows=0) | NOT covered |
| routed L3 + NAT/NAPT (is_routed=1, dec-TTL, replace, icsum) | NO (nflist empty) | NOT covered |
| tunnel (GRE/MAP-T) | NO | NOT covered |

The captured flows use **GDX local delivery** — a path our open driver does **not** implement (we
have L2-forward and L3-NAT paths, not a gdx/local-terminate path). So this is a *structural* /
*opcode-placement* validation, not a full byte-for-byte program match. A real port-to-port forward,
a VLAN bridge flow, or a LAN↔WAN NAT flow would be needed for a complete cmdlist match — and none
were live (generating that traffic is forbidden: could disturb the mgmt link).

---

## 2. HARNESS (dev-build) — drives the REAL builders

`~/be98/gt-be98-open-ethernet/harness/` on dev-build: compiles the actual
`driver/runner/cmdlist.c` emitter + the `xrdp_build_l2_cmdlist` / `xrdp_build_nat_cmdlist` /
`xrdp_build_ctx` / `xrdp_build_key` functions sliced verbatim out of `flow_offload.c`, with small
userspace shims for the kernel types/byteorder/`round_up`/`BIT`. It feeds the captured flow inputs
and prints our emitted bytes. (Harness lives only on dev-build; not committed.)

### Harness output (relevant excerpts)

```
CASE A  plain L2 forward (closest open analog to the live local-terminated flow)
  OUR cmdlist (4B):  fc 00 00 00          (just the NOP terminator + pad)
  OUR natc_key:      60cf843887b0 bc2411abdfd9 0800 0000   (MAC DA|SA|ethertype|...)

CASE B  VLAN push vid=30
  OUR cmdlist (20B): 4c30 0004 6030 1000 8100 6038 1000 001e fc00 0000

CASE C  routed SNAT <peer-ip>:53134 -> 1.2.3.4:40000  (Phase-2 NAT path)
  OUR cmdlist (28B): 6858 0800 6068 0004 01020304 d860 1000 6088 0002 9c40 d8c8 1000 fc00
  OUR natc_key:      0a000059 0a000008 cf8e08ae 06000001  (orig 5-tuple, BE)
```

---

## 3. BYTE-LEVEL DIFF: OUR builder vs STOCK silicon

The captured flow is GDX-local (no open analog), so a whole-program match is impossible. What CAN
be diffed is the **command-word framing** — and here the live bytes give a clean, decisive result.

### 3.1 OPCODE + OFFSET8 field placement — **CONFIRMED against silicon** ✅

The stock REPLACE command word is **`0x6014`** (appears twice in the body; `0x18` also appears as a
leading byte at offsets 18 and 24). Decoded under **our own bit-layout**
(`opcode = word>>26`, `offset8 = (word>>18)&0xff`, packed as two BE 16-bit halves so the
command-half is `(0x18<<10) | (offset<<2) | …`):

| stock half | our-layout decode | verdict |
|---|---|---|
| `0x6014` | opcode `0x18` = **REPLACE**, `offset8 = (0x14)>>2 = 5` | **MATCH** — both fields land exactly where our builder puts them |

i.e. REPLACE's `0x18` lands in the top byte as `0x60` in **both** our output and stock, and the
offset packs immediately below it. Our `xpe_pack_cmd()` opcode field (bits[31:26]) and offset8 field
(bits[25:18]) are **validated by real silicon**. This pins the high half of ABI UNKNOWN #3.

### 3.2 Operand sub-packing + NOP framing — **MISMATCH, still UNKNOWN** ⚠

| aspect | stock | ours | status |
|---|---|---|---|
| 16-bit REPLACE immediate | packed in the **trailing half of the same cmd word** (`…0x6014 0xeb98…`, `0xeb98` = data) | `replace_16` already does this (cmd half + data half); but `replace_bits_16` spends the low half on position/width | partial — `replace_16` shape OK, `replace_bits_16` differs |
| command/data interleave | command + inline-data half-words **interleaved** in one stream | same (cmd then data) | structurally similar |
| NOP / terminator | a `0x3f00` word appears (`0x3f<<8`); trailing `0xffff` decodes as NOP under `>>10` | we emit `0x3f<<10 = 0xfc00` | **MISMATCH / ambiguous** |
| position/width sub-fields | not separable from data without disasm | bits[17:13]/[12:8] | UNKNOWN |

**Why no further correction is safe:** the stock 28-byte body **interleaves command words and inline
data words**, so it cannot be unambiguously re-segmented into "command vs data" from the bytes alone
(e.g. `0x3f00` could be a NOP opcode-in-top-byte, or the trailing half of a preceding command's
data). The two facts that survive this ambiguity — the REPLACE opcode value and the offset8
placement — already MATCH our builder. Anything below that (the exact sub-offset operand math, the
position/width fields, whether NOP is `0x3f<<8` or `0x3f<<10`) needs the
`xpe_api.armb53_6813.o_saved` emitter disassembly (each `xpe_cmd_*`'s word composition), **not**
byte-staring at one GDX-local program.

### 3.3 Context entry + key

- **Context:** stock fields (`is_routed=0, is_l2_accel=0, is_hw_cso=1, cmd_list_data_length=28,
  cmd_list_length=40, tx_adjust=-10, vport=gdx0`) are printed **by name** by the bdmf examine, not
  as raw DDR bytes, so a byte-offset diff is impossible (ABI UNKNOWN #1 stands — needs rdpa.ko
  `_ucast_prepare_rdd_*` MWRITE-offset RE or the 6813 autogen header). Our two length fields
  (`cmd_list_data_length` + the larger `cmd_list_length`) match the stock entry's **two-length**
  structure — confirmed correct in concept. Our placeholder context byte offsets are NOT validated.
- **Key:** the stock NAT-C key is engine-internal (devmem-only = forbidden); the pktrunner
  `flows/L3` only exposes the FHW-Key↔RDPA-Key index map (`0x00100000+idx -> 0x0+idx`, re-confirmed
  this session), not the 16-byte masked key bytes. So our `xrdp_build_key` packing is **not**
  byte-validated against silicon — only its structure (16B, 4×u32, BE) is known-correct.

---

## 4. CORRECTIONS APPLIED TO THE DRIVER

Conservative — only what the live evidence proves:

- **`driver/runner/cmdlist.c`**: added a **LIVE-SILICON VALIDATION** block to the command-word
  encoding comment, recording that (a) the opcode field (bits[31:26]) and offset8 field
  (bits[25:18]) placement are **CONFIRMED** by the stock `0x6014` REPLACE word decoding cleanly to
  opcode 0x18 / offset 5, and (b) the sub-offset operand math + NOP framing remain UNKNOWN #3 and
  must be resolved via the `xpe_api.o` disasm, NOT by guessing from the interleaved bytes.

**No byte-emission code was changed.** The one thing the bytes prove (opcode + offset8 placement)
already matches our builder; every remaining difference is ambiguous from a single GDX-local program
and changing it on a guess would only break the proven driver↔QEMU-model contract that the M5
emulation relies on. This is the correct conservative outcome of a read-only validation.

### Build status

Driver module rebuilt **clean** on dev-build against `~/mainline` via `rtk`
(`make clean` then `rtk make … modules` → `EXIT=0`, `bcm4916-runner.ko` produced). The harness
(real builders) compiles and runs.

---

## 5. HONEST COVERAGE / REMAINING UNCERTAINTY

**What this validation PINS:**
- ✅ XPE opcode field placement (bits[31:26], REPLACE=0x18) — confirmed by stock `0x6014`.
- ✅ XPE offset8 field placement (bits[25:18]) — stock `0x6014` → offset 5.
- ✅ The context carries TWO length fields (data-length + aligned length) — our model matches.
- ✅ FHW-Key↔RDPA-Key 1:1 index map (re-confirmed).

**What is NOT covered (no live flow exercised it) — needs more traffic to validate:**
- ❌ **NAT/NAPT cmdlist** (`xrdp_build_nat_cmdlist`: decrement_8 / replace_32 / replace_16 /
  apply_icsum_16): nflist empty, no routed flow → a real **LAN↔WAN NAT flow** is required to
  validate the dec-TTL, IP/port replace, and incremental-checksum ops byte-for-byte.
- ❌ **VLAN push/pop/mangle cmdlist** (`xrdp_build_l2_cmdlist` VLAN paths): `l2_ucast nflows=0`,
  no bridge-accel flow → a real **tagged-VLAN bridge flow** is required.
- ❌ **The operand sub-packing below the opcode/offset (UNKNOWN #3 lower half)** and the **NOP
  framing**: not resolvable from the single interleaved GDX-local program → needs the
  `xpe_api.armb53_6813.o_saved` emitter disasm.
- ❌ **Exact context-entry byte offsets (UNKNOWN #1)** and **NAT-C key bytes**: bdmf prints by name;
  raw bytes need devmem (forbidden) or rdpa.ko writer RE.

**Bottom line:** the live capture cleanly **confirms our opcode + offset placement** (the high-value
half of UNKNOWN #3) and the two-length context structure, and surfaced **no builder bug** in the
fields it could reach. It cannot validate the NAT or VLAN programs (no such flow was live) or the
sub-operand math (needs disasm, not bytes). No guesses were applied; the driver still builds clean.
```
