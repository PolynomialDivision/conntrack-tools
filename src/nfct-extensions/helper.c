/*
 * (C) 2012 by Pablo Neira Ayuso <pablo@netfilter.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This code has been sponsored by Vyatta Inc. <http://www.vyatta.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <netinet/in.h>
#include <errno.h>
#include <dlfcn.h>

#include <libmnl/libmnl.h>
#include <linux/netfilter/nfnetlink_cthelper.h>
#include <libnetfilter_cthelper/libnetfilter_cthelper.h>

#include "nfct.h"
#include "helper.h"

static void
nfct_cmd_helper_usage(char *argv[])
{
	fprintf(stderr, "nfct v%s: Missing command\n"
			"%s helper list|add|delete|disable|get|flush "
			"[parameters...]\n", VERSION, argv[0]);
}

static int nfct_cmd_helper_list(struct mnl_socket *nl, int argc, char *argv[]);
static int nfct_cmd_helper_add(struct mnl_socket *nl, int argc, char *argv[]);
static int nfct_cmd_helper_delete(struct mnl_socket *nl, int argc, char *argv[]);
static int nfct_cmd_helper_get(struct mnl_socket *nl, int argc, char *argv[]);
static int nfct_cmd_helper_flush(struct mnl_socket *nl, int argc, char *argv[]);
static int nfct_cmd_helper_disable(struct mnl_socket *nl, int argc, char *argv[]);

static int
nfct_helper_parse_params(struct mnl_socket *nl, int argc, char *argv[], int cmd)
{
	int ret;

	if (argc < 3) {
		nfct_cmd_helper_usage(argv);
		return -1;
	}

	switch (cmd) {
	case NFCT_CMD_LIST:
	case NFCT_CMD_ADD:
	case NFCT_CMD_DELETE:
	case NFCT_CMD_GET:
	case NFCT_CMD_FLUSH:
	case NFCT_CMD_DISABLE:
		break;
	default:
		fprintf(stderr, "nfct v%s: Unknown command: %s\n",
			VERSION, argv[2]);
		nfct_cmd_helper_usage(argv);
		exit(EXIT_FAILURE);
	}

	switch (cmd) {
	case NFCT_CMD_LIST:
		ret = nfct_cmd_helper_list(nl, argc, argv);
		break;
	case NFCT_CMD_ADD:
		ret = nfct_cmd_helper_add(nl, argc, argv);
		break;
	case NFCT_CMD_DELETE:
		ret = nfct_cmd_helper_delete(nl, argc, argv);
		break;
	case NFCT_CMD_GET:
		ret = nfct_cmd_helper_get(nl, argc, argv);
		break;
	case NFCT_CMD_FLUSH:
		ret = nfct_cmd_helper_flush(nl, argc, argv);
		break;
	case NFCT_CMD_DISABLE:
		ret = nfct_cmd_helper_disable(nl, argc, argv);
		break;
	default:
		nfct_cmd_helper_usage(argv);
		return -1;
	}

	return ret;
}

static int nfct_helper_cb(const struct nlmsghdr *nlh, void *data)
{
	struct nfct_helper *t;
	char buf[4096];

	t = nfct_helper_alloc();
	if (t == NULL) {
		nfct_perror("OOM");
		goto err;
	}

	if (nfct_helper_nlmsg_parse_payload(nlh, t) < 0) {
		nfct_perror("nfct_helper_nlmsg_parse_payload");
		goto err_free;
	}

	nfct_helper_snprintf(buf, sizeof(buf), t, 0, 0);
	printf("%s\n", buf);

err_free:
	nfct_helper_free(t);
err:
	return MNL_CB_OK;
}

static int nfct_cmd_helper_list(struct mnl_socket *nl, int argc, char *argv[])
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	unsigned int seq, portid;

	if (argc > 3) {
		nfct_perror("too many arguments");
		return -1;
	}

	seq = time(NULL);
	nlh = nfct_helper_nlmsg_build_hdr(buf, NFNL_MSG_CTHELPER_GET,
						NLM_F_DUMP, seq);

	portid = mnl_socket_get_portid(nl);
	if (nfct_mnl_talk(nl, nlh, seq, portid, nfct_helper_cb, NULL) < 0) {
		nfct_perror("netlink error");
		return -1;
	}

	return 0;
}

static int nfct_cmd_helper_add(struct mnl_socket *nl, int argc, char *argv[])
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	uint32_t portid, seq;
	struct nfct_helper *t;
	uint16_t l3proto;
	uint8_t l4proto;
	struct ctd_helper *helper;
	int j;

	if (argc < 6) {
		nfct_perror("missing parameters\n"
			    "syntax: nfct add helper name family protocol");
		return -1;
	}

	if (strcmp(argv[4], "inet") == 0)
		l3proto = AF_INET;
	else if (strcmp(argv[4], "inet6") == 0)
		l3proto = AF_INET6;
	else {
		nfct_perror("unknown layer 3 protocol");
		return -1;
	}

	if (strcmp(argv[5], "tcp") == 0)
		l4proto = IPPROTO_TCP;
	else if (strcmp(argv[5], "udp") == 0)
		l4proto = IPPROTO_UDP;
	else {
		nfct_perror("unsupported layer 4 protocol");
		return -1;
	}

	helper = __helper_find(argv[3], l4proto);
	if (helper == NULL) {
		nfct_perror("that helper is not supported");
		return -1;
	}

	t = nfct_helper_alloc();
	if (t == NULL) {
		nfct_perror("OOM");
		return -1;
	}
	nfct_helper_attr_set(t, NFCTH_ATTR_NAME, argv[3]);
	nfct_helper_attr_set_u16(t, NFCTH_ATTR_PROTO_L3NUM, l3proto);
	nfct_helper_attr_set_u8(t, NFCTH_ATTR_PROTO_L4NUM, l4proto);
	nfct_helper_attr_set_u32(t, NFCTH_ATTR_PRIV_DATA_LEN,
				 helper->priv_data_len);

	for (j=0; j<CTD_HELPER_POLICY_MAX; j++) {
		struct nfct_helper_policy *p;

		if (!helper->policy[j].name[0])
			break;

		p = nfct_helper_policy_alloc();
		if (p == NULL) {
			nfct_perror("OOM");
			return -1;
		}

		nfct_helper_policy_attr_set(p, NFCTH_ATTR_POLICY_NAME,
					helper->policy[j].name);
		nfct_helper_policy_attr_set_u32(p, NFCTH_ATTR_POLICY_TIMEOUT,
					helper->policy[j].expect_timeout);
		nfct_helper_policy_attr_set_u32(p, NFCTH_ATTR_POLICY_MAX,
					helper->policy[j].expect_max);

		nfct_helper_attr_set(t, NFCTH_ATTR_POLICY+j, p);
	}

	seq = time(NULL);
	nlh = nfct_helper_nlmsg_build_hdr(buf, NFNL_MSG_CTHELPER_NEW,
					  NLM_F_CREATE | NLM_F_ACK, seq);
	nfct_helper_nlmsg_build_payload(nlh, t);

	nfct_helper_free(t);

	portid = mnl_socket_get_portid(nl);
	if (nfct_mnl_talk(nl, nlh, seq, portid, NULL, NULL) < 0) {
		nfct_perror("netlink error");
		return -1;
	}

	return 0;
}

static int
nfct_cmd_helper_delete(struct mnl_socket *nl, int argc, char *argv[])
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	uint32_t portid, seq;
	struct nfct_helper *t;

	if (argc < 4) {
		nfct_perror("missing helper policy name");
		return -1;
	} else if (argc > 6) {
		nfct_perror("too many arguments");
		return -1;
	}

	t = nfct_helper_alloc();
	if (t == NULL) {
		nfct_perror("OOM");
		return -1;
	}

	nfct_helper_attr_set(t, NFCTH_ATTR_NAME, argv[3]);

	if (argc >= 5) {
		uint16_t l3proto;

		if (strcmp(argv[4], "inet") == 0)
			l3proto = AF_INET;
		else if (strcmp(argv[4], "inet6") == 0)
			l3proto = AF_INET6;
		else {
			nfct_perror("unknown layer 3 protocol");
			return -1;
		}
		nfct_helper_attr_set_u16(t, NFCTH_ATTR_PROTO_L3NUM, l3proto);
	}

	if (argc == 6) {
		uint8_t l4proto;

		if (strcmp(argv[5], "tcp") == 0)
			l4proto = IPPROTO_TCP;
		else if (strcmp(argv[5], "udp") == 0)
			l4proto = IPPROTO_UDP;
		else {
			nfct_perror("unsupported layer 4 protocol");
			return -1;
		}
		nfct_helper_attr_set_u8(t, NFCTH_ATTR_PROTO_L4NUM, l4proto);
	}

	seq = time(NULL);
	nlh = nfct_helper_nlmsg_build_hdr(buf, NFNL_MSG_CTHELPER_DEL,
					  NLM_F_ACK, seq);
	nfct_helper_nlmsg_build_payload(nlh, t);

	nfct_helper_free(t);

	portid = mnl_socket_get_portid(nl);
	if (nfct_mnl_talk(nl, nlh, seq, portid, NULL, NULL) < 0) {
		nfct_perror("netlink error");
		return -1;
	}

	return 0;
}

static int nfct_cmd_helper_get(struct mnl_socket *nl, int argc, char *argv[])
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	uint32_t portid, seq;
	struct nfct_helper *t;

	if (argc < 4) {
		nfct_perror("missing helper policy name");
		return -1;
	} else if (argc > 6) {
		nfct_perror("too many arguments");
		return -1;
	}

	t = nfct_helper_alloc();
	if (t == NULL) {
		nfct_perror("OOM");
		return -1;
	}
	nfct_helper_attr_set(t, NFCTH_ATTR_NAME, argv[3]);

	if (argc >= 5) {
		uint16_t l3proto;

		if (strcmp(argv[4], "inet") == 0)
			l3proto = AF_INET;
		else if (strcmp(argv[4], "inet6") == 0)
			l3proto = AF_INET6;
		else {
			nfct_perror("unknown layer 3 protocol");
			return -1;
		}
		nfct_helper_attr_set_u16(t, NFCTH_ATTR_PROTO_L3NUM, l3proto);
	}

	if (argc == 6) {
		uint8_t l4proto;

		if (strcmp(argv[5], "tcp") == 0)
			l4proto = IPPROTO_TCP;
		else if (strcmp(argv[5], "udp") == 0)
			l4proto = IPPROTO_UDP;
		else {
			nfct_perror("unsupported layer 4 protocol");
			return -1;
		}
		nfct_helper_attr_set_u32(t, NFCTH_ATTR_PROTO_L4NUM, l4proto);
	}

	seq = time(NULL);
	nlh = nfct_helper_nlmsg_build_hdr(buf, NFNL_MSG_CTHELPER_GET,
					  NLM_F_ACK, seq);

	nfct_helper_nlmsg_build_payload(nlh, t);

	nfct_helper_free(t);

	portid = mnl_socket_get_portid(nl);
	if (nfct_mnl_talk(nl, nlh, seq, portid, nfct_helper_cb, NULL) < 0) {
		nfct_perror("netlink error");
		return -1;
	}

	return 0;
}

static int
nfct_cmd_helper_flush(struct mnl_socket *nl, int argc, char *argv[])
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	uint32_t portid, seq;

	if (argc > 3) {
		nfct_perror("too many arguments");
		return -1;
	}

	seq = time(NULL);
	nlh = nfct_helper_nlmsg_build_hdr(buf, NFNL_MSG_CTHELPER_DEL,
					   NLM_F_ACK, seq);

	portid = mnl_socket_get_portid(nl);
	if (nfct_mnl_talk(nl, nlh, seq, portid, NULL, NULL) < 0) {
		nfct_perror("netlink error");
		return -1;
	}

	return 0;
}

static int
nfct_cmd_helper_disable(struct mnl_socket *nl, int argc, char *argv[])
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	uint32_t portid, seq;
	struct nfct_helper *t;
	uint16_t l3proto;
	uint8_t l4proto;
	struct ctd_helper *helper;

	if (argc < 6) {
		nfct_perror("missing parameters\n"
			    "syntax: nfct disable helper name family protocol");
		return -1;
	}

	if (strcmp(argv[4], "inet") == 0)
		l3proto = AF_INET;
	else if (strcmp(argv[4], "inet6") == 0)
		l3proto = AF_INET6;
	else {
		nfct_perror("unknown layer 3 protocol");
		return -1;
	}

	if (strcmp(argv[5], "tcp") == 0)
		l4proto = IPPROTO_TCP;
	else if (strcmp(argv[5], "udp") == 0)
		l4proto = IPPROTO_UDP;
	else {
		nfct_perror("unsupported layer 4 protocol");
		return -1;
	}

	helper = __helper_find(argv[3], l4proto);
	if (helper == NULL) {
		nfct_perror("that helper is not supported");
		return -1;
	}

	t = nfct_helper_alloc();
	if (t == NULL) {
		nfct_perror("OOM");
		return -1;
	}
	nfct_helper_attr_set(t, NFCTH_ATTR_NAME, argv[3]);
	nfct_helper_attr_set_u16(t, NFCTH_ATTR_PROTO_L3NUM, l3proto);
	nfct_helper_attr_set_u8(t, NFCTH_ATTR_PROTO_L4NUM, l4proto);
	nfct_helper_attr_set_u32(t, NFCTH_ATTR_STATUS,
					NFCT_HELPER_STATUS_DISABLED);

	seq = time(NULL);
	nlh = nfct_helper_nlmsg_build_hdr(buf, NFNL_MSG_CTHELPER_NEW,
					  NLM_F_CREATE | NLM_F_ACK, seq);
	nfct_helper_nlmsg_build_payload(nlh, t);

	nfct_helper_free(t);

	portid = mnl_socket_get_portid(nl);
	if (nfct_mnl_talk(nl, nlh, seq, portid, NULL, NULL) < 0) {
		nfct_perror("netlink error");
		return -1;
	}

	return 0;
}

static struct nfct_extension helper = {
	.type		= NFCT_SUBSYS_HELPER,
	.parse_params	= nfct_helper_parse_params,
};

/*
 * supported helpers: to set up helpers via nfct, the following definitions are
 * provided for backward compatibility reasons since conntrackd does not depend
 * on nfct anymore to set up the userspace helpers.
 */

static struct ctd_helper amanda_helper = {
	.name		= "amanda",
	.l4proto	= IPPROTO_UDP,
	.policy		= {
		[0] = {
			.name			= "amanda",
			.expect_max		= 3,
			.expect_timeout		= 180,
		},
	},
};

static struct ctd_helper dhcpv6_helper = {
	.name		= "dhcpv6",
	.l4proto	= IPPROTO_UDP,
	.policy		= {
		[0] = {
			.name			= "dhcpv6",
			.expect_max		= 1,
			.expect_timeout		= 300,
		},
	},
};

#include "helpers/ftp.h"

static struct ctd_helper ftp_helper = {
	.name		= "ftp",
	.l4proto	= IPPROTO_TCP,
	.priv_data_len	= sizeof(struct ftp_info),
	.policy		= {
		[0] = {
			.name			= "ftp",
			.expect_max		= 1,
			.expect_timeout		= 300,
		},
	},
};

static struct ctd_helper mdns_helper = {
	.name		= "mdns",
	.l4proto	= IPPROTO_UDP,
	.priv_data_len	= 0,
	.policy		= {
		[0] = {
			.name		= "mdns",
			.expect_max	= 8,
			.expect_timeout = 30,
		},
	},
};

#include "helpers/rpc.h"

static struct ctd_helper rpc_helper_tcp = {
	.name		= "rpc",
	.l4proto	= IPPROTO_TCP,
	.priv_data_len	= sizeof(struct rpc_info),
	.policy		= {
		{
			.name			= "rpc",
			.expect_max		= 1,
			.expect_timeout		= 300,
		},
	},
};

static struct ctd_helper rpc_helper_udp = {
	.name		= "rpc",
	.l4proto	= IPPROTO_UDP,
	.priv_data_len	= sizeof(struct rpc_info),
	.policy		= {
		{
			.name			= "rpc",
			.expect_max		= 1,
			.expect_timeout		= 300,
		},
	},
};

#include "helpers/sane.h"

static struct ctd_helper sane_helper = {
	.name		= "sane",
	.l4proto	= IPPROTO_TCP,
	.priv_data_len	= sizeof(struct nf_ct_sane_master),
	.policy		= {
		[0] = {
			.name			= "sane",
			.expect_max		= 1,
			.expect_timeout		= 5 * 60,
		},
	},
};

static struct ctd_helper slp_helper = {
	.name		= "slp",
	.l4proto	= IPPROTO_UDP,
	.priv_data_len	= 0,
	.policy		= {
		[0] = {
			.name		= "slp",
			.expect_max	= 8,
			.expect_timeout = 16, /* default CONFIG_MC_MAX + 1 */
		},
	},
};

static struct ctd_helper ssdp_helper_udp = {
	.name		= "ssdp",
	.l4proto	= IPPROTO_UDP,
	.priv_data_len	= 0,
	.policy		= {
		[0] = {
			.name		= "ssdp",
			.expect_max	= 8,
			.expect_timeout = 5 * 60,
		},
	},
};

static struct ctd_helper ssdp_helper_tcp = {
	.name		= "ssdp",
	.l4proto	= IPPROTO_TCP,
	.priv_data_len	= 0,
	.policy		= {
		[0] = {
			.name		= "ssdp",
			.expect_max	= 8,
			.expect_timeout = 5 * 60,
		},
	},
};

static struct ctd_helper tftp_helper = {
	.name		= "tftp",
	.l4proto	= IPPROTO_UDP,
	.policy		= {
		[0] = {
			.name			= "tftp",
			.expect_max		= 1,
			.expect_timeout		= 5 * 60,
		},
	},
};

#include "helpers/tns.h"

static struct ctd_helper tns_helper = {
	.name		= "tns",
	.l4proto	= IPPROTO_TCP,
	.priv_data_len	= sizeof(struct tns_info),
	.policy		= {
		[0] = {
			.name		= "tns",
			.expect_max	= 1,
			.expect_timeout = 300,
		},
	},
};

static void __init helper_init(void)
{
	helper_register(&amanda_helper);
	helper_register(&dhcpv6_helper);
	helper_register(&ftp_helper);
	helper_register(&mdns_helper);
	helper_register(&rpc_helper_tcp);
	helper_register(&rpc_helper_udp);
	helper_register(&sane_helper);
	helper_register(&slp_helper);
	helper_register(&ssdp_helper_udp);
	helper_register(&ssdp_helper_tcp);
	helper_register(&tftp_helper);
	helper_register(&tns_helper);

	nfct_extension_register(&helper);
}
