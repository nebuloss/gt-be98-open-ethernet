# gt-be98-open-ethernet

Open-source, **mainline-Linux** wired-Ethernet + switch driver for the **ASUS GT-BE98**
router (Broadcom **BCM4916** SoC). Replaces the closed Broadcom BCA stack
(`bcm_enet` + the proprietary Runner/RDPA accelerator).

Wired-datapath counterpart to the WiFi effort
([`gt-be98-open-wifi`](https://github.com/nebuloss/gt-be98-open-wifi), which replaces the
closed `dhd` with an open FullMAC driver). Shared END GOAL: **run the GT-BE98 on the latest
mainline Linux kernel with fully open drivers** — no vendor-4.19-locked closed blobs.

## Prime directive (milestone 1)

Bring **ONE wired port up on an open mainline driver and pass basic L2/IP traffic**
(link + ping/DHCP) on the BCM4916 MAC. Prove the open MAC talks to the PHY and moves
packets to/from the CPU. *Then* the switch (DSA), multi-port, the 10G PHY.

## Strategy — extend mainline, do not reimplement

| Layer  | Mainline base                         | BCM4916 work                                    |
|--------|---------------------------------------|-------------------------------------------------|
| DTS    | `ARCH_BCMBCA` (`.../bcmbca/`)         | new `bcm4916.dtsi` + `gt-be98.dts`              |
| MAC    | `bcm4908_enet` (UNIMAC-style)         | extend; RE the BCM4916 register deltas          |
| Switch | `b53` / `bcm_sf2` DSA                 | internal + external (2.5G) switch               |
| PHY    | `mdio-bcm-unimac` + new BCM84891 drv  | 10G NBASE-T PHY (mainline coverage partial)     |

### ★ Critical caveat
Mainline = **CPU-forwarded Ethernet only, NO Runner/RDPA hardware offload** (the
accelerator is not reverse-engineered anywhere public). Expect **~1–3 Gbps
software-forwarded**, not the 10G / 4×2.5G hardware rate. v1 deliverable =
"fully open, routes/bridges at reduced throughput". RE'ing the Runner is a separate,
multi-person-year effort — out of scope for v1.

## ★ Safety — Ethernet IS the management link

A broken Ethernet driver disconnects the device entirely (no WiFi-style fallback).
**Never take down the live management Ethernet without a tested recovery path**
(serial console / CFE netboot / auto-reverting boot slot). Primary dev loop = QEMU + RE.

## Layout

- `driver/`   — the open MAC / DSA / PHY driver sources (out-of-tree build for iteration)
- `dts/`      — `bcm4916.dtsi`, `gt-be98.dts`, bindings
- `docs/`     — design notes, port/PHY/switch topology, recovery procedures
- `re-notes/` — reverse-engineering findings (register maps, MDIO layout, serdes config)
- `qemu/`     — `qemu-system-aarch64` BCM4916 machine/device-model for safe host-side iteration

## Infra (shared with the WiFi effort)

- **dev-code** (this box, 10.0.50.20): source + git only — **no builds here**.
- **dev-build** (`ssh guillaume@10.0.50.21`): toolchain + KDIR; build via `rtk <cmd>`.
- **device** (`admin@10.0.0.8`, :2222 dropbear / :2223 OpenSSH): the live ports/switch.
- **RE container**: Proxmox LXC **CT 310** on `root@10.0.0.2` (dev-reverse); the `re` MCP.

See `docs/` for the detailed methodology and milestone tracker.
