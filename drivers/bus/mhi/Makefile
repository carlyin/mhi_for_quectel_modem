ccflags-y += -g -DCONFIG_MHI_BUS_DEBUG
# core layer

obj-y += core/

# MHI client
mhi_uci-y := uci.o
obj-$(CONFIG_MHI_UCI) += mhi_uci.o
obj-$(CONFIG_MHI_NET_MBIM) += mhi_net_mbim.o

obj-$(CONFIG_MHI_BUS_PCI_GENERIC) += mhi_pci_generic.o
mhi_pci_generic-y += pci_generic.o


obj-m += mhi_dtr.o
