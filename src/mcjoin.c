/* Join a multicast group and/or generate UDP test data
 *
 * Copyright (C) 2004       David Stevens <dlstevens()us!ibm!com>
 * Copyright (C) 2008-2019  Joachim Nilsson <troglobit()gmail!com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>

#include "addr.h"
#include "log.h"

#define BUFSZ           100
#define MAX_NUM_GROUPS  2048
#define DEFAULT_GROUP   "225.1.2.3"
#define DEFAULT_PORT    1234
#define MAGIC_KEY       "Sender PID "

/* Esc[?25l (lower case L)    - Hide Cursor */
#define hidecursor()    if (logon()) fputs("\e[?25l", stderr)
/* Esc[?25h (lower case H)    - Show Cursor */
#define showcursor()    if (logon()) fputs("\e[?25h", stderr)

/* From The Practice of Programming, by Kernighan and Pike */
#ifndef NELEMS
#define NELEMS(array) (sizeof(array) / sizeof(array[0]))
#endif

/* Mode flags */
int join = 1;
int debug = 0;
int sender = 0;
int running = 1;

/* Global data */
int period = 100000;		/* 100 msec in micro seconds*/
int restart = 0;
size_t count = 0;
int port = DEFAULT_PORT;
unsigned char ttl = 1;
char *ident = PACKAGE_NAME;

size_t group_num = 0;
struct gr groups[MAX_NUM_GROUPS];

char iface[IFNAMSIZ + 1];
int num_joins = 0;


static int alloc_socket(inet_addr_t group)
{
	int sd, val, proto;

#ifdef AF_INET6
	if (group.ss_family == AF_INET6)
		proto = IPPROTO_IPV6;
	else
#endif
		proto = IPPROTO_IP;

	sd = socket(group.ss_family, SOCK_DGRAM, IPPROTO_UDP);
	if (sd < 0) {
		ERROR("Failed opening socket(): %s", strerror(errno));
		return -1;
	}

	val = 1;
#ifdef SO_REUSEPORT
	if (setsockopt(sd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val)))
		ERROR("Failed enabling SO_REUSEPORT: %s", strerror(errno));
#endif
	if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)))
		ERROR("Failed enabling SO_REUSEADDR: %s", strerror(errno));

#if defined(IP_PKTINFO) || !defined(IP_RECVDSTADDR)
	if (setsockopt(sd, SOL_IP, IP_PKTINFO, &val, sizeof(val)))
		ERROR("Failed enabling IP_PKTINFO: %s", strerror(errno));
#elif defined(IP_RECVDSTADDR)
	if (setsockopt(sd, IPPROTO_IP, IP_RECVDSTADDR, &val, sizeof(val)))
		ERROR("Failed enabling IP_RECVDSTADDR: %s", strerror(errno));
#endif

	val = 0;
#ifdef IP_MULTICAST_ALL
	if (setsockopt(sd, proto, IP_MULTICAST_ALL, &val, sizeof(val)))
		ERROR("Failed disabling IP_MULTICAST_ALL: %s", strerror(errno));
#endif

	if (bind(sd, (struct sockaddr *)&group, inet_addrlen(&group))) {
		ERROR("Failed binding to socket: %s", strerror(errno));
		close(sd);
		return -1;
	}

	return sd;
}

static int join_group(struct gr *sg)
{
	struct group_source_req gsr;
	struct group_req gr;
	char src[INET_ADDRSTR_LEN] = "*";
	char grp[INET_ADDRSTR_LEN];
	size_t len;
	void *arg;
	int ifindex;
	int sd, op, proto;

	/* Index port with id if IP_MULTICAST_ALL fails */
	sd = alloc_socket(sg->grp);
	if (sd < 0)
		return 1;

	ifindex = if_nametoindex(iface);
	if (!ifindex) {
		ERROR("invalid interface: %s", iface);
		goto error;
	}
	DEBUG("Added iface %s, idx %d", iface, ifindex);

#ifdef AF_INET6
	if (sg->grp.ss_family == AF_INET6)
		proto = IPPROTO_IPV6;
	else
#endif
		proto = IPPROTO_IP;

	if (sg->source) {
		gsr.gsr_interface  = ifindex;
		gsr.gsr_source     = sg->src;
		gsr.gsr_group      = sg->grp;
		op                 = MCAST_JOIN_SOURCE_GROUP;
		arg                = &gsr;
		len                = sizeof(gsr);
	} else {
		gr.gr_interface    = ifindex;
		gr.gr_group        = sg->grp;
		op                 = MCAST_JOIN_GROUP;
		arg                = &gr;
		len                = sizeof(gr);
	}

	if (sg->source)
		inet_address(&sg->src, src, sizeof(src));
	inet_address(&sg->grp, grp, sizeof(grp));
	DEBUG("Joining group (%s,%s) on iface %s, sd: %d", src, grp, iface, sd);

	if (setsockopt(sd, proto, op, arg, len)) {
		ERROR("Failed %s group (%s,%s) on sd %d ... %d: %s",
		      src, grp, "joining", sd, errno, strerror(errno));
		goto error;
	}

	if (sg->source)
		PRINT("joined source,group %s,%s on %s ...", sg->source, sg->group, iface);
	else
		PRINT("joined group %s on %s ...", sg->group, iface);
	sg->sd = sd;

	return 0;

error:
	close(sd);
	return 1;
}

static int send_socket(int family)
{
	inet_addr_t addr;
	char buf[INET_ADDRSTR_LEN];
	int ifindex;
	int sd;

	ifindex = ifinfo(iface, &addr, family);
	if (ifindex <= 0) {
		ERROR("No interface (%s), or no IPv%s address yet, rc %d: %s",
		      iface[0] ? iface : "N/A", family == AF_INET ? "4" : "6",
		      ifindex, strerror(errno));
		return -1;
	}

	DEBUG("Sending on iface %s addr %s ifindex %d", iface,
	      inet_address(&addr, buf, sizeof(buf)), ifindex);

	sd = socket(family, SOCK_DGRAM, 0);
	if (sd < 0) {
		ERROR("Failed opening socket(): %s", strerror(errno));
		return -1;
	}

	if (family == AF_INET) {
		struct ip_mreqn imr = { .imr_ifindex = ifindex };

		if (setsockopt(sd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)))
			ERROR("Failed setting IP_MULTICAST_TTL: %s", strerror(errno));

		if (setsockopt(sd, IPPROTO_IP, IP_MULTICAST_IF, &imr, sizeof(imr)))
			ERROR("Failed setting IP_MULTICAST_IF: %s", strerror(errno));
	}
#ifdef AF_INET6
	else {
		int hops = ttl;

		if (setsockopt(sd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)))
			ERROR("Failed setting IPV6_MULTICAST_HOPS: %s", strerror(errno));

		if (setsockopt(sd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex)))
			ERROR("Failed setting IPV6_MULTICAST_IF: %s", strerror(errno));
	}
#endif
	if (bind(sd, (struct sockaddr *)&addr, inet_addrlen(&addr)) == -1) {
		ERROR("Failed binding socket to %s", buf);
		close(sd);
		return -1;
	}

	return sd;
}

static void send_mcast(int signo)
{
	static unsigned int counter = 1;
	static int sd4 = -1;
	static int sd6 = -1;
	char buf[BUFSZ] = { 0 };
	size_t i;

	if (sd4 == -1) {
		sd4 = send_socket(AF_INET);
		if (sd4 == -1)
			return;
	}
#ifdef AF_INET6
	if (sd6 == -1) {
		sd6 = send_socket(AF_INET6);
		if (sd6 == -1)
			return;
	}
#endif

	for (i = 0; i < group_num; i++) {
		struct sockaddr *dest = (struct sockaddr *)&groups[i].grp;
		socklen_t len = inet_addrlen(&groups[i].grp);
		int sd = groups[i].grp.ss_family == AF_INET ? sd4 : sd6;

		snprintf(buf, sizeof(buf), "%s%u, MC group %s ... count: %u", MAGIC_KEY, getpid(), groups[i].group, counter++);
		DEBUG("Sending packet on signal %d, msg: %s", signo, buf);
		if (sendto(sd, buf, sizeof(buf), 0, dest, len) < 0)
			ERROR("Failed sending mcast packet: %s", strerror(errno));
	}
}

struct in_addr *find_dstaddr(struct msghdr *msgh)
{
	struct cmsghdr *cmsg;

	for (cmsg = CMSG_FIRSTHDR(msgh); cmsg; cmsg = CMSG_NXTHDR(msgh, cmsg)) {
#if defined(IP_PKTINFO) || !defined(IP_RECVDSTADDR)
		if (cmsg->cmsg_level == IPPROTO_IP &&
		    cmsg->cmsg_type == IP_PKTINFO)
			return &((struct in_pktinfo *)CMSG_DATA(cmsg))->
			    ipi_addr;
#elif defined(IP_RECVDSTADDR)
		if (cmsg->cmsg_level == IPPROTO_IP &&
		    cmsg->cmsg_type == IP_RECVDSTADDR)
			return (struct in_addr *)CMSG_DATA(cmsg);
#endif
	}

	return NULL;
}

static void progress(void)
{
	const char *style = ".oOOo.";
	static unsigned int i = 0;
	size_t num = 6;

	if (!logon())
		return;

	if (!(i % num))
		printf(".");

	putchar(style[i++ % num]);
	putchar('\b');

	fflush(stdout);
}

/*
 * rcvmsg() wrapper which uses out-of-band info to verify expected
 * destination address (multicast group)
 */
static ssize_t recv_mcast(int id)
{
	struct sockaddr_storage src;
	struct in_addr *dstaddr;
	struct msghdr msgh;
	struct iovec iov[1];
	ssize_t bytes;
	char cmbuf[0x100];
	char buf[BUFSZ];

	iov[0].iov_base = buf;
	iov[0].iov_len  = sizeof(buf);

	memset(&msgh, 0, sizeof(msgh));
	msgh.msg_name       = &src;
	msgh.msg_namelen    = sizeof(src);
	msgh.msg_iov        = iov;
	msgh.msg_iovlen     = NELEMS(iov);
	msgh.msg_control    = cmbuf;
	msgh.msg_controllen = sizeof(cmbuf);

	bytes = recvmsg(groups[id].sd, &msgh, MSG_DONTWAIT);
	if (bytes < 0)
		return -1;

	dstaddr = find_dstaddr(&msgh);
	if (dstaddr) {
		char *ptr;
		int pid = 0;

		buf[bytes] = 0;
		ptr = strstr(buf, MAGIC_KEY);
		if (ptr)
			pid = atoi(ptr + strlen(MAGIC_KEY));

		DEBUG("Count %5zu, our PID %d, sender PID %d, group %s msg: %s", groups[id].count, getpid(), pid, groups[id].group, buf);
		if (pid != getpid()) {
			char *dst = inet_ntoa(*dstaddr);

			if (strcmp(dst, groups[id].group)) {
				ERROR("Packet for group %s received on wrong socket, expected group %s.", dst, groups[id].group);
				return -1;
			}

			groups[id].count++;
			progress();
			return 0;
		}
	}

	return -1;
}

static int show_stats(void)
{
	if (join) {
		size_t i, total_count = 0;

		if (group_num > 1) {
			for (i = 0; i < group_num; i++) {
				PRINT("\nGroup %s received %zu packets", groups[i].group, groups[i].count);
				total_count += groups[i].count;
			}
		} else {
			total_count = groups[0].count;
		}

		PRINT("\nReceived total: %zu packets", total_count);
	}

	return 0;
}

static int loop(void)
{
	struct sigaction sa = {
		.sa_flags = SA_RESTART,
		.sa_handler = send_mcast,
	};
	size_t i;

	if (sender) {
		struct itimerval times;

		sigaction(SIGALRM, &sa, NULL);

		times.it_value.tv_sec     = 1;	/* wait a bit for system to "stabilize"  */
		times.it_value.tv_usec    = 0;	/* tv_sec or tv_usec cannot be both zero */
		times.it_interval.tv_sec  = (time_t)(period / 1000000);
		times.it_interval.tv_usec =   (long)(period % 1000000);
		setitimer(ITIMER_REAL, &times, NULL);
	}

	while (join && running) {
		for (i = 0; i < group_num; i++) {
			if (join_group(&groups[i]))
				return 1;
		}

		hidecursor();
		while (running) {
			struct pollfd pfd[MAX_NUM_GROUPS];
			int ret;

			/* One group per socket */
			for (i = 0; i < group_num; i++) {
				pfd[i].fd = groups[i].sd;
				pfd[i].events = POLLIN;
				pfd[i].revents = 0;
			}

			ret = poll(pfd, group_num, restart ? restart * 1000 : -1);
			if (ret <= 0) {
				if (ret < 0 || !restart)
					continue;

				for (i = 0; i < group_num; i++) {
					close(groups[i].sd);
					groups[i].sd = 0;
					num_joins = 0;
				}
				break;
			}

			for (i = 0; i < group_num; i++) {
				if (pfd[i].revents) {
					recv_mcast(i);
					pfd[i].revents = 0;
				}
			}

			if (count > 0) {
				size_t total = count * group_num;

				for (i = 0; i < group_num; i++)
					total -= groups[i].count;

				if (total <= 0) {
					running = 0;
					break;
				}
			}

			DEBUG("");
		}
		showcursor();
	}

	while (running) {
		pause();	/* Let signal handler(s) do their job */
		if (count > 0) {
			if (!--count)
				break;
		}
	}

	DEBUG("Leaving main loop");

	return show_stats();
}

static void exit_loop(int signo)
{
	DEBUG("We got signal! (signo: %d)", signo);
	running = 0;
}

static int usage(int code)
{
	if (!iface[0])
		ifdefault(iface, sizeof(iface));

	printf("Usage: %s [-dhjsv] [-c COUNT] [-i IFACE] [-l LEVEL] [-p PORT] [-r SEC]\n"
	       "              [-t TTL] [-w SEC]\n"
	       "              [[SOURCE,]GROUP0 .. [SOURCE,]GROUPN | [SOURCE,]GROUP+NUM]\n"
	       "Options:\n"
	       "  -c COUNT    Stop sending/receiving after COUNT number of packets\n"
	       "  -d          Run as daemon in background, output except progress to syslog\n"
	       "  -h          This help text\n"
	       "  -i IFACE    Interface to use for sending/receiving multicast, default: %s\n"
	       "  -j          Join groups, default unless acting as sender\n"
	       "  -l LEVEL    Set log level; none, notice*, debug\n"
	       "  -p PORT     UDP port number to send/listen to, default: %d\n"
	       "  -r SEC      Do a join/leave every SEC seconds (backwards compat. option)\n"
	       "  -s          Act as sender, sends packets to select groups, default: no\n"
	       "  -t TTL      TTL to use when sending multicast packets, default: 1\n"
	       "  -v          Display program version\n"
	       "  -w SEC      Initial wait before opening sockets\n"
	       "\n"
	       "Bug report address : %-40s\n", ident, iface, DEFAULT_PORT, PACKAGE_BUGREPORT);
#ifdef PACKAGE_URL
	printf("Project homepage   : %s\n", PACKAGE_URL);
#endif

	return code;
}

static char *progname(char *arg0)
{
       char *nm;

       nm = strrchr(arg0, '/');
       if (nm)
	       nm++;
       else
	       nm = arg0;

       return nm;
}

int main(int argc, char *argv[])
{
	struct sigaction sa = {
		.sa_flags = SA_RESTART,
		.sa_handler = exit_loop,
	};
	struct rlimit rlim;
	extern int optind;
	size_t ilen;
	int foreground = 1;
	int wait = 0;
	int i, c;

	for (i = 0; i < MAX_NUM_GROUPS; i++)
		memset(&groups[i], 0, sizeof(groups[0]));

	ident = progname(argv[0]);
	while ((c = getopt(argc, argv, "c:di:jl:p:r:st:vhw:")) != EOF) {
		switch (c) {
		case 'c':
			count = (size_t)atoi(optarg);
			break;

		case 'd':
			foreground = 0;
			break;

		case 'h':
			return usage(0);

		case 'i':
			ilen = strlen(optarg);
			if (ilen >= sizeof(iface)) {
				ERROR("Too long interface name, max %zd chars.", sizeof(iface) - 1);
				return 1;
			}
			strncpy(iface, optarg, sizeof(iface));
			iface[ilen] = 0;
			DEBUG("IFACE: %s", iface);
			break;

		case 'j':
			join++;
			break;

		case 'l':
			log_level = loglvl(optarg);
			break;

		case 'p':
			port = atoi(optarg);
			if (port < 1024 && geteuid())
				ERROR("Must be root to use privileged ports (< 1024)");
			break;

		case 'r':
			restart = atoi(optarg);
			DEBUG("RESTART: %d", restart);
			if (restart < 1)
				restart = 1;
			break;

		case 's':
			sender = 1;
			join--;
			break;

		case 't':
			ttl = atoi(optarg);
			break;

		case 'v':
			printf("%s\n", PACKAGE_VERSION);
			return 0;

		case 'w':
			wait = atoi(optarg);
			break;

		default:
			return usage(1);
		}
	}

	if (optind == argc)
		groups[group_num++].group = strdup(DEFAULT_GROUP);

	if (!foreground) {
		if (fork())
			_exit(0);
		if (setsid() == (pid_t)-1 || daemon(0, 0))
			_exit(1);
		log_syslog = 1;

		openlog(ident, log_opts, LOG_DAEMON);
		setlogmask(LOG_UPTO(log_level));
	}

	if (wait)
		sleep(wait);

	if (!iface[0])
		ifdefault(iface, sizeof(iface));

	if (getrlimit(RLIMIT_NOFILE, &rlim)) {
		ERROR("Failed reading RLIMIT_NOFILE");
		return 1;
	}

	DEBUG("NOFILE: current %ld max %ld", rlim.rlim_cur, rlim.rlim_max);
	rlim.rlim_cur = MAX_NUM_GROUPS + 10; /* Need stdio + pollfd, etc. */
	if (setrlimit(RLIMIT_NOFILE, &rlim)) {
		ERROR("Failed setting RLIMIT_NOFILE soft limit to %d", MAX_NUM_GROUPS);
		return 1;
	}
	DEBUG("NOFILE: set new current %ld max %ld", rlim.rlim_cur, rlim.rlim_max);

	/*
	 * mcjoin group+num
	 * mcjoin group0 group1 group2
	 */
	for (i = optind; i < argc; i++) {
		char *pos, *group, *source = NULL;
		int j, num = 1;

		group = argv[i];
		pos = strchr(group, '+');
		if (pos) {
			*pos = 0;
			num = atoi(&pos[1]);
		}

		pos = strchr(group, ',');
		if (pos) {
			*pos++ = 0;
			source = strdup(group);
			group  = pos;
		}

		if (num < 1 || (num + group_num) >= NELEMS(groups)) {
			ERROR("Invalid number of groups given (%d), or max (%zd) reached.", num, NELEMS(groups));
			return usage(1);
		}

		for (j = 0; j < num && group_num < NELEMS(groups); j++) {
#ifdef AF_INET6
			struct sockaddr_in6 *sin6;
#endif
			struct sockaddr_in *sin;
			inet_addr_t addr;
			socklen_t len;
			uint32_t step;
			void *ptr;
			int family;

#ifdef AF_INET6
			if (strchr(group, ':')) {
				family = AF_INET6;
				sin6 = (struct sockaddr_in6 *)&addr;
				ptr = &sin6->sin6_addr;
			} else
#endif
			{
				family = AF_INET;
				sin = (struct sockaddr_in *)&addr;
				ptr = &sin->sin_addr;
			}

			if (!inet_pton(family, group, ptr)) {
				ERROR("%s is not a valid multicast group", group);
				return usage(1);
			}

			DEBUG("Adding (S,G) %s,%s to list ...", source ?: "*", group);
			groups[group_num].source  = source;
			groups[group_num++].group = strdup(group);

			/* Next group ... */
#ifdef AF_INET6
			if (strchr(group, ':')) {
				memcpy(&step, &sin6->sin6_addr.s6_addr[12],
				    sizeof(step));
				step = htonl(ntohl(step) + 1);
				memcpy(&sin6->sin6_addr.s6_addr[12], &step,
				    sizeof(step));
				len = sizeof(*sin6);
			} else
#endif
			{
				step = ntohl(sin->sin_addr.s_addr);
				step++;
				sin->sin_addr.s_addr = htonl(step);
				len = sizeof(*sin);
			}

			inet_ntop(family, ptr, group, len);
		}
	}

	for (i = 0; i < (int)group_num; i++) {
#ifdef AF_INET6
		if (strchr(groups[i].group, ':')) {
			struct sockaddr_in6 *grp = (struct sockaddr_in6 *)&groups[i].grp;
			struct sockaddr_in6 *src = (struct sockaddr_in6 *)&groups[i].src;

			inet_pton(AF_INET6, groups[i].group, &grp->sin6_addr);
			grp->sin6_family = AF_INET6;
			grp->sin6_port   = htons(port);

			if (groups[i].source) {
				inet_pton(AF_INET6, groups[i].source, &src->sin6_addr);
				src->sin6_family = AF_INET6;
				src->sin6_port   = htons(port);
			}
		} else
#endif
		{
			struct sockaddr_in *grp = (struct sockaddr_in *)&groups[i].grp;
			struct sockaddr_in *src = (struct sockaddr_in *)&groups[i].src;

			inet_pton(AF_INET, groups[i].group, &grp->sin_addr);
			grp->sin_family = AF_INET;
			grp->sin_port   = htons(port);

			if (groups[i].source) {
				inet_pton(AF_INET, groups[i].source, &src->sin_addr);
				src->sin_family = AF_INET;
				src->sin_port   = htons(port);
			}
		}
#ifdef HAVE_STRUCT_SOCKADDR_STORAGE_SS_LEN
		groups[i].grp.ss_len = inet_addrlen(&groups[i].grp);
		if (groups[i].source)
			groups[i].src.ss_len = inet_addrlen(&groups[i].src);
#endif
	}

	/*
	 * Shared signal handlers between sender and receiver
	 */
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGHUP,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	return loop();
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
