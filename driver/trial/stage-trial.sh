#!/usr/bin/env bash
# stage-trial.sh - STAGED open-Runner-takeover live test for the GT-BE98.
#
# Produces a self-contained trial firmware (stock dev-build 4.19 kernel +
#   - bootfs DTB: our runner@82000000 node added, stock rdpa_drv disabled
#   - rootfs: /usr/lib/open-enet/{load.sh,bcm4916-runner.ko,fw/...} baked in +
#             a guard spliced into rom/etc/init.d/bcm-base-drivers.sh)
# then flashes it to SLOT1 ONLY, seq-bumps slot1>slot2, arms ONCE + the deadman,
# and (on the user's signal) reboots into it. Slot2 stays the committed GOOD
# fallback; a two-layer deadman (sw reboot + unpetted hw watchdog, WINDOW=240s)
# auto-reverts. Results land on /data (survives the revert).
#
# SAFETY: every line that touches the device is fenced with '### DEVICE ###' and
# left COMMENTED. The dev-build repack steps (1-2) are non-destructive and may be
# run freely to produce the artifact. DO NOT uncomment the fenced lines until the
# user gives the go-ahead.
#
# Provenance of every value: re-notes/realhw/10-runner-bringup-spec.md (Wave-7),
# driver/dts/gt-be98-open-runner.dtsi, and re-notes/realhw/{01,04}-*README.md +
# board/gt-be98/flash/open-flash.sh (the proven slot1 flash/seq/deadman).
set -euo pipefail

DEVBUILD=guillaume@10.0.50.21
DEV=admin@10.0.0.8            # committed GOOD = slot2; mgmt over Ethernet
PORT=2222
SDK=/home/guillaume/be98/gt-be98-firmware/vendor/asuswrt-merlin.ng/release/src-rt-5.04behnd.4916
TGT=$SDK/targets/96813GW
WORK=/tmp/open-trial          # scratch on dev-build
KO=/home/guillaume/drv4919/bcm4916-runner.ko        # built driver (dev-build)
FW=/tmp/bcm4916-runner-microcode-6813.bin           # CORRECT (BCM6813) blob, sha b48ce349...
REPO=/home/guillaume/be98/gt-be98-open-ethernet     # this repo (dev-code; rsync to dev-build as needed)

# ---------------------------------------------------------------------------
# 1) (dev-build, NON-DESTRUCTIVE) Build the modified Linux DTB from source.
#    Add our node + disable rdpa_drv in a COPY of the GT-BE98 board DTS, then
#    build just the .dtb with the kernel's dtc + include path.
# ---------------------------------------------------------------------------
# ssh $DEVBUILD bash -s <<'EOS'
#   set -e; SDK=...; WORK=/tmp/open-trial; mkdir -p $WORK
#   cp $SDK/kernel/dts/6813/GT-BE98.dts $WORK/GT-BE98.dts
#   # append the open-runner overlay (node + rdpa_drv disable) to the board dts
#   cat >> $WORK/GT-BE98.dts <<'DTS'
#   / { runner@82000000 { compatible="brcm,bcm4916-runner";
#         reg=<0x0 0x82000000 0x0 0x00CAF004>;
#         interrupt-parent=<&gic>; interrupt-names="queue0"; interrupts=<0 75 4>; }; };
#   &rdpa_drv { status="disabled"; };
#   DTS
#   # build the dtb (cpp + dtc with the dts include dir); confirm both edits landed
#   make -C $SDK/kernel/linux-4.19 ... <dtb target>   # or cpp|dtc with -I $SDK/kernel/dts
#   dtc -I dtb -O dts $WORK/GT-BE98.dtb | grep -E "bcm4916-runner|rdpa_drv|disabled"
# EOS

# ---------------------------------------------------------------------------
# 2) (dev-build, NON-DESTRUCTIVE) Repack bootfs FIT (swap Linux fdt only) and
#    rootfs squashfs (bake payload + guard). Split from the booting stock pkgtb.
# ---------------------------------------------------------------------------
# PKG=$(ls -t $TGT/*nand_squashfs.pkgtb | head -1)
# ssh $DEVBUILD bash -s <<EOS
#   set -e; mkdir -p $WORK; cd $WORK
#   dumpimage -T flat_dt -p 0 -o bootfs.itb "$PKG"      # bootfs FIT (ATF+uboot+kernel+fdts)
#   dumpimage -T flat_dt -p 1 -o rootfs.sqfs "$PKG"     # squashfs rootfs
#   # --- bootfs: replace ONLY the Linux fdt sub-image with $WORK/GT-BE98.dtb, keep
#   #     atf/uboot/uboot-fdt/kernel; repoint conf_lx_GT-BE98->new fdt; mkimage -E.
#   #     (use the proven /tmp/rebuild_fit2.py template, kernel UNCHANGED this time.)
#   # --- rootfs: bake payload + guard ---
#   rm -rf rootfs && unsquashfs -d rootfs rootfs.sqfs
#   mkdir -p rootfs/usr/lib/open-enet/fw/brcm
#   cp $KO          rootfs/usr/lib/open-enet/bcm4916-runner.ko
#   cp $FW          rootfs/usr/lib/open-enet/fw/brcm/bcm4916-runner-microcode.bin
#   cp /tmp/load.sh rootfs/usr/lib/open-enet/load.sh   # from $REPO/driver/trial/load.sh
#   chmod +x rootfs/usr/lib/open-enet/load.sh
#   # splice the guard after the start) label of bcm-base-drivers.sh (see
#   # driver/trial/bcm-base-drivers.guard.sh); verify it is present + before insmods.
#   mksquashfs rootfs rootfs.new.sqfs -comp xz -noappend   # match stock comp/flags
# EOS

# ---------------------------------------------------------------------------
# 3) (device, READ-ONLY) Preflight: GOOD=slot2 committed, both valid, no stale arm.
# ---------------------------------------------------------------------------
# ssh -p $PORT $DEV 'bcm_bootstate; grep -o "ubi.block=0,[0-9]" /proc/cmdline; ls -l /data/open_enet /data/.trial-armed 2>/dev/null'
#   REQUIRE: committed 2, valid 1,2; NO /data/open_enet.

### >>> DEVICE: SLOT1 FLASH (writes vol3 bootfs1 + vol4 rootfs1, seq-bump, ONCE) <<< ###
# Uses the proven flow (board/gt-be98/flash/open-flash.sh primitives):
#   ubirmvol/ubimkvol/ubiupdatevol vol4 (rootfs1) <- rootfs.new.sqfs
#   ubiupdatevol vol3 (bootfs1) <- repacked bootfs.itb
#   seq-bump slot1 = slot2_seq+1 (metadata vol1/vol2)  ;  bcm_bootstate 6 (ONCE slot1)
#   slot2 + loader + metadata-of-slot2 UNTOUCHED.
# scp the two artifacts to the device /tmp first, then run open-flash.sh.
### (left un-run) ###

### >>> DEVICE: ARM the takeover <<< ###
# ssh -p $PORT $DEV 'touch /data/open_enet'      # the guard only fires when this exists
### (left un-run) ###

### >>> DEVICE: pre-reboot GATE (READ-ONLY) - MUST be committed 2 + Reboot First <<< ###
# ssh -p $PORT $DEV 'bcm_bootstate | grep -E "committed|Reboot|valid"'

### >>> DEVICE: the trial boot <<< ###
# ssh -p $PORT $DEV 'reboot'   # boots slot1 ONCE; mgmt drops; deadman reverts <=240s
### (left un-run) ###

# ---------------------------------------------------------------------------
# 4) After it reverts to slot2 (~<=240s) and mgmt is back, read the breadcrumb:
# ---------------------------------------------------------------------------
# ssh -p $PORT $DEV 'cat /data/open-enet/trial.log; echo ----; cat /data/open-enet/dmesg.log'
#   -> shows how far cold-init got (FPM pool / microcode loaded / RNR pre-cfg /
#      SBPM / DSPTCHR / BBH / RNR enabled), and any oops/hang point.
# CLEANUP (only after analysis): ssh -p $PORT $DEV 'rm -f /data/open_enet'
echo "stage-trial.sh is a STAGED runbook. Device-touching steps are fenced + commented."
echo "Run dev-build repack (steps 1-2) to produce artifacts; flash only on user signal."
