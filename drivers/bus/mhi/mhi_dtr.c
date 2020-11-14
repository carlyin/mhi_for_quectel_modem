// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.*/

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/termios.h>
#include <linux/wait.h>
#include <linux/mhi.h>
//#include "internal.h"

struct __packed dtr_ctrl_msg {
	u32 preamble;
	u32 msg_id;
	u32 dest_id;
	u32 size;
	u32 msg;
};

#define CTRL_MAGIC (0x4C525443)
#define CTRL_MSG_DTR BIT(0)
#define CTRL_MSG_RTS BIT(1)
#define CTRL_MSG_DCD BIT(0)
#define CTRL_MSG_DSR BIT(1)
#define CTRL_MSG_RI BIT(3)
#define CTRL_HOST_STATE (0x10)
#define CTRL_DEVICE_STATE (0x11)
#define CTRL_GET_CHID(dtr) (dtr->dest_id & 0xFF)

static int mhi_dtr_queue_inbound(struct mhi_device *mhi_dev)
{
	int nr_trbs = mhi_get_free_desc_count(mhi_dev, DMA_FROM_DEVICE);
	size_t mtu = sizeof(struct dtr_ctrl_msg);
	void *buf;
	int ret = -EIO, i;

	for (i = 0; i < nr_trbs; i++) {
		buf = kmalloc(mtu, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;

		ret = mhi_queue_buf(mhi_dev, DMA_FROM_DEVICE, buf, mtu,
					 MHI_EOT);
		if (ret) {
			kfree(buf);
			return ret;
		}
	}

	return ret;
}

static void mhi_dtr_dl_xfer_cb(struct mhi_device *mhi_dev,
			       struct mhi_result *mhi_result)
{
	struct dtr_ctrl_msg *dtr_msg = mhi_result->buf_addr;
	size_t mtu = sizeof(struct dtr_ctrl_msg);

	if (mhi_result->transaction_status == -ENOTCONN) {
		kfree(mhi_result->buf_addr);
		return;
	}

	if (mhi_result->bytes_xferd != sizeof(*dtr_msg)) {
		dev_err(&mhi_dev->dev, "Unexpected length %zu received\n",
			mhi_result->bytes_xferd);
		return;
	}

	dev_info(&mhi_dev->dev, "preamble:0x%x msg_id:%u dest_id:%u msg:0x%x\n",
		 dtr_msg->preamble, dtr_msg->msg_id, dtr_msg->dest_id,
		 dtr_msg->msg);

	mhi_queue_buf(mhi_dev, DMA_FROM_DEVICE, mhi_result->buf_addr,
		mtu, MHI_EOT);
}

static void mhi_dtr_ul_xfer_cb(struct mhi_device *mhi_dev,
			       struct mhi_result *mhi_result)
{

}

static void mhi_dtr_remove(struct mhi_device *mhi_dev)
{
}

static int mhi_dtr_probe(struct mhi_device *mhi_dev,
			 const struct mhi_device_id *id)
{
	int ret;

	dev_info(&mhi_dev->dev, "Enter for DTR control channel\n");

	ret = mhi_prepare_for_transfer(mhi_dev);

	if (!ret)
		ret = mhi_dtr_queue_inbound(mhi_dev);

	dev_info(&mhi_dev->dev, "Exit with ret:%d\n", ret);

	return ret;
}

static const struct mhi_device_id mhi_dtr_table[] = {
	{ .chan = "IP_CTRL", .driver_data = sizeof(struct dtr_ctrl_msg) },
	{},
};

static struct mhi_driver mhi_dtr_driver = {
	.id_table = mhi_dtr_table,
	.remove = mhi_dtr_remove,
	.probe = mhi_dtr_probe,
	.ul_xfer_cb = mhi_dtr_ul_xfer_cb,
	.dl_xfer_cb = mhi_dtr_dl_xfer_cb,
	.driver = {
		.name = "MHI_DTR",
		.owner = THIS_MODULE,
	}
};

static int __init mhi_dtr_init(void)
{
	return mhi_driver_register(&mhi_dtr_driver);
}

static void __exit mhi_dtr_exit(void)
{
	mhi_driver_unregister(&mhi_dtr_driver);
}

module_init(mhi_dtr_init);
module_exit(mhi_dtr_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MHI DTR Driver");
