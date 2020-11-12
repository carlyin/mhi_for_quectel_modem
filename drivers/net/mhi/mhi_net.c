// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.*/

#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/mhi.h>
#include "mhi_net.h"

static void rx_alloc_submit(struct mhi_net *dev)
{
	struct sk_buff *skb;
	int no_tre;
	int i;
	int ret;

	no_tre = mhi_get_free_desc_count(dev->mhi_dev, DMA_FROM_DEVICE);

	for (i = 0; i < no_tre; i++) {
		skb = netdev_alloc_skb(dev->net_dev, dev->mru);
		if (unlikely(!skb)) {
			net_err_ratelimited("%s: Failed to alloc RX skb\n",
					    dev->net_dev->name);
			break;
		}

		ret = mhi_queue_skb(dev->mhi_dev, DMA_FROM_DEVICE, skb,
					 dev->mru, MHI_EOT);

		if (ret) {
			net_err_ratelimited("%s: Failed to queue RX buf (%d)\n",
					    dev->net_dev->name, ret);
			kfree_skb(skb);
			break;
		}
	}

	if (no_tre > 64)
		printk("%s %d/%d\n", "q ", i, no_tre);
	if (i < no_tre) {
		printk("%s %d/%d\n", "q ", i, no_tre);
		schedule_delayed_work(&dev->rx_refill, 1);
	}
}

static void mhi_net_rx_refill_work(struct work_struct *work)
{
	struct mhi_net *dev = container_of(work, struct mhi_net,
						      rx_refill.work);

	rx_alloc_submit(dev);
}

static void dev_fetch_sw_netstats(struct rtnl_link_stats64 *s,
			   const struct pcpu_sw_netstats __percpu *netstats)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		const struct pcpu_sw_netstats *stats;
		struct pcpu_sw_netstats tmp;
		unsigned int start;

		stats = per_cpu_ptr(netstats, cpu);
		do {
			start = u64_stats_fetch_begin_irq(&stats->syncp);
			tmp.rx_packets = stats->rx_packets;
			tmp.rx_bytes   = stats->rx_bytes;
			tmp.tx_packets = stats->tx_packets;
			tmp.tx_bytes   = stats->tx_bytes;
		} while (u64_stats_fetch_retry_irq(&stats->syncp, start));

		s->rx_packets += tmp.rx_packets;
		s->rx_bytes   += tmp.rx_bytes;
		s->tx_packets += tmp.tx_packets;
		s->tx_bytes   += tmp.tx_bytes;
	}
}

int mhi_net_open (struct net_device *net)
{
	struct mhi_net *dev = netdev_priv(net);
#if 0 
	int			ret;

	/* Start MHI channels */
	ret = mhi_prepare_for_transfer(dev->mhi_dev);
	if (ret)
		return ret;
	
	rx_alloc_submit(dev);
#endif

	netif_carrier_on(net);
	netif_start_queue (net);
	napi_enable(&dev->napi);

	return 0;
}

int mhi_net_stop (struct net_device *net)
{
	struct mhi_net *dev = netdev_priv(net);
	struct mhi_device *mhi_dev = dev->mhi_dev;
	struct sk_buff *skb;

	netif_stop_queue(net);
	netif_carrier_off(net);
	cancel_delayed_work_sync(&dev->rx_refill);
	mhi_unprepare_from_transfer(mhi_dev);

	while ((skb = skb_dequeue (&dev->rx_pending)))
		dev_kfree_skb_any(skb);

	return 0;
}

int mhi_net_xmit(struct sk_buff *skb, struct net_device *net_dev)
{
	struct mhi_net *dev = netdev_priv(net_dev);
	struct mhi_device *mhi_dev = dev->mhi_dev;
	const struct driver_info *info = dev->driver_info;
	int no_tre;
	int err;

	skb_tx_timestamp(skb);

	no_tre = mhi_get_free_desc_count(mhi_dev, DMA_TO_DEVICE);
	if (no_tre == 0) {
		netif_stop_queue(net_dev);
		return NETDEV_TX_BUSY;
	} else if (no_tre == 1) {
		netif_stop_queue(net_dev);
	}

	if (info->tx_fixup) {
		skb = info->tx_fixup (dev, skb, GFP_ATOMIC);
		if (!skb) {
			goto drop;
		}
	}
	
	err = mhi_queue_skb(mhi_dev, DMA_TO_DEVICE, skb, skb->len, MHI_EOT);
	if (unlikely(err)) {
		kfree_skb(skb);
		netif_stop_queue(net_dev);
		goto drop;
	}

	return NETDEV_TX_OK;

drop:
	net_dev->stats.tx_dropped++;

	return NETDEV_TX_OK;
}

static int mhi_net_poll(struct napi_struct *napi, int budget)
{
	struct net_device *net_dev = napi->dev;
	struct mhi_net *dev = netdev_priv(net_dev);
	struct mhi_device *mhi_dev = dev->mhi_dev;
	const struct driver_info *info = dev->driver_info;
	struct sk_buff		*skb;
	int rx_work = 0;
	static int max_rx_work = 0;

	rx_work = mhi_poll(mhi_dev, budget);
	if (rx_work > max_rx_work) {
		printk("rx_work=%d\n", rx_work);
		max_rx_work = rx_work;
	}

	
	if (rx_work < 0) {
		napi_complete(napi);
		return 0;
	}

	while ((skb = skb_dequeue (&dev->rx_pending))) {
		if (info->rx_fixup)
			info->rx_fixup(dev, skb);
	}

	rx_alloc_submit(dev);
	
	if (rx_work < budget)
		napi_complete(napi);

	return rx_work;
}

void mhi_net_get_stats64(struct net_device *net_dev, struct rtnl_link_stats64 *stats)
{
	struct mhi_net *dev = netdev_priv(net_dev);

	netdev_stats_to_stats64(stats, &net_dev->stats);
	dev_fetch_sw_netstats(stats, dev->stats64);
}

int mhi_net_change_mtu (struct net_device *net, int new_mtu)
{
	net->mtu = new_mtu;

	return 0;
}

static const struct net_device_ops mhi_net_ops = {
	.ndo_open = mhi_net_open,
	.ndo_stop		= mhi_net_stop,
	.ndo_start_xmit = mhi_net_xmit,
	.ndo_get_stats64	= mhi_net_get_stats64,
	.ndo_change_mtu		= mhi_net_change_mtu,
};

static void mhi_net_setup(struct net_device *net_dev)
{
	const unsigned char node_id[ETH_ALEN] = {0x02, 0x50, 0xf4, 0x00, 0x00, 0x00};

	ether_setup(net_dev);

	net_dev->needed_headroom = 16;
	net_dev->flags           |=  IFF_NOARP;
	net_dev->flags           &= ~(IFF_BROADCAST | IFF_MULTICAST);
	net_dev->netdev_ops = &mhi_net_ops;
	memcpy (net_dev->dev_addr, node_id, sizeof node_id);
}

int mhi_net_probe(struct mhi_device *mhi_dev, const struct mhi_device_id *id)
{
	struct mhi_net *dev;
	struct net_device	 *net;
	const struct driver_info	*info;
	int				status;
	const char			*name;

	info = (const struct driver_info *) id->driver_data;
	if (!info) {
		dev_dbg (&mhi_dev->dev, "blacklisted by %s\n", name);
		return -ENODEV;
	}

	net = alloc_netdev(sizeof(struct mhi_net),
			       info->net_name, NET_NAME_PREDICTABLE, mhi_net_setup);
	if (!net)
		return -ENOBUFS;

	/* netdev_printk() needs this so do it as early as possible */
	SET_NETDEV_DEV(net, &mhi_dev->dev);

	dev = netdev_priv(net);
	dev->mhi_dev = mhi_dev;
	dev->driver_info = info;
	dev->net_dev = net;
	dev->mru = 1500;

	skb_queue_head_init(&dev->rx_pending);
	INIT_DELAYED_WORK(&dev->rx_refill, mhi_net_rx_refill_work);

	dev->stats64 = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
	if (!dev->stats64) {
		status = -ENOMEM;
		goto _free_netdev;
	}

	if (info->bind) {
		status = info->bind (dev, id);
		if (status < 0)
			goto _free_stats64;
	}

	dev_set_drvdata(&mhi_dev->dev, dev);
	netif_napi_add(net, &dev->napi, mhi_net_poll, NAPI_POLL_WEIGHT);
	status = register_netdev (net);
	if (status)
		goto _free_stats64;

	netif_device_attach (net);

#if 1 
	/* Start MHI channels */
	mhi_prepare_for_transfer(dev->mhi_dev);
	rx_alloc_submit(dev);
#endif

	return 0;

_free_stats64:
	free_percpu(dev->stats64);
_free_netdev:
	free_netdev(net);
	
	return status;
}
EXPORT_SYMBOL_GPL(mhi_net_probe);

void mhi_net_remove(struct mhi_device *mhi_dev)
{
	struct mhi_net *dev = dev_get_drvdata(&mhi_dev->dev);

	napi_disable(&dev->napi);
	netif_napi_del(&dev->napi);
	unregister_netdev(dev->net_dev);
	free_percpu(dev->stats64);
	free_netdev(dev->net_dev);

	mhi_unprepare_from_transfer(dev->mhi_dev);	
}
EXPORT_SYMBOL_GPL(mhi_net_remove);

void mhi_net_dl_callback(struct mhi_device *mhi_dev,
				struct mhi_result *mhi_res)
{
	struct mhi_net *dev = dev_get_drvdata(&mhi_dev->dev);
	struct net_device *net_dev = dev->net_dev;
	struct sk_buff *skb = mhi_res->buf_addr;

	if (unlikely(mhi_res->transaction_status)) {
		if (mhi_res->transaction_status != -ENOTCONN)
			net_dev->stats.rx_errors++;
		kfree_skb(skb);
	} else {
		struct pcpu_sw_netstats *stats64 = this_cpu_ptr(dev->stats64);
		unsigned long flags;
		static u32 bytes_xferd = 0;
		if (mhi_res->bytes_xferd > bytes_xferd) {
			bytes_xferd = mhi_res->bytes_xferd;
			printk("%s bytes_xferd=%u\n", __func__, bytes_xferd);
		}

		flags = u64_stats_update_begin_irqsave(&stats64->syncp);
		stats64->rx_packets++;
		stats64->rx_bytes += skb->len;
		u64_stats_update_end_irqrestore(&stats64->syncp, flags);

		skb_put(skb, mhi_res->bytes_xferd);
		skb_queue_tail(&dev->rx_pending, skb);
	}
}
EXPORT_SYMBOL_GPL(mhi_net_dl_callback);

void mhi_net_ul_callback(struct mhi_device *mhi_dev,
				struct mhi_result *mhi_res)
{
	struct mhi_net *dev = dev_get_drvdata(&mhi_dev->dev);
	struct net_device *net_dev = dev->net_dev;
	struct sk_buff *skb = mhi_res->buf_addr;

	if (unlikely(mhi_res->transaction_status)) {
		if (mhi_res->transaction_status != -ENOTCONN)
			net_dev->stats.tx_errors++;
	} else {
		struct pcpu_sw_netstats *stats64 = this_cpu_ptr(dev->stats64);
		unsigned long flags;

		flags = u64_stats_update_begin_irqsave(&stats64->syncp);
		stats64->tx_packets++;
		stats64->tx_bytes += skb->len;
		u64_stats_update_end_irqrestore(&stats64->syncp, flags);
	}

	consume_skb(skb);

	if (netif_queue_stopped(net_dev))
		netif_wake_queue(net_dev);
}
EXPORT_SYMBOL_GPL(mhi_net_ul_callback);

void mhi_net_status_cb(struct mhi_device *mhi_dev, enum mhi_callback mhi_cb)
{
	struct mhi_net *dev = dev_get_drvdata(&mhi_dev->dev);

	if (mhi_cb != MHI_CB_PENDING_DATA)
		return;

	if (napi_schedule_prep(&dev->napi)) {
		__napi_schedule(&dev->napi);
	}
}
EXPORT_SYMBOL_GPL(mhi_net_status_cb);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MHI NET Driver");

