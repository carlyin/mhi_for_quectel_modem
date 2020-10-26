export CONFIG_MHI_BUS=m
export CONFIG_MHI_UCI=m
export CONFIG_MHI_NET_MBIM=m
export CONFIG_MHI_BUS_PCI_GENERIC=m
export CONFIG_MHI_BUS_DEBUG=y

KDIR := /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

modules: clean
	$(MAKE) -C $(KDIR) M=$(PWD)/drivers/bus/mhi modules

install: modules
	mkdir -p /lib/modules/$(shell uname -r)/kernel/drivers/bus/mhi/core
	cp ${PWD}/drivers/bus/mhi/core/mhi.ko /lib/modules/$(shell uname -r)/kernel/drivers/bus/mhi/core
	cp ${PWD}/drivers/bus/mhi/mhi_pci_generic.ko /lib/modules/$(shell uname -r)/kernel/drivers/bus/mhi
	cp ${PWD}/drivers/bus/mhi/mhi_uci.ko /lib/modules/$(shell uname -r)/kernel/drivers/bus/mhi
	cp ${PWD}/drivers/bus/mhi/mhi_net_mbim.ko /lib/modules/$(shell uname -r)/kernel/drivers/bus/mhi
	depmod

uninstall: clean
	rm -rf  /lib/modules/$(shell uname -r)/kernel/drivers/mhi

clean:
	rm -rf *~ .tmp_versions modules.order Module.symvers
	find . -type f -name *~ -o -name *.o -o -name *.ko -o -name *.cmd -o -name *.mod.c |  xargs rm -rf
