/* $Id: teles0.c,v 2.13.6.2 2001/09/23 22:24:52 kai Exp $
 *
 * low level stuff for Teles Memory IO isdn cards
 *
 * Author       Karsten Keil
 *              based on the teles driver from Jan den Ouden
 * Copyright    by Karsten Keil      <keil@isdn4linux.de>
 * 
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 *
 * Thanks to    Jan den Ouden
 *              Fritz Elfert
 *              Beat Doebeli
 *
 */

#include <linux/init.h>
#include "hisax.h"
#include "isdnl1.h"
#include "isac.h"
#include "hscx.h"

extern const char *CardType[];

const char *teles0_revision = "$Revision: 2.13.6.2 $";

#define TELES_IOMEM_SIZE	0x400
#define byteout(addr,val) outb(val,addr)
#define bytein(addr) inb(addr)

static u8
isac_read(struct IsdnCardState *cs, u8 off)
{
	return readb(cs->hw.teles0.membase + 
		     ((off & 1) ? 0x2ff : 0x100) + off);
}

static void
isac_write(struct IsdnCardState *cs, u8 off, u8 data)
{
	writeb(data, cs->hw.teles0.membase + 
	       ((off & 1) ? 0x2ff : 0x100) + off); mb();
}


static void
isac_read_fifo(struct IsdnCardState *cs, u8 * data, int size)
{
	int i;
	unsigned long ad = cs->hw.teles0.membase + 0x100;
	for (i = 0; i < size; i++)
		data[i] = readb(ad);
}

static void
isac_write_fifo(struct IsdnCardState *cs, u8 * data, int size)
{
	int i;
	unsigned long ad = cs->hw.teles0.membase + 0x100;
	for (i = 0; i < size; i++) {
		writeb(data[i], ad); mb();
	}
}

static struct dc_hw_ops isac_ops = {
	.read_reg   = isac_read,
	.write_reg  = isac_write,
	.read_fifo  = isac_read_fifo,
	.write_fifo = isac_write_fifo,
};

static u8
hscx_read(struct IsdnCardState *cs, int hscx, u8 off)
{
	return readb(cs->hw.teles0.membase + (hscx ? 0x1c0 : 0x180) +
		     ((off & 1) ? 0x1ff : 0) + off);
}

static void
hscx_write(struct IsdnCardState *cs, int hscx, u8 off, u8 data)
{
	writeb(data, cs->hw.teles0.membase + (hscx ? 0x1c0 : 0x180) +
	       ((off & 1) ? 0x1ff : 0) + off); mb();
}

static void
hscx_read_fifo(struct IsdnCardState *cs, int hscx, u8 *data, int size)
{
	int i;
	unsigned long ad = cs->hw.teles0.membase + (hscx ? 0x1c0 : 0x180);
	for (i = 0; i < size; i++)
		data[i] = readb(ad);
}

static void
hscx_write_fifo(struct IsdnCardState *cs, int hscx, u8 *data, int size)
{
	int i;
	unsigned long ad = cs->hw.teles0.membase + (hscx ? 0x1c0 : 0x180);
	for (i = 0; i < size; i++) {
		writeb(data[i], ad);
	}
}

static struct bc_hw_ops hscx_ops = {
	.read_reg   = hscx_read,
	.write_reg  = hscx_write,
	.read_fifo  = hscx_read_fifo,
	.write_fifo = hscx_write_fifo,
};

static void
teles0_interrupt(int intno, void *dev_id, struct pt_regs *regs)
{
	struct IsdnCardState *cs = dev_id;
	u8 val;
	int count = 0;

	spin_lock(&cs->lock);
	val = hscx_read(cs, 1, HSCX_ISTA);
      Start_HSCX:
	if (val)
		hscx_int_main(cs, val);
	val = isac_read(cs, ISAC_ISTA);
      Start_ISAC:
	if (val)
		isac_interrupt(cs, val);
	count++;
	val = hscx_read(cs, 1, HSCX_ISTA);
	if (val && count < 5) {
		if (cs->debug & L1_DEB_HSCX)
			debugl1(cs, "HSCX IntStat after IntRoutine");
		goto Start_HSCX;
	}
	val = isac_read(cs, ISAC_ISTA);
	if (val && count < 5) {
		if (cs->debug & L1_DEB_ISAC)
			debugl1(cs, "ISAC IntStat after IntRoutine");
		goto Start_ISAC;
	}
	hscx_write(cs, 0, HSCX_MASK, 0xFF);
	hscx_write(cs, 1, HSCX_MASK, 0xFF);
	isac_write(cs, ISAC_MASK, 0xFF);
	isac_write(cs, ISAC_MASK, 0x0);
	hscx_write(cs, 0, HSCX_MASK, 0x0);
	hscx_write(cs, 1, HSCX_MASK, 0x0);
	spin_unlock(&cs->lock);
}

void
release_io_teles0(struct IsdnCardState *cs)
{
	if (cs->hw.teles0.cfg_reg)
		release_region(cs->hw.teles0.cfg_reg, 8);
	iounmap((unsigned char *)cs->hw.teles0.membase);
	release_mem_region(cs->hw.teles0.phymem, TELES_IOMEM_SIZE);
}

static int
reset_teles0(struct IsdnCardState *cs)
{
	u8 cfval;

	if (cs->hw.teles0.cfg_reg) {
		switch (cs->irq) {
			case 2:
			case 9:
				cfval = 0x00;
				break;
			case 3:
				cfval = 0x02;
				break;
			case 4:
				cfval = 0x04;
				break;
			case 5:
				cfval = 0x06;
				break;
			case 10:
				cfval = 0x08;
				break;
			case 11:
				cfval = 0x0A;
				break;
			case 12:
				cfval = 0x0C;
				break;
			case 15:
				cfval = 0x0E;
				break;
			default:
				return(1);
		}
		cfval |= ((cs->hw.teles0.phymem >> 9) & 0xF0);
		byteout(cs->hw.teles0.cfg_reg + 4, cfval);
		HZDELAY(HZ / 10 + 1);
		byteout(cs->hw.teles0.cfg_reg + 4, cfval | 1);
		HZDELAY(HZ / 10 + 1);
	}
	writeb(0, cs->hw.teles0.membase + 0x80); mb();
	HZDELAY(HZ / 5 + 1);
	writeb(1, cs->hw.teles0.membase + 0x80); mb();
	HZDELAY(HZ / 5 + 1);
	return(0);
}

static int
Teles_card_msg(struct IsdnCardState *cs, int mt, void *arg)
{
	switch (mt) {
		case CARD_RESET:
			reset_teles0(cs);
			return(0);
		case CARD_RELEASE:
			release_io_teles0(cs);
			return(0);
		case CARD_TEST:
			return(0);
	}
	return(0);
}

static struct card_ops teles0_ops = {
	.init = inithscxisac,
};

int __init
setup_teles0(struct IsdnCard *card)
{
	u8 val;
	struct IsdnCardState *cs = card->cs;
	char tmp[64];

	strcpy(tmp, teles0_revision);
	printk(KERN_INFO "HiSax: Teles 8.0/16.0 driver Rev. %s\n", HiSax_getrev(tmp));
	if ((cs->typ != ISDN_CTYPE_16_0) && (cs->typ != ISDN_CTYPE_8_0))
		return (0);

	if (cs->typ == ISDN_CTYPE_16_0)
		cs->hw.teles0.cfg_reg = card->para[2];
	else			/* 8.0 */
		cs->hw.teles0.cfg_reg = 0;

	if (card->para[1] < 0x10000) {
		card->para[1] <<= 4;
		printk(KERN_INFO
		   "Teles0: membase configured DOSish, assuming 0x%lx\n",
		       (unsigned long) card->para[1]);
	}
	cs->irq = card->para[0];
	if (cs->hw.teles0.cfg_reg) {
		if (!request_region(cs->hw.teles0.cfg_reg, 8, "teles cfg")) {
			printk(KERN_WARNING
			  "HiSax: %s config port %x-%x already in use\n",
			       CardType[card->typ],
			       cs->hw.teles0.cfg_reg,
			       cs->hw.teles0.cfg_reg + 8);
			return (0);
		}
	}
	if (cs->hw.teles0.cfg_reg) {
		if ((val = bytein(cs->hw.teles0.cfg_reg + 0)) != 0x51) {
			printk(KERN_WARNING "Teles0: 16.0 Byte at %x is %x\n",
			       cs->hw.teles0.cfg_reg + 0, val);
			release_region(cs->hw.teles0.cfg_reg, 8);
			return (0);
		}
		if ((val = bytein(cs->hw.teles0.cfg_reg + 1)) != 0x93) {
			printk(KERN_WARNING "Teles0: 16.0 Byte at %x is %x\n",
			       cs->hw.teles0.cfg_reg + 1, val);
			release_region(cs->hw.teles0.cfg_reg, 8);
			return (0);
		}
		val = bytein(cs->hw.teles0.cfg_reg + 2);	/* 0x1e=without AB
								   * 0x1f=with AB
								   * 0x1c 16.3 ???
								 */
		if (val != 0x1e && val != 0x1f) {
			printk(KERN_WARNING "Teles0: 16.0 Byte at %x is %x\n",
			       cs->hw.teles0.cfg_reg + 2, val);
			release_region(cs->hw.teles0.cfg_reg, 8);
			return (0);
		}
	}
	/* 16.0 and 8.0 designed for IOM1 */
	test_and_set_bit(HW_IOM1, &cs->HW_Flags);
	cs->hw.teles0.phymem = card->para[1];
	if (check_mem_region(cs->hw.teles0.phymem, TELES_IOMEM_SIZE)) {
		printk(KERN_WARNING
			"HiSax: %s memory region %lx-%lx already in use\n",
			CardType[card->typ],
			cs->hw.teles0.phymem,
			cs->hw.teles0.phymem + TELES_IOMEM_SIZE);
		if (cs->hw.teles0.cfg_reg)
			release_region(cs->hw.teles0.cfg_reg, 8);
		return (0);
	} else {
		request_mem_region(cs->hw.teles0.phymem, TELES_IOMEM_SIZE,
			"teles iomem");
	}
	cs->hw.teles0.membase =
		(unsigned long) ioremap(cs->hw.teles0.phymem, TELES_IOMEM_SIZE);
	printk(KERN_INFO
	       "HiSax: %s config irq:%d mem:0x%lX cfg:0x%X\n",
	       CardType[cs->typ], cs->irq,
	       cs->hw.teles0.membase, cs->hw.teles0.cfg_reg);
	if (reset_teles0(cs)) {
		printk(KERN_WARNING "Teles0: wrong IRQ\n");
		release_io_teles0(cs);
		return (0);
	}
	cs->dc_hw_ops = &isac_ops;
	cs->bc_hw_ops = &hscx_ops;
	cs->cardmsg = &Teles_card_msg;
	cs->irq_func = &teles0_interrupt;
	cs->card_ops = &teles0_ops;
	ISACVersion(cs, "Teles0:");
	if (HscxVersion(cs, "Teles0:")) {
		printk(KERN_WARNING
		 "Teles0: wrong HSCX versions check IO/MEM addresses\n");
		release_io_teles0(cs);
		return (0);
	}
	return (1);
}
