export CONFIG_MHI_BUS=m
export CONFIG_MHI_UCI=m
export CONFIG_MHI_NET=m
export CONFIG_MHI_NET_CDC_MBIM=m
export CONFIG_MHI_NET_QMI_WWAN=m
#export CONFIG_MHI_NET_MBIM=m
export CONFIG_MHI_BUS_PCI_GENERIC=m
export CONFIG_MHI_BUS_DEBUG=y

KDIR := /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

obj-m += drivers/bus/mhi/
obj-m += drivers/net/mhi/

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

install: modules
	mkdir -p /lib/modules/$(shell uname -r)/kernel/drivers/bus/mhi/core
	mkdir -p /lib/modules/$(shell uname -r)/kernel/drivers/net/mhi
	cp ${PWD}/drivers/bus/mhi/core/*.ko /lib/modules/$(shell uname -r)/kernel/drivers/bus/mhi/core
	cp ${PWD}/drivers/bus/mhi/*.ko /lib/modules/$(shell uname -r)/kernel/drivers/bus/mhi
	cp ${PWD}/drivers/net/mhi/*.ko /lib/modules/$(shell uname -r)/kernel/drivers/net/mhi
	depmod

uninstall: clean
	rm -rf  /lib/modules/$(shell uname -r)/kernel/drivers/mhi

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
