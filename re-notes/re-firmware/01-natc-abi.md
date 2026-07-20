# NAT-C engine ABI — flow-install register interface, key mask, hash, table addressing

## Purpose

This note reverses the closed BCM4916/BCM6813 XRDP **NAT-C (NAT / connection
cache) engine** ABI: the exact register sequence the stock stack uses to install
a unicast flow (key + result/context) into the hardware connection table, the
per-table key **mask** rule, the **hash** that maps a masked key to a table
index, the command/doorbell encoding, and how a table-id selects a
direction/flow-class DDR table.

It closes reimplementation gaps **M1b** (flow-install ABI) and **U2** (NAT-C
indirect-register offsets, "ABI UNKNOWN #5" in `driver/runner/flow_offload.h`),
and lets us replace the placeholder `xrdp_natc_add` / `xrdp_natc_del`
(`driver/runner/bcm4916_runner.c:660-696`) and the `NATC_STAGE_*` /
`NATC_INDIR_*` / `NATC_CMD_*` defines (`driver/runner/flow_offload.h:56-62`) with
silicon-accurate encodings.

All facts below were read from stock objects by disassembly; no proprietary
source or verbatim listing is reproduced here. Raw disasm is retained only on the
build machine under `~/re-scratch/natc-abi/`.

## Method

Disassembled with `aarch64-linux-gnu-objdump -dr` (relocatable objects, so
external call/data reloc names print inline), one function at a time.

**rdpa.ko** (unstripped, `.../rdp/projects/BCM6813/target/rdpa/rdpa.ko`) — the
control-plane add path and the generated register accessors:

| symbol | addr | role |
|---|---|---|
| `drv_natc_key_result_entry_var_size_ctx_add` | `0xa8cf0` | top-level flow install |
| `drv_natc_key_idx_get` | `0xa8950` | key-mask + hash + slot allocation |
| `drv_natc_key_entry_add` | `0xa82e0` | SW shadow of a new key |
| `drv_natc_eng_key_result_write.constprop.0` | `0xa7640` | stage key+result into engine window |
| `drv_natc_eng_command_write.constprop.0` | `0xa7560` | issue command + poll busy |
| `ag_drv_natc_eng_command_status_set` | `0x107d00` | pack the command/status register |
| `ag_drv_natc_eng_key_result_set` | `0x1083a0` | write the key+result register array |
| `ag_drv_natc_indir_addr_set` / `_get` | `0x109c10` / `0x109c80` | indirect address register |
| `ag_drv_natc_indir_data_set` | `0x109d30` | indirect data window (21 words) |
| `ag_drv_natc_key_mask_get` / `_set` | `0x10b0e0` / `0x10af20` | per-table key mask |
| `ag_drv_natc_tbl_key_addr_set` / `ag_drv_natc_tbl_res_addr_set` | `0x10b930` / `0x10baf0` | per-table DDR key/result base |

**crossbow_natc.o** (`.../char/archer/impl1/crossbow/crossbow_natc.o`) — the
archer fast-path talks to the **same engine by direct MMIO** (`crossbow_reg_g`),
which pins the absolute register offsets and the busy/command encoding the
generated rdpa accessors hide behind a struct:

| symbol | addr | role |
|---|---|---|
| `crossbow_natc_hw_delete.constprop.0` | `0x0` | delete (key-only + cmd) |
| `crossbow_natc_hw_flush.constprop.0` | `0x170` | flush a table |
| `crossbow_natc_hw_lookup.constprop.0` | `0x270` | lookup (key-only + cmd) |
| `crossbow_natc_hash` | `0x1070` | archer-side hash |
| `crossbow_natc_flow_add` | `0x1150` | archer SW flow cache (not the HW path) |

Absolute block bases cross-checked against `docs/audit/08-hw-abi-regmap.md:95-98`
(NATC @ `0x82950000`; `NATC_TBL[0..7]` @ `+0x2d0`+n·`0x10`; `NATC_DDR_CFG` @
`+0x38c`; `NATC_KEY[0..7]` @ `+0x3b0`+n·`0x20`).

## ABI reference

### 1. Flow-install sequence (`drv_natc_key_result_entry_var_size_ctx_add`)

Signature (recovered): `(u8 table_id, u32 key[4], void *result, void *var_ctx,
struct{u32 len} *ctxlen)`.

1. **Validate** `table_id ≤ 3` (`cmp #3; b.hi`) → else `-8`. Hardware exposes 8
   tables (`NATC_TBL[0..7]`/`NATC_KEY[0..7]`); the RDPA control path uses only
   0..3. `table_id` is the direction/flow-class selector — each id owns its own
   key-mask register, DDR key-table base and DDR result-table base (§4).
2. `spin_lock_bh` on the global NAT-C lock.
3. **Apply the key mask** (§2): read the per-table mask
   (`ag_drv_natc_key_mask_get(table_id,0,&mask)`), then for each of the 4 key
   words `key[i] = key[i] & ~byteswap32(mask[3-i])`, in place.
4. `drv_natc_key_idx_get(table_id, key, &idx, ...)` — recomputes the masked key,
   hashes it (§3), and resolves the table slot index `idx`. Return 0 ⇒ the key
   is **new**; nonzero ⇒ already present.
5. If new: copy the variable-length context into the DDR shadow at
   `result_base + idx * ctx_entry_size` and record the key in the SW shadow
   (`drv_natc_key_entry_add`), bumping the per-table entry counter at
   `table_struct+0x58`.
6. **Stage** the result/context into the engine window:
   `drv_natc_eng_key_result_write` (§5), size = `table_struct+0x24` (key words) /
   `+0x28` (ctx entry size).
7. **Command**: `drv_natc_eng_command_write(table_id, 3)` → writes command **3**
   and polls busy (§6). Command 3 = "write/add entry".

Per-table geometry lives in a 0x60-byte struct array (base `.bss+0xd6d88`,
stride `0x60`): `+0x04` cached key mask, `+0x24` key size (bytes), `+0x28` ctx
entry size (bytes), `+0x40` DDR result-table base pointer, `+0x58` entry count.

### 2. Key-mask rule (corrects the driver's `key[i] &= ~mask` guess)

- The applied key is `masked[i] = key[i] AND NOT mask_word` — i.e. a mask **bit
  = 1 means "don't-care", zeroed out of the key** (confirmed `bic` = AND-NOT in
  both `var_size_ctx_add` @`0xa8d94` and `key_idx_get` @`0xa8a34`).
- The mask words are consumed in **reverse word order** and **byte-swapped**:
  `mask_applied_to_key[i] = byteswap32(mask[key_words-1-i])`. This is because the
  key is a big-endian byte string and the mask register array is stored in the
  opposite word order from the key array.
- Number of key words = `table_struct[+0x24] >> 2` (key size / 4); for the
  16-byte tables this is 4.
- Mask source: `ag_drv_natc_key_mask_get(table_id, word, *out)` reads the
  `NATC_KEY[table_id]` mask register bank (regmap `0x829503b0 + table_id*0x20`);
  the value is also cached at `table_struct+0x04`.

### 3. NAT-C hash (`drv_natc_key_idx_get` → index)

A global config byte at `natc_g+0x324` selects the hash primitive:

- **Mode 0 — XOR fold** (`0xa8a64`): seed `h = byteswap32(0x4899b351)` then for
  each masked key word `h ^= byteswap32(word)`. (Seed constant `0x4899b351`.)
- **Mode ≠ 0 — bit-by-bit CRC**: `rdd_crc_bit_by_bit_natc(masked_key, key_len)`
  (`0xa8b38`); the config byte further picks a CRC width variant.

The 32-bit `h` is then **folded to the table's index width** `N = 13 +
ddr_size_enum`, where `ddr_size_enum ∈ {0..5}` comes from
`_natc_tbl_ddr_size_enum(table_id, ddr_size)` (i.e. bigger DDR table ⇒ wider
index). Fold (XOR-fold):

```
t   = h ^ (h >> N)
idx = (t & ((1<<N)-1)) ^ (h >> 2N)      # for N ≤ 15
idx = (h & ((1<<N)-1)) ^ (h >> N)       # for N = 16 (2N collapses to a full word)
```

Observed widths: enum0→13 bits, enum1→14, enum2→15, enum3→16, enum4→17,
enum5→18. `drv_natc_find_empty_hash_key_entry` then does the open-addressed
probe from `idx`.

### 4. Table addressing (per-table DDR bases)

- `ag_drv_natc_tbl_key_addr_set(table_id, addr, …)` / `_tbl_res_addr_set(...)`
  program the **DDR base of the key table and result table for each `table_id`**
  into the `NATC_TBL[table_id]` register bank (regmap `0x829502d0 +
  table_id*0x10`, indexed `table_id<<3` in the block descriptor).
- The result entry for slot `idx` lives at
  `result_base + idx * ctx_entry_size` (`ctx_entry_size = table_struct[+0x28]`);
  the key table is addressed the same way with `key_size = table_struct[+0x24]`.
- `NATC_DDR_CFG` (regmap `0x8295038c`, accessor
  `ag_drv_natc_ddr_cfg_natc_ddr_size_*`) holds the per-table DDR size that feeds
  the hash index width `N` in §3.

### 5. Key + result staging window (`drv_natc_eng_key_result_write`)

- The engine key+result staging is a **single contiguous 0x48-byte (72-byte, 18
  ×u32) register window**, not the separate key/ctx PSRAM windows the driver
  currently models.
- Layout after the function's reorder: **key first** (16 bytes = 4 words),
  followed by the result/context (up to 56 bytes). The 4 key words are
  **word-reversed** into the front, and then **every one of the 18 words is
  byte-swapped (`rev32`)** — the whole entry is programmed big-endian.
- `ag_drv_natc_eng_key_result_set(0, 0, buf)` copies the 18 words into the
  `NATC_ENG` key_result register array.

### 6. Command / doorbell encoding (crossbow direct-MMIO = ground truth)

Offsets are relative to the NATC engine register block
(`crossbow_reg_g[+8]` → engine base):

| offset | register | notes |
|---|---|---|
| `+0x00` (status)/`+0x10` (cmd) | COMMAND / STATUS | write command, poll here |
| `+0x30..+0x3c` | KEY / HASH / INDEX staging | 4 words (16 B) written before the command (lookup/delete key) |

Command register at engine `+0x10`:

| field | bits | meaning |
|---|---|---|
| command | `[2:0]` | **1 = lookup/read, 3 = write/add-entry** |
| flush   | bit `16` | set with cmd ⇒ flush (`0x10001` = flush) |
| busy    | bit `4` (`0x10`) | 1 while the engine is processing; **poll until clear** (~1000 × `udelay`) |
| status  | bit `5` (`0x20`) / bit `7` | hit / miss-error result flags |

For a **flush**, `crossbow_natc_hw_flush` writes `table_id << 22` to engine
`+0x3c` (table select, bits `[24:22]`) then command `0x10001`.

The rdpa generated accessor `ag_drv_natc_eng_command_status_set` packs the same
register as a bitfield: **command = bits `[2:0]`**, **table_id = bits `[14:12]`**
(3-bit, 0..7), plus the same busy/status flags — confirming cmd low-bits and the
busy=bit4 poll. (Absolute offsets there are hidden behind the `NATC_ENG_BLOCK`
runtime pointer.)

### 7. Delete / no dedicated opcode

There is **no command value 4**. `crossbow_natc_hw_delete` (@`0x0`) writes the
target key's 4 hash/key words to engine `+0x30..+0x3c` and issues the **same
command 3**, overwriting/invalidating the slot; a whole-table wipe uses the
**flush** modifier (`0x10001`, §6). So the driver's `NATC_CMD_DEL = 4` is
incorrect — delete is a cmd-3 write of an invalidated entry, or a flush.

## Mapping to the open driver

| RE'd fact | driver placeholder (file:line) | status | conf. |
|---|---|---|---|
| ADD command = **3** | `NATC_CMD_ADD 3` — `flow_offload.h:61` | confirms | high |
| **No cmd 4**; delete = cmd-3 write-invalid / flush `0x10001` | `NATC_CMD_DEL 4` — `flow_offload.h:62` | **corrects** | high |
| 16-byte key = 4×`__be32`, big-endian | `struct natc_key { __be32 w[4] }` — `flow_offload.h:75` | confirms | high |
| Key mask = `key[i] &= ~byteswap32(mask[3-i])` (bit1 = don't-care), from `NATC_KEY[table_id]` | `xrdp_build_key` mask intent — `flow_offload.c:239-261` | corrects/pins (reverse-word + rev32) | high |
| Command reg at engine `+0x10`; busy=bit4 poll; not a PSRAM word | `NATC_INDIR_CMD 0x0204` — `flow_offload.h:59` | corrects/pins | high |
| Slot index is **hash-derived** (§3), not a monotonically-incremented counter | `idx = p->natc_next_idx++` — `bcm4916_runner.c:663` | corrects | high |
| Key+result = one 72-byte BE (rev32) window, key first | `NATC_STAGE_KEY 0x0100` / `NATC_STAGE_CTX 0x0120` — `flow_offload.h:56-57` | corrects/pins | high |
| Install seq = mask→idx_get(hash)→key_result_write→cmd3 | `xrdp_natc_add` steps — `bcm4916_runner.c:660-685` | pins full sequence | high |
| Per-table DDR key/result bases via `NATC_TBL[table_id]` @ `0x829502d0+n*0x10` | (none — new) `flow_offload.h:45-46` `XRDP_OFF_NATC` | newly-pins | med |
| Hash = XOR-fold seed `0x4899b351` **or** `rdd_crc_bit_by_bit_natc`; index width `N=13+ddr_size_enum` | (none — new) | newly-pins | med |
| `table_id` 0..3 (HW 0..7) = direction/flow-class selector | (none — new) | newly-pins | med |

## Unresolved

- **Absolute base of the NATC engine command/indir sub-block.** The command reg
  is engine`+0x10` and key staging engine`+0x30..0x3c` (crossbow, high
  confidence), but that engine base is reached through `crossbow_reg_g[+8]` /
  the rdpa `NATC_ENG_BLOCK` runtime pointer; the regmap pins NATC@`0x82950000`
  with `TBL@+0x2d0`, `KEY@+0x3b0`, `DDR_CFG@+0x38c` but not the ENG/INDIR
  sub-block offset. Needs a live devmem/FDT read on silicon to fix absolutely.
- **Exact per-table key byte composition** (which flow field lands in which key
  byte) is set by the RDD key-compose path (`rdd_l2_flow_key_var_size_ctx_compose`
  and the `natc_*_key` table config), not by these functions; the live capture in
  `re-notes/stock-watch-capture.md` remains the source for the concrete byte map.
- **Which bit marks an entry invalid** on a cmd-3 delete (valid bit vs. zeroed
  key) is not distinguishable from `crossbow_natc_hw_delete` alone.
- **`ag_drv_natc_eng_key_result_set` register stride** (the 18-word array's exact
  MMIO offset pattern) is generated code hidden behind the block pointer; only
  the logical 72-byte BE layout is settled.
- Which CRC variant `rdd_crc_bit_by_bit_natc` implements per config byte
  (CRC-32 vs. a Broadcom polynomial) was not disassembled.

## Sources

- `rdpa.ko` (BCM6813 target, unstripped): `drv_natc_key_result_entry_var_size_ctx_add`
  `0xa8cf0`, `drv_natc_key_idx_get` `0xa8950`, `drv_natc_eng_key_result_write.constprop.0`
  `0xa7640`, `drv_natc_eng_command_write.constprop.0` `0xa7560`,
  `ag_drv_natc_eng_command_status_set` `0x107d00`, `ag_drv_natc_indir_addr_set` `0x109c10`,
  `ag_drv_natc_indir_data_set` `0x109d30`, `ag_drv_natc_key_mask_get` `0x10b0e0`,
  `ag_drv_natc_tbl_key_addr_set` `0x10b930`, `ag_drv_natc_tbl_res_addr_set` `0x10baf0`.
- `crossbow_natc.o`: `crossbow_natc_hw_delete.constprop.0` `0x0`,
  `crossbow_natc_hw_flush.constprop.0` `0x170`, `crossbow_natc_hw_lookup.constprop.0` `0x270`.
- Register bases: `docs/audit/08-hw-abi-regmap.md:95-98`.
- Raw disasm retained at `~/re-scratch/natc-abi/` on the build machine (not committed).
