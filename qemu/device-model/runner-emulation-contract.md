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

### 3.2 Per-descriptor protocol (16 B big-endian, ABI 2.3/5bis-G1)

Each ring entry (`struct runner_rx_desc`):

| word | bytes | field |
|---|---|---|
| word0 | 0..3 | `[13:0]`=packet_length, `[18:14]`=source_port (ingress vport) |
| word1 | 4..7 | reason/dst_ssid (model may leave 0) |
| word2 | 8..11 | **`[31]`=ownership** (1=HOST owns), `[30:0]`=host_buffer_data_pointer (buffer phys, low 31 bits) |
| word3 | 12..15 | metadata (model may leave 0) |

**Initial state (set by guest in `create_ring`):** every descriptor's word2 =
`BE( buffer_phys & 0x7fffffff )` with **bit31 clear** ⇒ ownership = RUNNER. So at
start the model owns all slots and each carries a guest-provided buffer phys.

### 3.3 RX injection (model receives a frame from its host netdev backend)

When a frame arrives on the model's backend netdev, for the slot the model is
about to fill (it tracks its own producer index; start at 0, wrap at depth):

1. Read the descriptor's **word2** (BE) → `buf_phys = word2 & 0x7fffffff`.
   (Ownership must be RUNNER, i.e. bit31 clear; if it's HOST, the host hasn't
   re-armed yet → ring full, drop or back-pressure.)
2. **DMA the frame bytes into guest physical memory at `buf_phys`** (length =
   frame length, ≤ buffer size 2048).
3. Write **word0** (BE) = `(packet_length & 0x3fff) | ((src_port & 0x1f)<<14)`.
   `src_port` = the ingress vport for that backend (maps to a DSA user port);
   for a dumb single-port pipe, 0 is fine.
4. Write **word2** (BE) = `(buf_phys & 0x7fffffff) | 0x80000000` — i.e. **set
   ownership = HOST** (bit31). This publish must be the **last** write and must
   be ordered after the payload+word0 writes.
5. Advance the model's producer index (wrap at depth).
6. **Raise the RX IRQ** (see §5). RX has **no host-visible doorbell**; the IRQ
   only wakes the guest's NAPI poll, which then polls ownership in DDR.

### 3.4 Host consume + re-arm (what the guest does — informational)

The guest NAPI poll reads word2 (BE-swapped), checks bit31; if set, reads
word0 for length, copies the payload out, then **re-arms in place**: rewrites
word2 = `BE(buf_phys & 0x7fffffff)` (ownership cleared → RUNNER) using the
**same buffer**, and advances its consumer index. The model must treat a slot
whose ownership flips back to RUNNER as available again. (The guest reuses the
same buffer phys, so the model can keep using the word2 pointer it reads.)

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

| word | field (host order, BE in memory) |
|---|---|
| word0 | `[31]`=is_egress (1), `[21:8]`=packet_length |
| word1 | (sk_buf ptr / 1588 — unused in FPM mode) |
| word2 | `[28]`=is_emac, `[19:12]`=egress port (bits[11:4] of half@8), `[8]`=abs (0 ⇒ FPM-token mode) |
| word3 | **FPM token** (`pkt_buf_ptr_low_or_fpm_bn0`) when abs=0 |

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
so the model does not need to decode them.

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
