/*
 * Broadcom GENET (Gigabit Ethernet) Wake-on-LAN support
 *
 * Copyright (c) 2014 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt)				"bcmgenet_wol: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/clk.h>
#include <linux/version.h>
#include <linux/platform_device.h>
#include <net/arp.h>

#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/phy.h>

#include "bcmgenet.h"

/* ethtool function - get WOL (Wake on LAN) settings, Only Magic Packet
 * Detection is supported through ethtool
 */
void bcmgenet_get_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct bcmgenet_priv *priv = netdev_priv(dev);
	u32 reg;

	wol->supported = WAKE_MAGIC | WAKE_MAGICSECURE;
	wol->wolopts = priv->wolopts;
	memset(wol->sopass, 0, sizeof(wol->sopass));

	if (wol->wolopts & WAKE_MAGICSECURE) {
		reg = bcmgenet_umac_readl(priv, UMAC_MPD_PW_MS);
		put_unaligned_be16(reg, &wol->sopass[0]);
		reg = bcmgenet_umac_readl(priv, UMAC_MPD_PW_LS);
		put_unaligned_be32(reg, &wol->sopass[2]);
	}
}

/* ethtool function - set WOL (Wake on LAN) settings.
 * Only for magic packet detection mode.
 */
int bcmgenet_set_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct bcmgenet_priv *priv = netdev_priv(dev);
	struct device *kdev = &priv->pdev->dev;
	u32 reg;

	if (!device_can_wakeup(kdev))
		return -ENOTSUPP;

	if (wol->wolopts & ~(WAKE_MAGIC | WAKE_MAGICSECURE))
		return -EINVAL;

	reg = bcmgenet_umac_readl(priv, UMAC_MPD_CTRL);
	if (wol->wolopts & WAKE_MAGICSECURE) {
		bcmgenet_umac_writel(priv, get_unaligned_be16(&wol->sopass[0]),
				     UMAC_MPD_PW_MS);
		bcmgenet_umac_writel(priv, get_unaligned_be32(&wol->sopass[2]),
				     UMAC_MPD_PW_LS);
		reg |= MPD_PW_EN;
	} else {
		reg &= ~MPD_PW_EN;
	}
	bcmgenet_umac_writel(priv, reg, UMAC_MPD_CTRL);

	/* Flag the device and relevant IRQ as wakeup capable */
	if (wol->wolopts) {
		device_set_wakeup_enable(kdev, 1);
		/* Avoid unbalanced enable_irq_wake calls */
		if (priv->wol_irq_disabled)
			enable_irq_wake(priv->wol_irq);
		priv->wol_irq_disabled = false;
	} else {
		device_set_wakeup_enable(kdev, 0);
		/* Avoid unbalanced disable_irq_wake calls */
		if (!priv->wol_irq_disabled)
			disable_irq_wake(priv->wol_irq);
		priv->wol_irq_disabled = true;
	}

	priv->wolopts = wol->wolopts;

	return 0;
}

static int bcmgenet_poll_wol_status(struct bcmgenet_priv *priv)
{
	struct net_device *dev = priv->dev;
	int retries = 0;

	while (!(bcmgenet_rbuf_readl(priv, RBUF_STATUS)
		& RBUF_STATUS_WOL)) {
		retries++;
		if (retries > 5) {
			netdev_crit(dev, "polling wol mode timeout\n");
			return -ETIMEDOUT;
		}
		mdelay(1);
	}

	return retries;
}

int bcmgenet_wol_power_down_cfg(struct bcmgenet_priv *priv,
				enum bcmgenet_power_mode mode)
{
	struct net_device *dev = priv->dev;

	struct bcmgenet_rxnfc_rule *rule;
	u32 reg, hfb_ctrl_reg;
	int retries = 0;

	if (mode != GENET_POWER_WOL_MAGIC) {
		netif_err(priv, wol, dev, "unsupported mode: %d\n", mode);
		return -EINVAL;
	}

	/* disable RX */
	reg = bcmgenet_umac_readl(priv, UMAC_CMD);
	reg &= ~CMD_RX_EN;
	bcmgenet_umac_writel(priv, reg, UMAC_CMD);
	mdelay(10);

	hfb_ctrl_reg = bcmgenet_hfb_reg_readl(priv, HFB_CTRL);
	reg = (hfb_ctrl_reg & ~RBUF_HFB_EN) | RBUF_ACPI_EN;
	bcmgenet_hfb_reg_writel(priv, reg, HFB_CTRL);

	reg = bcmgenet_umac_readl(priv, UMAC_MPD_CTRL);
	reg |= MPD_EN;
	bcmgenet_umac_writel(priv, reg, UMAC_MPD_CTRL);

	/* Do not leave UniMAC in MPD mode only */
	retries = bcmgenet_poll_wol_status(priv);
	if (retries < 0) {
		reg = bcmgenet_umac_readl(priv, UMAC_MPD_CTRL);
		reg &= ~MPD_EN;
		bcmgenet_umac_writel(priv, reg, UMAC_MPD_CTRL);
		bcmgenet_hfb_reg_writel(priv, hfb_ctrl_reg, HFB_CTRL);
		return retries;
	}

	netif_dbg(priv, wol, dev, "MPD WOL-ready status set after %d msec\n",
		  retries);

	/* Switch enet_clock_sel */
	clk_prepare_enable(priv->clk_wol);
	if (priv->internal_phy)
		clk_set_parent(priv->clk_mux, priv->clk_slow);

	reg = 0;
	list_for_each_entry(rule, &priv->rxnfc_list, list) {
		if (rule->fs.ring_cookie == 0)
			reg |= (1 << rule->fs.location);
	}
	if (reg) {
		bcmgenet_hfb_reg_writel(priv, reg, HFB_FLT_ENABLE_V3PLUS + 4);
		reg = RBUF_HFB_EN | RBUF_ACPI_EN;
		bcmgenet_hfb_reg_writel(priv, reg, HFB_CTRL);
	}

	/* Enable CRC forward */
	reg = bcmgenet_umac_readl(priv, UMAC_CMD);
	priv->crc_fwd_en = 1;
	reg |= CMD_CRC_FWD;

	/* Receiver must be enabled for WOL MP detection */
	reg |= CMD_RX_EN;
	bcmgenet_umac_writel(priv, reg, UMAC_CMD);

	if (priv->hw_params->flags & GENET_HAS_EXT) {
		reg = bcmgenet_ext_readl(priv, EXT_EXT_PWR_MGMT);
		reg &= ~EXT_ENERGY_DET_MASK;
		bcmgenet_ext_writel(priv, reg, EXT_EXT_PWR_MGMT);
	}

	return 0;
}

void bcmgenet_wol_power_up_cfg(struct bcmgenet_priv *priv,
			       enum bcmgenet_power_mode mode)
{
	u32 reg;

	if (mode != GENET_POWER_WOL_MAGIC) {
		netif_err(priv, wol, priv->dev, "invalid mode: %d\n", mode);
		return;
	}

	reg = bcmgenet_umac_readl(priv, UMAC_MPD_CTRL);
	if (!(reg & MPD_EN))
		return;	/* already powered up so skip the rest */
	reg &= ~MPD_EN;
	bcmgenet_umac_writel(priv, reg, UMAC_MPD_CTRL);

	reg = bcmgenet_hfb_reg_readl(priv, HFB_CTRL);
	reg &= ~(RBUF_HFB_EN | RBUF_ACPI_EN);
	bcmgenet_hfb_reg_writel(priv, reg, HFB_CTRL);

	/* Disable CRC Forward */
	reg = bcmgenet_umac_readl(priv, UMAC_CMD);
	reg &= ~CMD_CRC_FWD;
	bcmgenet_umac_writel(priv, reg, UMAC_CMD);
	priv->crc_fwd_en = 0;

	/* Switch enet_clock_sel */
	clk_set_parent(priv->clk_mux, priv->clk_fast);
	clk_disable_unprepare(priv->clk_wol);
}
