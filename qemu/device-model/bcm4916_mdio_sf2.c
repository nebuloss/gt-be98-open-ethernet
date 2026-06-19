/*
 * SKELETON — BCM4916 SF2 MDIO controller + fake GPHY (QEMU sysbus device)
 *
 * STATUS: design skeleton only. NOT wired into a QEMU machine yet, NOT built.
 *         This is Stage 2 of the plan in qemu/README.md. It exists so the
 *         driver's MDIO/PHY probe path can eventually be exercised host-side
 *         without the real device.
 *
 * Models the "brcm,mdio-sf2" controller the mainline driver targets as
 * `mdio-bcm-unimac`. FDT-confirmed reg base (re-notes/bcm4916-regmap.md):
 *     mdio-sf2   SoC base 0x837ffd00, size 0x10   (+ 2nd reg 0xff85a024 size 4)
 *
 * The mdio-bcm-unimac register block (from mainline
 * drivers/net/mdio/mdio-bcm-unimac.c) is:
 *     MDIO_CMD   0x00   - command/data: start|op|pa(phy)|ra(reg)|data, bit29=busy/start
 *     MDIO_CFG   0x04   - clock divider / config
 *   (the driver polls MDIO_CMD for the start/busy bit to clear, then reads data)
 *
 * A real BCM4916 first-light model needs:
 *   1. this MDIO controller (read/write MMIO, decode C22 frames), plus
 *   2. a tiny "GPHY" behind it that answers ID + BMSR(link-up) so phy_device
 *      probe + genphy work, and ideally
 *   3. enough of UNIMAC (0x828a8000) / XPORT (0x837f0000) for bcm4908_enet probe.
 *
 * Build/registration (TODO, Stage 2): add to a QEMU machine (e.g. a
 * "bcm4916-virt" machine_init) via sysbus_create_simple / sysbus_mmio_map at the
 * SoC bases above, or splice onto -machine virt with -device on a custom build.
 * QEMU source tree on dev-build: ~/qemu-src. Build there, never on dev-code.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "qom/object.h"

#define TYPE_BCM4916_MDIO_SF2 "bcm4916-mdio-sf2"
OBJECT_DECLARE_SIMPLE_TYPE(Bcm4916MdioSf2State, BCM4916_MDIO_SF2)

/* register offsets within the 0x10 window @ 0x837ffd00 */
#define MDIO_CMD   0x00
#define MDIO_CFG   0x04

/* MDIO_CMD field layout (mdio-bcm-unimac C22) */
#define MDIO_START_BUSY  (1u << 29)
#define MDIO_READ_FAIL   (1u << 28)
#define MDIO_RD          (2u << 26)
#define MDIO_WR          (1u << 26)
#define MDIO_PMD_SHIFT   21          /* phy addr  */
#define MDIO_PMD_MASK    0x1f
#define MDIO_REG_SHIFT   16          /* reg addr  */
#define MDIO_REG_MASK    0x1f
#define MDIO_DATA_MASK   0xffff

/* fake single GPHY: respond at this phy address */
#define FAKE_GPHY_ADDR   0x01

struct Bcm4916MdioSf2State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t cmd;
    uint32_t cfg;
    /* minimal fake-PHY register file (C22, 32 regs) */
    uint16_t phy_regs[32];
};

/* C22 reg ids */
#define MII_BMCR    0x00
#define MII_BMSR    0x01
#define MII_PHYID1  0x02
#define MII_PHYID2  0x03

static uint16_t fake_phy_read(Bcm4916MdioSf2State *s, int reg)
{
    /* TODO: model autoneg result registers as needed by genphy */
    return s->phy_regs[reg & 0x1f];
}

static void fake_phy_write(Bcm4916MdioSf2State *s, int reg, uint16_t val)
{
    s->phy_regs[reg & 0x1f] = val;
}

static uint64_t mdio_read(void *opaque, hwaddr addr, unsigned size)
{
    Bcm4916MdioSf2State *s = opaque;
    switch (addr) {
    case MDIO_CMD:
        /* start/busy clears immediately in this synchronous model */
        return s->cmd & ~MDIO_START_BUSY;
    case MDIO_CFG:
        return s->cfg;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled read @0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
}

static void mdio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Bcm4916MdioSf2State *s = opaque;
    switch (addr) {
    case MDIO_CFG:
        s->cfg = val;
        break;
    case MDIO_CMD: {
        int pmd = (val >> MDIO_PMD_SHIFT) & MDIO_PMD_MASK;
        int reg = (val >> MDIO_REG_SHIFT) & MDIO_REG_MASK;
        if (pmd != FAKE_GPHY_ADDR) {
            /* no PHY here: read returns all-ones (genphy treats as absent) */
            s->cmd = (val & ~MDIO_DATA_MASK) | MDIO_DATA_MASK;
            break;
        }
        if (val & MDIO_RD) {
            s->cmd = (val & ~MDIO_DATA_MASK) | fake_phy_read(s, reg);
        } else if (val & MDIO_WR) {
            fake_phy_write(s, reg, val & MDIO_DATA_MASK);
            s->cmd = val;
        }
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled write @0x%" HWADDR_PRIx "\n",
                      __func__, addr);
    }
}

static const MemoryRegionOps mdio_ops = {
    .read = mdio_read,
    .write = mdio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void bcm4916_mdio_sf2_reset(DeviceState *dev)
{
    Bcm4916MdioSf2State *s = BCM4916_MDIO_SF2(dev);
    s->cmd = 0;
    s->cfg = 0;
    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    /* present a plausible Broadcom GPHY: link up, 1G, autoneg complete */
    s->phy_regs[MII_PHYID1] = 0x600d;   /* TODO: real BCM OUI/model */
    s->phy_regs[MII_PHYID2] = 0x84a2;
    s->phy_regs[MII_BMSR]   = 0x796d;   /* link up + autoneg-complete + caps */
}

static void bcm4916_mdio_sf2_init(Object *obj)
{
    Bcm4916MdioSf2State *s = BCM4916_MDIO_SF2(obj);
    memory_region_init_io(&s->iomem, obj, &mdio_ops, s,
                          TYPE_BCM4916_MDIO_SF2, 0x10);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void bcm4916_mdio_sf2_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "BCM4916 SF2 MDIO controller (skeleton)";
    /* device_class_set_legacy_reset(dc, bcm4916_mdio_sf2_reset); // wire on build */
    (void)bcm4916_mdio_sf2_reset;
}

static const TypeInfo bcm4916_mdio_sf2_info = {
    .name          = TYPE_BCM4916_MDIO_SF2,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Bcm4916MdioSf2State),
    .instance_init = bcm4916_mdio_sf2_init,
    .class_init    = bcm4916_mdio_sf2_class_init,
};

static void bcm4916_register_types(void)
{
    type_register_static(&bcm4916_mdio_sf2_info);
}

type_init(bcm4916_register_types)
