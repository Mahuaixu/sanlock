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
#include "wdmd.h"
#include "sanlk_reset.h"

#define DEFAULT_SYSRQ_DELAY 25

static char *daemon_name = (char *)"sanlk_resetd";
static int daemon_quit;
static int daemon_foreground;
static int daemon_debug;
static int use_watchdog = 1;
static int use_sysrq_reboot = 0;
static int sysrq_delay = DEFAULT_SYSRQ_DELAY;
static int we_are_resetting;
static int we_are_rebooting;
static uint64_t rebooting_time;

#define MAX_LS 4

static struct pollfd *pollfd;
static char *ls_names[MAX_LS];
static int ls_fd[MAX_LS];
static int ls_count;
static int signal_fd;
static int wdmd_fd;


#define log_debug(fmt, args...) \
do { \
	if (daemon_debug) \
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


static uint64_t monotime(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_sec;
}

/*
 * By default a 25 second delay is used before using sysrq to give sanlock
 * time to write our resetting event in its next lease renewal.
 *
 * It would not be surprising for sysrq reboot to fail or hang, so it's
 * important for the watchdog to also be there to reset us.  This
 * sysrq reboot is used only as a way to speed up the reset since the
 * watchdog requires 60 seconds to fire.
 */

static void sysrq_reboot(void)
{
	int fd, rv;

	log_error("Rebooting host with sysrq");
	/* give at least a small chance for the log message to be written */
	sleep(1);

	fd = open("/proc/sysrq-trigger", O_WRONLY);

	if (fd < 0) {
		log_error("failed to open sysrq-trigger %d %d", fd, errno);
		return;
	}

	rv = write(fd, "b", 1);
	if (rv < 0) {
		log_error("failed to write sysrq-trigger %d %d", rv, errno);
	}

	close(fd);

	/* If sysrq reboot worked, then I don't think we will get here. */
	/* If sysrq reboot failed, then the watchdog should reset us. */
	log_error("Reboot from sysrq is expected");
}

/*
 * Use the watchdog to reset the machine as soon as possible.
 * Intentionally set the expire time on the connection to
 * the current time so that the watchdog will expire and
 * reset as soon as possible.
 */

void watchdog_reset_self(void)
{
	uint64_t now;
	int rv;

	if (!use_watchdog)
		return;

	now = monotime();

	rv = wdmd_test_live(wdmd_fd, now, now);
	if (rv < 0) {
		log_error("watchdog_reset_self test_live failed %d", rv);
		return;
	}

	log_error("Resetting host with watchdog");
	return;
}

static int setup_wdmd(void)
{
	char name[WDMD_NAME_SIZE];
	int con;
	int rv;

	if (!use_watchdog)
		return 0;

	con = wdmd_connect();
	if (con < 0) {
		log_error("setup_wdmd connect failed %d", con);
		return con;
	}

	memset(name, 0, sizeof(name));

	snprintf(name, WDMD_NAME_SIZE - 1, "sanlk_resetd");

	rv = wdmd_register(con, name);
	if (rv < 0) {
		log_error("setup_wdmd register failed %d", rv);
		goto fail_close;
	}

	/* the refcount tells wdmd that it should not cleanly exit */

	rv = wdmd_refcount_set(con);
	if (rv < 0) {
		log_error("setup_wdmd refcount_set failed %d", rv);
		goto fail_close;
	}

	log_debug("setup_wdmd %d", con);

	wdmd_fd = con;
	return 0;

 fail_close:
	close(con);
	return -1;
}

static void close_wdmd(void)
{
	if (!use_watchdog)
		return;

	wdmd_refcount_clear(wdmd_fd);
	close(wdmd_fd);
}

/*
 * This event will be included in the next lease renewal of the lockspace.
 * This should be in about the next 20 seconds, unless renewals are
 * experiencing some delays.  We have about 60 seconds to get the renewal,
 * including the event, written before the watchdog fires (or syrq_delay until
 * sysrq reboot if that is configured).
 */

static void set_event_out(char *ls_name, uint64_t event_out, uint64_t from_host, uint64_t from_gen)
{
	struct sanlk_host_event he;
	int rv;

	he.host_id = from_host;
	he.generation = from_gen;
	he.event = event_out;
	he.data = 0;

	rv = sanlock_set_event(ls_name, &he, 0);
	if (rv < 0)
		log_error("set_event error %d ls %s", rv, ls_name);
}

static void unregister_ls(int i)
{
	sanlock_end_event(ls_fd[i], ls_names[i], 0);
	ls_names[i] = NULL;
	ls_fd[i] = -1;
	pollfd[i].fd = -1;
	pollfd[i].events = 0;
}

static int setup_signals(void)
{
	sigset_t mask;
	int rv;

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);

	rv = sigprocmask(SIG_BLOCK, &mask, NULL);
	if (rv < 0)
		return rv;

	signal_fd = signalfd(-1, &mask, 0);
	if (signal_fd < 0)
		return -errno;

	return 0;
}

static void process_signals(int fd)
{
	struct signalfd_siginfo fdsi;
	ssize_t rv;

	rv = read(fd, &fdsi, sizeof(struct signalfd_siginfo));
	if (rv != sizeof(struct signalfd_siginfo))
		return;

	if ((fdsi.ssi_signo == SIGTERM) || (fdsi.ssi_signo == SIGINT)) {
		log_debug("daemon_quit signal %d", fdsi.ssi_signo);
		daemon_quit = 1;
	}
}

static void usage(void)
{
	printf("%s [options] lockspace_name ...\n", daemon_name);
	printf("  --help | -h\n");
	printf("        Show this help information.\n");
	printf("  --version | -V\n");
	printf("        Show version.\n");
	printf("  --foreground | -f\n");
	printf("        Don't fork.\n");
	printf("  --daemon-debug | -D\n");
	printf("        Don't fork and print debugging to stdout.\n");
	printf("  --watchdog | -w 0|1\n");
	printf("        Disable (0) use of wdmd/watchdog for testing.\n");
	printf("  --sysrq-reboot | -r 0|1\n");
	printf("        Enable/Disable (1/0) use of /proc/sysrq-trigger to reboot (default 0).\n");
	printf("  --sysrq-delay | -d <sec>\n");
	printf("        Delay this many seconds before using /proc/sysrq-trigger (default %d).\n", DEFAULT_SYSRQ_DELAY);
	printf("\n");
	printf("Get reset events from lockspace_name (max %d).\n", MAX_LS);
}

int main(int argc, char *argv[])
{
	struct sanlk_host_event from_he;
	uint64_t from_host, from_gen;
	uint64_t event, event_out;
	int poll_timeout;
	int i, rv, fd;

	static struct option long_options[] = {
		{"help",	 no_argument,	    0, 'h' },
		{"version",      no_argument,	    0, 'V' },
		{"foreground",   no_argument,	    0, 'f' },
		{"daemon-debug", no_argument,	    0, 'D' },
		{"watchdog",     required_argument, 0, 'w' },
		{"sysrq-reboot", required_argument, 0, 'r' },
		{"sysrq-delay",  required_argument, 0, 'd' },
		{0, 0, 0, 0 }
	};

	while (1) {
		int c;
		int option_index = 0;

		c = getopt_long(argc, argv, "hVfDw:r:d:",
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
			printf("%s version: " VERSION "\n", daemon_name);
			exit(EXIT_SUCCESS);
		case 'f':
			daemon_foreground = 1;
			break;
		case 'D':
			daemon_foreground = 1;
			daemon_debug = 1;
			break;
		case 'w':
			use_watchdog = atoi(optarg);
			break;
		case 'r':
			use_sysrq_reboot = atoi(optarg);
			break;
		case 'd':
			sysrq_delay = atoi(optarg);
			break;
		case '?':
		default:
			usage();
			exit(EXIT_FAILURE);
		}
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

	if (!daemon_foreground) {
		if (daemon(0, 0) < 0) {
			fprintf(stderr, "cannot fork daemon\n");
			exit(EXIT_FAILURE);
		}
	}

	openlog(daemon_name, LOG_CONS | LOG_PID, LOG_DAEMON);

	log_warn("%s %s started %s", daemon_name, VERSION, use_watchdog ? "" : "use_watchdog=0");

	rv = setup_wdmd();
	if (rv < 0) {
		log_error("failed to set up wdmd");
		return rv;
	}

	rv = setup_signals();
	if (rv < 0) {
		log_error("failed to set up signals");
		goto out;
	}

	/* Add an extra slot for the signal fd. */
	pollfd = malloc((MAX_LS+1) * sizeof(struct pollfd));
	if (!pollfd)
		return -ENOMEM;

	for (i = 0; i < MAX_LS; i++) {
		ls_fd[i] = -1;
		pollfd[i].fd = -1;
		pollfd[i].events = 0;
		pollfd[i].revents = 0;
	}
	pollfd[MAX_LS].fd = signal_fd;
	pollfd[MAX_LS].events = POLLIN;
	pollfd[MAX_LS].revents = 0;

	ls_count = 0;

	for (i = 0; i < MAX_LS; i++) {
		if (!ls_names[i])
			continue;

		fd = sanlock_reg_event(ls_names[i], NULL, 0);
		if (fd < 0) {
			log_error("reg_event error %d ls %s", fd, ls_names[i]);
			ls_names[i] = NULL;
		} else {
			log_debug("reg_event fd %d ls %s", fd, ls_names[i]);
			ls_fd[i] = fd;
			pollfd[i].fd = fd;
			pollfd[i].events = POLLIN;
			ls_count++;
		}
	}

	if (!ls_count) {
		log_error("No lockspaces registered.");
		exit(EXIT_FAILURE);
	}

	poll_timeout = -1;

	while (1) {
		rv = poll(pollfd, MAX_LS+1, poll_timeout);
		if (rv == -1 && errno == EINTR)
			continue;
		if (rv < 0)
			break;

		if (pollfd[MAX_LS].revents & POLLIN)
			process_signals(pollfd[MAX_LS].fd);

		if (daemon_quit)
			break;

		if (we_are_rebooting && (monotime() - rebooting_time >= sysrq_delay)) {
			sysrq_reboot();
		}

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

				event = from_he.event;
				event_out = 0;

				if (event & (EVENT_RESET | EVENT_REBOOT)) {
					log_warn("request to %s%s(%llx %llx) from host %llu %llu ls %s",
						 (event & EVENT_RESET) ? "reset " : "",
						 (event & EVENT_REBOOT) ? "reboot " : "",
						 (unsigned long long)from_he.event,
						 (unsigned long long)from_he.data,
						 (unsigned long long)from_host,
						 (unsigned long long)from_gen,
						 ls_names[i]);
				}

				if (event & (EVENT_RESETTING | EVENT_REBOOTING)) {
					log_warn("notice of %s%s(%llx %llx) from host %llu %llu ls %s",
						 (event & EVENT_RESETTING) ? "resetting " : "",
						 (event & EVENT_REBOOTING) ? "rebooting " : "",
						 (unsigned long long)from_he.event,
						 (unsigned long long)from_he.data,
						 (unsigned long long)from_host,
						 (unsigned long long)from_gen,
						 ls_names[i]);
				}

				if ((event & EVENT_REBOOT) && !use_sysrq_reboot) {
					event &= ~EVENT_REBOOT;
					log_error("ignore reboot request sysrq_reboot not enabled");
				}

				if ((event & EVENT_RESET) && !we_are_resetting) {
					we_are_resetting = 1;
					poll_timeout = 1000;
					watchdog_reset_self();
					event_out |= EVENT_RESETTING;
				}

				if ((event & EVENT_REBOOT) && !we_are_rebooting) {
					we_are_rebooting = 1;
					rebooting_time = monotime();
					poll_timeout = 1000;
					event_out |= EVENT_REBOOTING;
				}

				if (event_out)
					set_event_out(ls_names[i], event_out, from_host, from_gen);
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

	log_debug("unregister daemon_quit=%d ls_count=%d", daemon_quit, ls_count);

	for (i = 0; i < MAX_LS; i++) {
		if (!ls_names[i])
			continue;
		if (ls_fd[i] == -1)
			continue;
		unregister_ls(i);
	}
out:
	close_wdmd();
	return 0;
}
