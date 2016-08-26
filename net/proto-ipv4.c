#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <stdlib.h>
#include <string.h>
#include <linux/types.h>
#include <arpa/inet.h>
#include <linux/mroute.h>
#include <linux/if.h>
#include <limits.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter_bridge/ebtables.h>
#include <linux/netfilter_arp/arp_tables.h>
#include <linux/netfilter/ipset/ip_set.h>
#include <linux/ip_vs.h>
#include "sanitise.h"
#include "compat.h"
#include "net.h"
#include "config.h"
#include "random.h"
#include "uid.h"
#include "utils.h"	// ARRAY_SIZE

/* workaround for <linux/in.h> vs. <netinet/in.h> */
#ifndef IP_MULTICAST_ALL
#define IP_MULTICAST_ALL 49
#endif

static int previous_ip;
static unsigned int ip_lifetime = 0;

struct addrtext {
	const char *name;
	int classmask;
};

#define SLASH8 0xffffff
#define SLASH12 0xfffff
#define SLASH16 0xffff
#define SLASH24 0xff
#define SLASH32 0

static in_addr_t new_ipv4_addr(void)
{
	in_addr_t v4;
	const struct addrtext addresses[] = {
		{ "0.0.0.0", SLASH8 },
		{ "10.0.0.0", SLASH8 },
		{ "127.0.0.0", SLASH8 },
		{ "169.254.0.0", SLASH16 },	/* link-local */
		{ "172.16.0.0", SLASH12 },
		{ "192.88.99.0", SLASH24 },	/* 6to4 anycast */
		{ "192.168.0.0", SLASH16 },
		{ "224.0.0.0", SLASH24 },	/* multi-cast */
		{ "255.255.255.255", SLASH32 },
	};

	int entry = rnd() % ARRAY_SIZE(addresses);
	const char *p = addresses[entry].name;

	inet_pton(AF_INET, p, &v4);

	if (addresses[entry].classmask != SLASH32)
		v4 |= htonl(rnd() % addresses[entry].classmask);

	return v4;
}

in_addr_t random_ipv4_address(void)
{
	int addr;

	if (ip_lifetime != 0) {
		ip_lifetime--;
		return previous_ip;
	}

	/* 90% of the time, just do localhost. */
	if (!(ONE_IN(10)))
		addr = 0x7f000001;
	else
		addr = new_ipv4_addr();

	previous_ip = addr;
	ip_lifetime = 5;

	return addr;
}

static void ipv4_gen_sockaddr(struct sockaddr **addr, socklen_t *addrlen)
{
	struct sockaddr_in *ipv4;

	ipv4 = zmalloc(sizeof(struct sockaddr_in));

	ipv4->sin_family = PF_INET;
	ipv4->sin_addr.s_addr = random_ipv4_address();
	ipv4->sin_port = htons(rnd() % 65535);

	*addr = (struct sockaddr *) ipv4;
	*addrlen = sizeof(struct sockaddr_in);
}

struct ipproto {
	unsigned int proto;
	unsigned int type;
};

static void inet_rand_socket(struct socket_triplet *st)
{
	struct ipproto ipprotos[] = {
		{ .proto = IPPROTO_IP, },
		{ .proto = IPPROTO_ICMP, .type = SOCK_DGRAM },
		{ .proto = IPPROTO_IGMP, },
		{ .proto = IPPROTO_IPIP, },
		{ .proto = IPPROTO_TCP, .type = SOCK_STREAM },
		{ .proto = IPPROTO_EGP, },
		{ .proto = IPPROTO_PUP, },
		{ .proto = IPPROTO_UDP, .type = SOCK_DGRAM },
		{ .proto = IPPROTO_IDP, },
		{ .proto = IPPROTO_TP, },
		{ .proto = IPPROTO_DCCP, .type = SOCK_DCCP },
		{ .proto = IPPROTO_IPV6, },
		{ .proto = IPPROTO_RSVP, },
		{ .proto = IPPROTO_GRE, },
		{ .proto = IPPROTO_ESP, },
		{ .proto = IPPROTO_AH, },
		{ .proto = IPPROTO_MTP, },
		{ .proto = IPPROTO_BEETPH, },
		{ .proto = IPPROTO_ENCAP, },
		{ .proto = IPPROTO_PIM, },
		{ .proto = IPPROTO_COMP, },
		{ .proto = IPPROTO_SCTP, .type = SOCK_SEQPACKET },
		{ .proto = IPPROTO_UDPLITE, .type = SOCK_DGRAM },
		{ .proto = IPPROTO_RAW, },
		{ .proto = IPPROTO_MPLS, },
	};
	unsigned char val;

	if (orig_uid == 0) {
		/* half the time, use raw sockets */
		st->type = SOCK_RAW;
		if (RAND_BOOL())
			return;
	}

	/* The rest of the time, use the correct type if present. */
	val = rnd() % ARRAY_SIZE(ipprotos);
	st->protocol = ipprotos[val].proto;
	if (ipprotos[val].type != 0)
		st->type = ipprotos[val].type;
	else {
		int types[] = { SOCK_STREAM, SOCK_DGRAM, SOCK_SEQPACKET };
		st->type = RAND_ARRAY(types);
	}
}

static const struct sock_option ip_opts[] = {
	{ .name = IP_TOS, },
	{ .name = IP_TTL, },
	{ .name = IP_HDRINCL, },
	{ .name = IP_OPTIONS, },
	{ .name = IP_ROUTER_ALERT, },
	{ .name = IP_RECVOPTS, },
	{ .name = IP_RETOPTS, },
	{ .name = IP_PKTINFO, },
	{ .name = IP_PKTOPTIONS, },
	{ .name = IP_MTU_DISCOVER, },
	{ .name = IP_RECVERR, },
	{ .name = IP_RECVTTL, },
	{ .name = IP_RECVTOS, },
	{ .name = IP_MTU, },
	{ .name = IP_FREEBIND, },
	{ .name = IP_IPSEC_POLICY, },
	{ .name = IP_XFRM_POLICY, },
	{ .name = IP_PASSSEC, },
	{ .name = IP_TRANSPARENT, },
	{ .name = IP_ORIGDSTADDR, },
	{ .name = IP_MINTTL, },
	{ .name = IP_NODEFRAG, },
	{ .name = IP_CHECKSUM, },
	{ .name = IP_BIND_ADDRESS_NO_PORT, },
	{ .name = IP_MULTICAST_IF, .len = sizeof(struct ip_mreqn) },
	{ .name = IP_MULTICAST_TTL, },
	{ .name = IP_MULTICAST_LOOP, },
	{ .name = IP_ADD_MEMBERSHIP, .len = sizeof(struct ip_mreqn) },
	{ .name = IP_DROP_MEMBERSHIP, .len = sizeof(struct ip_mreqn) },
	{ .name = IP_UNBLOCK_SOURCE, .len = sizeof(struct ip_mreq_source) },
	{ .name = IP_BLOCK_SOURCE, .len = sizeof(struct ip_mreq_source) },
	{ .name = IP_ADD_SOURCE_MEMBERSHIP, .len = sizeof(struct ip_mreq_source) },
	{ .name = IP_DROP_SOURCE_MEMBERSHIP, .len = sizeof(struct ip_mreq_source) },
	{ .name = IP_MSFILTER, },
	{ .name = MCAST_JOIN_GROUP, .len = sizeof(struct group_req) },
	{ .name = MCAST_BLOCK_SOURCE, .len = sizeof(struct group_source_req) },
	{ .name = MCAST_UNBLOCK_SOURCE, .len = sizeof(struct group_source_req) },
	{ .name = MCAST_LEAVE_GROUP, .len = sizeof(struct group_req) },
	{ .name = MCAST_JOIN_SOURCE_GROUP, .len = sizeof(struct group_source_req) },
	{ .name = MCAST_LEAVE_SOURCE_GROUP, .len = sizeof(struct group_source_req) },
	{ .name = MCAST_MSFILTER, },
	{ .name = IP_MULTICAST_ALL, },
	{ .name = IP_UNICAST_IF, },
	{ .name = MRT_INIT, },
	{ .name = MRT_DONE, },
	{ .name = MRT_ADD_VIF, .len = sizeof(struct vifctl) },
	{ .name = MRT_DEL_VIF, .len = sizeof(struct vifctl) },
	{ .name = MRT_ADD_MFC, .len = sizeof(struct mfcctl) },
	{ .name = MRT_DEL_MFC, .len = sizeof(struct mfcctl) },
	{ .name = MRT_VERSION, },
	{ .name = MRT_ASSERT, },
	{ .name = MRT_PIM, },
	{ .name = MRT_TABLE, .len = sizeof(__u32) },
	{ .name = MRT_ADD_MFC_PROXY, .len = sizeof(struct mfcctl) },
	{ .name = MRT_DEL_MFC_PROXY, .len = sizeof(struct mfcctl) },
	{ .name = IPT_SO_SET_REPLACE, },
	{ .name = IPT_SO_SET_ADD_COUNTERS, },
	{ .name = EBT_SO_SET_ENTRIES, },
	{ .name = EBT_SO_SET_COUNTERS, },
	{ .name = ARPT_SO_SET_REPLACE, },
	{ .name = ARPT_SO_SET_ADD_COUNTERS, },
	{ .name = SO_IP_SET, },	/* ugh, demux's based upon data */
	{ .name = IP_VS_SO_SET_NONE, },
	{ .name = IP_VS_SO_SET_INSERT, },
	{ .name = IP_VS_SO_SET_ADD, },
	{ .name = IP_VS_SO_SET_EDIT, },
	{ .name = IP_VS_SO_SET_DEL, },
	{ .name = IP_VS_SO_SET_FLUSH, },
	{ .name = IP_VS_SO_SET_LIST, },
	{ .name = IP_VS_SO_SET_ADDDEST, },
	{ .name = IP_VS_SO_SET_DELDEST, },
	{ .name = IP_VS_SO_SET_EDITDEST, },
	{ .name = IP_VS_SO_SET_TIMEOUT, },
	{ .name = IP_VS_SO_SET_STARTDAEMON, },
	{ .name = IP_VS_SO_SET_STOPDAEMON, },
	{ .name = IP_VS_SO_SET_RESTORE, },
	{ .name = IP_VS_SO_SET_SAVE, },
	{ .name = IP_VS_SO_SET_ZERO, },
};

static void ip_setsockopt(struct sockopt *so, __unused__ struct socket_triplet *triplet)
{
	unsigned char val;
	struct ip_mreq_source *ms;
	int mcaddr;

	val = rnd() % ARRAY_SIZE(ip_opts);
	so->optname = ip_opts[val].name;
	so->optlen = sockoptlen(ip_opts[val].len);

	switch (so->optname) {
	case IP_OPTIONS:
		so->optlen = rnd() % 40;
		break;

	case IP_MULTICAST_IF:
	case IP_ADD_MEMBERSHIP:
	case IP_DROP_MEMBERSHIP:
		mcaddr = 0xe0000000 | rnd() % 0xff;
		if (RAND_BOOL()) {
			struct ip_mreqn *mrn;

			mrn = (struct ip_mreqn *) so->optval;
			mrn->imr_multiaddr.s_addr = htonl(mcaddr);
			mrn->imr_address.s_addr = random_ipv4_address();
			mrn->imr_ifindex = rand32();
			so->optlen = sizeof(struct ip_mreqn);
		} else {
			struct ip_mreq *mr;

			mr = (struct ip_mreq *) so->optval;
			mr->imr_multiaddr.s_addr = htonl(mcaddr);
			mr->imr_interface.s_addr = random_ipv4_address();
			so->optlen = sizeof(struct ip_mreq);
		}
		break;

	case IP_MSFILTER:
		//FIXME: Read size from sysctl /proc/sys/net/core/optmem_max
		so->optlen = rnd() % sizeof(unsigned long)*(2*UIO_MAXIOV+512);
		so->optlen |= IP_MSFILTER_SIZE(0);
		break;

	case IP_BLOCK_SOURCE:
	case IP_UNBLOCK_SOURCE:
	case IP_ADD_SOURCE_MEMBERSHIP:
	case IP_DROP_SOURCE_MEMBERSHIP:
		mcaddr = 0xe0000000 | rnd() % 0xff;

		ms = (struct ip_mreq_source *) so->optval;
		ms->imr_multiaddr.s_addr = mcaddr;
		ms->imr_interface.s_addr = random_ipv4_address();
		ms->imr_sourceaddr.s_addr = random_ipv4_address();
		break;

	case MCAST_MSFILTER:
		//FIXME: Read size from sysctl /proc/sys/net/core/optmem_max
		so->optlen = rnd() % sizeof(unsigned long)*(2*UIO_MAXIOV+512);
		so->optlen |= GROUP_FILTER_SIZE(0);
		break;

	default:
		break;
	}
}


struct ip_sso_funcptr {
	unsigned int sol;
	void (*func)(struct sockopt *so, struct socket_triplet *triplet);
};

#define SOL_TCP 6
#define SOL_SCTP 132
#define SOL_UDPLITE 136
#define SOL_DCCP 269

static const struct ip_sso_funcptr ip_ssoptrs[IPPROTO_MAX] = {
	[IPPROTO_IP] = { .func = &ip_setsockopt },
	[IPPROTO_ICMP] = { .func = NULL },
	[IPPROTO_IGMP] = { .func = NULL },
	[IPPROTO_IPIP] = { .func = NULL },
	[IPPROTO_TCP] = { .sol = SOL_TCP, .func = &tcp_setsockopt },
	[IPPROTO_EGP] = { .func = NULL },
	[IPPROTO_PUP] = { .func = NULL },
	[IPPROTO_UDP] = { .sol = SOL_UDP, .func = &udp_setsockopt },
	[IPPROTO_IDP] = { .func = NULL },
	[IPPROTO_TP] = { .func = NULL },
	[IPPROTO_DCCP] = { .sol = SOL_DCCP, .func = &dccp_setsockopt },
#ifdef USE_IPV6
	[IPPROTO_IPV6] = { .sol = SOL_ICMPV6, .func = &icmpv6_setsockopt },
#endif
	[IPPROTO_RSVP] = { .func = NULL },
	[IPPROTO_GRE] = { .func = NULL },
	[IPPROTO_ESP] = { .func = NULL },
	[IPPROTO_AH] = { .func = NULL },
	[IPPROTO_MTP] = { .func = NULL },
	[IPPROTO_BEETPH] = { .func = NULL },
	[IPPROTO_ENCAP] = { .func = NULL },
	[IPPROTO_PIM] = { .func = NULL },
	[IPPROTO_COMP] = { .func = NULL },
	[IPPROTO_SCTP] = { .sol = SOL_SCTP, .func = &sctp_setsockopt },
	[IPPROTO_UDPLITE] = { .sol = SOL_UDPLITE, .func = &udplite_setsockopt },
	[IPPROTO_RAW] = { .sol = SOL_RAW, .func = &raw_setsockopt },
	[IPPROTO_MPLS] = { .func = NULL },
};

static void call_inet_sso_ptr(struct sockopt *so, struct socket_triplet *triplet)
{
	int proto = triplet->protocol;

	/* we might have gotten here from a non-IP socket, (see setsockopt.c
	 * Make sure we don't run past the end of the array above
	 * Don't adjust the actual triplet though, because it's what the real socket is.
	 */
	if (proto > IPPROTO_MAX)
		proto = rnd() % IPPROTO_MAX;

	if (ip_ssoptrs[proto].func != NULL) {
		if (ip_ssoptrs[proto].sol != 0)
			so->level = ip_ssoptrs[proto].sol;
		ip_ssoptrs[proto].func(so, triplet);
		return;
	}

	// unimplemented yet, or no sso for this proto.
	ip_setsockopt(so, triplet);
}

static void inet_setsockopt(struct sockopt *so, struct socket_triplet *triplet)
{
	so->level = SOL_IP;

	if (RAND_BOOL())
		ip_setsockopt(so, triplet);
	else
		call_inet_sso_ptr(so, triplet);
}

static struct socket_triplet ipv4_triplets[] = {
	{ .family = PF_INET, .protocol = IPPROTO_IP, .type = SOCK_DGRAM },
	{ .family = PF_INET, .protocol = IPPROTO_IP, .type = SOCK_SEQPACKET },
	{ .family = PF_INET, .protocol = IPPROTO_IP, .type = SOCK_STREAM },

	{ .family = PF_INET, .protocol = IPPROTO_TCP, .type = SOCK_STREAM },

	{ .family = PF_INET, .protocol = IPPROTO_UDP, .type = SOCK_DGRAM },

	{ .family = PF_INET, .protocol = IPPROTO_DCCP, .type = SOCK_DCCP },

	{ .family = PF_INET, .protocol = IPPROTO_SCTP, .type = SOCK_SEQPACKET },
	{ .family = PF_INET, .protocol = IPPROTO_SCTP, .type = SOCK_STREAM },

	{ .family = PF_INET, .protocol = IPPROTO_UDPLITE, .type = SOCK_DGRAM },
};

	// NNGH UGH HUAGLAHGLAH
/*
static void generate_sockets(void)
{
	if (orig_uid == 0) {
		unsigned int i;

		generate_socket(PF_INET, IPPROTO_ICMP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_IGMP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_IPIP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_TCP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_EGP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_PUP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_UDP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_IDP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_TP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_DCCP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_IPV6, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_RSVP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_GRE, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_ESP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_AH, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_MTP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_BEETPH, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_ENCAP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_PIM, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_COMP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_SCTP, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_UDPLITE, SOCK_PACKET);
		generate_socket(PF_INET, IPPROTO_MPLS, SOCK_PACKET);

		for (i = 0; i < 256; i++)
			generate_socket(PF_INET, i, SOCK_RAW);
	}
}
*/
const struct netproto proto_ipv4 = {
	.name = "ipv4",
	.socket = inet_rand_socket,
	.setsockopt = inet_setsockopt,
	.gen_sockaddr = ipv4_gen_sockaddr,
	.valid_triplets = ipv4_triplets,
	.nr_triplets = ARRAY_SIZE(ipv4_triplets),
};
