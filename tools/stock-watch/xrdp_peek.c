// SPDX-License-Identifier: GPL-2.0-only
/*
 * xrdp_peek.c - Phase-R read-mostly graft probe for the open BCM4916 (XRDP/Runner)
 *               Ethernet driver effort.
 *
 * PURPOSE
 * -------
 * Confirm, against LIVE silicon running the STOCK BCA stack, the register/memory
 * offsets our open driver (driver/runner/bcm4916_runner.{c,h}) currently carries
 * as placeholders: the CPU host-ring base + write/read index locations, the
 * CPU_TX_RING_INDICES doorbell offset in RNR_MEM, the FPM pool config, and the
 * RDD/PSRAM block layout. It does this WITHOUT disturbing the datapath.
 *
 * THREE SAFE MODES (all read-only; pick via params):
 *   1. sym=<name> [off=N] [len=L]      - resolve a stock GLOBAL via kallsyms and
 *                                        dump L bytes at sym+off (fault-tolerant).
 *   2. sym=<name> off=N deref=1 [len=L]- read a pointer at sym+off, then dump L
 *                                        bytes at *that pointer (follow a global
 *                                        pointer into a ring/cfg struct).
 *   3. phys=0x.. rlen=L allow_mmio=1   - ioremap a register window and read L
 *                                        bytes as u32s. OFF by default; only for
 *                                        registers known to be side-effect-free
 *                                        (NEVER point at FIFO/clear-on-read regs).
 *
 * SAFETY
 * ------
 *  - Memory reads use probe_kernel_read(): a bad address returns -EFAULT instead
 *    of oopsing. No kprobes, no function patching, no writes anywhere.
 *  - MMIO mode is opt-in (allow_mmio=1) and bounded (rlen clamped); register
 *    reads only. The caller is responsible for choosing safe register offsets.
 *  - len/rlen clamped to XRDP_PEEK_MAX. Module bails cleanly if a symbol is
 *    absent. Nothing to undo on rmmod.
 *
 * OUTPUT: pr_emerg() hex dumps; grep dmesg for "XRDPEEK".
 *
 * Build/load: same vermagic-exact 4.19 recipe as natc_dump (see Makefile/README).
 *   insmod xrdp_peek.ko sym=g_some_global off=0 len=128
 *   insmod xrdp_peek.ko sym=g_ptr off=0 deref=1 len=64
 *   insmod xrdp_peek.ko phys=0x82a00000 rlen=0xc8 allow_mmio=1
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/kallsyms.h>
#include <linux/uaccess.h>
#include <linux/moduleparam.h>

#define XRDP_PEEK_MAX	1024

static char *sym;
module_param(sym, charp, 0444);
MODULE_PARM_DESC(sym, "stock kernel global symbol name to resolve via kallsyms");

static int off;
module_param(off, int, 0444);
MODULE_PARM_DESC(off, "byte offset added to the resolved symbol address");

static int len = 64;
module_param(len, int, 0444);
MODULE_PARM_DESC(len, "bytes to dump (clamped to 1024)");

static int deref;
module_param(deref, int, 0444);
MODULE_PARM_DESC(deref, "1 = treat sym+off as a pointer, dump bytes at *ptr");

static unsigned long phys;
module_param(phys, ulong, 0444);
MODULE_PARM_DESC(phys, "physical MMIO base to ioremap+read (needs allow_mmio=1)");

static int rlen = 64;
module_param(rlen, int, 0444);
MODULE_PARM_DESC(rlen, "MMIO bytes to read as u32 words (clamped to 1024)");

static int allow_mmio;
module_param(allow_mmio, int, 0444);
MODULE_PARM_DESC(allow_mmio, "1 = permit the phys/ioremap MMIO read mode");

/* kallsyms_lookup_name is not exported on all 4.19 builds; resolve it too. */
static unsigned long (*p_lookup)(const char *name);

static void peek_hex(const char *tag, unsigned long base, const void *buf, int n)
{
	int i;
	char line[16 * 3 + 1];

	for (i = 0; i < n; i += 16) {
		int j, m = min(16, n - i);
		char *p = line;

		for (j = 0; j < m; j++)
			p += scnprintf(p, 4, "%02x ", ((const u8 *)buf)[i + j]);
		pr_emerg("XRDPEEK %s @%lx +%03d: %s\n", tag, base, i, line);
	}
}

static int peek_mem(unsigned long addr, int n)
{
	u8 *buf;
	long rc;

	if (n < 1)
		n = 1;
	if (n > XRDP_PEEK_MAX)
		n = XRDP_PEEK_MAX;

	buf = kzalloc(n, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* fault-tolerant kernel read: bad addr -> -EFAULT, no oops */
	rc = probe_kernel_read(buf, (void *)addr, n);
	if (rc) {
		pr_emerg("XRDPEEK: probe_kernel_read(@%lx, %d) FAILED rc=%ld (bad addr?)\n",
			 addr, n, rc);
		kfree(buf);
		return -EFAULT;
	}
	peek_hex("MEM", addr, buf, n);
	kfree(buf);
	return 0;
}

static int peek_mmio(unsigned long pa, int n)
{
	void __iomem *va;
	u32 *buf;
	int i, words;

	if (!allow_mmio) {
		pr_emerg("XRDPEEK: MMIO mode requires allow_mmio=1; refusing.\n");
		return -EPERM;
	}
	if (n < 4)
		n = 4;
	if (n > XRDP_PEEK_MAX)
		n = XRDP_PEEK_MAX;
	words = n / 4;

	va = ioremap(pa, n);
	if (!va) {
		pr_emerg("XRDPEEK: ioremap(%lx, %d) failed\n", pa, n);
		return -ENOMEM;
	}
	buf = kzalloc(words * 4, GFP_KERNEL);
	if (!buf) {
		iounmap(va);
		return -ENOMEM;
	}
	for (i = 0; i < words; i++)
		buf[i] = readl(va + i * 4);	/* raw u32 (LE host); Runner regs are BE in DDR */
	peek_hex("MMIO", pa, buf, words * 4);
	kfree(buf);
	iounmap(va);
	return 0;
}

static int __init xrdp_peek_init(void)
{
	p_lookup = (void *)kallsyms_lookup_name("kallsyms_lookup_name");
	if (!p_lookup)
		p_lookup = kallsyms_lookup_name;

	if (phys) {
		pr_emerg("XRDPEEK MMIO phys=%lx rlen=%d allow_mmio=%d\n",
			 phys, rlen, allow_mmio);
		peek_mmio(phys, rlen);
		return 0;	/* stay loaded so dmesg is easy to collect */
	}

	if (!sym) {
		pr_emerg("XRDPEEK: need sym=<name> (mem) or phys=0x.. allow_mmio=1 (mmio)\n");
		return -EINVAL;
	}

	{
		unsigned long a = p_lookup(sym);

		pr_emerg("XRDPEEK sym=%s -> %lx (off=%d deref=%d len=%d)\n",
			 sym, a, off, deref, len);
		if (!a) {
			pr_emerg("XRDPEEK: symbol '%s' not found - is the stock module loaded?\n", sym);
			return -ENOENT;
		}
		a += off;

		if (deref) {
			unsigned long ptr = 0;

			if (probe_kernel_read(&ptr, (void *)a, sizeof(ptr))) {
				pr_emerg("XRDPEEK: failed to read pointer at %lx\n", a);
				return -EFAULT;
			}
			pr_emerg("XRDPEEK deref: *(%lx) = %lx\n", a, ptr);
			if (!ptr)
				return 0;
			peek_mem(ptr, len);
		} else {
			peek_mem(a, len);
		}
	}
	return 0;
}

static void __exit xrdp_peek_exit(void)
{
	pr_emerg("XRDPEEK unloaded (no side effects to undo)\n");
}

module_init(xrdp_peek_init);
module_exit(xrdp_peek_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("open BCM4916 ethernet driver effort");
MODULE_DESCRIPTION("Phase-R read-mostly kallsyms/MMIO peek probe for XRDP ABI confirmation");
