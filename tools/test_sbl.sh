#!/bin/bash

echo "file /root/upstream/mhi_for_quectel_modem/drivers/bus/mhi/core/*.c +p" > /sys/kernel/debug/dynamic_debug/control
killall cat
cat /dev/kmsg > kmsg.txt &

t=0
while [ ${t} -lt 10 ]
do

while [ ! -c /dev/mhi_0000\:03\:00.0_DIAG ]
do
sleep 2
done

let t=t+1
echo `date "+%H%M%S"` t=${t}
./ramdump
done
