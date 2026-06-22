# Cycles 09-15 — ROOT CAUSE via FIT reverse-engineering (2026-06-22)

## The discovery (user's suggestion: "RE the working FIT to spot the diff")
The mainline kernel NEVER RAN in cycles 01-13 — the bug was our **bootfs FIT**, not the
kernel. RE of the working stock bootfs FIT (extracted from `/dev/ubi0_5`) vs our hand-rolled
one revealed the Broadcom two-stage boot:

```
mtd0 "loader" → bootfs FIT conf_uboot/conf_ub_<board> (loadables = atf, uboot)
              → ATF/BL31 @0x4000 (provides PSCI) + 2nd-stage Broadcom U-Boot @0x01000000
              → 2nd-stage U-Boot → conf_lx_<board> (kernel + fdt) → Linux
```

Our FIT shipped **only kernel + fdt** (no `atf`, no `uboot`, no `loadables`), so the chain
failed before the kernel — a constant ~87s watchdog revert regardless of kernel/DT/config.

## The fixes (each moved the slot1-phase timing — first movement in 13 cycles)
| cycle | change | slot1-phase |
|---|---|---|
| 09-11 | hand-rolled, no/incorrect ATF stage | **87 s** (constant) |
| 12-13 | added atf+uboot; ATF-stage config → `fdt_uboot` (Broadcom uboot's own dtb) | **100 s** |
| 14 | full stock FIT structure, but swapped `fdt_GT-BE98` globally (broke ATF stage) | 86 s |
| 15 | **correct**: ATF stage keeps stock Broadcom dtb; Linux stage gets our kernel + our mainline dtb (`fdt_main`) | **120 s** |

Monotonic increase 87→100→120 s = the boot progressing further through the chain at each fix.

## Key technical facts
- Stock FIT uses **external data** (`mkimage -E`: `data-size`+`data-position`), not inline.
- ATF/uboot/fdt_uboot extracted from stock FIT: `atf.bin` 24888B @0x4000,
  `uboot.bin` 2665920B @0x01000000, `fdt_uboot.dtb` 6786B. Board id = **GT-BE98**.
- Correct FIT build = `/tmp/rebuild_fit2.py` on dev-build: dtc-dump the stock FIT structure,
  `/incbin` all 62 extracted images, swap **only** `kernel`→ours, inject `fdt_main`=our
  mainline dtb and repoint **only** `conf_lx_GT-BE98`→`fdt_main`; `mkimage -E`.
  → `/tmp/cycle15_bootfs.itb`.

## Current state
The boot chain is fully reconstructed and our kernel is now being **loaded and executed**
with our dtb (longest phase yet), but it still does not reach userspace (no `/data`
breadcrumb, no fast revert from the reboot-probe/combined init). The remaining failure is now
genuinely in the **kernel bring-up** (or the Broadcom U-Boot→kernel handoff / dtb fixup),
which — unlike the FIT — is not visible without an early signal. Kernel/DT changes now
finally *matter* (they were no-ops while the chain was broken), so kernel bisection is now
valid; a serial UART would show the exact hang point immediately.

Device safe on committed slot2 throughout; every cycle flashes slot1-only + ONCE + cleans
`/data/.trial-armed` on revert.
