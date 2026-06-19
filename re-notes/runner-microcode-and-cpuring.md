# BCM4916 (BCM6813) Runner microcode + CPU host-ring — de-risk pass

De-risk pass answering the two datapath-blocking questions for a *fully-open* mainline
driver with (eventually) HW offload:

- **A. Is there a shippable (GPLv2 / redistributable) BCM4916 Runner microcode** we can
  load via `request_firmware()` in an open driver?
- **B. What is the FPM/CPU host-ring bring-up sequence** for an open SLOW-PATH datapath
  (MAC<->CPU, no offload)?

All work was READ-ONLY (reads/copies only; no rmmod/insmod/flash/devmem/reboot).

**Confidence tags:** `[SDK]` = read directly from the 4916 "behnd" GPL SDK
(asuswrt-merlin.ng `release/src-rt-5.04behnd.4916`, staged on the build host).
`[KO]` = from the stock 4916 `rdpa.ko` (unstripped staged copy, ELF symbol/section RE).
`[DEV]` = from the live device (read-only, `/sys/module`, on-disk module strings).
`[INFER]` = deduced/cross-referenced. Sources cited inline.

---

## A. MICROCODE VERDICT

### VERDICT: **PROPRIETARY — NON-REDISTRIBUTABLE.**

There is **no GPLv2 / redistributable BCM4916 (BCM6813) Runner microcode** available to us.
The 4916 Runner microcode exists only **compiled into the stock `rdpa.ko`, which declares
`license=Proprietary` and taints the kernel `P`.** It is NOT shipped as a `request_firmware()`
blob, NOT present as GPL C arrays in the 4916 GPL SDK, and NOT present as any standalone
firmware file on the device. A fully-open driver therefore **cannot legally ship the 4916
Runner microcode** by extracting it from `rdpa.ko`.

This **corrects** the more optimistic earlier note `xrdp-cpu-datapath.md`, whose
"GPL-shippable blob" conclusion was based on the *old 63138/63148 RDP impl2* SDK
(`broadcom-sdk-416L05`), where the microcode shipped as GPL C arrays. That does **not** carry
over to the 4916 XRDP generation. Pinned for 4916 below.

### Where the 4916 microcode actually lives `[KO]`

Embedded as global data objects inside `rdpa.ko` (7,186,008 bytes; vermagic `4.19.294 SMP
... aarch64`):

| symbol | size | meaning |
|---|---|---|
| `fw_binary_0` .. `fw_binary_7` | 32768 B each = **262144 B (256 KB) total** | the 8 Runner-core instruction-SRAM images |
| `fw_predict_0` .. `fw_predict_7` | 1024 B each (8 KB total) | per-core branch-prediction RAM images |
| `rdpa_version_fw_0/2/4/6` | 8 B each | per-core firmware version words |

Loaded by the functions `drv_rnr_load_microcode`, `drv_rnr_load_instructions`,
`drv_rnr_load_prediction` (all present as `FUNC` symbols in `rdpa.ko`) `[KO]`. The microcode
is written into Runner instruction SRAM via the `ag_drv_rnr_inst_*` register accessors
(`RNR_INST`, `RNR_INST_ADDRS`, `RNR_INST_MEM_ENTRY_REG`, `ag_drv_rnr_inst_mem_entry_get`),
i.e. block-write (`MWRITE`-style) into the runner instruction-memory window — same load model
as the older RDP, but XRDP-specific accessors `[KO][INFER]`.

The 8x32 KB layout matches XRDP's 8 packet-processor cores (the older 63138 RDP had 4 cores
A/B/C/D ~116/67 KB); this is concrete evidence the 4916 microcode is a *different binary* from
anything in the 416L05 GPL drop.

### License evidence (the critical citations)

- `rdpa.ko` `.modinfo`: **`license=Proprietary`**, `name=rdpa` `[KO]`.
- Live device: `/sys/module/rdpa/taint` = **`P`**; `/proc/sys/kernel/tainted` = `4097`
  (bit 0 = PROPRIETARY module loaded) `[DEV]`.
- On-device module file `/lib/modules/4.19.294/extra/rdpa.ko` carries the identical build
  paths `.../src-rt-5.04behnd.4916/rdp/projects/BCM6813/target/rdd/rdd_init.c` etc.,
  confirming it is the same BCM6813 rdpa build that embeds the microcode `[DEV]`.

So the only artifact containing the 4916 microcode is a Proprietary-licensed module.
Extracting `fw_binary_*` from it and redistributing it under GPL/`linux-firmware` would be
**redistributing Broadcom proprietary code without a license** — not permissible for a
"fully-open" driver.

### Confirmed ABSENT from the 4916 GPL SDK `[SDK]`

In `release/src-rt-5.04behnd.4916` (sparse: `bcmdrivers shared kernel`):

- **No Runner microcode in any form.** Zero `firmware_binary`/`fw_binary` C arrays; the only
  `*ucode*`/`*microcode*` files are unrelated **Serdes/Merlin PHY** ucode
  (`merlin16_shortfin_ucode_image.h`, `merlin28_shortfin_ucode_image.h`, etc.) and stock
  Linux x86/GPU ucode. None is the Runner.
- **No rdd/rdp source at all.** There is no `rdd/`, `rdp/projects/.../target/`, or `xrdp/`
  source directory. `shared/opensource/include/rdp/` contains only `bl_os_wraper.h` and
  `rdp_ms1588.h`. The build paths baked into `rdpa.ko` (`rdp/projects/BCM6813/target/rdd/...`,
  `.../rdp_subsystem/...`) point at an internal Broadcom tree that ASUS/Merlin **do not
  publish**.
- **No `drv_rnr_load_microcode` / `request_firmware` for the Runner anywhere** in
  `bcmdrivers`+`shared` (grep returns nothing outside the stock linux- tree).
- The rdpa runtime itself ships only as the proprietary `rdpa.ko` plus prebuilt aarch64 `.o`
  blobs (`archer_ucast.o`, `archer_mcast.o`, `cmdlist.o`, `pktrunner.o`, `bcm_bpm.o`, ...).
  The GPL parts are the enet driver (`net/enet/impl7` + `bcm96813/`, `DUAL/GPL` headers) and
  thin rdpa middleware shims (`rdpa_drv`, `rdpa_mw`, `rdpa_gpl_ext`).

### Confirmed ABSENT on the device `[DEV]`

`find /lib/firmware /rom /etc /data /lib/modules` for
`*rdp* *runner* *rdd* *rnr* *.itb *ucode* *xrdp*` returns **nothing** for the Runner. There is
**no standalone Runner firmware file** — the microcode is only inside `rdpa.ko`. (No
`request_firmware`/`.bin` strings for the Runner in the module either.)

### Net consequence for "fully open"

A fully-open (no-proprietary-module, no-proprietary-blob) datapath on this SoC is **NOT
shippable today**, because:
1. Even a slow-path (CPU-forwarded) datapath REQUIRES the Runner cores to be running
   firmware — the MACs DMA into Runner-managed pools, and *firmware* fills the host CPU rings
   (no HW direct-DMA bypass exists; see `xrdp-cpu-datapath.md`). `[INFER from KO+SDK]`
2. The only 4916 microcode we can obtain is Proprietary and non-redistributable.

The realistic "open" options are therefore: (a) ship a driver that *depends on* the
proprietary microcode obtained on-device (taints `P`, like the stock module — not "fully
open" in the FSF sense); (b) obtain a written redistribution license from Broadcom for the
4916 Runner microcode (unlikely); or (c) locate a different *4916/6813-class* GPL SDK that, unlike
Merlin's, ships the `rdd` microcode as GPL arrays (none found so far — see
`gpl-source-inventory.md`; this would need re-confirmation against any future drop).
Option (a) is the only one available now.

---

## B. CPU-RING / FPM BRING-UP MAP (open slow-path)

Goal: move first packet MAC<->CPU with the Runner in "trap-everything-to-CPU" mode (no flow
accel / NAT). The host-side ABI is small and conceptually GPL-portable; the XRDP register
offsets and descriptor bitfields must be RE'd from `rdpa.ko` because the matching `rdd`/
`rdp_subsystem` source is withheld.

### XRDP structures confirmed present in the 4916 rdpa.ko `[KO]`

The 4916 uses **separate feed / recycle / data rings** (XRDP model), not the single 63138
ring:

- `CPU_RING_DESCRIPTOR` register (the per-ring control descriptor).
- `CPU_FEED_RING_DESCRIPTOR_TABLE`, `CPU_FEED_RING_CACHE_TABLE`,
  `CPU_FEED_RING_INDEX_DDR_ADDR_TABLE`, `CPU_FEED_RING_RSV_TABLE` — host posts free FPM
  buffers to the Runner via the **feed ring** (built by `rdp_cpu_feed_ring.c`, a path baked
  into the module).
- `CPU_RECYCLE_RING_DESCRIPTOR_TABLE` — buffers handed back / recycled.
- `RING_CPU_TX_DESCRIPTOR` — the host TX descriptor.
- "CPU RX Ring Descriptors", `from_feed_ring: 0x%x` debug strings.

Compared to the 63138 16-byte `CPU_RX_DESCRIPTOR` documented in `xrdp-cpu-datapath.md`, the
XRDP descriptor *concept* is the same (ownership bit + buffer ptr + length + source_port +
reason) but the **bitfields and the ring split (feed/recycle vs. inline refill) differ** and
must be re-derived from `rdpa.ko` — do NOT assume the 63138 layout.

### Init order (reconstructed from rdpa.ko symbols + baked build paths) `[KO][INFER]`

The stock module's own source paths name the bring-up files (present in `rdpa.ko` strings,
absent from the GPL SDK):

1. `rdp_subsystem/data_path_init.c` + `data_path_init_common.c` — top-level
   `_data_path_init` (a `FUNC` in `rdpa.ko`): brings up the whole XRDP datapath.
2. **FPM / SBPM pool init** — `rdp_drv_sbpm.c` (+ FPM); `ag_drv_*` SBPM/FPM accessors. The
   Runner allocates RX buffers from these pools; the host posts free buffers via the **feed
   ring** (`rdp_cpu_feed_ring.c`). FPM exhaustion/refill IRQ = SPI 107 (see
   `bcm4916-regmap.md`).
3. **BBH RX/TX setup** — `xrdp_drv_bbh_rx_ag.c` / `xrdp_drv_bbh_tx_ag.c`; huge
   `ag_drv_bbh_rx_*` / `ag_drv_bbh_tx_*` accessor surface in `rdpa.ko` (enable, flow-ctrl,
   dispatcher sbpm bb_id, fifo size, etc.). One BBH per port DMAs MAC<->pool.
4. **Runner cores + microcode** — `rdd_init.c`; `drv_rnr_load_instructions` +
   `drv_rnr_load_prediction` (write `fw_binary_*` / `fw_predict_*` into RNR_INST SRAM), then
   enable cores. *(Blocked by part A — proprietary microcode.)*
5. **CPU RX/TX rings** — `rdp_cpu_feed_ring.c` (+ the cpu_rx/cpu_tx rdd glue): create the DDR
   data ring, the feed ring, the recycle ring; `_rdp_cpu_tx_ring_indices_alloc`,
   `_rdpa_recycle_thread_handler` are `FUNC`s in `rdpa.ko`.
6. **Reason->queue table** — `rdd_cpu_rx.c` programs CPU_REASON_TO_CPU_RX_QUEUE; for slow-path
   set every reason to one (or per-port) CPU queue with no flow lookup = pure L2 dumb pipe.

### Doorbell / poll model + IRQs `[INFER from KO + regmap]`

- **RX:** Runner FW writes a data-ring descriptor in host DDR and flips ownership=HOST; host
  polls the ownership bit (cache-coherent DDR poll, no MMIO doorbell on RX) and re-arms by
  posting a fresh FPM buffer on the **feed ring**. NAPI poll is woken by the per-queue IRQ.
- **TX:** host writes a `RING_CPU_TX_DESCRIPTOR` (XRDP pushes via FPM; `rdpa_gpl.ko` exports
  `rdpa_cpu_send_sysb_fpm` / `f_rdpa_cpu_send_raw_from_fpm`); FW consumes it and programs the
  egress BBH/MAC DMA.
- **IRQs:** datapath queue IRQs SPI 75..106 (queue 0..31), FPM SPI 107 `[FDT, bcm4916-regmap.md]`.

### Control plane (no firmware, fully GPL/mainline) `[SDK]`

UNIMAC/XLMAC/XPORT MAC bring-up, MDIO, PHY, and the SF2 switch are ordinary MMIO and are full
GPL source in the SDK (`net/enet/impl7` + `bcm96813/`, `sf2*.c`, `phy_drv_ext3.c`), mapping to
mainline `mdio-bcm-unimac` + `b53`/`bcm_sf2` (new 4916 variant) + phylink. This part is
unblocked and is the right place to start.

---

## OPEN SLOW-PATH PLAN

To get first-packet MAC<->CPU, an open driver must: (1) bring up the MAC/MDIO/PHY/switch
control plane (GPL, mainline-mappable — start here); (2) init FPM/SBPM pools + feed ring;
(3) init per-port BBH RX/TX DMA; (4) load + enable the Runner microcode; (5) create the CPU
data/feed/recycle rings and program the reason->CPU-queue table to "trap everything";
(6) NAPI poll the data ring, refill via the feed ring, TX via the FPM path.

**GPL-portable vs. must-be-RE'd:**
- *GPL-portable now:* the MAC/MDIO/PHY/switch control plane (SDK source); the host ring/pool
  *algorithms and call sequence* (documented by the 63138 GPL ring code + the enet caller
  hooks in `runner.c`/`enet_inline_runner.h`).
- *Must be RE'd from `rdpa.ko`:* the exact XRDP register offsets (`ag_drv_*`,
  `xrdp_drv_bbh_*`, `xrdp_drv_rnr_inst_*`), the feed/recycle/data ring + CPU TX descriptor
  bitfields, the FPM/SBPM init ordering, and the reason-table layout — because the matching
  `rdd`/`rdp_subsystem` GPL source is withheld from the Merlin SDK.
- *Blocked (the make-or-break):* the **Runner microcode itself is Proprietary and
  non-redistributable** (part A). A slow-path datapath cannot move even one packet without it.

**Realistic risk:** HIGH. Two compounding blockers — (1) the entire XRDP datapath
register/descriptor map must be reverse-engineered from a 7.2 MB proprietary `.ko` (the GPL
SDK gives caller-side hooks but no callee/regmap source), and (2) there is no legally
shippable open microcode, so a *truly fully-open* datapath is not achievable today; any
working datapath inherits a `P`-taint dependency on Broadcom's proprietary microcode obtained
from the device. The control-plane (DSA/phylink/MDIO) work is low-risk and should proceed
independently; the offloaded/slow-path Runner datapath should be treated as research-grade
until either a regmap is RE'd and a microcode-licensing path is resolved, or the netfilter-
flowtable-driving approach (drive the proprietary stack via mainline flow-offload, mtk_ppe
precedent) is pursued instead of a from-scratch open datapath.

---

## SOURCES

- 4916 GPL SDK `[SDK]`: asuswrt-merlin.ng `release/src-rt-5.04behnd.4916` (staged on the
  build host). No Runner microcode arrays; no `rdd`/`rdp_subsystem`/`xrdp` source; no
  `drv_rnr_load_microcode`/`request_firmware` for the Runner. Only Serdes/PHY ucode + stock
  kernel ucode. enet GPL source under `bcmdrivers/opensource/net/enet/{impl7,bcm96813}`
  (DUAL/GPL); offload core is prebuilt `.o` blobs + proprietary `rdpa.ko`.
- Stock `rdpa.ko` `[KO]` (unstripped staged copy on the RE container, ELF RE with
  readelf/strings; no aaa/izz, no reboots): `license=Proprietary`; `name=rdpa`;
  `fw_binary_0..7` (8x32 KB = 256 KB) + `fw_predict_0..7` (8x1 KB) + `rdpa_version_fw_*`;
  loaders `drv_rnr_load_microcode/instructions/prediction`; `ag_drv_rnr_inst_*` /
  `RNR_INST*`; CPU `CPU_FEED_RING_*`, `CPU_RECYCLE_RING_*`, `CPU_RING_DESCRIPTOR`,
  `RING_CPU_TX_DESCRIPTOR`; BBH `ag_drv_bbh_rx_*`/`ag_drv_bbh_tx_*`; `_data_path_init`;
  baked build paths `.../BCM6813/target/{rdd,rdp_subsystem}/...`.
- Live device `[DEV]` (read-only): `/sys/module/rdpa/taint` = `P`; `/proc/sys/kernel/tainted`
  = `4097`; `/lib/modules/4.19.294/extra/rdpa.ko` strings = same BCM6813 build paths;
  `find` for any standalone Runner firmware = none.
- Cross-refs: `re-notes/xrdp-cpu-datapath.md` (63138 ring ABI; its GPL-shippable verdict was
  for the OLD 416L05 SDK and does NOT hold for 4916 — corrected here);
  `re-notes/gpl-source-inventory.md` (4916 SDK split: enet GPL, offload core blob/withheld);
  `re-notes/bcm4916-regmap.md` (FDT addresses + IRQ SPIs).
