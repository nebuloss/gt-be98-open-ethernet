// SPDX-License-Identifier: GPL-2.0-only
/*
 * natc_dump.c - Stage A read-only NAT-C connection-table dumper for the open
 *               BCM4916 (XRDP/Runner) Ethernet driver effort.
 *
 * PURPOSE
 * -------
 * On a device running the STOCK Broadcom BCA stack (rdpa/pktrunner/cmdlist),
 * capture the RAW bytes of the already-populated NAT-C connection table in DDR:
 *   - the 16-byte NAT-C lookup KEY  (pins ABI UNKNOWN #5)
 *   - the FC_UCAST_FLOW_CONTEXT_ENTRY result + embedded cmdlist (pins UNKNOWN #1)
 * so the open driver's builders (driver/runner/{flow_offload,cmdlist}.{c,h}) and
 * the QEMU model can be validated/corrected byte-for-byte against real silicon.
 *
 * WHY THIS IS THE LOWEST-RISK CAPTURE (Stage A)
 * ---------------------------------------------
 * This module does NOT patch any function, does NOT touch any hardware register,
 * does NOT write DDR, and does NOT do its own physical-address arithmetic. It
 * simply CALLS the stock rdpa.ko's own read-only accessors:
 *
 *   int drv_natc_key_entry_get   (uint8_t table_id, uint32_t index,
 *                                 uint8_t *valid_out, void *key_out_16);
 *   int drv_natc_result_entry_get(uint8_t table_id, uint32_t index,
 *                                 void *result_out);
 *
 * (Signatures recovered by disassembly of rdpa.ko - see README. The key getter
 *  memcpy's key_len bytes then rev32's each word to host order, and returns the
 *  entry valid bit from key bit 47. The result getter takes a spin_lock_bh and
 *  memcpy's result_len raw bytes - no swap.)
 *
 * These are local (`t`) symbols, so we resolve them at load time via
 * kallsyms_lookup_name() (CONFIG_KALLSYMS=y on the device; CONFIG_KPROBES is
 * OFF, so we deliberately avoid kprobes). If a symbol is absent we bail cleanly
 * WITHOUT touching anything.
 *
 * SAFETY
 * ------
 *  - All buffers are fixed-size and bounded; getters fill at most their stride.
 *  - We allocate generous (256 B) output buffers, well over the 124 B context
 *    entry + any *_EXT, so the stock memcpy cannot overflow our buffer.
 *  - table_id is bounded 0..3 (the getters themselves reject table_id > 3).
 *  - index range is a module param (default small) so we never walk a huge table.
 *  - Read-only: no register writes, no DDR writes, no datapath change.
 *  - On unload nothing needs undoing (we registered no probes / hooks).
 *
 * OUTPUT
 * ------
 * pr_emerg() hex dumps (survive loglevel). Grep dmesg for "NATCDUMP".
 *
 * Build: out-of-tree against the device KDIR (see Makefile). vermagic must match
 * the running kernel exactly. Load: insmod natc_dump.ko [tables=0xf] [max_index=N].
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kallsyms.h>
#include <linux/moduleparam.h>

#define NATCDUMP_KEY_LEN	16	/* NAT-C key entry = 16 bytes (RDD struct) */
#define NATCDUMP_RES_BUF	256	/* >= 124 B FC_UCAST ctx + EXT, safety pad */
#define NATCDUMP_MAX_TABLES	4	/* getters reject table_id > 3 */

/* default: scan all 4 tables, first 64 indices each (the lab device has < 32) */
static int tables = 0xf;		/* bitmask of table ids to scan */
module_param(tables, int, 0644);
MODULE_PARM_DESC(tables, "bitmask of NAT-C table ids to scan (default 0xf = 0..3)");

static int max_index = 64;
module_param(max_index, int, 0644);
MODULE_PARM_DESC(max_index, "highest NAT-C index to probe per table (default 64)");

static int dump_invalid;
module_param(dump_invalid, int, 0644);
MODULE_PARM_DESC(dump_invalid, "1 = also dump entries whose valid bit is 0");

/*
 * Targeted index list. The NAT-C table is hash-indexed (max ~17408 entries),
 * so active flows live at sparse hash positions == the `hw_id` reported by
 * `bs /Bdmf/e ucast`. Pass them directly to dump exactly those entries instead
 * of walking the whole table. e.g. idxlist=0x76e1,0x1aad,0xfe55
 * If idxlist is empty we fall back to the [0..max_index) linear scan.
 */
static int idxlist[32];
static int idxlist_n;
module_param_array(idxlist, int, &idxlist_n, 0644);
MODULE_PARM_DESC(idxlist, "explicit NAT-C indices (hw_ids) to dump; overrides linear scan");

/* ---- stock accessor function-pointer typedefs (RE'd signatures) ---------- */
typedef int (*key_get_fn)(uint8_t table_id, uint32_t index,
			  uint8_t *valid_out, void *key_out);
typedef int (*res_get_fn)(uint8_t table_id, uint32_t index, void *result_out);

static key_get_fn p_key_get;
static res_get_fn p_res_get;

/* kallsyms_lookup_name is not exported on all 4.19 builds; resolve it too. */
static unsigned long (*p_lookup)(const char *name);

static void natcdump_hex(const char *tag, int tbl, int idx,
			 const void *buf, int len)
{
	/* print_hex_dump_bytes uses KERN_DEBUG; build our own at EMERG so the
	 * lines always reach the console + ring buffer. 16 bytes per line. */
	int i;
	char line[16 * 3 + 1];

	for (i = 0; i < len; i += 16) {
		int j, n = min(16, len - i);
		char *p = line;

		for (j = 0; j < n; j++)
			p += scnprintf(p, 4, "%02x ", ((const u8 *)buf)[i + j]);
		pr_emerg("NATCDUMP %s tbl=%d idx=0x%x +%03d: %s\n",
			 tag, tbl, idx, i, line);
	}
}

static int __init natcdump_init(void)
{
	int tbl, idx, valid_hits = 0;
	u8 *key, *res;

	/* Resolve kallsyms_lookup_name itself (it may not be EXPORT'd). */
	p_lookup = (void *)kallsyms_lookup_name("kallsyms_lookup_name");
	if (!p_lookup)
		p_lookup = kallsyms_lookup_name; /* fall back to direct ref */

	p_key_get = (key_get_fn)p_lookup("drv_natc_key_entry_get");
	p_res_get = (res_get_fn)p_lookup("drv_natc_result_entry_get");

	pr_emerg("NATCDUMP load: key_get=%px res_get=%px tables=0x%x max_index=%d\n",
		 p_key_get, p_res_get, tables, max_index);

	if (!p_key_get || !p_res_get) {
		pr_emerg("NATCDUMP: stock NAT-C accessors NOT found - is rdpa.ko loaded? Bailing (no side effects).\n");
		return -ENOENT;
	}

	key = kzalloc(NATCDUMP_KEY_LEN, GFP_KERNEL);
	res = kzalloc(NATCDUMP_RES_BUF, GFP_KERNEL);
	if (!key || !res) {
		kfree(key);
		kfree(res);
		return -ENOMEM;
	}

	for (tbl = 0; tbl < NATCDUMP_MAX_TABLES; tbl++) {
		int li;

		if (!(tables & (1 << tbl)))
			continue;

		for (li = 0; ; li++) {
			u8 valid = 0;
			int rc;

			/* targeted mode if idxlist given, else linear 0..max_index */
			if (idxlist_n) {
				if (li >= idxlist_n)
					break;
				idx = idxlist[li];
			} else {
				if (li >= max_index)
					break;
				idx = li;
			}

			memset(key, 0, NATCDUMP_KEY_LEN);
			memset(res, 0, NATCDUMP_RES_BUF);

			/* key getter: returns 0 and sets *valid on a real slot */
			rc = p_key_get((u8)tbl, (u32)idx, &valid, key);
			if (rc != 0)
				continue;	/* table_id rejected / out of range */

			if (!valid && !dump_invalid)
				continue;

			valid_hits++;

			/* result getter for the same slot (raw context bytes) */
			p_res_get((u8)tbl, (u32)idx, res);

			pr_emerg("NATCDUMP ==== ENTRY tbl=%d idx=0x%x valid=%d ====\n",
				 tbl, idx, valid);
			natcdump_hex("KEY", tbl, idx, key, NATCDUMP_KEY_LEN);
			/* dump 128 bytes of context (124 B entry + a little EXT) */
			natcdump_hex("CTX", tbl, idx, res, 128);
		}
	}

	pr_emerg("NATCDUMP done: %d valid entr%s dumped. Module stays resident; rmmod to remove.\n",
		 valid_hits, valid_hits == 1 ? "y" : "ies");

	kfree(key);
	kfree(res);
	return 0;	/* stay loaded so the dmesg lines are easy to collect */
}

static void __exit natcdump_exit(void)
{
	pr_emerg("NATCDUMP unloaded (no side effects to undo)\n");
}

module_init(natcdump_init);
module_exit(natcdump_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("open BCM4916 ethernet driver effort");
MODULE_DESCRIPTION("Stage A read-only NAT-C DDR connection-table dumper (via stock rdpa accessors)");
