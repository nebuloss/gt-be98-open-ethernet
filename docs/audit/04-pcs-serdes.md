# 04 — XPORT 10G PCS / SerDes (Merlin16 "Shortfin")

Audit of the open phylink PCS driver for the Broadcom BCM4916 / BCM6813 XPORT
multi-gig line side.

Files audited (line-by-line):

- `driver/pcs/pcs-bcm-xport.c` (314 lines)
- `driver/pcs/pcs-bcm-xport.h` (21 lines)

Read for accuracy / cross-reference:

- `re-notes/xport-serdes-bringup.md` (the RE oracle for the register sequences)
- `driver/mainline-patches/0005-net-dsa-bcm_sf2-wire-XPORT-serdes-PCS-for-BCM4916-10G.patch`
  (the sole caller: bcm_sf2 wiring)
- `qemu/device-model/bcm4916_sf2.c` (the emulation model whose register
  semantics this driver was validated against)

All `file:line` references below are to the committed source at the time of
audit. Where the code carries a placeholder, guess, or a value that is
hardcoded pending live silicon, it is called out explicitly.

---

## 1. Purpose

This subsystem is a **register-mapped `phylink_pcs`** that owns the *line side*
(PCS + SerDes physical-medium-dependent) of the three multi-gig ports on the
BCM4916 (the ASUS GT-BE98's eth0 / eth1 / eth3, DSA ports 5/6/7). Those ports
do not hang off the 1G UNIMAC/EGPHY path that mainline `bcm_sf2` already
supports; they hang off the **XPORT/XLMAC MAC wrapper**, whose line side is a
**Merlin16-Shortfin SerDes** (3 cores) fronted by an **MPCS 10G PCS** block.
Mainline has no driver for any of these blocks, so on real silicon `bcm_sf2`
cannot bring those ports to link at all (`pcs-bcm-xport.c:6-14`; the coverage
gap is `re-notes/dts-upstream-rebase.md` sec 4).

Where it sits in the datapath:

```
   phylink (bcm_sf2 DSA switch driver)
        │  .mac_select_pcs → this PCS   .mac_link_up → XLMAC enable (bcm_sf2)
        ▼
  ┌───────────────────────────┐
  │  bcm_xport_pcs (THIS)      │   line-side link owner
  │   ├── MPCS 10G PCS regs    │   (64/66b PCS lock)
  │   └── Merlin16 SerDes regs │   (PMD lane/PLL/CDR lock)
  └───────────────────────────┘
        │ (electrical) line side ── SFP / external 10G PHY / fixed-link
```

Crucial scope boundary: **this PCS does not move packets.** The XPORT/XLMAC MAC
config and the actual TX/RX enable live on the MAC side in `bcm_sf2`
(`bcm_sf2_xport_mac_enable`, patch 0005). The packet datapath itself is the
entirely separate **Runner conduit** (see `docs/audit/01-runner-datapath.md`).
This driver's only job is: bring the MPCS + SerDes out of reset, (on real HW)
load the SerDes microcode, and report `link/speed/duplex` up to phylink so the
switch's `mac_link_up` fires. It is the equivalent of `pcs-lynx.c` for this SoC
(`re-notes/xport-serdes-bringup.md:102-107`).

**The load-bearing caveat, stated up front:** the Merlin16-Shortfin SerDes will
**not** achieve PMD lock on real hardware until a ~31 KB proprietary microcode
image is PRAM-loaded into the SerDes micro-sequencer. That blob is
non-redistributable (same class as the Runner microcode). This driver performs
the register bring-up *surrounding* that load and polls for lock, but the load
itself is an unimplemented TODO (`bcm_xport_pcs_load_firmware`,
`pcs-bcm-xport.c:164-175`). On QEMU the model fakes the lock so the whole
phylink path exercises end-to-end; on real silicon, link will stay down until a
user supplies the blob. This boundary is the single most important thing to
understand about the subsystem (see §5, §6).

---

## 2. Architecture & data flow

### 2.1 Hardware blocks driven

Two register windows, both mapped by the caller (`bcm_sf2`) from DT phandles and
handed to `bcm_xport_pcs_create`:

| Window | Base (real HW) | Stride / layout | Purpose |
|---|---|---|---|
| SerDes core regs | `0x837ff500` | 3 cores × `0x100` | Merlin16-Shortfin PMD control/status |
| MPCS 10G PCS | `0x828c4000` | single reg used, `MPCS_REG @ +0xf8` | 10G PCS reset/lock |

Bases are documented in the header (`pcs-bcm-xport.h:5`) and the top-of-file
comment (`pcs-bcm-xport.c:19-29`). **Important:** the driver never hardcodes
these bases — it takes already-mapped `void __iomem *` pointers
(`serdes_base`, `mpcs_base`). The bases above are informational; the real
mapping is resolved by `bcm_sf2_xport_pcs_setup()` in patch 0005 from the
`brcm,serdes` / `brcm,mpcs` DT phandles. (On QEMU the real MPCS base
`0x828c4000` collides with the rdpa Runner window, so the model relocates all
three windows to a separate peripheral hole and the driver finds them purely via
DT — `re-notes/xport-serdes-bringup.md:129-138`.)

A third window, the **XPORT/XLMAC MAC core** at `0x837f0000`, is *not* touched by
this PCS driver — it is mapped separately (`brcm,xport` phandle) and driven by
`bcm_sf2` for MAC enable/speed. It is out of scope here but explains why
`pcs_link_up` is essentially empty (§4).

### 2.2 SerDes core register map (per core, stride `0x100`)

From `pcs-bcm-xport.c:56-87`, cross-checked against
`re-notes/xport-serdes-bringup.md:71-75`:

| Offset | Macro | Meaning |
|---|---|---|
| `+0x04` | `SERDES_INDIR_ACC_ADDR` | indirect-access address (**defined, never used**) |
| `+0x08` | `SERDES_INDIR_ACC_MASK` | indirect-access mask (**defined, never used**) |
| `+0x0c` | `SERDES_CONTROL` | `serdes_control_t`: iddq / resets / refsel / lane-mux |
| `+0x10` | `SERDES_STATUS` | rx_sigdet / cdr_lock / link_status / pll_lock |
| `+0x20` | `SERDES_AN_STATUS` | autoneg status (**defined, never read**) |
| `+0x24` | `SERDES_STATUS_1` | per-speed nibbles, one bit/lane |
| `+0xf0` | `SERDES_INDIR_ACC_CNTRL` | indirect-access control (**defined, never used**) |

`SERDES_CONTROL` bit fields (`pcs-bcm-xport.c:66-73`):

| Bit(s) | Macro | Meaning | Used by driver? |
|---|---|---|---|
| 0 | `SC_IDDQ` | power-down (IDDQ) | yes — cleared in core_init |
| 1 | `SC_REFCLK_RESET` | reference-clock reset | yes — cleared in core_init |
| 2 | `SC_SERDES_RESET` | SerDes core reset | yes — cleared in core_init |
| [5:3] | `SC_REFSEL_SHIFT` | refclk select | **no (defined only)** |
| [10:6] | `SC_PRTAD_SHIFT` | MDIO port address | **no (defined only)** |
| [15:11] | `SC_LN_OFFSET_SHIFT` | lane→port mux (`serdes_ln_offset`) | **no (defined only)** |
| 28 | `SC_COMCLK_ENABLE` | common-clock enable | yes — set in core_init |

`SERDES_STATUS` bit fields (`pcs-bcm-xport.c:75-79`):

| Bit | Macro | Meaning | Used? |
|---|---|---|---|
| 0 | `SS_RX_SIGDET` | RX signal detect | no (only in QEMU model) |
| 1 | `SS_CDR_LOCK` | CDR lock | **no (defined only)** |
| 2 | `SS_LINK_STATUS` | PMD link status | **yes — the link gate** |
| 3 | `SS_PLL_LOCK` | PLL lock | **no (defined only)** |

`SERDES_STATUS_1` per-speed nibbles (`pcs-bcm-xport.c:81-87`) — one bit per lane
inside each speed nibble; the driver only ever reads lane 0 (bit at the nibble
base):

| Shift | Macro | Speed | Decoded by get_state? |
|---|---|---|---|
| 0 | `SS1_10M_SHIFT` | 10M | no (defined only) |
| 4 | `SS1_100M_SHIFT` | 100M | no (defined only) |
| 8 | `SS1_1G_SHIFT` | 1G | yes |
| 12 | `SS1_2P5G_SHIFT` | 2.5G | yes |
| 16 | `SS1_5G_SHIFT` | 5G | yes |
| 20 | `SS1_10G_SHIFT` | 10G | yes |

### 2.3 MPCS register map

Only one register is used, `MPCS_REG = mpcs_base + 0xf8` (`MPCS_REG_OFF`,
`pcs-bcm-xport.c:89-96`):

| Bit | Macro | Meaning | Used? |
|---|---|---|---|
| 0 | `MPCS_PMD_RX_LOCK` | PMD rx-lock | **yes — ANDed into link** |
| 1 | `MPCS_SIGNAL_DETECT` | signal detect | no (only in QEMU model) |
| 2 | `MPCS_TX_CLK_VLD` | tx clock valid | **no (defined only)** |
| 3 | `MPCS_FG_POR_RSTB` | functional-group POR reset (deassert) | yes — reg_init |
| 4 | `MPCS_FG_CLK_EN` | functional-group clock enable | yes — reg_init |
| 5 | `MPCS_FG_REFCLK_RSTB` | functional-group refclk reset (deassert) | yes — reg_init |

### 2.4 Control flow (the lifecycle phylink drives)

1. **Probe / create** — `bcm_sf2` calls `bcm_xport_pcs_create()`
   (`pcs-bcm-xport.c:283`), which allocates the private struct, stores the two
   mapped windows + the `emulated` flag, wires `pcs.ops`, sets `pcs.poll = true`
   (there is **no PCS interrupt** — phylink polls `get_state`), and advertises
   `supported_interfaces` = {10GBASER, USXGMII, 2500BASEX}. Returns
   `&xp->pcs`; `bcm_sf2` stashes it and returns it from `.mac_select_pcs`.
2. **Enable** — when phylink brings the PCS up, `.pcs_enable`
   (`bcm_xport_pcs_enable`, `:177`) runs the bring-up: MPCS reset release →
   SerDes core 0 init → SerDes core 1 init → firmware load (stub) for both
   cores.
3. **Config** — `.pcs_config` (`:191`) validates the interface mode and, on real
   HW, would program the per-mode speed select. Today it only accepts the mode
   and logs (10GBASE-R needs nothing further; USXGMII/2500BASEX programming is
   stubbed).
4. **Poll state** — because `poll = true`, phylink periodically calls
   `.pcs_get_state` (`:214`), which reads SerDes `STATUS` + `STATUS_1` and MPCS
   `MPCS_REG`, computes `link = link_status && pmd_rx_lock`, decodes speed, and
   reports it. When this returns `link = true`, phylink fires the switch's
   `mac_link_up`.
5. **Link up** — `.pcs_link_up` (`:250`) is essentially a no-op (only a debug
   log); the MAC-side XLMAC enable happens in `bcm_sf2`'s `mac_link_up`.

There is **no `.pcs_an_restart`** implemented (the ops table at `:264-269` has
only enable/config/get_state/link_up), even though the prompt's checklist and
USXGMII/2500BASEX both involve in-band AN — see §5.

### 2.5 Port → core/lane mapping

Per the comment at `pcs-bcm-xport.c:118-123` (from `re-notes/bcm4916-regmap.md`):
port5 eth0 / port6 eth1 → SerDes **core 0**; port7 eth3 → SerDes **core 1**;
**lane 0** throughout. This is why `pcs_enable` initialises cores 0 and 1
(not the third physical core) and `pcs_get_state` reads **core 0 only** (the
10GBASE-R "fixed path" default). The lane→port mux register field
(`SC_LN_OFFSET_SHIFT`) that would generalise this is defined but never written
(§5) — the single-lane core-0 path is all that is exercised.

---

## 3. Data structures

### 3.1 `struct bcm_xport_pcs` (`pcs-bcm-xport.c:98-104`)

The one private object; one instance per switch (created once, selected for all
three XPORT ports).

| Field | Type | Meaning |
|---|---|---|
| `pcs` | `struct phylink_pcs` | embedded phylink PCS; `ops`, `poll`, `supported_interfaces` filled in create |
| `serdes` | `void __iomem *` | mapped SerDes window (base `0x837ff500`); indexed `core*0x100 + off` |
| `mpcs` | `void __iomem *` | mapped MPCS window (base `0x828c4000`); only `+0xf8` used |
| `dev` | `struct device *` | for `dev_*` logging |
| `emulated` | `bool` | QEMU flag; today it *only* suppresses the "no microcode" warning in `load_firmware` — it does **not** change any register logic |

`to_bcm_xport_pcs(p)` (`:106`) is the standard `container_of` from the embedded
`phylink_pcs` back to the wrapper.

Note the struct carries **no per-core / per-port state, no lock, and no cached
speed/mode** — every op re-reads hardware. There is no `int core` selection
member: `get_state` hardcodes `core = 0` locally (`:219`).

### 3.2 Register-map macros

All register offsets, bit macros, and per-speed shift macros are plain
`#define`s (`pcs-bcm-xport.c:56-96`), enumerated in §2.2/§2.3 with their
used/unused status. There are no C `struct` overlays for the registers — access
is via the `serdes_rd/serdes_wr` helpers and raw `readl/writel` on `mpcs`.

### 3.3 `bcm_xport_interfaces[]` (`pcs-bcm-xport.c:271-275`)

`static const phy_interface_t[]` = {`10GBASER`, `USXGMII`, `2500BASEX`}. Iterated
in `create` to set `pcs.supported_interfaces`, and mirrored (independently) by
the `switch` in `pcs_config`. **These two lists must be kept in sync by hand** —
there is no single source of truth (§5).

### 3.4 `bcm_xport_pcs_ops` (`pcs-bcm-xport.c:264-269`)

`static const struct phylink_pcs_ops`: `.pcs_enable`, `.pcs_config`,
`.pcs_get_state`, `.pcs_link_up`. No `.pcs_disable`, no `.pcs_an_restart`,
no `.pcs_link_down`, no `.pcs_pre_config`/`.pcs_post_config`.

---

## 4. Function reference (file order)

### 4.1 `serdes_rd` — `pcs-bcm-xport.c:108-111`

```c
static inline u32 serdes_rd(struct bcm_xport_pcs *xp, int core, u32 off)
```

Reads one 32-bit SerDes register: `readl(xp->serdes + core*SERDES_CORE_STRIDE
+ off)`. `core` selects the 0x100-strided core; `off` is one of the
`SERDES_*` offsets. No bounds check on `core` (caller always passes 0/1).
**Callees:** `readl`. **Callers:** `bcm_xport_serdes_core_init`,
`bcm_xport_pcs_get_state`.

### 4.2 `serdes_wr` — `pcs-bcm-xport.c:113-116`

```c
static inline void serdes_wr(struct bcm_xport_pcs *xp, int core, u32 off, u32 v)
```

Writes one 32-bit SerDes register (`writel`). Symmetric with `serdes_rd`.
**Callees:** `writel`. **Callers:** `bcm_xport_serdes_core_init` only.

### 4.3 `bcm_xport_mpcs_reg_init` — `pcs-bcm-xport.c:131-141`

```c
static void bcm_xport_mpcs_reg_init(struct bcm_xport_pcs *xp)
```

Deasserts the MPCS functional-group resets to bring the 10G PCS out of reset.
Read-modify-writes `MPCS_REG` (`mpcs + 0xf8`) **three times in strict order**:
first OR-in `MPCS_FG_CLK_EN` (bit4), then OR-in `MPCS_FG_POR_RSTB` (bit3), then
OR-in `MPCS_FG_REFCLK_RSTB` (bit5) — each with its own `writel`. Models
`mpcs_reg_init` from the SDK (`re-notes/xport-serdes-bringup.md:54-67`).

- **Why the split writes:** the SDK sequence releases clk_en → por_rstb →
  refclk_rstb in that order so the block powers up cleanly; the driver preserves
  the ordering by doing three separate register writes.
- **Hardware sequencing constraint:** order matters (per the oracle). **There
  are no `udelay`/settling delays between the writes** and no poll of
  `pmd_rx_lock`/`signal_detect` afterward — the SDK's `mpcs_reg_init` polls for
  lock after the release; this driver does not, deferring all lock observation
  to phylink's periodic `get_state` (§5).
- **Registers touched:** `MPCS_REG @ +0xf8` (RMW ×3).
- **Callers:** `bcm_xport_pcs_enable`. **Callees:** `readl`, `writel`.

### 4.4 `bcm_xport_serdes_core_init` — `pcs-bcm-xport.c:149-162`

```c
static void bcm_xport_serdes_core_init(struct bcm_xport_pcs *xp, int core)
```

Releases one SerDes core from reset. Reads `SERDES_CONTROL`, then does **three
sequential writes**: (1) clear `SC_IDDQ` and set `SC_COMCLK_ENABLE` (power up +
enable common clock); (2) clear `SC_REFCLK_RESET`; (3) clear `SC_SERDES_RESET`.
Models `_serdes_core_init` (`re-notes/xport-serdes-bringup.md:69-85`,
`[SDK phy_drv_shortfin.c:77]`).

- **Why:** the Merlin PMD must come up in order — power/common-clock first, then
  refclk, then the core reset — so downstream logic sees a running clock before
  reset deassertion.
- **What the SDK does that this does NOT:** the SDK `_serdes_core_init` also
  **PRAM-loads the Merlin microcode, verifies CRC, runs `PMD_setup_50_10p3125_VCO`
  and `datapath_reset_core`** after the reset release. This driver stops after
  the reset release; the microcode load is delegated to the (stub)
  `bcm_xport_pcs_load_firmware` called separately by `pcs_enable`, and the PLL
  VCO setup / datapath reset are **not performed at all** (§5, §6).
- **Hardware sequencing constraint:** the three writes must stay ordered; again
  there are **no inter-write delays** and **no PLL-lock poll** (`SS_PLL_LOCK` is
  never read). On real silicon a refclk→reset settling delay and a PLL-lock wait
  are normally required.
- **Registers touched:** `SERDES_CONTROL @ core*0x100 + 0x0c` (1 read, 3
  writes).
- **Callers:** `bcm_xport_pcs_enable` (for core 0 and core 1). **Callees:**
  `serdes_rd`, `serdes_wr`.

### 4.5 `bcm_xport_pcs_load_firmware` — `pcs-bcm-xport.c:167-175`

```c
static int bcm_xport_pcs_load_firmware(struct bcm_xport_pcs *xp, int core)
```

**The proprietary-blob boundary, and the biggest gap in the subsystem.** This is
where the ~31 KB Merlin16-Shortfin PMD microcode would be PRAM-loaded into the
SerDes micro-sequencer (and CRC-verified) so the lanes can actually lock. Today
it is a stub:

- If `xp->emulated` → returns 0 immediately (QEMU fakes lock, no blob needed).
- Otherwise → `dev_warn_once(...)` that the microcode load is not implemented and
  "link will not come up on real HW", then returns 0.

It never touches hardware and **always returns 0** (even the not-implemented
path is a success return; the caller ignores the value anyway). The TODO comment
(`:164-166`) records what must happen: PRAM-load the proprietary, non-redis­
tributable image via the SDK's load sequence before lanes lock.

- **The open/closed boundary explained:** everything *around* the microcode load
  is open register poking (reset release, lock polling). The microcode itself is
  a closed Broadcom blob (same non-redistributable class as the Runner
  microcode). The open driver deliberately draws the line here: it will host the
  load mechanism, but the image must be user-supplied on real silicon. Firmware
  handling is otherwise generic — no blob is embedded, extracted, or referenced
  by symbol/offset in this file.
- **Callers:** `bcm_xport_pcs_enable` (cores 0 and 1; return value discarded).
  **Callees:** `dev_warn_once`.

### 4.6 `bcm_xport_pcs_enable` — `pcs-bcm-xport.c:177-189`

```c
static int bcm_xport_pcs_enable(struct phylink_pcs *pcs)
```

`.pcs_enable` op. Runs the full line-side bring-up, unconditionally, for **both**
SerDes cores used by the XPORT ports (comment `:181-182`): `mpcs_reg_init` →
`serdes_core_init(0)` → `serdes_core_init(1)` → `load_firmware(0)` →
`load_firmware(1)`. Always returns 0.

- **Why both cores:** because a single PCS object is selected for all three
  ports (which map onto cores 0 and 1), `pcs_enable` doesn't know which port is
  coming up, so it initialises both. The third physical SerDes core is never
  initialised (no XPORT port maps to it — §2.5).
- **Ordering constraint:** MPCS reset-release before SerDes core init follows the
  oracle bring-up order. `load_firmware` runs after core reset release (matching
  the SDK, where PRAM load happens inside core init after reset deassert).
- **Idempotency / re-entry:** the op re-runs the full RMW sequence every time
  phylink enables the PCS; since all writes are OR-in-bits reset releases, a
  re-run is harmless, but there is no `.pcs_disable` to re-assert resets, so the
  cores are never returned to a low-power state.
- **Callers:** phylink core (via `bcm_xport_pcs_ops`). **Callees:**
  `bcm_xport_mpcs_reg_init`, `bcm_xport_serdes_core_init` (×2),
  `bcm_xport_pcs_load_firmware` (×2).

### 4.7 `bcm_xport_pcs_config` — `pcs-bcm-xport.c:191-212`

```c
static int bcm_xport_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
                                phy_interface_t interface,
                                const unsigned long *advertising,
                                bool permit_pause_to_mac)
```

`.pcs_config` op. Validates `interface`: accepts `10GBASER`, `USXGMII`,
`2500BASEX` (emits a `dev_dbg` and returns 0); returns `-EINVAL` for anything
else.

- **What it does NOT do:** for USXGMII/2500BASEX the SDK writes the per-mode
  speed-select / an_config registers (the `WAN__AN_X4_USXGMII_*` regs, speed
  code in bits[11:9]) — this is explicitly **stubbed**; the comment
  (`:202-205`) says the per-mode programming is "handled here on real HW" but no
  register is written. Only 10GBASE-R (which needs no extra programming) is
  actually functional. `neg_mode`, `advertising`, and `permit_pause_to_mac` are
  all **ignored**.
- **Registers touched:** none (log only).
- **Callers:** phylink core. **Callees:** `phy_modes` (for the log string).

### 4.8 `bcm_xport_pcs_get_state` — `pcs-bcm-xport.c:214-248`

```c
static void bcm_xport_pcs_get_state(struct phylink_pcs *pcs,
                                    unsigned int neg_mode,
                                    struct phylink_link_state *state)
```

`.pcs_get_state` op — the polled link/speed reporter (called periodically
because `poll = true`). Reads three registers from **core 0 only** (`core = 0`
hardcoded, `:219`): `SERDES_STATUS`, `SERDES_STATUS_1`, and MPCS `MPCS_REG`.

- **Link:** `link = (sstat & SS_LINK_STATUS) && (mpcs & MPCS_PMD_RX_LOCK)` —
  SerDes PMD link_status (STATUS bit2) AND MPCS rx-lock (bit0). Sets
  `state->link` and `state->an_complete = link`.
- **Duplex:** hardcoded `DUPLEX_FULL` (comment: 10G/serdes is always full
  duplex).
- **Speed:** decodes `SERDES_STATUS_1` lane-0 bits, highest-first: 10G → 5G →
  2.5G → 1G; **default fallthrough = `SPEED_10000`** (for 10GBASE-R). Note 10M
  and 100M nibbles are never decoded.
- **Pause:** `state->pause |= MLO_PAUSE_TX | MLO_PAUSE_RX` — **hardcoded both
  directions**, not read from any AN/register.

Sequencing: purely read-only, order-independent. **No `emulated` gating** — it
always reflects real register values; on QEMU the model sets link_status +
pmd_rx_lock + the 10G STATUS_1 bit once resets are released, so it reads "up";
on real HW without the microcode those bits never assert, so it correctly reads
"down". This is the natural place where the missing firmware manifests as a
permanently-down link.

- **Registers touched:** `SERDES_STATUS @ +0x10`, `SERDES_STATUS_1 @ +0x24`
  (core 0), `MPCS_REG @ +0xf8`.
- **Callers:** phylink core (poll loop). **Callees:** `serdes_rd`, `readl`.

Audit-relevant behaviours: `an_complete = link` conflates "link up" with "AN
complete" (wrong for the in-band-AN modes); speed/duplex/pause never consult
`SERDES_AN_STATUS @ +0x20` (defined but never read); `SS_PLL_LOCK`,
`SS_CDR_LOCK`, `SS_RX_SIGDET`, `MPCS_SIGNAL_DETECT`, `MPCS_TX_CLK_VLD` are all
available but not folded into the link decision. See §5.

### 4.9 `bcm_xport_pcs_link_up` — `pcs-bcm-xport.c:250-262`

```c
static void bcm_xport_pcs_link_up(struct phylink_pcs *pcs, unsigned int neg_mode,
                                  phy_interface_t interface, int speed, int duplex)
```

`.pcs_link_up` op — effectively a **no-op**: emits a `dev_dbg` and returns. The
comment (`:259-261`) documents *why*: the XLMAC speed/enable is done on the MAC
side by `bcm_sf2`'s `bcm_sf2_xport_mac_enable` (XLMAC_MODE `SPEED_MODE` +
SOFT_RESET release + TX/RX enable), so there is nothing SerDes-side to do once
PMD is locked for the fixed 10GBASE-R / USXGMII modes.

- **Registers touched:** none. **Callers:** phylink core. **Callees:**
  `phy_modes`.

### 4.10 `bcm_xport_pcs_create` — `pcs-bcm-xport.c:283-310`

```c
struct phylink_pcs *bcm_xport_pcs_create(struct device *dev,
                                         void __iomem *serdes_base,
                                         void __iomem *mpcs_base, bool emulated)
```

**The single public entry point** (exported `EXPORT_SYMBOL_GPL`, `:310`;
declared in `pcs-bcm-xport.h:15-18`). `devm_kzalloc`s the private struct
(returns `ERR_PTR(-ENOMEM)` on failure), stores the two pre-mapped windows +
`dev` + `emulated`, points `pcs.ops` at `bcm_xport_pcs_ops`, sets
`pcs.poll = true` (no PCS IRQ), sets each of the three `supported_interfaces`
bits by iterating `bcm_xport_interfaces[]`, logs an info line, and returns
`&xp->pcs`.

- **Lifetime:** `devm_`-managed against the caller's `dev`, so it is freed when
  `bcm_sf2` unbinds; there is no explicit destroy.
- **Callers:** `bcm_sf2_xport_pcs_setup()` in patch 0005 (`:262` of that patch),
  which `of_iomap`s the windows from the `brcm,serdes` / `brcm,mpcs` phandles,
  propagates `brcm,runner-emulated` → `emulated`, and stashes the result in
  `priv->xport_pcs`, returned from `.mac_select_pcs` for ports 5/6/7.
- **Callees:** `devm_kzalloc`, `__set_bit`, `dev_info`.

Module boilerplate: `MODULE_DESCRIPTION` / `MODULE_LICENSE("GPL")`
(`:312-313`). There is **no `platform_driver` / module_init** — this is a
library object linked into (or selected by) `bcm_sf2`, not a standalone
driver.

---

## 5. Audit findings

Severity is the auditor's judgement for the stated goal (reimplement full stock
10G behaviour on real silicon).

### High

- **H1 — SerDes PMD microcode load is unimplemented (`:167-175`).** Without the
  ~31 KB Merlin16-Shortfin PRAM image, no lane will lock on real hardware, so
  `link_status`/`pmd_rx_lock` never assert and `get_state` reports link-down
  forever. This is acknowledged (TODO + `dev_warn_once`) and is the fundamental
  open/closed boundary, but it means the subsystem is **non-functional on
  silicon today** — only the QEMU-faked path works. The stub also returns 0 on
  the failure path, so callers cannot detect the missing blob programmatically.

- **H2 — No PLL-lock / settling waits in the reset-release sequences
  (`:131-141`, `:149-162`).** Both `mpcs_reg_init` and `serdes_core_init` issue
  back-to-back `writel`s with no `udelay` and never poll `SS_PLL_LOCK` (bit3,
  defined but never read) after releasing the SerDes reset, nor
  `pmd_rx_lock`/`signal_detect` after releasing the MPCS resets. The SDK oracle
  polls for lock after the release (`re-notes/xport-serdes-bringup.md:60-62,
  77-81`). On real silicon, deasserting the core reset before the PLL has locked
  can leave the PMD in an indeterminate state. The driver leans entirely on
  phylink's later `get_state` polling to observe lock, which works for
  *reporting* but not for *sequencing* the bring-up.

- **H3 — `serdes_core_init` omits `PMD_setup_..._VCO` and `datapath_reset_core`
  (`:149-162`).** The SDK `_serdes_core_init` runs a VCO/PLL setup and a
  datapath reset *after* reset release; the open driver does neither. Even with
  the microcode loaded, the PMD may not reach the operating rate without the VCO
  configuration. Flagged in the notes (§4 of the RE note) but not in-code.

### Medium

- **M1 — USXGMII / 2500BASEX per-mode programming stubbed (`:191-212`).**
  `pcs_config` accepts these modes but writes no registers; the SDK requires the
  `an_config` (`0xc4b0/0xc4b1/0xc4b2`, speed bits[11:9]) programming and the MPCS
  PCS A/B select for USXGMII. So while the PCS is *selected* for eth0/eth3, only
  10GBASE-R actually functions; USXGMII/2500BASEX are proven only to the
  PCS-select stage (`re-notes/xport-serdes-bringup.md:168-170`).

- **M2 — `an_complete` conflated with `link`, AN status never read
  (`:232`).** `state->an_complete = link` and speed/pause are derived from
  fixed defaults / non-AN status. For the in-band-AN modes (USXGMII, 2500BASE-X)
  this is incorrect: `SERDES_AN_STATUS @ +0x20` is defined (`:62`) but never
  read, and there is no `.pcs_an_restart` op at all (`:264-269`) even though the
  audit brief lists AN restart as expected PCS functionality.

- **M3 — Pause is hardcoded (`:247`).** `state->pause |= MLO_PAUSE_TX |
  MLO_PAUSE_RX` regardless of negotiation or `permit_pause_to_mac` (which
  `pcs_config` ignores). Flow control will always be advertised as symmetric.

- **M4 — `get_state` reads core 0 only; lane→port mux never programmed
  (`:219`, `SC_LN_OFFSET_SHIFT` unused).** With `core` hardcoded to 0,
  port7/eth3 (which maps to core 1 per `:118-123`) is never actually reported
  from its own core, and the `serdes_ln_offset` lane→port mux field is defined
  but never written. Only the single-lane core-0 10GBASE-R path is real; the
  multi-lane / cascade ports are un-handled (`re-notes/xport-serdes-bringup.md:
  171-173`).

### Low / informational

- **L1 — Register bases in comments are unverified against live silicon.** The
  SerDes `0x837ff500`, MPCS `0x828c4000`, and the offsets are RE'd from the SDK
  6813A0 arrays and modelled in QEMU, **not confirmed against a live regdump**
  (device work is read-only — `re-notes/xport-serdes-bringup.md:174-177`,
  no-connectivity-break rule). Treat every offset here as "matches the SDK
  oracle" rather than "confirmed on hardware".

- **L2 — Many bit macros defined but never used** (dead-but-documented):
  `SERDES_INDIR_ACC_ADDR/MASK/CNTRL`, `SERDES_AN_STATUS`, `SC_REFSEL_SHIFT`,
  `SC_PRTAD_SHIFT`, `SC_LN_OFFSET_SHIFT`, `SS_CDR_LOCK`, `SS_PLL_LOCK`,
  `SS_RX_SIGDET`, `MPCS_SIGNAL_DETECT`, `MPCS_TX_CLK_VLD`, `SS1_10M_SHIFT`,
  `SS1_100M_SHIFT` (verified by usage count). They document the register layout
  but signal how much of the block is not yet driven.

- **L3 — Two independent copies of the supported-interface list** (`:271-275`
  array vs the `switch` in `pcs_config` `:198-201`) must be kept in sync by
  hand; adding a mode in one place without the other silently breaks selection
  or config.

- **L4 — `emulated` flag is under-used (`:103`, only read at `:169`).** It only
  suppresses the firmware warning. That happens to be fine because the QEMU model
  fakes the register values, but it means there is no explicit emulation branch —
  correctness on QEMU depends entirely on the model matching the read paths.

- **L5 — `load_firmware` return value ignored (`:186-187`).** `pcs_enable`
  discards the `int` return; if a real load were added and failed, the bring-up
  would still report success.

- **L6 — No `.pcs_disable` / reset re-assert.** Cores are brought out of reset
  and never returned to IDDQ/reset on link-down or unbind; only `devm` teardown
  frees the struct (registers keep their last-written state).

- **L7 — No `core` bounds check in `serdes_rd/serdes_wr` (`:108-116`).** Safe
  today (callers pass 0/1) but would compute an out-of-window pointer for
  `core ≥ 3`.

- **L8 — Doc/demo inconsistency (informational).** The port map comment
  (`:118-123`) assigns eth3 → core 1 (a serdes port), but the QEMU boot result
  (`re-notes/xport-serdes-bringup.md:150`) shows `eth3 speed=1000 (rgmii)` and
  eth1 as the 10G XPORT port. This is the demo DTS wiring only one XPORT port,
  not a driver bug, but the eth-name↔port↔core identity is not yet pinned on
  hardware (see O5).

---

## 6. Open questions / unknowns

1. **The microcode load mechanism (O1).** How exactly the ~31 KB PMD image is
   PRAM-loaded on this SoC (the register/window sequence, CRC verification,
   post-load VCO/datapath steps) is known only from the closed SDK and is not
   implemented here. Whether it can be driven purely through the two windows this
   driver maps, or needs the XPORT/indirect-access path
   (`SERDES_INDIR_ACC_*`, defined but unused), is undetermined from code+notes.
   Firmware sourcing on real silicon is left to the user (extract from their own
   vendor SerDes lib / licensed image) — this doc deliberately keeps that
   generic.

2. **Real lock timing / settling requirements (O2).** Because QEMU fakes lock the
   instant resets are released, the actual delays and PLL/CDR lock-wait loops the
   silicon needs between the reset-release writes are unknown. H2/H3 cannot be
   resolved without a live board.

3. **USXGMII / 2500BASE-X programming (O3).** The exact `an_config` register
   values (0xc4b0/0xc4b1/0xc4b2 and the PCS A/B select) and the in-band AN
   handshake behaviour are recorded in the RE note but not validated; eth0/eth3
   have never linked past PCS-select. Whether `SERDES_AN_STATUS` must be polled
   and how `an_complete` should be derived is open.

4. **Register bases/offsets vs silicon (O4).** All bases and bitfields are from
   the SDK 6813A0 arrays + QEMU model, never confirmed against a live regdump
   (read-only device policy). A single live `devmem`/regdump pass would validate
   or correct L1.

5. **DSA-port → XLMAC-core → SerDes-core identity (O5).** The mapping (port5/6 →
   core0, port7 → core1, lane0; `bcm_sf2_xlmac_off(6)` = quad1 port2 = `0x4800`)
   is assumed 1:1 and unverified on hardware
   (`re-notes/xport-serdes-bringup.md:176-177`). `get_state` currently hardcodes
   core 0, so multi-core correctness is untested.

6. **Third SerDes core (O6).** Hardware has 3 cores; only 0 and 1 are ever
   initialised. Whether the third core is unused on the GT-BE98 or serves a port
   not yet wired is not stated in code+notes.

---

*Sources cross-referenced during audit: `driver/pcs/pcs-bcm-xport.{c,h}`,
`re-notes/xport-serdes-bringup.md`,
`driver/mainline-patches/0005-net-dsa-bcm_sf2-wire-XPORT-serdes-PCS-for-BCM4916-10G.patch`
(the caller), and `qemu/device-model/bcm4916_sf2.c` (the model the register
read/write semantics were validated against). No device IPs, MACs, hostnames,
credentials, or proprietary microcode symbol offsets are reproduced here;
firmware handling is described generically.*
