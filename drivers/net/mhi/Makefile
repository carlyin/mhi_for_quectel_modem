# SPDX-License-Identifier: GPL-2.0
#
# Makefile for MHI Network drivers
#

ccflags-y += -g
obj-$(CONFIG_MHI_NET)	+= mhi_net.o
obj-$(CONFIG_MHI_NET_QMI_WWAN)	+= mhi_net_qmi_wwan.o
mhi_net_qmi_wwan-y+=qmi_wwan.o
obj-$(CONFIG_MHI_NET_CDC_MBIM)	+= mhi_net_cdc_mbim.o
mhi_net_cdc_mbim-y+=cdc_mbim.o
