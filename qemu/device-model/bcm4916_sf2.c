/*
 * BCM4916 / GT-BE98 control-plane model for QEMU (Stage 2)
 *
 * Two QOM sysbus devices that, together, present enough of the BCM4916
 * switch + MDIO control plane that MAINLINE Linux can probe the integrated
 * Starfighter-2 switch via the bcm_sf2 / b53 DSA drivers and enumerate the
 * internal PHYs via mdio-bcm-unimac + phylib.
 *
 *   TYPE_BCM4916_SF2   - the SF2 switch core (compatible "brcm,bcm4916-switch",
 *                        bound by the bcm_sf2 BCM4916 of_match added in
 *                        driver/mainline-patches/)
 *   TYPE_BCM4916_MDIO  - the UNIMAC MDIO master (compatible "brcm,unimac-mdio")
 *
 * WHY model the BCM4908 SF2 layout rather than the raw 4916 FDT?
 * -----------------------------------------------------------------
 * The live GT-BE98 FDT exposes the switch only as a logical "RUNNER_SW" node
 * with NO reg of its own (re-notes/bcm4916-regmap.md, open question #1): the
 * SF2 core base is hidden behind the closed Runner stack. Mainline has no
 * "runner" driver. The realistic mainline path is the bcm_sf2/b53 DSA driver,
 * whose closest in-tree match is the "brcm,bcm4908-switch" binding (BCM4908 is
 * the prior SF2-class BCA SoC and is register-compatible at the SF2 core).
 * So we model the *mainline driver's* expected register interface, taken from:
 *
 *   - drivers/net/dsa/bcm_sf2.c        (probe flow, reg_readl/core_readl)
 *   - drivers/net/dsa/bcm_sf2_regs.h   (REG_* offsets, CORE_* offsets)
 *   - drivers/net/dsa/b53/b53_common.c (b53_switch_register/init, chip table)
 *   - drivers/net/mdio/mdio-bcm-unimac.c (MDIO CMD/CFG layout)
 *   - arch/arm64/boot/dts/broadcom/bcmbca/bcm4908.dtsi (reg / reg-names / ports)
 *
 * KEY PROBE FACTS (so we know exactly what must respond):
 *   * bcm_sf2 sets pdata->chip_id = priv->type = BCM4908_DEVICE_ID from the
 *     of_match data, then b53_switch_register() sees dev->pdata and SKIPS
 *     b53_switch_detect() entirely. => There is NO MDIO/register chip-ID read
 *     at probe; the chip ID comes from the DT compatible. (b53_common.c
 *     b53_switch_register(): "if (dev->pdata) dev->chip_id = pdata->chip_id;
 *     if (!dev->chip_id && b53_switch_detect(dev)) return -EINVAL;")
 *   * bcm_sf2 DOES read, in the "reg" region:
 *        REG_SWITCH_REVISION (4908 off 0x10)  -> top/core rev (cosmetic)
 *        REG_PHY_REVISION    (4908 off 0x14)  -> gphy rev (cosmetic)
 *   * bcm_sf2 reads, in the "core" region (page<<10 | reg<<2):
 *        CORE_IMP0_PRT_ID (0x0804) -> num_ports = val + 1
 *     plus many CORE_* config writes/read-backs during b53_switch_init().
 *   * PHY enumeration happens on the SEPARATE mdio-bcm-unimac bus; bcm_sf2
 *     registers that MDIO bus and phylib scans it. The fake PHYs here answer
 *     PHYID1/2 (real Broadcom EGPHY id) + BMSR (link up, autoneg complete).
 *
 * Register-layout cross-check against the vendor GPL SF2 driver
 * (~/re-sdk/.../bcmdrivers/opensource/net/enet/impl7/sf2.c): it drives the SF2
 * via the same PAGE_CONTROL / REG_SWITCH_MODE page+reg model b53 uses
 * (SF2SW_RREG(unit, PAGE_CONTROL, REG_SWITCH_MODE, ...)), and tracks an
 * e->rev_id read from a revision register -- consistent with the model below.
 *
 * Integration: instantiated and MMIO-mapped by hw/arm/virt.c create_bcm4916()
 * when the machine option `bcm4916=on` is set; virt.c also emits the matching
 * FDT nodes into the auto-generated DTB, so a stock mainline kernel binds
 * without an external -dtb. See qemu/README.md.
 *
 * Build: copied to hw/net/bcm4916_sf2.c on dev-build, added to
 * hw/net/meson.build under CONFIG_BCM4916, CONFIG_BCM4916=y in arm config.
 * Never build on dev-code.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "migration/vmstate.h"

/* ------------------------------------------------------------------ */
/* SF2 switch core                                                     */
/* ------------------------------------------------------------------ */

#define TYPE_BCM4916_SF2 "bcm4916-sf2"
OBJECT_DECLARE_SIMPLE_TYPE(Bcm4916Sf2State, BCM4916_SF2)

/*
 * Six register regions, sizes/layout taken verbatim from mainline
 * bcm4908.dtsi (the brcm,bcm4908-switch node). We back the whole switch
 * with one contiguous MMIO window and decode within it; the DT splits it
 * into 6 reg entries with matching reg-names, all relative to the same base:
 *   core      off 0x00000  size 0x40000
 *   reg       off 0x40000  size 0x00110
 *   intrl2_0  off 0x40340  size 0x00030
 *   intrl2_1  off 0x40380  size 0x00030
 *   fcb       off 0x40600  size 0x00034
 *   acb       off 0x40800  size 0x00208
 */
#define SF2_CORE_BASE   0x00000
#define SF2_CORE_SIZE   0x40000
#define SF2_REG_BASE    0x40000
#define SF2_WINDOW_SIZE 0x41000   /* covers up to acb end (0x40800+0x208) */

/* "reg" region offsets, from bcm_sf2_4908_reg_offsets[] in bcm_sf2.c.
 * These are RELATIVE to SF2_REG_BASE. */
#define REG4908_SWITCH_CNTRL     0x00
#define REG4908_SWITCH_STATUS    0x04
#define REG4908_DIR_DATA_WRITE   0x08
#define REG4908_DIR_DATA_READ    0x0c
#define REG4908_SWITCH_REVISION  0x10   /* <-- reg_readl(REG_SWITCH_REVISION) */
#define REG4908_PHY_REVISION     0x14   /* <-- reg_readl(REG_PHY_REVISION)   */
#define REG4908_SPHY_CNTRL       0x24
#define REG4908_CROSSBAR         0xc8
#define REG4908_RGMII_11_CNTRL   0x14c

/* "core" page-register decoding: SF2_PAGE_REG_MKADDR(page,reg)=page<<10|reg<<2
 * (bcm_sf2.c). The only core read whose value matters at probe is
 * CORE_IMP0_PRT_ID @ 0x0804 (= page 2, reg 1). bcm_sf2 does
 *   priv->hw_params.num_ports = CORE_IMP0_PRT_ID + 1 (then clamps to DSA_MAX);
 * the real port set comes from the b53 BCM4908 chip table (enabled_ports=0x1bf,
 * imp_port=8). We return the IMP/CPU port index 8, the natural value. */
#define CORE_IMP0_PRT_ID         0x0804

struct Bcm4916Sf2State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    /* sparse backing for the few stateful regs; everything else reads 0 */
    uint32_t reg_switch_cntrl;
    uint32_t reg_sphy_cntrl;
    /* a small register file for the "reg" region so writes read back */
    uint32_t regfile[0x110 / 4];
};

/*
 * SF2 top/core revision. bcm_sf2 just logs this; b53 uses its own core_rev
 * from the chip table. Use a plausible SF2 value: top 0x53 core rev 0x06.
 * REG_SWITCH_REVISION layout (bcm_sf2_regs.h):
 *   SF2_REV_MASK 0xffff (core rev in low 16), SWITCH_TOP_REV in bits >> 16.
 */
#define SF2_TOP_REV     0x0053
#define SF2_CORE_REV    0x0006
#define SF2_PHY_REV     0x0000

static uint64_t sf2_read(void *opaque, hwaddr addr, unsigned size)
{
    Bcm4916Sf2State *s = opaque;

    if (addr < SF2_CORE_SIZE) {
        /* CORE region (page<<10 | reg<<2) */
        switch (addr) {
        case CORE_IMP0_PRT_ID:
            return 8;   /* IMP/CPU port index for BCM4908-class SF2 */
        default:
            /* Most b53_switch_init() reads are status/read-backs that are
             * happy with 0. Return 0 to keep init progressing. */
            return 0;
        }
    }

    /* "reg" region (and beyond) */
    hwaddr roff = addr - SF2_REG_BASE;
    switch (roff) {
    case REG4908_SWITCH_REVISION:
        return ((uint32_t)SF2_TOP_REV << 16) | SF2_CORE_REV;
    case REG4908_PHY_REVISION:
        return SF2_PHY_REV;
    case REG4908_SWITCH_CNTRL:
        return s->reg_switch_cntrl;
    case REG4908_SPHY_CNTRL:
        return s->reg_sphy_cntrl;
    default:
        if (roff < sizeof(s->regfile)) {
            return s->regfile[roff / 4];
        }
        qemu_log_mask(LOG_UNIMP,
                      "bcm4916-sf2: unhandled read @0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void sf2_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Bcm4916Sf2State *s = opaque;

    if (addr < SF2_CORE_SIZE) {
        /* CORE writes: accept silently (config), nothing to model yet. */
        return;
    }

    hwaddr roff = addr - SF2_REG_BASE;
    switch (roff) {
    case REG4908_SWITCH_CNTRL:
        s->reg_switch_cntrl = val;
        break;
    case REG4908_SPHY_CNTRL:
        s->reg_sphy_cntrl = val;
        break;
    case REG4908_SWITCH_REVISION:
    case REG4908_PHY_REVISION:
        /* read-only */
        break;
    default:
        if (roff < sizeof(s->regfile)) {
            s->regfile[roff / 4] = val;
        } else {
            qemu_log_mask(LOG_UNIMP,
                          "bcm4916-sf2: unhandled write @0x%" HWADDR_PRIx
                          " = 0x%" PRIx64 "\n", addr, val);
        }
    }
}

static const MemoryRegionOps sf2_ops = {
    .read = sf2_read,
    .write = sf2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void bcm4916_sf2_reset(DeviceState *dev)
{
    Bcm4916Sf2State *s = BCM4916_SF2(dev);
    s->reg_switch_cntrl = 0;
    s->reg_sphy_cntrl = 0;
    memset(s->regfile, 0, sizeof(s->regfile));
}

static void bcm4916_sf2_init(Object *obj)
{
    Bcm4916Sf2State *s = BCM4916_SF2(obj);
    memory_region_init_io(&s->iomem, obj, &sf2_ops, s,
                          TYPE_BCM4916_SF2, SF2_WINDOW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_bcm4916_sf2 = {
    .name = TYPE_BCM4916_SF2,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(reg_switch_cntrl, Bcm4916Sf2State),
        VMSTATE_UINT32(reg_sphy_cntrl, Bcm4916Sf2State),
        VMSTATE_UINT32_ARRAY(regfile, Bcm4916Sf2State, 0x110 / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void bcm4916_sf2_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "BCM4916 Starfighter-2 switch core (bcm_sf2/b53)";
    dc->vmsd = &vmstate_bcm4916_sf2;
    device_class_set_legacy_reset(dc, bcm4916_sf2_reset);
}

/* ------------------------------------------------------------------ */
/* UNIMAC MDIO master + fake PHYs                                      */
/* ------------------------------------------------------------------ */

#define TYPE_BCM4916_MDIO "bcm4916-mdio"
OBJECT_DECLARE_SIMPLE_TYPE(Bcm4916MdioState, BCM4916_MDIO)

/* mdio-bcm-unimac register block (drivers/net/mdio/mdio-bcm-unimac.c) */
#define MDIO_CMD          0x00
#define  MDIO_START_BUSY  (1u << 29)
#define  MDIO_READ_FAIL   (1u << 28)
#define  MDIO_RD          (2u << 26)
#define  MDIO_WR          (1u << 26)
#define  MDIO_PMD_SHIFT   21
#define  MDIO_PMD_MASK    0x1f
#define  MDIO_REG_SHIFT   16
#define  MDIO_REG_MASK    0x1f
#define  MDIO_DATA_MASK   0xffff
#define MDIO_CFG          0x04

#define MDIO_WINDOW_SIZE  0x8   /* matches bcm4908.dtsi mdio reg = <... 0x8> */

#define MDIO_NUM_PHYS     32

/* C22 register ids */
#define MII_BMCR    0x00
#define MII_BMSR    0x01
#define MII_PHYID1  0x02
#define MII_PHYID2  0x03
#define MII_MMD_CTRL  0x0d   /* MMD access control (C45-over-C22) */
#define MII_MMD_DATA  0x0e   /* MMD access data */
#define MII_MMD_CTRL_NOINCR 0x4000
#define MII_MMD_DEVAD_MASK  0x1f

/* MMD register slots we model for the C45 BCM4916 XPHY (addr 9):
 *   VEND1 (0x1e) 0x400d  copper status (link bit5, speed bits[4:2])
 *   AN    (0x07) 0xfff9  1000BASE-T aux status (duplex/pause)
 * matching driver/mainline-patches/0004 (Broadcom GPL phy_drv_ext3.c).
 * 10G link-up status word: link(bit5)=1 + speed mode 6 (10G) -> 0x38. */
#define XPHY_VEND1_STATUS_REG 0x400d
#define XPHY_VEND1_STATUS_10G 0x0038
#define XPHY_AN_AUX_STAT_REG  0xfff9
#define XPHY_AN_AUX_STAT_VAL  0x0200   /* pause(bit1)=1; >1G forces full */

/*
 * PHY map for GT-BE98 (re-notes/bcm4916-regmap.md). We model the internal
 * EGPHYs and the external PHYs at their real MDIO addresses with their real
 * 32-bit PHY IDs (PHYID1<<16 | PHYID2):
 *   addr 2  internal EGPHY (eth2, first-light target) id 0x359050e0
 *   addr 9  external 10G BCM84891 (eth0)              id 0x359050e1
 *   addr 21 external multigig PHY (eth3)              id 0x35905081
 *   addr 1,3,4 the other internal EGPHYs (quad)       id 0x359050e0
 * Any other address reads back 0xffff (no PHY) -> phylib treats as absent.
 */
typedef struct {
    uint8_t addr;
    uint32_t phy_id;   /* 22-bit OUI + model/rev, as read via PHYID1/2 */
} Bcm4916PhyDesc;

static const Bcm4916PhyDesc bcm4916_phys[] = {
    { 1,  0x359050e0 },
    { 2,  0x359050e0 },   /* eth2 first-light EGPHY */
    { 3,  0x359050e0 },
    { 4,  0x359050e0 },
    { 9,  0x359050e1 },   /* eth0 external 10G */
    { 21, 0x35905081 },   /* eth3 external multigig */
};

struct Bcm4916MdioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t cmd;
    uint32_t cfg;
    /* per-PHY C22 register file (only used regs are meaningful) */
    uint16_t phy_regs[MDIO_NUM_PHYS][32];
    bool phy_present[MDIO_NUM_PHYS];
    /* C45-over-C22 MMD indirection (MII_MMD_CTRL/DATA) per PHY */
    uint16_t mmd_devad[MDIO_NUM_PHYS];
    uint16_t mmd_addr[MDIO_NUM_PHYS];
    /* sparse MMD store: only the few regs the XPHY driver reads */
    uint16_t xphy_vend1_status[MDIO_NUM_PHYS];
    uint16_t xphy_an_aux[MDIO_NUM_PHYS];
};

/* Resolve a selected MMD register (after MII_MMD_CTRL/DATA programming). */
static uint16_t mmd_reg_read(Bcm4916MdioState *s, int pa)
{
    uint16_t devad = s->mmd_devad[pa] & MII_MMD_DEVAD_MASK;
    uint16_t addr  = s->mmd_addr[pa];

    if (devad == 0x1e && addr == XPHY_VEND1_STATUS_REG) {
        return s->xphy_vend1_status[pa];
    }
    if (devad == 0x07 && addr == XPHY_AN_AUX_STAT_REG) {
        return s->xphy_an_aux[pa];
    }
    /* Unmodeled MMD regs read 0 (benign for genphy_c45 helpers). */
    return 0x0000;
}

static uint16_t mdio_phy_read(Bcm4916MdioState *s, int pa, int reg)
{
    if (pa >= MDIO_NUM_PHYS || !s->phy_present[pa]) {
        return 0xffff;   /* no PHY here */
    }
    reg &= 0x1f;
    /* C45-over-C22 MMD data read (function=DATA selected via CTRL) */
    if (reg == MII_MMD_DATA) {
        return mmd_reg_read(s, pa);
    }
    return s->phy_regs[pa][reg];
}

static void mdio_phy_write(Bcm4916MdioState *s, int pa, int reg, uint16_t val)
{
    if (pa >= MDIO_NUM_PHYS || !s->phy_present[pa]) {
        return;
    }
    reg &= 0x1f;
    /* C45-over-C22 MMD indirection (drivers/net/phy/phy-core.c
     * mmd_phy_indirect): CTRL<-devad, DATA<-regaddr, CTRL<-devad|NOINCR. */
    if (reg == MII_MMD_CTRL) {
        s->mmd_devad[pa] = val & MII_MMD_DEVAD_MASK;
        return;
    }
    if (reg == MII_MMD_DATA) {
        /* If a devad is selected but no addr latched yet, this DATA write is
         * the register address phase; otherwise it is a data write (ignored,
         * the modeled MMD regs are read-only status). The mainline sequence
         * always writes the address with the plain CTRL (no NOINCR) still
         * holding the devad, so latch addr here. */
        s->mmd_addr[pa] = val;
        return;
    }
    /* plain C22 register */
    s->phy_regs[pa][reg] = val;
}

static uint64_t mdio_read(void *opaque, hwaddr addr, unsigned size)
{
    Bcm4916MdioState *s = opaque;
    switch (addr) {
    case MDIO_CMD:
        /* synchronous model: start/busy is always already clear on read */
        return s->cmd & ~MDIO_START_BUSY;
    case MDIO_CFG:
        return s->cfg;
    default:
        return 0;
    }
}

static void mdio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Bcm4916MdioState *s = opaque;
    switch (addr) {
    case MDIO_CFG:
        s->cfg = val;
        break;
    case MDIO_CMD: {
        int pa  = (val >> MDIO_PMD_SHIFT) & MDIO_PMD_MASK;
        int reg = (val >> MDIO_REG_SHIFT) & MDIO_REG_MASK;

        /*
         * unimac_mdio_start() reads CMD, ORs in START_BUSY, writes it back,
         * then polls for START_BUSY to clear and reads the low 16 bits as
         * data. So the *transaction* is the prior CMD write that set RD/WR;
         * the start-busy write just kicks it. Handle both: if RD/WR bits are
         * present, perform the access now and stash the result in cmd.
         */
        if (val & MDIO_RD) {
            uint16_t data = mdio_phy_read(s, pa, reg);
            s->cmd = (val & ~(MDIO_DATA_MASK | MDIO_START_BUSY)) | data;
            if (!s->phy_present[pa]) {
                s->cmd |= MDIO_READ_FAIL;
            }
        } else if (val & MDIO_WR) {
            mdio_phy_write(s, pa, reg, val & MDIO_DATA_MASK);
            s->cmd = val & ~MDIO_START_BUSY;
        } else {
            /* a bare start-busy kick after a previous RD/WR cmd: rerun it */
            uint32_t prev = s->cmd;
            int ppa  = (prev >> MDIO_PMD_SHIFT) & MDIO_PMD_MASK;
            int preg = (prev >> MDIO_REG_SHIFT) & MDIO_REG_MASK;
            if (prev & MDIO_RD) {
                uint16_t data = mdio_phy_read(s, ppa, preg);
                s->cmd = (prev & ~(MDIO_DATA_MASK | MDIO_START_BUSY)) | data;
                if (!s->phy_present[ppa]) {
                    s->cmd |= MDIO_READ_FAIL;
                }
            } else {
                s->cmd = prev & ~MDIO_START_BUSY;
            }
        }
        break;
    }
    default:
        break;
    }
}

static const MemoryRegionOps mdio_ops = {
    .read = mdio_read,
    .write = mdio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void mdio_init_phy(Bcm4916MdioState *s, uint8_t addr, uint32_t id)
{
    if (addr >= MDIO_NUM_PHYS) {
        return;
    }
    s->phy_present[addr] = true;
    s->phy_regs[addr][MII_PHYID1] = (id >> 16) & 0xffff;
    s->phy_regs[addr][MII_PHYID2] = id & 0xffff;
    /* BMSR: 0x796d = 100/10 caps + autoneg-complete (bit5) + link-up (bit2)
     * + extended-status/autoneg-capable. Good enough for genphy to read
     * "link up, autoneg done". */
    s->phy_regs[addr][MII_BMSR]   = 0x796d;
    /* BMCR: autoneg enabled */
    s->phy_regs[addr][MII_BMCR]   = 0x1000;

    /* For the BCM4916 integrated 10G XPHY (id 0x359050e1, eth0 @ addr 9):
     * preload the Broadcom-proprietary VEND1 copper status to "link up, 10G"
     * and the AN aux status, so the C45 read_status path in
     * driver/mainline-patches/0004 reports link at 10G. */
    if (id == 0x359050e1) {
        s->xphy_vend1_status[addr] = XPHY_VEND1_STATUS_10G;
        s->xphy_an_aux[addr]       = XPHY_AN_AUX_STAT_VAL;
    }
}

static void bcm4916_mdio_reset(DeviceState *dev)
{
    Bcm4916MdioState *s = BCM4916_MDIO(dev);
    int i;
    s->cmd = 0;
    s->cfg = 0;
    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    memset(s->phy_present, 0, sizeof(s->phy_present));
    memset(s->mmd_devad, 0, sizeof(s->mmd_devad));
    memset(s->mmd_addr, 0, sizeof(s->mmd_addr));
    memset(s->xphy_vend1_status, 0, sizeof(s->xphy_vend1_status));
    memset(s->xphy_an_aux, 0, sizeof(s->xphy_an_aux));
    for (i = 0; i < ARRAY_SIZE(bcm4916_phys); i++) {
        mdio_init_phy(s, bcm4916_phys[i].addr, bcm4916_phys[i].phy_id);
    }
}

static void bcm4916_mdio_realize_init(Object *obj)
{
    Bcm4916MdioState *s = BCM4916_MDIO(obj);
    memory_region_init_io(&s->iomem, obj, &mdio_ops, s,
                          TYPE_BCM4916_MDIO, MDIO_WINDOW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_bcm4916_mdio = {
    .name = TYPE_BCM4916_MDIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cmd, Bcm4916MdioState),
        VMSTATE_UINT32(cfg, Bcm4916MdioState),
        VMSTATE_END_OF_LIST()
    },
};

static void bcm4916_mdio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "BCM4916 UNIMAC MDIO master + fake PHYs";
    dc->vmsd = &vmstate_bcm4916_mdio;
    device_class_set_legacy_reset(dc, bcm4916_mdio_reset);
}

/* ------------------------------------------------------------------ */
/* XPORT 10G blocks: XLMAC MAC + MPCS 10G PCS + Merlin serdes             */
/* ------------------------------------------------------------------ */

/*
 * Models enough of the BCM4916/6813 XPORT line-side register blocks that the
 * mainline phylink PCS (drivers/net/pcs/pcs-bcm-xport.c) + the bcm_sf2 XPORT
 * MAC helper (patch 0005) probe, bring the serdes/MPCS "up", and report a 10G
 * link for the XPORT ports (eth0/eth1/eth3). Register offsets/bitfields match
 * the driver, RE'd from the Broadcom GPL behnd SDK:
 *   serdes  0x837ff500 (3 cores x 0x100)   serdes_access_6813.h
 *             CONTROL +0x0c, STATUS +0x10 (link bit2/pll bit3/sigdet bit0),
 *             STATUS_1 +0x24 (per-speed nibbles; 10G nibble @ bit20)
 *   mpcs    0x828c4000                       mpcs.c (MPCS_REG @ +0xf8:
 *             pmd_rx_lock bit0, signal_detect bit1, fg_* resets bits3..5)
 *   xport   0x837f0000 (XLMAC cores)         XPORT_AG.h (CTRL +0x00 / MODE +0x08)
 *
 * "Link" model: the serdes reports PMD link + 10G speed and the MPCS reports
 * rx-lock once their reset/clk-enable bits have been written by the driver
 * (.pcs_enable), faking the proprietary Merlin PMD microcode lock. This lets
 * the whole phylink path run end to end under QEMU.
 */

/* serdes (per core, stride 0x100) */
#define SERDES_CORE_STRIDE      0x100
#define SERDES_CONTROL          0x0c
#define SERDES_STATUS           0x10
#define SERDES_STATUS_1         0x24
#define  SS_RX_SIGDET           (1u << 0)
#define  SS_LINK_STATUS         (1u << 2)
#define  SS_PLL_LOCK            (1u << 3)
#define  SC_SERDES_RESET        (1u << 2)
#define  SS1_10G_SHIFT          20
#define SERDES_WINDOW           0x300

/* mpcs */
#define MPCS_REG_OFF            0xf8
#define  MPCS_PMD_RX_LOCK       (1u << 0)
#define  MPCS_SIGNAL_DETECT     (1u << 1)
#define  MPCS_FG_POR_RSTB       (1u << 3)
#define  MPCS_FG_CLK_EN         (1u << 4)
#define  MPCS_FG_REFCLK_RSTB    (1u << 5)
#define  MPCS_FG_RESET_MASK     (MPCS_FG_CLK_EN | MPCS_FG_POR_RSTB | \
                                 MPCS_FG_REFCLK_RSTB)
#define MPCS_WINDOW             0x100

/* xport / XLMAC */
#define XPORT_WINDOW            0x8000

#define TYPE_BCM4916_XPORT "bcm4916-xport"
OBJECT_DECLARE_SIMPLE_TYPE(Bcm4916XportState, BCM4916_XPORT)

struct Bcm4916XportState {
    SysBusDevice parent_obj;
    MemoryRegion serdes_io;     /* 0x837ff500 */
    MemoryRegion mpcs_io;       /* 0x828c4000 */
    MemoryRegion xport_io;      /* 0x837f0000 */
    /* serdes per-core CONTROL latch (drives the "link" model) */
    uint32_t serdes_ctrl[3];
    uint32_t mpcs_reg;          /* MPCS_REG latch */
    uint32_t xlmac[XPORT_WINDOW / 4];   /* sparse XLMAC reg file */
};

/* serdes window */
static uint64_t serdes_read(void *opaque, hwaddr addr, unsigned size)
{
    Bcm4916XportState *s = opaque;
    int core = addr / SERDES_CORE_STRIDE;
    hwaddr off = addr % SERDES_CORE_STRIDE;

    if (core >= 3) {
        return 0;
    }
    switch (off) {
    case SERDES_CONTROL:
        return s->serdes_ctrl[core];
    case SERDES_STATUS:
        /* Report PMD link + PLL lock + sigdet once the driver has released
         * SERDES_RESET (SC_SERDES_RESET cleared) in CONTROL. */
        if (!(s->serdes_ctrl[core] & SC_SERDES_RESET)) {
            return SS_LINK_STATUS | SS_PLL_LOCK | SS_RX_SIGDET;
        }
        return 0;
    case SERDES_STATUS_1:
        /* lane0 at 10G once linked */
        if (!(s->serdes_ctrl[core] & SC_SERDES_RESET)) {
            return 1u << SS1_10G_SHIFT;
        }
        return 0;
    default:
        return 0;
    }
}

static void serdes_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Bcm4916XportState *s = opaque;
    int core = addr / SERDES_CORE_STRIDE;
    hwaddr off = addr % SERDES_CORE_STRIDE;

    if (core < 3 && off == SERDES_CONTROL) {
        s->serdes_ctrl[core] = val;
    }
}

static const MemoryRegionOps serdes_ops = {
    .read = serdes_read,
    .write = serdes_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* mpcs window */
static uint64_t mpcs_read(void *opaque, hwaddr addr, unsigned size)
{
    Bcm4916XportState *s = opaque;

    if (addr == MPCS_REG_OFF) {
        /* Report PMD rx-lock + signal detect once the driver released the
         * functional-group resets (clk_en/por_rstb/refclk_rstb). */
        if ((s->mpcs_reg & MPCS_FG_RESET_MASK) == MPCS_FG_RESET_MASK) {
            return s->mpcs_reg | MPCS_PMD_RX_LOCK | MPCS_SIGNAL_DETECT;
        }
        return s->mpcs_reg;
    }
    return 0;
}

static void mpcs_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Bcm4916XportState *s = opaque;

    if (addr == MPCS_REG_OFF) {
        s->mpcs_reg = val;
    }
}

static const MemoryRegionOps mpcs_ops = {
    .read = mpcs_read,
    .write = mpcs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* xport / XLMAC window: plain read/write-back register file */
static uint64_t xport_read(void *opaque, hwaddr addr, unsigned size)
{
    Bcm4916XportState *s = opaque;
    if (addr < XPORT_WINDOW) {
        return s->xlmac[addr / 4];
    }
    return 0;
}

static void xport_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Bcm4916XportState *s = opaque;
    if (addr < XPORT_WINDOW) {
        s->xlmac[addr / 4] = val;
    }
}

static const MemoryRegionOps xport_ops = {
    .read = xport_read,
    .write = xport_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void bcm4916_xport_reset(DeviceState *dev)
{
    Bcm4916XportState *s = BCM4916_XPORT(dev);
    int i;
    for (i = 0; i < 3; i++) {
        s->serdes_ctrl[i] = SC_SERDES_RESET;   /* held in reset until driver */
    }
    s->mpcs_reg = 0;
    memset(s->xlmac, 0, sizeof(s->xlmac));
}

static void bcm4916_xport_init(Object *obj)
{
    Bcm4916XportState *s = BCM4916_XPORT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->serdes_io, obj, &serdes_ops, s,
                          "bcm4916-serdes", SERDES_WINDOW);
    memory_region_init_io(&s->mpcs_io, obj, &mpcs_ops, s,
                          "bcm4916-mpcs", MPCS_WINDOW);
    memory_region_init_io(&s->xport_io, obj, &xport_ops, s,
                          "bcm4916-xport", XPORT_WINDOW);
    sysbus_init_mmio(sbd, &s->serdes_io);    /* mmio 0 */
    sysbus_init_mmio(sbd, &s->mpcs_io);      /* mmio 1 */
    sysbus_init_mmio(sbd, &s->xport_io);     /* mmio 2 */
}

static const VMStateDescription vmstate_bcm4916_xport = {
    .name = TYPE_BCM4916_XPORT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(serdes_ctrl, Bcm4916XportState, 3),
        VMSTATE_UINT32(mpcs_reg, Bcm4916XportState),
        VMSTATE_UINT32_ARRAY(xlmac, Bcm4916XportState, XPORT_WINDOW / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void bcm4916_xport_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "BCM4916 XPORT serdes+MPCS+XLMAC 10G blocks";
    dc->vmsd = &vmstate_bcm4916_xport;
    device_class_set_legacy_reset(dc, bcm4916_xport_reset);
}

/* ------------------------------------------------------------------ */

static const TypeInfo bcm4916_types[] = {
    {
        .name          = TYPE_BCM4916_SF2,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Bcm4916Sf2State),
        .instance_init = bcm4916_sf2_init,
        .class_init    = bcm4916_sf2_class_init,
    },
    {
        .name          = TYPE_BCM4916_MDIO,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Bcm4916MdioState),
        .instance_init = bcm4916_mdio_realize_init,
        .class_init    = bcm4916_mdio_class_init,
    },
    {
        .name          = TYPE_BCM4916_XPORT,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Bcm4916XportState),
        .instance_init = bcm4916_xport_init,
        .class_init    = bcm4916_xport_class_init,
    },
};

DEFINE_TYPES(bcm4916_types)
