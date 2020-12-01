// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.*/

#ifndef	__LINUX_MHI_NET_H
#define	__LINUX_MHI_NET_H

struct mhi_net;

struct driver_info {
	char		*description;
	char          *net_name;
	/* init device ... can sleep, or cause probe() failure */
	int	(*bind)(struct mhi_net *dev, const struct mhi_device_id *id);
	/* cleanup device ... can sleep, but can't fail */
	void	(*unbind)(struct mhi_net *dev);
	/* fixup rx packet (strip framing) */
	int	(*rx_fixup)(struct mhi_net *dev, struct sk_buff *skb);
	/* fixup tx packet (add framing) */
	struct sk_buff * (*tx_fixup)(struct mhi_net *dev,
				struct sk_buff *skb, gfp_t flags);
};

struct mhi_net {
	struct mhi_device *mhi_dev;
	struct net_device *net_dev;
	struct napi_struct napi;
	const struct driver_info *driver_info;
	struct pcpu_sw_netstats __percpu *stats64;
	struct sk_buff_head rx_pending;
	struct delayed_work rx_refill;

	struct mutex chan_lock;
	spinlock_t rx_lock;
	unsigned long		flags;
#		define EVENT_DEV_OPEN	       1
#		define EVENT_CHAN_ENABLE	2
#		define EVENT_CHAN_HALT  	3
	size_t			mru;
	unsigned long		data[5];
};

extern int mhi_net_probe(struct mhi_device *mhi_dev, const struct mhi_device_id *id);
extern void mhi_net_remove(struct mhi_device *mhi_dev);
extern void mhi_net_dl_callback(struct mhi_device *mhi_dev,
				struct mhi_result *mhi_res);
extern void mhi_net_ul_callback(struct mhi_device *mhi_dev,
				struct mhi_result *mhi_res);
extern void mhi_net_status_cb(struct mhi_device *mhi_dev, enum mhi_callback mhi_cb);

/* verify that the ethernet protocol is IPv4 or IPv6 */
static inline bool is_ip_proto(__be16 proto)
{
	switch (proto) {
	case htons(ETH_P_IP):
	case htons(ETH_P_IPV6):
		return true;
	}
	return false;
}
#endif
