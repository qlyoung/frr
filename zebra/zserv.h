/* Zebra daemon server header.
 * Copyright (C) 1997, 98 Kunihiro Ishiguro
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef _ZEBRA_ZSERV_H
#define _ZEBRA_ZSERV_H

#include "rib.h"
#include "if.h"
#include "workqueue.h"
#include "vrf.h"
#include "routemap.h"
#include "vty.h"
#include "zclient.h"
#include "pbr.h"

#include "zebra/zebra_ns.h"
#include "zebra/zebra_pw.h"
//#include "zebra/zebra_pbr.h"

/* Default port information. */
#define ZEBRA_VTY_PORT                2601

/* Default configuration filename. */
#define DEFAULT_CONFIG_FILE "zebra.conf"

#define ZEBRA_RMAP_DEFAULT_UPDATE_TIMER 5 /* disabled by default */

/* Client structure. */
struct zserv {
	/* Client pthread */
	struct frr_pthread *pthread;

	/* Client file descriptor. */
	int sock;

	/* Input/output buffer to the client. */
	pthread_mutex_t ibuf_mtx;
	struct stream_fifo *ibuf_fifo;
	pthread_mutex_t obuf_mtx;
	struct stream_fifo *obuf_fifo;

	/* Private I/O buffers */
	struct stream *ibuf_work;
	struct stream *obuf_work;

	/* Buffer of data waiting to be written to client. */
	struct buffer *wb;

	/* Threads for read/write. */
	struct thread *t_read;
	struct thread *t_write;

	/* default routing table this client munges */
	int rtm_table;

	/* This client's redistribute flag. */
	struct redist_proto mi_redist[AFI_MAX][ZEBRA_ROUTE_MAX];
	vrf_bitmap_t redist[AFI_MAX][ZEBRA_ROUTE_MAX];

	/* Redistribute default route flag. */
	vrf_bitmap_t redist_default;

	/* Interface information. */
	vrf_bitmap_t ifinfo;

	/* Router-id information. */
	vrf_bitmap_t ridinfo;

	bool notify_owner;

	/* client's protocol */
	uint8_t proto;
	unsigned short instance;
	uint8_t is_synchronous;

	/* Statistics */
	uint32_t redist_v4_add_cnt;
	uint32_t redist_v4_del_cnt;
	uint32_t redist_v6_add_cnt;
	uint32_t redist_v6_del_cnt;
	uint32_t v4_route_add_cnt;
	uint32_t v4_route_upd8_cnt;
	uint32_t v4_route_del_cnt;
	uint32_t v6_route_add_cnt;
	uint32_t v6_route_del_cnt;
	uint32_t v6_route_upd8_cnt;
	uint32_t connected_rt_add_cnt;
	uint32_t connected_rt_del_cnt;
	uint32_t ifup_cnt;
	uint32_t ifdown_cnt;
	uint32_t ifadd_cnt;
	uint32_t ifdel_cnt;
	uint32_t if_bfd_cnt;
	uint32_t bfd_peer_add_cnt;
	uint32_t bfd_peer_upd8_cnt;
	uint32_t bfd_peer_del_cnt;
	uint32_t bfd_peer_replay_cnt;
	uint32_t vrfadd_cnt;
	uint32_t vrfdel_cnt;
	uint32_t if_vrfchg_cnt;
	uint32_t bfd_client_reg_cnt;
	uint32_t vniadd_cnt;
	uint32_t vnidel_cnt;
	uint32_t l3vniadd_cnt;
	uint32_t l3vnidel_cnt;
	uint32_t macipadd_cnt;
	uint32_t macipdel_cnt;
	uint32_t prefixadd_cnt;
	uint32_t prefixdel_cnt;

	time_t connect_time;
	time_t last_read_time;
	time_t last_write_time;
	time_t nh_reg_time;
	time_t nh_dereg_time;
	time_t nh_last_upd_time;

	int last_read_cmd;
	int last_write_cmd;
};

#define ZAPI_HANDLER_ARGS                                                      \
	struct zserv *client, struct zmsghdr *hdr, struct stream *msg,         \
		struct zebra_vrf *zvrf

/* Zebra instance */
struct zebra_t {
	/* Thread master */
	struct thread_master *master;
	struct list *client_list;

	/* default table */
	uint32_t rtm_table_default;

/* rib work queue */
#define ZEBRA_RIB_PROCESS_HOLD_TIME 10
	struct work_queue *ribq;
	struct meta_queue *mq;

	/* LSP work queue */
	struct work_queue *lsp_process_q;

#define ZEBRA_ZAPI_PACKETS_TO_PROCESS 10
	uint32_t packets_to_process;
};
extern struct zebra_t zebrad;
extern unsigned int multipath_num;

/* Prototypes. */
extern void zserv_init(void);
extern void zebra_zserv_socket_init(char *path);

extern int zsend_vrf_add(struct zserv *, struct zebra_vrf *);
extern int zsend_vrf_delete(struct zserv *, struct zebra_vrf *);

extern int zsend_interface_add(struct zserv *, struct interface *);
extern int zsend_interface_delete(struct zserv *, struct interface *);
extern int zsend_interface_addresses(struct zserv *, struct interface *);
extern int zsend_interface_address(int, struct zserv *, struct interface *,
				   struct connected *);
extern void nbr_connected_add_ipv6(struct interface *, struct in6_addr *);
extern void nbr_connected_delete_ipv6(struct interface *, struct in6_addr *);
extern int zsend_interface_update(int, struct zserv *, struct interface *);
extern int zsend_redistribute_route(int, struct zserv *, struct prefix *,
				    struct prefix *, struct route_entry *);
extern int zsend_router_id_update(struct zserv *, struct prefix *, vrf_id_t);
extern int zsend_interface_vrf_update(struct zserv *, struct interface *,
				      vrf_id_t);

extern int zsend_interface_link_params(struct zserv *, struct interface *);
extern int zsend_pw_update(struct zserv *, struct zebra_pw *);

extern int zsend_route_notify_owner(struct route_entry *re, struct prefix *p,
				    enum zapi_route_notify_owner note);

struct zebra_pbr_ipset;
struct zebra_pbr_ipset_entry;
struct zebra_pbr_iptable;
struct zebra_pbr_rule;
extern void zsend_rule_notify_owner(struct zebra_pbr_rule *rule,
				    enum zapi_rule_notify_owner note);
extern void zsend_ipset_notify_owner(
			struct zebra_pbr_ipset *ipset,
			enum zapi_ipset_notify_owner note);
extern void zsend_ipset_entry_notify_owner(
			struct zebra_pbr_ipset_entry *ipset,
			enum zapi_ipset_entry_notify_owner note);
extern void zsend_iptable_notify_owner(
			struct zebra_pbr_iptable *iptable,
			enum zapi_iptable_notify_owner note);

extern void zserv_nexthop_num_warn(const char *, const struct prefix *,
				   const unsigned int);
extern int zebra_server_send_message(struct zserv *client, struct stream *msg);

extern struct zserv *zebra_find_client(uint8_t proto, unsigned short instance);

#if defined(HANDLE_ZAPI_FUZZING)
extern void zserv_read_file(char *input);
#endif

#endif /* _ZEBRA_ZEBRA_H */
