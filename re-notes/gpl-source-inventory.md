# GPL Source Inventory — BCM4916 / ASUS GT-BE98 open-offload base

Goal: find the matching 4912/4916-era **XRDP** GPL source for (a) XRDP descriptor
bitfields + register offsets, (b) the GPLv2 Runner microcode arrays, (c) the GPL
XRDP enet + rdpa + flow-accel (pktflow/fcache/cmdlist) sources.

All fetch/extract/grep was done on **dev-build**; nothing built or
downloaded on dev-code. Verdicts below are from actually grepping the trees, not
from product pages.

---

## TL;DR / Recommendation

- **Best source found:** `RMerl/asuswrt-merlin.ng`, tree
  **`release/src-rt-5.04behnd.4916`** (the literal BE/WiFi-7 "behnd" SDK for the
  **4916** chip). SDK tag `bcm963xx_5.04L.04p3 (17.10.369.39012)`, BB0B, 2024-04-25.
  This is the exact GT-BE98-class GPL drop (ASUS ships Merlin's base from this SDK).
- **Staged on dev-build at:**
  `~/re-sdk/asuswrt-merlin.ng/release/src-rt-5.04behnd.4916/` (sparse checkout:
  `bcmdrivers shared kernel`, 1.3 GB).
- **GPL-source-vs-blob verdict for the OFFLOAD CONTROL PLANE:** **MOSTLY BLOB.**
  - The **Ethernet driver datapath is full GPL source** (enet `impl7` and the
    even-richer `enet/bcm96813/` board dir — 46+ .c files, zero blobs) and IS
    directly reusable for a mainline enet driver.
  - The **flow-acceleration engine is NOT source**: `pktflow`, `cmdlist`, `archer`
    (24 objects + crossbow), `pktrunner`, `bpm`, and the **rdpa core** all ship as
    **prebuilt aarch64 ELF `.o` blobs**. Only thin GPL shims are source.
  - The **Runner microcode (rdd firmware C arrays) is ABSENT entirely** — none in
    the tree (the only "ucode" present is unrelated Serdes/Merlin PHY firmware).
  - The **core rdpa API headers** (`rdpa_api.h`, `rdpa_ip_class.h`, `rdpa_ucast.h`,
    `rdpa_port.h` — the descriptor bitfields/regmap we wanted) are **also stripped**;
    the enet driver `#include <rdpa_api.h>` resolves to a build-time include path that
    ASUS does not ship. Only the GPL middleware headers (`rdpa_drv.h`, `rdpa_cmd_*.h`,
    `rdpa_mw_*.h`, GPON/EPON `rdpa_gpl_ext`) are present.

**Bottom line:** use `src-rt-5.04behnd.4916` as the base for the **open enet driver**
(MAC/UNIMAC/XPORT/SF2/PHY bring-up — full source). For the **HW offload control plane**
there is **no GPL source**: the Runner/Archer flow path, the rdpa core, and the Runner
microcode are blobs/withheld. Offload work must combine (1) the GPL enet driver's
*caller-side* offload hooks (which ARE source and show the API call sequence) with
(2) **binary RE of the 4916's `.o`/`.ko` offload blobs** + the rdpa API headers
recovered from another SDK or from the device binaries.

---

## Existing baseline (already on dev-build) — does NOT cover 4916

`~/re-sdk/broadcom-sdk-416L05/` (from datashed.science; also the only SDK datashed
hosts — confirmed its index has just `broadcom-sdk-416L05/`).

- SDK v4.16.05. RDP **impl1/impl2 only** (`shared/broadcom/rdp/impl1|impl2/rdd`).
- impl2 = **BCM63138 / 63148** (old RDP), enet `impl5`/`impl6`.
- Chip defines top out at `CONFIG_BCM963138/963148/94908/96858`. **Zero hits** for
  4916/6813/4912/6855/6856. This is the older RDP, **not XRDP** — confirms the recon's
  conclusion that 416L05 is the wrong arch for the 4916.

---

## Candidate sources evaluated

| Candidate | URL | Chip / arch | Verdict |
|---|---|---|---|
| **RMerl/asuswrt-merlin.ng** `src-rt-5.04behnd.4916` | github.com/RMerl/asuswrt-merlin.ng (branch `main`) | **BCM4916 / 6813 / 6856, XRDP** (`CONFIG_BCM_XRDP`, `CONFIG_BCM96813`), enet `impl7`+`bcm96813` | **CHOSEN.** Full enet GPL source; offload core = blobs (see split below). Directly git-cloneable, no ASUS portal needed. |
| gnuton/asuswrt-merlin.ng | github.com/gnuton/asuswrt-merlin.ng | Same SDK (GT-BE98 maintainer fork) | Equivalent tree; RMerl upstream used as canonical. |
| HE7086/asuswrt (ZenWiFi Pro ET12) | github.com/HE7086/asuswrt | BCM4912 (older 675x tree) | Useful cross-ref for 4912 but older than the 4916 BE tree; not needed. |
| `broadcom-sdk-416L05` (datashed) | datashed.science/misc/bcm/gpl/ | BCM63138/63148, RDP impl2 | Wrong arch (already have it). Datashed hosts no newer SDK. |
| ASUS GPL portal (dlcdnets) | dlcdnets.asus.com/pub/ASUS/wireless/GT-BE98/ | GT-BE98 | FW zips exist (e.g. `FW_GT_BE98_300610239197.zip`, 72 MB, HTTP 200), but **no `GPL_*` file is published** at the expected paths (all 404). ASUS's GPL is effectively the same SDK that Merlin already mirrors — Merlin is the easier, equivalent route. |
| Broadcom GitHub org | github.com/Broadcom | — | No router GPL SDK (only SDK samples / unrelated). |

`src-rt-5.04behnd.4916` sits alongside `src-rt-5.04axhnd.675x` in the same repo;
`675x` = 4912/6855 AX tree, `behnd.4916` = the WiFi-7 4916 tree (what we want).

---

## Detailed inventory of the chosen tree (`src-rt-5.04behnd.4916`)

### Chip / arch confirmation
- `CONFIG_BCM96813` and `CONFIG_BCM6856`, `CONFIG_BCM_XRDP` present in enet configs.
- `bcmdrivers/opensource/net/enet/bcm96813/` board dir exists (4916 board codename).
- `phy_drv_ext3.c` present (matches our device's EXT3 cascade PHY @ MDIO 9/21).

### GPL SOURCE present (reusable)

**Ethernet driver — FULL SOURCE, the gold:**
`bcmdrivers/opensource/net/enet/impl7/` (46 .c, 0 blobs) and the richer
`bcmdrivers/opensource/net/enet/bcm96813/`. Key files:
- `enet.c`, `enet.h`, `enet_defs.h`, `enet_types.h`, `enet_ethtool.c`, `dt_parsing.c`
- `runner.c/.h`, `runner_common.h`, `runner_standalone.c`, `runner_with_switch.c`,
  `runner_sdev.c`, `enet_inline_runner.h` — the **caller-side** Runner offload hooks
  (descriptor ring setup, the API call sequence into rdpa/runner)
- `archer_enet.c/.h`, `crossbow_enet.c` — caller-side Archer/Crossbow accel hooks
- `sf2.c`, `sf2_cfp.c`, `sf2_*` — the **SF2 switch** driver (matches `RUNNER_SW` /
  `mdio-sf2` in the recon) — directly relevant to the switch/DSA work
- `port.c`, `port_types.c`, `mux_index.c`, `syspvsw.c`, `vlan_tag.c`, `rdp_ring.c`,
  `enet_ring.c`, `enet_swqueue.c`, `enet_blog.c`, `enet_xdp.c`, `enet_macsec.c`,
  `flctl_lite.c`, `dynamic_meters.c`, `fm_nft*.c`
- PHY: `bcmdrivers/opensource/phy/` has `phy_drv_ext3.c` source, but the actual PHY
  driver core (`phy_drv.o`, `phy_drv_brcm.o`, `mac_drv_unimac.o`, `unimac_drv_impl1.o`)
  ships as **prebuilt .o blobs**.

**rdpa/runner GPL glue — SOURCE but only middleware:**
- `bcmdrivers/opensource/char/rdpa_drv/impl1/` — ioctl command layer
  (`rdpa_cmd_ic/port/cpu/filter/br/...`), ships its lib as `rdpa_cmd.o` (blob) + .h
- `bcmdrivers/opensource/char/rdpa_mw/impl1/` — middleware (vlan/qos/blog_parse), lib
  as `rdpa_mw.o` (blob) + .h
- `bcmdrivers/opensource/char/rdpa_gpl_ext/impl1/` — GPON/EPON GPL ext + autogen
  `rdpa_ag_gpon.h`/`rdpa_ag_epon.h` (WAN-only, not our LAN offload)
- `bcmdrivers/opensource/char/archer/impl1/archer_gpl.c`, `crossbow_gpl.c` — GPL shims
- `bcmdrivers/broadcom/char/pktrunner/{impl1/pktrunner_sim.c, shared/pktrunner_fpi.c}`
  — only a simulator + FPI helper, not the real engine

### BLOBS / WITHHELD (the offload control plane)

Prebuilt aarch64 ELF `.o` (verified with `file`), no `.c` anywhere in tree:
- `bcmdrivers/broadcom/char/pktflow/impl1/pktflow.o`  (fcache/flow cache — the L3/L4
  flow learning engine; **no fcache .c exists anywhere**)
- `bcmdrivers/broadcom/char/cmdlist/impl1/cmdlist.o`  (the cmdlist builder — exactly
  the "cmdlist build" path we wanted as source)
- `bcmdrivers/broadcom/char/archer/impl1/` — **24 objects** incl. `archer_ucast.o`,
  `archer_mcast.o`, `archer_host.o`, `crossbow/*.o` (the 4916's primary SW accelerator)
- `bcmdrivers/broadcom/char/pktrunner/impl2/pktrunner.o`  (Runner flow programming)
- `bcmdrivers/broadcom/char/bpm/`, `bp3/`, `vlan/`, `pon_drv/`, `net/gdx/` — blobs
- **rdpa core**: there is **no `rdpa/` or `rdd/` directory at all**; the rdpa runtime is
  the `rdpa_cmd.o`/`rdpa_mw.o` blobs. `rdpa_ip_class`, `rdpa_ucast`, flow-create
  implementation — none present as source.

Total offload blob surface that would need RE: ~2.5 MB of `.o`
(archer + pktrunner + pktflow + cmdlist + bpm + rdpa shims), plus the wl wifi blobs
(separate concern).

### Runner microcode (the GPLv2 C arrays we hoped for)
**ABSENT.** No `*_microcode*`, no `firmware_binary` arrays, no `rdd` runner firmware
anywhere in the tree. Grep for `runner.*firmware|rdd.*microcode|firmware_binary` over
`bcmdrivers/`+`shared/` (excluding the stock linux- tree) returns nothing. The Runner
microcode is delivered separately (loaded from a firmware blob on the device), not in
this GPL drop.

### Core rdpa/XRDP API headers (descriptor bitfields + register offsets)
**ALSO STRIPPED.** The enet driver does `#include <rdpa_api.h>`, `#include "runner.h"`,
`#include "cmdlist_api.h"` — but `rdpa_api.h`, `rdpa_ip_class.h`, `rdpa_ucast.h`,
`cmdlist_api.h` (the real ones) are **not in the tree**; they live on a build-time
include path ASUS omits. `shared/opensource/include/rdp/` contains only
`bl_os_wraper.h` + `rdp_ms1588.h`. So this tree does **not** give us the XRDP
descriptor regmap as source either — that must come from RE of the device binaries /
another SDK.

---

## What this means for the open-offload effort

1. **Open enet driver (first light + switch/PHY):** strong basis. Base it on
   `enet/bcm96813/` + `impl7`, plus `sf2*.c` for the integrated switch and
   `phy_drv_ext3.c`. Cross-check every register against the recon FDT notes
   (`re-notes/bcm4916-regmap.md`). This path is fully GPL.

2. **HW offload (Runner/Archer/rdpa):** **no GPL source exists.** The control plane
   (pktflow/cmdlist/archer/rdpa core) is blobs; the Runner microcode and the rdpa API
   headers are withheld. The GPL enet `runner.c`/`enet_inline_runner.h`/`archer_enet.c`
   give us the **caller side** (which rdpa/runner functions are called, in what order,
   with what args) — valuable as an RE roadmap, but the callee implementations and the
   descriptor/regmap definitions are not present.

3. **Recommended offload strategy = source + binary RE hybrid:**
   - Use the GPL enet caller hooks to enumerate the offload API entry points.
   - RE the 4916's `.o`/`.ko` offload blobs (the 2.5 MB above + the device's
     `bcm_enet.ko`/rdpa modules already pulled in recon) to recover the rdpa API
     structs, descriptor bitfields, and cmdlist format.
   - Recover the Runner microcode from the device firmware partition for offline study
     (it is not redistributable as GPL from any tree found).

---

## Provenance / reproduce
```
ssh <dev-build>
cd ~/re-sdk
git clone --depth 1 --filter=blob:none --sparse https://github.com/RMerl/asuswrt-merlin.ng.git
cd asuswrt-merlin.ng
git sparse-checkout set release/src-rt-5.04behnd.4916/bcmdrivers \
                          release/src-rt-5.04behnd.4916/shared \
                          release/src-rt-5.04behnd.4916/kernel
# tree at: ~/re-sdk/asuswrt-merlin.ng/release/src-rt-5.04behnd.4916/  (1.3 GB)
```
