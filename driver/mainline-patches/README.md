# Mainline kernel patches — BCM4916 / ASUS GT-BE98 switch + PHY

Minimal, upstreamable mainline patches that make OUR `brcm,bcm4916-switch`
device tree bind end-to-end through the existing `bcm_sf2` / `b53` DSA stack,
and add the internal 1G EGPHY to the Broadcom PHY driver.

These were developed against the mainline tree on the build host (`~/mainline`,
v7.1/7.2-era) and **proven under the QEMU control-plane harness** (see
`qemu/README.md` — the switch now reports as **BCM4916**, not BCM4908).

## The patches

| Patch | File(s) touched | What it does |
|-------|-----------------|--------------|
| `0001-net-dsa-b53-add-BCM4916-chip-support.patch` | `drivers/net/dsa/b53/b53_priv.h`, `b53_common.c` | Adds `BCM4916_DEVICE_ID = 0x4916` and a chip-info row cloned from the BCM4908 SF2 row (5 GPHY ports + 10G crossbar port 7 + IMP port 8; `enabled_ports=0x1bf`, `imp_port=8`). |
| `0002-net-dsa-bcm_sf2-add-brcm-bcm4916-switch-binding.patch` | `drivers/net/dsa/bcm_sf2.c` | Adds `bcm_sf2_4916_data` (reuses `bcm_sf2_4908_reg_offsets` + the 4908 crossbar config; only `.type` differs) and a `brcm,bcm4916-switch` `of_device_id`. Also folds `BCM4916_DEVICE_ID` into the 4908 switch-cases (rgmii-cntrl, led-base, port-override, crossbar, 2G speed-up) so BCM4916 follows the identical 4908 register path. |
| `0003-net-phy-broadcom-add-BCM4916-internal-EGPHY-0x359050.patch` | `drivers/net/phy/broadcom.c`, `include/linux/brcmphy.h` | Adds an **EXACT-match** entry for the internal 1G EGPHY `0x359050e0`. Exact (not model-mask) because the model field `0x359050e` is shared with the external 10G PHY `0x359050e1` — a model-mask entry would wrongly claim the 10G PHY. The EGPHY is a standard Broadcom GPHY and reuses `bcm54xx_config_init`. |

### Why clone BCM4908?
The BCM4916 integrated switch is SF2-class and register-compatible with the
BCM4908 at the SF2 core (see `re-notes/bcm4916-regmap.md`). bcm_sf2 sets
`pdata->chip_id` from the DT compatible, so b53 skips MDIO chip detection — the
chip identity comes entirely from the `brcm,bcm4916-switch` compatible and the
b53 chip row. No register delta was found that requires diverging from 4908.

## How to apply + build (on the build host, never on dev-code)

```sh
cd ~/mainline
git am ../path/to/driver/mainline-patches/000*.patch     # or: patch -p1 < each
# kernel config (all built-in for the QEMU harness):
#   CONFIG_NET_DSA=y CONFIG_NET_DSA_BCM_SF2=y CONFIG_B53=y
#   CONFIG_NET_DSA_TAG_BRCM=y CONFIG_MDIO_BCM_UNIMAC=y
#   CONFIG_BROADCOM_PHY=y CONFIG_MACB=y
# (to keep NET_DSA=y: BRIDGE=y, VLAN_8021Q=y, HSR=n)
rtk make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image
```

## Verified result (QEMU harness, `QEMU_BCM4916=1`)

```
brcm-sf2 a800000.ethernet-switch: found switch: BCM4916, rev 0
brcm-sf2 ... eth2 ...: PHY [a900000.mdio--1:02] driver [Broadcom BCM4916 EGPHY]
brcm-sf2 ... eth0 ...: PHY [a900000.mdio--1:09] driver [Generic PHY]
DSA: tree 0 setup
brcm-sf2 a800000.ethernet-switch: Starfighter 2 top: 0.53, core: 0.06
```

- Switch binds as **BCM4916** via `brcm,bcm4916-switch` (was BCM4908 before).
- Internal EGPHY (`0x359050e0`, eth2) now matches the **Broadcom BCM4916 EGPHY**
  driver instead of Generic PHY.
- The external 10G PHY (`0x359050e1`, BCM84891, eth0) stays **Generic PHY** —
  see "PHY status" below; not addressed in this pass by design.

## PHY status / what is NOT done

- **External 10G `0x359050e1` (BCM84891, eth0):** left as Generic PHY. The
  in-tree `bcm84881.c` matches model `0x3590508X` (mask `0xfffffff0`), i.e. the
  *eth3* multigig PHY `0x35905081` — NOT this `0x359050eX` part. Properly
  supporting the BCM84891 is a non-trivial new C45 PHY effort (config/PCS/serdes
  bring-up) and is deliberately deferred; Generic PHY is acceptable for probe.
- **External multigig `0x35905081` (eth3, MDIO addr 21):** already matched by the
  existing `bcm84881.c` (`PHY_ID_MATCH_MODEL(0x35905080)`), so **no patch is
  needed** for it. The current QEMU FDT does not wire an eth3 port, so it is not
  exercised in the harness boot above.
