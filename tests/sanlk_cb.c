#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "sanlock.h"
#include "sanlock_admin.h"
#include "../src/sanlock_sock.h"

int main(int argc, char *argv[])
{
	struct sanlk_lockspace ls;
	struct sm_header h;
	struct sanlk_callback cb;
	struct pollfd pollfd;
	int fd, rv;

	if (argc < 2) {
		printf("sanlk_cb <lockspace_name>\n");
		return -1;
	}

	memset(&ls, 0, sizeof(ls));

	strcpy(ls.name, argv[1]);

	fd = sanlock_reg_lockspace(&ls, 0);
	if (fd < 0) {
		printf("reg error %d\n", fd);
		return -1;
	}

	printf("sanlock_reg_lockspace fd %d\n", fd);

	memset(&pollfd, 0, sizeof(pollfd));
	pollfd.fd = fd;
	pollfd.events = POLLIN;

	while (1) {
		rv = poll(&pollfd, 1, -1);
		if (rv == -1 && errno == EINTR)
			continue;

		if (rv < 0) {
			printf("poll error %d\n", rv);
			exit(0);
		}

		if (pollfd.revents & POLLIN) {
#if 0
			rv = recv(fd, &h, sizeof(h), MSG_WAITALL);
			if (rv < 0) {
				printf("recv h %d %d\n", rv, errno);
				return -errno;
			}
			if (rv != sizeof(h)) {
				printf("recv rv %d\n", rv);
				return -1;
			}

			printf("h m %x v %x c %d l %d\n",
			       h.magic, h.version, h.cmd, h.length);

			rv = recv(fd, &cb, sizeof(cb), MSG_WAITALL);
			if (rv < 0) {
				printf("recv cb %d %d\n", rv, errno);
				return -errno;
			}
			if (rv != sizeof(cb)) {
				printf("recv rv %d\n", rv);
				return -1;
			}
#endif

			rv = sanlock_get_callback(fd, 0, &cb, sizeof(cb));

			if (cb.hm.type != SANLK_CB_HOST_MESSAGE) {
				printf("unknown cb type %d\n", cb.hm.type);
				continue;
			}

			printf("host message from host_id %llu gen %llu\n",
				(unsigned long long)cb.hm.from_host_id,
				(unsigned long long)cb.hm.from_generation);
			printf("msg 0x%08x seq 0x%08x\n",
				cb.hm.msg, cb.hm.seq);
		}

		if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			printf("poll revents %x\n", pollfd.revents);
			exit(0);
		}
	}

	return 0;
}

