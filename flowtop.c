/*
 * netsniff-ng - the packet sniffing beast
 * Copyright 2011 - 2013 Daniel Borkmann.
 * Copyright 2011 Emmanuel Roullit.
 * Subject to the GPL, version 2.
 */

#define _LGPL_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <netdb.h>
#include <ctype.h>
#include <netinet/in.h>
#include <curses.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/fsuid.h>
#include <urcu.h>
#include <libgen.h>
#include <inttypes.h>
#include <poll.h>
#include <fcntl.h>

#include "die.h"
#include "xmalloc.h"
#include "conntrack.h"
#include "config.h"
#include "str.h"
#include "sig.h"
#include "lookup.h"
#include "geoip.h"
#include "built_in.h"
#include "locking.h"
#include "pkt_buff.h"
#include "screen.h"
#include "proc.h"
#include "sysctl.h"

struct flow_entry {
	uint32_t flow_id, use, status;
	uint8_t  l3_proto, l4_proto;
	uint32_t ip4_src_addr, ip4_dst_addr;
	uint32_t ip6_src_addr[4], ip6_dst_addr[4];
	uint16_t port_src, port_dst;
	uint8_t  tcp_state, tcp_flags, sctp_state, dccp_state;
	uint64_t counter_pkts, counter_bytes;
	uint64_t timestamp_start, timestamp_stop;
	char country_src[128], country_dst[128];
	char city_src[128], city_dst[128];
	char rev_dns_src[256], rev_dns_dst[256];
	char cmdline[256];
	struct flow_entry *next;
	int inode;
	unsigned int procnum;
	bool is_visible;
	struct nf_conntrack *ct;
};

struct flow_list {
	struct flow_entry *head;
	struct spinlock lock;
};

#ifndef ATTR_TIMESTAMP_START
# define ATTR_TIMESTAMP_START 63
#endif
#ifndef ATTR_TIMESTAMP_STOP
# define ATTR_TIMESTAMP_STOP 64
#endif

#define SCROLL_MAX 1000

#define INCLUDE_IPV4	(1 << 0)
#define INCLUDE_IPV6	(1 << 1)
#define INCLUDE_UDP	(1 << 2)
#define INCLUDE_TCP	(1 << 3)
#define INCLUDE_DCCP	(1 << 4)
#define INCLUDE_ICMP	(1 << 5)
#define INCLUDE_SCTP	(1 << 6)

static volatile sig_atomic_t sigint = 0;
static int what = INCLUDE_IPV4 | INCLUDE_IPV6 | INCLUDE_TCP, show_src = 0;
static struct flow_list flow_list;
static int nfct_acct_val = -1;

static const char *short_options = "vhTUsDIS46u";
static const struct option long_options[] = {
	{"ipv4",	no_argument,		NULL, '4'},
	{"ipv6",	no_argument,		NULL, '6'},
	{"tcp",		no_argument,		NULL, 'T'},
	{"udp",		no_argument,		NULL, 'U'},
	{"dccp",	no_argument,		NULL, 'D'},
	{"icmp",	no_argument,		NULL, 'I'},
	{"sctp",	no_argument,		NULL, 'S'},
	{"show-src",	no_argument,		NULL, 's'},
	{"update",	no_argument,		NULL, 'u'},
	{"version",	no_argument,		NULL, 'v'},
	{"help",	no_argument,		NULL, 'h'},
	{NULL, 0, NULL, 0}
};

static const char *copyright = "Please report bugs to <bugs@netsniff-ng.org>\n"
	"Copyright (C) 2011-2013 Daniel Borkmann <dborkma@tik.ee.ethz.ch>\n"
	"Copyright (C) 2011-2012 Emmanuel Roullit <emmanuel.roullit@gmail.com>\n"
	"Swiss federal institute of technology (ETH Zurich)\n"
	"License: GNU GPL version 2.0\n"
	"This is free software: you are free to change and redistribute it.\n"
	"There is NO WARRANTY, to the extent permitted by law.";

static const char *const l3proto2str[AF_MAX] = {
	[AF_INET]			= "ipv4",
	[AF_INET6]			= "ipv6",
};

static const char *const l4proto2str[IPPROTO_MAX] = {
	[IPPROTO_TCP]			= "tcp",
	[IPPROTO_UDP]			= "udp",
	[IPPROTO_UDPLITE]               = "udplite",
	[IPPROTO_ICMP]                  = "icmp",
	[IPPROTO_ICMPV6]                = "icmpv6",
	[IPPROTO_SCTP]                  = "sctp",
	[IPPROTO_GRE]                   = "gre",
	[IPPROTO_DCCP]                  = "dccp",
	[IPPROTO_IGMP]			= "igmp",
	[IPPROTO_IPIP]			= "ipip",
	[IPPROTO_EGP]			= "egp",
	[IPPROTO_PUP]			= "pup",
	[IPPROTO_IDP]			= "idp",
	[IPPROTO_RSVP]			= "rsvp",
	[IPPROTO_IPV6]			= "ip6tun",
	[IPPROTO_ESP]			= "esp",
	[IPPROTO_AH]			= "ah",
	[IPPROTO_PIM]			= "pim",
	[IPPROTO_COMP]			= "comp",
};

static const char *const tcp_state2str[TCP_CONNTRACK_MAX] = {
	[TCP_CONNTRACK_NONE]		= "NOSTATE",
	[TCP_CONNTRACK_SYN_SENT]	= "SYN_SENT",
	[TCP_CONNTRACK_SYN_RECV]	= "SYN_RECV",
	[TCP_CONNTRACK_ESTABLISHED]	= "ESTABLISHED",
	[TCP_CONNTRACK_FIN_WAIT]	= "FIN_WAIT",
	[TCP_CONNTRACK_CLOSE_WAIT]	= "CLOSE_WAIT",
	[TCP_CONNTRACK_LAST_ACK]	= "LAST_ACK",
	[TCP_CONNTRACK_TIME_WAIT]	= "TIME_WAIT",
	[TCP_CONNTRACK_CLOSE]		= "CLOSE",
	[TCP_CONNTRACK_SYN_SENT2]	= "SYN_SENT2",
};

static const char *const dccp_state2str[DCCP_CONNTRACK_MAX] = {
	[DCCP_CONNTRACK_NONE]		= "NOSTATE",
	[DCCP_CONNTRACK_REQUEST]	= "REQUEST",
	[DCCP_CONNTRACK_RESPOND]	= "RESPOND",
	[DCCP_CONNTRACK_PARTOPEN]	= "PARTOPEN",
	[DCCP_CONNTRACK_OPEN]		= "OPEN",
	[DCCP_CONNTRACK_CLOSEREQ]	= "CLOSEREQ",
	[DCCP_CONNTRACK_CLOSING]	= "CLOSING",
	[DCCP_CONNTRACK_TIMEWAIT]	= "TIMEWAIT",
	[DCCP_CONNTRACK_IGNORE]		= "IGNORE",
	[DCCP_CONNTRACK_INVALID]	= "INVALID",
};

static const char *const sctp_state2str[SCTP_CONNTRACK_MAX] = {
	[SCTP_CONNTRACK_NONE]		= "NOSTATE",
	[SCTP_CONNTRACK_CLOSED]		= "CLOSED",
	[SCTP_CONNTRACK_COOKIE_WAIT]	= "COOKIE_WAIT",
	[SCTP_CONNTRACK_COOKIE_ECHOED]	= "COOKIE_ECHOED",
	[SCTP_CONNTRACK_ESTABLISHED]	= "ESTABLISHED",
	[SCTP_CONNTRACK_SHUTDOWN_SENT]	= "SHUTDOWN_SENT",
	[SCTP_CONNTRACK_SHUTDOWN_RECD]	= "SHUTDOWN_RECD",
	[SCTP_CONNTRACK_SHUTDOWN_ACK_SENT] = "SHUTDOWN_ACK_SENT",
};

static const struct nfct_filter_ipv4 filter_ipv4 = {
	.addr = __constant_htonl(INADDR_LOOPBACK),
	.mask = 0xffffffff,
};

static const struct nfct_filter_ipv6 filter_ipv6 = {
	.addr = { 0x0, 0x0, 0x0, 0x1 },
	.mask = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff },
};

static void signal_handler(int number)
{
	switch (number) {
	case SIGINT:
	case SIGQUIT:
	case SIGTERM:
		sigint = 1;
		break;
	case SIGHUP:
	default:
		break;
	}
}

static void flow_entry_from_ct(struct flow_entry *n, struct nf_conntrack *ct);
static void flow_entry_get_extended(struct flow_entry *n);

static void help(void)
{
	printf("flowtop %s, top-like netfilter TCP/UDP/SCTP/.. flow tracking\n",
	       VERSION_STRING);
	puts("http://www.netsniff-ng.org\n\n"
	     "Usage: flowtop [options]\n"
	     "Options:\n"
	     "  -4|--ipv4              Show only IPv4 flows (default)\n"
	     "  -6|--ipv6              Show only IPv6 flows (default)\n"
	     "  -T|--tcp               Show only TCP flows (default)\n"
	     "  -U|--udp               Show only UDP flows\n"
	     "  -D|--dccp              Show only DCCP flows\n"
	     "  -I|--icmp              Show only ICMP/ICMPv6 flows\n"
	     "  -S|--sctp              Show only SCTP flows\n"
	     "  -s|--show-src          Also show source, not only dest\n"
	     "  -u|--update            Update GeoIP databases\n"
	     "  -v|--version           Print version and exit\n"
	     "  -h|--help              Print this help and exit\n\n"
	     "Examples:\n"
	     "  flowtop\n"
	     "  flowtop -46UTDISs\n\n"
	     "Note:\n"
	     "  If netfilter is not running, you can activate it with e.g.:\n"
	     "   iptables -A INPUT -p tcp -m state --state ESTABLISHED -j ACCEPT\n"
	     "   iptables -A OUTPUT -p tcp -m state --state NEW,ESTABLISHED -j ACCEPT\n");
	puts(copyright);
	die();
}

static void version(void)
{
	printf("flowtop %s, Git id: %s\n", VERSION_LONG, GITVERSION);
	puts("top-like netfilter TCP/UDP/SCTP/.. flow tracking\n"
	     "http://www.netsniff-ng.org\n");
	puts(copyright);
	die();
}

static inline struct flow_entry *flow_entry_xalloc(void)
{
	return xzmalloc(sizeof(struct flow_entry));
}

static inline void flow_entry_xfree(struct flow_entry *n)
{
	if (n->ct)
		nfct_destroy(n->ct);

	xfree(n);
}

static inline void flow_list_init(struct flow_list *fl)
{
	fl->head = NULL;
	spinlock_init(&fl->lock);
}

static void flow_list_new_entry(struct flow_list *fl, struct nf_conntrack *ct)
{
	struct flow_entry *n = flow_entry_xalloc();

	n->ct = nfct_clone(ct);

	flow_entry_from_ct(n, ct);
	flow_entry_get_extended(n);

	rcu_assign_pointer(n->next, fl->head);
	rcu_assign_pointer(fl->head, n);
}

static struct flow_entry *flow_list_find_id(struct flow_list *fl,
					    uint32_t id)
{
	struct flow_entry *n = rcu_dereference(fl->head);

	while (n != NULL) {
		if (n->flow_id == id)
			return n;

		n = rcu_dereference(n->next);
	}

	return NULL;
}

static struct flow_entry *flow_list_find_prev_id(struct flow_list *fl,
						 uint32_t id)
{
	struct flow_entry *prev = rcu_dereference(fl->head), *next;

	if (prev->flow_id == id)
		return NULL;

	while ((next = rcu_dereference(prev->next)) != NULL) {
		if (next->flow_id == id)
			return prev;

		prev = next;
	}

	return NULL;
}

static void flow_list_update_entry(struct flow_list *fl,
				   struct nf_conntrack *ct)
{
	struct flow_entry *n;

	n = flow_list_find_id(fl, nfct_get_attr_u32(ct, ATTR_ID));
	if (n == NULL) {
		flow_list_new_entry(fl, ct);
		return;
	}

	flow_entry_from_ct(n, ct);
}

static void flow_list_destroy_entry(struct flow_list *fl,
				    struct nf_conntrack *ct)
{
	struct flow_entry *n1, *n2;
	uint32_t id = nfct_get_attr_u32(ct, ATTR_ID);

	n1 = flow_list_find_id(fl, id);
	if (n1) {
		n2 = flow_list_find_prev_id(fl, id);
		if (n2) {
			rcu_assign_pointer(n2->next, n1->next);
			n1->next = NULL;

			flow_entry_xfree(n1);
		} else {
			struct flow_entry *next = fl->head->next;

			flow_entry_xfree(fl->head);
			fl->head = next;
		}
	}
}

static void flow_list_destroy(struct flow_list *fl)
{
	struct flow_entry *n;

	while (fl->head != NULL) {
		n = rcu_dereference(fl->head->next);
		fl->head->next = NULL;

		flow_entry_xfree(fl->head);
		rcu_assign_pointer(fl->head, n);
	}

	synchronize_rcu();
	spinlock_destroy(&fl->lock);
}

static int walk_process(unsigned int pid, struct flow_entry *n)
{
	int ret;
	DIR *dir;
	struct dirent *ent;
	char path[1024];

	if (snprintf(path, sizeof(path), "/proc/%u/fd", pid) == -1)
		panic("giant process name! %u\n", pid);

	dir = opendir(path);
	if (!dir)
        	return 0;

	while ((ent = readdir(dir))) {
		struct stat statbuf;

		if (snprintf(path, sizeof(path), "/proc/%u/fd/%s",
			     pid, ent->d_name) < 0)
			continue;

		if (stat(path, &statbuf) < 0)
			continue;

		if (S_ISSOCK(statbuf.st_mode) && (ino_t) n->inode == statbuf.st_ino) {
			ret = proc_get_cmdline(pid, n->cmdline, sizeof(n->cmdline));
			if (ret < 0)
				panic("Failed to get process cmdline: %s\n", strerror(errno));

			n->procnum = pid;
			closedir(dir);
			return 1;
		}
	}

	closedir(dir);
	return 0;
}

static void walk_processes(struct flow_entry *n)
{
	int ret;
	DIR *dir;
	struct dirent *ent;

	/* n->inode must be set */
	if (n->inode <= 0) {
		n->cmdline[0] = '\0';
		return;
	}

	dir = opendir("/proc");
	if (!dir)
		panic("Cannot open /proc: %s\n", strerror(errno));

	while ((ent = readdir(dir))) {
		const char *name = ent->d_name;
		char *end;
		unsigned int pid = strtoul(name, &end, 10);

		/* not a PID */
		if (pid == 0 && end == name)
			continue;

		ret = walk_process(pid, n);
		if (ret > 0)
			break;
	}

	closedir(dir);
}

static int get_port_inode(uint16_t port, int proto, bool is_ip6)
{
	int ret = -ENOENT;
	char path[128], buff[1024];
	FILE *proc;

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "/proc/net/%s%s",
		 l4proto2str[proto], is_ip6 ? "6" : "");

	proc = fopen(path, "r");
	if (!proc)
		return -EIO;

	memset(buff, 0, sizeof(buff));

	while (fgets(buff, sizeof(buff), proc) != NULL) {
		int inode = 0;
		unsigned int lport = 0;

		buff[sizeof(buff) - 1] = 0;
		if (sscanf(buff, "%*u: %*X:%X %*X:%*X %*X %*X:%*X %*X:%*X "
			   "%*X %*u %*u %u", &lport, &inode) == 2) {
			if ((uint16_t) lport == port) {
				ret = inode;
				break;
			}
		}

		memset(buff, 0, sizeof(buff));
	}

	fclose(proc);
	return ret;
}

#define CP_NFCT(elem, attr, x)				\
	do { n->elem = nfct_get_attr_u##x(ct,(attr)); } while (0)
#define CP_NFCT_BUFF(elem, attr) do {			\
	const uint8_t *buff = nfct_get_attr(ct,(attr));	\
	if (buff != NULL)				\
		memcpy(n->elem, buff, sizeof(n->elem));	\
} while (0)

static void flow_entry_from_ct(struct flow_entry *n, struct nf_conntrack *ct)
{
	CP_NFCT(l3_proto, ATTR_ORIG_L3PROTO, 8);
	CP_NFCT(l4_proto, ATTR_ORIG_L4PROTO, 8);

	CP_NFCT(ip4_src_addr, ATTR_ORIG_IPV4_SRC, 32);
	CP_NFCT(ip4_dst_addr, ATTR_ORIG_IPV4_DST, 32);

	CP_NFCT(port_src, ATTR_ORIG_PORT_SRC, 16);
	CP_NFCT(port_dst, ATTR_ORIG_PORT_DST, 16);

	CP_NFCT(status, ATTR_STATUS, 32);

	CP_NFCT(tcp_state, ATTR_TCP_STATE, 8);
	CP_NFCT(tcp_flags, ATTR_TCP_FLAGS_ORIG, 8);
	CP_NFCT(sctp_state, ATTR_SCTP_STATE, 8);
	CP_NFCT(dccp_state, ATTR_DCCP_STATE, 8);

	CP_NFCT(counter_pkts, ATTR_ORIG_COUNTER_PACKETS, 64);
	CP_NFCT(counter_bytes, ATTR_ORIG_COUNTER_BYTES, 64);

	CP_NFCT(timestamp_start, ATTR_TIMESTAMP_START, 64);
	CP_NFCT(timestamp_stop, ATTR_TIMESTAMP_STOP, 64);

	CP_NFCT(flow_id, ATTR_ID, 32);
	CP_NFCT(use, ATTR_USE, 32);

	CP_NFCT_BUFF(ip6_src_addr, ATTR_ORIG_IPV6_SRC);
	CP_NFCT_BUFF(ip6_dst_addr, ATTR_ORIG_IPV6_DST);

	n->port_src = ntohs(n->port_src);
	n->port_dst = ntohs(n->port_dst);

	n->ip4_src_addr = ntohl(n->ip4_src_addr);
	n->ip4_dst_addr = ntohl(n->ip4_dst_addr);
}

enum flow_entry_direction {
	flow_entry_src,
	flow_entry_dst,
};

static inline bool flow_entry_get_extended_is_dns(struct flow_entry *n)
{
	/* We don't want to analyze / display DNS itself, since we
	 * use it to resolve reverse dns.
	 */
	return n->port_src == 53 || n->port_dst == 53;
}

#define SELFLD(dir,src_member,dst_member)	\
	(((dir) == flow_entry_src) ? n->src_member : n->dst_member)

static void flow_entry_get_sain4_obj(struct flow_entry *n,
				     enum flow_entry_direction dir,
				     struct sockaddr_in *sa)
{
	memset(sa, 0, sizeof(*sa));
	sa->sin_family = PF_INET;
	sa->sin_addr.s_addr = htonl(SELFLD(dir, ip4_src_addr, ip4_dst_addr));
}

static void flow_entry_get_sain6_obj(struct flow_entry *n,
				     enum flow_entry_direction dir,
				     struct sockaddr_in6 *sa)
{
	memset(sa, 0, sizeof(*sa));
	sa->sin6_family = PF_INET6;

	memcpy(&sa->sin6_addr, SELFLD(dir, ip6_src_addr, ip6_dst_addr),
	       sizeof(sa->sin6_addr));
}

static void
flow_entry_geo_city_lookup_generic(struct flow_entry *n,
				   enum flow_entry_direction dir)
{
	struct sockaddr_in sa4;
	struct sockaddr_in6 sa6;
	const char *city = NULL;

	switch (n->l3_proto) {
	default:
		bug();

	case AF_INET:
		flow_entry_get_sain4_obj(n, dir, &sa4);
		city = geoip4_city_name(&sa4);
		break;

	case AF_INET6:
		flow_entry_get_sain6_obj(n, dir, &sa6);
		city = geoip6_city_name(&sa6);
		break;
	}

	build_bug_on(sizeof(n->city_src) != sizeof(n->city_dst));

	if (city) {
		memcpy(SELFLD(dir, city_src, city_dst), city,
		       min(sizeof(n->city_src), strlen(city)));
	} else {
		memset(SELFLD(dir, city_src, city_dst), 0,
		       sizeof(n->city_src));
	}
}

static void
flow_entry_geo_country_lookup_generic(struct flow_entry *n,
				      enum flow_entry_direction dir)
{
	struct sockaddr_in sa4;
	struct sockaddr_in6 sa6;
	const char *country = NULL;

	switch (n->l3_proto) {
	default:
		bug();

	case AF_INET:
		flow_entry_get_sain4_obj(n, dir, &sa4);
		country = geoip4_country_name(&sa4);
		break;

	case AF_INET6:
		flow_entry_get_sain6_obj(n, dir, &sa6);
		country = geoip6_country_name(&sa6);
		break;
	}

	build_bug_on(sizeof(n->country_src) != sizeof(n->country_dst));

	if (country) {
		memcpy(SELFLD(dir, country_src, country_dst), country,
		       min(sizeof(n->country_src), strlen(country)));
	} else {
		memset(SELFLD(dir, country_src, country_dst), 0,
		       sizeof(n->country_src));
	}
}

static void flow_entry_get_extended_geo(struct flow_entry *n,
					enum flow_entry_direction dir)
{
	flow_entry_geo_city_lookup_generic(n, dir);
	flow_entry_geo_country_lookup_generic(n, dir);
}

static void flow_entry_get_extended_revdns(struct flow_entry *n,
					   enum flow_entry_direction dir)
{
	size_t sa_len;
	struct sockaddr_in sa4;
	struct sockaddr_in6 sa6;
	struct sockaddr *sa;
	struct hostent *hent;

	switch (n->l3_proto) {
	default:
		bug();

	case AF_INET:
		flow_entry_get_sain4_obj(n, dir, &sa4);
		sa = (struct sockaddr *) &sa4;
		sa_len = sizeof(sa4);
		hent = gethostbyaddr(&sa4.sin_addr, sizeof(sa4.sin_addr), AF_INET);
		break;

	case AF_INET6:
		flow_entry_get_sain6_obj(n, dir, &sa6);
		sa = (struct sockaddr *) &sa6;
		sa_len = sizeof(sa6);
		hent = gethostbyaddr(&sa6.sin6_addr, sizeof(sa6.sin6_addr), AF_INET6);
		break;
	}

	build_bug_on(sizeof(n->rev_dns_src) != sizeof(n->rev_dns_dst));
	getnameinfo(sa, sa_len, SELFLD(dir, rev_dns_src, rev_dns_dst),
		    sizeof(n->rev_dns_src), NULL, 0, NI_NUMERICHOST);

	if (hent) {
		memset(n->rev_dns_dst, 0, sizeof(n->rev_dns_dst));
		memcpy(SELFLD(dir, rev_dns_src, rev_dns_dst),
		       hent->h_name, min(sizeof(n->rev_dns_src),
					 strlen(hent->h_name)));
	}
}

static void flow_entry_get_extended(struct flow_entry *n)
{
	if (n->flow_id == 0 || flow_entry_get_extended_is_dns(n))
		return;

	flow_entry_get_extended_revdns(n, flow_entry_src);
	flow_entry_get_extended_geo(n, flow_entry_src);

	flow_entry_get_extended_revdns(n, flow_entry_dst);
	flow_entry_get_extended_geo(n, flow_entry_dst);

	/* Lookup application */
	n->inode = get_port_inode(n->port_src, n->l4_proto,
				  n->l3_proto == AF_INET6);
	if (n->inode > 0)
		walk_processes(n);
}

static uint16_t presenter_get_port(uint16_t src, uint16_t dst, bool is_tcp)
{
	if (src < dst && src < 1024) {
		return src;
	} else if (dst < src && dst < 1024) {
		return dst;
	} else {
		const char *tmp1, *tmp2;
		if (is_tcp) {
			tmp1 = lookup_port_tcp(src);
			tmp2 = lookup_port_tcp(dst);
		} else {
			tmp1 = lookup_port_udp(src);
			tmp2 = lookup_port_udp(dst);
		}
		if (tmp1 && !tmp2) {
			return src;
		} else if (!tmp1 && tmp2) {
			return dst;
		} else {
			if (src < dst)
				return src;
			else
				return dst;
		}
	}
}

static char *bandw2str(double bytes, char *buf, size_t len)
{
	if (bytes > 1000000000.)
		snprintf(buf, len, "%.1fG", bytes / 1000000000.);
	else if (bytes > 1000000.)
		snprintf(buf, len, "%.1fM", bytes / 1000000.);
	else if (bytes > 1000.)
		snprintf(buf, len, "%.1fK", bytes / 1000.);
	else
		snprintf(buf, len, "%g", bytes);

	return buf;
}

static void presenter_screen_do_line(WINDOW *screen, struct flow_entry *n,
				     unsigned int *line)
{
	char tmp[128], *pname = NULL;
	uint16_t port;

	mvwprintw(screen, *line, 2, "");

	/* PID, application name */
	if (n->procnum > 0) {
		slprintf(tmp, sizeof(tmp), "%s(%d)", basename(n->cmdline),
			 n->procnum);

		printw("[");
		attron(COLOR_PAIR(3));
		printw("%s", tmp);
		attroff(COLOR_PAIR(3));
		printw("]:");
	}

	/* L3 protocol, L4 protocol, states */
	printw("%s:%s", l3proto2str[n->l3_proto], l4proto2str[n->l4_proto]);
	printw("[");
	attron(COLOR_PAIR(3));
	switch (n->l4_proto) {
	case IPPROTO_TCP:
		printw("%s", tcp_state2str[n->tcp_state]);
		break;
	case IPPROTO_SCTP:
		printw("%s", sctp_state2str[n->sctp_state]);
		break;
	case IPPROTO_DCCP:
		printw("%s", dccp_state2str[n->dccp_state]);
		break;
	case IPPROTO_UDP:
	case IPPROTO_UDPLITE:
	case IPPROTO_ICMP:
	case IPPROTO_ICMPV6:
		printw("NOSTATE");
		break;
	}
	attroff(COLOR_PAIR(3));
	printw("]");

	/* Guess application port */
	switch (n->l4_proto) {
	case IPPROTO_TCP:
		port = presenter_get_port(n->port_src, n->port_dst, true);
		pname = lookup_port_tcp(port);
		break;
	case IPPROTO_UDP:
	case IPPROTO_UDPLITE:
		port = presenter_get_port(n->port_src, n->port_dst, false);
		pname = lookup_port_udp(port);
		break;
	}
	if (pname) {
		attron(A_BOLD);
		printw(":%s", pname);
		attroff(A_BOLD);
	}
	printw(" ->");

	/* Number packets, bytes */
	if (n->counter_pkts > 0 && n->counter_bytes > 0) {
		char bytes_str[64];

		printw(" (%"PRIu64" pkts, %s bytes) ->", n->counter_pkts,
		       bandw2str(n->counter_bytes, bytes_str,
				 sizeof(bytes_str) - 1));
	}

	/* Show source information: reverse DNS, port, country, city */
	if (show_src) {
		attron(COLOR_PAIR(1));
		mvwprintw(screen, ++(*line), 8, "src: %s", n->rev_dns_src);
		attroff(COLOR_PAIR(1));

		printw(":%"PRIu16, n->port_src);

		if (n->country_src[0]) {
			printw(" (");

			attron(COLOR_PAIR(4));
			printw("%s", n->country_src);
			attroff(COLOR_PAIR(4));

			if (n->city_src[0])
				printw(", %s", n->city_src);

			printw(")");
		}

		printw(" => ");
	}

	/* Show dest information: reverse DNS, port, country, city */
	attron(COLOR_PAIR(2));
	mvwprintw(screen, ++(*line), 8, "dst: %s", n->rev_dns_dst);
	attroff(COLOR_PAIR(2));

	printw(":%"PRIu16, n->port_dst);

	if (n->country_dst[0]) {
		printw(" (");

		attron(COLOR_PAIR(4));
		printw("%s", n->country_dst);
		attroff(COLOR_PAIR(4));

		if (n->city_dst[0])
			printw(", %s", n->city_dst);

		printw(")");
	}
}

static inline bool presenter_flow_wrong_state(struct flow_entry *n)
{
	switch (n->l4_proto) {
	case IPPROTO_TCP:
		switch (n->tcp_state) {
		case TCP_CONNTRACK_SYN_SENT:
		case TCP_CONNTRACK_SYN_RECV:
		case TCP_CONNTRACK_ESTABLISHED:
		case TCP_CONNTRACK_FIN_WAIT:
		case TCP_CONNTRACK_CLOSE_WAIT:
		case TCP_CONNTRACK_LAST_ACK:
		case TCP_CONNTRACK_TIME_WAIT:
		case TCP_CONNTRACK_CLOSE:
		case TCP_CONNTRACK_SYN_SENT2:
		case TCP_CONNTRACK_NONE:
			return false;
			break;
		}
		break;
	case IPPROTO_SCTP:
		switch (n->sctp_state) {
		case SCTP_CONNTRACK_NONE:
		case SCTP_CONNTRACK_CLOSED:
		case SCTP_CONNTRACK_COOKIE_WAIT:
		case SCTP_CONNTRACK_COOKIE_ECHOED:
		case SCTP_CONNTRACK_ESTABLISHED:
		case SCTP_CONNTRACK_SHUTDOWN_SENT:
		case SCTP_CONNTRACK_SHUTDOWN_RECD:
		case SCTP_CONNTRACK_SHUTDOWN_ACK_SENT:
			return false;
			break;
		}
		break;
	case IPPROTO_DCCP:
		switch (n->dccp_state) {
		case DCCP_CONNTRACK_NONE:
		case DCCP_CONNTRACK_REQUEST:
		case DCCP_CONNTRACK_RESPOND:
		case DCCP_CONNTRACK_PARTOPEN:
		case DCCP_CONNTRACK_OPEN:
		case DCCP_CONNTRACK_CLOSEREQ:
		case DCCP_CONNTRACK_CLOSING:
		case DCCP_CONNTRACK_TIMEWAIT:
		case DCCP_CONNTRACK_IGNORE:
		case DCCP_CONNTRACK_INVALID:
			return false;
			break;
		}
		break;
	case IPPROTO_UDP:
	case IPPROTO_UDPLITE:
	case IPPROTO_ICMP:
	case IPPROTO_ICMPV6:
		return false;
		break;
	}

	return true;
}

static void presenter_screen_update(WINDOW *screen, struct flow_list *fl,
				    int skip_lines)
{
	int maxy;
	int skip_left = skip_lines;
	unsigned int flows = 0;
	unsigned int line = 3;
	struct flow_entry *n;

	curs_set(0);

	maxy = getmaxy(screen);
	maxy -= 6;

	start_color();
	init_pair(1, COLOR_RED, COLOR_BLACK);
	init_pair(2, COLOR_BLUE, COLOR_BLACK);
	init_pair(3, COLOR_YELLOW, COLOR_BLACK);
	init_pair(4, COLOR_GREEN, COLOR_BLACK);

	wclear(screen);
	clear();

	rcu_read_lock();

	n = rcu_dereference(fl->head);
	if (!n)
		mvwprintw(screen, line, 2, "(No active sessions! "
			  "Is netfilter running?)");

	for (; n; n = rcu_dereference(n->next)) {
		n->is_visible = false;
		if (presenter_get_port(n->port_src, n->port_dst, false) == 53)
			continue;

		if (presenter_flow_wrong_state(n))
			continue;

		/* count only flows which might be showed */
		flows++;

		if (maxy <= 0)
			continue;

		if (skip_left > 0) {
			skip_left--;
			continue;
		}

		n->is_visible = true;

		presenter_screen_do_line(screen, n, &line);

		line++;
		maxy -= (2 + 1 * show_src);
	}

	mvwprintw(screen, 1, 2, "Kernel netfilter flows(%u) for %s%s%s%s%s%s"
		  "[+%d]", flows, what & INCLUDE_TCP ? "TCP, " : "" ,
		  what & INCLUDE_UDP ? "UDP, " : "",
		  what & INCLUDE_SCTP ? "SCTP, " : "",
		  what & INCLUDE_DCCP ? "DCCP, " : "",
		  what & INCLUDE_ICMP && what & INCLUDE_IPV4 ? "ICMP, " : "",
		  what & INCLUDE_ICMP && what & INCLUDE_IPV6 ? "ICMP6, " : "",
		  skip_lines);

	rcu_read_unlock();

	wrefresh(screen);
	refresh();
}

static void presenter(void)
{
	int skip_lines = 0;
	WINDOW *screen;

	lookup_init_ports(PORTS_TCP);
	lookup_init_ports(PORTS_UDP);
	screen = screen_init(false);

	rcu_register_thread();
	while (!sigint) {
		switch (getch()) {
		case 'q':
			sigint = 1;
			break;
		case KEY_UP:
		case 'u':
		case 'k':
			skip_lines--;
			if (skip_lines < 0)
				skip_lines = 0;
			break;
		case KEY_DOWN:
		case 'd':
		case 'j':
			skip_lines++;
			if (skip_lines > SCROLL_MAX)
				skip_lines = SCROLL_MAX;
			break;
		default:
			fflush(stdin);
			break;
		}

		presenter_screen_update(screen, &flow_list, skip_lines);
		usleep(200000);
	}
	rcu_unregister_thread();

	screen_end();
	lookup_cleanup_ports(PORTS_UDP);
	lookup_cleanup_ports(PORTS_TCP);
}

static int collector_cb(enum nf_conntrack_msg_type type,
			struct nf_conntrack *ct, void *data __maybe_unused)
{
	if (sigint)
		return NFCT_CB_STOP;

	synchronize_rcu();
	spinlock_lock(&flow_list.lock);

	switch (type) {
	case NFCT_T_NEW:
		flow_list_new_entry(&flow_list, ct);
		break;
	case NFCT_T_UPDATE:
		flow_list_update_entry(&flow_list, ct);
		break;
	case NFCT_T_DESTROY:
		flow_list_destroy_entry(&flow_list, ct);
		break;
	default:
		break;
	}

	spinlock_unlock(&flow_list.lock);

	return NFCT_CB_CONTINUE;
}

static inline void collector_flush(void)
{
	struct nfct_handle *nfct = nfct_open(CONNTRACK, 0);
	uint8_t family;

	if (!nfct)
		panic("Cannot create a nfct to flush connections: %s\n",
			strerror(errno));

	family = AF_INET;
	nfct_query(nfct, NFCT_Q_FLUSH, &family);

	family = AF_INET6;
	nfct_query(nfct, NFCT_Q_FLUSH, &family);

	nfct_close(nfct);
}

static void restore_sysctl(void *value)
{
	int int_val = *(int *)value;

	if (int_val == 0)
		sysctl_set_int("net/netfilter/nf_conntrack_acct", int_val);
}

static void on_panic_handler(void *arg)
{
	restore_sysctl(arg);
	screen_end();
}

static void conntrack_acct_enable(void)
{
	/* We can still work w/o traffic accounting so just warn about error */
	if (sysctl_get_int("net/netfilter/nf_conntrack_acct", &nfct_acct_val)) {
		fprintf(stderr, "Can't read net/netfilter/nf_conntrack_acct: %s\n",
			strerror(errno));
	}

	if (nfct_acct_val == 1)
		return;

	if (sysctl_set_int("net/netfilter/nf_conntrack_acct", 1)) {
		fprintf(stderr, "Can't write net/netfilter/nf_conntrack_acct: %s\n",
			strerror(errno));
	}
}

static int dump_cb(enum nf_conntrack_msg_type type,
		   struct nf_conntrack *ct, void *data __maybe_unused)
{
	struct flow_entry *n;

	if (type != NFCT_T_UPDATE)
		return NFCT_CB_CONTINUE;

	if (sigint)
		return NFCT_CB_STOP;

	n = flow_list_find_id(&flow_list, nfct_get_attr_u32(ct, ATTR_ID));
	if (!n)
		return NFCT_CB_CONTINUE;

	flow_entry_from_ct(n, ct);

	return NFCT_CB_CONTINUE;
}

static void collector_refresh_flows(struct nfct_handle *handle)
{
	struct flow_entry *n;

	n = rcu_dereference(flow_list.head);
	for (; n; n = rcu_dereference(n->next)) {
		if (!n->is_visible)
			continue;

		nfct_query(handle, NFCT_Q_GET, n->ct);
	}
}

static void *collector(void *null __maybe_unused)
{
	struct nfct_handle *ct_event;
	struct nfct_handle *ct_dump;
	struct nfct_filter *filter;
	struct pollfd poll_fd[1];
	int ret;

	ct_event = nfct_open(CONNTRACK, NF_NETLINK_CONNTRACK_NEW |
				      NF_NETLINK_CONNTRACK_UPDATE |
				      NF_NETLINK_CONNTRACK_DESTROY);
	if (!ct_event)
		panic("Cannot create a nfct handle: %s\n", strerror(errno));

	filter = nfct_filter_create();
	if (!filter)
		panic("Cannot create a nfct filter: %s\n", strerror(errno));

	ret = nfct_filter_attach(nfct_fd(ct_event), filter);
	if (ret < 0)
		panic("Cannot attach filter to handle: %s\n", strerror(errno));

	if (what & INCLUDE_UDP) {
		nfct_filter_add_attr_u32(filter, NFCT_FILTER_L4PROTO, IPPROTO_UDP);
		nfct_filter_add_attr_u32(filter, NFCT_FILTER_L4PROTO, IPPROTO_UDPLITE);
	}
	if (what & INCLUDE_TCP)
		nfct_filter_add_attr_u32(filter, NFCT_FILTER_L4PROTO, IPPROTO_TCP);
	if (what & INCLUDE_DCCP)
		nfct_filter_add_attr_u32(filter, NFCT_FILTER_L4PROTO, IPPROTO_DCCP);
	if (what & INCLUDE_SCTP)
		nfct_filter_add_attr_u32(filter, NFCT_FILTER_L4PROTO, IPPROTO_SCTP);
	if (what & INCLUDE_ICMP && what & INCLUDE_IPV4)
		nfct_filter_add_attr_u32(filter, NFCT_FILTER_L4PROTO, IPPROTO_ICMP);
	if (what & INCLUDE_ICMP && what & INCLUDE_IPV6)
		nfct_filter_add_attr_u32(filter, NFCT_FILTER_L4PROTO, IPPROTO_ICMPV6);
	if (what & INCLUDE_IPV4) {
		nfct_filter_set_logic(filter, NFCT_FILTER_SRC_IPV4, NFCT_FILTER_LOGIC_NEGATIVE);
		nfct_filter_add_attr(filter, NFCT_FILTER_SRC_IPV4, &filter_ipv4);
	}
	if (what & INCLUDE_IPV6) {
		nfct_filter_set_logic(filter, NFCT_FILTER_SRC_IPV6, NFCT_FILTER_LOGIC_NEGATIVE);
		nfct_filter_add_attr(filter, NFCT_FILTER_SRC_IPV6, &filter_ipv6);
	}

	ret = nfct_filter_attach(nfct_fd(ct_event), filter);
	if (ret < 0)
		panic("Cannot attach filter to handle: %s\n", strerror(errno));

	nfct_filter_destroy(filter);

	nfct_callback_register(ct_event, NFCT_T_ALL, collector_cb, NULL);
	flow_list_init(&flow_list);

	ct_dump = nfct_open(CONNTRACK, NF_NETLINK_CONNTRACK_UPDATE);
	if (!ct_dump)
		panic("Cannot create a nfct handle: %s\n", strerror(errno));

	nfct_callback_register(ct_dump, NFCT_T_ALL, dump_cb, NULL);

	poll_fd[0].fd = nfct_fd(ct_event);
	poll_fd[0].events = POLLIN;

	if (fcntl(nfct_fd(ct_event), F_SETFL, O_NONBLOCK) == -1)
		panic("Cannot set non-blocking socket: fcntl(): %s\n",
		      strerror(errno));

	if (fcntl(nfct_fd(ct_dump), F_SETFL, O_NONBLOCK) == -1)
		panic("Cannot set non-blocking socket: fcntl(): %s\n",
		      strerror(errno));

	collector_flush();

	rcu_register_thread();

	while (!sigint && ret >= 0) {
		int status;

		usleep(300000);

		collector_refresh_flows(ct_dump);

		status = poll(poll_fd, 1, 0);
		if (status < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;

			panic("Error while polling: %s\n", strerror(errno));
		} else if (status == 0) {
			continue;
		}

		if (poll_fd[0].revents & POLLIN)
			nfct_catch(ct_event);
	}

	rcu_unregister_thread();

	flow_list_destroy(&flow_list);
	nfct_close(ct_event);
	nfct_close(ct_dump);

	pthread_exit(NULL);
}

int main(int argc, char **argv)
{
	pthread_t tid;
	int ret, c, opt_index, what_cmd = 0;

	setfsuid(getuid());
	setfsgid(getgid());

	while ((c = getopt_long(argc, argv, short_options, long_options,
	       &opt_index)) != EOF) {
		switch (c) {
		case '4':
			what_cmd |= INCLUDE_IPV4;
			break;
		case '6':
			what_cmd |= INCLUDE_IPV6;
			break;
		case 'T':
			what_cmd |= INCLUDE_TCP;
			break;
		case 'U':
			what_cmd |= INCLUDE_UDP;
			break;
		case 'D':
			what_cmd |= INCLUDE_DCCP;
			break;
		case 'I':
			what_cmd |= INCLUDE_ICMP;
			break;
		case 'S':
			what_cmd |= INCLUDE_SCTP;
			break;
		case 's':
			show_src = 1;
			break;
		case 'u':
			update_geoip();
			die();
			break;
		case 'h':
			help();
			break;
		case 'v':
			version();
			break;
		default:
			break;
		}
	}

	if (what_cmd > 0)
		what = what_cmd;

	rcu_init();

	register_signal(SIGINT, signal_handler);
	register_signal(SIGQUIT, signal_handler);
	register_signal(SIGTERM, signal_handler);
	register_signal(SIGHUP, signal_handler);

	panic_handler_add(on_panic_handler, &nfct_acct_val);

	conntrack_acct_enable();

	init_geoip(1);

	ret = pthread_create(&tid, NULL, collector, NULL);
	if (ret < 0)
		panic("Cannot create phthread!\n");

	presenter();

	destroy_geoip();

	restore_sysctl(&nfct_acct_val);

	return 0;
}
