// SPDX-License-Identifier: GPL-2.0
/*
 * bcm4916_runner_pdev.c - platform_device shim for the open BCM4916 Runner
 * driver on a STOCK kernel that has no DT node for it.
 *
 * The BCM6813 stock kernel is built with CONFIG_OF_OVERLAY unset, so we cannot
 * splice a `runner@82000000` node in at runtime via configfs. Instead this shim
 * registers a platform_device named "bcm4916-runner" (== the driver's
 * driver.name), so the platform bus binds it to our driver BY NAME (no of_node).
 *
 * Only the XRDP MMIO window (0x82000000 + 0xCAF004, taken verbatim from the
 * stock rdpa_drv node) is provided. The CPU-RX IRQ (GIC SPI 75) is intentionally
 * omitted: mapping a raw SPI to a Linux virq without a DT node is fiddly, and the
 * driver treats the IRQ as optional and falls back to NAPI poll mode - which is
 * fine for a first-light takeover.
 *
 * USAGE (live, rmmod-recoverable takeover): rmmod the stock datapath stack first
 * (that frees the 0x82000000 window via request_mem_region release), THEN
 *   insmod bcm4916-runner.ko          # registers the platform_driver
 *   insmod bcm4916-runner-pdev.ko     # registers the device -> probe fires
 * Unload order to abort: rmmod bcm4916-runner-pdev (removes device -> driver
 * .remove), then rmmod bcm4916-runner. No DT change, no flash, no boot-state
 * change -> a plain reboot fully restores the stock firmware.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/dma-mapping.h>

#define XRDP_BASE	0x82000000UL
#define XRDP_SIZE	0x00caf004UL

static struct platform_device *runner_pdev;
static u64 runner_dma_mask = DMA_BIT_MASK(40);

static int __init runner_pdev_init(void)
{
	struct resource res = DEFINE_RES_MEM(XRDP_BASE, XRDP_SIZE);
	struct platform_device_info info = {
		.name		= "bcm4916-runner",
		.id		= -1,
		.res		= &res,
		.num_res	= 1,
		.dma_mask	= runner_dma_mask,
	};

	runner_pdev = platform_device_register_full(&info);
	if (IS_ERR(runner_pdev)) {
		pr_err("bcm4916-runner-pdev: register failed: %ld\n",
		       PTR_ERR(runner_pdev));
		return PTR_ERR(runner_pdev);
	}
	pr_info("bcm4916-runner-pdev: device registered (MEM 0x%08lx+0x%lx, poll mode)\n",
		XRDP_BASE, XRDP_SIZE);
	return 0;
}

static void __exit runner_pdev_exit(void)
{
	platform_device_unregister(runner_pdev);
	pr_info("bcm4916-runner-pdev: device unregistered\n");
}

module_init(runner_pdev_init);
module_exit(runner_pdev_exit);

MODULE_DESCRIPTION("platform_device shim for the open BCM4916 Runner driver (no-DT stock kernel)");
MODULE_AUTHOR("gt-be98-open-ethernet");
MODULE_LICENSE("GPL");
