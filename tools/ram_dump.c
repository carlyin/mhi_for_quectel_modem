#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <sys/poll.h>

//static const unsigned char edl_cmd[] = {0x4b, 0x65, 0x01, 0x00, 0x54, 0x0f, 0x7e};
static const unsigned char dump_cmd1[] = {0x4b, 0x12, 0x18, 0x02, 0x01, 0x00, 0xd2, 0x7e};
static const unsigned char dump_cmd2[] = {0x7e, 0x01, 0x04, 0x00, 0x4b, 0x25, 0x03, 0x00, 0x7e};
static unsigned char rx_buf[2048];

int main(int argc, char *argv[]) {
	const char *mhi_dev = "0000:03:00.0";
	char mhi_chan[64];
	int fd;
	int ret;
	int opt;

///lib/firmware/firehose/prog_firehose_sdx55.mbn
	optind = 1;
    while ( -1 != (opt = getopt(argc, argv, "f:h"))) {
        switch (opt) {
			case 'f':
			break;
			default:
			break;
		}
    }

	snprintf(mhi_chan, sizeof(mhi_chan), "/dev/mhi_%s_%s", mhi_dev, "SAHARA");
	if (!access(mhi_chan, F_OK))
		goto _find_sbl;

	snprintf(mhi_chan, sizeof(mhi_chan), "/dev/mhi_%s_%s", mhi_dev, "DIAG");
	fd = open(mhi_chan, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd == -1) {
		printf("fail to open %s, errno: %d (%s)\n", mhi_chan, errno, strerror(errno));
		return 1;
	}
	printf("open('%s') = %d\n", mhi_chan, fd);

	while (1) {
		struct pollfd pollfds[] = {{fd, POLLIN, 0}};
		ret = poll(pollfds, 1, 3000);
		if (ret < 0)
			break;
		else if (ret == 0) {
			printf("send dump cmd\n");
			write(fd, dump_cmd1, sizeof(dump_cmd1));
			usleep(100*1000);
			write(fd, dump_cmd2, sizeof(dump_cmd2));
		}
		else if (pollfds[0].revents & POLLIN) {
			ret = read(fd, rx_buf, sizeof(rx_buf));
			printf("read = %d\n", ret);
			if (ret <= 0)
				break;
		}
		else {
			break;
		}
	}

	close(fd);

	snprintf(mhi_chan, sizeof(mhi_chan), "/dev/mhi_%s_%s", mhi_dev, "SAHARA");
	printf("wait %s\n", mhi_chan);
	for (ret = 0; ret < 3; ret++) {
		if (!access(mhi_chan, F_OK))
			break;
		sleep(1);
	}

_find_sbl:
	fd = open(mhi_chan, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd == -1) {
		printf("fail to open %s, errno: %d (%s)\n", mhi_chan, errno, strerror(errno));
		return 2;
	}
	printf("open('%s') = %d\n", mhi_chan, fd);

	close(fd);

	symlinkat(mhi_chan, AT_FDCWD, "/dev/mhi_SAHARA");
 	system("./QLog -p /dev/mhi_SAHARA -s log123");
	return ret;	
}
