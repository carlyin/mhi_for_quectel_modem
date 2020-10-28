#!/bin/bash

modprobe -r mhi_uci mhi_net_mbim mhi_pci_generic mhi
modprobe -a mhi_uci mhi_net_mbim mhi_pci_generic mhi
echo "file /home/carl/mhi_for_quectel_modem/drivers/bus/mhi/core/*.c +p" > /sys/kernel/debug/dynamic_debug/control
#echo "file /home/carl/mhi_for_quectel_modem/drivers/bus/mhi/*.c +p" > /sys/kernel/debug/dynamic_debug/control
