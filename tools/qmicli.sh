#!/bin/bash

./a.out &
cat /dev/kmsg | grep mhi &
sleep 1

echo "file /root/upstream/github/drivers/bus/mhi/core/*.c +p" > /sys/kernel/debug/dynamic_debug/control
#echo "file /root/upstream/github/drivers/bus/mhi/*.c +p" > /sys/kernel/debug/dynamic_debug/control

function qcli()
{
	echo qmicli --device-open-qmi -d /dev/mhi_0000:03:00.0_QMI $@
	qmicli --device-open-qmi -d /dev/mhi_0000:03:00.0_QMI $@
}

qcli --wda-set-data-format="link-layer-protocol=raw-ip,ep-type=pcie,ep-iface-number=4,ul-protocol=qmapv5,dl-protocol=qmapv5,dl-datagram-max-size=31744,dl-max-datagrams=63"
qcli --wda-get-data-format="ep-type=pcie,ep-iface-number=4"

cid=`qcli --client-no-release-cid --wds-noop | grep CID | awk NR==1 | awk -F "'" {'print $2'}`
qcli --client-cid=${cid} --client-no-release-cid --wds-set-ip-family=4
qcli --client-cid=${cid} --client-no-release-cid --wds-bind-mux-data-port="ep-type=pcie,ep-iface-number=4,mux-id=129"
qcli --client-cid=${cid} --client-no-release-cid --wds-start-network="apn=cmnet"
qcli --client-cid=${cid} --client-no-release-cid --wds-get-packet-service-status

insmod drivers/net/mhi/mhi_net.ko
insmod drivers/net/mhi/qmi_wwan.ko
busybox udhcpc -fnq -i wwan0

ip ro add default dev wwan0
wget https://mirrors.aliyun.com/ubuntu-releases/groovy/ubuntu-20.10-desktop-amd64.iso
