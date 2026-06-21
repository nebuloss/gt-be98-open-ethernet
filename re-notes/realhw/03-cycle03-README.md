# Cycle 03 — reserved-memory fix (2026-06-21)

**Change:** added `/memreserve/ 0 0x100000` + a `no-map` reserved-memory node to our DTS, covering the
ATF/BL31 secure monitor (services PSCI) + PMC firmware — which upstream `bcm6813.dtsi` entirely lacks
and the vendor reserves. Rebuilt DTB, repackaged (`GT-BE98-mainline-rsvdmem_nand_squashfs.pkgtb`, lzo
22.4 MB kernel). Flashed slot1, ONCE-booted.

## OUTCOME: still no userspace signal ✗ (auto-revert clean ✓)
- Clean revert to committed slot2 in ~70 s (cycles 01/02 were ~60 s — likely just WD-fire timing
  variance, not a reliable progress signal). No `/data` breadcrumb. boot_failed_count=0.

## ★ THE OBSERVABILITY WALL (after 3 blind cycles, 2 real bugs fixed)
Bugs found + fixed via merlin comparison (genuine progress): (1) kernel-too-big → U-Boot FIT overlap →
BOOTM_ERR_RESET; (2) missing reserved-memory → ATF/BL31 clobber → early PSCI/SMP fault. But mainline
still produces ZERO observable signal, and **I can no longer tell whether mainline reaches userspace**:
- The only userspace signal (the `/data` breadcrumb) requires mainline to mount the NAND/UBI `/data`
  volume — mainline may lack the bcmbca NAND pinctrl, so "no breadcrumb" doesn't prove "didn't boot".
- Network reachability requires our (unproven) driver — circular.
- Cross-kernel RAM-console (ramoops) is impractical: I can't reserve the region in the committed slot2
  (never-touch) to stop it clobbering the buffer before I can read it; DDR-persistence across the warm
  reset is also unverified.
- No serial console on this unit.

So blind iteration has hit its limit: any further fix (init binary, a clock stub, NAND, etc.) cannot be
*confirmed* without a signal.

## THE UNLOCK: a driver-independent shell
A **USB-Ethernet dongle** (mainline `xhci_hcd` + `cdc_ether`/`r8152`/`ax88179` are all built and
robust) gives mainline a reachable login **independent of our driver, NAND, and serial** — instantly
showing whether mainline reaches userspace and, if so, exactly why `/data` and our driver fail. The
trial kernel already includes those USB drivers + the init brings up the conduit; adding a USB-Eth
bring-up to the init makes it a ~10-minute debug once a dongle is plugged in. (Serial would also work,
but the unit has none.)

Everything is staged (verified `.pkgtb`, proven flash + ONCE-revert flow, persistent logs), so once a
dongle is available this resolves fast. Device left safe on committed slot2.
