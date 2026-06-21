# Cycles 05-07 — isolate the mainline failure (2026-06-21)

Building on cycle 04's proof that `bcm_bootstate 6` ONCE genuinely boots slot1 and the
flash/auto-revert chain works (so mainline IS being booted every cycle).

## Cycle 05 — minimal DTS, no datapath
Built a bare DT (`dts/gt-be98-min.dts`): upstream-proven `bcm6813.dtsi` + memory +
ATF/PMC reserved region + PL011 console + NAND only. **No `bcm4916-enet.dtsi`, no
runner/switch/xport/MAC/PHY nodes** — so the built-in `bcm4916_runner` driver has no node
to probe and nothing touches the (no-PMC, no-microcode) XRDP/Runner MMIO that can hang the
SoC. → **still no breadcrumb, ~70 s revert.** Rules out "our datapath probe hangs boot".

## Cycle 06 — match the reference board's NAND
Upstream `bcm96813.dts` (the proven 6813 reference board) configures NAND with
`brcm,nand-ecc-use-strap` + `brcm,wp-not-connected`. The SoC DTSI nand node carries NO ECC
config — the *board* dts must. Cycles 04/05 enabled the controller WITHOUT the strap, so
mainline `brcmnand` had no ECC layout and likely never bound (non-fatal → boot continues).
Added the strap props (matching the reference). → **still no breadcrumb, ~70 s revert.**
The NAND-breadcrumb channel is exhausted.

## Cycle 07 — ★reboot-probe: does mainline reach userspace at all?★
Replaced the embedded initramfs `init` with a diagnostic one that, the instant PID 1 runs,
does an immediate clean reboot (`sysrq b` + `reboot -f`). If mainline reaches userspace the
slot1 phase collapses to ~15-20 s (boot→init→reset); an early panic just waits out the
unpetted watchdog. Rebuilt the Image (relinked initramfs; switched FIT kernel to **gzip**
`Image.gz` since dev-build has no `lzop`), minimal DT, tight 4 s polling.
→ **SLOT1_PHASE = 87 s** (= watchdog timeout, NOT a fast init-driven reset).

## Cycle 08 — ★clean A/B confirms: NO userspace★
The cycle-07 reboot-probe timing was confounded (gzip decompress ~30 s × 2 passes dominates
the slot1 phase and masks any init-driven speedup; and `BOOT REASON 0x24` is constant for
clean reboots AND both trial types, so it does NOT discriminate watchdog vs reboot). Ran the
clean A/B: same gzip Image, same minimal DT, but the **looping** (non-rebooting) real init.
→ **SLOT1_PHASE = 86 s, identical to cycle-07's 87 s.** If the reboot-probe init had run, 07
would have reverted faster than the looping 08. Identical ⇒ the init never runs in either ⇒
**mainline does not reach userspace.** (To make the timing detector usable again, decompress
must be fast — install `lzop` and go back to lzo so the ~5 s/pass no longer masks the signal.)

## CONCLUSION: mainline hangs/panics BEFORE userspace
Across cycles 04-07, with NAND matched to the reference board and the DT stripped to bare
SoC, mainline never reaches `init`. Boot-critical config is all present (GIC, PL011+console,
arch-timer, common-clk fixed-clocks, PSCI, SMP, OF); clocks are simple fixed-clocks (no
missing clk driver); console UART clock is the correct 50 MHz. So it is an early
(pre-userspace) hang, not an obvious config gap.

### Residual uncertainty
The reboot-probe assumes mainline PSCI `SYSTEM_RESET` works. Stock may reset via a
Broadcom-specific path, not PSCI; if ATF doesn't implement PSCI reset here, a *running*
init's reboot would hang and the watchdog would fire anyway — a false "no userspace". Worth
a 2-variant timing test (reboot-now vs sleep-30-then-reboot) to confirm, but see below.

## Observability options now (all non-serial channels assessed)
- **/data breadcrumb**: needs mainline userspace (the thing in question) AND brcmnand bind. ✗
- **USB-Eth lifeline**: BLOCKED — the 6813 USB rail is powered by the BCA PMC
  (`brcm,bca-pmc-3-2` @ 0xffb01018), which has NO mainline driver and no mainline `pmb`; the
  USB block won't enumerate without a PMC power-on we can't provide. (xhci/phy regs captured
  for the record: xhci@0x800c0000, gbl@0x800cc100, ctrl@0x800cf200.) ✗
- **our-driver network**: that's the DUT. ✗
- **ramoops/pstore**: can't reserve a buffer in the committed slot2 (never-touch) to read it
  back after revert; DDR persistence across the warm reset unverified. ✗
- **serial UART**: would show the exact pre-userspace hang point immediately. (User reports
  no serial console access.) ← the clean unlock for a pre-userspace hang.

## State
Device safe on committed slot2. Real bugs fixed and committed this session: FIT overlap
(cycle 02), reserved-memory/ATF (cycle 03), NAND enable (cycle 04), NAND ecc-strap (cycle 06).
Reboot-probe init left on dev-build (`init.real` backed up) — it is a reusable 1-bit
"reached-userspace?" detector for any further bisection.
