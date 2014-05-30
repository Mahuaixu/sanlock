/*
 * Copyright 2014 Red Hat, Inc.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v2 or (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <inttypes.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <syslog.h>
#include <dirent.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/signalfd.h>

#include "sanlock.h"
#include "sanlock_admin.h"
#include "sanlock_resource.h"
#include "sanlock_direct.h"
#include "sanlk_reset.h"

#define MAX_LS 4

static char *prog_name;
static struct pollfd *pollfd;
static char *ls_names[MAX_LS];
static int ls_fd[MAX_LS];
static int ls_count;
static int event_reply;
static int use_watchdog = 1;
static int use_sysrq_reboot = 0;

static int last_status_live;
static int last_status_fail;
static int last_status_unknown;


#define log_debug(fmt, args...) \
do { \
	fprintf(stderr, "%llu " fmt "\n", (unsigned long long)time(NULL), ##args); \
} while (0)

#define log_error(fmt, args...) \
do { \
	log_debug(fmt, ##args); \
	syslog(LOG_ERR, fmt, ##args); \
} while (0)

#define log_warn(fmt, args...) \
do { \
	log_debug(fmt, ##args); \
	syslog(LOG_WARNING, fmt, ##args); \
} while (0)


static void unregister_ls(int i)
{
	sanlock_end_event(ls_fd[i], ls_names[i], 0);
	ls_names[i] = NULL;
	ls_fd[i] = -1;
	pollfd[i].fd = -1;
	pollfd[i].events = 0;
}

static int reset_done(uint64_t host_id)
{
	struct sanlk_host *hss, *hs;
	int hss_count;
	int found;
	int i, j;
	int val;
	int rv;
	int found_count;
	int free_count, live_count, fail_count, dead_count, unknown_count;

	found_count = 0;
	free_count = 0;
	live_count = 0;
	fail_count = 0;
	dead_count = 0;
	unknown_count = 0;

	for (i = 0; i < MAX_LS; i++) {
		if (!ls_names[i])
			continue;

		hss_count = 0;
		hss = NULL;
		hs = NULL;

		rv = sanlock_get_hosts(ls_names[i], 0, &hss, &hss_count, 0);

		if (!hss_count || !hss) {
			log_error("sanlock_get_hosts error %d ls %s", rv, ls_names[i]);
			continue;
		}

		found = 0;
		hs = hss;

		for (j = 0; j < hss_count; j++) {
			if (hs->host_id != host_id) {
				hs++;
				continue;
			}

			found = 1;

			val = hs->flags & SANLK_HOST_MASK;

			if (val == SANLK_HOST_FREE)
				free_count++;
			else if (val == SANLK_HOST_LIVE)
				live_count++;
			else if (val == SANLK_HOST_FAIL)
				fail_count++;
			else if (val == SANLK_HOST_DEAD)
				dead_count++;
			else if (val == SANLK_HOST_UNKNOWN)
				unknown_count++;
			break;
		}

		free(hss);

		if (!found) {
			log_error("status of host_id %llu not found ls %s",
				  (unsigned long long)host_id, ls_names[i]);
		} else {
			found_count++;
		}
	}

	if (!found_count) {
		log_error("status of host_id %llu not found", (unsigned long long)host_id);
		return 0;
	}

	if (!free_count && !live_count && !fail_count && !dead_count && !unknown_count) {
		log_error("status of host_id %llu no status", (unsigned long long)host_id);
		return 0;
	}

	if (live_count) {
		if (!last_status_live)
			log_debug("host_id %llu status: live", (unsigned long long)host_id);
		last_status_live = 1;
		last_status_fail = 0;
		last_status_unknown = 0;
		return 0;
	}

	if (fail_count) {
		if (!last_status_fail)
			log_debug("host_id %llu status: fail", (unsigned long long)host_id);
		last_status_fail = 1;
		last_status_live = 0;
		last_status_unknown = 0;
		return 0;
	}

	if (unknown_count) {
		if (!last_status_unknown)
			log_debug("host_id %llu status: unknown", (unsigned long long)host_id);
		last_status_unknown = 1;
		last_status_fail = 0;
		last_status_live = 0;
		return 0;
	}

	if (free_count && !fail_count) {
		log_debug("host_id %llu status: free", (unsigned long long)host_id);
		return 1;
	}

	if (dead_count) {
		log_debug("host_id %llu status: dead", (unsigned long long)host_id);
		return 1;
	}

	log_debug("host_id %llu status: %u", (unsigned long long)host_id, val);
	return 0;
}

static void usage(void)
{
	printf("%s [options] -i <host_id> lockspace_name ...\n", prog_name);
	printf("  --help | -h\n");
	printf("        Show this help information.\n");
	printf("  --version | -V\n");
	printf("        Show version.\n");
	printf("  --host-id | -i <num>\n");
	printf("        Host id to reset.\n");
	printf("  --generation | -g <num>\n");
	printf("        Generation of host id (default current generation).\n");
	printf("  --watchdog | -w 0|1\n");
	printf("        Disable (0) use of wdmd/watchdog for testing.\n");
	printf("  --sysrq-reboot | -r 0|1\n");
	printf("        Enable/Disable (1/0) use of /proc/sysrq-trigger to reboot (default 0).\n");
	printf("\n");
	printf("The event will be set in each lockspace_name (max %d).\n", MAX_LS);
	printf("Use -g 0 to use the current generation.\n");
}

int main(int argc, char *argv[])
{
	struct sanlk_host_event he;
	struct sanlk_host_event from_he;
	uint64_t from_host, from_gen;
	uint32_t flags = 0;
	uint32_t begin = time(NULL);
	int i, fd, rv;
	int done = 0;

	prog_name = argv[0];

	memset(&he, 0, sizeof(he));

	static struct option long_options[] = {
		{"help",	 no_argument,       0, 'h' },
		{"version",      no_argument,       0, 'V' },
		{"host-id",      required_argument, 0, 'i' },
		{"generation",   required_argument, 0, 'g' },
		{"watchdog",     required_argument, 0, 'w' },
		{"sysrq-reboot", required_argument, 0, 'r' },
		{0, 0, 0, 0 }
	};

	while (1) {
		int c;
		int option_index = 0;

		c = getopt_long(argc, argv, "hVi:g:w:r:",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case '0':
			break;
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
		case 'V':
			printf("%s version: " VERSION "\n", prog_name);
			exit(EXIT_SUCCESS);
		case 'i':
			he.host_id = strtoull(optarg, NULL, 0);
			break;
		case 'g':
			he.generation = strtoull(optarg, NULL, 0);
			break;
		case 'w':
			use_watchdog = atoi(optarg);
			break;
		case 'r':
			use_sysrq_reboot = atoi(optarg);
			break;
		case '?':
		default:
			usage();
			exit(EXIT_FAILURE);
		}
	}

	if (!he.host_id) {
		log_error("host_id is required");
		exit(EXIT_FAILURE);
	}

	ls_count = 0;

	for (i = optind; i < argc; i++) {
		if (ls_count == MAX_LS) {
			fprintf(stderr, "ignore lockspace_name %s", argv[i]);
			continue;
		}
		ls_names[ls_count] = argv[i];
		ls_count++;
	}

	if (!ls_count) {
		log_error("lockspace_name is required");
		exit(EXIT_FAILURE);
	}

	pollfd = malloc(MAX_LS * sizeof(struct pollfd));
	if (!pollfd)
		return -ENOMEM;

	for (i = 0; i < MAX_LS; i++) {
		ls_fd[i] = -1;
		pollfd[i].fd = -1;
		pollfd[i].events = 0;
		pollfd[i].revents = 0;
	}

	openlog(prog_name, LOG_CONS | LOG_PID, LOG_DAEMON);

	ls_count = 0;

	for (i = 0; i < MAX_LS; i++) {
		if (!ls_names[i])
			continue;

		fd = sanlock_reg_event(ls_names[i], NULL, 0);
		if (fd < 0) {
			log_error("reg_event error %d ls %s", fd, ls_names[i]);
			ls_names[i] = NULL;
		} else {
			ls_fd[i] = fd;
			pollfd[i].fd = ls_fd[i];
			pollfd[i].events = POLLIN;
			ls_count++;
		}
	}

	if (!ls_count) {
		log_error("No lockspaces registered.");
		exit(EXIT_FAILURE);
	}

	if (use_watchdog)
		he.event |= EVENT_RESET;
	if (use_sysrq_reboot)
		he.event |= EVENT_REBOOT;
	if (!he.generation)
		flags = SANLK_SETEV_CUR_GENERATION;

	for (i = 0; i < MAX_LS; i++) {
		if (!ls_names[i])
			continue;
		if (ls_fd[i] == -1)
			continue;

		rv = sanlock_set_event(ls_names[i], &he, flags);
		if (rv < 0) {
			log_error("set_event %s error %d", ls_names[i], rv);
			unregister_ls(i);
		} else {
			log_warn("asked host %llu %llu to %s%s(%llx %llx)",
				 (unsigned long long)he.host_id,
				 (unsigned long long)he.generation,
				 (he.event & EVENT_RESET) ? "reset " : "",
				 (he.event & EVENT_REBOOT) ? "reboot " : "",
				 (unsigned long long)he.event,
				 (unsigned long long)he.data);
		}
	}

	if (!ls_count) {
		log_error("No lockspaces to use after set_event error.");
		exit(EXIT_FAILURE);
	}

	while (1) {
		rv = poll(pollfd, MAX_LS, 1000);
		if (rv == -1 && errno == EINTR)
			continue;
		if (rv < 0)
			break;

		done = reset_done(he.host_id);
		if (done)
			break;

		for (i = 0; i < MAX_LS; i++) {
			if (pollfd[i].fd < 0)
				continue;

			if (pollfd[i].revents & POLLIN) {
				rv = sanlock_get_event(ls_fd[i], 0, &from_he, &from_host, &from_gen);
				if (rv < 0) {
					log_error("unregister fd %d get_event error %d ls %s",
						  ls_fd[i], rv, ls_names[i]);
					unregister_ls(i);
					ls_count--;
					continue;
				}

				if ((from_host == he.host_id) &&
				    (from_he.event & EVENT_RESETTING)) {
					log_warn("notice of %s%s(%llx %llx) from host %llu %llu ls %s",
						 (from_he.event & EVENT_RESETTING) ? "resetting " : "",
						 (from_he.event & EVENT_REBOOTING) ? "rebooting " : "",
						 (unsigned long long)from_he.event,
						 (unsigned long long)from_he.data,
						 (unsigned long long)from_host,
						 (unsigned long long)from_gen,
						 ls_names[i]);
					event_reply = 1;
				} else {
					log_warn("event ignored %llx %llx from host %llu %llu ls %s",
						 (unsigned long long)from_he.event,
						 (unsigned long long)from_he.data,
						 (unsigned long long)from_host,
						 (unsigned long long)from_gen,
						 ls_names[i]);
				}
			}

			if (pollfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				log_debug("unregister fd %d poll %x ls %s",
					  ls_fd[i], pollfd[i].revents, ls_names[i]);
				unregister_ls(i);
				ls_count--;
			}
		}

		if (!ls_count)
			break;
	}

	for (i = 0; i < MAX_LS; i++) {
		if (!ls_names[i])
			continue;
		if (ls_fd[i] == -1)
			continue;
		unregister_ls(i);
	}

	if (done) {
		log_debug("reset done in %u seconds", (uint32_t)(time(NULL) - begin));
		exit(EXIT_SUCCESS);
	}

	log_debug("reset failed");
	exit(EXIT_FAILURE);
}
