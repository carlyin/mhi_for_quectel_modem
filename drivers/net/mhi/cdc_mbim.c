// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.*/

#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/netdevice.h>
#include <linux/if_vlan.h>
#include <linux/skbuff.h>
#include <linux/usb/cdc.h>
#include <linux/mhi.h>
#include "mhi_net.h"

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

static int cdc_mbim_bind(struct mhi_net *dev, const struct mhi_device_id *id)
{
	struct cdc_mbim_ctx *ctx = (void *)&dev->data;

	ctx->tx_seq = 0;
	ctx->rx_seq = 0;
	ctx->rx_max = 31*1024;
	dev->mru = ctx->rx_max;

	return 0;
}

static void cdc_mbim_unbind(struct mhi_net *dev)
{
}

/* verify that the ethernet protocol is IPv4 or IPv6 */
static bool is_ip_proto(__be16 proto)
{
	switch (proto) {
	case htons(ETH_P_IP):
	case htons(ETH_P_IPV6):
		return true;
	}
	return false;
}

static struct sk_buff * cdc_mbim_tx_fixup(struct mhi_net *dev,
				struct sk_buff *skb, gfp_t flags)
{
	struct cdc_mbim_ctx *ctx = (void *)&dev->data;
	struct cdc_mbim_hdr *mhdr;
	__le32 sign;
	u8 *c;
	u16 tci = 0;
	bool is_ip;
	unsigned int skb_len;

	if (skb->len <= ETH_HLEN)
		goto error;

	skb_reset_mac_header(skb);

	if (skb->len > VLAN_ETH_HLEN && __vlan_get_tag(skb, &tci) == 0) {
		is_ip = is_ip_proto(vlan_eth_hdr(skb)->h_vlan_encapsulated_proto);
		skb_pull(skb, VLAN_ETH_HLEN);
	} else {
		is_ip = is_ip_proto(eth_hdr(skb)->h_proto);
		skb_pull(skb, ETH_HLEN);
	}

	if (!is_ip)
		goto error;

	if (skb_headroom(skb) < sizeof(struct cdc_mbim_hdr))
		goto error;

	skb_len = skb->len;
	skb_push(skb, sizeof(struct cdc_mbim_hdr));

	mhdr = (struct cdc_mbim_hdr *)skb->data;
	mhdr->nth16.dwSignature = cpu_to_le32(USB_CDC_NCM_NTH16_SIGN);
	mhdr->nth16.wHeaderLength = cpu_to_le16(sizeof(struct usb_cdc_ncm_nth16));
	mhdr->nth16.wSequence = cpu_to_le16(ctx->tx_seq++);
	mhdr->nth16.wBlockLength = cpu_to_le16(skb->len);
	mhdr->nth16.wNdpIndex = cpu_to_le16(sizeof(struct usb_cdc_ncm_nth16));

	if (tci < 256)
		sign = cpu_to_le32(USB_CDC_MBIM_NDP16_IPS_SIGN);
	else if (tci < 512)
		sign = cpu_to_le32(USB_CDC_MBIM_NDP16_DSS_SIGN);
	else
		goto error;

	c = (u8 *)&sign;
	c[3] = tci;

	mhdr->ndp16.dwSignature = sign;
	mhdr->ndp16.wLength = cpu_to_le16(sizeof(struct usb_cdc_ncm_ndp16) + sizeof(struct usb_cdc_ncm_dpe16) * 2);
	mhdr->ndp16.wNextNdpIndex = 0;

	mhdr->ndp16.dpe16[0].wDatagramIndex = cpu_to_le16(sizeof(struct cdc_mbim_hdr));
	mhdr->ndp16.dpe16[0].wDatagramLength = cpu_to_le16(skb_len);

	mhdr->ndp16.dpe16[1].wDatagramIndex = 0;
	mhdr->ndp16.dpe16[1].wDatagramLength = 0;

	return skb;

error:
	dev_kfree_skb_any (skb);
	return NULL;
}

static int cdc_mbim_rx_fixup(struct mhi_net *dev, struct sk_buff *skb_in)
{
	struct net_device *net_dev = dev->net_dev;
	struct cdc_mbim_ctx *ctx = (void *)&dev->data;
	struct usb_cdc_ncm_nth16 *nth16;
	int ndpoffset, len;
	u16 wSequence;
	struct sk_buff_head skb_chain;
	struct sk_buff *new_skb;

	__skb_queue_head_init(&skb_chain);

	if (skb_in->len < (sizeof(struct usb_cdc_ncm_nth16) + sizeof(struct usb_cdc_ncm_ndp16))) {
		net_err_ratelimited("%s: frame too short\n", net_dev->name);
		return 1;
	}

	nth16 = (struct usb_cdc_ncm_nth16 *)skb_in->data;

	if (nth16->dwSignature != cpu_to_le32(USB_CDC_NCM_NTH16_SIGN)) {
		net_err_ratelimited("%s: invalid NTH16 signature <%#010x>\n",
			net_dev->name, le32_to_cpu(nth16->dwSignature));
		goto error;
	}

	len = le16_to_cpu(nth16->wBlockLength);
	if (len > ctx->rx_max) {
		net_err_ratelimited("%s: unsupported NTB block length %u/%u\n",
			net_dev->name, len, ctx->rx_max);
		goto error;
	}

	wSequence = le16_to_cpu(nth16->wSequence);
	if (ctx->rx_seq !=  wSequence) {
		net_err_ratelimited("%s: sequence number glitch prev=%d curr=%d\n",
			net_dev->name, ctx->rx_seq, wSequence);
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
				net_dev->name, ndpoffset);
			goto error;
		}

		ndp16 = (struct usb_cdc_ncm_ndp16 *)(skb_in->data + ndpoffset);

		if (le16_to_cpu(ndp16->wLength) < 0x10) {
			net_err_ratelimited("%s: invalid DPT16 length <%u>\n",
				net_dev->name, le16_to_cpu(ndp16->wLength));
			goto error;
		}

		nframes = ((le16_to_cpu(ndp16->wLength) - sizeof(struct usb_cdc_ncm_ndp16)) / sizeof(struct usb_cdc_ncm_dpe16));

		if (skb_in->len < (sizeof(struct usb_cdc_ncm_ndp16) + nframes * (sizeof(struct usb_cdc_ncm_dpe16)))) {
			net_err_ratelimited("%s: Invalid nframes = %d\n",
				net_dev->name, nframes);
			goto error;
		}

		switch (ndp16->dwSignature & cpu_to_le32(0x00ffffff)) {
			case cpu_to_le32(USB_CDC_MBIM_NDP16_IPS_SIGN):
				c = (u8 *)&ndp16->dwSignature;
				tci = c[3];
			break;
			case cpu_to_le32(USB_CDC_MBIM_NDP16_DSS_SIGN):
				c = (u8 *)&ndp16->dwSignature;
				tci = c[3] + 256;
			break;
			default:
				net_err_ratelimited("%s: unsupported NDP signature <0x%08x>\n",
					net_dev->name, le32_to_cpu(ndp16->dwSignature));
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
					net_dev->name, x, offset, skb_len);
				goto error;
			}

			new_skb = netdev_alloc_skb(net_dev, skb_len + ETH_HLEN);
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
						net_dev->name, skb_in->data[offset]);
					goto error;
			}

			/* add an ethernet header */
			skb_put(new_skb, ETH_HLEN);
			skb_reset_mac_header(new_skb);
			eth_hdr(new_skb)->h_proto = new_skb->protocol;
			eth_zero_addr(eth_hdr(new_skb)->h_source);
			memcpy(eth_hdr(new_skb)->h_dest, net_dev->dev_addr, ETH_ALEN);

			/* add datagram */
			skb_put_data(new_skb, skb_in->data + offset, skb_len);

			/* map MBIM session to VLAN */
			if (tci)
				__vlan_hwaccel_put_tag(new_skb, htons(ETH_P_8021Q), tci);

			__skb_queue_tail(&skb_chain, new_skb);
		}

		/* are there more NDPs to process? */
		ndpoffset = le16_to_cpu(ndp16->wNextNdpIndex);
	}

error:
	while ((new_skb = __skb_dequeue (&skb_chain))) {
		__skb_pull(new_skb, ETH_HLEN);
		netif_receive_skb(new_skb);
	}

	dev_kfree_skb_any(skb_in);
	return 0;
}

static const struct driver_info cdc_mbim_info = {
	.description = "CDC MBIM",
	.net_name = "wwan%d",
	.bind = cdc_mbim_bind,
	.unbind = cdc_mbim_unbind,
	.rx_fixup = cdc_mbim_rx_fixup,
	.tx_fixup = cdc_mbim_tx_fixup,
};

static const struct mhi_device_id products[] = {
	{ .chan = "IP_HW0_MBIM", .driver_data = (kernel_ulong_t)(&cdc_mbim_info) },
	{}
};
MODULE_DEVICE_TABLE(mhi, products);

static struct mhi_driver cdc_mbim_driver = {
	.probe = mhi_net_probe,
	.remove = mhi_net_remove,
	.dl_xfer_cb = mhi_net_dl_callback,
	.ul_xfer_cb = mhi_net_ul_callback,
	.status_cb = mhi_net_status_cb,
	.id_table = products,
	.driver = {
		.name = "cdc_mbim",
	},
};

module_mhi_driver(cdc_mbim_driver);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MHI NET CDC MBIM Driver");
