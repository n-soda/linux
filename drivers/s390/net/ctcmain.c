/*
 * $Id: ctcmain.c,v 1.36 2003/02/18 09:15:14 mschwide Exp $
 *
 * CTC / ESCON network driver
 *
 * Copyright (C) 2001 IBM Deutschland Entwicklung GmbH, IBM Corporation
 * Author(s): Fritz Elfert (elfert@de.ibm.com, felfert@millenux.com)
 * Fixes by : Jochen R�hrig (roehrig@de.ibm.com)
 *            Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 * Driver Model stuff by : Cornelia Huck <cohuck@de.ibm.com>
 *
 * Documentation used:
 *  - Principles of Operation (IBM doc#: SA22-7201-06)
 *  - Common IO/-Device Commands and Self Description (IBM doc#: SA22-7204-02)
 *  - Common IO/-Device Commands and Self Description (IBM doc#: SN22-5535)
 *  - ESCON Channel-to-Channel Adapter (IBM doc#: SA22-7203-00)
 *  - ESCON I/O Interface (IBM doc#: SA22-7202-029
 *
 * and the source of the original CTC driver by:
 *  Dieter Wellerdiek (wel@de.ibm.com)
 *  Martin Schwidefsky (schwidefsky@de.ibm.com)
 *  Denis Joseph Barrow (djbarrow@de.ibm.com,barrow_dj@yahoo.com)
 *  Jochen R�hrig (roehrig@de.ibm.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * RELEASE-TAG: CTC/ESCON network driver $Revision: 1.36 $
 *
 */

#undef DEBUG

#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/sched.h>

#include <linux/signal.h>
#include <linux/string.h>

#include <linux/ip.h>
#include <linux/if_arp.h>
#include <linux/tcp.h>
#include <linux/skbuff.h>
#include <linux/ctype.h>
#include <net/dst.h>

#include <asm/io.h>
#include <asm/ccwdev.h>
#include <asm/ccwgroup.h>
#include <asm/bitops.h>
#include <asm/uaccess.h>

#include <asm/idals.h>

#include "ctctty.h"
#include "fsm.h"
#include "cu3088.h"

MODULE_AUTHOR("(C) 2000 IBM Corp. by Fritz Elfert (felfert@millenux.com)");
MODULE_DESCRIPTION("Linux for S/390 CTC/Escon Driver");
MODULE_LICENSE("GPL");

/**
 * CCW commands, used in this driver.
 */
#define CCW_CMD_WRITE		0x01
#define CCW_CMD_READ		0x02
#define CCW_CMD_SET_EXTENDED	0xc3
#define CCW_CMD_PREPARE		0xe3

#define CTC_PROTO_S390          0
#define CTC_PROTO_LINUX         1
#define CTC_PROTO_LINUX_TTY     2
#define CTC_PROTO_OS390         3
#define CTC_PROTO_MAX           3

#define CTC_BUFSIZE_LIMIT       65535
#define CTC_BUFSIZE_DEFAULT     32768

#define CTC_TIMEOUT_5SEC        5000

#define CTC_INITIAL_BLOCKLEN    2

#define READ			0
#define WRITE			1

#define CTC_ID_SIZE             DEVICE_ID_SIZE+3


struct ctc_profile {
	unsigned long maxmulti;
	unsigned long maxcqueue;
	unsigned long doios_single;
	unsigned long doios_multi;
	unsigned long txlen;
	unsigned long tx_time;
	struct timespec send_stamp;
};

/**
 * Definition of one channel
 */
struct channel {

	/**
	 * Pointer to next channel in list.
	 */
	struct channel *next;
	char id[CTC_ID_SIZE];
	struct ccw_device *cdev;

	/**
	 * Type of this channel.
	 * CTC/A or Escon for valid channels.
	 */
	enum channel_types type;

	/**
	 * Misc. flags. See CHANNEL_FLAGS_... below
	 */
	__u32 flags;

	/**
	 * The protocol of this channel
	 */
	__u16 protocol;

	/**
	 * I/O and irq related stuff
	 */
	struct ccw1 *ccw;
	struct irb *irb;

	/**
	 * RX/TX buffer size
	 */
	int max_bufsize;

	/**
	 * Transmit/Receive buffer.
	 */
	struct sk_buff *trans_skb;

	/**
	 * Universal I/O queue.
	 */
	struct sk_buff_head io_queue;

	/**
	 * TX queue for collecting skb's during busy.
	 */
	struct sk_buff_head collect_queue;

	/**
	 * Amount of data in collect_queue.
	 */
	int collect_len;

	/**
	 * spinlock for collect_queue and collect_len
	 */
	spinlock_t collect_lock;

	/**
	 * Timer for detecting unresposive
	 * I/O operations.
	 */
	fsm_timer timer;

	/**
	 * Retry counter for misc. operations.
	 */
	int retry;

	/**
	 * The finite state machine of this channel
	 */
	fsm_instance *fsm;

	/**
	 * The corresponding net_device this channel
	 * belongs to.
	 */
	struct net_device *netdev;

	struct ctc_profile prof;

	unsigned char *trans_skb_data;
};

#define CHANNEL_FLAGS_READ            0
#define CHANNEL_FLAGS_WRITE           1
#define CHANNEL_FLAGS_INUSE           2
#define CHANNEL_FLAGS_BUFSIZE_CHANGED 4
#define CHANNEL_FLAGS_RWMASK 1
#define CHANNEL_DIRECTION(f) (f & CHANNEL_FLAGS_RWMASK)

/**
 * Linked list of all detected channels.
 */
static struct channel *channels = NULL;

struct ctc_priv {
	struct net_device_stats stats;
	unsigned long tbusy;
	/**
	 * The finite state machine of this interface.
	 */
	fsm_instance *fsm;
	/**
	 * The protocol of this device
	 */
	__u16 protocol;
 	/**
 	 * Timer for restarting after I/O Errors
 	 */
 	fsm_timer               restart_timer;

	struct channel *channel[2];
};

/**
 * Definition of our link level header.
 */
struct ll_header {
	__u16 length;
	__u16 type;
	__u16 unused;
};
#define LL_HEADER_LENGTH (sizeof(struct ll_header))

/**
 * Compatibility macros for busy handling
 * of network devices.
 */
static __inline__ void
ctc_clear_busy(struct net_device * dev)
{
	clear_bit(0, &(((struct ctc_priv *) dev->priv)->tbusy));
	netif_wake_queue(dev);
}

static __inline__ int
ctc_test_and_set_busy(struct net_device * dev)
{
	netif_stop_queue(dev);
	return test_and_set_bit(0, &((struct ctc_priv *) dev->priv)->tbusy);
}

/**
 * Print Banner.
 */
static void
print_banner(void)
{
	static int printed = 0;
	char vbuf[] = "$Revision: 1.36 $";
	char *version = vbuf;

	if (printed)
		return;
	if ((version = strchr(version, ':'))) {
		char *p = strchr(version + 1, '$');
		if (p)
			*p = '\0';
	} else
		version = " ??? ";
	printk(KERN_INFO "CTC driver Version%s"
#ifdef DEBUG
	       " (DEBUG-VERSION, " __DATE__ __TIME__ ")"
#endif
	       " initialized\n", version);
	printed = 1;
}

/**
 * Return type of a detected device.
 */
static enum channel_types
get_channel_type(struct ccw_device_id *id)
{
	enum channel_types type = (enum channel_types) id->driver_info;

	if (type == channel_type_ficon)
		type = channel_type_escon;

	return type;
}

/**
 * States of the interface statemachine.
 */
enum dev_states {
	DEV_STATE_STOPPED,
	DEV_STATE_STARTWAIT_RXTX,
	DEV_STATE_STARTWAIT_RX,
	DEV_STATE_STARTWAIT_TX,
	DEV_STATE_STOPWAIT_RXTX,
	DEV_STATE_STOPWAIT_RX,
	DEV_STATE_STOPWAIT_TX,
	DEV_STATE_RUNNING,
	/**
	 * MUST be always the last element!!
	 */
	NR_DEV_STATES
};

static const char *dev_state_names[] = {
	"Stopped",
	"StartWait RXTX",
	"StartWait RX",
	"StartWait TX",
	"StopWait RXTX",
	"StopWait RX",
	"StopWait TX",
	"Running",
};

/**
 * Events of the interface statemachine.
 */
enum dev_events {
	DEV_EVENT_START,
	DEV_EVENT_STOP,
	DEV_EVENT_RXUP,
	DEV_EVENT_TXUP,
	DEV_EVENT_RXDOWN,
	DEV_EVENT_TXDOWN,
	DEV_EVENT_RESTART,
	/**
	 * MUST be always the last element!!
	 */
	NR_DEV_EVENTS
};

static const char *dev_event_names[] = {
	"Start",
	"Stop",
	"RX up",
	"TX up",
	"RX down",
	"TX down",
	"Restart",
};

/**
 * Events of the channel statemachine
 */
enum ch_events {
	/**
	 * Events, representing return code of
	 * I/O operations (ccw_device_start, ccw_device_halt et al.)
	 */
	CH_EVENT_IO_SUCCESS,
	CH_EVENT_IO_EBUSY,
	CH_EVENT_IO_ENODEV,
	CH_EVENT_IO_EIO,
	CH_EVENT_IO_UNKNOWN,

	CH_EVENT_ATTNBUSY,
	CH_EVENT_ATTN,
	CH_EVENT_BUSY,

	/**
	 * Events, representing unit-check
	 */
	CH_EVENT_UC_RCRESET,
	CH_EVENT_UC_RSRESET,
	CH_EVENT_UC_TXTIMEOUT,
	CH_EVENT_UC_TXPARITY,
	CH_EVENT_UC_HWFAIL,
	CH_EVENT_UC_RXPARITY,
	CH_EVENT_UC_ZERO,
	CH_EVENT_UC_UNKNOWN,

	/**
	 * Events, representing subchannel-check
	 */
	CH_EVENT_SC_UNKNOWN,

	/**
	 * Events, representing machine checks
	 */
	CH_EVENT_MC_FAIL,
	CH_EVENT_MC_GOOD,

	/**
	 * Event, representing normal IRQ
	 */
	CH_EVENT_IRQ,
	CH_EVENT_FINSTAT,

	/**
	 * Event, representing timer expiry.
	 */
	CH_EVENT_TIMER,

	/**
	 * Events, representing commands from upper levels.
	 */
	CH_EVENT_START,
	CH_EVENT_STOP,

	/**
	 * MUST be always the last element!!
	 */
	NR_CH_EVENTS,
};

static const char *ch_event_names[] = {
	"do_IO success",
	"do_IO busy",
	"do_IO enodev",
	"do_IO ioerr",
	"do_IO unknown",

	"Status ATTN & BUSY",
	"Status ATTN",
	"Status BUSY",

	"Unit check remote reset",
	"Unit check remote system reset",
	"Unit check TX timeout",
	"Unit check TX parity",
	"Unit check Hardware failure",
	"Unit check RX parity",
	"Unit check ZERO",
	"Unit check Unknown",

	"SubChannel check Unknown",

	"Machine check failure",
	"Machine check operational",

	"IRQ normal",
	"IRQ final",

	"Timer",

	"Start",
	"Stop",
};

/**
 * States of the channel statemachine.
 */
enum ch_states {
	/**
	 * Channel not assigned to any device,
	 * initial state, direction invalid
	 */
	CH_STATE_IDLE,

	/**
	 * Channel assigned but not operating
	 */
	CH_STATE_STOPPED,
	CH_STATE_STARTWAIT,
	CH_STATE_STARTRETRY,
	CH_STATE_SETUPWAIT,
	CH_STATE_RXINIT,
	CH_STATE_TXINIT,
	CH_STATE_RX,
	CH_STATE_TX,
	CH_STATE_RXIDLE,
	CH_STATE_TXIDLE,
	CH_STATE_RXERR,
	CH_STATE_TXERR,
	CH_STATE_TERM,
	CH_STATE_DTERM,
	CH_STATE_NOTOP,

	/**
	 * MUST be always the last element!!
	 */
	NR_CH_STATES,
};

static const char *ch_state_names[] = {
	"Idle",
	"Stopped",
	"StartWait",
	"StartRetry",
	"SetupWait",
	"RX init",
	"TX init",
	"RX",
	"TX",
	"RX idle",
	"TX idle",
	"RX error",
	"TX error",
	"Terminating",
	"Restarting",
	"Not operational",
};

#ifdef DEBUG
/**
 * Dump header and first 16 bytes of an sk_buff for debugging purposes.
 *
 * @param skb    The sk_buff to dump.
 * @param offset Offset relative to skb-data, where to start the dump.
 */
static void
ctc_dump_skb(struct sk_buff *skb, int offset)
{
	unsigned char *p = skb->data;
	__u16 bl;
	struct ll_header *header;
	int i;

	p += offset;
	bl = *((__u16 *) p);
	p += 2;
	header = (struct ll_header *) p;
	p -= 2;

	printk(KERN_DEBUG "dump:\n");
	printk(KERN_DEBUG "blocklen=%d %04x\n", bl, bl);

	printk(KERN_DEBUG "h->length=%d %04x\n", header->length,
	       header->length);
	printk(KERN_DEBUG "h->type=%04x\n", header->type);
	printk(KERN_DEBUG "h->unused=%04x\n", header->unused);
	if (bl > 16)
		bl = 16;
	printk(KERN_DEBUG "data: ");
	for (i = 0; i < bl; i++)
		printk("%02x%s", *p++, (i % 16) ? " " : "\n<7>");
	printk("\n");
}
#else
static inline void
ctc_dump_skb(struct sk_buff *skb, int offset)
{
}
#endif

/**
 * Unpack a just received skb and hand it over to
 * upper layers.
 *
 * @param ch The channel where this skb has been received.
 * @param pskb The received skb.
 */
static __inline__ void
ctc_unpack_skb(struct channel *ch, struct sk_buff *pskb)
{
	struct net_device *dev = ch->netdev;
	struct ctc_priv *privptr = (struct ctc_priv *) dev->priv;

	__u16 len = *((__u16 *) pskb->data);
	skb_put(pskb, 2 + LL_HEADER_LENGTH);
	skb_pull(pskb, 2);
	pskb->dev = dev;
	pskb->ip_summed = CHECKSUM_UNNECESSARY;
	while (len > 0) {
		struct sk_buff *skb;
		struct ll_header *header = (struct ll_header *) pskb->data;

		skb_pull(pskb, LL_HEADER_LENGTH);
		if ((ch->protocol == CTC_PROTO_S390) &&
		    (header->type != ETH_P_IP)) {
			/**
			 * Check packet type only if we stick strictly
			 * to S/390's protocol of OS390. This only
			 * supports IP. Otherwise allow any packet
			 * type.
			 */
			printk(KERN_WARNING
			       "%s Illegal packet type 0x%04x "
			       "received, dropping\n", dev->name, header->type);
			ctc_dump_skb(pskb, -6);
			privptr->stats.rx_dropped++;
			privptr->stats.rx_frame_errors++;
			return;
		}
		pskb->protocol = ntohs(header->type);
		header->length -= LL_HEADER_LENGTH;
		if ((header->length == 0) ||
		    (header->length > skb_tailroom(pskb)) ||
		    (header->length > len)) {
			printk(KERN_WARNING
			       "%s Illegal packet size %d "
			       "received (MTU=%d blocklen=%d), "
			       "dropping\n", dev->name, header->length,
			       dev->mtu, len);
			ctc_dump_skb(pskb, -6);
			privptr->stats.rx_dropped++;
			privptr->stats.rx_length_errors++;
			return;
		}
		if (header->length > skb_tailroom(pskb)) {
			printk(KERN_WARNING
			       "%s Illegal packet size %d "
			       "(beyond the end of received data), "
			       "dropping\n", dev->name, header->length);
			ctc_dump_skb(pskb, -6);
			privptr->stats.rx_dropped++;
			privptr->stats.rx_length_errors++;
			return;
		}
		skb_put(pskb, header->length);
		pskb->mac.raw = pskb->data;
		len -= (LL_HEADER_LENGTH + header->length);
		skb = dev_alloc_skb(pskb->len);
		if (!skb) {
			printk(KERN_WARNING
			       "%s Out of memory in ctc_unpack_skb\n",
			       dev->name);
			privptr->stats.rx_dropped++;
			return;
		}
		memcpy(skb_put(skb, pskb->len), pskb->data, pskb->len);
		skb->mac.raw = skb->data;
		skb->dev = pskb->dev;
		skb->protocol = pskb->protocol;
		pskb->ip_summed = CHECKSUM_UNNECESSARY;
		if (ch->protocol == CTC_PROTO_LINUX_TTY)
			ctc_tty_netif_rx(skb);
		else
			netif_rx(skb);
		dev->last_rx = jiffies;
		privptr->stats.rx_packets++;
		privptr->stats.rx_bytes += skb->len;
		if (len > 0) {
			skb_pull(pskb, header->length);
			skb_put(pskb, LL_HEADER_LENGTH);
		}
	}
}

/**
 * Check return code of a preceeding do_IO, halt_IO etc...
 *
 * @param ch          The channel, the error belongs to.
 * @param return_code The error code to inspect.
 */
static void inline
ccw_check_return_code(struct channel *ch, int return_code)
{
	switch (return_code) {
	case 0:
		fsm_event(ch->fsm, CH_EVENT_IO_SUCCESS, ch);
		break;
	case -EBUSY:
		printk(KERN_INFO "%s: Busy !\n", ch->id);
		fsm_event(ch->fsm, CH_EVENT_IO_EBUSY, ch);
		break;
	case -ENODEV:
		printk(KERN_EMERG
		       "%s: Invalid device called for IO\n", ch->id);
		fsm_event(ch->fsm, CH_EVENT_IO_ENODEV, ch);
		break;
	case -EIO:
		printk(KERN_EMERG "%s: Status pending... \n", ch->id);
		fsm_event(ch->fsm, CH_EVENT_IO_EIO, ch);
		break;
	default:
		printk(KERN_EMERG
		       "%s: Unknown error in do_IO %04x\n",
		       ch->id, return_code);
		fsm_event(ch->fsm, CH_EVENT_IO_UNKNOWN, ch);
	}
}

/**
 * Check sense of a unit check.
 *
 * @param ch    The channel, the sense code belongs to.
 * @param sense The sense code to inspect.
 */
static void inline
ccw_unit_check(struct channel *ch, unsigned char sense)
{
	if (sense & SNS0_INTERVENTION_REQ) {
		if (sense & 0x01) {
			if (ch->protocol != CTC_PROTO_LINUX_TTY)
				printk(KERN_DEBUG
				       "%s: Interface disc. or Sel. reset "
				       "(remote)\n", ch->id);
			fsm_event(ch->fsm, CH_EVENT_UC_RCRESET, ch);
		} else {
			printk(KERN_DEBUG "%s: System reset (remote)\n",
			       ch->id);
			fsm_event(ch->fsm, CH_EVENT_UC_RSRESET, ch);
		}
	} else if (sense & SNS0_EQUIPMENT_CHECK) {
		if (sense & SNS0_BUS_OUT_CHECK) {
			printk(KERN_WARNING
			       "%s: Hardware malfunction (remote)\n",
			       ch->id);
			fsm_event(ch->fsm, CH_EVENT_UC_HWFAIL, ch);
		} else {
			printk(KERN_WARNING
			       "%s: Read-data parity error (remote)\n",
			       ch->id);
			fsm_event(ch->fsm, CH_EVENT_UC_RXPARITY, ch);
		}
	} else if (sense & SNS0_BUS_OUT_CHECK) {
		if (sense & 0x04) {
			printk(KERN_WARNING
			       "%s: Data-streaming timeout)\n", ch->id);
			fsm_event(ch->fsm, CH_EVENT_UC_TXTIMEOUT, ch);
		} else {
			printk(KERN_WARNING
			       "%s: Data-transfer parity error\n",
			       ch->id);
			fsm_event(ch->fsm, CH_EVENT_UC_TXPARITY, ch);
		}
	} else if (sense & SNS0_CMD_REJECT) {
		printk(KERN_WARNING "%s: Command reject\n", ch->id);
	} else if (sense == 0) {
		printk(KERN_DEBUG "%s: Unit check ZERO\n", ch->id);
		fsm_event(ch->fsm, CH_EVENT_UC_ZERO, ch);
	} else {
		printk(KERN_WARNING
		       "%s: Unit Check with sense code: %02x\n",
		       ch->id, sense);
		fsm_event(ch->fsm, CH_EVENT_UC_UNKNOWN, ch);
	}
}

static void
ctc_purge_skb_queue(struct sk_buff_head *q)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(q))) {
		atomic_dec(&skb->users);
		dev_kfree_skb_irq(skb);
	}
}

static __inline__ int
ctc_checkalloc_buffer(struct channel *ch, int warn)
{
	if ((ch->trans_skb == NULL) ||
	    (ch->flags & CHANNEL_FLAGS_BUFSIZE_CHANGED)) {
		if (ch->trans_skb != NULL)
			dev_kfree_skb(ch->trans_skb);
		clear_normalized_cda(&ch->ccw[1]);
		ch->trans_skb = __dev_alloc_skb(ch->max_bufsize,
						GFP_ATOMIC | GFP_DMA);
		if (ch->trans_skb == NULL) {
			if (warn)
				printk(KERN_WARNING
				       "%s: Couldn't alloc %s trans_skb\n",
				       ch->id,
				       (CHANNEL_DIRECTION(ch->flags) == READ) ?
				       "RX" : "TX");
			return -ENOMEM;
		}
		ch->ccw[1].count = ch->max_bufsize;
		if (set_normalized_cda(&ch->ccw[1], ch->trans_skb->data)) {
			dev_kfree_skb(ch->trans_skb);
			ch->trans_skb = NULL;
			if (warn)
				printk(KERN_WARNING
				       "%s: set_normalized_cda for %s "
				       "trans_skb failed, dropping packets\n",
				       ch->id,
				       (CHANNEL_DIRECTION(ch->flags) == READ) ?
				       "RX" : "TX");
			return -ENOMEM;
		}
		ch->ccw[1].count = 0;
		ch->trans_skb_data = ch->trans_skb->data;
		ch->flags &= ~CHANNEL_FLAGS_BUFSIZE_CHANGED;
	}
	return 0;
}

/**
 * Dummy NOP action for statemachines
 */
static void
fsm_action_nop(fsm_instance * fi, int event, void *arg)
{
}

/**
 * Actions for channel - statemachines.
 *****************************************************************************/

/**
 * Normal data has been send. Free the corresponding
 * skb (it's in io_queue), reset dev->tbusy and
 * revert to idle state.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_txdone(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	struct net_device *dev = ch->netdev;
	struct ctc_priv *privptr = dev->priv;
	struct sk_buff *skb;
	int first = 1;
	int i;

	struct timespec done_stamp = xtime;
	unsigned long duration =
	    (done_stamp.tv_sec - ch->prof.send_stamp.tv_sec) * 1000000 +
	    (done_stamp.tv_nsec - ch->prof.send_stamp.tv_nsec) / 1000;
	if (duration > ch->prof.tx_time)
		ch->prof.tx_time = duration;

	if (ch->irb->scsw.count != 0)
		printk(KERN_DEBUG "%s: TX not complete, remaining %d bytes\n",
		       dev->name, ch->irb->scsw.count);

	fsm_deltimer(&ch->timer);
	while ((skb = skb_dequeue(&ch->io_queue))) {
		privptr->stats.tx_packets++;
		privptr->stats.tx_bytes += skb->len - LL_HEADER_LENGTH;
		if (first) {
			privptr->stats.tx_bytes += 2;
			first = 0;
		}
		atomic_dec(&skb->users);
		dev_kfree_skb_irq(skb);
	}
	spin_lock(&ch->collect_lock);
	clear_normalized_cda(&ch->ccw[4]);
	if (ch->collect_len > 0) {
		int rc;

		if (ctc_checkalloc_buffer(ch, 1)) {
			spin_unlock(&ch->collect_lock);
			return;
		}
		ch->trans_skb->tail = ch->trans_skb->data = ch->trans_skb_data;
		ch->trans_skb->len = 0;
		if (ch->prof.maxmulti < (ch->collect_len + 2))
			ch->prof.maxmulti = ch->collect_len + 2;
		if (ch->prof.maxcqueue < skb_queue_len(&ch->collect_queue))
			ch->prof.maxcqueue = skb_queue_len(&ch->collect_queue);
		*((__u16 *) skb_put(ch->trans_skb, 2)) = ch->collect_len + 2;
		i = 0;
		while ((skb = skb_dequeue(&ch->collect_queue))) {
			memcpy(skb_put(ch->trans_skb, skb->len), skb->data,
			       skb->len);
			privptr->stats.tx_packets++;
			privptr->stats.tx_bytes += skb->len - LL_HEADER_LENGTH;
			atomic_dec(&skb->users);
			dev_kfree_skb_irq(skb);
			i++;
		}
		ch->collect_len = 0;
		spin_unlock(&ch->collect_lock);
		ch->ccw[1].count = ch->trans_skb->len;
		fsm_addtimer(&ch->timer, CTC_TIMEOUT_5SEC, CH_EVENT_TIMER, ch);
		ch->prof.send_stamp = xtime;
		rc = ccw_device_start(ch->cdev, &ch->ccw[0],
				      (unsigned long) ch, 0xff, 0);
		ch->prof.doios_multi++;
		if (rc != 0) {
			privptr->stats.tx_dropped += i;
			privptr->stats.tx_errors += i;
			fsm_deltimer(&ch->timer);
			ccw_check_return_code(ch, rc);
		}
	} else {
		spin_unlock(&ch->collect_lock);
		fsm_newstate(fi, CH_STATE_TXIDLE);
	}
	ctc_clear_busy(dev);
}

/**
 * Initial data is sent.
 * Notify device statemachine that we are up and
 * running.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_txidle(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;

	fsm_deltimer(&ch->timer);
	fsm_newstate(fi, CH_STATE_TXIDLE);
	fsm_event(((struct ctc_priv *) ch->netdev->priv)->fsm, DEV_EVENT_TXUP,
		  ch->netdev);
}

/**
 * Got normal data, check for sanity, queue it up, allocate new buffer
 * trigger bottom half, and initiate next read.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_rx(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	struct net_device *dev = ch->netdev;
	struct ctc_priv *privptr = dev->priv;
	int len = ch->max_bufsize - ch->irb->scsw.count;
	struct sk_buff *skb = ch->trans_skb;
	__u16 block_len = *((__u16 *) skb->data);
	int check_len;
	int rc;

	fsm_deltimer(&ch->timer);
	if (len < 8) {
		printk(KERN_DEBUG "%s: got packet with length %d < 8\n",
		       dev->name, len);
		privptr->stats.rx_dropped++;
		privptr->stats.rx_length_errors++;
		goto again;
	}
	if (len > ch->max_bufsize) {
		printk(KERN_DEBUG "%s: got packet with length %d > %d\n",
		       dev->name, len, ch->max_bufsize);
		privptr->stats.rx_dropped++;
		privptr->stats.rx_length_errors++;
		goto again;
	}

	/**
	 * VM TCP seems to have a bug sending 2 trailing bytes of garbage.
	 */
	switch (ch->protocol) {
	case CTC_PROTO_S390:
	case CTC_PROTO_OS390:
		check_len = block_len + 2;
		break;
	default:
		check_len = block_len;
		break;
	}
	if ((len < block_len) || (len > check_len)) {
		printk(KERN_DEBUG "%s: got block length %d != rx length %d\n",
		       dev->name, block_len, len);
		ctc_dump_skb(skb, 0);
		*((__u16 *) skb->data) = len;
		privptr->stats.rx_dropped++;
		privptr->stats.rx_length_errors++;
		goto again;
	}
	block_len -= 2;
	if (block_len > 0) {
		*((__u16 *) skb->data) = block_len;
		ctc_unpack_skb(ch, skb);
	}
      again:
	skb->data = skb->tail = ch->trans_skb_data;
	skb->len = 0;
	if (ctc_checkalloc_buffer(ch, 1))
		return;
	ch->ccw[1].count = ch->max_bufsize;
	rc = ccw_device_start(ch->cdev, &ch->ccw[0], (unsigned long) ch, 0xff, 0);
	if (rc != 0)
		ccw_check_return_code(ch, rc);
}

static void ch_action_rxidle(fsm_instance * fi, int event, void *arg);

/**
 * Initialize connection by sending a __u16 of value 0.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_firstio(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	int rc;

	if (fsm_getstate(fi) == CH_STATE_TXIDLE)
		printk(KERN_DEBUG "%s: remote side issued READ?, "
		       "init ...\n", ch->id);
	fsm_deltimer(&ch->timer);
	if (ctc_checkalloc_buffer(ch, 1))
		return;
	if ((fsm_getstate(fi) == CH_STATE_SETUPWAIT) &&
	    (ch->protocol == CTC_PROTO_OS390)) {
		/* OS/390 resp. z/OS */
		if (CHANNEL_DIRECTION(ch->flags) == READ) {
			*((__u16 *) ch->trans_skb->data) = CTC_INITIAL_BLOCKLEN;
			fsm_addtimer(&ch->timer, CTC_TIMEOUT_5SEC,
				     CH_EVENT_TIMER, ch);
			ch_action_rxidle(fi, event, arg);
		} else {
			struct net_device *dev = ch->netdev;
			fsm_newstate(fi, CH_STATE_TXIDLE);
			fsm_event(((struct ctc_priv *) dev->priv)->fsm,
				  DEV_EVENT_TXUP, dev);
		}
		return;
	}

	/**
	 * Don�t setup a timer for receiving the initial RX frame
	 * if in compatibility mode, since VM TCP delays the initial
	 * frame until it has some data to send.
	 */
	if ((CHANNEL_DIRECTION(ch->flags) == WRITE) ||
	    (ch->protocol != CTC_PROTO_S390))
		fsm_addtimer(&ch->timer, CTC_TIMEOUT_5SEC, CH_EVENT_TIMER, ch);

	*((__u16 *) ch->trans_skb->data) = CTC_INITIAL_BLOCKLEN;
	ch->ccw[1].count = 2;	/* Transfer only length */

	fsm_newstate(fi, (CHANNEL_DIRECTION(ch->flags) == READ)
		     ? CH_STATE_RXINIT : CH_STATE_TXINIT);
	rc = ccw_device_start(ch->cdev, &ch->ccw[0], (unsigned long) ch, 0xff, 0);
	if (rc != 0) {
		fsm_deltimer(&ch->timer);
		fsm_newstate(fi, CH_STATE_SETUPWAIT);
		ccw_check_return_code(ch, rc);
	}
	/**
	 * If in compatibility mode since we don�t setup a timer, we
	 * also signal RX channel up immediately. This enables us
	 * to send packets early which in turn usually triggers some
	 * reply from VM TCP which brings up the RX channel to it�s
	 * final state.
	 */
	if ((CHANNEL_DIRECTION(ch->flags) == READ) &&
	    (ch->protocol == CTC_PROTO_S390)) {
		struct net_device *dev = ch->netdev;
		fsm_event(((struct ctc_priv *) dev->priv)->fsm, DEV_EVENT_RXUP,
			  dev);
	}
}

/**
 * Got initial data, check it. If OK,
 * notify device statemachine that we are up and
 * running.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_rxidle(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	struct net_device *dev = ch->netdev;
	__u16 buflen;
	int rc;

	fsm_deltimer(&ch->timer);
	buflen = *((__u16 *) ch->trans_skb->data);
	pr_debug("%s: Initial RX count %d\n", dev->name, buflen);
	if (buflen >= CTC_INITIAL_BLOCKLEN) {
		if (ctc_checkalloc_buffer(ch, 1))
			return;
		ch->ccw[1].count = ch->max_bufsize;
		fsm_newstate(fi, CH_STATE_RXIDLE);
		rc = ccw_device_start(ch->cdev, &ch->ccw[0],
				      (unsigned long) ch, 0xff, 0);
		if (rc != 0) {
			fsm_newstate(fi, CH_STATE_RXINIT);
			ccw_check_return_code(ch, rc);
		} else
			fsm_event(((struct ctc_priv *) dev->priv)->fsm,
				  DEV_EVENT_RXUP, dev);
	} else {
		printk(KERN_DEBUG "%s: Initial RX count %d not %d\n",
		       dev->name, buflen, CTC_INITIAL_BLOCKLEN);
		ch_action_firstio(fi, event, arg);
	}
}

/**
 * Set channel into extended mode.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_setmode(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	int rc;
	unsigned long saveflags;

	fsm_deltimer(&ch->timer);
	fsm_addtimer(&ch->timer, CTC_TIMEOUT_5SEC, CH_EVENT_TIMER, ch);
	fsm_newstate(fi, CH_STATE_SETUPWAIT);
	if (event == CH_EVENT_TIMER)
		spin_lock_irqsave(get_ccwdev_lock(ch->cdev), saveflags);
	rc = ccw_device_start(ch->cdev, &ch->ccw[6], (unsigned long) ch, 0xff, 0);
	if (event == CH_EVENT_TIMER)
		spin_unlock_irqrestore(get_ccwdev_lock(ch->cdev), saveflags);
	if (rc != 0) {
		fsm_deltimer(&ch->timer);
		fsm_newstate(fi, CH_STATE_STARTWAIT);
		ccw_check_return_code(ch, rc);
	} else
		ch->retry = 0;
}

/**
 * Setup channel.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_start(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	unsigned long saveflags;
	int rc;
	struct net_device *dev;

	if (ch == NULL) {
		printk(KERN_WARNING "ch_action_start ch=NULL\n");
		return;
	}
	if (ch->netdev == NULL) {
		printk(KERN_WARNING "ch_action_start dev=NULL, id=%s\n",
		       ch->id);
		return;
	}
	dev = ch->netdev;

	pr_debug("%s: %s channel start\n", dev->name,
		 (CHANNEL_DIRECTION(ch->flags) == READ) ? "RX" : "TX");

	if (ch->trans_skb != NULL) {
		clear_normalized_cda(&ch->ccw[1]);
		dev_kfree_skb(ch->trans_skb);
		ch->trans_skb = NULL;
	}
	if (CHANNEL_DIRECTION(ch->flags) == READ) {
		ch->ccw[1].cmd_code = CCW_CMD_READ;
		ch->ccw[1].flags = CCW_FLAG_SLI;
		ch->ccw[1].count = 0;
	} else {
		ch->ccw[1].cmd_code = CCW_CMD_WRITE;
		ch->ccw[1].flags = CCW_FLAG_SLI | CCW_FLAG_CC;
		ch->ccw[1].count = 0;
	}
	if (ctc_checkalloc_buffer(ch, 0))
		printk(KERN_NOTICE
		       "%s: Could not allocate %s trans_skb, delaying "
		       "allocation until first transfer\n",
		       dev->name,
		       (CHANNEL_DIRECTION(ch->flags) == READ) ? "RX" : "TX");

	ch->ccw[0].cmd_code = CCW_CMD_PREPARE;
	ch->ccw[0].flags = CCW_FLAG_SLI | CCW_FLAG_CC;
	ch->ccw[0].count = 0;
	ch->ccw[0].cda = 0;
	ch->ccw[2].cmd_code = CCW_CMD_NOOP;	/* jointed CE + DE */
	ch->ccw[2].flags = CCW_FLAG_SLI;
	ch->ccw[2].count = 0;
	ch->ccw[2].cda = 0;
	memcpy(&ch->ccw[3], &ch->ccw[0], sizeof (struct ccw1) * 3);
	ch->ccw[4].cda = 0;
	ch->ccw[4].flags &= ~CCW_FLAG_IDA;

	fsm_newstate(fi, CH_STATE_STARTWAIT);
	fsm_addtimer(&ch->timer, 1000, CH_EVENT_TIMER, ch);
	spin_lock_irqsave(get_ccwdev_lock(ch->cdev), saveflags);
	rc = ccw_device_halt(ch->cdev, (unsigned long) ch);
	spin_unlock_irqrestore(get_ccwdev_lock(ch->cdev), saveflags);
	if (rc != 0) {
		fsm_deltimer(&ch->timer);
		ccw_check_return_code(ch, rc);
	}
	pr_debug("ctc: %s(): leaving\n", __FUNCTION__);
}

/**
 * Shutdown a channel.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_haltio(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	unsigned long saveflags;
	int rc;
	int oldstate;

	fsm_deltimer(&ch->timer);
	fsm_addtimer(&ch->timer, CTC_TIMEOUT_5SEC, CH_EVENT_TIMER, ch);
	if (event == CH_EVENT_STOP)
		spin_lock_irqsave(get_ccwdev_lock(ch->cdev), saveflags);
	oldstate = fsm_getstate(fi);
	fsm_newstate(fi, CH_STATE_TERM);
	rc = ccw_device_halt(ch->cdev, (unsigned long) ch);
	if (event == CH_EVENT_STOP)
		spin_unlock_irqrestore(get_ccwdev_lock(ch->cdev), saveflags);
	if (rc != 0) {
		fsm_deltimer(&ch->timer);
		fsm_newstate(fi, oldstate);
		ccw_check_return_code(ch, rc);
	}
}

/**
 * A channel has successfully been halted.
 * Cleanup it's queue and notify interface statemachine.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_stopped(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	struct net_device *dev = ch->netdev;

	fsm_deltimer(&ch->timer);
	fsm_newstate(fi, CH_STATE_STOPPED);
	if (ch->trans_skb != NULL) {
		clear_normalized_cda(&ch->ccw[1]);
		dev_kfree_skb(ch->trans_skb);
		ch->trans_skb = NULL;
	}
	if (CHANNEL_DIRECTION(ch->flags) == READ) {
		skb_queue_purge(&ch->io_queue);
		fsm_event(((struct ctc_priv *) dev->priv)->fsm,
			  DEV_EVENT_RXDOWN, dev);
	} else {
		ctc_purge_skb_queue(&ch->io_queue);
		spin_lock(&ch->collect_lock);
		ctc_purge_skb_queue(&ch->collect_queue);
		ch->collect_len = 0;
		spin_unlock(&ch->collect_lock);
		fsm_event(((struct ctc_priv *) dev->priv)->fsm,
			  DEV_EVENT_TXDOWN, dev);
	}
}

/**
 * A stop command from device statemachine arrived and we are in
 * not operational mode. Set state to stopped.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_stop(fsm_instance * fi, int event, void *arg)
{
	fsm_newstate(fi, CH_STATE_STOPPED);
}

/**
 * A machine check for no path, not operational status or gone device has
 * happened.
 * Cleanup queue and notify interface statemachine.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_fail(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	struct net_device *dev = ch->netdev;

	fsm_deltimer(&ch->timer);
	fsm_newstate(fi, CH_STATE_NOTOP);
	if (CHANNEL_DIRECTION(ch->flags) == READ) {
		skb_queue_purge(&ch->io_queue);
		fsm_event(((struct ctc_priv *) dev->priv)->fsm,
			  DEV_EVENT_RXDOWN, dev);
	} else {
		ctc_purge_skb_queue(&ch->io_queue);
		spin_lock(&ch->collect_lock);
		ctc_purge_skb_queue(&ch->collect_queue);
		ch->collect_len = 0;
		spin_unlock(&ch->collect_lock);
		fsm_event(((struct ctc_priv *) dev->priv)->fsm,
			  DEV_EVENT_TXDOWN, dev);
	}
}

/**
 * Handle error during setup of channel.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_setuperr(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	struct net_device *dev = ch->netdev;

	/**
	 * Special case: Got UC_RCRESET on setmode.
	 * This means that remote side isn't setup. In this case
	 * simply retry after some 10 secs...
	 */
	if ((fsm_getstate(fi) == CH_STATE_SETUPWAIT) &&
	    ((event == CH_EVENT_UC_RCRESET) ||
	     (event == CH_EVENT_UC_RSRESET))) {
		fsm_newstate(fi, CH_STATE_STARTRETRY);
		fsm_deltimer(&ch->timer);
		fsm_addtimer(&ch->timer, CTC_TIMEOUT_5SEC, CH_EVENT_TIMER, ch);
		if (CHANNEL_DIRECTION(ch->flags) == READ) {
			int rc = ccw_device_halt(ch->cdev, (unsigned long) ch);
			if (rc != 0)
				ccw_check_return_code(ch, rc);
		}
		return;
	}

	printk(KERN_DEBUG "%s: Error %s during %s channel setup state=%s\n",
	       dev->name, ch_event_names[event],
	       (CHANNEL_DIRECTION(ch->flags) == READ) ? "RX" : "TX",
	       fsm_getstate_str(fi));
	if (CHANNEL_DIRECTION(ch->flags) == READ) {
		fsm_newstate(fi, CH_STATE_RXERR);
		fsm_event(((struct ctc_priv *) dev->priv)->fsm,
			  DEV_EVENT_RXDOWN, dev);
	} else {
		fsm_newstate(fi, CH_STATE_TXERR);
		fsm_event(((struct ctc_priv *) dev->priv)->fsm,
			  DEV_EVENT_TXDOWN, dev);
	}
}

/**
 * Restart a channel after an error.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_restart(fsm_instance * fi, int event, void *arg)
{
	unsigned long saveflags;
	int oldstate;
	int rc;

	struct channel *ch = (struct channel *) arg;
	struct net_device *dev = ch->netdev;

	fsm_deltimer(&ch->timer);
	printk(KERN_DEBUG "%s: %s channel restart\n", dev->name,
	       (CHANNEL_DIRECTION(ch->flags) == READ) ? "RX" : "TX");
	fsm_addtimer(&ch->timer, CTC_TIMEOUT_5SEC, CH_EVENT_TIMER, ch);
	oldstate = fsm_getstate(fi);
	fsm_newstate(fi, CH_STATE_STARTWAIT);
	if (event == CH_EVENT_TIMER)
		spin_lock_irqsave(get_ccwdev_lock(ch->cdev), saveflags);
	rc = ccw_device_halt(ch->cdev, (unsigned long) ch);
	if (event == CH_EVENT_TIMER)
		spin_unlock_irqrestore(get_ccwdev_lock(ch->cdev), saveflags);
	if (rc != 0) {
		fsm_deltimer(&ch->timer);
		fsm_newstate(fi, oldstate);
		ccw_check_return_code(ch, rc);
	}
}

/**
 * Handle error during RX initial handshake (exchange of
 * 0-length block header)
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_rxiniterr(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	struct net_device *dev = ch->netdev;

	if (event == CH_EVENT_TIMER) {
		fsm_deltimer(&ch->timer);
		printk(KERN_DEBUG "%s: Timeout during RX init handshake\n",
		       dev->name);
		if (ch->retry++ < 3)
			ch_action_restart(fi, event, arg);
		else {
			fsm_newstate(fi, CH_STATE_RXERR);
			fsm_event(((struct ctc_priv *) dev->priv)->fsm,
				  DEV_EVENT_RXDOWN, dev);
		}
	} else
		printk(KERN_WARNING "%s: Error during RX init handshake\n",
		       dev->name);
}

/**
 * Notify device statemachine if we gave up initialization
 * of RX channel.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_rxinitfail(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	struct net_device *dev = ch->netdev;

	fsm_newstate(fi, CH_STATE_RXERR);
	printk(KERN_WARNING "%s: RX initialization failed\n", dev->name);
	printk(KERN_WARNING "%s: RX <-> RX connection detected\n", dev->name);
	fsm_event(((struct ctc_priv *) dev->priv)->fsm, DEV_EVENT_RXDOWN, dev);
}

/**
 * Handle RX Unit check remote reset (remote disconnected)
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_rxdisc(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	struct channel *ch2;
	struct net_device *dev = ch->netdev;

	fsm_deltimer(&ch->timer);
	printk(KERN_DEBUG "%s: Got remote disconnect, re-initializing ...\n",
	       dev->name);

	/**
	 * Notify device statemachine
	 */
	fsm_event(((struct ctc_priv *) dev->priv)->fsm, DEV_EVENT_RXDOWN, dev);
	fsm_event(((struct ctc_priv *) dev->priv)->fsm, DEV_EVENT_TXDOWN, dev);

	fsm_newstate(fi, CH_STATE_DTERM);
	ch2 = ((struct ctc_priv *) dev->priv)->channel[WRITE];
	fsm_newstate(ch2->fsm, CH_STATE_DTERM);

	ccw_device_halt(ch->cdev, (unsigned long) ch);
	ccw_device_halt(ch2->cdev, (unsigned long) ch2);
}

/**
 * Handle error during TX channel initialization.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_txiniterr(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	struct net_device *dev = ch->netdev;

	if (event == CH_EVENT_TIMER) {
		fsm_deltimer(&ch->timer);
		printk(KERN_DEBUG "%s: Timeout during TX init handshake\n",
		       dev->name);
		if (ch->retry++ < 3)
			ch_action_restart(fi, event, arg);
		else {
			fsm_newstate(fi, CH_STATE_TXERR);
			fsm_event(((struct ctc_priv *) dev->priv)->fsm,
				  DEV_EVENT_TXDOWN, dev);
		}
	} else
		printk(KERN_WARNING "%s: Error during TX init handshake\n",
		       dev->name);
}

/**
 * Handle TX timeout by retrying operation.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_txretry(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	struct net_device *dev = ch->netdev;
	unsigned long saveflags;

	fsm_deltimer(&ch->timer);
	if (ch->retry++ > 3) {
		printk(KERN_DEBUG "%s: TX retry failed, restarting channel\n",
		       dev->name);
		fsm_event(((struct ctc_priv *) dev->priv)->fsm,
			  DEV_EVENT_TXDOWN, dev);
		ch_action_restart(fi, event, arg);
	} else {
		struct sk_buff *skb;

		printk(KERN_DEBUG "%s: TX retry %d\n", dev->name, ch->retry);
		if ((skb = skb_peek(&ch->io_queue))) {
			int rc = 0;

			clear_normalized_cda(&ch->ccw[4]);
			ch->ccw[4].count = skb->len;
			if (set_normalized_cda(&ch->ccw[4], skb->data)) {
				printk(KERN_DEBUG "%s: IDAL alloc failed, "
				       "restarting channel\n", dev->name);
				fsm_event(((struct ctc_priv *) dev->priv)->fsm,
					  DEV_EVENT_TXDOWN, dev);
				ch_action_restart(fi, event, arg);
				return;
			}
			fsm_addtimer(&ch->timer, 1000, CH_EVENT_TIMER, ch);
			if (event == CH_EVENT_TIMER)
				spin_lock_irqsave(get_ccwdev_lock(ch->cdev),
						  saveflags);
			rc = ccw_device_start(ch->cdev, &ch->ccw[3],
					      (unsigned long) ch, 0xff, 0);
			if (event == CH_EVENT_TIMER)
				spin_unlock_irqrestore(get_ccwdev_lock(ch->cdev),
						       saveflags);
			if (rc != 0) {
				fsm_deltimer(&ch->timer);
				ccw_check_return_code(ch, rc);
				ctc_purge_skb_queue(&ch->io_queue);
			}
		}
	}

}

/**
 * Handle fatal errors during an I/O command.
 *
 * @param fi    An instance of a channel statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from channel * upon call.
 */
static void
ch_action_iofatal(fsm_instance * fi, int event, void *arg)
{
	struct channel *ch = (struct channel *) arg;
	struct net_device *dev = ch->netdev;

	fsm_deltimer(&ch->timer);
	if (CHANNEL_DIRECTION(ch->flags) == READ) {
		printk(KERN_DEBUG "%s: RX I/O error\n", dev->name);
		fsm_newstate(fi, CH_STATE_RXERR);
		fsm_event(((struct ctc_priv *) dev->priv)->fsm,
			  DEV_EVENT_RXDOWN, dev);
	} else {
		printk(KERN_DEBUG "%s: TX I/O error\n", dev->name);
		fsm_newstate(fi, CH_STATE_TXERR);
		fsm_event(((struct ctc_priv *) dev->priv)->fsm,
			  DEV_EVENT_TXDOWN, dev);
	}
}

static void 
ch_action_reinit(fsm_instance *fi, int event, void *arg)
{
 	struct channel *ch = (struct channel *)arg;
 	struct net_device *dev = ch->netdev;
 	struct ctc_priv *privptr = dev->priv;
 
 	ch_action_iofatal(fi, event, arg);
 	fsm_addtimer(&privptr->restart_timer, 1000, DEV_EVENT_RESTART, dev);
}


/**
 * The statemachine for a channel.
 */
static const fsm_node ch_fsm[] = {
	{CH_STATE_STOPPED, CH_EVENT_STOP, fsm_action_nop},
	{CH_STATE_STOPPED, CH_EVENT_START, ch_action_start},
	{CH_STATE_STOPPED, CH_EVENT_FINSTAT, fsm_action_nop},
	{CH_STATE_STOPPED, CH_EVENT_MC_FAIL, fsm_action_nop},

	{CH_STATE_NOTOP, CH_EVENT_STOP, ch_action_stop},
	{CH_STATE_NOTOP, CH_EVENT_START, fsm_action_nop},
	{CH_STATE_NOTOP, CH_EVENT_FINSTAT, fsm_action_nop},
	{CH_STATE_NOTOP, CH_EVENT_MC_FAIL, fsm_action_nop},
	{CH_STATE_NOTOP, CH_EVENT_MC_GOOD, ch_action_start},

	{CH_STATE_STARTWAIT, CH_EVENT_STOP, ch_action_haltio},
	{CH_STATE_STARTWAIT, CH_EVENT_START, fsm_action_nop},
	{CH_STATE_STARTWAIT, CH_EVENT_FINSTAT, ch_action_setmode},
	{CH_STATE_STARTWAIT, CH_EVENT_TIMER, ch_action_setuperr},
	{CH_STATE_STARTWAIT, CH_EVENT_IO_ENODEV, ch_action_iofatal},
	{CH_STATE_STARTWAIT, CH_EVENT_IO_EIO, ch_action_reinit},
	{CH_STATE_STARTWAIT, CH_EVENT_MC_FAIL, ch_action_fail},

	{CH_STATE_STARTRETRY, CH_EVENT_STOP, ch_action_haltio},
	{CH_STATE_STARTRETRY, CH_EVENT_TIMER, ch_action_setmode},
	{CH_STATE_STARTRETRY, CH_EVENT_FINSTAT, fsm_action_nop},
	{CH_STATE_STARTRETRY, CH_EVENT_MC_FAIL, ch_action_fail},

	{CH_STATE_SETUPWAIT, CH_EVENT_STOP, ch_action_haltio},
	{CH_STATE_SETUPWAIT, CH_EVENT_START, fsm_action_nop},
	{CH_STATE_SETUPWAIT, CH_EVENT_FINSTAT, ch_action_firstio},
	{CH_STATE_SETUPWAIT, CH_EVENT_UC_RCRESET, ch_action_setuperr},
	{CH_STATE_SETUPWAIT, CH_EVENT_UC_RSRESET, ch_action_setuperr},
	{CH_STATE_SETUPWAIT, CH_EVENT_TIMER, ch_action_setmode},
	{CH_STATE_SETUPWAIT, CH_EVENT_IO_ENODEV, ch_action_iofatal},
	{CH_STATE_SETUPWAIT, CH_EVENT_IO_EIO, ch_action_reinit},
	{CH_STATE_SETUPWAIT, CH_EVENT_MC_FAIL, ch_action_fail},

	{CH_STATE_RXINIT, CH_EVENT_STOP, ch_action_haltio},
	{CH_STATE_RXINIT, CH_EVENT_START, fsm_action_nop},
	{CH_STATE_RXINIT, CH_EVENT_FINSTAT, ch_action_rxidle},
	{CH_STATE_RXINIT, CH_EVENT_UC_RCRESET, ch_action_rxiniterr},
	{CH_STATE_RXINIT, CH_EVENT_UC_RSRESET, ch_action_rxiniterr},
	{CH_STATE_RXINIT, CH_EVENT_TIMER, ch_action_rxiniterr},
	{CH_STATE_RXINIT, CH_EVENT_ATTNBUSY, ch_action_rxinitfail},
	{CH_STATE_RXINIT, CH_EVENT_IO_ENODEV, ch_action_iofatal},
	{CH_STATE_RXINIT, CH_EVENT_IO_EIO, ch_action_reinit},
	{CH_STATE_RXINIT, CH_EVENT_UC_ZERO, ch_action_firstio},
	{CH_STATE_RXINIT, CH_EVENT_MC_FAIL, ch_action_fail},

	{CH_STATE_RXIDLE, CH_EVENT_STOP, ch_action_haltio},
	{CH_STATE_RXIDLE, CH_EVENT_START, fsm_action_nop},
	{CH_STATE_RXIDLE, CH_EVENT_FINSTAT, ch_action_rx},
	{CH_STATE_RXIDLE, CH_EVENT_UC_RCRESET, ch_action_rxdisc},
//      { CH_STATE_RXIDLE,     CH_EVENT_UC_RSRESET, ch_action_rxretry    },
	{CH_STATE_RXIDLE, CH_EVENT_IO_ENODEV, ch_action_iofatal},
	{CH_STATE_RXIDLE, CH_EVENT_IO_EIO, ch_action_reinit},
	{CH_STATE_RXIDLE, CH_EVENT_MC_FAIL, ch_action_fail},
	{CH_STATE_RXIDLE, CH_EVENT_UC_ZERO, ch_action_rx},

	{CH_STATE_TXINIT, CH_EVENT_STOP, ch_action_haltio},
	{CH_STATE_TXINIT, CH_EVENT_START, fsm_action_nop},
	{CH_STATE_TXINIT, CH_EVENT_FINSTAT, ch_action_txidle},
	{CH_STATE_TXINIT, CH_EVENT_UC_RCRESET, ch_action_txiniterr},
	{CH_STATE_TXINIT, CH_EVENT_UC_RSRESET, ch_action_txiniterr},
	{CH_STATE_TXINIT, CH_EVENT_TIMER, ch_action_txiniterr},
	{CH_STATE_TXINIT, CH_EVENT_IO_ENODEV, ch_action_iofatal},
	{CH_STATE_TXINIT, CH_EVENT_IO_EIO, ch_action_reinit},
	{CH_STATE_TXINIT, CH_EVENT_MC_FAIL, ch_action_fail},

	{CH_STATE_TXIDLE, CH_EVENT_STOP, ch_action_haltio},
	{CH_STATE_TXIDLE, CH_EVENT_START, fsm_action_nop},
	{CH_STATE_TXIDLE, CH_EVENT_FINSTAT, ch_action_firstio},
	{CH_STATE_TXIDLE, CH_EVENT_UC_RCRESET, fsm_action_nop},
	{CH_STATE_TXIDLE, CH_EVENT_UC_RSRESET, fsm_action_nop},
	{CH_STATE_TXIDLE, CH_EVENT_IO_ENODEV, ch_action_iofatal},
	{CH_STATE_TXIDLE, CH_EVENT_IO_EIO, ch_action_reinit},
	{CH_STATE_TXIDLE, CH_EVENT_MC_FAIL, ch_action_fail},

	{CH_STATE_TERM, CH_EVENT_STOP, fsm_action_nop},
	{CH_STATE_TERM, CH_EVENT_START, ch_action_restart},
	{CH_STATE_TERM, CH_EVENT_FINSTAT, ch_action_stopped},
	{CH_STATE_TERM, CH_EVENT_UC_RCRESET, fsm_action_nop},
	{CH_STATE_TERM, CH_EVENT_UC_RSRESET, fsm_action_nop},
	{CH_STATE_TERM, CH_EVENT_MC_FAIL, ch_action_fail},

	{CH_STATE_DTERM, CH_EVENT_STOP, ch_action_haltio},
	{CH_STATE_DTERM, CH_EVENT_START, ch_action_restart},
	{CH_STATE_DTERM, CH_EVENT_FINSTAT, ch_action_setmode},
	{CH_STATE_DTERM, CH_EVENT_UC_RCRESET, fsm_action_nop},
	{CH_STATE_DTERM, CH_EVENT_UC_RSRESET, fsm_action_nop},
	{CH_STATE_DTERM, CH_EVENT_MC_FAIL, ch_action_fail},

	{CH_STATE_TX, CH_EVENT_STOP, ch_action_haltio},
	{CH_STATE_TX, CH_EVENT_START, fsm_action_nop},
	{CH_STATE_TX, CH_EVENT_FINSTAT, ch_action_txdone},
	{CH_STATE_TX, CH_EVENT_UC_RCRESET, ch_action_txretry},
	{CH_STATE_TX, CH_EVENT_UC_RSRESET, ch_action_txretry},
	{CH_STATE_TX, CH_EVENT_TIMER, ch_action_txretry},
	{CH_STATE_TX, CH_EVENT_IO_ENODEV, ch_action_iofatal},
	{CH_STATE_TX, CH_EVENT_IO_EIO, ch_action_reinit},
	{CH_STATE_TX, CH_EVENT_MC_FAIL, ch_action_fail},

	{CH_STATE_RXERR, CH_EVENT_STOP, ch_action_haltio},
	{CH_STATE_TXERR, CH_EVENT_STOP, ch_action_haltio},
	{CH_STATE_TXERR, CH_EVENT_MC_FAIL, ch_action_fail},
	{CH_STATE_RXERR, CH_EVENT_MC_FAIL, ch_action_fail},
};

static const int CH_FSM_LEN = sizeof (ch_fsm) / sizeof (fsm_node);

/**
 * Functions related to setup and device detection.
 *****************************************************************************/

static inline int
less_than(char *id1, char *id2)
{
	int dev1,dev2,i;

	for (i=0;i<5;i++) {
		id1++;
		id2++;
	}
	dev1 = simple_strtoul(id1, &id1, 16);
	dev2 = simple_strtoul(id2, &id2, 16);
	
	return (dev1<dev2);
}

/**
 * Add a new channel to the list of channels.
 * Keeps the channel list sorted.
 *
 * @param cdev  The ccw_device to be added.
 * @param type  The type class of the new channel.
 *
 * @return 0 on success, !0 on error.
 */
static int
add_channel(struct ccw_device *cdev, enum channel_types type)
{
	struct channel **c = &channels;
	struct channel *ch;

	if ((ch =
	     (struct channel *) kmalloc(sizeof (struct channel),
					GFP_KERNEL)) == NULL) {
		printk(KERN_WARNING "ctc: Out of memory in add_channel\n");
		return -1;
	}
	memset(ch, 0, sizeof (struct channel));
	if ((ch->ccw = (struct ccw1 *) kmalloc(sizeof (struct ccw1) * 8,
					       GFP_KERNEL | GFP_DMA)) == NULL) {
		kfree(ch);
		printk(KERN_WARNING "ctc: Out of memory in add_channel\n");
		return -1;
	}

	/**
	 * "static" ccws are used in the following way:
	 *
	 * ccw[0..2] (Channel program for generic I/O):
	 *           0: prepare
	 *           1: read or write (depending on direction) with fixed
	 *              buffer (idal allocated once when buffer is allocated)
	 *           2: nop
	 * ccw[3..5] (Channel program for direct write of packets)
	 *           3: prepare
	 *           4: write (idal allocated on every write).
	 *           5: nop
	 * ccw[6..7] (Channel program for initial channel setup):
	 *           3: set extended mode
	 *           4: nop
	 *
	 * ch->ccw[0..5] are initialized in ch_action_start because
	 * the channel's direction is yet unknown here.
	 */
	ch->ccw[6].cmd_code = CCW_CMD_SET_EXTENDED;
	ch->ccw[6].flags = CCW_FLAG_SLI;
	ch->ccw[6].count = 0;
	ch->ccw[6].cda = 0;

	ch->ccw[7].cmd_code = CCW_CMD_NOOP;
	ch->ccw[7].flags = CCW_FLAG_SLI;
	ch->ccw[7].count = 0;
	ch->ccw[7].cda = 0;

	ch->cdev = cdev;
	snprintf(ch->id, DEVICE_ID_SIZE, "ch-%s", cdev->dev.bus_id);
	ch->type = type;
	ch->fsm = init_fsm(ch->id, ch_state_names,
			   ch_event_names, NR_CH_STATES, NR_CH_EVENTS,
			   ch_fsm, CH_FSM_LEN, GFP_KERNEL);
	if (ch->fsm == NULL) {
		printk(KERN_WARNING
		       "ctc: Could not create FSM in add_channel\n");
		kfree(ch);
		return -1;
	}
	fsm_newstate(ch->fsm, CH_STATE_IDLE);
	if ((ch->irb = (struct irb *) kmalloc(sizeof (struct irb),
					      GFP_KERNEL)) == NULL) {
		printk(KERN_WARNING "ctc: Out of memory in add_channel\n");
		kfree_fsm(ch->fsm);
		kfree(ch);
		return -1;
	}
	memset(ch->irb, 0, sizeof (struct irb));
	while (*c && less_than((*c)->id, ch->id))
		c = &(*c)->next;
	if (!strncmp((*c)->id, ch->id, CTC_ID_SIZE)) {
		printk(KERN_DEBUG
		       "ctc: add_channel: device %s already in list, "
		       "using old entry\n", (*c)->id);
		kfree(ch->irb);
		kfree_fsm(ch->fsm);
		kfree(ch);
		return 0;
	}
	fsm_settimer(ch->fsm, &ch->timer);
	skb_queue_head_init(&ch->io_queue);
	skb_queue_head_init(&ch->collect_queue);
	ch->next = *c;
	*c = ch;
	return 0;
}

/**
 * Release a specific channel in the channel list.
 *
 * @param ch Pointer to channel struct to be released.
 */
static void
channel_free(struct channel *ch)
{
	ch->flags &= ~CHANNEL_FLAGS_INUSE;
	fsm_newstate(ch->fsm, CH_STATE_IDLE);
}

/**
 * Remove a specific channel in the channel list.
 *
 * @param ch Pointer to channel struct to be released.
 */
static void
channel_remove(struct channel *ch)
{
	struct channel **c = &channels;

	if (ch == NULL)
		return;

	channel_free(ch);
	while (*c) {
		if (*c == ch) {
			*c = ch->next;
			fsm_deltimer(&ch->timer);
			kfree_fsm(ch->fsm);
			clear_normalized_cda(&ch->ccw[4]);
			if (ch->trans_skb != NULL) {
				clear_normalized_cda(&ch->ccw[1]);
				dev_kfree_skb(ch->trans_skb);
			}
			kfree(ch->ccw);
			return;
		}
		c = &((*c)->next);
	}
}

/**
 * Get a specific channel from the channel list.
 *
 * @param type Type of channel we are interested in.
 * @param id Id of channel we are interested in.
 * @param direction Direction we want to use this channel for.
 *
 * @return Pointer to a channel or NULL if no matching channel available.
 */
static struct channel
*
channel_get(enum channel_types type, char *id, int direction)
{
	struct channel *ch = channels;

	pr_debug("ctc: %s(): searching for ch with id %d and type %d\n",
		 __FUNCTION__, id, type);

	while (ch && ((strncmp(ch->id, id, CTC_ID_SIZE)) || (ch->type != type))) {
		pr_debug("ctc: %s(): ch=0x%p (id=%s, type=%d\n",
			 __FUNCTION__, ch, ch->id, ch->type);
		ch = ch->next;
	}
	pr_debug("ctc: %s(): ch=0x%pq (id=%s, type=%d\n",
		 __FUNCTION__, ch, ch->id, ch->type);
	if (!ch) {
		printk(KERN_WARNING "ctc: %s(): channel with id %s "
		       "and type %d not found in channel list\n",
		       __FUNCTION__, id, type);
	} else {
		if (ch->flags & CHANNEL_FLAGS_INUSE)
			ch = NULL;
		else {
			ch->flags |= CHANNEL_FLAGS_INUSE;
			ch->flags &= ~CHANNEL_FLAGS_RWMASK;
			ch->flags |= (direction == WRITE)
			    ? CHANNEL_FLAGS_WRITE : CHANNEL_FLAGS_READ;
			fsm_newstate(ch->fsm, CH_STATE_STOPPED);
		}
	}
	return ch;
}

/**
 * Return the channel type by name.
 *
 * @param name Name of network interface.
 *
 * @return Type class of channel to be used for that interface.
 */
static enum channel_types inline
extract_channel_media(char *name)
{
	enum channel_types ret = channel_type_unknown;

	if (name != NULL) {
		if (strncmp(name, "ctc", 3) == 0)
			ret = channel_type_parallel;
		if (strncmp(name, "escon", 5) == 0)
			ret = channel_type_escon;
	}
	return ret;
}

/**
 * Main IRQ handler.
 *
 * @param cdev    The ccw_device the interrupt is for.
 * @param intparm interruption parameter.
 * @param irb     interruption response block.
 */
static void
ctc_irq_handler(struct ccw_device *cdev, unsigned long intparm, struct irb *irb)
{
	struct channel *ch;
	struct net_device *dev;
	struct ctc_priv *priv;

	/* Check for unsolicited interrupts. */
	if (!cdev->dev.driver_data) {
		printk(KERN_WARNING
		       "ctc: Got unsolicited irq: %s c-%02x d-%02x\n",
		       cdev->dev.bus_id, irb->scsw.cstat,
		       irb->scsw.dstat);
		return;
	}
	
	priv = cdev->dev.driver_data;
	ch = (struct channel *) intparm;
	if ((ch != priv->channel[READ]) && (ch != priv->channel[WRITE]))
		ch = NULL;
	
	if (!ch) {
		/* Try to extract channel from driver data. */
		if (priv->channel[READ]->cdev == cdev)
			ch = priv->channel[READ];
		else if (priv->channel[WRITE]->cdev == cdev)
			ch = priv->channel[READ];
		else {
			printk(KERN_ERR
			       "ctc: Can't determine channel for interrupt, "
			       "device %s\n", cdev->dev.bus_id);
			return;
		}
	}
	
	dev = (struct net_device *) (ch->netdev);
	if (dev == NULL) {
		printk(KERN_CRIT
		       "ctc: ctc_irq_handler dev = NULL bus_id=%s, ch=0x%p\n",
		       cdev->dev.bus_id, ch);
		return;
	}

	pr_debug("%s: interrupt for device: %s received c-%02x d-%02x\n"
		 dev->name, ch->id, irb->scsw.cstat, irb->scsw.dstat);

	/* Copy interruption response block. */
	memcpy(ch->irb, irb, sizeof(struct irb));

	/* Check for good subchannel return code, otherwise error message */
	if (ch->irb->scsw.cstat) {
		fsm_event(ch->fsm, CH_EVENT_SC_UNKNOWN, ch);
		printk(KERN_WARNING
		       "%s: subchannel check for device: %s - %02x %02x\n",
		       dev->name, ch->id, ch->irb->scsw.cstat,
		       ch->irb->scsw.dstat);
		return;
	}

	/* Check the reason-code of a unit check */
	if (ch->irb->scsw.dstat & DEV_STAT_UNIT_CHECK) {
		ccw_unit_check(ch, ch->irb->ecw[0]);
		return;
	}
	if (ch->irb->scsw.dstat & DEV_STAT_BUSY) {
		if (ch->irb->scsw.dstat & DEV_STAT_ATTENTION)
			fsm_event(ch->fsm, CH_EVENT_ATTNBUSY, ch);
		else
			fsm_event(ch->fsm, CH_EVENT_BUSY, ch);
		return;
	}
	if (ch->irb->scsw.dstat & DEV_STAT_ATTENTION) {
		fsm_event(ch->fsm, CH_EVENT_ATTN, ch);
		return;
	}
	if ((ch->irb->scsw.stctl & SCSW_STCTL_SEC_STATUS) ||
	    (ch->irb->scsw.stctl == SCSW_STCTL_STATUS_PEND) ||
	    (ch->irb->scsw.stctl ==
	     (SCSW_STCTL_ALERT_STATUS | SCSW_STCTL_STATUS_PEND)))
		fsm_event(ch->fsm, CH_EVENT_FINSTAT, ch);
	else
		fsm_event(ch->fsm, CH_EVENT_IRQ, ch);

}

/**
 * Actions for interface - statemachine.
 *****************************************************************************/

/**
 * Startup channels by sending CH_EVENT_START to each channel.
 *
 * @param fi    An instance of an interface statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from struct net_device * upon call.
 */
static void
dev_action_start(fsm_instance * fi, int event, void *arg)
{
	struct net_device *dev = (struct net_device *) arg;
	struct ctc_priv *privptr = dev->priv;
	int direction;

	fsm_deltimer(&privptr->restart_timer);
	fsm_newstate(fi, DEV_STATE_STARTWAIT_RXTX);
	for (direction = READ; direction <= WRITE; direction++) {
		struct channel *ch = privptr->channel[direction];
		fsm_event(ch->fsm, CH_EVENT_START, ch);
	}
}

/**
 * Shutdown channels by sending CH_EVENT_STOP to each channel.
 *
 * @param fi    An instance of an interface statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from struct net_device * upon call.
 */
static void
dev_action_stop(fsm_instance * fi, int event, void *arg)
{
	struct net_device *dev = (struct net_device *) arg;
	struct ctc_priv *privptr = dev->priv;
	int direction;

	fsm_newstate(fi, DEV_STATE_STOPWAIT_RXTX);
	for (direction = READ; direction <= WRITE; direction++) {
		struct channel *ch = privptr->channel[direction];
		fsm_event(ch->fsm, CH_EVENT_STOP, ch);
	}
}
static void 
dev_action_restart(fsm_instance *fi, int event, void *arg)
{
	struct net_device *dev = (struct net_device *)arg;
	struct ctc_priv *privptr = dev->priv;
	
	printk(KERN_DEBUG "%s: Restarting\n", dev->name);
	dev_action_stop(fi, event, arg);
	fsm_event(privptr->fsm, DEV_EVENT_STOP, dev);
	fsm_addtimer(&privptr->restart_timer, CTC_TIMEOUT_5SEC,
		     DEV_EVENT_START, dev);
}

/**
 * Called from channel statemachine
 * when a channel is up and running.
 *
 * @param fi    An instance of an interface statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from struct net_device * upon call.
 */
static void
dev_action_chup(fsm_instance * fi, int event, void *arg)
{
	struct net_device *dev = (struct net_device *) arg;
	struct ctc_priv *privptr = dev->priv;

	switch (fsm_getstate(fi)) {
	case DEV_STATE_STARTWAIT_RXTX:
		if (event == DEV_EVENT_RXUP)
			fsm_newstate(fi, DEV_STATE_STARTWAIT_TX);
		else
			fsm_newstate(fi, DEV_STATE_STARTWAIT_RX);
		break;
	case DEV_STATE_STARTWAIT_RX:
		if (event == DEV_EVENT_RXUP) {
			fsm_newstate(fi, DEV_STATE_RUNNING);
			printk(KERN_INFO
			       "%s: connected with remote side\n", dev->name);
			if (privptr->protocol == CTC_PROTO_LINUX_TTY)
				ctc_tty_setcarrier(dev, 1);
			ctc_clear_busy(dev);
		}
		break;
	case DEV_STATE_STARTWAIT_TX:
		if (event == DEV_EVENT_TXUP) {
			fsm_newstate(fi, DEV_STATE_RUNNING);
			printk(KERN_INFO
			       "%s: connected with remote side\n", dev->name);
			if (privptr->protocol == CTC_PROTO_LINUX_TTY)
				ctc_tty_setcarrier(dev, 1);
			ctc_clear_busy(dev);
		}
		break;
	case DEV_STATE_STOPWAIT_TX:
		if (event == DEV_EVENT_RXUP)
			fsm_newstate(fi, DEV_STATE_STOPWAIT_RXTX);
		break;
	case DEV_STATE_STOPWAIT_RX:
		if (event == DEV_EVENT_TXUP)
			fsm_newstate(fi, DEV_STATE_STOPWAIT_RXTX);
		break;
	}
}

/**
 * Called from channel statemachine
 * when a channel has been shutdown.
 *
 * @param fi    An instance of an interface statemachine.
 * @param event The event, just happened.
 * @param arg   Generic pointer, casted from struct net_device * upon call.
 */
static void
dev_action_chdown(fsm_instance * fi, int event, void *arg)
{
	struct net_device *dev = (struct net_device *) arg;
	struct ctc_priv *privptr = dev->priv;

	switch (fsm_getstate(fi)) {
	case DEV_STATE_RUNNING:
		if (privptr->protocol == CTC_PROTO_LINUX_TTY)
			ctc_tty_setcarrier(dev, 0);
		if (event == DEV_EVENT_TXDOWN)
			fsm_newstate(fi, DEV_STATE_STARTWAIT_TX);
		else
			fsm_newstate(fi, DEV_STATE_STARTWAIT_RX);
		break;
	case DEV_STATE_STARTWAIT_RX:
		if (event == DEV_EVENT_TXDOWN)
			fsm_newstate(fi, DEV_STATE_STARTWAIT_RXTX);
		break;
	case DEV_STATE_STARTWAIT_TX:
		if (event == DEV_EVENT_RXDOWN)
			fsm_newstate(fi, DEV_STATE_STARTWAIT_RXTX);
		break;
	case DEV_STATE_STOPWAIT_RXTX:
		if (event == DEV_EVENT_TXDOWN)
			fsm_newstate(fi, DEV_STATE_STOPWAIT_RX);
		else
			fsm_newstate(fi, DEV_STATE_STOPWAIT_TX);
		break;
	case DEV_STATE_STOPWAIT_RX:
		if (event == DEV_EVENT_RXDOWN)
			fsm_newstate(fi, DEV_STATE_STOPPED);
		break;
	case DEV_STATE_STOPWAIT_TX:
		if (event == DEV_EVENT_TXDOWN)
			fsm_newstate(fi, DEV_STATE_STOPPED);
		break;
	}
}

static const fsm_node dev_fsm[] = {
	{DEV_STATE_STOPPED, DEV_EVENT_START, dev_action_start},

	{DEV_STATE_STOPWAIT_RXTX, DEV_EVENT_START, dev_action_start},
	{DEV_STATE_STOPWAIT_RXTX, DEV_EVENT_RXDOWN, dev_action_chdown},
	{DEV_STATE_STOPWAIT_RXTX, DEV_EVENT_TXDOWN, dev_action_chdown},
 	{DEV_STATE_STOPWAIT_RXTX, DEV_EVENT_RESTART, dev_action_restart },

	{DEV_STATE_STOPWAIT_RX, DEV_EVENT_START, dev_action_start},
	{DEV_STATE_STOPWAIT_RX, DEV_EVENT_RXUP, dev_action_chup},
	{DEV_STATE_STOPWAIT_RX, DEV_EVENT_TXUP, dev_action_chup},
	{DEV_STATE_STOPWAIT_RX, DEV_EVENT_RXDOWN, dev_action_chdown},
 	{DEV_STATE_STOPWAIT_RX, DEV_EVENT_RESTART, dev_action_restart },

	{DEV_STATE_STOPWAIT_TX, DEV_EVENT_START, dev_action_start},
	{DEV_STATE_STOPWAIT_TX, DEV_EVENT_RXUP, dev_action_chup},
	{DEV_STATE_STOPWAIT_TX, DEV_EVENT_TXUP, dev_action_chup},
	{DEV_STATE_STOPWAIT_TX, DEV_EVENT_TXDOWN, dev_action_chdown},
 	{DEV_STATE_STOPWAIT_TX, DEV_EVENT_RESTART, dev_action_restart },

	{DEV_STATE_STARTWAIT_RXTX, DEV_EVENT_STOP, dev_action_stop},
	{DEV_STATE_STARTWAIT_RXTX, DEV_EVENT_RXUP, dev_action_chup},
	{DEV_STATE_STARTWAIT_RXTX, DEV_EVENT_TXUP, dev_action_chup},
	{DEV_STATE_STARTWAIT_RXTX, DEV_EVENT_RXDOWN, dev_action_chdown},
	{DEV_STATE_STARTWAIT_RXTX, DEV_EVENT_TXDOWN, dev_action_chdown},
 	{DEV_STATE_STARTWAIT_RXTX, DEV_EVENT_RESTART, dev_action_restart },

	{DEV_STATE_STARTWAIT_TX, DEV_EVENT_STOP, dev_action_stop},
	{DEV_STATE_STARTWAIT_TX, DEV_EVENT_RXUP, dev_action_chup},
	{DEV_STATE_STARTWAIT_TX, DEV_EVENT_TXUP, dev_action_chup},
	{DEV_STATE_STARTWAIT_TX, DEV_EVENT_RXDOWN, dev_action_chdown},
 	{DEV_STATE_STARTWAIT_TX, DEV_EVENT_RESTART, dev_action_restart },

	{DEV_STATE_STARTWAIT_RX, DEV_EVENT_STOP, dev_action_stop},
	{DEV_STATE_STARTWAIT_RX, DEV_EVENT_RXUP, dev_action_chup},
	{DEV_STATE_STARTWAIT_RX, DEV_EVENT_TXUP, dev_action_chup},
	{DEV_STATE_STARTWAIT_RX, DEV_EVENT_TXDOWN, dev_action_chdown},
 	{DEV_STATE_STARTWAIT_RX, DEV_EVENT_RESTART, dev_action_restart },

	{DEV_STATE_RUNNING, DEV_EVENT_STOP, dev_action_stop},
	{DEV_STATE_RUNNING, DEV_EVENT_RXDOWN, dev_action_chdown},
	{DEV_STATE_RUNNING, DEV_EVENT_TXDOWN, dev_action_chdown},
	{DEV_STATE_RUNNING, DEV_EVENT_TXUP, fsm_action_nop},
	{DEV_STATE_RUNNING, DEV_EVENT_RXUP, fsm_action_nop},
 	{DEV_STATE_RUNNING, DEV_EVENT_RESTART, dev_action_restart },
};

static const int DEV_FSM_LEN = sizeof (dev_fsm) / sizeof (fsm_node);

/**
 * Transmit a packet.
 * This is a helper function for ctc_tx().
 *
 * @param ch Channel to be used for sending.
 * @param skb Pointer to struct sk_buff of packet to send.
 *            The linklevel header has already been set up
 *            by ctc_tx().
 *
 * @return 0 on success, -ERRNO on failure. (Never fails.)
 */
static int
transmit_skb(struct channel *ch, struct sk_buff *skb)
{
	unsigned long saveflags;
	struct ll_header header;
	int rc = 0;

	if (fsm_getstate(ch->fsm) != CH_STATE_TXIDLE) {
		int l = skb->len + LL_HEADER_LENGTH;

		spin_lock_irqsave(&ch->collect_lock, saveflags);
		if (ch->collect_len + l > ch->max_bufsize - 2)
			rc = -EBUSY;
		else {
			atomic_inc(&skb->users);
			header.length = l;
			header.type = skb->protocol;
			header.unused = 0;
			memcpy(skb_push(skb, LL_HEADER_LENGTH), &header,
			       LL_HEADER_LENGTH);
			skb_queue_tail(&ch->collect_queue, skb);
			ch->collect_len += l;
		}
		spin_unlock_irqrestore(&ch->collect_lock, saveflags);
	} else {
		__u16 block_len;
		int ccw_idx;
		struct sk_buff *nskb;
		unsigned long hi;

		/**
		 * Protect skb against beeing free'd by upper
		 * layers.
		 */
		atomic_inc(&skb->users);
		ch->prof.txlen += skb->len;
		header.length = skb->len + LL_HEADER_LENGTH;
		header.type = skb->protocol;
		header.unused = 0;
		memcpy(skb_push(skb, LL_HEADER_LENGTH), &header,
		       LL_HEADER_LENGTH);
		block_len = skb->len + 2;
		*((__u16 *) skb_push(skb, 2)) = block_len;

		/**
		 * IDAL support in CTC is broken, so we have to
		 * care about skb's above 2G ourselves.
		 */
		hi = ((unsigned long) skb->tail + LL_HEADER_LENGTH) >> 31;
		if (hi) {
			nskb = alloc_skb(skb->len, GFP_ATOMIC | GFP_DMA);
			if (!nskb) {
				atomic_dec(&skb->users);
				skb_pull(skb, LL_HEADER_LENGTH + 2);
				return -ENOMEM;
			} else {
				memcpy(skb_put(nskb, skb->len),
				       skb->data, skb->len);
				atomic_inc(&nskb->users);
				atomic_dec(&skb->users);
				dev_kfree_skb_irq(skb);
				skb = nskb;
			}
		}

		ch->ccw[4].count = block_len;
		if (set_normalized_cda(&ch->ccw[4], skb->data)) {
			/**
			 * idal allocation failed, try via copying to
			 * trans_skb. trans_skb usually has a pre-allocated
			 * idal.
			 */
			if (ctc_checkalloc_buffer(ch, 1)) {
				/**
				 * Remove our header. It gets added
				 * again on retransmit.
				 */
				atomic_dec(&skb->users);
				skb_pull(skb, LL_HEADER_LENGTH + 2);
				return -EBUSY;
			}

			ch->trans_skb->tail = ch->trans_skb->data;
			ch->trans_skb->len = 0;
			ch->ccw[1].count = skb->len;
			memcpy(skb_put(ch->trans_skb, skb->len), skb->data,
			       skb->len);
			atomic_dec(&skb->users);
			dev_kfree_skb_irq(skb);
			ccw_idx = 0;
		} else {
			skb_queue_tail(&ch->io_queue, skb);
			ccw_idx = 3;
		}
		ch->retry = 0;
		fsm_newstate(ch->fsm, CH_STATE_TX);
		fsm_addtimer(&ch->timer, CTC_TIMEOUT_5SEC, CH_EVENT_TIMER, ch);
		spin_lock_irqsave(get_ccwdev_lock(ch->cdev), saveflags);
		ch->prof.send_stamp = xtime;
		rc = ccw_device_start(ch->cdev, &ch->ccw[ccw_idx],
				      (unsigned long) ch, 0xff, 0);
		spin_unlock_irqrestore(get_ccwdev_lock(ch->cdev), saveflags);
		if (ccw_idx == 3)
			ch->prof.doios_single++;
		if (rc != 0) {
			fsm_deltimer(&ch->timer);
			ccw_check_return_code(ch, rc);
			if (ccw_idx == 3)
				skb_dequeue_tail(&ch->io_queue);
			/**
			 * Remove our header. It gets added
			 * again on retransmit.
			 */
			skb_pull(skb, LL_HEADER_LENGTH + 2);
		} else {
			if (ccw_idx == 0) {
				struct net_device *dev = ch->netdev;
				struct ctc_priv *privptr = dev->priv;
				privptr->stats.tx_packets++;
				privptr->stats.tx_bytes +=
				    skb->len - LL_HEADER_LENGTH;
			}
		}
	}

	return rc;
}

/**
 * Interface API for upper network layers
 *****************************************************************************/

/**
 * Open an interface.
 * Called from generic network layer when ifconfig up is run.
 *
 * @param dev Pointer to interface struct.
 *
 * @return 0 on success, -ERRNO on failure. (Never fails.)
 */
static int
ctc_open(struct net_device * dev)
{
	MOD_INC_USE_COUNT;
	fsm_event(((struct ctc_priv *) dev->priv)->fsm, DEV_EVENT_START, dev);
	return 0;
}

/**
 * Close an interface.
 * Called from generic network layer when ifconfig down is run.
 *
 * @param dev Pointer to interface struct.
 *
 * @return 0 on success, -ERRNO on failure. (Never fails.)
 */
static int
ctc_close(struct net_device * dev)
{
	fsm_event(((struct ctc_priv *) dev->priv)->fsm, DEV_EVENT_STOP, dev);
	MOD_DEC_USE_COUNT;
	return 0;
}

/**
 * Start transmission of a packet.
 * Called from generic network device layer.
 *
 * @param skb Pointer to buffer containing the packet.
 * @param dev Pointer to interface struct.
 *
 * @return 0 if packet consumed, !0 if packet rejected.
 *         Note: If we return !0, then the packet is free'd by
 *               the generic network layer.
 */
static int
ctc_tx(struct sk_buff *skb, struct net_device * dev)
{
	int rc = 0;
	struct ctc_priv *privptr = (struct ctc_priv *) dev->priv;

	/**
	 * Some sanity checks ...
	 */
	if (skb == NULL) {
		printk(KERN_WARNING "%s: NULL sk_buff passed\n", dev->name);
		privptr->stats.tx_dropped++;
		return 0;
	}
	if (skb_headroom(skb) < (LL_HEADER_LENGTH + 2)) {
		printk(KERN_WARNING
		       "%s: Got sk_buff with head room < %ld bytes\n",
		       dev->name, LL_HEADER_LENGTH + 2);
		dev_kfree_skb(skb);
		privptr->stats.tx_dropped++;
		return 0;
	}

	/**
	 * If channels are not running, try to restart them
	 * notify anybody about a link failure and throw
	 * away packet. 
	 */
	if (fsm_getstate(privptr->fsm) != DEV_STATE_RUNNING) {
		fsm_event(privptr->fsm, DEV_EVENT_START, dev);
		if (privptr->protocol == CTC_PROTO_LINUX_TTY)
			return -EBUSY;
		dst_link_failure(skb);
		dev_kfree_skb(skb);
		privptr->stats.tx_dropped++;
		privptr->stats.tx_errors++;
		privptr->stats.tx_carrier_errors++;
		return 0;
	}

	if (ctc_test_and_set_busy(dev))
		return -EBUSY;

	dev->trans_start = jiffies;
	if (transmit_skb(privptr->channel[WRITE], skb) != 0)
		rc = 1;
	ctc_clear_busy(dev);
	return rc;
}

/**
 * Sets MTU of an interface.
 *
 * @param dev     Pointer to interface struct.
 * @param new_mtu The new MTU to use for this interface.
 *
 * @return 0 on success, -EINVAL if MTU is out of valid range.
 *         (valid range is 576 .. 65527). If VM is on the
 *         remote side, maximum MTU is 32760, however this is
 *         <em>not</em> checked here.
 */
static int
ctc_change_mtu(struct net_device * dev, int new_mtu)
{
	struct ctc_priv *privptr = (struct ctc_priv *) dev->priv;

	if ((new_mtu < 576) || (new_mtu > 65527) ||
	    (new_mtu > (privptr->channel[READ]->max_bufsize -
			LL_HEADER_LENGTH - 2)))
		return -EINVAL;
	dev->mtu = new_mtu;
	dev->hard_header_len = LL_HEADER_LENGTH + 2;
	return 0;
}

/**
 * Returns interface statistics of a device.
 *
 * @param dev Pointer to interface struct.
 *
 * @return Pointer to stats struct of this interface.
 */
static struct net_device_stats *
ctc_stats(struct net_device * dev)
{
	return &((struct ctc_priv *) dev->priv)->stats;
}

/*
 * sysfs attributes
 */
#define CTRL_BUFSIZE 40

static ssize_t
buffer_show(struct device *dev, char *buf)
{
	struct ctc_priv *priv;

	priv = dev->driver_data;
	if (!priv)
		return -ENODEV;
	return sprintf(buf, "%d\n",
		       priv->channel[READ]->max_bufsize);
}

static ssize_t
buffer_write(struct device *dev, const char *buf, size_t count)
{
	struct ctc_priv *priv;
	struct net_device *ndev;
	int bs1;

	priv = dev->driver_data;
	if (!priv)
		return -ENODEV;
	ndev = priv->channel[READ]->netdev;
	if (!ndev)
		return -ENODEV;
	sscanf(buf, "%u", &bs1);

	if (bs1 > CTC_BUFSIZE_LIMIT)
		return -EINVAL;
	if ((ndev->flags & IFF_RUNNING) &&
	    (bs1 < (ndev->mtu + LL_HEADER_LENGTH + 2)))
		return -EINVAL;
	if (bs1 < (576 + LL_HEADER_LENGTH + 2))
		return -EINVAL;

	priv->channel[READ]->max_bufsize =
	    priv->channel[WRITE]->max_bufsize = bs1;
	if (!(ndev->flags & IFF_RUNNING))
		ndev->mtu = bs1 - LL_HEADER_LENGTH - 2;
	priv->channel[READ]->flags |= CHANNEL_FLAGS_BUFSIZE_CHANGED;
	priv->channel[WRITE]->flags |= CHANNEL_FLAGS_BUFSIZE_CHANGED;

	return count;

}

static DEVICE_ATTR(buffer, 0644, buffer_show, buffer_write);

static int
ctc_add_attributes(struct device *dev)
{
	return device_create_file(dev, &dev_attr_buffer);

}

static void
ctc_remove_attributes(struct device *dev)
{
	device_remove_file(dev, &dev_attr_buffer);

}

#if 0
/* FIXME: This has to be converted to another interface, as we can only have one
 *        value per file and can't have atomicity then */
#define STATS_BUFSIZE 2048

static int
ctc_stat_open(struct inode *inode, struct file *file)
{
	file->private_data = kmalloc(STATS_BUFSIZE, GFP_KERNEL);
	if (file->private_data == NULL)
		return -ENOMEM;
	MOD_INC_USE_COUNT;
	return 0;
}

static int
ctc_stat_close(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	MOD_DEC_USE_COUNT;
	return 0;
}

static ssize_t
ctc_stat_write(struct file *file, const char *buf, size_t count, loff_t * off)
{
	struct proc_dir_entry *pde = PDE(file->f_dentry->d_inode);
	struct net_device *dev;
	struct ctc_priv *privptr;

	if (!(dev = find_netdev_by_ino(pde)))
		return -ENODEV;
	privptr = (struct ctc_priv *) dev->priv;
	privptr->channel[WRITE]->prof.maxmulti = 0;
	privptr->channel[WRITE]->prof.maxcqueue = 0;
	privptr->channel[WRITE]->prof.doios_single = 0;
	privptr->channel[WRITE]->prof.doios_multi = 0;
	privptr->channel[WRITE]->prof.txlen = 0;
	privptr->channel[WRITE]->prof.tx_time = 0;
	return count;
}

static ssize_t
ctc_stat_read(struct file *file, char *buf, size_t count, loff_t * off)
{
	struct proc_dir_entry *pde = PDE(file->f_dentry->d_inode);
	char *sbuf = (char *) file->private_data;
	struct net_device *dev;
	struct ctc_priv *privptr;
	ssize_t ret = 0;
	char *p = sbuf;
	int l;

	if (!(dev = find_netdev_by_ino(pde)))
		return -ENODEV;
	if (off != &file->f_pos)
		return -ESPIPE;

	privptr = (struct ctc_priv *) dev->priv;

	if (file->f_pos == 0) {
		p += sprintf(p, "Device FSM state: %s\n",
			     fsm_getstate_str(privptr->fsm));
		p += sprintf(p, "RX channel FSM state: %s\n",
			     fsm_getstate_str(privptr->channel[READ]->fsm));
		p += sprintf(p, "TX channel FSM state: %s\n",
			     fsm_getstate_str(privptr->channel[WRITE]->fsm));
		p += sprintf(p, "Max. TX buffer used: %ld\n",
			     privptr->channel[WRITE]->prof.maxmulti);
		p += sprintf(p, "Max. chained SKBs: %ld\n",
			     privptr->channel[WRITE]->prof.maxcqueue);
		p += sprintf(p, "TX single write ops: %ld\n",
			     privptr->channel[WRITE]->prof.doios_single);
		p += sprintf(p, "TX multi write ops: %ld\n",
			     privptr->channel[WRITE]->prof.doios_multi);
		p += sprintf(p, "Netto bytes written: %ld\n",
			     privptr->channel[WRITE]->prof.txlen);
		p += sprintf(p, "Max. TX IO-time: %ld\n",
			     privptr->channel[WRITE]->prof.tx_time);
	}
	l = strlen(sbuf);
	p = sbuf;
	if (file->f_pos < l) {
		p += file->f_pos;
		l = strlen(p);
		ret = (count > l) ? l : count;
		if (copy_to_user(buf, p, ret))
			return -EFAULT;
	}
	file->f_pos += ret;
	return ret;
}

static struct file_operations ctc_stat_fops = {
	.read    = ctc_stat_read,
	.write   = ctc_stat_write,
	.open    = ctc_stat_open,
	.release = ctc_stat_close,
};
#endif

static void
ctc_netdev_unregister(struct net_device * dev)
{
	struct ctc_priv *privptr;

	if (!dev)
		return;
	privptr = (struct ctc_priv *) dev->priv;
	if (privptr->protocol != CTC_PROTO_LINUX_TTY)
		unregister_netdev(dev);
	else
		ctc_tty_unregister_netdev(dev);
}

static int
ctc_netdev_register(struct net_device * dev)
{
	struct ctc_priv *privptr = (struct ctc_priv *) dev->priv;
	if (privptr->protocol != CTC_PROTO_LINUX_TTY)
		return register_netdev(dev);
	else
		return ctc_tty_register_netdev(dev);
}

static void
ctc_free_netdevice(struct net_device * dev, int free_dev)
{
	struct ctc_priv *privptr;
	if (!dev)
		return;
	privptr = dev->priv;
	if (privptr) {
		if (privptr->fsm)
			kfree_fsm(privptr->fsm);
		kfree(privptr);
	}
#ifdef MODULE
	if (free_dev)
		kfree(dev);
#endif
}

/**
 * Initialize everything of the net device except the name and the
 * channel structs.
 */
static struct net_device *
ctc_init_netdevice(struct net_device * dev, int alloc_device, 
		   struct ctc_priv *privptr)
{
	if (!privptr)
		return NULL;

	if (alloc_device) {
		dev = kmalloc(sizeof (struct net_device), GFP_KERNEL);
		if (!dev)
			return NULL;
		memset(dev, 0, sizeof (struct net_device));
	}

	dev->priv = privptr;
	privptr->fsm = init_fsm("ctcdev", dev_state_names,
				dev_event_names, NR_DEV_STATES, NR_DEV_EVENTS,
				dev_fsm, DEV_FSM_LEN, GFP_KERNEL);
	if (privptr->fsm == NULL) {
		if (alloc_device)
			kfree(dev);
		return NULL;
	}
	fsm_newstate(privptr->fsm, DEV_STATE_STOPPED);
	fsm_settimer(privptr->fsm, &privptr->restart_timer);
	dev->mtu = CTC_BUFSIZE_DEFAULT - LL_HEADER_LENGTH - 2;
	dev->hard_start_xmit = ctc_tx;
	dev->open = ctc_open;
	dev->stop = ctc_close;
	dev->get_stats = ctc_stats;
	dev->change_mtu = ctc_change_mtu;
	dev->hard_header_len = LL_HEADER_LENGTH + 2;
	dev->addr_len = 0;
	dev->type = ARPHRD_SLIP;
	dev->tx_queue_len = 100;
	dev->flags = IFF_POINTOPOINT | IFF_NOARP;
	return dev;
}

static ssize_t
ctc_proto_show(struct device *dev, char *buf)
{
	struct ctc_priv *priv;

	priv = dev->driver_data;
	if (!priv)
		return -ENODEV;

	return sprintf(buf, "%d\n", priv->protocol);
}

static ssize_t
ctc_proto_store(struct device *dev, const char *buf, size_t count)
{
	struct ctc_priv *priv;
	int value;

	priv = dev->driver_data;
	if (!priv)
		return -ENODEV;
	sscanf(buf, "%u", &value);
	/* TODO: sanity checks */
	priv->protocol = value;

	return count;
}

static DEVICE_ATTR(protocol, 0644, ctc_proto_show, ctc_proto_store);

static int
ctc_add_files(struct device *dev)
{
	return device_create_file(dev, &dev_attr_protocol);

}

static void
ctc_remove_files(struct device *dev)
{
	device_remove_file(dev, &dev_attr_protocol);

}

/**
 * Add ctc specific attributes.
 * Add ctc private data.
 * 
 * @param cgdev pointer to ccwgroup_device just added
 *
 * @returns 0 on success, !0 on failure.
 */

static int
ctc_probe_device(struct ccwgroup_device *cgdev)
{
	struct ctc_priv *priv;
	int rc;

	if (!get_device(&cgdev->dev))
		return -ENODEV;

	priv = kmalloc(sizeof (struct ctc_priv), GFP_KERNEL);
	if (!priv) {
		printk(KERN_ERR "%s: Out of memory\n", __func__);
		put_device(&cgdev->dev);
		return -ENOMEM;
	}

	memset(priv, 0, sizeof (struct ctc_priv));
	rc = ctc_add_files(&cgdev->dev);
	if (rc) {
		kfree(priv);
		put_device(&cgdev->dev);
		return rc;
	}

	cgdev->cdev[0]->handler = ctc_irq_handler;
	cgdev->cdev[1]->handler = ctc_irq_handler;
	cgdev->dev.driver_data = priv;
	cgdev->cdev[0]->dev.driver_data = priv;
	cgdev->cdev[1]->dev.driver_data = priv;
	snprintf(cgdev->dev.name, DEVICE_NAME_SIZE, "%s",
		 cu3088_type[cgdev->cdev[0]->id.driver_info]);

	return 0;
}

/**
 *
 * Setup an interface.
 *
 * @param cgdev  Device to be setup.
 *
 * @returns 0 on success, !0 on failure.
 */
static int
ctc_new_device(struct ccwgroup_device *cgdev)
{
	char read_id[CTC_ID_SIZE];
	char write_id[CTC_ID_SIZE];
	int direction;
	enum channel_types type;
	struct ctc_priv *privptr;
	struct net_device *dev;

	privptr = cgdev->dev.driver_data;
	if (!privptr)
		return -ENODEV;

	type = get_channel_type(&cgdev->cdev[0]->id);
	
	snprintf(read_id, DEVICE_ID_SIZE, "ch-%s", cgdev->cdev[0]->dev.bus_id);
	snprintf(write_id, DEVICE_ID_SIZE, "ch-%s", cgdev->cdev[1]->dev.bus_id);

	if (add_channel(cgdev->cdev[0], type))
		return -ENOMEM;
	if (add_channel(cgdev->cdev[1], type))
		return -ENOMEM;

	ccw_device_set_online(cgdev->cdev[0]);
	ccw_device_set_online(cgdev->cdev[1]);	

	dev = ctc_init_netdevice(NULL, 1, privptr);

	if (!dev) {
		printk(KERN_WARNING "ctc_init_netdevice failed\n");
		goto out;
	}

	if (privptr->protocol == CTC_PROTO_LINUX_TTY)
		snprintf(dev->name, 8, "ctctty%%d");
	else
		snprintf(dev->name, 8, "ctc%%d");

	for (direction = READ; direction <= WRITE; direction++) {
		privptr->channel[direction] =
		    channel_get(type, direction == READ ? read_id : write_id,
				direction);
		if (privptr->channel[direction] == NULL) {
			if (direction == WRITE) 
				channel_free(privptr->channel[READ]);

			ctc_free_netdevice(dev, 1);
			goto out;
		}
		privptr->channel[direction]->netdev = dev;
		privptr->channel[direction]->protocol = privptr->protocol;
		privptr->channel[direction]->max_bufsize = CTC_BUFSIZE_DEFAULT;
	}
	if (ctc_netdev_register(dev) != 0) {
		ctc_free_netdevice(dev, 1);
		goto out;
	}

	ctc_add_attributes(&cgdev->dev);

	strncpy(privptr->fsm->name, dev->name, sizeof (privptr->fsm->name));

	print_banner();

	printk(KERN_INFO
	       "%s: read: %s, write: %s, proto: %d\n",
	       dev->name, privptr->channel[READ]->id,
	       privptr->channel[WRITE]->id, privptr->protocol);

	return 0;
out:
	ccw_device_set_offline(cgdev->cdev[1]);
	ccw_device_set_offline(cgdev->cdev[0]);

	return -ENODEV;
}

/**
 * Shutdown an interface.
 *
 * @param cgdev  Device to be shut down.
 *
 * @returns 0 on success, !0 on failure.
 */
static int
ctc_shutdown_device(struct ccwgroup_device *cgdev)
{
	struct ctc_priv *priv;
	struct net_device *ndev;
		

	priv = cgdev->dev.driver_data;
	if (!priv)
		return -ENODEV;
	ndev = priv->channel[READ]->netdev;

	/* Close the device */
	ctc_close(ndev);
	ndev->flags &=~IFF_RUNNING;

	ctc_remove_attributes(&cgdev->dev);

	channel_free(priv->channel[READ]);
	channel_free(priv->channel[WRITE]);

	ctc_netdev_unregister(ndev);
	ndev->priv = NULL;
	ctc_free_netdevice(ndev, 1);

	kfree_fsm(priv->fsm);

	ccw_device_set_offline(cgdev->cdev[1]);
	ccw_device_set_offline(cgdev->cdev[0]);

	channel_remove(priv->channel[READ]);
	channel_remove(priv->channel[WRITE]);
	
	return 0;

}

static int
ctc_remove_device(struct ccwgroup_device *cgdev)
{
	struct ctc_priv *priv;

	priv = cgdev->dev.driver_data;
	if (!priv)
		return 0;

	ctc_remove_files(&cgdev->dev);
	cgdev->dev.driver_data = NULL;
	kfree(priv);
	put_device(&cgdev->dev);
	return 0;
}

static struct ccwgroup_driver ctc_group_driver = {
	.name        = "ctc",
	.max_slaves  = 2,
	.driver_id   = 0xC3E3C3,
	.probe       = ctc_probe_device,
	.remove      = ctc_remove_device,
	.set_online  = ctc_new_device,
	.set_offline = ctc_shutdown_device,
};

/**
 * Module related routines
 *****************************************************************************/

/**
 * Prepare to be unloaded. Free IRQ's and release all resources.
 * This is called just before this module is unloaded. It is
 * <em>not</em> called, if the usage count is !0, so we don't need to check
 * for that.
 */
static void __exit
ctc_exit(void)
{
	unregister_cu3088_discipline(&ctc_group_driver);
	ctc_tty_cleanup();
	printk(KERN_INFO "CTC driver unloaded\n");
}

/**
 * Initialize module.
 * This is called just after the module is loaded.
 *
 * @return 0 on success, !0 on error.
 */
static int __init
ctc_init(void)
{
	int ret = 0;

	print_banner();

	ctc_tty_init();
	ret = register_cu3088_discipline(&ctc_group_driver);
	if (ret) 
		ctc_tty_cleanup();
	return ret;
}

module_init(ctc_init);
module_exit(ctc_exit);

/* --- This is the END my friend --- */
