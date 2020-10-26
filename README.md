# mhi_for_quectel_modem

usage:

# uname -r
5.8.0-23-generic  
  
# lsb_release  -a
No LSB modules are available.  
Distributor ID:	Ubuntu  
Description:	Ubuntu 20.04 LTS  
Release:	20.04  
Codename:	focal  
  
# make
# insmod drivers/bus/mhi/core/mhi.ko
# insmod drivers/bus/mhi/mhi_pci_generic.ko
# insmod drivers/bus/mhi/mhi_uci.ko
# insmod drivers/bus/mhi/mhi_net_mbim.ko
  
# lspci
01:00.0 PCI bridge: Texas Instruments XIO2001 PCI Express-to-PCI Bridge  
03:00.0 Unassigned class [ff00]: Qualcomm Device 0306  
  
# ls -l /dev/mhi_0000\:03\:00.0_*
crw------- 1 root root 240, 1 10月 26 16:36 /dev/mhi_0000:03:00.0_DIAG  
crw------- 1 root root 240, 3 10月 26 16:36 /dev/mhi_0000:03:00.0_DUN  
crw------- 1 root root 240, 0 10月 26 16:36 /dev/mhi_0000:03:00.0_LOOPBACK  
crw------- 1 root root 240, 2 10月 26 16:36 /dev/mhi_0000:03:00.0_MBIM  
  
# ifconfig mhi_hwip0
mhi_hwip0: flags=193<UP,RUNNING,NOARP>  mtu 1500  
        inet 10.32.70.164  netmask 255.255.255.248  
  
# mbim-network /dev/mhi_0000\:03\:00.0_MBIM start
Profile at '/etc/mbim-network.conf' not found...  
Querying subscriber ready status 'mbimcli -d /dev/mhi_0000:03:00.0_MBIM --query-subscriber-ready-status --no-close '...  
[26 10月 2020, 16:39:05] -Warning ** [/dev/mhi_0000:03:00.0_MBIM] Couldn't find udev device  
[26 10月 2020, 16:39:05] -Warning ** [/dev/mhi_0000:03:00.0_MBIM] Couldn't get descriptors file path  
[/dev/mhi_0000:03:00.0_MBIM] Subscriber ready status retrieved: Ready state: 'initialized' Subscriber ID: '460028563800461' SIM ICCID: '89860015120716380461' Ready info: 'none' Telephone numbers: (0) 'unknown' [/dev/mhi_0000:03:00.0_MBIM] Session not closed: TRID: '3'  
Saving state at /tmp/mbim-network-state-mhi_0000:03:00.0_MBIM... (TRID: 3)  
Querying registration state 'mbimcli -d /dev/mhi_0000:03:00.0_MBIM --query-registration-state --no-open=3 --no-close '...  
[26 10月 2020, 16:39:06] -Warning ** [/dev/mhi_0000:03:00.0_MBIM] Couldn't find udev device  
[26 10月 2020, 16:39:06] -Warning ** [/dev/mhi_0000:03:00.0_MBIM] Couldn't get descriptors file path  
[/dev/mhi_0000:03:00.0_MBIM] Registration status: Network error: 'unknown' Register state: 'home' Register mode: 'automatic' Available data classes: 'lte' Current cellular class: 'gsm' Provider ID: '46000' Provider name: 'CMCC' Roaming text: 'unknown' Registration flags: 'packet-service-automatic-attach' [/dev/mhi_0000:03:00.0_MBIM] Session not closed: TRID: '4'  
Saving state at /tmp/mbim-network-state-mhi_0000:03:00.0_MBIM... (TRID: 4)  
Attaching to packet service with 'mbimcli -d /dev/mhi_0000:03:00.0_MBIM --attach-packet-service --no-open=4 --no-close '...  
[26 10月 2020, 16:39:06] -Warning ** [/dev/mhi_0000:03:00.0_MBIM] Couldn't find udev device  
[26 10月 2020, 16:39:06] -Warning ** [/dev/mhi_0000:03:00.0_MBIM] Couldn't get descriptors file path  
Saving state at /tmp/mbim-network-state-mhi_0000:03:00.0_MBIM... (TRID: 5)  
Starting network with 'mbimcli -d /dev/mhi_0000:03:00.0_MBIM --connect=apn='' --no-open=5 --no-close '...  
[26 10月 2020, 16:39:06] -Warning ** [/dev/mhi_0000:03:00.0_MBIM] Couldn't find udev device  
[26 10月 2020, 16:39:06] -Warning ** [/dev/mhi_0000:03:00.0_MBIM] Couldn't get descriptors file path  
Network started successfully  
  
# busybox udhcpc -fnq -i mhi_hwip0  
udhcpc: started, v1.30.1  
udhcpc: sending discover  
udhcpc: sending select for 10.32.70.164  
udhcpc: lease of 10.32.70.164 obtained, lease time 7200  
/etc/udhcpc/default.script: Resetting default routes  
SIOCDELRT: No such process  
/etc/udhcpc/default.script: Adding DNS 211.138.180.2  
/etc/udhcpc/default.script: Adding DNS 211.138.180.3  
