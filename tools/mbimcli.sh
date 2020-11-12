#!/bin/bash

killall ping
echo "file /root/upstream/github/drivers/bus/mhi/core/*.c +p" > /sys/kernel/debug/dynamic_debug/control
#echo "file /root/upstream/github/drivers/bus/mhi/*.c +p" > /sys/kernel/debug/dynamic_debug/control
mbimdev=/dev/mhi_0000:03:00.0_MBIM
fuser /dev/mhi_0000\:03\:00.0_MBIM
service ModemManager stop
killall -9  mbim-proxy
sleep 1
fuser /dev/mhi_0000\:03\:00.0_MBIM

set -e
mbimcli  -d ${mbimdev} --query-subscriber-ready-status --no-close
mbimcli  -d ${mbimdev} --query-registration-state --no-open=3 --no-close
mbimcli  -d ${mbimdev} --attach-packet-service --no-open=4 --no-close
mbimcli  -d ${mbimdev} --query-packet-service-state --no-open=5 --no-close

mbimcli  -d ${mbimdev} --connect="apn=\"cment0\", session-id=0" --no-open=6 --no-close
mbimcli  -d ${mbimdev} --query-connection-state=0 --no-open=7 --no-close
mbimcli  -d ${mbimdev} --query-ip-configuration=0 --no-open=8 --no-close
ip0v4=`mbimcli -p -d ${mbimdev} --query-ip-configuration=0 --no-open=9 --no-close | grep "IP \[0\]" | awk NR==1 | awk -F "'" {'print $2'}`

mbimcli  -d ${mbimdev} --connect="apn=\"cmnet1\", session-id=1" --no-open=10 --no-close
mbimcli  -d ${mbimdev} --query-connection-state=1 --no-open=11 --no-close
mbimcli  -d ${mbimdev} --query-ip-configuration=1 --no-open=12 --no-close
ip1v4=`mbimcli -p -d ${mbimdev} --query-ip-configuration=1 --no-open=13 --no-close | grep "IP \[0\]" | awk NR==1 | awk -F "'" {'print $2'}`

set -v
echo IPV4[0] = ${ip0v4}
ip addr flush wwan0
ifconfig wwan0 up
ip addr add dev wwan0 ${ip0v4}

echo IPV4[1] = ${ip1v4}
if [ ! -d "/sys/class/net/wwan0.1" ]
then
ip link add link wwan0 name wwan0.1 type vlan id 1
fi
ip addr flush wwan0.1
ifconfig wwan0.1 up
ip addr add dev wwan0.1 ${ip1v4}

echo "done"
ping 8.8.8.8 -I wwan0 -c 3
ping 8.8.8.8 -I wwan0 & 

ip ro add default dev wwan0
wget https://mirrors.aliyun.com/ubuntu-releases/groovy/ubuntu-20.10-desktop-amd64.iso
cat /sys/kernel/debug/mhi/0000\:03\:00.0/channels 
cat /sys/kernel/debug/mhi/0000\:03\:00.0/events
cat /sys/kernel/debug/mhi/0000\:03\:00.0/states
