list="
include/linux/mhi.h
include/linux/mod_devicetable.h
drivers/bus/mhi/core/pm.c
drivers/bus/mhi/core/debugfs.c
drivers/bus/mhi/core/init.c
drivers/bus/mhi/core/boot.c
drivers/bus/mhi/core/main.c
drivers/bus/mhi/core/internal.h
"


set -e
for l in ${list}
do
file ${l}
wget https://git.kernel.org/pub/scm/linux/kernel/git/next/linux-next.git/plain/${l} -O ${l}
done
sudo sudo cp  include/linux/mhi.h /lib/modules/`uname -r`/build/include/linux/mhi.h
