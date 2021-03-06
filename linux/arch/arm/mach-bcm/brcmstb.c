/*
 * Copyright (C) 2013-2016 Broadcom
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/arm-gic.h>
#include <linux/of_platform.h>

#include <linux/brcmstb/cma_driver.h>
#include <linux/clocksource.h>
#include <linux/console.h>
#include <linux/of_address.h>
#include <asm/mach/map.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>

/*
 * Storage for debug-macro.S's state.
 *
 * This must be in .data not .bss so that it gets initialized each time the
 * kernel is loaded. The data is declared here rather than debug-macro.S so
 * that multiple inclusions of debug-macro.S point at the same data.
 */
u32 brcmstb_uart_config[3] = {
	/* Debug UART initialization required */
	1,
	/* Debug UART physical address */
	0,
	/* Debug UART virtual address */
	0,
};

static void __init brcmstb_init_irq(void)
{
	gic_set_irqchip_flags(IRQCHIP_MASK_ON_SUSPEND);
	irqchip_init();
}

static const char *const brcmstb_match[] __initconst = {
	"brcm,bcm7445",
	"brcm,brcmstb",
	NULL
};

static void __init brcmstb_init_machine(void)
{
	of_platform_populate(NULL, of_default_bus_match_table, NULL, NULL);
}

static void __init brcmstb_init_early(void)
{
	add_preferred_console("ttyS", 0, "115200");
}

DT_MACHINE_START(BRCMSTB, "Broadcom STB (Flattened Device Tree)")
	.dt_compat	= brcmstb_match,
	.init_irq	= brcmstb_init_irq,
	.init_machine	= brcmstb_init_machine,
	.init_early	= brcmstb_init_early,
MACHINE_END
