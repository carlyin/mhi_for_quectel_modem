// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.*/

#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/netdevice.h>
#include <linux/if_vlan.h>
#include <linux/skbuff.h>
#include <linux/mhi.h>
#include "mhi_net.h"

#define QMAP_DEFAULT_MUX_ID 0x81

struct qmi_wwan_ctx {
	u8 mux_version;
	u32 rx_max;
};

enum qmap_version {
	QMAP_V1 = 5,
	QMAP_V5 = 9
};

enum qmap_v5_header_type {
	QMAP_HEADER_TYPE_UNKNOWN,
	QMAP_HEADER_TYPE_COALESCING = 0x1,
	QMAP_HEADER_TYPE_CSUM_OFFLOAD = 0x2,
	QMAP_HEADER_TYPE_ENUM_LENGTH
};

struct qmap_hdr {
#if defined(__LITTLE_ENDIAN_BITFIELD)
	u8  pad_len:6;
	u8  next_hdr:1;
	u8  cd_bit:1;
#elif defined (__BIG_ENDIAN_BITFIELD)
	u8  cd_bit:1;
	u8  next_hdr:1;
	u8  pad_len:6;
#else
#error	"Please fix <asm/byteorder.h>"
#endif
	u8  mux_id;
	__be16 pkt_len;
} __attribute__ ((packed));

struct qmap_v5_csum_hdr {
#if defined(__LITTLE_ENDIAN_BITFIELD)
	u8  next_hdr:1;
	u8  header_type:7;
	u8  hw_reserved:7;
	u8  csum_valid_required:1;
#elif defined (__BIG_ENDIAN_BITFIELD)
	u8  header_type:7;
	u8  next_hdr:1;
	u8  csum_valid_required:1;
	u8  hw_reserved:7;
#else
#error	"Please fix <asm/byteorder.h>"
#endif
	__be16 reserved;
} __attribute__ ((packed));

static int qmi_wwan_bind(struct mhi_net *dev, const struct mhi_device_id *id)
{
	struct qmi_wwan_ctx *ctx = (void *)&dev->data;

	if (!strcmp(id->chan, "IP_HW0_QMAPV1"))
		ctx->mux_version = QMAP_V1;
	else if (!strcmp(id->chan, "IP_HW0_QMAPV5"))
		ctx->mux_version = QMAP_V5;
	else
		return -EINVAL;
	ctx->rx_max = 31*1024;
	dev->mru = ctx->rx_max;

	return 0;
}

static void qmi_wwan_unbind(struct mhi_net *dev)
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

static struct sk_buff * qmi_wwan_tx_fixup(struct mhi_net *dev,
				struct sk_buff *skb, gfp_t flags)
{
	struct qmi_wwan_ctx *ctx = (void *)&dev->data;
	struct qmap_hdr *qhdr;
	struct qmap_v5_csum_hdr *qv5hdr;
	unsigned skb_len;
	u16 mux_id = QMAP_DEFAULT_MUX_ID;
	bool is_ip;

	if (skb->len <= ETH_HLEN)
		goto error;

	skb_reset_mac_header(skb);

	if (skb->len > VLAN_ETH_HLEN && __vlan_get_tag(skb, &mux_id) == 0) {
		if (mux_id >= QMAP_DEFAULT_MUX_ID)
			goto error;

		is_ip = is_ip_proto(vlan_eth_hdr(skb)->h_vlan_encapsulated_proto);
		skb_pull(skb, VLAN_ETH_HLEN);
	} else {
		is_ip = is_ip_proto(eth_hdr(skb)->h_proto);
		skb_pull(skb, ETH_HLEN);
	}

	if (!is_ip)
		goto error;

	skb_len = skb->len;
	if (ctx->mux_version == QMAP_V1) {
		qhdr = (struct qmap_hdr *)skb_push(skb, sizeof(*qhdr));

		qhdr->pad_len = 0;
		qhdr->next_hdr = 1;
		qhdr->cd_bit = 0;
		qhdr->mux_id = mux_id;
		qhdr->pkt_len = cpu_to_be16(skb_len);
	} else if (ctx->mux_version == QMAP_V5) {
		qhdr = (struct qmap_hdr *)skb_push(skb, sizeof(*qhdr) + sizeof(*qv5hdr));
		qv5hdr = (struct qmap_v5_csum_hdr *)(qhdr + 1);

		qhdr->pad_len = 0;
		qhdr->next_hdr = 1;
		qhdr->cd_bit = 0;
		qhdr->mux_id = mux_id;
		qhdr->pkt_len = cpu_to_be16(skb_len);

		qv5hdr->next_hdr = 0;
		qv5hdr->header_type = QMAP_HEADER_TYPE_CSUM_OFFLOAD;
		qv5hdr->hw_reserved = 0;
		qv5hdr->csum_valid_required = 0;
		qv5hdr->reserved = 0;
	} else {
		goto error;
	}

	return skb;

error:
	dev_kfree_skb_any (skb);
	return NULL;
}

static int qmi_wwan_rx_fixup(struct mhi_net *dev, struct sk_buff *skb_in)
{
	struct net_device *net_dev = dev->net_dev;
	struct sk_buff_head skb_chain;
	struct sk_buff *new_skb;
	struct qmap_hdr *qhdr;
	struct qmap_v5_csum_hdr *qv5hdr;
	uint pkt_len;
	uint skb_len;
	size_t hdr_size;

	__skb_queue_head_init(&skb_chain);

	while (skb_in->len > sizeof(*qhdr)) {
		qhdr = (struct qmap_hdr *)skb_in->data;
		hdr_size = sizeof(*qhdr);
		if (qhdr->next_hdr) {
			qv5hdr = (struct qmap_v5_csum_hdr *)(qhdr + 1);
			hdr_size += sizeof(*qv5hdr);
		} else {
			qv5hdr = NULL;
		}
		pkt_len = be16_to_cpu(qhdr->pkt_len);
		skb_len = pkt_len - qhdr->pad_len;

		if (skb_in->len < (pkt_len + hdr_size))
			goto error_pkt;

		if (qhdr->cd_bit)
			goto skip_pkt;

		new_skb = netdev_alloc_skb(net_dev, skb_len + ETH_HLEN);
		if (!new_skb)
			goto error_pkt;

		switch (skb_in->data[hdr_size] & 0xf0) {
			case 0x40:
				new_skb->protocol = htons(ETH_P_IP);
			break;
			case 0x60:
				new_skb->protocol = htons(ETH_P_IPV6);
			break;
			default:
				goto error_pkt;
		}

		/* add an ethernet header */
		skb_put(new_skb, ETH_HLEN);
		skb_reset_mac_header(new_skb);
		eth_hdr(new_skb)->h_proto = new_skb->protocol;
		eth_zero_addr(eth_hdr(new_skb)->h_source);
		memcpy(eth_hdr(new_skb)->h_dest, net_dev->dev_addr, ETH_ALEN);

		/* add datagram */
		skb_put_data(new_skb, skb_in->data + hdr_size, skb_len);

		/* map mux_id to VLAN */
		if (qhdr->mux_id != QMAP_DEFAULT_MUX_ID)
			__vlan_hwaccel_put_tag(new_skb, htons(ETH_P_8021Q), qhdr->mux_id);

		__skb_queue_tail(&skb_chain, new_skb);
skip_pkt:
		skb_pull(skb_in, pkt_len + hdr_size);
	}

error_pkt:
	while ((new_skb = __skb_dequeue (&skb_chain))) {
		__skb_pull(new_skb, ETH_HLEN);
		netif_receive_skb(new_skb);
	}

	kfree_skb(skb_in);
	return 0;
}

static const struct driver_info qmi_wwan_info = {
	.description = "MHI NET QMI WWAN",
	.net_name = "wwan%d",
	.bind = qmi_wwan_bind,
	.unbind = qmi_wwan_unbind,
	.rx_fixup = qmi_wwan_rx_fixup,
	.tx_fixup = qmi_wwan_tx_fixup,
};

static const struct mhi_device_id products[] = {
	{ .chan = "IP_HW0_QMAPV1", .driver_data = (kernel_ulong_t)(&qmi_wwan_info) },
	{ .chan = "IP_HW0_QMAPV5", .driver_data = (kernel_ulong_t)(&qmi_wwan_info) },
	{}
};
MODULE_DEVICE_TABLE(mhi, products);

static struct mhi_driver qmi_wwan_driver = {
	.probe = mhi_net_probe,
	.remove = mhi_net_remove,
	.dl_xfer_cb = mhi_net_dl_callback,
	.ul_xfer_cb = mhi_net_ul_callback,
	.status_cb = mhi_net_status_cb,
	.id_table = products,
	.driver = {
		.name = "qmi_wwan",
	},
};

module_mhi_driver(qmi_wwan_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MHI NET QMI WWAN Driver");

