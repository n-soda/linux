/*
 * arch/ppc/platforms/4xx/ebony.c
 *
 * Ebony board specific routines
 *
 * Matt Porter <mporter@kernel.crashing.org>
 * Copyright 2002-2004 MontaVista Software Inc.
 *
 * Eugene Surovegin <eugene.surovegin@zultys.com> or <ebs@ebshome.net>
 * Copyright (c) 2003, 2004 Zultys Technologies
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/config.h>
#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/reboot.h>
#include <linux/pci.h>
#include <linux/kdev_t.h>
#include <linux/types.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/ide.h>
#include <linux/initrd.h>
#include <linux/irq.h>
#include <linux/seq_file.h>
#include <linux/root_dev.h>
#include <linux/tty.h>
#include <linux/serial.h>
#include <linux/serial_core.h>

#include <asm/system.h>
#include <asm/pgtable.h>
#include <asm/page.h>
#include <asm/dma.h>
#include <asm/io.h>
#include <asm/machdep.h>
#include <asm/ocp.h>
#include <asm/pci-bridge.h>
#include <asm/time.h>
#include <asm/todc.h>
#include <asm/bootinfo.h>
#include <asm/ppc4xx_pic.h>

#include <syslib/gen550.h>

static struct ibm44x_clocks clocks __initdata;

/*
 * Ebony external IRQ triggering/polarity settings
 */
unsigned char ppc4xx_uic_ext_irq_cfg[] __initdata = {
	(IRQ_SENSE_LEVEL | IRQ_POLARITY_NEGATIVE),	/* IRQ0: PCI slot 0 */
	(IRQ_SENSE_LEVEL | IRQ_POLARITY_NEGATIVE),	/* IRQ1: PCI slot 1 */
	(IRQ_SENSE_LEVEL | IRQ_POLARITY_NEGATIVE),	/* IRQ2: PCI slot 2 */
	(IRQ_SENSE_LEVEL | IRQ_POLARITY_NEGATIVE),	/* IRQ3: PCI slot 3 */
	(IRQ_SENSE_LEVEL | IRQ_POLARITY_POSITIVE),	/* IRQ4: IRDA */
	(IRQ_SENSE_EDGE  | IRQ_POLARITY_NEGATIVE),	/* IRQ5: SMI pushbutton */
	(IRQ_SENSE_LEVEL | IRQ_POLARITY_NEGATIVE),	/* IRQ6: PHYs */
	(IRQ_SENSE_LEVEL | IRQ_POLARITY_POSITIVE),	/* IRQ7: AUX */
	(IRQ_SENSE_LEVEL | IRQ_POLARITY_NEGATIVE),	/* IRQ8: EXT */
	(IRQ_SENSE_LEVEL | IRQ_POLARITY_NEGATIVE),	/* IRQ9: EXT */
	(IRQ_SENSE_LEVEL | IRQ_POLARITY_NEGATIVE),	/* IRQ10: EXT */
	(IRQ_SENSE_LEVEL | IRQ_POLARITY_NEGATIVE),	/* IRQ11: EXT */
	(IRQ_SENSE_LEVEL | IRQ_POLARITY_POSITIVE),	/* IRQ12: EXT */
};

static void __init
ebony_calibrate_decr(void)
{
	unsigned int freq;

	/*
	 * Determine system clock speed
	 *
	 * If we are on Rev. B silicon, then use
	 * default external system clock.  If we are
	 * on Rev. C silicon then errata forces us to
	 * use the internal clock.
	 */
	switch (PVR_REV(mfspr(PVR))) {
		case PVR_REV(PVR_440GP_RB):
			freq = EBONY_440GP_RB_SYSCLK;
			break;
		case PVR_REV(PVR_440GP_RC1):
		default:
			freq = EBONY_440GP_RC_SYSCLK;
			break;
	}

	ibm44x_calibrate_decr(freq);
}

static int
ebony_show_cpuinfo(struct seq_file *m)
{
	seq_printf(m, "vendor\t\t: IBM\n");
	seq_printf(m, "machine\t\t: Ebony\n");

	return 0;
}

static inline int
ebony_map_irq(struct pci_dev *dev, unsigned char idsel, unsigned char pin)
{
	static char pci_irq_table[][4] =
	/*
	 *	PCI IDSEL/INTPIN->INTLINE
	 * 	   A   B   C   D
	 */
	{
		{ 23, 23, 23, 23 },	/* IDSEL 1 - PCI Slot 0 */
		{ 24, 24, 24, 24 },	/* IDSEL 2 - PCI Slot 1 */
		{ 25, 25, 25, 25 },	/* IDSEL 3 - PCI Slot 2 */
		{ 26, 26, 26, 26 },	/* IDSEL 4 - PCI Slot 3 */
	};

	const long min_idsel = 1, max_idsel = 4, irqs_per_slot = 4;
	return PCI_IRQ_TABLE_LOOKUP;
}

#define PCIX_WRITEL(value, offset) \
	(writel(value, pcix_reg_base + offset))

/*
 * FIXME: This is only here to "make it work".  This will move
 * to a ibm_pcix.c which will contain a generic IBM PCIX bridge
 * configuration library. -Matt
 */
static void __init
ebony_setup_pcix(void)
{
	void *pcix_reg_base;

	pcix_reg_base = ioremap64(PCIX0_REG_BASE, PCIX0_REG_SIZE);

	/* Disable all windows */
	PCIX_WRITEL(0, PCIX0_POM0SA);
	PCIX_WRITEL(0, PCIX0_POM1SA);
	PCIX_WRITEL(0, PCIX0_POM2SA);
	PCIX_WRITEL(0, PCIX0_PIM0SA);
	PCIX_WRITEL(0, PCIX0_PIM1SA);
	PCIX_WRITEL(0, PCIX0_PIM2SA);

	/* Setup 2GB PLB->PCI outbound mem window (3_8000_0000->0_8000_0000) */
	PCIX_WRITEL(0x00000003, PCIX0_POM0LAH);
	PCIX_WRITEL(0x80000000, PCIX0_POM0LAL);
	PCIX_WRITEL(0x00000000, PCIX0_POM0PCIAH);
	PCIX_WRITEL(0x80000000, PCIX0_POM0PCIAL);
	PCIX_WRITEL(0x80000001, PCIX0_POM0SA);

	/* Setup 2GB PCI->PLB inbound memory window at 0, enable MSIs */
	PCIX_WRITEL(0x00000000, PCIX0_PIM0LAH);
	PCIX_WRITEL(0x00000000, PCIX0_PIM0LAL);
	PCIX_WRITEL(0x80000007, PCIX0_PIM0SA);

	eieio();
}

static void __init
ebony_setup_hose(void)
{
	struct pci_controller *hose;

	/* Configure windows on the PCI-X host bridge */
	ebony_setup_pcix();

	hose = pcibios_alloc_controller();

	if (!hose)
		return;

	hose->first_busno = 0;
	hose->last_busno = 0xff;

	hose->pci_mem_offset = EBONY_PCI_MEM_OFFSET;

	pci_init_resource(&hose->io_resource,
			EBONY_PCI_LOWER_IO,
			EBONY_PCI_UPPER_IO,
			IORESOURCE_IO,
			"PCI host bridge");

	pci_init_resource(&hose->mem_resources[0],
			EBONY_PCI_LOWER_MEM,
			EBONY_PCI_UPPER_MEM,
			IORESOURCE_MEM,
			"PCI host bridge");

	hose->io_space.start = EBONY_PCI_LOWER_IO;
	hose->io_space.end = EBONY_PCI_UPPER_IO;
	hose->mem_space.start = EBONY_PCI_LOWER_MEM;
	hose->mem_space.end = EBONY_PCI_UPPER_MEM;
	isa_io_base =
		(unsigned long)ioremap64(EBONY_PCI_IO_BASE, EBONY_PCI_IO_SIZE);
	hose->io_base_virt = (void *)isa_io_base;

	setup_indirect_pci(hose,
			EBONY_PCI_CFGA_PLB32,
			EBONY_PCI_CFGD_PLB32);
	hose->set_cfg_type = 1;

	hose->last_busno = pciauto_bus_scan(hose, hose->first_busno);

	ppc_md.pci_swizzle = common_swizzle;
	ppc_md.pci_map_irq = ebony_map_irq;
}

TODC_ALLOC();

static void __init
ebony_early_serial_map(void)
{
	struct uart_port port;

	/* Setup ioremapped serial port access */
	memset(&port, 0, sizeof(port));
	port.membase = ioremap64(PPC440GP_UART0_ADDR, 8);
	port.irq = 0;
	port.uartclk = clocks.uart0;
	port.regshift = 0;
	port.iotype = SERIAL_IO_MEM;
	port.flags = ASYNC_BOOT_AUTOCONF | ASYNC_SKIP_TEST;
	port.line = 0;

	if (early_serial_setup(&port) != 0) {
		printk("Early serial init of port 0 failed\n");
	}

#if defined(CONFIG_SERIAL_TEXT_DEBUG) || defined(CONFIG_KGDB)
	/* Configure debug serial access */
	gen550_init(0, &port);
#endif

	port.membase = ioremap64(PPC440GP_UART1_ADDR, 8);
	port.irq = 1;
	port.uartclk = clocks.uart1;
	port.line = 1;

	if (early_serial_setup(&port) != 0) {
		printk("Early serial init of port 1 failed\n");
	}

#if defined(CONFIG_SERIAL_TEXT_DEBUG) || defined(CONFIG_KGDB)
	/* Configure debug serial access */
	gen550_init(1, &port);
#endif
}

static void __init
ebony_setup_arch(void)
{
	unsigned char * vpd_base;
	struct ocp_def *def;
	struct ocp_func_emac_data *emacdata;

	/* Set mac_addr for each EMAC */
	vpd_base = ioremap64(EBONY_VPD_BASE, EBONY_VPD_SIZE);
	def = ocp_get_one_device(OCP_VENDOR_IBM, OCP_FUNC_EMAC, 0);
	emacdata = def->additions;
	memcpy(emacdata->mac_addr, EBONY_NA0_ADDR(vpd_base), 6);
	def = ocp_get_one_device(OCP_VENDOR_IBM, OCP_FUNC_EMAC, 1);
	emacdata = def->additions;
	memcpy(emacdata->mac_addr, EBONY_NA1_ADDR(vpd_base), 6);
	iounmap(vpd_base);

	/*
	 * Determine various clocks.
	 * To be completely correct we should get SysClk
	 * from FPGA, because it can be changed by on-board switches
	 * --ebs
	 */
	ibm440gp_get_clocks(&clocks, 33333333, 6 * 1843200);
	ocp_sys_info.opb_bus_freq = clocks.opb;

	/* Setup TODC access */
	TODC_INIT(TODC_TYPE_DS1743,
			0,
			0,
			ioremap64(EBONY_RTC_ADDR, EBONY_RTC_SIZE),
			8);

	/* init to some ~sane value until calibrate_delay() runs */
        loops_per_jiffy = 50000000/HZ;

	/* Setup PCI host bridge */
	ebony_setup_hose();

#ifdef CONFIG_BLK_DEV_INITRD
	if (initrd_start)
		ROOT_DEV = Root_RAM0;
	else
#endif
#ifdef CONFIG_ROOT_NFS
		ROOT_DEV = Root_NFS;
#else
		ROOT_DEV = Root_HDA1;
#endif

	ebony_early_serial_map();

	/* Identify the system */
	printk("IBM Ebony port (MontaVista Software, Inc. (source@mvista.com))\n");
}

void __init platform_init(unsigned long r3, unsigned long r4,
		unsigned long r5, unsigned long r6, unsigned long r7)
{
	parse_bootinfo((struct bi_record *) (r3 + KERNELBASE));

	ibm44x_platform_init();

	ppc_md.setup_arch = ebony_setup_arch;
	ppc_md.show_cpuinfo = ebony_show_cpuinfo;
	ppc_md.get_irq = NULL;		/* Set in ppc4xx_pic_init() */

	ppc_md.calibrate_decr = ebony_calibrate_decr;
	ppc_md.time_init = todc_time_init;
	ppc_md.set_rtc_time = todc_set_rtc_time;
	ppc_md.get_rtc_time = todc_get_rtc_time;

	ppc_md.nvram_read_val = todc_direct_read_val;
	ppc_md.nvram_write_val = todc_direct_write_val;
#ifdef CONFIG_KGDB
	ppc_md.early_serial_map = ebony_early_serial_map;
#endif
}

