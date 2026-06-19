# GT-BE98 open-Ethernet — RECOVERY PLAN (research/read-only pass)

> **Why this doc exists.** On the GT-BE98, **Ethernet IS the management link**: the test
> device `admin@10.0.0.8` is reachable only over `br0` (10.0.0.8/24), whose sole uplink
> member is `eth0`. A broken open Ethernet driver disconnects the device with **no in-band
> fallback** — the on-device webui (port 80) and SSH (2222/2223) all ride the same `br0`.
> Therefore a recovery path that does **not** depend on Ethernet (serial console / U-Boot
> netboot) is **mandatory** before any connectivity-risking driver load.
>
> This pass is **READ-ONLY**: nothing below was exercised. No reboot/flash/insmod/nvram/slot
> change was made. Data was gathered via `ssh -p 2222 admin@10.0.0.8` only.

---

## (a) Current boot / slot / bootloader facts

### HW-CONFIRMED (read live from the device, 2026-06-19)

| Fact | Value | Source |
|---|---|---|
| SoC / board | Broadcom **BCM4916**, U-Boot board name **bcm96813** ("Broadcom-v8A", `brcm,brcm-v8A`) | `/proc/device-tree/{model,compatible}`, U-Boot strings |
| **Bootloader** | **U-Boot 2019.07** (SPL `Sep 07 2023`) — **NOT CFE** | strings in `/dev/mtd1ro` (loader) |
| Bootloader location | `mtd1` = **`loader`**, 0x200000 (2 MiB) at NAND offset 0x200000 | `/proc/mtd`, `mtdparts=` |
| U-Boot `bootcmd` | `printenv;run once;sdk boot_img` | U-Boot env strings |
| U-Boot `bootdelay` | **5** (≈5 s serial-interrupt-to-prompt window) | U-Boot env strings |
| U-Boot net env | `ipaddr=192.168.1.1`, `ethaddr=60:CF:84:38:87:B0` present (TFTP-capable U-Boot) | U-Boot env strings |
| `once=true` | one-shot trial-boot flag is a real U-Boot env var (consumed at boot) | U-Boot env strings |
| Anti-rollback | U-Boot enforces antirollback level on COMMIT (`Committing antirollback level`) | U-Boot strings — **be careful never to bump it on a trial** |
| **Console / UART** | **`console=ttyAMA0,115200`**, `stdout-path` set in DT chosen | `/proc/cmdline`, `/proc/device-tree/chosen/stdout-path` |
| **Running slot** | **slot 2** (`root=/dev/ubiblock0_6`, `ubi.block=0,6` → `rootfs2`) | `/proc/cmdline` |
| Boot-image state | `committed 2 valid 1,2 seq 52,97`; **Booted/Reboot Partition: Second** | `bcm_bootstate` |
| **Slot-1 commit flag** | **0 (UNCOMMITTED)** ⚠ | `bcm_bootstate` ("First partition commit flag : 0") |
| **Slot-2 commit flag** | **1 (committed)** | `bcm_bootstate` ("Second partition commit flag : 1") |
| Trial state | `/data/.trial-armed` PRESENT; `reset_reason=34`; `/tmp/deadman-disarm` PRESENT; `wdtctl timeleft` = 240 then idle | device files |
| HW watchdog ctl | `/bin/wdtctl` present (`-t 4..600 start` / `stop` / `ping` / `timeleft`); deadman re-arms `wdtctl -t 240 start` (NO petting) | `wdtctl` help, `deadman-early` |
| Mgmt link | `eth0` (10G, link up 10000 Mb/s, MAC 60:cf:84:38:87:b0) → bridged into `br0` = **10.0.0.8/24** | `ip addr`, `ethtool eth0` |
| Mgmt services | `:2222` (dropbear, real busybox shell), `:2223` (OpenSSH transport), `:80` (webui) — **all on br0/eth0** | `netstat -ltn` |
| Switch / ports | `bcmsw` switch netdev + `eth0..eth3` (each VLAN-tagged .20/.30/.50/.70); stock closed Runner/RDPA | `/sys/class/net` |

### NAND partition map (`/proc/mtd`)
```
mtd0 brcmnand.0  (whole, 256 MiB)
mtd1 loader      0x00200000   <- U-Boot SPL + env (the bootloader)
mtd2 image       0x0fd00000   <- UBI volume holding both slots
mtd3 metadata1 / mtd4 metadata2   <- bootstate metadata (bcm_bootstate writes here)
mtd5 bootfs1 / mtd6 rootfs1       <- SLOT 1
mtd7 bootfs2 / mtd8 rootfs2       <- SLOT 2 (running now, ubi.block=0,6)
mtd9 data        <- /data (UBIFS; dead-man state, /data/.trial-armed)
mtd10 defaults / mtd11 jffs2
```

### Slot model inherited from the WiFi bench (applies here)
- **Dual-image A/B**: slot1 = `rootfs1`/`ubi.block=0,4`; slot2 = `rootfs2`/`ubi.block=0,6`.
- `bcm_bootstate` is the slot/commit primitive: `5`=boot part1, `7`=boot part2 (persistent),
  `3/4/6/8`=ONCE variants, `+1/-1/+2/-2`=commit/uncommit a partition. **U-Boot consumes the
  ONCE flag at the trial boot**, so any reset after a trial returns to the COMMITTED slot.
- WiFi pattern was **slot1 = committed-safe fallback (v34, NEVER uncommit), slot2 = bench**.
  The bench currently runs slot2 — consistent — **EXCEPT slot1 is no longer committed**
  (see the ⚠ gap below). NOTE: WiFi memory says **software `reboot` is broken on the slot2
  graft** (`reboot`/`reboot -f`/sysrq all swallowed) — so on this bench a reboot to recover
  may itself require the HW watchdog or a power-cycle.

### Auto-revert / watchdog mechanism (HW-confirmed scripts)
Two independent layers, both keyed off `/data/.trial-armed`:
1. **`deadman-early`** (first sysinit service) re-arms the **HW watchdog `wdtctl -t 240 start`
   with NO petting**, and (if `.trial-armed`) forks `/sbin/trial-deadman`. If the boot hangs
   before services come up, the HW watchdog fires at ~240 s → reset → U-Boot boots the
   committed slot (ONCE consumed).
2. **`watchdog-disarm`** (LAST default-runlevel service) = the health-gated keeper:
   - **TRIAL boot** (`.trial-armed`): leaves the watchdog **armed + unpetted** so an
     unreachable trial AUTO-REVERTS; operator disarms only from a confirmed SSH session.
   - **COMMITTED boot**: `committed_boot_keeper()` pets for a 90 s **HEALTH WINDOW**, then
     either enters steady-state petting (healthy) or **AUTO-REVERTS** to the other valid slot.
   - **`healthy()` gate** = `br0` has an `inet ` address **AND** a mgmt service listens on
     `:2222` or `:80`. ★Both conditions depend on Ethernet★ — so this gate is *exactly* the
     right tripwire for an Ethernet-killing driver: kill eth0 → br0 loses IP → unhealthy →
     auto-revert. (Test hook: `touch /data/.force-unhealthy`.)

---

## ⚠ CRITICAL SAFETY GAP found this pass

**Slot 1 is currently UNCOMMITTED (commit flag 0); only slot 2 is committed.**
The WiFi recovery scheme depends on **slot1 = the sacred, always-committed safe fallback**.
Right now there is **no committed known-good fallback that is independent of the bench slot**.
The auto-revert re-commits "the other VALID slot" and slot1 is `valid` (`valid 1,2`), but it
is uncommitted, and we have **not verified slot1 contains a known-good stock/wl image** that
actually brings Ethernet up. Until that is fixed, an Ethernet brick could revert to a slot
whose health is unknown. **This must be remediated (with user approval) before live testing.**

---

## (b) Ranked recovery options

### Option 1 — Serial console (U-Boot prompt over UART) — **PRIMARY, mandatory**
- **What it does:** UART gives a console that is *completely independent of Ethernet*. With
  `bootdelay=5` you can interrupt U-Boot to a prompt and `setenv`/`run`/`tftpboot`/`bootm`,
  or just watch the kernel boot. This is the only recovery that still works if Ethernet,
  webui, AND SSH are all dead.
- **Requires (PHYSICAL/USER):** open the case, locate the 4-pin UART header, attach a
  **3.3 V USB-TTL adapter** (GND/TX/RX; do NOT connect VCC), terminal at **115200 8N1**
  (`console=ttyAMA0,115200` confirmed). ASUS routers use a standard 0.1″ 3.3 V header;
  the **exact GT-BE98 header location/pin order is NOT publicly documented** (no WikiDevi/
  TechInfoDepot/OpenWrt teardown found) — must be identified on the board (probe for 3.3 V
  idle-high TX with a scope/multimeter, GND = ground plane). **Not known to be wired on the
  bench today** — assume it is NOT and that attaching it is a user action.
- **Risk:** LOW (read/console). Mis-wiring VCC or 5 V is the only real hazard → use 3.3 V,
  leave VCC unconnected.

### Option 2 — U-Boot TFTP netboot recovery — **SECONDARY (needs serial first)**
- **What it does:** U-Boot 2019.07 is TFTP-capable (`ipaddr`/`ethaddr`/`serverip` env). From
  the U-Boot prompt you can `tftpboot` a known-good kernel/FIT and `bootm`, or re-flash a
  slot — recovering even a both-slots-bad situation.
- **Requires:** Option 1 (serial) to reach the prompt **first**; a TFTP server on the wired
  LAN with the recovery image; the router's wired switch/PHY working at the U-Boot level
  (U-Boot uses its own MAC/PHY init, independent of our Linux driver, so this is unaffected
  by an Ethernet driver bug in Linux). Anti-rollback level must not have been bumped.
- **Risk:** MEDIUM — flashing is involved; safe as long as we netboot/flash the *committed*
  good image and never lower antirollback.

### Option 3 — Auto-reverting trial slot (deadman + HW watchdog) — **FIRST-LINE for routine iter**
- **What it does:** Boot our open-Ethernet image as a **trial** (`.trial-armed` + ONCE flag),
  leaving the watchdog armed/unpetted. If our driver kills Ethernet, the `healthy()` gate
  (br0 IP + mgmt port) fails → **auto-revert to the committed good slot within ≤240 s** with
  no human in the loop. This is the WiFi bench's proven self-heal, and its health gate is
  Ethernet-based, so it natively covers Ethernet failures.
- **Requires:** a **committed known-good slot that brings Ethernet up** (the gap above — must
  fix slot1 first), `.trial-armed` correctly populated (`TRIAL_SLOT`/`GOOD_SLOT`/`WINDOW`),
  and **do NOT** `wdtctl stop` / `touch /tmp/deadman-disarm` until a healthy reachable boot
  is confirmed. NB: the trial image must run the **stock** Ethernet stack at boot and only
  load our open driver *after* a disarm point, OR accept the ≤240 s revert as the safety net.
- **Risk:** MEDIUM. Caveats: (1) software `reboot` is reportedly broken on the slot2 graft, so
  the revert may rely on the HW watchdog reset rather than a clean `reboot` — acceptable but
  means a hard reset. (2) If our driver wedges the *whole host* (not just eth0) before the
  watchdog is armed, only serial/power-cycle recovers — hence Option 1 must still be in place.
  (3) WiFi memory warns a *host-wide* wedge (their BHM case) defeated even the watchdog and
  needed a manual power-cycle — treat host-unreachable as a possible Ethernet-driver outcome.

### Option 4 — Webui reboot lever (admin-scoped) — **NOT a primary Ethernet recovery**
- The WiFi effort notes an admin webui reboot lever, but **the webui (:80) rides br0/eth0**.
  ★If our driver kills eth0, the webui is unreachable too.★ Therefore the webui **CANNOT** be
  the primary recovery for Ethernet work. Serial (Option 1) is mandatory; webui is at best a
  convenience when Ethernet is *still up*.

### Option 5 — Manual power-cycle — **last resort, user-only**
- Pull power, boot. Because the ONCE trial flag is consumed and `/data/.trial-armed` survives,
  a clean power-cycle returns to the committed slot. Needs a human; software reboot is
  unreliable on this graft, so this is the ultimate backstop behind serial.

---

## (c) Recommended recovery procedure to establish BEFORE any connectivity-risking test

Establish **two independent recovery rails** — one that needs no Ethernet (serial), one that
self-heals automatically (auto-revert) — and fix the committed-fallback gap:

1. **Fix the committed-safe fallback (user-approved, one device cycle).**
   - Verify slot1 (`rootfs1`/`ubi.block=0,4`) contains a **known-good image that brings
     Ethernet up** (ideally the stock/wl `v34` baseline). If unsure, (re)flash a known-good
     image to slot1 via the WiFi `bench-persist`/stock flow.
   - `bcm_bootstate +1` to **re-commit slot1** so the A/B state is `committed 1,2 valid 1,2`
     — i.e. BOTH slots committed, with **slot1 = the sacred Ethernet-good fallback that is
     NEVER uncommitted** (mirrors the WiFi rule). Keep slot2 as the open-Ethernet bench.
2. **Wire and verify the serial console (USER/PHYSICAL).**
   - Attach a 3.3 V USB-TTL adapter to the GT-BE98 UART header; terminal 115200 8N1.
   - Power-cycle and confirm you see SPL/U-Boot output and can **interrupt to the U-Boot
     prompt within the 5 s `bootdelay`** (press a key). Confirm `printenv` shows
     `bootcmd`/`bootdelay`/`ipaddr`. This proves the Ethernet-independent recovery rail.
3. **Stage U-Boot TFTP recovery.**
   - Put a known-good recovery FIT/kernel on a TFTP server on the wired LAN; confirm from the
     U-Boot prompt that `tftpboot` reaches it (read-only test: fetch, do NOT flash). This
     arms Option 2 for a both-slots-bad scenario.
4. **Arm the auto-revert trial for the test boot.**
   - Boot the open-Ethernet image as a **trial** on slot2: populate `/data/.trial-armed`
     (`TRIAL_SLOT=2 GOOD_SLOT=1 WINDOW=… SHA=…`), leave the HW watchdog **armed/unpetted**.
   - **Do NOT** `wdtctl stop` or `touch /tmp/deadman-disarm` until you have a confirmed
     healthy SSH session on the trial. If the open driver kills Ethernet, the health gate
     trips and the box auto-reverts to committed slot1 within ≤240 s.
5. **Test in the smallest reversible increment.**
   - Prefer loading the open driver **after boot via `insmod`** (so the box has already come
     up healthy on the stock stack and the watchdog window has been satisfied), rather than
     making it the boot-time Ethernet driver — an `insmod` failure is recoverable by `rmmod`
     IF the host itself stays alive, and the boot path stays on the known-good stack.
   - Keep the WiFi bench's hygiene: `ssh-keygen -R '[10.0.0.8]:2222'` after reboots; one
     device cycle at a time; clean `/data` if full.

---

## (d) Pre-test GREEN checklist — ALL must be GREEN before loading the open Ethernet driver live

- [ ] **Serial console verified** — UART attached (3.3 V, 115200 8N1), SPL/U-Boot output
      visible, U-Boot prompt reachable within the 5 s `bootdelay`. *(REQUIRES USER/PHYSICAL.)*
- [ ] **Committed Ethernet-good fallback exists** — slot1 holds a known-good image that brings
      Ethernet up, and `bcm_bootstate` shows slot1 **committed** (`committed 1,2 valid 1,2`).
      Slot1 will NEVER be uncommitted. *(Requires user-approved one device cycle.)*
- [ ] **U-Boot TFTP recovery staged** — recovery FIT on a wired-LAN TFTP server, `tftpboot`
      fetch confirmed from the U-Boot prompt (read-only). *(Needs serial.)*
- [ ] **Auto-revert armed** — `/data/.trial-armed` correctly populated; HW watchdog armed
      (`wdtctl timeleft` counting from 240) and **NOT** disarmed; `/tmp/deadman-disarm` and
      `/data/.force-unhealthy` ABSENT for the test boot.
- [ ] **Health gate understood** — confirmed `watchdog-disarm` healthy() = (br0 has inet) AND
      (listener on :2222 or :80); both ride eth0, so an Ethernet kill trips revert.
- [ ] **Power-cycle access available** — a human (or PDU) can power-cycle, since software
      `reboot` is unreliable on the slot2 graft. *(REQUIRES USER/PHYSICAL.)*
- [ ] **Incremental plan** — first live test is `insmod`-after-healthy-boot (rmmod-recoverable),
      NOT the boot-time Ethernet driver. Stock Ethernet stack still owns the boot path.
- [ ] **Host-wide-wedge contingency acknowledged** — if the driver wedges the whole host
      (not just eth0), watchdog may not save it → serial/power-cycle is the only recovery.

### Items that REQUIRE user / physical action (cannot be done from this read-only session)
1. **Attach & verify the UART cable** (open case, locate header, 3.3 V USB-TTL, 115200). The
   exact GT-BE98 header pinout is **not publicly documented** — must be found on the board.
2. **Power-cycle / out-of-band power control** (software reboot unreliable on slot2).
3. **Approve & perform the slot1 re-commit / known-good reflash** (the one boot/flash change
   needed to close the committed-fallback gap).
4. **Provide / set up the TFTP recovery server** on the wired LAN.

---

### Sources (web research for UART specifics — no GT-BE98-specific teardown found)
- ASUS router serial/UART hacking references (general, not GT-BE98-specific):
  [SNBForums RT-AC5300 serial pinout](https://www.snbforums.com/threads/asus-rt-ac5300-serial-pinout.36780/),
  [Unbrick ASUS RT-AC68U over serial](https://www.snbforums.com/threads/unbrick-asus-rt-ac68u-router-over-serial-connection-using-a-raspberry-pi.56567/),
  [Hardware Hacking 101: root shell via UART](https://riverloopsecurity.com/blog/2020/01/hw-101-uart/).
- All GT-BE98-specific boot/slot/bootloader facts above were read **live from the device**
  (U-Boot, not CFE; ttyAMA0 @115200; dual-slot bcm_bootstate), not from the web.
