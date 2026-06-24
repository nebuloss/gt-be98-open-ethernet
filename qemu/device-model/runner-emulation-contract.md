# BCM4916 XRDP "Runner" — QEMU model emulation contract

This is the **precise contract** the QEMU Runner device model must implement so
the open driver `driver/runner/bcm4916_runner.c` works against it unmodified.
It is the emulator-facing counterpart of `re-notes/xrdp-datapath-abi.md` and
matches exactly what the driver reads/writes today.

The driver runs in **emulated mode** (`brcm,runner-emulated` DT prop /
`runner_emulated=1`): **no Runner microcode is loaded**. The QEMU model *is* the
Runner — it fakes the cores' behaviour. The model therefore does NOT need to
emulate RNR_INST/RNR_PRED microcode execution; it only needs the FPM pool, the
ring memory protocol, and the IRQ.

All multi-byte descriptor fields are **big-endian in guest memory** (the Runner
is BE; the ARM guest is LE and byte-swaps). The model must read/write them BE.

---

## 1. Address map

One QOM sysbus device mapped at the rdpa window:

| | value |
|---|---|
| base | `0x82000000` |
| size | `0x00caf004` |
| DT compatible | `brcm,bcm4916-runner` |

The driver derives sub-block bases by **offset from this single base**. The
model only needs to decode the offsets the driver actually touches (below); all
other offsets may read-as-zero / write-ignore.

### Register windows the driver touches

| Block | Window offset (from base) | Used by driver for |
|---|---|---|
| **PSRAM** | `0x000000` | ring-config publish (CPU_RING_DESCRIPTOR) |
| **RNR_MEM[0..7]** | `0x700000 + n*0x20000` | TX index "doorbell" (write_idx) |
| **FPM** | `0xa00000` | pool cfg + token alloc/free |

---

## 2. FPM block (offset `0xa00000`)

The model maintains a free pool of buffer tokens. The guest reserves the pool
DDR itself (dma_alloc_coherent) and tells the model its base via FPM_POOL1_CFG2.

### 2.1 Registers (offsets within the FPM block)

| Offset | Name | On WRITE | On READ |
|---|---|---|---|
| `0x0000` | `FPM_CTL` | bit4 `INIT_MEM`: start pool memory init; bit16 `POOL1_ENABLE`: enable pool; bit14 soft-reset. | return last value but **with bit4 (`INIT_MEM`) cleared** (init completes instantly in the model). |
| `0x0040` | `POOL1_CFG1` | bits[26:24] chunk-size select: `0`=512B, `1`=256B. Store as `chunk_size`. | last value |
| `0x0044` | `POOL1_CFG2` | pool DDR base (low 32 bits, masked `0xfffffffc`). Store as `pool_pbase`. | last value |
| `0x0054` | `POOL1_STAT2` | (ignore) | bits[17:0] = number of tokens currently available |
| `0x00c4` | `FPM_SPARE` | head/tail pad scratch (store, otherwise inert) | last value |
| `0x0400` | `POOL0_ALLOC_DEALLOC` | **FREE**: the written u32 is a token; mark its index free. | **ALLOC**: return a fresh token (see 2.2) or 0 if exhausted. |

The driver's `INIT_MEM` poll loop reads `FPM_CTL` until bit4 clears — the model
clearing it on read (or immediately after the write) satisfies this.

### 2.2 Token format (bit-exact; ABI 3.1)

A token is a u32:

```
bit  31      : VALID (always 1 for an allocated token)
bits 30..29  : pool   (0 for pool0)
bits 28..12  : index  (17-bit chunk index within the pool)
bits 11..0   : size   (bytes; the requested allocation size)
```

`ALLOC` (read of `0x0400`): pick a free chunk index `i`, return
`VALID | (0<<29) | (i<<12) | (size & 0xfff)`. The model may ignore the size
request and just hand out one chunk (chunk_size, default 512). Return `0`
(VALID clear) when the pool is empty — the driver treats that as TX back-pressure.

`FREE` (write of `0x0400`): extract `index = (token>>12)&0x1ffff`, mark it free.

**Buffer address math (must agree with the guest):**
`buffer_phys = pool_pbase + index * chunk_size`.
The guest computes the same; the model uses it to know *where in guest DDR* a TX
token's payload lives (see TX path).

The model should size the pool from `POOL1_CFG2`/the reserved region; the driver
asks for a 32 MB / 512 B-chunk pool (≈65536 tokens).

---

## 3. CPU RX data ring

### 3.1 Ring discovery (PSRAM CPU_RING_DESCRIPTOR)

On bring-up the driver `memcpy_toio`'s a 16-byte **CPU_RING_DESCRIPTOR** into
PSRAM at offset `PSRAM_CPU_RING_DESC_TABLE = 0x0000` (contract offset; the real
RDD offset is TBD — driver and model share this constant). Layout, big-endian
(ABI 2.1):

| bytes | field |
|---|---|
| `w0` (0..3) | `[31:27]`=size_of_entry (log2 bytes; 4 for 16B), `[26:16]`=number_of_entries (ring depth), `[15:0]`=interrupt_id |
| `w1` (4..7) | `[31:16]`=drop_counter, `[15:0]`=write_idx |
| `w2` (8..11) | base_addr_low (ring DDR phys, low 32) |
| `w3` (12..15) | `[15:0]`=read_idx, `[7:0]`=base_addr_high (bits 39..32 of ring phys) |

From this the model learns: **ring phys base** = `(base_addr_high<<32)|base_addr_low`,
**depth** = number_of_entries, **entry size** = 16 B. The driver uses depth 256.

### 3.2 Per-descriptor protocol (16 B big-endian — BCM6813/XRDP CPU_RX_DESCRIPTOR)

Each ring entry (`struct runner_rx_desc`), abs-address mode. Layout is the 6813
GPL `CPU_RX_DESCRIPTOR` (XRDP_CFE2 `rdp_cpu_ring_defs.h`), validated against live
silicon. **This replaces the old 416L05 layout (word0=len, word2=ptr).** Fields
below are the HOST (post-swap, little-endian word) view; in DDR each word is BE.

| word | bytes | field (host LE-word view) |
|---|---|---|
| word0 | 0..3 | `host_buffer_data_ptr_low[31:0]` = packet DDR phys, low 32 |
| word1 | 4..7 | `[1]`=is_chksum_verified, `[15:2]`=packet_length, `[16]`=abs, `[31:24]`=ptr_hi (phys[39:32]) |
| word2 | 8..11 | `[5:0]`=reason, `[12:6]`=data_offset, `[31]`=is_src_lan; LAN: `[29:25]`=source_port; CPU/WLAN: `[29:25]`=vport, `[24:21]`=ssid |
| word3 | 12..15 | `[15:0]`=wl_metadata/dst_ssid_vector, `[29]`=is_ucast, `[30]`=is_rx_offload, `[31]`=is_exception |

**Initial state (set by guest in `create_ring` / re-arm):** word0 =
`BE(buffer_phys & 0xffffffff)`, word1 = `BE(0x00010000 | (ptr_hi<<24))`
(abs bit set, **packet_length = 0**), word2 = word3 = 0. packet_length==0 marks
the slot **RUNNER-owned / empty**. So at start the model owns all slots and each
carries a guest-provided abs buffer phys in word0.

### 3.3 RX injection (model receives a frame from its host netdev backend)

When a frame arrives on the model's backend netdev, for the slot the model is
about to fill (it tracks its own producer index; start at 0, wrap at depth):

1. Read **word0** (BE) → `buf_phys_low = word0`; read **word1** ptr_hi
   (`[31:24]`) for the high address bits. The slot must be empty
   (word1 packet_length == 0); if non-zero the host hasn't consumed it yet →
   ring full, drop or back-pressure.
2. **DMA the frame bytes into guest physical memory at `buf_phys`** (length =
   frame length, ≤ buffer size 2048).
3. Write **word2** (BE) for the source: LAN frame =>
   `(src_port<<25) | 0x80000000` (is_src_lan=1); CPU/WLAN => leave is_src_lan=0
   and set `(vport<<25)|(ssid<<21)`. reason/data_offset may stay 0.
4. Write **word3** (BE) flags as needed (`is_ucast`/`is_exception`); 0 is fine
   for a plain LAN unicast pipe.
5. Write **word1** (BE) = `(packet_length<<2) | 0x00010000 | (ptr_hi<<24)` —
   i.e. set **packet_length** (and keep abs). This publish must be the **last**
   write and ordered after the payload + word0/word2/word3 writes; a non-zero
   length is what the guest polls on.
6. Advance the model's producer index (wrap at depth).
7. **Raise the RX IRQ** (see §5). RX has **no host-visible doorbell**; the IRQ
   only wakes the guest's NAPI poll, which then polls word1 length in DDR.

### 3.4 Host consume + re-arm (what the guest does — informational)

The guest NAPI poll reads word1 (BE-swapped), checks `packet_length != 0`; if
set, copies `packet_length` bytes out of the buffer at the word0/word1 abs phys,
then **re-arms in place**: rewrites word0 = `BE(buf_phys)`, word1 =
`BE(0x00010000 | (ptr_hi<<24))` (abs set, length cleared → RUNNER), word2=word3=0,
using the **same buffer**, and advances its consumer index. The model must treat
a slot whose length goes back to 0 as available again.

---

## 4. CPU TX ring

### 4.1 Ring discovery

The driver publishes a second CPU_RING_DESCRIPTOR (same 16-B layout as §3.1)
into PSRAM at `PSRAM_CPU_TX_RING_DESC_TABLE = 0x0080` (contract offset). From it
the model learns the TX ring phys base + depth (256) + entry size (16 B). The
**TX descriptors live in guest DDR at that base** (the driver stages them with
dma_alloc_coherent and the model reads them from guest memory).

### 4.2 TX descriptor (16 B big-endian, ABI 2.2)

`struct runner_tx_desc`, the fields the model must honour:

| word | field (host LE-word view, BE in memory) |
|---|---|
| word0 | `[31]`=is_egress (1), `[21:8]`=packet_length, `[7:0]`=sk_buf_ptr_high |
| word1 | (sk_buf ptr / 1588 — unused in FPM mode) |
| word2 | `[31]`=color, `[30]`=do_not_recycle, `[29]`=flag_1588, `[28]`=is_emac, `[27:20]`=wan_flow/egress source_port, `[16]`=abs (0 ⇒ FPM-token mode), `[13:10]`=ssid, `[7:0]`=pkt_buf_ptr_high |
| word3 | abs=0: **FPM token** = `fpm_bn0[19:0] | fpm_sop[29:20]`; abs=1: pkt_buf_ptr_low |

(Source: 6813 GPL `RING_CPU_TX_DESCRIPTOR`, `rdd_data_structures_auto.h`. NOTE the
abs bit is **word2 bit16**, not bit8 as in the old 416L05-derived note.)

The driver always uses **abs=0 (FPM-token mode)**: word3 is an FPM token. The
model computes the payload location via the token math (§2.2):
`buf_phys = pool_pbase + ((token>>12)&0x1ffff) * chunk_size`, and reads
`packet_length` bytes of payload from guest DDR there.

### 4.3 TX doorbell (the index write — ABI 5bis-G2)

There is **no MMIO TX doorbell**. After staging a descriptor the driver:

1. advances its local `write_idx` (mod depth), then
2. for each Runner core `n` (0..7) writes the new `write_idx` as a **big-endian
   u16** into `RNR_MEM[n]` at offset `CPU_TX_RING_INDICES_OFF = 0x0000` (i.e.
   guest writes to `base + 0x700000 + n*0x20000 + 0x0000`).

The model must **trap the u16 write to RNR_MEM[n]+0x0 as the TX kick**: on that
write, compare the new `write_idx` (BE-swapped) against the model's TX consumer
index and, for each newly-produced slot:

1. read the TX descriptor from the TX ring in guest DDR (BE),
2. resolve the payload via the word3 FPM token (§4.2),
3. **emit the frame out the model's host netdev backend**,
4. **free the FPM token** back to the pool (mark its index free) unless
   `do_not_recycle` is set (the driver does not set it),
5. advance the model's TX consumer index.

The driver writes the index to **all 8 cores**; the model can act on the write
to **any** core (e.g. core 0) and ignore the rest, or de-dup by tracking a
single consumer index. A `dma_wmb()` precedes the index writes; the model sees a
fully-written descriptor by the time the index advances.

No TX completion IRQ is required (the driver consumes the skb immediately).

---

## 5. Interrupts

GIC SPIs (level-high), from the DT:

| name | SPI | meaning |
|---|---|---|
| `queue0` | 75 | RX data-ring queue 0 — raise after an RX injection (§3.3) |
| `fpm` | 107 | FPM refill/pool event — optional; not required for first light |

The driver maps `queue0` and, on receiving it, **disables the IRQ and schedules
NAPI**; NAPI re-enables the IRQ when the ring drains. So the model should:

- raise `queue0` (level) when it injects RX frame(s) and the line is enabled;
- it is fine to coalesce (raise once for a burst);
- the guest clears the condition by draining the ring (ownership poll), not by
  an MMIO ack — so the model should **deassert** the level when the producer and
  consumer indices meet (ring empty) or simply pulse and rely on NAPI. A simple
  correct behaviour: keep `queue0` asserted while `producer != consumer`
  (unconsumed RX slots exist), deassert when caught up.

If the model raises no IRQ at all, the driver still works in **poll fallback**
(it schedules NAPI on open when no IRQ is wired) — useful for an initial model.

---

## 6. Bring-up writes the model must accept (in order)

Replaying `runner_probe`:

1. **FPM**: `POOL1_CFG1` (chunk size), `POOL1_CFG2` (pool base), `FPM_SPARE`,
   `FPM_CTL |= INIT_MEM` (then poll until cleared), `FPM_CTL |= POOL1_ENABLE`.
2. (microcode load — **skipped** in emulated mode; model needs nothing here.)
3. **RX ring publish**: 16-B CPU_RING_DESCRIPTOR `memcpy_toio` to
   `PSRAM + 0x0000`.
4. **TX ring publish**: 16-B CPU_RING_DESCRIPTOR `memcpy_toio` to
   `PSRAM + 0x0080`.
5. Steady state: FPM `0x0400` reads (alloc) / writes (free); RNR_MEM[n]+0x0
   u16 writes (TX kick); RX descriptor word2/word0 reads+writes in guest DDR.

UBUS decode-window / BBH / DSPTCHR / QM / DMA / SBPM writes from the real
`data_path_init` are **not issued** by this slow-path driver (they're stubbed),
so the model does not need to decode them — EXCEPT when Route A is enabled (next).

### Route A egress (driver `route_a=1`)

When the driver runs with `route_a=1`, it additionally brings up the QM + a
TM-core egress queue + a BBH_TX QMQ binding so an injected CPU_TX PD egresses the
way the stock image_2 thread routes (dispatcher → TM/QM core → BBH_TX), not the
CFE2 direct-to-BBH model. The model decodes:

- **QM** (`0xc00000`): `MEM_AUTO_INIT(0x138)` write + `MEM_AUTO_INIT_STS(0x13c)`
  read (returns `MEM_INIT_DONE=1` instantly); `ENABLE_CTRL(0x000)`; per-group
  `RUNNER_GRP` `QUEUE_CONFIG(+0x04)` (start/end queue) + `RNR_CONFIG(+0x00)`
  (bb_id/task/enable), groups at `0x300` stride `0x10`.
- **BBH_TX** (`0x890000`, stride `0x2000`): `QMQ` bits at `0x4b0`/`0x7b0` per
  instance.
- **Descriptor**: word0 `first_level_q[30:22]` (the target QM queue).

`runner_tx_kick` then gates emission: if Route A is active (QM enabled + ≥1
RUNNER_GRP enabled), a frame egresses only if its `first_level_q` lies in an
enabled group's `[start,end]` AND some BBH_TX instance is QM-fed (QMQ set) — else
it is dropped with a `LOG_GUEST_ERROR`. This regression-guards the driver's
Route A register writes. Debug counter: `NATC + 0x10` = frames egressed via
Route A. When `route_a=0` the QM/BBH state stays zero and TX emits as before
(legacy path). Validated: `run-validate.sh` (legacy) and the `route_a` cmdline
variant (`bcm4916_runner.route_a=1 …`) both reach `VALIDATE_DONE` tx=4/rx=6, the
latter logging `TX emit(route_a)`.

---

## 6b. NAT-C offload — XPE cmdlist decode (byte-exact 6813 layout)

When an RX frame **hits** a programmed NAT-C entry (§offload), the model runs
the cmdlist embedded in the FC_UCAST context at byte offset
`XPE_CTX_CMDLIST_OFF = 24` and forwards the result without delivering to the
CPU. The cmdlist is **length-delimited** by `cmd_list_data_length`
(`ctx[CTX_OFF_CMDLIST_DLEN]`) — there is **no NOP terminator**; trailing slot
slack is padded with the byte `0xfc`, which lies past `dlen` and is never
decoded.

**Encoding (contract source: `driver/runner/cmdlist.c`, the byte-exact emitter
pinned from `xpe_api.armb53_6813.o`).** Every command word is 32-bit
**big-endian** (emitted as two 16-bit BE half-words). Common fields:

- `opcode = byte0 >> 2`
- for offset-based ops, `byte1 = (offset >> 1) + 1`, so `offset = (byte1-1)*2`.

> SUPERSEDES the OLD uniform packing `opcode<<26 | offset8<<18 | position<<13 |
> width<<8 | nbytes` (with MCOPY/MOVE/ICSUM at 0x13/0x2c/0x36). The driver no
> longer emits that. The model now decodes the per-op byte layout below.

| op | byte0 | opcode | byte1 | byte2 | byte3 | inline data after word | effect |
|---|---|---|---|---|---|---|---|
| `replace_32`      | `0x60` | `0x18` | `(off>>1)+1` | `0x94` (.data ref) | `4` | 4 B = 32-bit imm (hi half, lo half; BE) | overwrite 4 B @off |
| `replace_16`      | `0x60` | `0x18` | `(off>>1)+1` | `0x94` | `2` | 2 B = 16-bit imm (BE) | overwrite 2 B @off |
| `replace_bits_16` | `0x50` | `0x14` | `(off>>1)+1` | `0x94` | `(pos&0xf) \| (((width-1)+pos)&0xf)<<4` | 2 B = `imm<<pos` | replace `width` bits at `pos` in the 16-bit word @off |
| `move_packet`     | `0x4c` | `0x13` | `from` (RAW byte off) | `to` (RAW byte off) | `nbytes` (`[6:0]`) | — | move `nbytes`; `to>from` = insert hole (VLAN push), `from>to` = delete hole (VLAN pop) |
| `decrement_8`     | `0x6a` | `0x1a` | `(off>>1)+1` | `(off>>1)+1` | `0xff` | — | byte @off -= 1 (TTL) |
| `apply_icsum_16`  | `0x70` | `0x1c` | `(off>>1)+1` | imm hi | imm lo | — (imm inline in low half, `0` here) | IP/L4 incremental checksum fixup @off |

Decode notes:

- **REPLACE (0x60)** is disambiguated by `byte3` (= nbytes): `4` → 32-bit, else
  → 16-bit. Both consume their immediate from the bytes that *follow* the
  command word (the open driver emits a flat single buffer, with the `.data`
  operand inline immediately after each command word; `byte2 = 0x94` is the
  pre-relocation `.data` reference and is not otherwise interpreted).
- **REPLACE_BITS (0x50)** recovers `position = byte3 & 0xf` and
  `width = ((byte3>>4)&0xf) - position + 1`; the inline operand is already
  pre-shifted (`imm << position`), so the model masks it to the
  `[(1<<width)-1] << position` window.
- **MOVE_PACKET (0x4c)** carries **raw** byte offsets in `byte1`/`byte2` (not
  `/2`) and no inline data. VLAN push = `insert_16(12,4)` = `move(12->16,4)` then
  `replace_32(12,tag)`; VLAN pop = `delete_16(12,4)` = `move(16->12,4)`.
- **ICSUM (0x70)** carries no separate data word (the 16-bit immediate is inline
  in the command word's low half, `0` in the open driver). The model recomputes
  the IP header checksum at `IP4_OFF_CSUM` or the L4 checksum elsewhere; real HW
  applies the precomputed incremental delta.

Approximation: ICSUM **recomputes** the full checksum rather than applying a
ones-complement delta (functionally identical for a well-formed frame; the model
zeroes the field and recomputes over the IP header / pseudo-header + L4 segment).

---

## 7. Contract constants shared with the driver

These must match `driver/runner/bcm4916_runner.{c,h}` exactly:

| constant | value | where |
|---|---|---|
| rdpa window base / size | `0x82000000` / `0x00caf004` | header |
| FPM block offset | `0xa00000` | header |
| FPM alloc/dealloc reg | FPM + `0x400` | header |
| RNR_MEM[n] base | `0x700000 + n*0x20000` | header |
| TX index doorbell offset in RNR_MEM | `0x0000` | header `CPU_TX_RING_INDICES_OFF` |
| PSRAM RX ring-cfg offset | `0x0000` | `PSRAM_CPU_RING_DESC_TABLE` |
| PSRAM TX ring-cfg offset | `0x0080` | `PSRAM_CPU_TX_RING_DESC_TABLE` |
| RX/TX ring depth | 256 | `RX_RING_DEPTH`/`TX_RING_DEPTH` |
| RX buffer size | 2048 | `RX_BUF_SIZE` |
| FPM chunk size | 512 | `FPM_CHUNK_SIZE_DEFAULT` |
| ownership bit | `word2 bit31` (BE) | ABI 2.4 |

> NOTE: the PSRAM ring-cfg offsets and the RNR_MEM TX-index offset are
> *placeholder* values (the true RDD table offsets are not yet pinned from the
> GPL RDD map). They are defined identically on both sides, so driver and
> emulator interoperate today; when the real offsets are pinned, update both the
> header and this contract together.
