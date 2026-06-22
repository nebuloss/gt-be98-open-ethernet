# Our open driver loads on real silicon — 4.19 module port (2026-06-22)

Pivot to incrementally modifying the dev-build STOCK kernel (it boots — see
[[devbuild-stock-kernel-base]]) and loading our open driver as a 4.19 module, instead of
chasing a mainline boot. ★Our slow-path CPU-conduit driver now COMPILES for the device kernel
AND LOADS on the live device.★

## Module build loop (dev-build → live device, fast + safe)
- Device kernel: 4.19.294, vermagic `4.19.294 SMP preempt mod_unload aarch64`, **MODVERSIONS
  off, MODULE_SIG off**, already tainted P → unsigned out-of-tree modules load, vermagic-only
  check (no CRC), no slot1 boot needed (load on the running committed slot2 @10.0.0.8).
- KDIR: `…/src-rt-5.04behnd.4916/kernel/linux-4.19`. Toolchain:
  `…/toolchain/am-toolchains/brcm-arm-hnd/crosstools-aarch64-gcc-10.3-…/bin/aarch64-buildroot-linux-gnu-`.
- Build: `make -C $KDIR M=$PWD ARCH=arm64 CROSS_COMPILE=$CC MODEL=GTBE98 modules`.
  - **`MODEL=GTBE98` is REQUIRED** — the SDK kernel Makefile does `KBUILD_CFLAGS += -D$(MODEL)`;
    empty MODEL → bare `-D` → "macro names must be identifiers". (Real kernel objs use -DGTBE98.)
  - **Broadcom include paths REQUIRED** (skbuff.h pulls bcm_skbuff.h): ccflags-y +=
    `-I$SDK/kernel/bcmkernel/include -I$SDK/kernel/bcmkernel/include/uapi`
    `-I$SDK/bcmdrivers/broadcom/include/bcm963xx -I$SDK/bcmdrivers/opensource/include/bcm963xx`
    `-I$SDK/shared/opensource/include/bcm963xx`.
- Load: `firmware_class` is `=m` (FW_LOADER=m); `modprobe firmware_class` first or our module's
  request_firmware/release_firmware refs are "unknown symbol". Then `insmod bcm4916-runner.ko`.

## 4.19 API adaptations (in the local build copy; integrate into repo with version guards)
bcm4916_runner.c (vs mainline 6.x):
1. `platform_get_irq_byname_optional` → `platform_get_irq_byname` (4.19 lacks _optional).
2. `netif_napi_add(d,n,poll)` → add 4th arg `NAPI_POLL_WEIGHT` (4.19 weighted signature).
3. `devm_register_netdev(dev,ndev)` → `register_netdev(ndev)` (no devm variant; add
   unregister_netdev in remove for the post-probe path — TODO).
4. `.remove` callback returns void (6.11+) → 4.19 needs `int` (+ `return 0;`).
flow_offload.c: needs mainline `net/flow_offload.h` (absent in 4.19) → **deferred**; replaced
with `flow_offload_stub.c` (no-op xrdp_offload_init/deinit/setup_tc/selftest/nat_selftest).
cmdlist.c: pure logic — compiles unchanged. flow_offload's nf_flow_table use also needs the
4.19 TC/flow API port later.

## RESULT
`bcm4916-runner.ko` (23.8 KB: bcm4916_runner.o + cmdlist.o + flow_offload_stub.o) built and
`INSMOD_OK` on the live device. Registers the platform_driver but **does not probe** (stock DT
has no `brcm,bcm4916-runner` node) → no XRDP/Runner MMIO touched → device stable, dmesg clean.
rmmod clean. Device on committed slot2 throughout.

## NEXT (incremental, ordered by risk)
1. Port `flow_offload.c` to the 4.19 TC/flow_offload API (un-defer the fast-path) — or validate
   the cmdlist/NAT-C ABI against the LIVE stock stack (Phase-R, read-only; cmdlist.o vs captured
   stock cmdlists — see [[stock-driver-watch-lever]]).
2. ★Phase-W (RISKY, watchdog-guarded): to actually PROBE our driver and exercise the datapath,
   instantiate a matching platform_device (DT overlay or platform_device_register with the 4916
   XRDP reg/irq). The stock Runner stack (pktrunner/rdpa/bdmf/cmdlist/pktflow) is LOADED and owns
   the Runner — our probe touching the same MMIO will conflict; must rmmod the stock stack first
   and guard with the deadman/auto-revert. NOTE a stock module is also named `cmdlist` (ours uses
   a cmdlist.o object but the symbols are static/composite — verify no clash at takeover).
