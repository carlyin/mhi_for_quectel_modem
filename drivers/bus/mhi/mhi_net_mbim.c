// SPDX-License-Identifier: GPL-2.0-or-later
/* MHI Network driver - Network over MHI
 *
 * Copyright (C) 2020 Linaro Ltd <loic.poulain@linaro.org>
 */

#include <linux/if_arp.h>
#include <linux/mhi.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/usb/cdc.h>

struct mhi_net_stats {
	u64 rx_packets;
	u64 rx_bytes;
	u64 rx_errors;
	u64 rx_dropped;
	u64 tx_packets;
	u64 tx_bytes;
	u64 tx_errors;
	u64 tx_dropped;
	atomic_t rx_queued;
};

struct cdc_mbim_ctx {
	u16 tx_seq;
	u16 rx_seq;
	u32 rx_max;
};

struct cdc_mbim_hdr {
	struct usb_cdc_ncm_nth16 nth16;
	struct usb_cdc_ncm_ndp16 ndp16;
	struct usb_cdc_ncm_dpe16 dpe16[2];
} __attribute__ ((packed));

struct mhi_net_dev {
	struct mhi_device *mdev;
	struct net_device *ndev;
	struct delayed_work rx_refill;
	struct mhi_net_stats stats;
	struct cdc_mbim_ctx mbim_ctx;
	u32 rx_queue_sz;
};

#define QCUSB_MRECEIVE_MAX_BUFFER_SIZE (1024*31) //maybe 31KB is enough
#define QCUSB_MTRANSMIT_MAX_BUFFER_SIZE (1024*16)
#define NTB_OUT_MAX_DATAGRAMS        16

static const struct usb_cdc_ncm_ntb_parameters ncmNTBParams = {
	.bmNtbFormatsSupported = USB_CDC_NCM_NTB16_SUPPORTED,
	.dwNtbInMaxSize = QCUSB_MRECEIVE_MAX_BUFFER_SIZE,
	.wNdpInDivisor = 0x04,
	.wNdpInPayloadRemainder = 0x0,
	.wNdpInAlignment = 0x4,

	.dwNtbOutMaxSize = QCUSB_MTRANSMIT_MAX_BUFFER_SIZE,
	.wNdpOutDivisor = 0x04,
	.wNdpOutPayloadRemainder = 0x0,
	.wNdpOutAlignment = 0x4,
	.wNtbOutMaxDatagrams = NTB_OUT_MAX_DATAGRAMS,
};

static int cdc_mbim_tx_fixup(struct mhi_net_dev *mhi_netdev,
	struct sk_buff *skb, u8 session_id)
{
	struct net_device *ndev = mhi_netdev->ndev;
	struct cdc_mbim_ctx *ctx = &mhi_netdev->mbim_ctx;
	struct cdc_mbim_hdr *mhdr;
	__le32 sign;
	u8 *c;
	u16 tci = session_id;
	unsigned int skb_len = skb->len;

	if (skb_headroom(skb) < sizeof(struct cdc_mbim_hdr)) {
		net_err_ratelimited("%s: skb_headroom small! headroom is %u, need %zd\n",
			ndev->name, skb_headroom(skb), sizeof(struct cdc_mbim_hdr));
		return -ENOBUFS;
	}

	skb_push(skb, sizeof(struct cdc_mbim_hdr));

	mhdr = (struct cdc_mbim_hdr *)skb->data;
	mhdr->nth16.dwSignature = cpu_to_le32(USB_CDC_NCM_NTH16_SIGN);
	mhdr->nth16.wHeaderLength = cpu_to_le16(sizeof(struct usb_cdc_ncm_nth16));
	mhdr->nth16.wSequence = cpu_to_le16(ctx->tx_seq++);
	mhdr->nth16.wBlockLength = cpu_to_le16(skb->len);
	mhdr->nth16.wNdpIndex = cpu_to_le16(sizeof(struct usb_cdc_ncm_nth16));

	sign = cpu_to_le32(USB_CDC_MBIM_NDP16_IPS_SIGN);
	c = (u8 *)&sign;
	c[3] = tci;

	mhdr->ndp16.dwSignature = sign;
	mhdr->ndp16.wLength = cpu_to_le16(sizeof(struct usb_cdc_ncm_ndp16) + sizeof(struct usb_cdc_ncm_dpe16) * 2);
	mhdr->ndp16.wNextNdpIndex = 0;

	mhdr->ndp16.dpe16[0].wDatagramIndex = sizeof(struct cdc_mbim_hdr);
	mhdr->ndp16.dpe16[0].wDatagramLength = skb_len;

	mhdr->ndp16.dpe16[1].wDatagramIndex = 0;
	mhdr->ndp16.dpe16[1].wDatagramLength = 0;

	return 0;
}


static void cdc_mbim_rx_fixup(struct mhi_net_dev *mhi_netdev, struct sk_buff *skb_in)
{
	struct cdc_mbim_ctx *ctx = &mhi_netdev->mbim_ctx;
	struct net_device *ndev = mhi_netdev->ndev;
	struct usb_cdc_ncm_nth16 *nth16;
	int ndpoffset, len;
	u16 wSequence;
	struct sk_buff_head skb_chain;
	struct sk_buff *new_skb;

	__skb_queue_head_init(&skb_chain);

	if (skb_in->len < (sizeof(struct usb_cdc_ncm_nth16) + sizeof(struct usb_cdc_ncm_ndp16))) {
		net_err_ratelimited("%s: frame too short\n", ndev->name);
		goto error;
	}

	nth16 = (struct usb_cdc_ncm_nth16 *)skb_in->data;

	if (nth16->dwSignature != cpu_to_le32(USB_CDC_NCM_NTH16_SIGN)) {
		net_err_ratelimited("%s: invalid NTH16 signature <%#010x>\n",
			ndev->name, le32_to_cpu(nth16->dwSignature));
		goto error;
	}

	len = le16_to_cpu(nth16->wBlockLength);
	if (len > ctx->rx_max) {
		net_err_ratelimited("%s: unsupported NTB block length %u/%u\n",
			ndev->name, len, ctx->rx_max);
		goto error;
	}

	wSequence = le16_to_cpu(nth16->wSequence);
	if (ctx->rx_seq !=  wSequence) {
		net_err_ratelimited("%s: sequence number glitch prev=%d curr=%d\n",
			ndev->name, ctx->rx_seq, wSequence);
	}
	ctx->rx_seq = wSequence + 1;

	ndpoffset = nth16->wNdpIndex;

	while (ndpoffset > 0) {
		struct usb_cdc_ncm_ndp16 *ndp16 ;
		struct usb_cdc_ncm_dpe16 *dpe16;
		int nframes, x;
		u8 *c;
		u16 tci = 0;

		if (skb_in->len < (ndpoffset + sizeof(struct usb_cdc_ncm_ndp16))) {
			net_err_ratelimited("%s: invalid NDP offset  <%u>\n",
				ndev->name, ndpoffset);
			goto error;
		}

		ndp16 = (struct usb_cdc_ncm_ndp16 *)(skb_in->data + ndpoffset);

		if (le16_to_cpu(ndp16->wLength) < 0x10) {
			net_err_ratelimited("%s: invalid DPT16 length <%u>\n",
				ndev->name, le16_to_cpu(ndp16->wLength));
			goto error;
		}

		nframes = ((le16_to_cpu(ndp16->wLength) - sizeof(struct usb_cdc_ncm_ndp16)) / sizeof(struct usb_cdc_ncm_dpe16));

		if (skb_in->len < (sizeof(struct usb_cdc_ncm_ndp16) + nframes * (sizeof(struct usb_cdc_ncm_dpe16)))) {
			net_err_ratelimited("%s: Invalid nframes = %d\n",
				ndev->name, nframes);
			goto error;
		}

		switch (ndp16->dwSignature & cpu_to_le32(0x00ffffff)) {
			case cpu_to_le32(USB_CDC_MBIM_NDP16_IPS_SIGN):
				c = (u8 *)&ndp16->dwSignature;
				tci = c[3];
				/* tag IPS<0> packets too if MBIM_IPS0_VID exists */
				//if (!tci && info->flags & FLAG_IPS0_VLAN)
				//	tci = MBIM_IPS0_VID;
			break;
			case cpu_to_le32(USB_CDC_MBIM_NDP16_DSS_SIGN):
				c = (u8 *)&ndp16->dwSignature;
				tci = c[3] + 256;
			break;
			default:
				net_err_ratelimited("%s: unsupported NDP signature <0x%08x>\n",
					ndev->name, le32_to_cpu(ndp16->dwSignature));
			goto error;
		}

		if (tci != 0) {
			net_err_ratelimited("%s: unsupported tci %d by now\n",
				ndev->name, tci);
			goto error;
		}

		dpe16 = ndp16->dpe16;

		for (x = 0; x < nframes; x++, dpe16++) {
			int offset = le16_to_cpu(dpe16->wDatagramIndex);
			int skb_len = le16_to_cpu(dpe16->wDatagramLength);

			if (offset == 0 || skb_len == 0) {
				break;
			}

			/* sanity checking */
			if (((offset + skb_len) > skb_in->len) || (skb_len > ctx->rx_max)) {
				net_err_ratelimited("%s: invalid frame detected (ignored) x=%d, offset=%d, skb_len=%u\n",
					ndev->name, x, offset, skb_len);
				goto error;
			}

			new_skb = netdev_alloc_skb(ndev,  skb_len);
			if (!new_skb) {
				goto error;
			}

			switch (skb_in->data[offset] & 0xf0) {
				case 0x40:
					new_skb->protocol = htons(ETH_P_IP);
				break;
				case 0x60:
					new_skb->protocol = htons(ETH_P_IPV6);
				break;
				default:
					net_err_ratelimited("%s: unknow skb->protocol %02x\n",
						ndev->name, skb_in->data[offset]);
					goto error;
			}
			
			skb_put(new_skb, skb_len);
			memcpy(new_skb->data, skb_in->data + offset, skb_len);

			skb_reset_transport_header(new_skb);
			skb_reset_network_header(new_skb);
			new_skb->pkt_type = PACKET_HOST;
			skb_set_mac_header(new_skb, 0);
 
			__skb_queue_tail(&skb_chain, new_skb);
		}

		/* are there more NDPs to process? */
		ndpoffset = le16_to_cpu(ndp16->wNextNdpIndex);
	}

error:
	while ((new_skb = __skb_dequeue (&skb_chain))) {
		netif_receive_skb(new_skb);
	}	
}



static int mhi_ndo_open(struct net_device *ndev)
{
	struct mhi_net_dev *mhi_netdev = netdev_priv(ndev);

	/* Feed the rx buffer pool */
	schedule_delayed_work(&mhi_netdev->rx_refill, 0);

	/* Carrier is established via out-of-band channel (e.g. qmi) */
	netif_carrier_on(ndev);

	netif_start_queue(ndev);

	return 0;
}

static int mhi_ndo_stop(struct net_device *ndev)
{
	struct mhi_net_dev *mhi_netdev = netdev_priv(ndev);

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	cancel_delayed_work_sync(&mhi_netdev->rx_refill);

	return 0;
}

static int mhi_ndo_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct mhi_net_dev *mhi_netdev = netdev_priv(ndev);
	struct mhi_device *mdev = mhi_netdev->mdev;
	int err;

	err = cdc_mbim_tx_fixup(mhi_netdev, skb, 0);
	if (err) {
		mhi_netdev->stats.tx_dropped++;
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	skb_tx_timestamp(skb);

	/* mhi_queue_skb is not thread-safe, but xmit is serialized by the
	 * network core. Once MHI core will be thread save, migrate to
	 * NETIF_F_LLTX support.
	 */
	err = mhi_queue_skb(mdev, DMA_TO_DEVICE, skb, skb->len, MHI_EOT);
	if (err == -ENOMEM) {
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	} else if (unlikely(err)) {
		net_err_ratelimited("%s: Failed to queue TX buf (%d)\n",
				    ndev->name, err);
		mhi_netdev->stats.tx_dropped++;
		kfree_skb(skb);
	}

	return NETDEV_TX_OK;
}

static void mhi_ndo_get_stats64(struct net_device *ndev,
				struct rtnl_link_stats64 *stats)
{
	struct mhi_net_dev *mhi_netdev = netdev_priv(ndev);

	stats->rx_packets = mhi_netdev->stats.rx_packets;
	stats->rx_bytes = mhi_netdev->stats.rx_bytes;
	stats->rx_errors = mhi_netdev->stats.rx_errors;
	stats->rx_dropped = mhi_netdev->stats.rx_dropped;
	stats->tx_packets = mhi_netdev->stats.tx_packets;
	stats->tx_bytes = mhi_netdev->stats.tx_bytes;
	stats->tx_errors = mhi_netdev->stats.tx_errors;
	stats->tx_dropped = mhi_netdev->stats.tx_dropped;
}

static const struct net_device_ops mhi_netdev_ops = {
	.ndo_open               = mhi_ndo_open,
	.ndo_stop               = mhi_ndo_stop,
	.ndo_start_xmit         = mhi_ndo_xmit,
	.ndo_get_stats64	= mhi_ndo_get_stats64,
};

static void mhi_net_setup(struct net_device *ndev)
{
	ether_setup(ndev);

	ndev->needed_headroom = sizeof(struct cdc_mbim_hdr);
	ndev->header_ops = NULL;  /* No header */
	ndev->type = ARPHRD_NONE;
	ndev->hard_header_len = 0;
	ndev->addr_len = 0;
	ndev->flags |= IFF_NOARP;
	ndev->flags &= ~(IFF_BROADCAST | IFF_MULTICAST);
	ndev->features |= (NETIF_F_VLAN_CHALLENGED); /* Do not support VALN by now */
	ndev->netdev_ops = &mhi_netdev_ops;
}

static void mhi_net_dl_callback(struct mhi_device *mhi_dev,
				struct mhi_result *mhi_res)
{
	struct mhi_net_dev *mhi_netdev = dev_get_drvdata(&mhi_dev->dev);
	struct sk_buff *skb = mhi_res->buf_addr;
	int remaining;

	remaining = atomic_dec_return(&mhi_netdev->stats.rx_queued);

	if (unlikely(mhi_res->transaction_status)) {
		mhi_netdev->stats.rx_errors++;
		kfree_skb(skb);
	} else {
		mhi_netdev->stats.rx_packets++;
		mhi_netdev->stats.rx_bytes += mhi_res->bytes_xferd;

		skb_put(skb, mhi_res->bytes_xferd);
		cdc_mbim_rx_fixup(mhi_netdev, skb);
		kfree_skb(skb);
	}

	/* Refill if RX buffers queue becomes low */
	if (remaining <= mhi_netdev->rx_queue_sz / 2)
		schedule_delayed_work(&mhi_netdev->rx_refill, 0);
}

static void mhi_net_ul_callback(struct mhi_device *mhi_dev,
				struct mhi_result *mhi_res)
{
	struct mhi_net_dev *mhi_netdev = dev_get_drvdata(&mhi_dev->dev);
	struct net_device *ndev = mhi_netdev->ndev;
	struct sk_buff *skb = mhi_res->buf_addr;

	/* Hardware has consumed the buffer, so free the skb (which is not
	 * freed by the MHI stack) and perform accounting.
	 */
	consume_skb(skb);

	if (unlikely(mhi_res->transaction_status)) {
		mhi_netdev->stats.tx_errors++;
	} else {
		mhi_netdev->stats.tx_packets++;
		mhi_netdev->stats.tx_bytes += mhi_res->bytes_xferd;
	}

	if (netif_queue_stopped(ndev))
		netif_wake_queue(ndev);
}

static void mhi_net_rx_refill_work(struct work_struct *work)
{
	struct mhi_net_dev *mhi_netdev = container_of(work, struct mhi_net_dev,
						      rx_refill.work);
	struct net_device *ndev = mhi_netdev->ndev;
	struct mhi_device *mdev = mhi_netdev->mdev;
	struct sk_buff *skb;
	int err;

	do {
		skb = netdev_alloc_skb(ndev, READ_ONCE(mhi_netdev->mbim_ctx.rx_max));
		if (unlikely(!skb))
			break;

		err = mhi_queue_skb(mdev, DMA_FROM_DEVICE, skb, mhi_netdev->mbim_ctx.rx_max,
				    MHI_EOT);
		if (err) {
			if (unlikely(err != -ENOMEM)) {
				net_err_ratelimited("%s: Failed to queue RX buf (%d)\n",
						    ndev->name, err);
			}
			kfree_skb(skb);
			break;
		}

		atomic_inc_return(&mhi_netdev->stats.rx_queued);
	} while (1);

	/* If we're still starved of rx buffers, reschedule latter */
	if (unlikely(!atomic_read(&mhi_netdev->stats.rx_queued)))
		schedule_delayed_work(&mhi_netdev->rx_refill, HZ / 2);
}

static int mhi_net_probe(struct mhi_device *mhi_dev,
			 const struct mhi_device_id *id)
{
	const char *netname = (char *)id->driver_data;
	struct mhi_net_dev *mhi_netdev;
	struct net_device *ndev;
	struct device *dev = &mhi_dev->dev;
	int err;

	ndev = alloc_netdev(sizeof(*mhi_netdev), netname, NET_NAME_PREDICTABLE,
			    mhi_net_setup);
	if (!ndev)
		return -ENOMEM;

	mhi_netdev = netdev_priv(ndev);
	dev_set_drvdata(dev, mhi_netdev);
	mhi_netdev->ndev = ndev;
	mhi_netdev->mdev = mhi_dev;
	mhi_netdev->mbim_ctx.rx_seq = 0;
	mhi_netdev->mbim_ctx.tx_seq = 0;
	mhi_netdev->mbim_ctx.rx_max = ncmNTBParams.dwNtbInMaxSize;
	SET_NETDEV_DEV(ndev, &mhi_dev->dev);

	/* All MHI net channels have 128 ring elements (at least for now) */
	mhi_netdev->rx_queue_sz = 128;

	INIT_DELAYED_WORK(&mhi_netdev->rx_refill, mhi_net_rx_refill_work);

	/* Start MHI channels */
	err = mhi_prepare_for_transfer(mhi_dev);
	if (err)
		goto out_err;

	err = register_netdev(ndev);
	if (err)
		goto out_err;

	return 0;

out_err:
	free_netdev(ndev);
	return err;
}

static void mhi_net_remove(struct mhi_device *mhi_dev)
{
	struct mhi_net_dev *mhi_netdev = dev_get_drvdata(&mhi_dev->dev);

	unregister_netdev(mhi_netdev->ndev);

	mhi_unprepare_from_transfer(mhi_netdev->mdev);

	free_netdev(mhi_netdev->ndev);
}

static const struct mhi_device_id mhi_net_id_table[] = {
	{ .chan = "IP_HW0_MBIM", .driver_data = (kernel_ulong_t)"mhi_hwip%d" },
	{}
};
MODULE_DEVICE_TABLE(mhi, mhi_net_id_table);

static struct mhi_driver mhi_net_driver = {
	.probe = mhi_net_probe,
	.remove = mhi_net_remove,
	.dl_xfer_cb = mhi_net_dl_callback,
	.ul_xfer_cb = mhi_net_ul_callback,
	.id_table = mhi_net_id_table,
	.driver = {
		.name = "mhi_net",
	},
};

module_mhi_driver(mhi_net_driver);

MODULE_AUTHOR("Loic Poulain <loic.poulain@linaro.org>");
MODULE_DESCRIPTION("Network over MHI");
MODULE_LICENSE("GPL v2");
