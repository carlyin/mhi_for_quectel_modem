#!/bin/bash

mbimdev=/dev/mhi_0000:03:00.0_MBIM
service ModemManager stop
killall mbim-proxy

set -e
mbimcli -p -d ${mbimdev} --query-subscriber-ready-status --no-close
mbimcli -p -d ${mbimdev} --query-registration-state --no-open=3 --no-close
mbimcli -p -d ${mbimdev} --attach-packet-service --no-open=4 --no-close
mbimcli -p -d ${mbimdev} --query-packet-service-state --no-open=5 --no-close

mbimcli -p -d ${mbimdev} --connect="apn=\"cment0\", session-id=0" --no-open=6 --no-close
mbimcli -p -d ${mbimdev} --query-connection-state=0 --no-open=7 --no-close
mbimcli -p -d ${mbimdev} --query-ip-configuration=0 --no-open=8 --no-close
ipv4=`mbimcli -p -d ${mbimdev} --query-ip-configuration=0 --no-open=9 --no-close | grep "IP \[0\]" | awk NR==1 | awk -F "'" {'print $2'}`
echo IPV4[0] = ${ipv4}
ifconfig wwan0 ${ipv4}

mbimcli -p -d ${mbimdev} --connect="apn=\"cmnet1\", session-id=1" --no-open=10 --no-close
mbimcli -p -d ${mbimdev} --query-connection-state=1 --no-open=11 --no-close
mbimcli -p -d ${mbimdev} --query-ip-configuration=1 --no-open=12 --no-close
ipv4=`mbimcli -p -d ${mbimdev} --query-ip-configuration=1 --no-open=13 --no-close | grep "IP \[0\]" | awk NR==1 | awk -F "'" {'print $2'}`
echo IPV4[1] = ${ipv4}
ip link add link wwan0 name wwan0.1 type vlan id 1
ifconfig wwan0.1 ${ipv4}
