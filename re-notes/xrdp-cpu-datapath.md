# BCM4916 XRDP/Runner CPU-port datapath — feasibility for a fully-open mainline driver

Scope: can the Runner CPU-port host datapath be driven from OPEN (GPL) code in a "dumb
pipe" mode (RX/TX rings moving packets MAC<->Linux CPU, no flow accel/NAT), and what is the
register/descriptor ABI?

**Confidence tags:** `[GPL-SRC]` = read directly from the Broadcom GPL SDK source.
`[BIN]` = confirmed against staged stock binaries on the RE container (`/opt/re-bins/*`).
`[FDT]` = from the live device tree (see `bcm4916-regmap.md`). `[INFER]` = deduced.

---

## FEASIBILITY VERDICT

**YES — but it requires loading the proprietary-format Runner microcode blob.**
More precisely: **YES-but-needs-blob-firmware.**

A fully *CPU-forwarded* (no HW accel/NAT/offload) v1 datapath is architecturally feasible
and the host-side ABI is small, simple, and fully documented in GPL source. **However, you
cannot move even a single packet between a MAC port and a Linux skb without the Runner cores
executing firmware.** The MAC ports (UNIMAC/XLMAC/XPORT) do **not** DMA into CPU-visible
rings; they DMA into Runner-managed buffer pools (FPM/SBPM), and it is the **Runner firmware
running on the packet processor cores** that classifies each packet, writes the CPU RX
descriptor into the host DDR ring, flips the ownership bit, and on TX pulls the host's TX
descriptor out of Runner SRAM and DMAs it to the MAC. There is no hardware bypass / "Runner
idle" direct-DMA mode. `[GPL-SRC][BIN]`

The good news for "open":
- The Runner firmware Broadcom ships **is itself GPLv2-licensed and redistributable** — in
  the 416L05 GPL drop it is shipped as plain C `uint32_t firmware_binary_A[] = {...}` arrays
  (`runner_fw_a/b/c/d.c`, "Firmware version 1.4.0"). `[GPL-SRC]` So a fully-open driver can
  legally carry and load it. But it is *opaque microcode* (instruction words only, no
  high-level source), so "fully open" in the FSF sense = "GPL blob you can ship," not
  "auditable C datapath." This is the one unavoidable blob.
- Everything on the *host* side — the CPU ring create/read/refill/TX logic, descriptor
  layout, the BPM/SBPM/FPM pool drivers, the BBH DMA programming, the RDD microcode-load and
  table-init glue, and the MAC/MDIO/switch control plane — is **GPL source** and
  re-implementable in a clean mainline driver. `[GPL-SRC]`

So: a v1 "dumb pipe" open driver = clean GPL host driver + clean GPL pool/BBH/Runner-init
code + **redistributing Broadcom's GPL Runner firmware image as a `request_firmware()` blob,
configured for a minimal CPU-trap-everything ruleset.** The firmware is the make-or-break
dependency and it cannot be avoided, only legally shipped.

**Caveat on generation:** the authoritative GPL source I traced is `broadcom-sdk-416L05`,
which targets **BCM63138/63148** ("RDP" impl2: dual Runner A/B + pico C/D, BPM/SBPM pools).
The GT-BE98's **BCM4916 is the newer "XRDP"** generation (FPM pool, different Runner core
count, different register map). The *architecture and ABI shape are confirmed identical in
concept* against the 4916 stock binaries `[BIN]` (host ring + FW-filled descriptors +
`drv_rnr_load_microcode` + FPM/SBPM), but the **exact XRDP register offsets and the XRDP
descriptor bitfields differ** and still need to be pinned from the 4916 `rdpa.ko`/`pktrunner.ko`.
The 416L05 source is the Rosetta stone, not a drop-in.

---

## 1. THE CPU-PORT DATAPATH — how a packet reaches an skb

### RX (MAC -> CPU), confirmed end to end `[GPL-SRC]`

1. **MAC RX -> buffer pool.** The **BBH** (Buffer/Burst Handler, one per port) DMAs the
   incoming frame from the MAC into a buffer allocated from the **SBPM/BPM** free pool
   (XRDP: **FPM**). BBH + pool are programmed by GPL code
   (`shared/opensource/drv/dpi/bcm63138_data_path_init.c`, `rdp_drv_bbh.c`,
   `rdp_drv_sbpm.c`, `rdp_drv_bpm.c`).
2. **Runner FW classifies.** The Runner firmware (in core instruction SRAM) inspects the
   packet, looks up `CPU_REASON_TO_CPU_RX_QUEUE_TABLE` (in Runner COMMON SRAM, programmed by
   GPL `rdd_cpu_rx_initialize`) to map a "reason" -> a CPU RX queue/ring id.
3. **Runner writes the host ring descriptor.** The FW writes a 16-byte `CPU_RX_DESCRIPTOR`
   into the **host DDR ring** (whose base phys addr the host gave the FW at ring-create) and
   sets ownership = HOST. **This step is done by firmware, not by any host-visible DMA
   engine.**
4. **Host polls the ring.** The CPU side (`rdp_cpu_ring_read_packet_refill` ->
   `ReadPacketFromRing`) polls the ownership bit of `host_ring[ring_id].head`, reads the
   descriptor, hands `data_ptr`/`packet_size`/`src_port`/`reason` up, allocates a fresh pool
   buffer, and re-arms the slot (`AssignPacketBuffertoRing`) by writing the new buffer phys
   addr back with ownership cleared (= RUNNER). Then advances head (ring wraps at `end`).
   **The RX read path is pure cache-coherent polling of DDR — there is no MMIO doorbell on
   RX.** An IRQ (the per-queue SPI, queue0..31 SPI 75..106 `[FDT]`) just wakes the poll loop.

Source files: `shared/opensource/rdp/rdp_cpu_ring.c`,
`shared/opensource/include/rdp/rdp_cpu_ring_inline.h`,
`shared/opensource/include/rdp/rdp_cpu_ring_defs.h`,
`shared/broadcom/rdp/impl2/rdd/rdd_cpu.c`.

### TX (CPU -> MAC), confirmed `[GPL-SRC]`

1. Host driver (`bcmeapi_pkt_xmt_dispatch` -> `rdpa_cpu_send_sysb` / `_send_raw`) builds a
   **`RDD_CPU_TX_DESCRIPTOR`** and writes it into a **CPU_TX queue that lives in Runner
   SRAM** (not host DDR), at `DEVICE_ADDRESS(sram_base) + g_cpu_tx_queue_write_ptr[runner]`
   (`f_rdd_cpu_tx_send_message`, `rdd_cpu_tx_initialize` in `rdd_cpu.c`). On XRDP the host
   instead pushes via FPM (`rdpa_cpu_send_sysb_fpm` / `f_rdpa_cpu_send_raw_from_fpm` are
   exported by `rdpa_gpl.ko` `[BIN]`).
2. The **Runner FW** consumes the TX descriptor, (copies/links the payload into a pool
   buffer), and programs the egress **BBH/MAC** to DMA it out.

So TX *does* use an MMIO write (into Runner SRAM TX queue), but the actual MAC DMA is again
performed by firmware, not by the host.

### Descriptor ABI — `CPU_RX_DESCRIPTOR` (16 bytes, RDP/63138 layout) `[GPL-SRC]`

From `rdp_cpu_ring_defs.h`. **Runner is big-endian, ARM host is little-endian, so every word
is `swap4bytes()`'d on access** (the LE struct variant is the one used on ARM):

```
word0: packet_length:14 | source_port:5 | is_chksum_verified:1 | flow_id:12
word1: descriptor_type:4 | reserved:5 | dst_ssid:16 | reason:6 | payload_offset_flag:1
word2: host_buffer_data_pointer:31 | ownership:1      <- bit31 = ownership (1=HOST,0=RUNNER)
word3: {wl_metadata / free_index / ssid_vector} | ip_sync_1588_idx:4 | ... | is_ucast:1 | is_rx_offload:1
```

- Ownership bit = MSB of word2 (`& 0x80000000` after swap). Host sees a packet when this is
  set; host re-arms by clearing it.
- `host_buffer_data_pointer` = packet buffer phys addr >> 0 (31-bit field; pool buffers are
  in low DDR). `PHYS_TO_CACHED()` to read payload; cache-invalidate before reading.
- For a dumb pipe v1 you only need: `packet_length`, `source_port` (-> which netdev/port),
  `reason` (set the FW to use a single trap-to-CPU reason), and `host_buffer_data_pointer`.

`RDD_CPU_TX_DESCRIPTOR` fields (TX queue / EMAC / queue, command) are in
`rdd_data_structures_auto.h` / `rdd_cpu.c` macros (`..._EMAC_WRITE`, `..._QUEUE_WRITE`,
`..._COMMAND_WRITE`). **The XRDP RX/TX descriptor bitfields differ and must be re-derived
from the 4916 `rdpa.ko`** (the stock `bcm_enet.ko` "v7.0" and `rdpa_gpl.ko` symbol tables are
the cross-check). `[INFER]`

### Ring object (host side) `[GPL-SRC]`

`RING_DESCTIPTOR` (`rdp_cpu_ring.h`): `{ring_id, admin_status, num_of_entries,
size_of_entry=sizeof(CPU_RX_DESCRIPTOR), packet_size, head/base/end, buff_cache[],
databuf_alloc/free callbacks, stats}`. `rdp_cpu_ring_create_ring()` allocates the
**non-cacheable** descriptor array via `rdp_mm_aligned_alloc`, fills each slot with a
freshly-allocated pool buffer (ownership=RUNNER), and returns the ring's **phys base** in
`*ring_head` — that phys base is what gets handed to the Runner/RDD so the FW knows where to
write. Max queues = `RDPA_CPU_MAX_QUEUES` (+ WLAN queues).

---

## 2. RUNNER MICROCODE DEPENDENCY (the blob answer)

**Moving ONE packet to the CPU REQUIRES the Runner firmware. There is no no-accel/bypass
mode.** `[GPL-SRC][BIN]` The MAC->host-ring write in RX and the host-TX-queue->MAC DMA in TX
are *performed by the firmware executing on the Runner cores*. `rdd_runner_enable/disable`
only gate the cores; with the cores idle, nothing fills or drains the rings.

How the firmware is loaded `[GPL-SRC]`:
- `rdd_load_microcode(A,B,C,D ptrs)` in `shared/broadcom/rdp/impl2/rdd/rdd_init.c` does
  `MWRITE_BLK_32(sram_fast_program_ptr, microcode_ptr, sizeof(RUNNER_INST_MAIN))` for each of
  the 4 cores (A/B = "fast/main", C/D = "pico") into Runner instruction SRAM, plus
  prediction RAM (`predict_runner_fw_*`), plus context/common/private SRAM init.
- The images are GPLv2 C arrays in `shared/broadcom/rdp/impl2/firmware_dsl_63148/runner_fw_*.c`
  (`uint32_t firmware_binary_A[]`, ~116 KB each for A/B, ~67 KB for C/D).

On the 4916/XRDP stock build `[BIN]`: `rdpa.ko` (7.2 MB — size dominated by embedded FW
arrays) exposes `drv_rnr_load_microcode`, `drv_rnr_load_prediction`,
`rdpa_version_firmware_revision`, `ag_drv_rnr_regs_prediction_*`. Same load model; XRDP adds
the FPM pool (`rdpa_cpu_send_sysb_fpm`, "Rx FPM buffers in use", SBPM scratch symbols seen in
`rdpa.ko`/`rdpa_gpl.ko`).

**Can the blob be avoided?** No, not for v1. Options, in order of realism:
1. **Ship Broadcom's GPL Runner FW as a `request_firmware()` blob** and load it with a
   minimal "trap everything to one CPU queue" table config. Legally fine (GPLv2). This is the
   recommended path. The microcode is opaque but redistributable.
2. Write your own Runner microcode (Broadcom packet-processor ISA). Not realistic — no public
   ISA/toolchain.
3. Find a HW direct-DMA path that bypasses the Runner. **Does not exist** on this SoC family
   (this is the defining XRDP/RDP property vs. the BCM4908 direct-MAC design).

---

## 3. FPM / BPM / SBPM POOL MANAGER

- **GPL and required.** `rdp_drv_bpm.c` (BPM), `rdp_drv_sbpm.c` (SBPM) are GPL host drivers
  that program the pool managers; `bcm63138_data_path_init.c` sizes/initializes them. The
  Runner allocates RX buffers from these pools and the host refills them. `[GPL-SRC]`
- On XRDP the equivalent is **FPM** (free-pool manager); stock ships `bcm_mpm.ko` + `bcm_bpm.ko`
  `[BIN]` and `rdpa_gpl.ko` exports the `*_fpm` send helpers `[BIN]`.
- The rings absolutely depend on the pool: every RX slot and every TX buffer comes from
  FPM/BPM. A v1 driver must bring up FPM and keep it refilled (the `databuf_alloc/free`
  callbacks in `RING_DESCTIPTOR` — stock uses `bdmf_sysb_databuf_alloc` / `gbpm_alloc_mult_buf`).
- The FPM register block on 4916 is inside the `brcm,rdpa` window @ `0x82000000` `[FDT]`; the
  datapath IRQs include **fpm SPI 107** `[FDT]` (free-pool exhaustion/refill).

---

## 4. MAC / MDIO / PHY / SWITCH CONTROL PLANE (the reusable part)

**GPL and largely mainline-mappable** `[GPL-SRC]`:
- `bcmdrivers/opensource/net/enet/impl5/{bcmenet.c,bcmsw.c,bcmmii.h,bcmgmac.c}` — UNIMAC
  bring-up, MDIO, PHY polling, and switch port config via standard register accessors
  (`bcmswaccess`, `bcmswshared`). This is ordinary MMIO MAC/switch programming, no firmware.
- Mainline reuse:
  - **`mdio-bcm-unimac`** (`drivers/net/mdio/`) for the UNIMAC MDIO @ `0x828a8000` and the
    `mdio-sf2` @ `0x837ffd00` `[FDT]`.
  - **`b53` / `bcm_sf2`** (`drivers/net/dsa/`) for the integrated switch (`switch0` under the
    xrdp bus `[FDT]`). The 4916 switch register model is the SF2 family; b53 ops likely map
    with a new variant. This is the most directly reusable mainline code.
  - UNIMAC/XLMAC/XPORT link config (XPORT @ `0x837f0000`, XLMAC @ `0x828b2000` `[FDT]`) needs
    new but straightforward MMIO code modeled on the GPL `bcmsw.c`/XPORT init.
- This whole plane is the part you can write/reuse cleanly with no blob.

---

## 5. REVISED OPEN-DRIVER ARCHITECTURE PROPOSAL (v1, CPU-forwarded dumb pipe)

Three layers; only the firmware is a blob.

**(A) Control plane — reuse mainline, write a 4916 variant.** `[reuse]`
- `mdio-bcm-unimac` for MDIO; `b53`/`bcm_sf2` (new 4916 variant) for the switch; PHY via
  phylink. Port/link/PHY all driven by standard MMIO. No firmware. Highest-confidence reuse.

**(B) Runner bring-up + datapath driver — NEW GPL code (port from 416L05).** `[new]`
- `runner_init`: reset cores, `request_firmware()` the Runner image, `MWRITE_BLK` it into
  instruction/prediction SRAM (port of `rdd_load_microcode`), init COMMON/PRIVATE/CONTEXT
  SRAM, enable cores.
- `fpm_init` + BBH init (port of `*_data_path_init.c`, `rdp_drv_fpm`/`bbh`) — XRDP register
  offsets from 4916 `rdpa.ko`.
- CPU RX rings: port of `rdp_cpu_ring.c` (create/read_refill/assign, ownership polling) — the
  ABI is tiny (~750 lines) and clean. NAPI poll loop driven by the queue SPIs (75..106).
- CPU TX: push `RDD_CPU_TX_DESCRIPTOR` to Runner SRAM TX queue / FPM (XRDP `*_fpm` path).
- **Minimal table config**: program `CPU_REASON_TO_CPU_RX_QUEUE_TABLE` so *all* ingress
  traps to one (or per-port) CPU queue with NO ucast/flow lookup -> pure L2 dumb pipe; the
  CPU does all bridging/forwarding in Linux. This is the "no offload" mode and it is exactly
  what the reason-table init in `rdd_cpu_rx_initialize` already supports (everything to
  `direct_queue_*`).

**(C) Firmware blob.** `[blob, GPL, shippable]`
- Carry Broadcom's GPL Runner microcode (the 4916/XRDP image, extractable from `rdpa.ko`'s
  embedded arrays or shipped as the SDK C arrays) via `linux-firmware`-style
  `request_firmware()`. Document it as GPLv2 firmware. Unavoidable.

**What's genuinely new vs. reused:**
- Reused (mainline): MDIO (`mdio-bcm-unimac`), switch (`b53`/`bcm_sf2` variant), phylink. ~MAC
  control plane.
- New GPL (no equivalent upstream — *this is the bulk of the work*): Runner FW loader,
  FPM/BBH init, XRDP CPU host-ring RX/TX, NAPI integration, reason-table "dumb-pipe" config,
  and the XRDP register/descriptor map (none of this exists upstream — `bcm4912.dtsi` has no
  enet nodes; `bcm4908_enet` is the wrong, direct-DMA model).
- Blob: GPL Runner microcode image.

**Risk / open items before committing:**
1. **Pin the XRDP descriptor bitfields & register offsets from 4916 `rdpa.ko`/`pktrunner.ko`**
   (the 416L05 layout is RDP-63138, structurally identical but not bit-identical).
2. Confirm the 4916 Runner FW image can be cleanly extracted + loaded standalone (the GPL SDK
   matching 4916 — a 4912/4916-era release, not 416L05 — would give the C arrays + the exact
   `drv_rnr_load_microcode` sequence; worth locating on datashed/OpenWrt mirrors).
3. Confirm FPM init ordering and the host TX path (`*_send_sysb_fpm`) on XRDP.

---

## SOURCES

GPL source (Broadcom SDK `broadcom-sdk-416L05`, mirror
`http://www.datashed.science/misc/bcm/gpl/broadcom-sdk-416L05/`, full tarball
`.../broadcom_sdk_416L05_pkg.tar.bz2`; extracted on dev-build `~/re-sdk/`):
- `shared/opensource/rdp/rdp_cpu_ring.c` — CPU ring create/read/refill/TX, BPM alloc.  **[primary]**
- `shared/opensource/include/rdp/rdp_cpu_ring_defs.h` — `CPU_RX_DESCRIPTOR` 16-byte ABI.
- `shared/opensource/include/rdp/rdp_cpu_ring_inline.h` — `ReadPacketFromRing` / `AssignPacketBuffertoRing` (ownership + endian swap).
- `shared/opensource/include/rdp/rdp_cpu_ring.h` — `RING_DESCTIPTOR`, `CPU_RX_PARAMS`.
- `shared/broadcom/rdp/impl2/rdd/rdd_cpu.c` — `rdd_cpu_rx_initialize` (reason->queue table), `rdd_cpu_tx_initialize`, `f_rdd_cpu_tx_send_message`.
- `shared/broadcom/rdp/impl2/rdd/rdd_init.c` — `rdd_load_microcode` (MWRITE_BLK to Runner SRAM), `rdd_runner_enable/disable`.
- `shared/broadcom/rdp/impl2/firmware_dsl_63148/runner_fw_a/b/c/d.c` + `predict_runner_fw_*.c` — GPLv2 Runner microcode C arrays (`firmware_binary_A[]`, "Firmware version 1.4.0").
- `shared/opensource/drv/dpi/bcm63138_data_path_init.c` — BBH/SBPM/BPM init + datapath bring-up.
- `shared/opensource/rdp/rdp_drv_{bbh,bpm,sbpm}.c` — BBH + pool-manager drivers.
- `bcmdrivers/opensource/net/enet/impl5/{bcmenet.c,bcmsw.c,bcmmii.h}` — GPL MAC/MDIO/switch control plane.
- `bcmdrivers/opensource/net/enet/shared/bcmenet_runner_inline.h` — host TX dispatch (`rdpa_cpu_send_sysb`, `_fpm`).
- `bcmdrivers/opensource/char/rdpa_gpl/impl1/include/{rdpa_cpu.h,rdpa_cpu_helper.h}` + `autogen/rdpa_ag_cpu.h` — RDPA CPU API.

Stock 4916 binaries (the RE container `/opt/re-bins/`, strings only):
- `rdpa.ko` (7.2 MB): `drv_rnr_load_microcode`, `drv_rnr_load_prediction`, `rdpa_version_firmware_revision`, FPM/SBPM scratch symbols — confirms XRDP uses the same FW-load + host-ring model.
- `rdpa_gpl.ko`: `rdpa_cpu_send_sysb_fpm`, `f_rdpa_cpu_send_raw_from_fpm` — XRDP host TX via FPM.
- Pools present: `bcm_mpm.ko`, `bcm_bpm.ko`; runner: `pktrunner.ko`, `cmdlist.ko`, `bdmf.ko`.

FDT addresses: see `re-notes/bcm4916-regmap.md` (`brcm,rdpa` @0x82000000, UNIMAC @0x828a8000,
XLMAC @0x828b2000, XPORT @0x837f0000, mdio-sf2 @0x837ffd00, GIC @0x81001000; datapath IRQs
fpm SPI 107, queue0..31 SPI 75..106).
