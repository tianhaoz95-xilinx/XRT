/*
 * A GEM style device manager for PCIe based OpenCL accelerators.
 *
 * Copyright (C) 2016-2018 Xilinx, Inc. All rights reserved.
 *
 * Authors: Max Zhen <maxz@xilinx.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * Statement of Theory
 *
 * This is the mailbox sub-device driver added into existing xclmgmt / xocl
 * driver so that user pf and mgmt pf can send and receive messages of
 * arbitrary length to / from peer. The driver is written based on the spec of
 * pg114 document (https://www.xilinx.com/support/documentation/
 * ip_documentation/mailbox/v2_1/pg114-mailbox.pdf). The HW provides one TX
 * channel and one RX channel, which operate completely independent of each
 * other. Data can be pushed into or read from a channel in DWORD unit as a
 * FIFO.
 *
 *
 * Packet layer
 *
 * The driver implemented two transport layers - packet and message layer (see
 * below). A packet is a fixed size chunk of data that can be send through TX
 * channel or retrieved from RX channel. The TX and RX interrupt happens at
 * packet boundary, instead of DWORD boundary. The driver will not attempt to
 * send next packet until the previous one is read by peer. Similarly, the
 * driver will not attempt to read the data from HW until a full packet has been
 * written to HW by peer. No polling is implemented. Data transfer is entirely
 * interrupt driven. So, the interrupt functionality needs to work and enabled
 * on both mgmt and user pf for mailbox driver to function properly.
 *
 * A TX packet is considered as time'd out after sitting in the TX channel of
 * mailbox HW for two packet ticks (1 packet tick = 1 second, for now) without
 * being read by peer. Currently, the driver will not try to re-transmit the
 * packet after timeout. It just simply propagate the error to the upper layer.
 * A retry at packet layer can be implement later, if considered as appropriate.
 *
 *
 * Message layer
 *
 * A message is a data buffer of arbitrary length. The driver will break a
 * message into multiple packets and transmit them to the peer, which, in turn,
 * will assemble them into a full message before it's delivered to upper layer
 * for further processing. One message requires at least one packet to be
 * transferred to the peer.
 *
 * Each message has a unique temporary u64 ID (see communication model below
 * for more detail). The ID shows up in each packet's header. So, at packet
 * layer, there is no assumption that adjacent packets belong to the same
 * message. However, for the sake of simplicity, at message layer, the driver
 * will not attempt to send the next message until the sending of current one
 * is finished. I.E., we implement a FIFO for message TX channel. All messages
 * are sent by driver in the order of received from upper layer. We can
 * implement messages of different priority later, if needed. There is no
 * certain order for receiving messages. It's up to the peer side to decide
 * which message gets enqueued into its own TX queue first, which will be
 * received first on the other side.
 *
 * A message is considered as time'd out when it's transmit (send or receive)
 * is not finished within 10 packet ticks. This applies to all messages queued
 * up on both RX and TX channels. Again, no retry for a time'd out message is
 * implemented. The error will be simply passed to upper layer. Also, a TX
 * message may time out earlier if it's being transmitted and one of it's
 * packets time'd out. During normal operation, timeout should never happen.
 *
 * The upper layer can choose to queue a message for TX or RX asynchronously
 * when it provides a callback or wait synchronously when no callback is
 * provided.
 *
 *
 * Communication model
 *
 * At the highest layer, the driver implements a request-response communication
 * model. A request may or may not require a response, but a response must match
 * a request, or it'll be silently dropped. The driver provides a few kernel
 * APIs for mgmt and user pf to talk to each other in this model (see kernel
 * APIs section below for details). Each request or response is a message by
 * itself. A request message will automatically be assigned a message ID when
 * it's enqueued into TX channel for sending. If this request requires a
 * response, the buffer provided by caller for receiving response will be
 * enqueued into RX channel as well. The enqueued response message will have
 * the same message ID as the corresponding request message. The response
 * message, if provided, will always be enqueued before the request message is
 * enqueued to avoid race condition.
 *
 * The driver will automatically enqueue a special message into the RX channel
 * for receiving new request after initialized. This request RX message has a
 * special message ID (id=0) and never time'd out. When a new request comes
 * from peer, it'll be copied into request RX message then passed to the
 * callback provided by upper layer through xocl_peer_listen() API for further
 * processing. Currently, the driver implements only one kernel thread for RX
 * channel and one for TX channel. So, all message callback happens in the
 * context of that channel thread. So, the user of mailbox driver needs to be
 * careful when it calls xocl_peer_request() synchronously in this context.
 * You may see deadlock when both ends are trying to call xocl_peer_request()
 * synchronously at the same time.
 *
 *
 * +------------------+            +------------------+
 * | Request/Response | <--------> | Request/Response |
 * +------------------+            +------------------+
 * | Message          | <--------> | Message          |
 * +------------------+            +------------------+
 * | Packet           | <--------> | Packet           |
 * +------------------+            +------------------+
 * | RX/TX Channel    | <<======>> | RX/TX Channel    |
 * +------------------+            +------------------+
 *   mgmt pf                         user pf
 */

#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/poll.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include "../xocl_drv.h"
#include "mailbox_proto.h"

int mailbox_no_intr;
dev_t mailbox_dev;
module_param(mailbox_no_intr, int, (S_IRUGO|S_IWUSR));
MODULE_PARM_DESC(mailbox_no_intr,
	"Disable mailbox interrupt and do timer-driven msg passing");

#define	PACKET_SIZE	16 /* Number of DWORD. */

#define	FLAG_STI	(1 << 0)
#define	FLAG_RTI	(1 << 1)

#define	STATUS_EMPTY	(1 << 0)
#define	STATUS_FULL	(1 << 1)
#define	STATUS_STA	(1 << 2)
#define	STATUS_RTA	(1 << 3)

#define	MBX_ERR(mbx, fmt, arg...)	\
	xocl_err(&mbx->mbx_pdev->dev, fmt "\n", ##arg)
#define	MBX_INFO(mbx, fmt, arg...)	\
	xocl_info(&mbx->mbx_pdev->dev, fmt "\n", ##arg)
#define	MBX_DBG(mbx, fmt, arg...)	\
	xocl_dbg(&mbx->mbx_pdev->dev, fmt "\n", ##arg)

#define	MAILBOX_TIMER	(HZ / 5) /* in jiffies */
#define	MSG_RX_TTL	100UL	/* in MAILBOX_TIMER */
#define	MSG_TX_TTL	10UL	/* in MAILBOX_TIMER */
#define	MSG_MAX_TTL	0xFFFFFFFF
#define	TEST_MSG_LEN	128

#define	INVALID_MSG_ID		((u64)-1)

#define	MAX_MSG_QUEUE_SZ	(PAGE_SIZE << 16)
#define	MAX_MSG_QUEUE_LEN	5
#define	MAX_MSG_SZ		(PAGE_SIZE << 15)

#define	BYTE_TO_MB(x)		((x)>>20)

/*
 * Mailbox IP register layout
 */
struct mailbox_reg {
	u32			mbr_wrdata;
	u32			mbr_resv1;
	u32			mbr_rddata;
	u32			mbr_resv2;
	u32			mbr_status;
	u32			mbr_error;
	u32			mbr_sit;
	u32			mbr_rit;
	u32			mbr_is;
	u32			mbr_ie;
	u32			mbr_ip;
	u32			mbr_ctrl;
} __attribute__((packed));

/*
 * A message transport by mailbox.
 */
struct mailbox_msg {
	struct list_head	mbm_list;
	struct mailbox_channel	*mbm_ch;
	u64			mbm_req_id;
	char			*mbm_data;
	size_t			mbm_len;
	int			mbm_error;
	struct completion	mbm_complete;
	mailbox_msg_cb_t	mbm_cb;
	void			*mbm_cb_arg;
	u32			mbm_flags;
	u32			mbm_ttl;
	bool			mbm_chan_sw;
};

/*
 * A packet transport by mailbox.
 * When extending, only add new data structure to body. Choose to add new flag
 * if new feature can be safely ignored by peer, other wise, add new type.
 */
enum packet_type {
	PKT_INVALID = 0,
	PKT_TEST,
	PKT_MSG_START,
	PKT_MSG_BODY
};

/* Lower 8 bits for type, the rest for flags. */
#define	PKT_TYPE_MASK		0xff
#define	PKT_TYPE_MSG_END	(1 << 31)
struct mailbox_pkt {
	struct {
		u32		type;
		u32		payload_size;
	} hdr;
	union {
		u32		data[PACKET_SIZE - 2];
		struct {
			u64	msg_req_id;
			u32	msg_flags;
			u32	msg_size;
			u32	payload[0];
		} msg_start;
		struct {
			u32	payload[0];
		} msg_body;
	} body;
} __attribute__((packed));

/*
 * Mailbox communication channel.
 */
#define MBXCS_BIT_READY		0
#define MBXCS_BIT_STOP		1
#define MBXCS_BIT_TICK		2
#define MBXCS_BIT_POLL_MODE	3

struct mailbox_channel;
typedef	void (*chan_func_t)(struct mailbox_channel *ch);
struct mailbox_channel {
	struct mailbox		*mbc_parent;
	char			*mbc_name;

	struct workqueue_struct	*mbc_wq;
	struct work_struct	mbc_work;
	struct completion	mbc_worker;
	chan_func_t		mbc_tran;
	unsigned long		mbc_state;

	struct mutex		mbc_mutex;
	struct list_head	mbc_msgs;

	struct mailbox_msg	*mbc_cur_msg;
	int			mbc_bytes_done;
	struct mailbox_pkt	mbc_packet;

	struct timer_list	mbc_timer;
	bool			mbc_timer_on;

	/*
	 * Software channel settings
	 */
	wait_queue_head_t	sw_chan_wq;
	struct mutex		sw_chan_mutex;
	void			*sw_chan_buf;
	size_t			sw_chan_buf_sz;
	uint64_t		sw_chan_msg_id;
	uint64_t		sw_chan_msg_flags;

	atomic_t		trigger;
};

/*
 * The mailbox softstate.
 */
struct mailbox {
	struct platform_device	*mbx_pdev;
	struct mailbox_reg	*mbx_regs;
	struct cdev		*sys_cdev;
	struct device		*sys_device;
	u32			mbx_irq;

	struct mailbox_channel	mbx_rx;
	struct mailbox_channel	mbx_tx;

	/* For listening to peer's request. */
	mailbox_msg_cb_t	mbx_listen_cb;
	void			*mbx_listen_cb_arg;
	struct workqueue_struct	*mbx_listen_wq;
	struct work_struct	mbx_listen_worker;

	int			mbx_paired;
	/*
	 * For testing basic intr and mailbox comm functionality via sysfs.
	 * No locking protection, use with care.
	 */
	struct mailbox_pkt	mbx_tst_pkt;
	char			mbx_tst_tx_msg[TEST_MSG_LEN];
	char			mbx_tst_rx_msg[TEST_MSG_LEN];
	size_t			mbx_tst_tx_msg_len;

	/* Req list for all incoming request message */
	struct completion	mbx_comp;
	struct mutex		mbx_lock;
	struct list_head	mbx_req_list;
	uint8_t			mbx_req_cnt;
	size_t			mbx_req_sz;

	uint32_t		mbx_prot_ver;
	uint64_t		mbx_ch_state;
	uint64_t		mbx_ch_switch;
	char			mbx_comm_id[256];
	uint32_t		mbx_proto_ver;

	bool			mbx_peer_dead;
};

static inline const char *reg2name(struct mailbox *mbx, u32 *reg)
{
	const char *reg_names[] = {
		"wrdata",
		"reserved1",
		"rddata",
		"reserved2",
		"status",
		"error",
		"sit",
		"rit",
		"is",
		"ie",
		"ip",
		"ctrl"
	};

	return reg_names[((uintptr_t)reg -
		(uintptr_t)mbx->mbx_regs) / sizeof(u32)];
}

int mailbox_request(struct platform_device *, void *, size_t,
	void *, size_t *, mailbox_msg_cb_t, void *);
int mailbox_post_notify(struct platform_device *, void *, size_t);
int mailbox_get(struct platform_device *pdev, enum mb_kind kind, u64 *data);

static inline u32 mailbox_reg_rd(struct mailbox *mbx, u32 *reg)
{
	u32 val = ioread32(reg);

#ifdef	MAILBOX_REG_DEBUG
	MBX_DBG(mbx, "REG_RD(%s)=0x%x", reg2name(mbx, reg), val);
#endif
	return val;
}

static inline void mailbox_reg_wr(struct mailbox *mbx, u32 *reg, u32 val)
{
#ifdef	MAILBOX_REG_DEBUG
	MBX_DBG(mbx, "REG_WR(%s, 0x%x)", reg2name(mbx, reg), val);
#endif
	iowrite32(val, reg);
}

static inline void reset_pkt(struct mailbox_pkt *pkt)
{
	pkt->hdr.type = PKT_INVALID;
}

static inline bool valid_pkt(struct mailbox_pkt *pkt)
{
	return (pkt->hdr.type != PKT_INVALID);
}

irqreturn_t mailbox_isr(int irq, void *arg)
{
	struct mailbox *mbx = (struct mailbox *)arg;
	u32 is = mailbox_reg_rd(mbx, &mbx->mbx_regs->mbr_is);

	while (is) {
		MBX_DBG(mbx, "intr status: 0x%x", is);

		if ((is & FLAG_STI) != 0) {
			/* A packet has been sent successfully. */
			complete(&mbx->mbx_tx.mbc_worker);
		}
		if ((is & FLAG_RTI) != 0) {
			/* A packet is waiting to be received from mailbox. */
			complete(&mbx->mbx_rx.mbc_worker);
		}
		/* Anything else is not expected. */
		if ((is & (FLAG_STI | FLAG_RTI)) == 0) {
			MBX_ERR(mbx, "spurious mailbox irq %d, is=0x%x",
				irq, is);
		}

		/* Clear intr state for receiving next one. */
		mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_is, is);

		is = mailbox_reg_rd(mbx, &mbx->mbx_regs->mbr_is);
	}

	return IRQ_HANDLED;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
static void chan_timer(unsigned long data)
{
	struct mailbox_channel *ch = (struct mailbox_channel *)data;
#else
static void chan_timer(struct timer_list *t)
{
	struct mailbox_channel *ch = from_timer(ch, t, mbc_timer);
#endif

	MBX_DBG(ch->mbc_parent, "%s tick", ch->mbc_name);

	set_bit(MBXCS_BIT_TICK, &ch->mbc_state);
	complete(&ch->mbc_worker);

	/* We're a periodic timer. */
	mod_timer(&ch->mbc_timer, jiffies + MAILBOX_TIMER);
}

static void chan_config_timer(struct mailbox_channel *ch)
{
	struct list_head *pos, *n;
	struct mailbox_msg *msg = NULL;
	bool on = false;

	mutex_lock(&ch->mbc_mutex);

	if (test_bit(MBXCS_BIT_POLL_MODE, &ch->mbc_state)) {
		on = true;
	} else {
		list_for_each_safe(pos, n, &ch->mbc_msgs) {
			msg = list_entry(pos, struct mailbox_msg, mbm_list);
			if (msg->mbm_req_id == 0)
				continue;
			on = true;
			break;
		}
	}

	if (on != ch->mbc_timer_on) {
		ch->mbc_timer_on = on;
		if (on)
			mod_timer(&ch->mbc_timer, jiffies + MAILBOX_TIMER);
		else
			del_timer_sync(&ch->mbc_timer);
	}

	mutex_unlock(&ch->mbc_mutex);
}

static void free_msg(struct mailbox_msg *msg)
{
	vfree(msg);
}

static void msg_done(struct mailbox_msg *msg, int err)
{
	struct mailbox_channel *ch = msg->mbm_ch;
	struct mailbox *mbx = ch->mbc_parent;

	MBX_DBG(ch->mbc_parent, "%s finishing msg id=0x%llx err=%d",
		ch->mbc_name, msg->mbm_req_id, err);

	msg->mbm_error = err;

	if (msg->mbm_cb) {
		msg->mbm_cb(msg->mbm_cb_arg, msg->mbm_data, msg->mbm_len,
			msg->mbm_req_id, msg->mbm_error, msg->mbm_chan_sw);
		free_msg(msg);
		goto done;
	}

	if (msg->mbm_flags & MB_REQ_FLAG_RECV_REQ) {
		if ((mbx->mbx_req_sz+msg->mbm_len) >= MAX_MSG_QUEUE_SZ ||
			mbx->mbx_req_cnt >= MAX_MSG_QUEUE_LEN) {
			goto done;
		}
		mutex_lock(&ch->mbc_parent->mbx_lock);
		list_add_tail(&msg->mbm_list, &ch->mbc_parent->mbx_req_list);
		mbx->mbx_req_cnt++;
		mbx->mbx_req_sz += msg->mbm_len;
		mutex_unlock(&ch->mbc_parent->mbx_lock);
		complete(&ch->mbc_parent->mbx_comp);
	} else {
		complete(&msg->mbm_complete);
	}
done:
	chan_config_timer(ch);
}

static void chan_msg_done(struct mailbox_channel *ch, int err)
{
	if (!ch->mbc_cur_msg)
		return;

	msg_done(ch->mbc_cur_msg, err);
	ch->mbc_cur_msg = NULL;
	ch->mbc_bytes_done = 0;
}

static void clean_sw_buf(struct mailbox_channel *ch)
{
	if (!ch->sw_chan_buf)
		return;

	vfree(ch->sw_chan_buf);
	ch->sw_chan_buf = NULL;
}


void timeout_msg(struct mailbox_channel *ch)
{
	struct mailbox *mbx = ch->mbc_parent;
	struct mailbox_msg *msg = NULL;
	struct list_head *pos, *n;
	struct list_head l = LIST_HEAD_INIT(l);
	bool reschedule = false;

	/* Check active msg first. */
	msg = ch->mbc_cur_msg;
	if (msg) {
		if (msg->mbm_ttl == 0) {
			MBX_ERR(mbx, "found outstanding msg time'd out");
			if (!mbx->mbx_peer_dead) {
				MBX_ERR(mbx, "peer becomes dead");
				mbx->mbx_peer_dead = true;
			}
			chan_msg_done(ch, -ETIME);
			mutex_lock(&ch->sw_chan_mutex);
			clean_sw_buf(ch);
			mutex_unlock(&ch->sw_chan_mutex);
			/* Peer is not active any more. */
		} else {
			msg->mbm_ttl--;
			/* Need to come back again for this one. */
			reschedule = true;
		}
	}

	mutex_lock(&ch->mbc_mutex);

	list_for_each_safe(pos, n, &ch->mbc_msgs) {
		msg = list_entry(pos, struct mailbox_msg, mbm_list);
		if (msg->mbm_req_id == 0)
			continue;
		if (msg->mbm_ttl == 0) {
			list_del(&msg->mbm_list);
			list_add_tail(&msg->mbm_list, &l);
		} else {
			msg->mbm_ttl--;
			/* Need to come back again for this one. */
			reschedule = true;
		}
	}

	mutex_unlock(&ch->mbc_mutex);

	if (!list_empty(&l))
		MBX_ERR(mbx, "found waiting msg time'd out");

	list_for_each_safe(pos, n, &l) {
		msg = list_entry(pos, struct mailbox_msg, mbm_list);
		list_del(&msg->mbm_list);
		msg_done(msg, -ETIME);
	}
}

static void chann_worker(struct work_struct *work)
{
	struct mailbox_channel *ch =
		container_of(work, struct mailbox_channel, mbc_work);
	struct mailbox *mbx = ch->mbc_parent;

	while (!test_bit(MBXCS_BIT_STOP, &ch->mbc_state)) {
		/* Peer is active, if we are woken up not by a timer. */
		if (!test_bit(MBXCS_BIT_TICK, &ch->mbc_state)) {
			if (mbx->mbx_peer_dead) {
				MBX_ERR(mbx, "peer becomes active");
				mbx->mbx_peer_dead = false;
			}
		}

		MBX_DBG(mbx, "%s worker start", ch->mbc_name);
		ch->mbc_tran(ch);
		wait_for_completion_interruptible(&ch->mbc_worker);
	}
}

static inline u32 mailbox_chk_err(struct mailbox *mbx)
{
	u32 val = mailbox_reg_rd(mbx, &mbx->mbx_regs->mbr_error);

	/* Ignore bad register value after firewall is tripped. */
	if (val == 0xffffffff)
		val = 0;

	/* Error should not be seen, shout when found. */
	if (val)
		MBX_ERR(mbx, "mailbox error detected, error=0x%x\n", val);
	return val;
}

static int chan_msg_enqueue(struct mailbox_channel *ch, struct mailbox_msg *msg)
{
	int rv = 0;

	MBX_DBG(ch->mbc_parent, "%s enqueuing msg, id=0x%llx\n",
		ch->mbc_name, msg->mbm_req_id);

	BUG_ON(msg->mbm_req_id == INVALID_MSG_ID);

	mutex_lock(&ch->mbc_mutex);
	if (test_bit(MBXCS_BIT_STOP, &ch->mbc_state)) {
		rv = -ESHUTDOWN;
	} else {
		list_add_tail(&msg->mbm_list, &ch->mbc_msgs);
		msg->mbm_ch = ch;
	}
	mutex_unlock(&ch->mbc_mutex);

	chan_config_timer(ch);

	return rv;
}

static struct mailbox_msg *chan_msg_dequeue(struct mailbox_channel *ch,
	u64 req_id)
{
	struct mailbox_msg *msg = NULL;
	struct list_head *pos;

	mutex_lock(&ch->mbc_mutex);

	/* Take the first msg. */
	if (req_id == INVALID_MSG_ID) {
		msg = list_first_entry_or_null(&ch->mbc_msgs,
		struct mailbox_msg, mbm_list);
	/* Take the msg w/ specified ID. */
	} else {
		list_for_each(pos, &ch->mbc_msgs) {
			struct mailbox_msg *temp;

			temp = list_entry(pos, struct mailbox_msg, mbm_list);
			if (temp->mbm_req_id == req_id) {
				msg = temp;
				break;
			}
		}
	}

	if (msg) {
		MBX_DBG(ch->mbc_parent, "%s dequeued msg, id=0x%llx\n",
			ch->mbc_name, msg->mbm_req_id);
		list_del(&msg->mbm_list);
	}

	mutex_unlock(&ch->mbc_mutex);
	return msg;
}

static struct mailbox_msg *alloc_msg(void *buf, size_t len)
{
	char *newbuf = NULL;
	struct mailbox_msg *msg = NULL;
	/* Give MB*2 secs as time to live */

	if (!buf) {
		msg = vzalloc(sizeof(struct mailbox_msg) + len);
		if (!msg)
			return NULL;
		newbuf = ((char *)msg) + sizeof(struct mailbox_msg);
	} else {
		msg = vzalloc(sizeof(struct mailbox_msg));
		if (!msg)
			return NULL;
		newbuf = buf;
	}

	INIT_LIST_HEAD(&msg->mbm_list);
	msg->mbm_data = newbuf;
	msg->mbm_len = len;
	msg->mbm_ttl = MSG_MAX_TTL;
	msg->mbm_chan_sw = false;
	init_completion(&msg->mbm_complete);

	return msg;
}

static int chan_init(struct mailbox *mbx, char *nm,
	struct mailbox_channel *ch, chan_func_t fn)
{
	ch->mbc_parent = mbx;
	ch->mbc_name = nm;
	ch->mbc_tran = fn;
	INIT_LIST_HEAD(&ch->mbc_msgs);
	init_completion(&ch->mbc_worker);
	mutex_init(&ch->mbc_mutex);

	ch->mbc_cur_msg = NULL;
	ch->mbc_bytes_done = 0;

	reset_pkt(&ch->mbc_packet);
	set_bit(MBXCS_BIT_READY, &ch->mbc_state);

	/* One thread for one channel. */
	ch->mbc_wq =
		create_singlethread_workqueue(dev_name(&mbx->mbx_pdev->dev));
	if (!ch->mbc_wq) {
		ch->mbc_parent = NULL;
		return -ENOMEM;
	}

	INIT_WORK(&ch->mbc_work, chann_worker);
	queue_work(ch->mbc_wq, &ch->mbc_work);

	mutex_init(&ch->sw_chan_mutex);
	init_waitqueue_head(&ch->sw_chan_wq);

	mutex_lock(&ch->sw_chan_mutex);
	ch->sw_chan_buf = NULL;
	ch->sw_chan_buf_sz = 0;
	ch->sw_chan_msg_id = 0;
	mutex_unlock(&ch->sw_chan_mutex);

	/* One timer for one channel. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
	setup_timer(&ch->mbc_timer, chan_timer, (unsigned long)ch);
#else
	timer_setup(&ch->mbc_timer, chan_timer, 0);
#endif

	atomic_set(&ch->trigger, 0);
	return 0;
}

static void chan_fini(struct mailbox_channel *ch)
{
	struct mailbox_msg *msg;

	if (!ch->mbc_parent)
		return;

	/*
	 * Holding mutex to ensure no new msg is enqueued after
	 * flag is set.
	 */
	mutex_lock(&ch->mbc_mutex);
	set_bit(MBXCS_BIT_STOP, &ch->mbc_state);
	mutex_unlock(&ch->mbc_mutex);

	complete(&ch->mbc_worker);
	cancel_work_sync(&ch->mbc_work);
	destroy_workqueue(ch->mbc_wq);

	mutex_lock(&ch->sw_chan_mutex);
	if (ch->sw_chan_buf != NULL)
		vfree(ch->sw_chan_buf);
	mutex_unlock(&ch->sw_chan_mutex);

	msg = ch->mbc_cur_msg;
	if (msg)
		chan_msg_done(ch, -ESHUTDOWN);

	while ((msg = chan_msg_dequeue(ch, INVALID_MSG_ID)) != NULL)
		msg_done(msg, -ESHUTDOWN);

	del_timer_sync(&ch->mbc_timer);
}

static void listen_wq_fini(struct mailbox *mbx)
{
	BUG_ON(mbx == NULL);

	if (mbx->mbx_listen_wq != NULL) {
		complete(&mbx->mbx_comp);
		cancel_work_sync(&mbx->mbx_listen_worker);
		destroy_workqueue(mbx->mbx_listen_wq);
	}

}

static void chan_recv_pkt(struct mailbox_channel *ch)
{
	int i, retry = 10;
	struct mailbox *mbx = ch->mbc_parent;
	struct mailbox_pkt *pkt = &ch->mbc_packet;

	BUG_ON(valid_pkt(pkt));

	/* Picking up a packet from HW. */
	for (i = 0; i < PACKET_SIZE; i++) {
		while ((mailbox_reg_rd(mbx,
			&mbx->mbx_regs->mbr_status) & STATUS_EMPTY) &&
			(retry-- > 0))
			msleep(100);

		*(((u32 *)pkt) + i) =
			mailbox_reg_rd(mbx, &mbx->mbx_regs->mbr_rddata);
	}
	if ((mailbox_chk_err(mbx) & STATUS_EMPTY) != 0)
		reset_pkt(pkt);
	else
		MBX_DBG(mbx, "received pkt: type=0x%x", pkt->hdr.type);
}

static void chan_send_pkt(struct mailbox_channel *ch)
{
	int i;
	struct mailbox *mbx = ch->mbc_parent;
	struct mailbox_pkt *pkt = &ch->mbc_packet;

	BUG_ON(!valid_pkt(pkt));

	MBX_DBG(mbx, "sending pkt: type=0x%x", pkt->hdr.type);

	/* Pushing a packet into HW. */
	for (i = 0; i < PACKET_SIZE; i++) {
		mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_wrdata,
			*(((u32 *)pkt) + i));
	}

	reset_pkt(pkt);
	if (ch->mbc_cur_msg)
		ch->mbc_bytes_done += ch->mbc_packet.hdr.payload_size;

	BUG_ON((mailbox_chk_err(mbx) & STATUS_FULL) != 0);
}

static int chan_pkt2msg(struct mailbox_channel *ch)
{
	struct mailbox *mbx = ch->mbc_parent;
	void *msg_data, *pkt_data;
	struct mailbox_msg *msg = ch->mbc_cur_msg;
	struct mailbox_pkt *pkt = &ch->mbc_packet;
	size_t cnt = pkt->hdr.payload_size;
	u32 type = (pkt->hdr.type & PKT_TYPE_MASK);

	BUG_ON(((type != PKT_MSG_START) && (type != PKT_MSG_BODY)) || !msg);

	if (type == PKT_MSG_START) {
		msg->mbm_req_id = pkt->body.msg_start.msg_req_id;
		BUG_ON(msg->mbm_len < pkt->body.msg_start.msg_size);
		msg->mbm_len = pkt->body.msg_start.msg_size;
		pkt_data = pkt->body.msg_start.payload;
	} else {
		pkt_data = pkt->body.msg_body.payload;
	}

	if (cnt > msg->mbm_len - ch->mbc_bytes_done) {
		MBX_ERR(mbx, "invalid mailbox packet size\n");
		return -EBADMSG;
	}

	msg_data = msg->mbm_data + ch->mbc_bytes_done;
	(void) memcpy(msg_data, pkt_data, cnt);
	ch->mbc_bytes_done += cnt;

	reset_pkt(pkt);
	return 0;
}

static void do_sw_rx(struct mailbox_channel *ch)
{
	struct mailbox *mbx = ch->mbc_parent;
	int err = 0;
	struct mailbox_msg *msg = NULL;
	size_t len = 0;

	mutex_lock(&ch->sw_chan_mutex);
	if (!ch->sw_chan_buf)
		goto done;
	if (ch->mbc_cur_msg)
		goto done;

	if (ch->sw_chan_msg_flags & MB_REQ_FLAG_RESPONSE) {
		msg = chan_msg_dequeue(ch, ch->sw_chan_msg_id);
		if (!msg) {
			MBX_ERR(mbx, "Failed to dequeue msg with req id %llx\n",
				ch->sw_chan_msg_id);
		}
	} else if (ch->sw_chan_msg_flags & MB_REQ_FLAG_REQUEST) {
		len = ch->sw_chan_buf_sz;
		if (len < MAX_MSG_SZ)
			msg = alloc_msg(NULL, len);
		if (msg) {
			msg->mbm_req_id = ch->sw_chan_msg_id;
			msg->mbm_ch = ch;
			msg->mbm_flags = MB_REQ_FLAG_RECV_REQ;
			msg->mbm_chan_sw = true;
		} else {
			MBX_ERR(mbx, "Failed to allocate msg len:%lx\n", len);
		}

	} else {
		MBX_ERR(mbx, "Invalid incoming msg\n");
	}

	if (msg) {
		memcpy(msg->mbm_data, ch->sw_chan_buf, ch->sw_chan_buf_sz);
		ch->mbc_cur_msg = msg;
	}
	chan_msg_done(ch, err);
	ch->sw_chan_msg_id = 0;
	atomic_inc(&ch->trigger);
	mutex_unlock(&ch->sw_chan_mutex);
	wake_up_interruptible(&ch->sw_chan_wq);
	return;

done:
	mutex_unlock(&ch->sw_chan_mutex);
}

static void do_hw_rx(struct mailbox_channel *ch)
{
	struct mailbox *mbx = ch->mbc_parent;
	struct mailbox_pkt *pkt = &ch->mbc_packet;
	struct mailbox_msg *msg = NULL;
	u32 type;
	u64 id = 0;
	bool eom = false, read_hw = false;
	int err = 0;
	u32 st = mailbox_reg_rd(mbx, &mbx->mbx_regs->mbr_status);
	size_t len = 0;

	/* Check if a packet is ready for reading. */
	if (st == 0xffffffff) {
		/* Device is still being reset. */
		read_hw = false;
	} else if (test_bit(MBXCS_BIT_POLL_MODE, &ch->mbc_state)) {
		read_hw = ((st & STATUS_EMPTY) == 0);
	} else {
		read_hw = ((st & STATUS_RTA) != 0);
	}
	if (!read_hw)
		return;

	chan_recv_pkt(ch);
	type = pkt->hdr.type & PKT_TYPE_MASK;
	eom = ((pkt->hdr.type & PKT_TYPE_MSG_END) != 0);

	switch (type) {
	case PKT_TEST:
		(void) memcpy(&mbx->mbx_tst_pkt, &ch->mbc_packet,
			sizeof(struct mailbox_pkt));
		reset_pkt(pkt);
		return;
	case PKT_MSG_START:
		if (ch->mbc_cur_msg) {
			MBX_ERR(mbx, "received partial msg\n");
			chan_msg_done(ch, -EBADMSG);
		}

		/* Get a new active msg. */
		if (pkt->body.msg_start.msg_flags & MB_REQ_FLAG_RESPONSE) {
			id = pkt->body.msg_start.msg_req_id;
			ch->mbc_cur_msg = chan_msg_dequeue(ch, id);
		} else if (pkt->body.msg_start.msg_flags & MB_REQ_FLAG_REQUEST) {
			/* no msg, alloc dynamically */
			len = pkt->body.msg_start.msg_size;

			if (len < MAX_MSG_SZ)
				msg = alloc_msg(NULL, len);
			if (msg) {
				msg->mbm_ch = ch;
				msg->mbm_flags = MB_REQ_FLAG_RECV_REQ;
				ch->mbc_cur_msg = msg;
			} else {
				MBX_ERR(mbx, "Failed to allocate msg len:%lx\n",
					len);
			}
		}

		if (!ch->mbc_cur_msg) {
			MBX_ERR(mbx, "got unexpected msg start pkt\n");
			reset_pkt(pkt);
		} else if (pkt->body.msg_start.msg_size >
			ch->mbc_cur_msg->mbm_len) {
			chan_msg_done(ch, -EMSGSIZE);
			MBX_ERR(mbx, "received msg is too big");
			reset_pkt(pkt);
		}
		break;
	case PKT_MSG_BODY:
		if (!ch->mbc_cur_msg) {
			MBX_ERR(mbx, "got unexpected msg body pkt\n");
			reset_pkt(pkt);
		}
		break;
	default:
		MBX_ERR(mbx, "invalid mailbox pkt type\n");
		reset_pkt(pkt);
		return;
	}


	if (valid_pkt(pkt)) {
		err = chan_pkt2msg(ch);
		if (err || eom)
			chan_msg_done(ch, err);
	}
}

/*
 * Worker for RX channel.
 */
static void chan_do_rx(struct mailbox_channel *ch)
{
	do_sw_rx(ch);
	do_hw_rx(ch);
	/* Handle timer event. */
	if (test_bit(MBXCS_BIT_TICK, &ch->mbc_state)) {
		timeout_msg(ch);
		clear_bit(MBXCS_BIT_TICK, &ch->mbc_state);
	}
}

static void chan_msg2pkt(struct mailbox_channel *ch)
{
	size_t cnt = 0;
	size_t payload_off = 0;
	void *msg_data, *pkt_data;
	struct mailbox_msg *msg = ch->mbc_cur_msg;
	struct mailbox_pkt *pkt = &ch->mbc_packet;
	bool is_start = (ch->mbc_bytes_done == 0);
	bool is_eom = false;

	if (is_start) {
		payload_off = offsetof(struct mailbox_pkt,
			body.msg_start.payload);
	} else {
		payload_off = offsetof(struct mailbox_pkt,
			body.msg_body.payload);
	}
	cnt = PACKET_SIZE * sizeof(u32) - payload_off;
	if (cnt >= msg->mbm_len - ch->mbc_bytes_done) {
		cnt = msg->mbm_len - ch->mbc_bytes_done;
		is_eom = true;
	}

	pkt->hdr.type = is_start ? PKT_MSG_START : PKT_MSG_BODY;
	pkt->hdr.type |= is_eom ? PKT_TYPE_MSG_END : 0;
	pkt->hdr.payload_size = cnt;

	if (is_start) {
		pkt->body.msg_start.msg_req_id = msg->mbm_req_id;
		pkt->body.msg_start.msg_size = msg->mbm_len;
		pkt->body.msg_start.msg_flags = msg->mbm_flags;
		pkt_data = pkt->body.msg_start.payload;
	} else {
		pkt_data = pkt->body.msg_body.payload;
	}
	msg_data = msg->mbm_data + ch->mbc_bytes_done;
	(void) memcpy(pkt_data, msg_data, cnt);
}

static void handle_tx_timer_event(struct mailbox_channel *ch)
{
	if (!test_bit(MBXCS_BIT_TICK, &ch->mbc_state))
		return;
	timeout_msg(ch);
	clear_bit(MBXCS_BIT_TICK, &ch->mbc_state);
}

static void do_sw_tx(struct mailbox_channel *ch)
{
	mutex_lock(&ch->sw_chan_mutex);

	if (ch->mbc_cur_msg) {
		if (ch->sw_chan_buf) {
			atomic_inc(&ch->trigger);
			mutex_unlock(&ch->sw_chan_mutex);
			wake_up_interruptible(&ch->sw_chan_wq);
			return;
		}
		if (!ch->mbc_cur_msg->mbm_chan_sw)
			goto done;
		ch->sw_chan_buf = vmalloc(ch->mbc_cur_msg->mbm_len);
		if (!ch->sw_chan_buf)
			goto done;
		ch->sw_chan_buf_sz = ch->mbc_cur_msg->mbm_len;
		ch->sw_chan_msg_id = ch->mbc_cur_msg->mbm_req_id;
		ch->sw_chan_msg_flags = ch->mbc_cur_msg->mbm_flags;
		(void) memcpy(ch->sw_chan_buf, ch->mbc_cur_msg->mbm_data,
			ch->sw_chan_buf_sz);
		ch->mbc_bytes_done = ch->mbc_cur_msg->mbm_len;
		atomic_inc(&ch->trigger);
		mutex_unlock(&ch->sw_chan_mutex);
		wake_up_interruptible(&ch->sw_chan_wq);
		return;
	}
done:
	mutex_unlock(&ch->sw_chan_mutex);
}


static void do_hw_tx(struct mailbox_channel *ch)
{
	struct mailbox *mbx = ch->mbc_parent;

	u32 st = mailbox_reg_rd(mbx, &mbx->mbx_regs->mbr_status);
	/*
	 * The mailbox is free for sending new pkt now. See if we
	 * have something to send.
	 */

	if (ch->mbc_cur_msg) {
		if (ch->mbc_cur_msg->mbm_chan_sw)
			return;

		/* Check if a packet has been read by peer. */
		if ((st != 0xffffffff) && ((st & STATUS_STA) != 0)) {
			if (ch->mbc_cur_msg) {
				chan_msg2pkt(ch);
			} else if (valid_pkt(&mbx->mbx_tst_pkt)) {
				(void) memcpy(&ch->mbc_packet, &mbx->mbx_tst_pkt,
					sizeof(struct mailbox_pkt));
				reset_pkt(&mbx->mbx_tst_pkt);
			} else {
				return; /* Nothing to send. */
			}
			chan_send_pkt(ch);
		}
	}
}


static void check_tx_msg_done(struct mailbox_channel *ch)
{
	if (!ch->mbc_cur_msg)
		return;

	if (ch->mbc_cur_msg->mbm_len != ch->mbc_bytes_done)
		return;

	if (ch->mbc_cur_msg->mbm_chan_sw) {
		mutex_lock(&ch->sw_chan_mutex);
		clean_sw_buf(ch);
		mutex_unlock(&ch->sw_chan_mutex);
		chan_msg_done(ch, 0);
	} else {
		/* HACK FOR NOW */
		struct mailbox *mbx = ch->mbc_parent;
		u32 st = mailbox_reg_rd(mbx, &mbx->mbx_regs->mbr_status);

		if ((st != 0xffffffff) && ((st & STATUS_STA) != 0))
			chan_msg_done(ch, 0);
	}

}

static void msg_timer_on(struct mailbox_msg *msg, bool is_tx)
{
	if (is_tx)
		msg->mbm_ttl = max(BYTE_TO_MB(msg->mbm_len) * 2, MSG_TX_TTL);
	else
		msg->mbm_ttl = MSG_RX_TTL;
}

static void dequeue_tx_msg(struct mailbox_channel *ch)
{
	if (ch->mbc_cur_msg)
		return;

	ch->mbc_cur_msg = chan_msg_dequeue(ch, INVALID_MSG_ID);

	if (!ch->mbc_cur_msg)
		return;

	msg_timer_on(ch->mbc_cur_msg, true);
}

/*
 * Worker for TX channel.
 */
static void chan_do_tx(struct mailbox_channel *ch)
{
	check_tx_msg_done(ch);
	dequeue_tx_msg(ch);
	do_sw_tx(ch);
	do_hw_tx(ch);
	handle_tx_timer_event(ch);
}

static int mailbox_connect_status(struct platform_device *pdev)
{
	struct mailbox *mbx = platform_get_drvdata(pdev);
	int ret = 0;

	mutex_lock(&mbx->mbx_lock);
	ret = mbx->mbx_paired;
	mutex_unlock(&mbx->mbx_lock);
	return ret;
}

static ssize_t mailbox_ctl_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mailbox *mbx = platform_get_drvdata(pdev);
	u32 *reg = (u32 *)mbx->mbx_regs;
	int r, n;
	int nreg = sizeof(struct mailbox_reg) / sizeof(u32);

	for (r = 0, n = 0; r < nreg; r++, reg++) {
		/* Non-status registers. */
		if ((reg == &mbx->mbx_regs->mbr_resv1)		||
			(reg == &mbx->mbx_regs->mbr_wrdata)	||
			(reg == &mbx->mbx_regs->mbr_rddata)	||
			(reg == &mbx->mbx_regs->mbr_resv2))
			continue;
		/* Write-only status register. */
		if (reg == &mbx->mbx_regs->mbr_ctrl) {
			n += sprintf(buf + n, "%02ld %10s = --\n",
				r * sizeof(u32), reg2name(mbx, reg));
		/* Read-able status register. */
		} else {
			n += sprintf(buf + n, "%02ld %10s = 0x%08x\n",
				r * sizeof(u32), reg2name(mbx, reg),
				mailbox_reg_rd(mbx, reg));
		}
	}

	return n;
}

static ssize_t mailbox_ctl_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mailbox *mbx = platform_get_drvdata(pdev);
	u32 off, val;
	int nreg = sizeof(struct mailbox_reg) / sizeof(u32);
	u32 *reg = (u32 *)mbx->mbx_regs;

	if (sscanf(buf, "%d:%d", &off, &val) != 2 || (off % sizeof(u32)) ||
		!(off >= 0 && off < nreg * sizeof(u32))) {
		MBX_ERR(mbx, "input should be <reg_offset:reg_val>");
		return -EINVAL;
	}
	reg += off / sizeof(u32);

	mailbox_reg_wr(mbx, reg, val);
	return count;
}
/* HW register level debugging i/f. */
static DEVICE_ATTR_RW(mailbox_ctl);

static ssize_t mailbox_pkt_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mailbox *mbx = platform_get_drvdata(pdev);
	int ret = 0;

	if (valid_pkt(&mbx->mbx_tst_pkt)) {
		(void) memcpy(buf, mbx->mbx_tst_pkt.body.data,
			mbx->mbx_tst_pkt.hdr.payload_size);
		ret = mbx->mbx_tst_pkt.hdr.payload_size;
	}

	return ret;
}

static ssize_t mailbox_pkt_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mailbox *mbx = platform_get_drvdata(pdev);
	size_t maxlen = sizeof(mbx->mbx_tst_pkt.body.data);

	if (count > maxlen) {
		MBX_ERR(mbx, "max input length is %ld", maxlen);
		return 0;
	}

	(void) memcpy(mbx->mbx_tst_pkt.body.data, buf, count);
	mbx->mbx_tst_pkt.hdr.payload_size = count;
	mbx->mbx_tst_pkt.hdr.type = PKT_TEST;
	complete(&mbx->mbx_tx.mbc_worker);
	return count;
}

/* Packet test i/f. */
static DEVICE_ATTR_RW(mailbox_pkt);

static ssize_t mailbox_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mailbox *mbx = platform_get_drvdata(pdev);
	struct mailbox_req req;
	size_t respsz = sizeof(mbx->mbx_tst_rx_msg);
	int ret = 0;

	req.req = MAILBOX_REQ_TEST_READ;
	ret = mailbox_request(to_platform_device(dev), &req, sizeof(req),
		mbx->mbx_tst_rx_msg, &respsz, NULL, NULL);
	if (ret) {
		MBX_ERR(mbx, "failed to read test msg from peer: %d", ret);
	} else if (respsz > 0) {
		(void) memcpy(buf, mbx->mbx_tst_rx_msg, respsz);
		ret = respsz;
	}

	return ret;
}

static ssize_t mailbox_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mailbox *mbx = platform_get_drvdata(pdev);
	size_t maxlen = sizeof(mbx->mbx_tst_tx_msg);
	struct mailbox_req req = { 0 };

	if (count > maxlen) {
		MBX_ERR(mbx, "max input length is %ld", maxlen);
		return 0;
	}

	(void) memcpy(mbx->mbx_tst_tx_msg, buf, count);
	mbx->mbx_tst_tx_msg_len = count;
	req.req = MAILBOX_REQ_TEST_READY;
	(void) mailbox_post_notify(mbx->mbx_pdev, &req, sizeof(req));

	return count;
}

/* Msg test i/f. */
static DEVICE_ATTR_RW(mailbox);

static ssize_t connection_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	int ret;

	ret = mailbox_connect_status(pdev);
	return sprintf(buf, "0x%x\n", ret);
}
static DEVICE_ATTR_RO(connection);


static struct attribute *mailbox_attrs[] = {
	&dev_attr_mailbox.attr,
	&dev_attr_mailbox_ctl.attr,
	&dev_attr_mailbox_pkt.attr,
	&dev_attr_connection.attr,
	NULL,
};

static const struct attribute_group mailbox_attrgroup = {
	.attrs = mailbox_attrs,
};

static void dft_post_msg_cb(void *arg, void *buf, size_t len, u64 id, int err,
	bool sw_ch)
{
	struct mailbox_msg *msg = (struct mailbox_msg *)arg;

	if (!err)
		return;
	MBX_ERR(msg->mbm_ch->mbc_parent, "failed to post msg, err=%d", err);
}

static bool req_is_sw(struct platform_device *pdev, enum mailbox_request req)
{
	uint64_t ch_switch = 0;

	(void) mailbox_get(pdev, CHAN_SWITCH, &ch_switch);
	return (ch_switch & (1 << req));
}

/*
 * Msg will be sent to peer and reply will be received.
 */
int mailbox_request(struct platform_device *pdev, void *req, size_t reqlen,
	void *resp, size_t *resplen, mailbox_msg_cb_t cb, void *cbarg)
{
	int rv = -ENOMEM;
	struct mailbox *mbx = platform_get_drvdata(pdev);
	struct mailbox_msg *reqmsg = NULL, *respmsg = NULL;
	bool sw_ch = req_is_sw(pdev, ((struct mailbox_req *)req)->req);

	MBX_INFO(mbx, "sending request: %d via %s",
		((struct mailbox_req *)req)->req, (sw_ch ? "SW" : "HW"));

	/* If peer is not alive, no point sending req and waiting for resp. */
	if (mbx->mbx_peer_dead)
		return -ENOTCONN;

	if (cb) {
		reqmsg = alloc_msg(NULL, reqlen);
		if (reqmsg)
			(void) memcpy(reqmsg->mbm_data, req, reqlen);
	} else {
		reqmsg = alloc_msg(req, reqlen);
	}
	if (!reqmsg)
		goto fail;

	reqmsg->mbm_chan_sw = sw_ch;
	reqmsg->mbm_cb = NULL;
	reqmsg->mbm_cb_arg = NULL;
	reqmsg->mbm_req_id = (uintptr_t)reqmsg->mbm_data;
	reqmsg->mbm_flags |= MB_REQ_FLAG_REQUEST;

	respmsg = alloc_msg(resp, *resplen);
	if (!respmsg)
		goto fail;
	respmsg->mbm_cb = cb;
	respmsg->mbm_cb_arg = cbarg;
	/* Only interested in response w/ same ID. */
	respmsg->mbm_req_id = reqmsg->mbm_req_id;
	respmsg->mbm_chan_sw = sw_ch;

	/* Always enqueue RX msg before TX one to avoid race. */
	rv = chan_msg_enqueue(&mbx->mbx_rx, respmsg);
	if (rv)
		goto fail;
	rv = chan_msg_enqueue(&mbx->mbx_tx, reqmsg);
	if (rv) {
		respmsg = chan_msg_dequeue(&mbx->mbx_rx, reqmsg->mbm_req_id);
		goto fail;
	}

	/* Kick TX channel to try to send out msg. */
	complete(&mbx->mbx_tx.mbc_worker);

	wait_for_completion(&reqmsg->mbm_complete);

	rv = reqmsg->mbm_error;
	if (rv) {
		(void) chan_msg_dequeue(&mbx->mbx_rx, reqmsg->mbm_req_id);
		goto fail;
	}
	free_msg(reqmsg);
	msg_timer_on(respmsg, false);

	if (cb)
		return 0;

	wait_for_completion(&respmsg->mbm_complete);
	rv = respmsg->mbm_error;
	if (rv == 0)
		*resplen = respmsg->mbm_len;

	free_msg(respmsg);

	return rv;

fail:
	if (reqmsg)
		free_msg(reqmsg);
	if (respmsg)
		free_msg(respmsg);
	return rv;
}

/*
 * Request will be posted, no wait for reply.
 */
int mailbox_post_notify(struct platform_device *pdev, void *buf, size_t len)
{
	int rv = 0;
	struct mailbox *mbx = platform_get_drvdata(pdev);
	struct mailbox_msg *msg = NULL;
	bool sw_ch = req_is_sw(pdev, ((struct mailbox_req *)buf)->req);

	/* No checking for peer's liveness for posted msgs. */

	MBX_INFO(mbx, "posting request: %d via %s",
		((struct mailbox_req *)buf)->req, sw_ch ? "SW" : "HW");

	msg = alloc_msg(NULL, len);
	if (!msg)
		return -ENOMEM;

	(void) memcpy(msg->mbm_data, buf, len);
	msg->mbm_cb = dft_post_msg_cb;
	msg->mbm_cb_arg = msg;
	msg->mbm_chan_sw = sw_ch;
	msg->mbm_req_id = (uintptr_t)msg->mbm_data;
	msg->mbm_flags |= MB_REQ_FLAG_REQUEST;

	rv = chan_msg_enqueue(&mbx->mbx_tx, msg);
	if (rv)
		free_msg(msg);
	else /* Kick TX channel to try to send out msg. */
		complete(&mbx->mbx_tx.mbc_worker);

	return rv;
}

/*
 * Response will be always posted, no waiting.
 */
int mailbox_post_response(struct platform_device *pdev,
	enum mailbox_request req, u64 reqid, void *buf, size_t len)
{
	int rv = 0;
	struct mailbox *mbx = platform_get_drvdata(pdev);
	struct mailbox_msg *msg = NULL;
	bool sw_ch = req_is_sw(pdev, req);

	MBX_INFO(mbx, "posting response for: %d via %s",
		req, sw_ch ? "SW" : "HW");

	/* No checking for peer's liveness for posted msgs. */

	msg = alloc_msg(NULL, len);
	if (!msg)
		return -ENOMEM;

	(void) memcpy(msg->mbm_data, buf, len);
	msg->mbm_cb = dft_post_msg_cb;
	msg->mbm_cb_arg = msg;
	msg->mbm_chan_sw = sw_ch;
	msg->mbm_req_id = reqid;
	msg->mbm_flags |= MB_REQ_FLAG_RESPONSE;

	rv = chan_msg_enqueue(&mbx->mbx_tx, msg);
	if (rv)
		free_msg(msg);
	else /* Kick TX channel to try to send out msg. */
		complete(&mbx->mbx_tx.mbc_worker);

	return rv;
}

static void process_request(struct mailbox *mbx, struct mailbox_msg *msg)
{
	struct mailbox_req *req = (struct mailbox_req *)msg->mbm_data;
	int rc;
	const char *recvstr = "received request from peer";
	const char *sendstr = "sending test msg to peer";

	if (req->req == MAILBOX_REQ_TEST_READ) {
		MBX_INFO(mbx, "%s: %d", recvstr, req->req);
		if (mbx->mbx_tst_tx_msg_len) {
			MBX_INFO(mbx, "%s", sendstr);
			rc = mailbox_post_response(mbx->mbx_pdev, req->req,
				msg->mbm_req_id, mbx->mbx_tst_tx_msg,
				mbx->mbx_tst_tx_msg_len);
			if (rc)
				MBX_ERR(mbx, "%s failed: %d", sendstr, rc);
			else
				mbx->mbx_tst_tx_msg_len = 0;

		}
	} else if (req->req == MAILBOX_REQ_TEST_READY) {
		MBX_INFO(mbx, "%s: %d", recvstr, req->req);
	} else if (mbx->mbx_listen_cb) {
		/* Call client's registered callback to process request. */
		MBX_INFO(mbx, "%s: %d, passed on", recvstr, req->req);
		mbx->mbx_listen_cb(mbx->mbx_listen_cb_arg, msg->mbm_data,
			msg->mbm_len, msg->mbm_req_id, msg->mbm_error,
			msg->mbm_chan_sw);
	} else {
		MBX_INFO(mbx, "%s: %d, dropped", recvstr, req->req);
	}
}

/*
 * Wait for request from peer.
 */
static void mailbox_recv_request(struct work_struct *work)
{
	int rv = 0;
	struct mailbox_msg *msg = NULL;
	struct mailbox *mbx =
		container_of(work, struct mailbox, mbx_listen_worker);

	for (;;) {
		/* Only interested in request msg. */

		rv = wait_for_completion_interruptible(&mbx->mbx_comp);
		if (rv)
			break;
		mutex_lock(&mbx->mbx_lock);
		msg = list_first_entry_or_null(&mbx->mbx_req_list,
			struct mailbox_msg, mbm_list);

		if (msg) {
			list_del(&msg->mbm_list);
			mbx->mbx_req_cnt--;
			mbx->mbx_req_sz -= msg->mbm_len;
			mutex_unlock(&mbx->mbx_lock);
		} else {
			mutex_unlock(&mbx->mbx_lock);
			break;
		}

		process_request(mbx, msg);
		free_msg(msg);
	}

	if (rv == -ESHUTDOWN)
		MBX_INFO(mbx, "channel is closed, no listen to peer");
	else if (rv != 0)
		MBX_ERR(mbx, "failed to receive request from peer, err=%d", rv);

	if (msg)
		free_msg(msg);
}

int mailbox_listen(struct platform_device *pdev,
	mailbox_msg_cb_t cb, void *cbarg)
{
	struct mailbox *mbx = platform_get_drvdata(pdev);

	mbx->mbx_listen_cb_arg = cbarg;
	wmb();
	mbx->mbx_listen_cb = cb;

	return 0;
}

static int mailbox_enable_intr_mode(struct mailbox *mbx)
{
	struct resource *res;
	int ret;
	struct platform_device *pdev = mbx->mbx_pdev;
	xdev_handle_t xdev = xocl_get_xdev(pdev);
	u32 is;

	if (mbx->mbx_irq != -1)
		return 0;

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (res == NULL) {
		MBX_ERR(mbx, "failed to acquire intr resource");
		return -EINVAL;
	}

	ret = xocl_user_interrupt_reg(xdev, res->start, mailbox_isr, mbx);
	if (ret) {
		MBX_ERR(mbx, "failed to add intr handler");
		return ret;
	}
	ret = xocl_user_interrupt_config(xdev, res->start, true);
	BUG_ON(ret != 0);

	/* Only see intr when we have full packet sent or received. */
	mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_rit, PACKET_SIZE - 1);
	mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_sit, 0);

	/* clear interrupt */
	is = mailbox_reg_rd(mbx, &mbx->mbx_regs->mbr_is);
	mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_is, is);

	/* Finally, enable TX / RX intr. */
	mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_ie, 0x3);

	clear_bit(MBXCS_BIT_POLL_MODE, &mbx->mbx_rx.mbc_state);
	chan_config_timer(&mbx->mbx_rx);

	clear_bit(MBXCS_BIT_POLL_MODE, &mbx->mbx_tx.mbc_state);
	chan_config_timer(&mbx->mbx_tx);

	mbx->mbx_irq = res->start;
	return 0;
}

static void mailbox_disable_intr_mode(struct mailbox *mbx)
{
	struct platform_device *pdev = mbx->mbx_pdev;
	xdev_handle_t xdev = xocl_get_xdev(pdev);

	/*
	 * No need to turn on polling mode for TX, which has
	 * a channel stall checking timer always on when there is
	 * outstanding TX packet.
	 */
	set_bit(MBXCS_BIT_POLL_MODE, &mbx->mbx_rx.mbc_state);
	chan_config_timer(&mbx->mbx_rx);

	/* Disable both TX / RX intrs. */
	mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_ie, 0x0);

	mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_rit, 0x0);
	mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_sit, 0x0);

	if (mbx->mbx_irq == -1)
		return;

	(void) xocl_user_interrupt_config(xdev, mbx->mbx_irq, false);
	(void) xocl_user_interrupt_reg(xdev, mbx->mbx_irq, NULL, mbx);

	mbx->mbx_irq = -1;
}


int mailbox_get(struct platform_device *pdev, enum mb_kind kind, u64 *data)
{
	struct mailbox *mbx = platform_get_drvdata(pdev);
	int ret = 0;

	mutex_lock(&mbx->mbx_lock);
	switch (kind) {
	case CHAN_STATE:
		*data = mbx->mbx_ch_state;
		break;
	case CHAN_SWITCH:
		*data = mbx->mbx_ch_switch;
		break;
	case COMM_ID:
		(void) memcpy(data, mbx->mbx_comm_id, sizeof(mbx->mbx_comm_id));
		break;
	case VERSION:
		*data = mbx->mbx_proto_ver;
		break;
	default:
		MBX_INFO(mbx, "unknown data kind: %d", kind);
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&mbx->mbx_lock);

	return ret;
}


int mailbox_set(struct platform_device *pdev, enum mb_kind kind, u64 data)
{
	struct mailbox *mbx = platform_get_drvdata(pdev);
	int ret = 0;

	switch (kind) {
	case RESET:
		if (mailbox_no_intr)
			break;

		if (data == 1) {
			/* Post reset */
			MBX_INFO(mbx, "enable intr mode");
			if (mailbox_enable_intr_mode(mbx) != 0)
				MBX_ERR(mbx, "enable intr failed after reset");
		} else {
			/* Pre reset */
			MBX_INFO(mbx, "enable polling mode");
			mailbox_disable_intr_mode(mbx);
		}
		break;
	case CHAN_STATE:
		mutex_lock(&mbx->mbx_lock);
		mbx->mbx_ch_state = data;
		mutex_unlock(&mbx->mbx_lock);
		break;
	case CHAN_SWITCH:
		mutex_lock(&mbx->mbx_lock);
		mbx->mbx_ch_switch = data;
		mutex_unlock(&mbx->mbx_lock);
		break;
	case COMM_ID:
		mutex_lock(&mbx->mbx_lock);
		(void) memcpy(mbx->mbx_comm_id, (void *)(uintptr_t)data,
			sizeof(mbx->mbx_comm_id));
		mutex_unlock(&mbx->mbx_lock);
		break;
	case VERSION:
		mutex_lock(&mbx->mbx_lock);
		mbx->mbx_proto_ver = (uint32_t) data;
		mutex_unlock(&mbx->mbx_lock);
		break;
	default:
		MBX_INFO(mbx, "unknown data kind: %d", kind);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int mailbox_sw_transfer(struct platform_device *pdev, void *args)
{
	struct mailbox *mbx;
	struct mailbox_channel *ch;
	struct sw_chan *sw_chan_args;
	int ret = 0;

	mbx = platform_get_drvdata(pdev);

	sw_chan_args = (struct sw_chan *)args;

	if (sw_chan_args->is_tx)
		ch = &mbx->mbx_tx;
	else
		ch = &mbx->mbx_rx;

	if (sw_chan_args->is_tx) {
		/* wake tx worker */
		complete(&ch->mbc_worker);

		/* sleep until do_hw_tx copies to sw_chan_buf */
		if (wait_event_interruptible(ch->sw_chan_wq,
			atomic_read(&ch->trigger) > 0) == -ERESTARTSYS) {
			return -ERESTARTSYS;
		}

		/*
		 * if mbm_len > userspace buf size (chan_from_ioctl.sz), then
		 * don't attempt a copy, instead set the size and return
		 * -EMSGSIZE. This will initiate a resize of userspace buffer
		 * and attempt the ioctl again from userspace.
		 */

		mutex_lock(&ch->sw_chan_mutex);

		if (ch->sw_chan_buf_sz > sw_chan_args->sz) {
			sw_chan_args->sz = ch->sw_chan_buf_sz;
			mutex_unlock(&ch->sw_chan_mutex);
			return -EMSGSIZE;
		}
		atomic_dec_if_positive(&ch->trigger);

		ret = copy_to_user(sw_chan_args->data,
					ch->sw_chan_buf,
					ch->sw_chan_buf_sz);
		sw_chan_args->id = ch->sw_chan_msg_id;
		sw_chan_args->sz = ch->sw_chan_buf_sz;
		sw_chan_args->flags = ch->sw_chan_msg_flags;
		ch->sw_chan_msg_id = 0;
		mutex_unlock(&ch->sw_chan_mutex);
		complete(&ch->mbc_worker);

		if (ret != 0)
			ret = -EBADMSG;

		return ret;
	} else {
		/* copy into sw_chan_buf */
		mutex_lock(&ch->sw_chan_mutex);
		if (ch->sw_chan_buf == NULL) {
			ch->sw_chan_buf = vmalloc(sw_chan_args->sz);
			ch->sw_chan_buf_sz = sw_chan_args->sz;
			ch->sw_chan_msg_id = sw_chan_args->id;
			ch->sw_chan_msg_flags = sw_chan_args->flags;
			ret = copy_from_user(ch->sw_chan_buf,
						sw_chan_args->data,
						sw_chan_args->sz);
		}
		mutex_unlock(&ch->sw_chan_mutex);

		if (ret != 0) {
			ret = -EBADMSG;
			goto end;
		}

		/*
		 * signal channel worker that we are here and the
		 * packet is ready to take
		 */
		complete(&ch->mbc_worker);

		/* sleep until chan_do_rx dequeues */
		if (wait_event_interruptible(ch->sw_chan_wq,
			atomic_read(&ch->trigger) > 0) == -ERESTARTSYS) {
			MBX_ERR(mbx, "sw_chan_wq signalled with ERESTARTSYS");
			ret = -ERESTARTSYS;
			goto end;
		}
	}

end:
	mutex_lock(&ch->sw_chan_mutex);
	atomic_dec_if_positive(&ch->trigger);
	if (ch->sw_chan_msg_id == 0)
		clean_sw_buf(ch);
	mutex_unlock(&ch->sw_chan_mutex);
	return ret;
}

/* Kernel APIs exported from this sub-device driver. */
static struct xocl_mailbox_funcs mailbox_ops = {
	.request	= mailbox_request,
	.post_notify	= mailbox_post_notify,
	.post_response	= mailbox_post_response,
	.listen		= mailbox_listen,
	.set		= mailbox_set,
	.get		= mailbox_get,
	.sw_transfer	= mailbox_sw_transfer,
};

static int mailbox_open(struct inode *inode, struct file *file)
{
	struct mailbox *mbx = NULL;

	mbx = xocl_drvinst_open(inode->i_cdev);
	if (!mbx)
		return -ENXIO;

	/* create a reference to our char device in the opened file */
	file->private_data = mbx;
	return 0;
}

/*
 * Called when the device goes from used to unused.
 */
static int mailbox_close(struct inode *inode, struct file *file)
{
	struct mailbox *mbx = file->private_data;

	xocl_drvinst_close(mbx);
	return 0;
}

static ssize_t
mailbox_read(struct file *file, char __user *buf, size_t n, loff_t *of) {
	struct mailbox *mbx = file->private_data;
	struct platform_device *pdev = mbx->mbx_pdev;
	int ret = 0;
	struct sw_chan *sw_chan_args = (struct sw_chan *)buf;

	/* set by user already? */
	sw_chan_args->is_tx = 1;

	ret = mailbox_sw_transfer(pdev, sw_chan_args);
	if (ret == 0)
		return sw_chan_args->sz;

	return ret;
}

static ssize_t
mailbox_write(struct file *file, const char __user *buf, size_t n, loff_t *of) {
	struct mailbox *mbx = file->private_data;
	struct platform_device *pdev = mbx->mbx_pdev;
	int ret = 0;
	struct sw_chan *sw_chan_args = (struct sw_chan *)buf;

	/* set by user already? */
	sw_chan_args->is_tx = 0;

	ret = mailbox_sw_transfer(pdev, sw_chan_args);
	if (ret == 0)
		return n;

	return ret;
}

static uint mailbox_poll(struct file *file, poll_table *wait)
{
	struct mailbox *mbx = file->private_data;
	struct mailbox_channel *ch = &mbx->mbx_tx;
	int counter;

	poll_wait(file, &ch->sw_chan_wq, wait);
	counter = atomic_read(&ch->trigger);
	MBX_INFO(mbx, "mailbox_poll: %d", counter);
	if (counter == 0)
		return 0;
	return POLLIN;
}

/*
 * pseudo device file operations for the mailbox
 */
static const struct file_operations mailbox_fops = {
	.owner = THIS_MODULE,
	.open = mailbox_open,
	.release = mailbox_close,
	.read = mailbox_read,
	.write = mailbox_write,
	.poll = mailbox_poll,
};

static int mailbox_remove(struct platform_device *pdev)
{
	struct mailbox *mbx = platform_get_drvdata(pdev);

	BUG_ON(mbx == NULL);

	mailbox_disable_intr_mode(mbx);

	sysfs_remove_group(&pdev->dev.kobj, &mailbox_attrgroup);

	chan_fini(&mbx->mbx_rx);
	chan_fini(&mbx->mbx_tx);
	listen_wq_fini(mbx);

	BUG_ON(!(list_empty(&mbx->mbx_req_list)));

	xocl_subdev_register(pdev, XOCL_SUBDEV_MAILBOX, NULL);

	if (mbx->mbx_regs)
		iounmap(mbx->mbx_regs);

	MBX_INFO(mbx, "mailbox cleaned up successfully");

	if (mbx->sys_device)
		device_destroy(xrt_class, mbx->sys_cdev->dev);
	if (mbx->sys_cdev)
		cdev_del(mbx->sys_cdev);
	platform_set_drvdata(pdev, NULL);
	xocl_drvinst_free(mbx);

	return 0;
}

static int mailbox_probe(struct platform_device *pdev)
{
	struct mailbox *mbx = NULL;
	struct resource *res;
	int ret;
	struct xocl_dev_core *core = xocl_get_xdev(pdev);

	mbx = xocl_drvinst_alloc(&pdev->dev, sizeof(struct mailbox));
	if (!mbx)
		return -ENOMEM;
	platform_set_drvdata(pdev, mbx);
	mbx->mbx_pdev = pdev;
	mbx->mbx_irq = (u32)-1;


	init_completion(&mbx->mbx_comp);
	mutex_init(&mbx->mbx_lock);
	INIT_LIST_HEAD(&mbx->mbx_req_list);
	mbx->mbx_req_cnt = 0;
	mbx->mbx_req_sz = 0;
	mbx->mbx_peer_dead = false;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mbx->mbx_regs = ioremap_nocache(res->start, res->end - res->start + 1);
	if (!mbx->mbx_regs) {
		MBX_ERR(mbx, "failed to map in registers");
		ret = -EIO;
		goto failed;
	}
	/* Reset both TX channel and RX channel */
	mailbox_reg_wr(mbx, &mbx->mbx_regs->mbr_ctrl, 0x3);

	/* Set up software communication channels. */
	ret = chan_init(mbx, "RX", &mbx->mbx_rx, chan_do_rx);
	if (ret != 0) {
		MBX_ERR(mbx, "failed to init rx channel");
		goto failed;
	}
	ret = chan_init(mbx, "TX", &mbx->mbx_tx, chan_do_tx);
	if (ret != 0) {
		MBX_ERR(mbx, "failed to init tx channel");
		goto failed;
	}
	/* Dedicated thread for listening to peer request. */
	mbx->mbx_listen_wq =
		create_singlethread_workqueue(dev_name(&mbx->mbx_pdev->dev));
	if (!mbx->mbx_listen_wq) {
		MBX_ERR(mbx, "failed to create request-listen work queue");
		goto failed;
	}
	INIT_WORK(&mbx->mbx_listen_worker, mailbox_recv_request);
	queue_work(mbx->mbx_listen_wq, &mbx->mbx_listen_worker);

	ret = sysfs_create_group(&pdev->dev.kobj, &mailbox_attrgroup);
	if (ret != 0) {
		MBX_ERR(mbx, "failed to init sysfs");
		goto failed;
	}

	if (mailbox_no_intr) {
		MBX_INFO(mbx, "Enabled timer-driven mode");
		mailbox_disable_intr_mode(mbx);
	} else {
		ret = mailbox_enable_intr_mode(mbx);
		if (ret != 0) {
			MBX_INFO(mbx, "failed to enable intr mode");
			/* Ignore error, fall back to timer driven mode */
			mailbox_disable_intr_mode(mbx);
		}
	}

	xocl_subdev_register(pdev, XOCL_SUBDEV_MAILBOX, &mailbox_ops);

	mbx->mbx_prot_ver = MB_PROTOCOL_VER;

	mbx->sys_cdev = cdev_alloc();
	mbx->sys_cdev->ops = &mailbox_fops;
	mbx->sys_cdev->owner = THIS_MODULE;
	mbx->sys_cdev->dev = MKDEV(MAJOR(mailbox_dev), 0);
	ret = cdev_add(mbx->sys_cdev, mbx->sys_cdev->dev, 1);
	if (ret) {
		MBX_ERR(mbx, "cdev add failed");
		goto failed;
	}

	mbx->sys_device = device_create(xrt_class, &pdev->dev,
			mbx->sys_cdev->dev, NULL, "%s%d",
			platform_get_device_id(pdev)->name,
			XOCL_DEV_ID(core->pdev));
	if (IS_ERR(mbx->sys_device)) {
		ret = PTR_ERR(mbx->sys_device);
		goto failed;
	}

	xocl_drvinst_set_filedev(mbx, mbx->sys_cdev);

	MBX_INFO(mbx, "successfully initialized");
	return 0;

failed:
	mailbox_remove(pdev);
	return ret;
}

struct platform_device_id mailbox_id_table[] = {
	{ XOCL_MAILBOX, 0 },
	{ },
};

static struct platform_driver mailbox_driver = {
	.probe		= mailbox_probe,
	.remove		= mailbox_remove,
	.driver		= {
		.name	= XOCL_MAILBOX,
	},
	.id_table = mailbox_id_table,
};

int __init xocl_init_mailbox(void)
{
	int err = 0;

	BUILD_BUG_ON(sizeof(struct mailbox_pkt) != sizeof(u32) * PACKET_SIZE);

	err = alloc_chrdev_region(&mailbox_dev, 0, XOCL_MAX_DEVICES, XOCL_MAILBOX);
	if (err < 0)
		goto err_chrdev_reg;

	err = platform_driver_register(&mailbox_driver);
	if (err < 0)
		goto err_driver_reg;

	return 0;
err_driver_reg:
	unregister_chrdev_region(mailbox_dev, 1);
err_chrdev_reg:
	return err;
}

void xocl_fini_mailbox(void)
{
	unregister_chrdev_region(mailbox_dev, XOCL_MAX_DEVICES);
	platform_driver_unregister(&mailbox_driver);
}
