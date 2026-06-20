# stock-watch — observe the STOCK Broadcom stack on real BCM4916 silicon

Tools to watch how the closed BCA stack (rdpa/pktrunner/cmdlist) programs the
XRDP Runner's NAT-C fast path, so the open driver's builders and the QEMU model
can be validated/corrected byte-for-byte. Built on dev-build, loaded CAREFULLY on
a live device (the Ethernet is the management link).

## Stage A — `natc_dump.ko` (read-only DDR connection-table dump) [DONE]

A kernel module that, on load, reads the already-populated NAT-C connection table
in DDR and hex-dumps the RAW bytes of each active flow's:
- 16-byte NAT-C lookup KEY (pins ABI UNKNOWN #5)
- FC_UCAST_FLOW_CONTEXT_ENTRY result + embedded cmdlist (pins UNKNOWN #1)

### Why it is the lowest-risk capture
It patches NOTHING and touches NO register. It kallsyms-resolves the stock
rdpa.ko read-only accessors and calls them:
```
drv_natc_key_entry_get   (u8 table_id, u32 index, u8 *valid_out, void *key16)
drv_natc_result_entry_get(u8 table_id, u32 index, void *result)
```
(These are local `t` symbols; CONFIG_KALLSYMS=y on the device lets a module
resolve them. CONFIG_KPROBES is OFF, so we deliberately do NOT use kprobes.
CONFIG_STRICT_DEVMEM=y blocks userspace /dev/mem reads, so a module is required.)
No DDR writes, no register pokes, no datapath change. On unload nothing is undone.

### Hooking mechanism (vs the WiFi effort)
The WiFi effort's `dhd-kprobe-trace` used `register_kprobe` (that bench had
CONFIG_KPROBES=y). This Ethernet device does NOT (CONFIG_KPROBES is unset), so
Stage A avoids hooking entirely and instead READS already-resident DDR via the
stock accessors. If a future Stage B needs to intercept flow CREATION, it must
use text-patch trampolines (kprobes unavailable), not register_kprobe.

### Build (on dev-build, never on dev-code)
```sh
make            # -> natc_dump.ko, vermagic must match the running device kernel
make clean
# verify vermagic:
strings natc_dump.ko | grep vermagic   # 4.19.294 SMP preempt mod_unload aarch64
```
CONFIG_MODULE_SIG is off on the device, so the unsigned vermagic-exact .ko loads.

### Load / capture / unload (CAREFUL — live management link)
Always confirm SSH before and after each step.
```sh
# push (scp is mangled; use cat-over-ssh)
cat natc_dump.ko | ssh -p 2222 <dev> 'cat > /tmp/natc_dump.ko'

# the active flows live at sparse HASH indices == the `hw_id` from bs examine;
# a linear 0..N scan finds nothing. Pass the live hw_ids:
ssh -p 2222 <dev> '
  IDS=$(bs /Bdmf/e ucast | grep -oE "hw_id=0x[0-9a-f]+" | sed "s/hw_id=//" \
        | tr "\n" "," | sed "s/,$//")
  dmesg -c >/dev/null
  insmod /tmp/natc_dump.ko idxlist=$IDS      # dump_invalid=1 to also dump empties
  dmesg | grep NATCDUMP                        # raw KEY + CTX hex
  rmmod natc_dump
'
```
Module params:
- `idxlist=<csv hex>` — explicit NAT-C indices (hw_ids) to dump (preferred).
- `tables=0xf` — bitmask of NAT-C table ids to scan (flow table = table 0).
- `max_index=N` — linear-scan ceiling when idxlist is empty (default 64).
- `dump_invalid=1` — also dump entries whose valid bit is 0.

### Captured result
See `re-notes/stock-watch-capture.md` for the raw bytes, the name->offset map,
and the driver corrections (NAT-C key w[3] = ToS<<24|0x28<<16|flags<<8|0x68;
FC_UCAST entry = 124 B with command_list at byte 24). Link stayed up through every
load/unload; no oops, no reboot.

## Stage B — function hooks at flow creation [NOT NEEDED]
Stage A + the GPL SDK struct source fully pinned both UNKNOWNs, so Stage B was not
attempted. If ever needed (e.g. to catch a NAT/VLAN flow being created live), it
would hook `pktrunner_ucast_cmdlist_create` / `addIpv4Commands` / the natc-add via
text-patch trampolines (kprobes are unavailable on this kernel).
