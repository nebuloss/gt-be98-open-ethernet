# 03 — FC_UCAST_FLOW_CONTEXT_ENTRY ABI (the NAT-C "result")

Clean-room, interoperability RE of the closed BCM6813 XRDP flow-offload result
struct. Facts derived by disassembling the (unstripped) stock `rdpa.ko`; all raw
disassembly stays in `~/re-scratch/fc-ucast/` on the build machine. No verbatim
proprietary listings are reproduced here — only offsets, bit positions, and
encodings, expressed in our own words.

## Purpose

The flow lookup (NAT-C) returns a **result / context entry** —
`FC_UCAST_FLOW_CONTEXT_ENTRY` — that the Runner executes to forward + rewrite a
packet. The open driver currently stages this entry as a **flat-byte
driver↔QEMU-model contract** (`driver/runner/flow_offload.h` `CTX_OFF_*`,
`driver/runner/flow_offload.c:158` `xrdp_build_ctx`) and openly admits the real
6813 byte/bit layout is unpinned (`flow_offload.h:23` "ABI UNKNOWN #1";
`flow_offload.h:95` "a real-silicon builder must instead emit the GPL bitfield
struct").

This note pins the **real packed-bitfield layout**: which 32-bit word and which
bit range each field occupies, the `command_list_length_32` units, and the
`+24` / `124-byte` structure. It closes audit gap **M1a / U1**
(`docs/audit/10-reimplementation-guide.md`) and supplies the real-silicon
context contract that `docs/audit/09-hardware-acceleration.md §6` flags as
missing. It is the result-side companion to `01-natc-abi.md` (the key) and
`02-cmdlist-abi.md` (the command_list body that this entry embeds).

## Method

Binary (unstripped, 12 349 syms), on the build machine, read-only:

```
rdpa.ko = .../rdp/projects/BCM6813/target/rdpa/rdpa.ko   (elf64-littleaarch64)
```

Disassembled one function at a time with
`aarch64-linux-gnu-objdump -dr --start-address=… --stop-address=…` (relocations
kept). Symbol boundaries from `nm rdpa.ko | sort`. The result struct is written
**field-by-field** by these builders; the store offset + `bfi`/`bfxil`/`bfxil`
shift-mask sequence *is* the bitfield layout:

| symbol | addr | role (dst = result entry) |
|---|---|---|
| `ucast_prepare_rdd_ip_flow_result`            | 0x36d30 | **routed / L3+NAT** result builder (dst = x19) |
| `l2_ucast_prepare_rdd_ip_flow_result.part.0`  | 0x32370 | **L2-accel / bridge** result builder (dst = x19) |
| `l2_ucast_prepare_rdd_ip_flow_result`         | 0x33aa0 | thunk → `.part.0` / flood-master variant |
| `_ucast_prepare_rdd_ip_flow_tunnel_result`    | 0x36a90 | tunnel (GRE/DS-lite/L2TP…) overlay, dst bytes 24..47 + word@40/44 |
| `_ucast_prepare_service_queue_params`         | 0x39460 | service-queue overlay (service_queue_id + enable) |
| `_ucast_prepare_rdd_ip_flow_key_params`       | 0x37cd0 | key copier (5-tuple → key struct; not the result) |
| `_l2_ucast_prepare_rdd_flow_params`           | 0x341c0 | dispatcher → key_params + result.part.0 |
| `_l2_ucast_prepare_rdd_key_flow_params`       | 0x33af0 | L2 key builder (not the result) |

Note: the closed BCM6813 GPL RDD auto-headers
(`rdd_data_structures_auto.h`, `FC_UCAST_FLOW_CONTEXT_ENTRY_STRUCT`) are **not
present** in the 4916 GPL SDK checkout (RDD is proprietary — consistent with the
project memory). Field *positions* below are hard (they are the compiler-emitted
`bfi` operands); field *names* are inferred from behaviour and cross-checked
against the live capture (`re-notes/stock-watch-capture.md`) and the open driver
— confidence is stated per row.

### Endianness convention used below

The B53 host is little-endian. Each builder reads/modifies/writes the entry as
native 32-bit words (`ldr`/`str [x19,#W]`); a bit position `b` inside `WORD@W`
lands in host **byte `W + b/8`, bit `b%8`**. Only the embedded `command_list`
region is additionally `rev32`-byteswapped (see below). "`WORD@W bits[hi:lo]`"
is therefore the reproducible, endianness-neutral way to name a field, and maps
directly to the GPL bitfield-struct a real-HW builder must emit.

## ABI reference

### 1. Structure envelope (high confidence)

| fact | value | evidence |
|---|---|---|
| command_list body offset | **struct byte +24 (0x18)** | tunnel builder `add x3,x0,#0x18 … add x6,x0,#0x30` rev32 loop @0x36b14; `.part.0` `add x0,x19,#0x18 … add x3,x19,#0x30` @0x32480 |
| command_list byteswap | each 32-bit word `rev32` on store | `ldr;rev32;str [x0],#4` loops @0x36b58, @0x324a4 |
| command_list region size | 100 bytes (bytes 24..123) | 24-byte header + total 124 ⇒ 100 |
| total entry size | **124 bytes** | consistent: 24-B header + 100-B command_list (driver-pinned; disasm shows header ≤ byte 47 + command_list@24) |

### 2. command_list_length_32 — units (high confidence)

`.part.0` @0x3254c:

```
ldrb w0,[x20,#110]   ; source command_list length, in BYTES
add  x0,x0,#0x18     ; + 24  (the header size)
lsr  x0,#2           ; / 4   (32-bit words)
strb w0,[x19,#7]     ; -> entry byte 7  (= WORD@4 bits[31:24])
```

So **`command_list_length_32` = (24 + command_list_bytes) / 4** — the length of
the **whole entry** (24-byte header **plus** command_list) counted in **32-bit
words**, stored in **entry byte 7 (top byte of WORD@4 / "word 1")**. This
confirms *both* that the field is word-units *and* the +24 header. Worked
example matching the live capture: a 16-byte command_list ⇒ (24+16)/4 = **10 =
0x0a**, i.e. a 40-byte entry — exactly the `cmd_list_length=40 → 0x0a` seen in
`stock-watch-capture.md §2`.

### 3. Bitfield map of the result entry

Fields written by the two main result builders. `src` = offset in the rdpa
result-info struct (arg passed to the builder); the destination is the entry.
Confidence: **H**igh position, name inferred at H/M/L.

| entry field (WORD@byte bits) | width | routed src | L2 src | inferred meaning | conf |
|---|---|---|---|---|---|
| WORD@4 bit23 | 1 | =1 const | =1 const | **valid** (set unconditionally) | H |
| WORD@4 bits[31:24] (byte 7) | 8 | — | (24+len)/4 | **command_list_length_32** | H |
| WORD@4 bit21 | 1 | +24 | +81 | class/fwd flag (present both paths) | M |
| WORD@4 bit20 | 1 | — | +100 | flag | M |
| WORD@4 bit19 | 1 | +25 | +82 | flag | M |
| WORD@4 bits[7:5] | 3 | +27 | — | 3-bit class/action code | L |
| WORD@12 bits[13:0] | 14 | +50 | +98 | 14-bit metadata/fwd descriptor | L |
| WORD@12 bits[28:24] | 5 | +12 | +68 | egress **port / service-queue** id (see §4) | M |
| WORD@12 bit23 | 1 | +48 | +96 | **is_tunnel / DF gate** (`tbnz #23` → tunnel path) | M |
| WORD@12 bit21 | 1 | +33 | — | flag | L |
| WORD@12 bit19 | 1 | +34 | +87 | flag | L |
| WORD@12 bit18 | 1 | — | +80 | flag | L |
| WORD@16 bit15 | 1 | +28 | — | flag | L |
| WORD@16 bits[14:8] | 7 | proto-derived (+0x10/+0x15/+0xb) | same | L4/next-hdr protocol class | M |
| WORD@22 bits[14:10] | 5 | +328 | +328 | **policer_id** (+ enable elsewhere) | M |
| WORD@22 bits[5:0] | 6 | +60 | +108 | 6-bit id/index | L |
| byte@5 | 8 | +61 | +109 | **egress vport** (full byte) | M |
| byte@6 bit1 | 1 | — | +36 | flag | L |
| byte@21 bit7 | 1 | +29 | +83 | flag | L |
| byte@21 bit6 | 1 | +26 | — | flag | L |
| byte@21 bit5 | 1 | +62≠0 | — | flag | L |
| WORD@8 bit19 | 1 | — | +86 | flag | L |
| WORD@11 bit26 (byte 14 bit2) | 1 | +336 | — | **is_hw_cso** candidate | L |

### 4. service-queue overlay (`_ucast_prepare_service_queue_params` @0x39460)

Runs after the base builder and (re)writes the queue fields:

| entry field | width | src | meaning | conf |
|---|---|---|---|---|
| WORD@12 bits[28:24] | 5 | +92 | **service_queue_id** | M |
| byte@21 bit4 | 1 | +93 | **service_queue enable** | M |

So the 5-bit field at `WORD@12[28:24]` is the shared **egress port / service
queue** selector (base builder seeds it from the flow's egress; the service-queue
stage overrides it when a service queue is bound), and the *enable* bit sits at
`byte@21 bit4`.

### 5. tunnel overlay (`_ucast_prepare_rdd_ip_flow_tunnel_result` @0x36a90)

Writes the tunnel descriptor into the same 24..47 region later covered by the
command_list header words:

| entry field | width | encoding | meaning | conf |
|---|---|---|---|---|
| WORD@44 bits[27:24] | 4 | enum: 1,2,3,4 (per tunnel type src+305) | **tunnel type** | M |
| WORD@44 bit13 | 1 | src+329 | tunnel flag | L |
| WORD@44 bit12 | 1 | set/cleared per type | tunnel enable | L |
| WORD@40 (byte 40..43) | 32 | src+324 | **tunnel_index_ref / descriptor** | M |
| byte@46 | 8 | src+306 + 2 | tunnel header length | L |
| byte@44 | 8 | src+307+2 (0xff = none) | tunnel L2 hdr length / sentinel | L |
| entry bytes 24..39 | 16 | `ldp/stp` copy of src+0x134, then `rev32` | inline tunnel (GRE/etc) header | M |

## Mapping to the open driver

Driver refs: `driver/runner/flow_offload.h` (the `CTX_OFF_*` contract) and
`driver/runner/flow_offload.c:158` `xrdp_build_ctx`.

| RE'd fact | driver placeholder (file:line) | status | conf |
|---|---|---|---|
| command_list body @ struct **+24** | `flow_offload.h:97` `XPE_CTX_CMDLIST_OFF 24` | **confirms** | H |
| total entry **124 B**, command_list region 100 B | `flow_offload.h:98` `XPE_CTX_ENTRY_MAX 124` | **confirms** (driver's `XPE_CMDLIST_MAX_BYTES=80` is short of the real 100) | H |
| `command_list_length_32` = **(24+bytes)/4** words @ **byte 7** | `flow_offload.c:196-198` two trailing byte counts (`CTX_OFF_CMDLIST_DLEN/LEN`, "real HW: clen/4") | **corrects** — one field, in WORD 1 byte 7, units include the 24-B header (driver's `clen/4` omits the header and the position) | H |
| **valid** = `WORD@4 bit23`, set unconditionally | `flow_offload.h:119` `CTX_OFF_VALID` (= 24+80+2 = 106, a trailing byte) | **corrects** — valid is a bitfield in word 1, not a trailing byte; the driver's flat offset does not exist in the real struct | H |
| flags (routed/l2_accel/mcast/tunnel) are bitfields spread over WORD@4 / WORD@12; is_routed vs is_l2_accel = *which builder runs* | `flow_offload.h:107-111` `CTX_OFF_FLAGS 8` (single byte, bits 7/5/4/3) | **corrects** — no single flags byte@8; class encoded structurally + WORD@12 bit23 tunnel gate | M |
| egress **vport** = `byte@5` (8-bit) and/or `WORD@12[28:24]` port field | `flow_offload.h:112` `CTX_OFF_VPORT 12` | **corrects / newly-pins** | M |
| **service_queue_id** = `WORD@12[28:24]`, enable = `byte@21 bit4` | `flow_offload.h:113` `CTX_OFF_SERVICE_Q 13` | **corrects** | M |
| **is_hw_cso** ≈ `byte@14` region (`WORD@11 bit26` = byte14 bit2) | `flow_offload.h:114` `CTX_OFF_IS_HW_CSO 14` (bit0) | **partial-confirm** — right byte, bit unconfirmed | L |
| **policer_id** = `WORD@22 bits[14:10]` (5-bit) | (no driver field yet) | **newly-pins** | M |
| **tunnel_index_ref** = `WORD@40` + tunnel-type `WORD@44[27:24]` | `flow_offload.h` `struct xrdp_flow` has no tunnel path | **newly-pins** | M |

Net for a real-silicon `xrdp_build_ctx`: emit a 124-byte packed struct; set
`valid` (WORD@4 bit23); write `command_list_length_32 = (24 + clen)/4` into byte
7; copy the command_list to +24 **byte-swapped per 32-bit word** (`rev32`); place
service_queue_id/policer/port in the WORD@12 / WORD@22 bitfields above rather
than in the flat `CTX_OFF_*` bytes.

## Unresolved

- **Exact field names** — the GPL `FC_UCAST_FLOW_CONTEXT_ENTRY_STRUCT` header is
  absent from the 4916 GPL SDK, so names for the many 1-bit flags (WORD@4
  bit19/20/21, WORD@12 bit18/19/21, byte@21 bits) are inferred, not read. Their
  *positions* are certain.
- **is_routed vs is_l2_accel bit** — appears to be encoded by *which* builder
  runs (routed 0x36d30 vs L2 0x32370) rather than a single discriminator bit;
  whether the entry also carries an explicit `is_l2_accel` flag bit was not
  isolated (candidate: WORD@4 bit21, set from a source bool in both paths).
- **is_hw_cso bit** — a cso-like 1-bit field lands in the byte-14 word
  (`WORD@11 bit26`), matching the driver's byte offset but not its bit (0 vs 2);
  low confidence pending a cso-on capture.
- **The 14-bit WORD@12[13:0] and 7-bit WORD@16[14:8] descriptors** — decoded as
  metadata / protocol-class from the proto-dependent `add #0x10/#0x15/#0xb`
  arithmetic; the precise sub-encoding (which bits = ip_ver / L4-proto / trap)
  was not fully resolved.
- **command_list region 100 vs driver 80** — the real region is 100 bytes
  (124−24); the driver caps its buffer at `XPE_CMDLIST_MAX_BYTES=80`. Whether
  the stock builder ever emits >80 bytes for a NAT+VLAN flow is not settled here.

## Sources

- `rdpa.ko` (BCM6813, unstripped) — functions & addresses in §Method.
- Raw disasm: `~/re-scratch/fc-ucast/{ucast_result,l2_result_part0,tunnel_result,service_q,l2_result,l2_flow_params,ucast_key_params}.txt` on the build machine.
- Cross-refs: `driver/runner/flow_offload.{h,c}`; `re-notes/stock-watch-capture.md §2` (live 124-B entry, cmdlist@+24, cmd_list_length→word units); `re-notes/re-firmware/01-natc-abi.md` (key), `02-cmdlist-abi.md` (command_list body).
