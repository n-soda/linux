/*
 *  linux/include/asm-arm/arch-pxa/hardware.h
 *
 *  Author:	Nicolas Pitre
 *  Created:	Jun 15, 2001
 *  Copyright:	MontaVista Software Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __ASM_ARCH_HARDWARE_H
#define __ASM_ARCH_HARDWARE_H

/*
 * We requires absolute addresses.
 */
#define PCIO_BASE		0

/*
 * Workarounds for at least 2 errata so far require this.
 * The mapping is set in mach-pxa/generic.c.
 */
#define UNCACHED_PHYS_0		0xff000000
#define UNCACHED_ADDR		UNCACHED_PHYS_0

/*
 * Intel PXA2xx internal register mapping:
 *
 * 0x40000000 - 0x41ffffff <--> 0xf2000000 - 0xf3ffffff
 * 0x44000000 - 0x45ffffff <--> 0xf4000000 - 0xf5ffffff
 * 0x48000000 - 0x49ffffff <--> 0xf6000000 - 0xf7ffffff
 * 0x4c000000 - 0x4dffffff <--> 0xf8000000 - 0xf9ffffff
 * 0x50000000 - 0x51ffffff <--> 0xfa000000 - 0xfbffffff
 * 0x54000000 - 0x55ffffff <--> 0xfc000000 - 0xfdffffff
 * 0x58000000 - 0x59ffffff <--> 0xfe000000 - 0xffffffff
 *
 * Note that not all PXA2xx chips implement all those addresses, and the
 * kernel only maps the minimum needed range of this mapping.
 */
#define io_p2v(x) (0xf2000000 + ((x) & 0x01ffffff) + (((x) & 0x1c000000) >> 1))
#define io_v2p(x) (0x40000000 + ((x) & 0x01ffffff) + (((x) & 0x0e000000) << 1))

#ifndef __ASSEMBLY__

#if 0
# define __REG(x)	(*((volatile u32 *)io_p2v(x)))
#else
/*
 * This __REG() version gives the same results as the one above,  except
 * that we are fooling gcc somehow so it generates far better and smaller
 * assembly code for access to contigous registers.  It's a shame that gcc
 * doesn't guess this by itself.
 */
#include <asm/types.h>
typedef struct { volatile u32 offset[4096]; } __regbase;
# define __REGP(x)	((__regbase *)((x)&~4095))->offset[((x)&4095)>>2]
# define __REG(x)	__REGP(io_p2v(x))
#endif

/* Let's kick gcc's ass again... */
# define __REG2(x,y)	\
	( __builtin_constant_p(y) ? (__REG((x) + (y))) \
				  : (*(volatile u32 *)((u32)&__REG(x) + (y))) )

# define __PREG(x)	(io_v2p((u32)&(x)))

#else

# define __REG(x)	io_p2v(x)
# define __PREG(x)	io_v2p(x)

#endif

#include "pxa-regs.h"

#ifndef __ASSEMBLY__

/*
 * Handy routine to set GPIO alternate functions
 */
extern void pxa_gpio_mode( int gpio_mode );

/*
 * return current lclk frequency in units of 10kHz
 */
extern unsigned int get_lclk_frequency_10khz(void);

#endif


/*
 * Implementation specifics
 */

#include "lubbock.h"
#include "mainstone.h"
#include "idp.h"

#endif  /* _ASM_ARCH_HARDWARE_H */
