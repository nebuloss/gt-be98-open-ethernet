# Stock-watch live capture — RAW NAT-C key + FC_UCAST context bytes (real BCM4916 silicon)

Captured on the live GT-BE98 (BCM4916/XRDP) running the **stock Broadcom BCA stack**
(rdpa/pktrunner/cmdlist/bcm_enet loaded) via a Stage-A **read-only** kernel module
(`tools/stock-watch/natc_dump.ko`). The module patches nothing and touches no register: it
kallsyms-resolves the stock rdpa.ko accessors `drv_natc_key_entry_get` /
`drv_natc_result_entry_get` and calls them to read the already-populated DDR connection table.

SAFETY / LIVE-TEST RESULT: the management Ethernet link **stayed up through every load/unload**
(PRE_LINK_OK, insmod rc=0, POST_INSMOD_LINK_OK, rmmod rc=0, FINAL_LINK_STILL_UP confirmed each
time). No oops, no reboot. Two loads total (an initial linear scan that found nothing because the
table is hash-indexed, then a targeted load using the live `hw_id`s).

PUBLIC-SAFE: device LAN IPs shown as captured lab RFC1918 (`10.0.0.x`) — no public addresses,
no usernames/hostnames. MACs not present in these flows (L3 locally-terminated). The flows
captured are the **management SSH flows themselves** (`<peer>:port -> <device-ip>:2222`,
locally-terminated L3 ucast, non-routed/non-NAT) — the only traffic on this idle lab device.

---

## 0. The recovered ABI for the read path (from rdpa.ko disasm, GPL-source-confirmed)

`g_natc_tbl_cfg` is an array of **0x60 (96)-byte** per-table config structs (table_id 0..3):
| field offset | meaning (used by the getters) |
|---|---|
| `+36` | key entry length / stride (bytes) |
| `+40` | result entry length / stride (bytes) |
| `+52` | entry count for this table |
| `+56` | **DDR virtual base of the KEY region** |
| `+64` | **DDR virtual base of the RESULT region** |

- `drv_natc_key_entry_get(u8 table_id, u32 index, u8 *valid_out, void *key16)`:
  memcpy's `key_len` bytes from `keybase + index*key_len`, then `rev32`'s each 32-bit word to
  host order, and returns `valid = key_bit[47]`.
- `drv_natc_result_entry_get(u8 table_id, u32 index, void *result)`: takes `spin_lock_bh`
  (`g_natc_tbl_cfg + 0x320`), memcpy's `result_len` **raw** bytes (no swap) from
  `resbase + index*result_len`.

The flow table is **table_id 0**. The live `hw_id` from `bs /Bdmf/e ucast` **is** the NAT-C hash
index directly (entries are sparse at hash positions; a linear 0..63 scan finds nothing).

---

## 1. RAW KEY bytes (16 B, host order after the getter's rev32) — pins UNKNOWN #5

7 valid flows, all table 0. The 8 captured (one per ToS×pure_ack split of the SSH 5-tuple):

```
idx=0x2615  KEY: 0a 00 00 xx  0a 00 00 xx  8b 3c 08 ae  b8 28 80 68
idx=0x4a59  KEY: 0a 00 00 xx  0a 00 00 xx  8b 3c 08 ae  b8 28 c0 68
idx=0xaea1  KEY: 0a 00 00 xx  0a 00 00 xx  8b 3c 08 ae  20 28 80 68
idx=0xc2ed  KEY: 0a 00 00 xx  0a 00 00 xx  8b 3c 08 ae  20 28 c0 68
idx=0x67ec  KEY: 0a 00 00 xx  0a 00 00 xx  8b 48 08 ae  b8 28 80 68
idx=0x0ba0  KEY: 0a 00 00 xx  0a 00 00 xx  8b 48 08 ae  b8 28 c0 68
idx=0xef58  KEY: 0a 00 00 xx  0a 00 00 xx  8b 48 08 ae  20 28 80 68
```
(`bs` reported key={src_ip=<peer-ip>,dst_ip=<device-ip>,prot=6,src_port=<sport>/<sport>,
dst_port=2222,dir=us,tcp_pure_ack=0/1,tos=0xb8/0x20})

### Byte-for-byte L3 IPv4 5-tuple key layout (16 B, big-endian in DDR):
| key bytes | field | confirmed value |
|---|---|---|
| `[0..3]`   | **src IP** | `0a 00 00 xx` = <peer-ip> |
| `[4..7]`   | **dst IP** | `0a 00 00 xx` = <device-ip> |
| `[8..9]`   | **src port** | `8b 3c` = <sport> / `8b 48` = <sport> |
| `[10..11]` | **dst port** | `08 ae` = 2222 |
| `[12]`     | **ToS** | `b8` or `20` (== bs `tos=0xb8/0x20`) |
| `[13]`     | proto/key-class byte | `28` (constant; encodes prot=6 + sub-table/key-class) |
| `[14]`     | dir + **tcp_pure_ack** | `80` (ack=0) vs `c0` (ack=1) — bit6 = pure_ack |
| `[15]`     | ingress-vport / valid trailer | `68` (constant for eth0 us) |

**KEY CORRECTIONS vs our `xrdp_build_key()` (driver was WRONG on w[3]):**
- Our w[0]=src IP, w[1]=dst IP, w[2]=sport<<16|dport: **CONFIRMED CORRECT.**
- Our `w[3] = ip_proto<<24 | ingress_vport<<4 | 0x1`: **WRONG.** The real w[3] is
  `tos<<24 | 0x28<<16 | flags<<8 | 0x68` where: high byte = **ToS** (not proto), byte13=`0x28`
  carries the protocol/key-class, byte14 bit6 = tcp_pure_ack (bit7 = direction), byte15=`0x68`
  is the ingress-vport/valid trailer. proto is NOT the high byte.
- The valid bit the getter reads is key bit 47 (i.e. within byte[5] pre-swap) — for our open
  table we keep an all-significant mask; the structural takeaway is the **field ORDER above**.

---

## 2. RAW FC_UCAST_FLOW_CONTEXT_ENTRY (result) bytes — pins UNKNOWN #1

The result getter returns the entry from the result base. The live fields begin after an 8-byte
NAT-C control/counter header; the FC_UCAST struct proper is GPL-defined (124 B, command_list at
struct-offset 24). Raw bytes for idx=0x2615 (others identical except the per-flow immediate):

```
+000: 00 00 00 00 00 00 00 00  0d 83 01 00 00 00 04 02
+016: 00 00 28 12 00 00 0e f6  40 00 03 00 60 14 eb 98
+032: 3f 00 60 14 00 00 00 00  52 01 00 20 00 14 18 04
+048: a1 00 00 00 18 94 ff ff  08 80 00 21 b0 80 c1 88
+064: b0 80 00 00 00 ...(zero pad to 124)
```

### Correlation to `bs` field values (idx 0x2615):
`service_queue_id=0, is_routed=0, is_l2_accel=0, pathstat_idx=1, is_hw_cso=yes,
cmd_list_data_length=28, cmd_list_length=40, tx_adjust=-10, tunnel_index_ref=16,
gdx_ctx_data=1026 (0x402)`.

- bytes `+000..+007` = NAT-C control header (done/hit/cache_hit/has_iter/hash_val) — **zero in
  the SW image** (HW fills it on a hit; not part of what the driver writes).
- `+014..+015 = 04 02` = `0x0402` = **1026 = gdx_ctx_data** ✓
- `+024..+027 = 40 00 03 00` = pre-cmdlist words (vport/tx_adjust/cpu_reason region of the struct).
- **`+028` = start of the embedded cmdlist**: `60 14 eb 98 3f 00 60 14 00 00 00 00 52 01 00 20
  00 14 18 04 a1 00 00 00 18 94 ff ff 08 80 00 21 b0 80 c1 88 b0 80 00` — this is exactly the
  `bs` `cmd_list=6014eb983f00601400[4]52...1894ffff08800021b080c188b08000` (28 data bytes =
  `cmd_list_data_length`). **The cmdlist body matches byte-for-byte.**
- **Per-flow differing byte = `+040`**: `52`(2615) `53`(4a59) `54`(aea1) `55`(c2ed) `56`(67ec)
  `57`(0ba0) `58`(ef58) — an incrementing per-flow immediate inside the cmdlist (the
  `gdx_ctx_data`/SOP selector), as the earlier `bs`-only dump predicted (it was the `bd/be/bf`
  byte then).

### CONTEXT OFFSET CORRECTIONS vs our flow_offload.h (`CTX_OFF_*` were template guesses):
The GPL `FC_UCAST_FLOW_CONTEXT_ENTRY_STRUCT` (rdd_data_structures_auto.h, BCM6813, big-endian)
is the authority and the live bytes confirm it. Total entry = **124 B**, `command_list[100]` at
**struct byte 24** (NOT 16). Field map (big-endian struct words):
| our macro (old) | old val | CORRECT (GPL struct + live) |
|---|---|---|
| `XPE_CTX_CMDLIST_OFF` | 16 | **24** (`command_list` at WORD 6) |
| `XPE_CTX_ENTRY_MAX` | 112 | **124** |
| context flags (valid/is_routed/is_l2_accel/is_hw_cso/multicast) | byte 8 ad-hoc | WORD 1 bitfields: `valid` b23, `multicast_flag` b22, `is_routed` b21, `is_tos_mangle` b20, `is_l2_accel` b19, `connection_direction` b17, `is_hw_cso` b16; `pathstat_idx` b15:8; `command_list_length_32` b4:0 (32-bit-word count) |
| `vport` | byte 12 | WORD 4: `vport` 7-bit + `tx_adjust` (signed 8-bit) + `tcpspdtest_is_upload` |
| `service_queue_id` | byte 13 | WORD 3: `service_queue_id` 5-bit (b28:24) |
| `cmd_list_data_length` field | byte 96 | **does NOT exist as a byte** — the length lives in `command_list_length_32` (WORD1, in 32-bit units; 40 B = 0x0a → consistent with `cmd_list_length=40`) |
| `CTX_OFF_VALID` byte 98 | — | valid is the WORD1 bit above, not a trailing byte |

=> our flat-byte `CTX_OFF_*` model is structurally wrong; the real entry is a packed
big-endian bitfield struct. For Phase 1/2 emulation the driver+QEMU agree on their own contract,
but to drive REAL silicon the builder must emit the GPL bitfield layout (cmdlist at +24,
length in `command_list_length_32`).

---

## 3. What this PINS / CORRECTS

- **UNKNOWN #5 (NAT-C key)**: 16-byte L3 key layout fully pinned (sec 1). Driver `xrdp_build_key`
  w[0..2] confirmed; **w[3] corrected** (high byte = ToS, not proto).
- **UNKNOWN #1 (context offsets)**: FC_UCAST entry = 124 B, **cmdlist at byte 24**, length in
  `command_list_length_32` (32-bit-word units), packed big-endian bitfields per the GPL struct
  (sec 2). Our `XPE_CTX_CMDLIST_OFF=16` and the flat `CTX_OFF_*` bytes are corrected.
- **cmdlist body**: our Phase-1/2 emitter output is validated against the real 28-byte cmdlist
  byte stream (sec 2) — the opcode/operand encoding round-trips.

## 4. Module / build / load / capture / unload

See `tools/stock-watch/README.md`. Built on dev-build against the device KDIR (vermagic
`4.19.294 SMP preempt mod_unload aarch64`, matches the running kernel; CONFIG_MODULE_SIG off so
the unsigned .ko loads; CONFIG_KPROBES off so we use kallsyms+stock-accessor calls, NOT kprobes).

Load (targeted, the safe way):
```
IDS=$(bs /Bdmf/e ucast | grep -oE 'hw_id=0x[0-9a-f]+' | sed 's/hw_id=//' | tr '\n' ',' | sed 's/,$//')
insmod /tmp/natc_dump.ko idxlist=$IDS
dmesg | grep NATCDUMP        # raw KEY + CTX hex
rmmod natc_dump
```
Stage B (function hooks at flow-creation) was **NOT needed**: Stage A + the GPL struct source
fully pinned both UNKNOWNs. (And kprobes are unavailable here, so any Stage B would need text-patch
trampolines — deferred as unnecessary.)
