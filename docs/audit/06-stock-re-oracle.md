# 06 — Stock-watch RE oracle tooling

Audit of the live "stock-watch" reverse-engineering tools under `tools/stock-watch/`.
These are the instruments that observe Broadcom's **closed** BCA stack
(`bcm_enet` / `rdpa` / `pktrunner` / `cmdlist` + the proprietary Runner
microcode) running on real BCM4916 silicon, so the **open** driver
(`driver/runner/*`) and the QEMU model can be validated and corrected
byte-for-byte against the hardware. They are the methodology that turns the
closed stack into a written spec.

Files audited (all paths repo-relative):

| File | Kind | Mechanism |
|---|---|---|
| `tools/stock-watch/natc_dump.c` | kernel module | kallsyms-resolve + call stock read-only accessors |
| `tools/stock-watch/xrdp_peek.c` | kernel module | `probe_kernel_read` of globals + opt-in `ioremap`/`readl` MMIO |
| `tools/stock-watch/rdpa_trace.c` | kernel module | `register_kprobe` pre-handlers on stock rdpa functions |
| `tools/stock-watch/route-a-oracle.sh` | shell | drives `xrdp_peek.ko` MMIO mode over a register list |
| `tools/stock-watch/rdpa-trace-events.sh` | shell | tracefs `kprobe_events` (fallback for `rdpa_trace.ko`) |
| `tools/stock-watch/README.md` | doc | build/vermagic/load recipe + capture status |
| `tools/stock-watch/Makefile` | build | out-of-tree cross build of the three `.ko` |

Cross-referenced RE notes: `re-notes/stock-watch-capture.md`,
`re-notes/offload-live-validation.md`, `re-notes/phase-r-live-abi-validation.md`,
`re-notes/realhw/11-route-a-egress-spec.md`.

---

## 1. Purpose

The open driver must reproduce, bit-for-bit, what the closed stack programs into
the XRDP "Runner" hardware. Two things block that:

1. **The NAT-C fast-path ABI** — the 16-byte connection-table lookup key, the
   `FC_UCAST_FLOW_CONTEXT_ENTRY` result, and the embedded XPE cmdlist — is
   defined by proprietary code and the autogen 6813 RDD headers, not by anything
   observable at the netdev boundary.
2. **The "Route A" slow-path TX egress control plane** — how a CPU-injected
   packet descriptor reaches BBH_TX through the QM aggregator and a TM Runner
   core — lives only inside `rdpa.ko` and the microcode.

Stock-watch answers both **without disturbing the live management link** (the
device's only Ethernet is the SSH management path; see the
`no-connectivity-break-live-tests` rule). It provides three escalating,
read-only observation techniques and two shell oracles that orchestrate them:

- **`natc_dump.ko` (Stage A)** — read the already-populated NAT-C connection
  table straight out of DDR by *calling the stock driver's own read-only
  accessors*. Pins the key layout (ABI UNKNOWN #5) and the context/cmdlist
  layout (ABI UNKNOWN #1). Touches no register, writes nothing, hooks nothing.
- **`xrdp_peek.ko` (Phase-R)** — dump a stock global (fault-tolerant
  `probe_kernel_read`), follow a global pointer one level, or — opt-in only —
  `ioremap`+`readl` a register window. Confirms the placeholder register/ring
  offsets the open driver carries.
- **`rdpa_trace.ko` (Stage C)** — `register_kprobe` pre-handlers that capture
  stock function *arguments as they happen* (the per-packet CPU_TX
  `egress_queue`, the flow-add / NAT-C-add / cmdlist-compile args) that a static
  snapshot cannot see.
- **`route-a-oracle.sh`** — drives `xrdp_peek.ko` MMIO mode over the exact QM /
  BBH_TX / RNR_MEM register list that Route A needs, to pin the
  `route_a_*` module-param defaults for the open driver.
- **`rdpa-trace-events.sh`** — the no-module tracefs equivalent of
  `rdpa_trace.ko`, kept as a fallback for a kernel with
  `CONFIG_KPROBE_EVENTS` (currently unavailable — see findings).

Where they sit in the datapath: these tools observe the **stock** control plane
and connection table for the two hardware pipelines the open driver targets —
the **HW flow-offload fast path** (CPU → NAT-C lookup → cmdlist rewrite → egress)
and the **CPU_TX slow path** (host ring → QM queue → TM core → BBH_TX). They do
not sit *in* the open datapath; they are the spec-extraction rig for it.

---

## 2. Architecture & data flow

### 2.1 Three read-only observation techniques, ranked by risk

The tools deliberately escalate from "cannot possibly perturb the datapath" to
"touches the live datapath but only reads":

1. **Call stock accessors (`natc_dump`)** — no hooks, no MMIO, no writes. The
   stock accessor does its own physical-address arithmetic; the module only
   supplies a table id + index and gets bytes back. Lowest risk.
2. **`probe_kernel_read` of globals (`xrdp_peek` mem/deref modes)** — reads
   kernel virtual addresses fault-tolerantly. A bad address returns `-EFAULT`
   instead of oopsing.
3. **`ioremap`+`readl` MMIO (`xrdp_peek` phys mode)** — opt-in
   (`allow_mmio=1`) because raw reads of *some* XRDP register space have
   hung the SoC (see findings). Highest risk; caller must pick side-effect-free
   registers.
4. **kprobe pre-handlers (`rdpa_trace`)** — sit on the live datapath but only
   read arg registers + `probe_kernel_read` struct fields, capped per-probe.

### 2.2 The kallsyms constraint that shapes everything

The stock device kernel is `CONFIG_KALLSYMS=y` but **`CONFIG_KALLSYMS_ALL=OFF`**
(`re-notes/phase-r-live-abi-validation.md` §"kallsyms constraint"): only
FUNCTION symbols (both exported `T` and local `t`) and exported data are
resolvable — stock *data* globals (e.g. `host_ring`) are **not** in kallsyms.
This is why:

- `natc_dump` reads the NAT-C table by calling the *accessor functions*
  (`drv_natc_key_entry_get` / `drv_natc_result_entry_get`, both local `t`
  symbols) rather than dereferencing the `g_natc_tbl_cfg` data global directly.
- `xrdp_peek`'s deref mode exists to chase a *resolvable* global-pointer symbol
  into a data struct that is not itself a resolvable symbol.

Two more environmental facts drive the design:

- **`CONFIG_STRICT_DEVMEM=y`** on the stock kernel blocks userspace `/dev/mem`
  and busybox `devmem`, so DDR/register reads must come from a kernel module.
- **`CONFIG_KPROBES` was OFF** on the original stock kernel, so Stage A/Phase-R
  avoid kprobes entirely; a rebuilt SDK kernel (2026-06-24) added
  `CONFIG_KPROBES=y` + `CONFIG_KALLSYMS_ALL=y`, which enabled `rdpa_trace.ko`
  (`README.md` §"Stage C"). `CONFIG_FTRACE` remains OFF, so the tracefs path
  (`rdpa-trace-events.sh`) is still unavailable.

### 2.3 Hardware blocks and registers the oracle scripts drive

`route-a-oracle.sh` reads (via `xrdp_peek.ko` MMIO) exactly the register set that
`re-notes/realhw/11-route-a-egress-spec.md` says Route A must program. Base
addresses and offsets, as written literally in the script and matching the spec:

| Block | Base | Offsets read | What it reveals |
|---|---|---|---|
| **QM global** | `0x82c00000` | window `+0x000..0x140` | `ENABLE_CTRL@0x000`, `FPM_CONTROL@0x00c`, `FPM_BASE@0x034`, `DDR_SOP@0x03c`, `MEM_AUTO_INIT@0x138`, `MEM_AUTO_INIT_STS@0x13c` |
| **QM RUNNER_GRP** | `0x82c00300` | 15 groups × 4 regs, stride `0x10`, window `0xf0` | `RNR_CONFIG` (`RNR_BB_ID[5:0]`, `RNR_TASK[11:8]`, `RNR_ENABLE[16]`) + `QUEUE_CONFIG` (`START_QUEUE[8:0]`, `END_QUEUE[24:16]`) per group |
| **BBH_TX** | `0x82890000`, stride `0x2000` (4 instances) | `+0x000` MACTYPE/BBCFG_1..3, `+0x050` RNRCFG_1/2, `+0x400`/`+0x4b0` LAN Q2RNR/QMQ, `+0x700`/`+0x7b0` unified Q2RNR/QMQ | which instance is QM-fed (`QMQ` bit set) → `route_a_bbh_inst`; TM ptraddr/task |
| **DS_TM RNR_MEM** | `0x827e2d1c` | window `0x20` | core7 `FIRST_QUEUE_MAPPING` (RDD `0x2d1c`), big-endian SRAM |
| **US_TM RNR_MEM** | `0x827c36bc` | window `0x20` | core6 `FIRST_QUEUE_MAPPING` (RDD `0x36bc`), big-endian SRAM |

TM RNR_MEM addresses are computed in the spec/script as
`0x82700000 + core*0x20000 + rdd_off`: core7 → `0x827e2d1c`, core6 → `0x827c36bc`.
All of these are **side-effect-free config registers / SRAM** (not FIFO /
clear-on-read), which is why MMIO reads of them are treated as safe.

### 2.4 The control-plane symbols `rdpa_trace.ko` probes

Two probe groups (bitmask via `grp=`), defined in the `probes[]` table
(`rdpa_trace.c:90`):

**GRP_ROUTEA (`0x1`) — CPU_TX egress + QM/BBH setup:**
`rdpa_cpu_send_pbuf`, `rdpa_cpu_send_sysb`,
`rdpa_cpu_tx_port_enet_or_dsl_wan`, `ag_drv_qm_rnr_group_cfg_set`,
`ag_drv_bbh_tx_unified_configurations_q2rnr_set`,
`ag_drv_bbh_tx_unified_configurations_qmq_set`.

**GRP_OFFLOAD (`0x2`) — flow-add / NAT-C / cmdlist:**
`ucast_attr_flow_add`, `l2_ucast_attr_flow_add`,
`drv_natc_key_result_entry_var_size_ctx_add`,
`rdpa_cmd_list_update_context`, `rdd_connection_entry_add`.

### 2.5 How observations feed the open driver

- **NAT-C key layout** (`natc_dump` + capture note) corrected the open
  `xrdp_build_key()` `w[3]` field ordering (was `proto<<24`; is
  `tos<<24 | 0x28<<16 | flags<<8 | 0x68`).
- **Context/cmdlist layout** (`natc_dump`) corrected `flow_offload.h`:
  `FC_UCAST` entry = 124 B, cmdlist at struct byte 24 (not 16), length carried
  in `command_list_length_32` (32-bit-word units), packed big-endian bitfields.
- **XPE opcode/offset placement** (`offload-live-validation.md`) confirmed
  `xpe_pack_cmd()` opcode bits[31:26] and offset8 bits[25:18] against the stock
  `0x6014` REPLACE word.
- **`rdpa_cpu_send_pbuf` arg order** (`rdpa_trace` kprobe) was live-confirmed as
  `(pbuf, info)` — reversing the RE's earlier cross-tree `(info, pbuf)` guess;
  the fix is baked into `rdpa_trace.c:98-102` and cited in commit
  `9b6c264`.
- **Route A params** (`route-a-oracle.sh`) pin `route_a_bbh_inst=1` (BBH_TX[1]
  is the only QM-fed instance) and the QM `ENABLE_CTRL=0x0307` /
  `MEM_AUTO_INIT_STS=0x01` values; `route_a_queue`/`route_a_grp` are supplied as
  the open driver's module params consumed at `bcm4916_runner.c:594` (the TX
  descriptor `first_level_q`) and in the QM/BBH bring-up.

---

## 3. Data structures

### 3.1 `natc_dump.c`

Compile-time constants (`natc_dump.c:61-63`):

- `NATCDUMP_KEY_LEN = 16` — NAT-C key entry size (RDD struct).
- `NATCDUMP_RES_BUF = 256` — result buffer; ≥ 124-B `FC_UCAST` context + any
  `_EXT`, deliberately oversized so the stock `memcpy` cannot overrun.
- `NATCDUMP_MAX_TABLES = 4` — the getters reject `table_id > 3`.

Module params:

- `tables` (int, default `0xf`) — bitmask of NAT-C table ids to scan.
- `max_index` (int, default `64`) — linear-scan ceiling per table.
- `dump_invalid` (int, default `0`) — also dump entries whose valid bit is 0.
- `idxlist[32]` + `idxlist_n` (int array) — explicit NAT-C indices (the `hw_id`s
  from `bs /Bdmf/e ucast`) to dump; overrides the linear scan. The table is
  hash-indexed and sparse, so active flows live at scattered indices == the
  `hw_id`, and a linear `0..N` scan finds nothing (`natc_dump.c:78-88`).

Stock accessor function-pointer typedefs (`natc_dump.c:91-96`), RE'd signatures:

```c
typedef int (*key_get_fn)(uint8_t table_id, uint32_t index,
                          uint8_t *valid_out, void *key_out);   /* drv_natc_key_entry_get   */
typedef int (*res_get_fn)(uint8_t table_id, uint32_t index, void *result_out); /* drv_natc_result_entry_get */
```

Plus `p_lookup` — a resolved pointer to `kallsyms_lookup_name` itself
(`natc_dump.c:99`), in case it is not directly linkable.

The **recovered stock-side layout** these accessors walk (from
`re-notes/stock-watch-capture.md` §0, *not* declared in this module):
`g_natc_tbl_cfg` is an array of 0x60-byte per-table config structs with
key-length at `+36`, result-length at `+40`, entry count at `+52`, key DDR base
at `+56`, result DDR base at `+64`, and a `spin_lock_bh` at `+0x320`. The key
getter memcpy's `key_len` bytes then `rev32`'s each word to host order and
returns `valid = key_bit[47]`; the result getter takes the lock and memcpy's
`result_len` **raw** (unswapped) bytes.

### 3.2 `xrdp_peek.c`

- `XRDP_PEEK_MAX = 1024` (`xrdp_peek.c:51`) — clamp for both `len` and `rlen`.
- Params: `sym` (charp), `off` (int), `len` (int, default 64), `deref` (int),
  `phys` (ulong), `rlen` (int, default 64), `allow_mmio` (int). All `0444`
  (read-only sysfs, load-time). `p_lookup` mirrors `natc_dump`.

Three modes selected by params: (1) `sym[+off]` mem dump; (2) `sym+off` deref →
dump at `*ptr`; (3) `phys` MMIO ioremap read (needs `allow_mmio=1`).

### 3.3 `rdpa_trace.c`

`struct deref_spec` (`rdpa_trace.c:62-69`) — one struct-field dump directive:

- `arg` — which arg register (`x0..x7`, index into `regs->regs[]`) holds the base
  pointer.
- `off` — byte offset into the pointed-at struct.
- `len` — bytes to read (clamped to `DEREF_MAX = 64`).
- `chase` (bool) — if set, read a pointer at `(arg+off)` first, then dump `len`
  bytes from `*that*` (used for a buffer reached via a pointer field, e.g. the
  cmdlist bytes behind `cmd_list_update_params_t`).
- `label` — human tag printed with the hex.

`struct trace_probe` (`rdpa_trace.c:71-81`):

- `sym` — kallsyms function name to probe.
- `group` — `GRP_ROUTEA (0x1)` / `GRP_OFFLOAD (0x2)`.
- `nregs` — how many arg regs to print (**see finding A-6: unused**).
- `desc` — one-line description of what the probe reveals.
- `deref[MAX_DEREF]` — up to 4 `deref_spec`s.
- runtime: `struct kprobe kp`, `atomic_t hits`, `bool armed`.

Constants: `MAX_DEREF = 4`, `DEREF_MAX = 64`, `RDPATRACE = "RDPATRACE"`,
`GRP_ROUTEA = 0x1`, `GRP_OFFLOAD = 0x2`. Params: `grp` (int, default
`GRP_ROUTEA`), `max_hits` (int, default 16; `0` = unlimited).

The `probes[]` array (`rdpa_trace.c:90-192`) is the RE knowledge base — every
entry carries the symbol, its arg registers, and the struct offsets recovered
from SDK RE, with inline comments citing what is confirmed vs cross-tree/guessed.
Key encoded facts:

- `rdpa_cpu_send_pbuf` (`:96-110`): `x0=pbuf`, `x1=info`;
  `info.queue_id @ +24` (egress QM queue), `info.port_obj @ +8`; `pbuf.fpm_bn @
  0x10`, `len u16 @ 0x16`, `flags @ 0x18`. Arg order **LIVE-CONFIRMED
  2026-06-24**.
- `ag_drv_qm_rnr_group_cfg_set` (`:128-139`): `x0=rnr_idx`, `x1=qm_rnr_group_cfg`
  (14 B): `start_queue@0`, `end_queue@2`, `pd_fifo_base@4`, `pd_fifo_size@6`,
  `upd_fifo_base@8`, `upd_fifo_size@10`, `rnr_bb_id@11`, `rnr_task@12`,
  `rnr_enable@13`.
- `drv_natc_key_result_entry_var_size_ctx_add` (`:163-173`): `x2=key(16B BE)`,
  `x3=result/ctx` (124 B, cmdlist@24), `x4=entry_idx*`. Arg count marked
  cross-tree, confirm live.
- `rdpa_cmd_list_update_context` (`:174-184`): `x0=params`; buffer pointer at
  `+0x10` (chased), len@`0x1c`, data_len@`0x20`, `final_len_32@0x2c` (OUT).

---

## 4. Function reference

### 4.1 `natc_dump.c`

#### `natcdump_hex(const char *tag, int tbl, int idx, const void *buf, int len)` — `natc_dump.c:101`

Custom hex dumper. Iterates `buf` 16 bytes per line, formatting each byte with
`scnprintf(p, 4, "%02x ", ...)` into a `48+1`-byte line buffer, and emits one
`pr_emerg("NATCDUMP %s tbl=%d idx=0x%x +%03d: %s\n", ...)` per line. It is a
hand-rolled replacement for `print_hex_dump_bytes` specifically because that
helper uses `KERN_DEBUG`; `pr_emerg` guarantees the lines survive any loglevel
and reach both console and ring buffer, which matters for grepping `dmesg` on a
device with no serial console. Pure formatting — touches no hardware. Callees:
`min`, `scnprintf`, `pr_emerg`. Caller: `natcdump_init`.

#### `natcdump_init(void)` — `natc_dump.c:120` (`__init`)

The whole module. Sequence:

1. Resolve `kallsyms_lookup_name` into `p_lookup` — first by asking kallsyms for
   its own name, else falling back to the direct symbol reference
   (`:126-128`).
2. Resolve `p_key_get = drv_natc_key_entry_get` and
   `p_res_get = drv_natc_result_entry_get` via `p_lookup` (`:130-131`). Prints
   the resolved pointers with `%px`. If either is NULL, bail `-ENOENT` with a
   message ("is rdpa.ko loaded?") — **no side effects** (`:136-139`).
3. `kzalloc` a 16-B `key` buffer and a 256-B `res` buffer; `-ENOMEM` on failure
   (`:141-147`).
4. Double loop over tables `0..3` (skipping bits not set in `tables`) and, per
   table, an index loop that is either the explicit `idxlist` (targeted mode) or
   the linear `0..max_index` scan (`:149-168`).
5. Per index: `memset` both buffers, call `p_key_get(tbl, idx, &valid, key)`.
   `rc != 0` means the getter rejected the table id / out-of-range → `continue`
   (`:174-176`). Skip invalid entries unless `dump_invalid` (`:178-179`).
6. On a valid hit: increment `valid_hits`, call `p_res_get(tbl, idx, res)` for
   the same slot, then emit an `==== ENTRY ====` marker, a `KEY` dump (16 B) and
   a `CTX` dump (128 B = the 124-B entry + a little `_EXT`) (`:181-190`).
7. Print a done line, free both buffers, `return 0` so the module **stays
   resident** (makes the dmesg lines easy to collect) (`:194-199`).

Why it is the lowest-risk capture: it patches nothing, writes no register/DDR,
and does no physical-address arithmetic itself — the stock accessors do all of
that. Hardware touched: **none directly**; indirectly the stock getter reads the
NAT-C key/result DDR regions. Ordering: the key getter must run first and return
`valid`/`rc` to gate the result getter. Callees: `p_lookup`, `p_key_get`,
`p_res_get`, `kzalloc`/`kfree`, `natcdump_hex`, `pr_emerg`.

#### `natcdump_exit(void)` — `natc_dump.c:202` (`__exit`)

Prints "unloaded (no side effects to undo)". There genuinely is nothing to
undo — no probes, no mappings, no writes.

### 4.2 `xrdp_peek.c`

#### `peek_hex(const char *tag, unsigned long base, const void *buf, int n)` — `xrdp_peek.c:84`

Same 16-byte-per-line `pr_emerg` hex dumper as `natcdump_hex`, but keyed on a
`base` address and tagged `XRDPEEK`. Pure formatting. Callers: `peek_mem`,
`peek_mmio`.

#### `peek_mem(unsigned long addr, int n)` — `xrdp_peek.c:99`

Fault-tolerant kernel-virtual read. Clamps `n` to `[1, XRDP_PEEK_MAX]`,
`kzalloc`s a buffer, and reads with `probe_kernel_read(buf, (void*)addr, n)` —
so a bad address yields `-EFAULT` and a diagnostic line instead of an oops
(`:114-120`). On success calls `peek_hex("MEM", ...)`. Frees, returns 0/`-EFAULT`/
`-ENOMEM`. Callers: `xrdp_peek_init` (mem and deref modes).

#### `peek_mmio(unsigned long pa, int n)` — `xrdp_peek.c:126`

The opt-in register reader. Refuses unless `allow_mmio` is set (`-EPERM`,
`:132-135`) — the guard that keeps MMIO off by default. Clamps `n` to
`[4, XRDP_PEEK_MAX]`, computes `words = n/4`, `ioremap(pa, n)`, `kzalloc`s a
word buffer, then reads each 32-bit word with `readl(va + i*4)` (`:152-153`).
The comment flags that `readl` returns a host-LE `u32` while Runner registers are
big-endian in DDR — the caller/decoder must account for endianness. Dumps via
`peek_hex("MMIO", ...)`, `iounmap`s, frees. **Hardware touched: real MMIO** — the
caller is responsible for pointing only at side-effect-free registers (never
FIFO / clear-on-read). Caller: `xrdp_peek_init` (phys mode).

#### `xrdp_peek_init(void)` — `xrdp_peek.c:160` (`__init`)

Mode dispatch:

- Resolve `p_lookup` (same idiom as `natc_dump`).
- If `phys` set → announce and call `peek_mmio(phys, rlen)`, `return 0` (stay
  loaded) (`:166-171`). MMIO mode ignores `sym`.
- Else require `sym` (`-EINVAL` if absent) (`:173-176`).
- Resolve `a = p_lookup(sym)`, print `sym -> addr`, bail `-ENOENT` if 0
  (`:179-186`), then `a += off`.
- If `deref`: `probe_kernel_read` a pointer at `a`, print `*(a) = ptr`, and if
  non-NULL `peek_mem(ptr, len)` — one level of pointer chasing into a struct
  (`:189-199`). Else `peek_mem(a, len)` directly (`:201`).

Callees: `p_lookup`, `peek_mmio`, `peek_mem`, `probe_kernel_read`.

#### `xrdp_peek_exit(void)` — `xrdp_peek.c:207` (`__exit`)

Prints "unloaded (no side effects to undo)". `iounmap` already happened inside
`peek_mmio`; nothing persists.

### 4.3 `rdpa_trace.c`

#### `trace_pre(struct kprobe *p, struct pt_regs *regs)` — `rdpa_trace.c:194`

The single kprobe pre-handler shared by every armed probe. `container_of`
recovers the owning `trace_probe` from the embedded `kp` (`:196`).
`atomic_inc_return(&tp->hits)`; if `max_hits` is set and the cap is exceeded,
return 0 immediately **without printing** — the cheap path that keeps a
per-packet hot probe from flooding dmesg / adding latency (`:199-201`). Otherwise
print `x0..x4` as `%px` (`:203-207`), then walk `deref[0..MAX_DEREF-1]`: skip
empty slots (`len == 0`), clamp `len` to `DEREF_MAX`, compute
`ptr = regs->regs[d->arg] + d->off`; if `chase`, `probe_kernel_read` a pointer at
`ptr` and follow it (fault → print `<chase fault>` and continue); then
`probe_kernel_read(buf, ptr, len)` (fault → `<fault>` and continue); format the
bytes into `hex` and emit a labelled line (`:209-240`). Returns 0.

Safety: pre-handler only, no writes; all struct reads are fault-tolerant. This
handler runs **on the live datapath** for hot symbols like `rdpa_cpu_send_pbuf`,
which is exactly why the `max_hits` cap and the "arm one group at a time"
guidance exist. Callees: `atomic_inc_return`, `probe_kernel_read`, `snprintf`,
`pr_emerg`.

#### `rdpa_trace_init(void)` — `rdpa_trace.c:244` (`__init`)

Iterates `probes[]`; for each whose `group & grp`: zero its `hits`, set
`kp.symbol_name = sym` and `kp.pre_handler = trace_pre`, and
`register_kprobe(&tp->kp)`. On failure (symbol absent on this build), `pr_warn`
and continue — **never fatal** (`:259-263`). On success set `armed = true`,
count it, and print the armed symbol + resolved `kp.addr` + `desc`
(`:264-267`). Prints the armed count and returns 0 even if zero armed
(informational). This is the mechanism note: kprobes-by-name work because rdpa
ships as modules whose symbols enter kallsyms once loaded. Callees:
`register_kprobe`, `atomic_set`, `pr_emerg`/`pr_warn`.

#### `rdpa_trace_exit(void)` — `rdpa_trace.c:274` (`__exit`)

Walks `probes[]`, and for each `armed` entry `unregister_kprobe(&tp->kp)`, prints
its final hit count, clears `armed`. This is the required teardown — kprobes must
be explicitly unregistered (unlike the other two modules which have nothing to
undo). Callees: `unregister_kprobe`, `atomic_read`, `pr_emerg`.

### 4.4 `route-a-oracle.sh`

Runs **on the device** (the stock (fallback) slot) with a matching-vermagic `xrdp_peek.ko`
present; read-only. Uses `xrdp_peek.ko` MMIO mode because the stock kernel's
`CONFIG_STRICT_DEVMEM=y` blocks userspace `/dev/mem`/`devmem` (`:5-8`). All
addresses are written literally because busybox ash 32-bit arithmetic overflows
above `0x80000000` (`:49-50`).

- **`KO` resolution** (`:19-20`) — `$1` or `./xrdp_peek.ko`; existence-checked.
- **`peek <phys> <rlen> <label>`** (`:23-30`) — one measurement: `rmmod` any
  stale instance, clear dmesg (`dmesg -c`), `insmod` with `phys=/rlen=
  allow_mmio=1`, echo a header, `dmesg | grep XRDPEEK`, `rmmod`. Each call is one
  `ioremap`+`readl` window.
- **`bbh_one <base> <n> <rnrcfg> <lan> <uni>`** (`:51-58`) — four `peek`s per
  BBH_TX instance: MACTYPE/BBCFG_1..3 at `+0x000` (16 B); RNRCFG_1/2 at `+0x050`
  (32 B); a `0xc0` window from `+0x400` reaching LAN QMQ at `+0x4b0`; the same
  from `+0x700` reaching unified QMQ at `+0x7b0`. Precomputed absolute addresses
  are passed in (no in-shell arithmetic).
- **Main body** (`:32-72`) — reads QM global (`0x82c00000 +0x140`), QM
  RUNNER_GRP (`0x82c00300 +0xf0`), four BBH_TX instances
  (`0x82890000` + `i*0x2000`), and the two TM `FIRST_QUEUE_MAPPING` SRAM words
  (`0x827e2d1c`, `0x827c36bc`). Then prints the suggested open-driver invocation:
  `insmod bcm4916-runner.ko route_a=1 route_a_grp=<G> route_a_queue=<Q>
  route_a_tm_bb_id=<BB> route_a_tm_task=<T> route_a_bbh_inst=<I>` (`:73-76`).

The mapping of these dumps to `route_a_*` values is done by hand against
`re-notes/realhw/11` §B/C (the LIVE ORACLE RESULTS section of that spec records
the outcome: `route_a_bbh_inst=1`, `ENABLE_CTRL=0x0307`,
`MEM_AUTO_INIT_STS=0x01`).

### 4.5 `rdpa-trace-events.sh`

The no-module tracefs equivalent of `rdpa_trace.ko`, for a kernel with
`CONFIG_KPROBE_EVENTS` (+`CONFIG_KPROBES`). The header explicitly warns this path
is **probably unavailable** on the current kprobe kernel because
`CONFIG_FTRACE` is OFF and `KPROBE_EVENTS` depends on the ftrace/tracing core
(`:10-14`) — so `rdpa_trace.ko` is the primary tool and this is a
"if/when FTRACE is enabled" fallback.

- Mounts debugfs if needed; bails if no tracefs (`:26-28`).
- **`add <name> <symbol> <args>`** (`:33-41`) — appends a `p:` probe line to
  `kprobe_events`; on success enables it, else reports SKIP (tolerates absent
  symbols / rejected arg syntax). arm64 kprobe arg syntax: register fetch
  `%x0..%x7`; struct field via `+OFFSET(%xN):type`.
- **`on [routea|offload|all]`** (`:44-68`) — clears the trace buffer, then adds
  the routea probes (`cpu_pbuf` = `rdpa_cpu_send_pbuf` with
  `qid=+24(%x1):u32 len=+22(%x0):u16 fpmbn=+16(%x0):u32`; `cpu_send`; `qm_grp`;
  `bbh_qmq`) and/or the offload probes (`ucast_add`, `natc_add`, `cmdlist` with
  `len=+28(%x0):u32`, `rdd_add`). Note the byte offsets mirror `rdpa_trace.c`:
  `queue_id@+24`, `pbuf.len@+22 (0x16)`, `fpm_bn@+16 (0x10)`, cmdlist
  `len@+28 (0x1c)`.
- **`show`** — `cat $T/trace`.
- **`off`** — disable + clear all the named kprobes and empty `kprobe_events`.

### 4.6 `Makefile`

`obj-m += natc_dump.o xrdp_peek.o rdpa_trace.o` (`:26-28`) — builds all three
`.ko` out-of-tree against the SDK `KDIR` with the aarch64 cross-toolchain.

- `SDK`/`KDIR`/`TC`/`ARCH`/`CROSS_COMPILE` default to the asuswrt-merlin.ng 4916
  "behnd" 4.19 tree + brcm-arm-hnd crosstools, all overridable (`:32-38`).
- `BUILD_NAME ?= gt-be98` (`:44`) works around the merlin kernel Makefile's
  `KBUILD_CFLAGS += -D$(MODEL)` (`MODEL = subst(-,,BUILD_NAME)`), which would
  otherwise emit a bare `-D`.
- `all` / `clean` targets invoke `$(MAKE) -C $(KDIR) M=$(PWD) ... modules|clean`.

vermagic **must match the running device kernel exactly**; the same `.ko` built
against a non-kprobe kernel would produce a non-functional `rdpa_trace.ko`, so
the tree used must be the exact kernel booted for the trace session (`:12-18`).

---

## 5. Audit findings

**A-1 (medium, latent buffer overflow) — `natc_dump.c:141` vs `:174`.** The
`key` buffer is a fixed `NATCDUMP_KEY_LEN = 16` bytes, but `drv_natc_key_entry_get`
memcpy's `key_len` bytes taken from the stock per-table config (`g_natc_tbl_cfg
+36`), *before* any valid check. The result buffer got a 256-B safety pad
(`:62`, "so the stock memcpy cannot overflow our buffer") but the key buffer did
not. The default `tables = 0xf` scans all four NAT-C tables; only table 0's
16-byte key stride is confirmed. If any of tables 1..3 has a key stride > 16, the
stock memcpy overruns the 16-byte kzalloc. Table 0 (the flow table) is the only
one actually needed — the code should either scan only table 0 by default or size
the key buffer like the result buffer.

**A-2 (low, ignored return) — `natc_dump.c:184`.** `p_res_get(...)` return value
is discarded. The result getter takes `spin_lock_bh` and can presumably fail; on
failure `res` stays zeroed (memset at `:171`) and a zero CTX is dumped as if
valid. No correctness impact for capture, but a silently-zero context could be
misread as real data.

**A-3 (low, redundant/contradictory fallback) — `natc_dump.c:126-128`,
`xrdp_peek.c:162-164`.** The idiom resolves `kallsyms_lookup_name` via
`kallsyms_lookup_name("kallsyms_lookup_name")` and, on NULL, falls back to the
*direct symbol reference* `p_lookup = kallsyms_lookup_name`. On 4.19 (this
target) `kallsyms_lookup_name` is still exported, so the direct reference links
and the self-lookup is redundant; if it were genuinely un-exported (5.7+), the
fallback line would fail to link, so the fallback cannot actually rescue the case
its comment describes. Harmless here but the comment overstates portability.

**A-4 (medium, safety-model contradiction) — `route-a-oracle.sh` vs
`re-notes/phase-r-live-abi-validation.md`.** phase-R records that a raw MMIO read
of XRDP/Runner space **hung the SoC unrecoverably** (`xrdp_peek phys=0x82a00000
allow_mmio=1`, FPM block → external bus abort), and concludes "**never** raw
`ioremap`+`readl` XRDP/Runner/RDD space from a graft module." Yet
`route-a-oracle.sh` does exactly that against QM (`0x82c00000`), BBH_TX
(`0x82890000`+), and TM RNR_MEM (`0x827e2d1c`/`0x827c36bc`).
`re-notes/realhw/11` §"LIVE ORACLE RESULTS" reports those reads *succeeded* — so
empirically some XRDP sub-blocks are readable and the FPM block is not. The tool
still trusts the caller to know the difference, and one wrong `phys=` value has
already required an automatic watchdog recovery. The
side-effect-free-only rule is real but enforced only by the human choosing
addresses, with a demonstrated failure mode.

**A-5 (low, MMIO endianness left to the reader) — `xrdp_peek.c:153`.** `readl`
returns host-LE `u32`; the comment notes Runner regs are big-endian in DDR, but
the tool dumps the raw words with no swap, so every consumer (and every
hand-mapping against `re-notes/realhw/11`) must byte-swap correctly. This is a
foot-gun for the manual mapping step; the QM/BBH offsets in the notes are given
byte-wise, so a mis-swap would silently mis-read a bitfield.

**A-6 (low, dead field) — `rdpa_trace.c:73` (`nregs`) vs `:203-207`.**
`trace_probe.nregs` is documented as "how many arg regs to print" but
`trace_pre` unconditionally prints exactly `x0..x4` (5 registers) regardless of
`nregs`. Probes declaring `nregs=5` that actually need `x5..x7` in a deref (none
currently do) would not have those registers printed for context. The field is
effectively unused.

**A-7 (info, unverified/cross-tree probe offsets) — `rdpa_trace.c:163-192`.**
Several `probes[]` entries carry offsets/arg-counts the comments flag as
unconfirmed: `drv_natc_key_result_entry_var_size_ctx_add` arg count is
"cross-tree - confirm live" (`:166`); `rdd_connection_entry_add` arg layout is
"unconfirmed; raw regs are the safety net" (`:187`). These are honestly labelled
placeholders, not silent guesses, but they are not yet silicon-validated (the
offload group has not had a live capture window — see A-8).

**A-8 (info, offload group unexercised) — `re-notes/offload-live-validation.md`
§5.** The GRP_OFFLOAD path (NAT/NAPT cmdlist, VLAN push/pop) was never captured
live because the idle lab device had no routed/NAT or bridge-accel flow
(`nflist` empty, `l2_ucast nflows=0`). Only the GDX-local (`is_routed=0`) SSH
flow existed, which validates opcode/offset *placement* but not the NAT/VLAN
operand math. `rdpa_trace.ko grp=2` and the offload probes are therefore READY
but effectively unverified against silicon.

**A-9 (info, tracefs fallback non-functional today) —
`rdpa-trace-events.sh:10-14`.** The script's own header states the current
kprobe kernel has `CONFIG_KPROBES=y` but `CONFIG_FTRACE` OFF, so
`CONFIG_KPROBE_EVENTS`/tracefs is unavailable and `$T` will not exist — the
script exits at `:28`. It is committed as a forward-looking fallback, not a
working tool on the present kernel.

**A-10 (low, `route_a_queue` not fully resolved) —
`re-notes/realhw/11` §"LIVE ORACLE RESULTS", G2/G4.** The oracle pinned
`route_a_bbh_inst=1` and the QM enable/auto-init values, but the
logical→physical queue map (which global QM queue the port's CPU-TX egress uses,
in the 80–111 vs literal-0 range) was **not** resolved: only candidate B
(`queue≈80, grp=0, tm_bb_id=7, tm_task=3`) and candidate A
(`queue=0, grp=1, tm_bb_id=6, tm_task=4`) are offered, to be tried empirically
via the module params. So the script's ultimate deliverable (a single pinned
`route_a_queue`) is incomplete.

**A-11 (low, `dmesg -c` clears the ring) — `route-a-oracle.sh:25`.** `peek()`
does `dmesg -c` before every `insmod`, discarding the entire kernel ring buffer
each iteration. On a live device this destroys any unrelated kernel messages
(including anything the operator might need if something goes wrong mid-run).

**A-12 (info, stale index-range comment) — `natc_dump.c:80`.** The comment says
the NAT-C table is "max ~17408 entries", but the live `hw_id`s captured in
`re-notes/stock-watch-capture.md` §1 range up to `0xef58` (61272), well beyond
17408. Either the size comment is stale or the `hw_id` encodes more than a raw
index; the linear scan default (`max_index=64`) is unaffected, but the comment
should not be trusted as the index bound.

---

## 6. Open questions / unknowns

1. **Physical QM egress queue for the LAN CPU-TX path (A-10).** The oracle
   captured `info.queue_id = 0` for locally-originated CPU→LAN frames, but that
   is a logical/port-relative rdpa queue, not necessarily physical QM queue 0.
   The logical→physical map (candidate A vs B) needs either a refined kprobe on
   the `port_obj`→queue-base resolution or an empirical egress test with the open
   driver's `route_a_queue`. Not determinable from the current code+notes.

2. **Whether BBH_TX[1] is the instance for the specific first LAN port**
   (`RUNNER_FIRST_PORT`/port_gphy1). The oracle proved BBH_TX[1] is the only
   QM-fed instance (`QMQ` bit set), but not that it maps to the port the open
   driver brings up first (`re-notes/realhw/11` last line).

3. **QM `MEM_AUTO_INIT` done semantics.** The live read showed
   `MEM_AUTO_INIT_STS@0x13c = 0x01` (done bit0), which the notes accept as
   confirming the SRAM auto-init poll — but the exact wait/timeout the stock
   `rdpa.ko` uses is still rdpa-internal; the open driver's poll is derived, not
   observed.

4. **GRP_OFFLOAD ABI against silicon (A-7/A-8).** The NAT-C-add, cmdlist-compile,
   and RDD-commit arg layouts in `probes[]` are RE'd from the SDK but never
   captured live (no NAT/VLAN flow existed). The unresolved lower half of XPE
   ABI UNKNOWN #3 (operand sub-packing, NOP framing `0x3f<<8` vs `0x3f<<10`) is
   explicitly stated to need the `xpe_api.armb53_6813.o` emitter disassembly, not
   more byte-staring (`re-notes/offload-live-validation.md` §3.2/§5).

5. **Which XRDP sub-blocks are MMIO-safe (A-4).** Empirically QM/BBH/RNR_MEM
   reads succeeded and the FPM block (`0x82a00000`) hung, but there is no
   derived rule for *why*, so `route-a-oracle.sh`'s address list is safe by
   observation only. Extending the oracle to new XRDP registers carries an
   unquantified SoC-hang risk that only the automatic watchdog recovery bounds.

6. **Result-region byte offsets of the context entry (ABI UNKNOWN #1) beyond
   what `natc_dump` proved.** `natc_dump` pinned cmdlist@24 and total=124 B and
   confirmed the cmdlist body byte-for-byte, but the full packed-bitfield map of
   the `FC_UCAST_FLOW_CONTEXT_ENTRY` is taken from the GPL 6813 autogen header,
   not independently observed; fields the bdmf shell prints by name
   (`is_hw_cso`, `tx_adjust`, etc.) were never diffed as raw DDR bytes
   (`re-notes/offload-live-validation.md` §3.3).
