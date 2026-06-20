/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pcs-bcm-xport.h - phylink PCS for the Broadcom BCM4916/6813 XPORT serdes+MPCS.
 *
 * The creator maps the MPCS (0x828c4000) + serdes (0x837ff500) register windows
 * and returns a phylink_pcs that bcm_sf2 selects for the XPORT 10G ports via
 * .mac_select_pcs. See pcs-bcm-xport.c for the register/bring-up details.
 */
#ifndef _PCS_BCM_XPORT_H
#define _PCS_BCM_XPORT_H

#include <linux/device.h>
#include <linux/phylink.h>

struct phylink_pcs *bcm_xport_pcs_create(struct device *dev,
					 void __iomem *serdes_base,
					 void __iomem *mpcs_base,
					 bool emulated);

#endif /* _PCS_BCM_XPORT_H */
