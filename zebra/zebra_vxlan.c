/*
 * Zebra EVPN for VxLAN code
 * Copyright (C) 2016, 2017 Cumulus Networks, Inc.
 *
 * This file is part of FRR.
 *
 * FRR is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * FRR is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FRR; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <zebra.h>

#include "hash.h"
#include "if.h"
#include "jhash.h"
#include "linklist.h"
#include "log.h"
#include "memory.h"
#include "prefix.h"
#include "stream.h"
#include "table.h"
#include "vlan.h"
#include "vxlan.h"
#ifdef GNU_LINUX
#include <linux/neighbour.h>
#endif

#include "zebra/zebra_router.h"
#include "zebra/debug.h"
#include "zebra/interface.h"
#include "zebra/rib.h"
#include "zebra/rt.h"
#include "zebra/rt_netlink.h"
#include "zebra/zebra_errors.h"
#include "zebra/zebra_l2.h"
#include "zebra/zebra_memory.h"
#include "zebra/zebra_ns.h"
#include "zebra/zebra_vrf.h"
#include "zebra/zebra_vxlan.h"
#include "zebra/zebra_vxlan_private.h"
#include "zebra/zebra_evpn_mh.h"
#include "zebra/zebra_router.h"

DEFINE_MTYPE_STATIC(ZEBRA, HOST_PREFIX, "host prefix");
DEFINE_MTYPE_STATIC(ZEBRA, ZVNI, "VNI hash");
DEFINE_MTYPE_STATIC(ZEBRA, ZL3VNI, "L3 VNI hash");
DEFINE_MTYPE_STATIC(ZEBRA, ZVNI_VTEP, "VNI remote VTEP");
DEFINE_MTYPE_STATIC(ZEBRA, MAC, "VNI MAC");
DEFINE_MTYPE_STATIC(ZEBRA, NEIGH, "VNI Neighbor");
DEFINE_MTYPE_STATIC(ZEBRA, ZVXLAN_SG, "zebra VxLAN multicast group");

DEFINE_HOOK(zebra_rmac_update, (zebra_mac_t *rmac, zebra_l3vni_t *zl3vni,
	    bool delete, const char *reason), (rmac, zl3vni, delete, reason))

/* definitions */
/* PMSI strings. */
#define VXLAN_FLOOD_STR_NO_INFO "-"
#define VXLAN_FLOOD_STR_DEFAULT VXLAN_FLOOD_STR_NO_INFO
static const struct message zvtep_flood_str[] = {
	{VXLAN_FLOOD_DISABLED, VXLAN_FLOOD_STR_NO_INFO},
	{VXLAN_FLOOD_PIM_SM, "PIM-SM"},
	{VXLAN_FLOOD_HEAD_END_REPL, "HER"},
	{0}
};

/* static function declarations */
static int ip_prefix_send_to_client(vrf_id_t vrf_id, struct prefix *p,
				    uint16_t cmd);
static void zvni_print_neigh(zebra_neigh_t *n, void *ctxt, json_object *json);
static void zvni_print_neigh_hash(struct hash_bucket *bucket, void *ctxt);
static void zvni_print_dad_neigh_hash(struct hash_bucket *bucket, void *ctxt);
static void zvni_print_neigh_hash_all_vni(struct hash_bucket *bucket,
					  void **args);
static void zl3vni_print_nh(zebra_neigh_t *n, struct vty *vty,
			    json_object *json);
static void zl3vni_print_rmac(zebra_mac_t *zrmac, struct vty *vty,
			      json_object *json);
static void zvni_print_mac(zebra_mac_t *mac, void *ctxt, json_object *json);
static void zvni_print_mac_hash(struct hash_bucket *bucket, void *ctxt);
static void zvni_print_mac_hash_all_vni(struct hash_bucket *bucket, void *ctxt);
static void zvni_print(zebra_vni_t *zvni, void **ctxt);
static void zvni_print_hash(struct hash_bucket *bucket, void *ctxt[]);

static int zvni_macip_send_msg_to_client(vni_t vni, struct ethaddr *macaddr,
					 struct ipaddr *ip, uint8_t flags,
					 uint32_t seq, int state,
					 struct zebra_evpn_es *es,
					 uint16_t cmd);
static unsigned int neigh_hash_keymake(const void *p);
static void *zvni_neigh_alloc(void *p);
static zebra_neigh_t *zvni_neigh_add(zebra_vni_t *zvni, struct ipaddr *ip,
				     struct ethaddr *mac, zebra_mac_t *zmac,
					 uint32_t n_flags);
static int zvni_neigh_del(zebra_vni_t *zvni, zebra_neigh_t *n);
static void zvni_neigh_del_all(zebra_vni_t *zvni, int uninstall, int upd_client,
			       uint32_t flags);
static zebra_neigh_t *zvni_neigh_lookup(zebra_vni_t *zvni, struct ipaddr *ip);
static int zvni_neigh_send_add_to_client(vni_t vni, struct ipaddr *ip,
					 struct ethaddr *mac, zebra_mac_t *zmac,
					 uint32_t flags, uint32_t seq);
static int zvni_neigh_send_del_to_client(vni_t vni, struct ipaddr *ip,
					 struct ethaddr *mac,
					 uint32_t flags, int state, bool force);
static int zvni_rem_neigh_install(zebra_vni_t *zvni,
		zebra_neigh_t *n, bool was_static);
static int zvni_neigh_uninstall(zebra_vni_t *zvni, zebra_neigh_t *n);
static int zvni_neigh_probe(zebra_vni_t *zvni, zebra_neigh_t *n);
static zebra_vni_t *zvni_from_svi(struct interface *ifp,
				  struct interface *br_if);
static struct interface *zvni_map_to_svi(vlanid_t vid, struct interface *br_if);
static struct interface *zvni_map_to_macvlan(struct interface *br_if,
					     struct interface *svi_if);

/* l3-vni next-hop neigh related APIs */
static zebra_neigh_t *zl3vni_nh_lookup(zebra_l3vni_t *zl3vni,
				       const struct ipaddr *ip);
static void *zl3vni_nh_alloc(void *p);
static zebra_neigh_t *zl3vni_nh_add(zebra_l3vni_t *zl3vni,
				    const struct ipaddr *vtep_ip,
				    const struct ethaddr *rmac);
static int zl3vni_nh_del(zebra_l3vni_t *zl3vni, zebra_neigh_t *n);
static int zl3vni_nh_install(zebra_l3vni_t *zl3vni, zebra_neigh_t *n);
static int zl3vni_nh_uninstall(zebra_l3vni_t *zl3vni, zebra_neigh_t *n);

/* l3-vni rmac related APIs */
static void zl3vni_print_rmac_hash(struct hash_bucket *, void *);
static zebra_mac_t *zl3vni_rmac_lookup(zebra_l3vni_t *zl3vni,
				       const struct ethaddr *rmac);
static void *zl3vni_rmac_alloc(void *p);
static zebra_mac_t *zl3vni_rmac_add(zebra_l3vni_t *zl3vni,
				    const struct ethaddr *rmac);
static int zl3vni_rmac_del(zebra_l3vni_t *zl3vni, zebra_mac_t *zrmac);
static int zl3vni_rmac_install(zebra_l3vni_t *zl3vni, zebra_mac_t *zrmac);
static int zl3vni_rmac_uninstall(zebra_l3vni_t *zl3vni, zebra_mac_t *zrmac);

/* l3-vni related APIs*/
static zebra_l3vni_t *zl3vni_lookup(vni_t vni);
static void *zl3vni_alloc(void *p);
static zebra_l3vni_t *zl3vni_add(vni_t vni, vrf_id_t vrf_id);
static int zl3vni_del(zebra_l3vni_t *zl3vni);
static void zebra_vxlan_process_l3vni_oper_up(zebra_l3vni_t *zl3vni);
static void zebra_vxlan_process_l3vni_oper_down(zebra_l3vni_t *zl3vni);

static unsigned int mac_hash_keymake(const void *p);
static bool mac_cmp(const void *p1, const void *p2);
static void *zvni_mac_alloc(void *p);
static zebra_mac_t *zvni_mac_add(zebra_vni_t *zvni, struct ethaddr *macaddr);
static int zvni_mac_del(zebra_vni_t *zvni, zebra_mac_t *mac);
static void zvni_mac_del_all(zebra_vni_t *zvni, int uninstall, int upd_client,
			     uint32_t flags);
static zebra_mac_t *zvni_mac_lookup(zebra_vni_t *zvni, struct ethaddr *macaddr);
static int zvni_mac_send_add_to_client(vni_t vni, struct ethaddr *macaddr,
		uint32_t flags, uint32_t seq, struct zebra_evpn_es *es);
static int zvni_mac_send_del_to_client(vni_t vni, struct ethaddr *macaddr,
		uint32_t flags, bool force);
static zebra_vni_t *zvni_map_vlan(struct interface *ifp,
				  struct interface *br_if, vlanid_t vid);
static int zvni_rem_mac_install(zebra_vni_t *zvni,
		zebra_mac_t *mac, bool was_static);
static int zvni_rem_mac_uninstall(zebra_vni_t *zvni, zebra_mac_t *mac);
static void zvni_install_mac_hash(struct hash_bucket *bucket, void *ctxt);

static unsigned int vni_hash_keymake(const void *p);
static void *zvni_alloc(void *p);
static zebra_vni_t *zvni_add(vni_t vni);
static int zvni_del(zebra_vni_t *zvni);
static int zvni_send_add_to_client(zebra_vni_t *zvni);
static int zvni_send_del_to_client(zebra_vni_t *zvni);
static void zvni_build_hash_table(void);
static int zvni_vtep_match(struct in_addr *vtep_ip, zebra_vtep_t *zvtep);
static zebra_vtep_t *zvni_vtep_find(zebra_vni_t *zvni, struct in_addr *vtep_ip);
static zebra_vtep_t *zvni_vtep_add(zebra_vni_t *zvni, struct in_addr *vtep_ip,
		int flood_control);
static int zvni_vtep_del(zebra_vni_t *zvni, zebra_vtep_t *zvtep);
static int zvni_vtep_del_all(zebra_vni_t *zvni, int uninstall);
static int zvni_vtep_install(zebra_vni_t *zvni, zebra_vtep_t *zvtep);
static int zvni_vtep_uninstall(zebra_vni_t *zvni, struct in_addr *vtep_ip);
static int zvni_del_macip_for_intf(struct interface *ifp, zebra_vni_t *zvni);
static int zvni_add_macip_for_intf(struct interface *ifp, zebra_vni_t *zvni);
static int zvni_gw_macip_add(struct interface *ifp, zebra_vni_t *zvni,
			     struct ethaddr *macaddr, struct ipaddr *ip);
static int zvni_gw_macip_del(struct interface *ifp, zebra_vni_t *zvni,
			     struct ipaddr *ip);
struct interface *zebra_get_vrr_intf_for_svi(struct interface *ifp);
static int advertise_gw_macip_enabled(zebra_vni_t *zvni);
static int advertise_svi_macip_enabled(zebra_vni_t *zvni);
static int zebra_vxlan_ip_inherit_dad_from_mac(struct zebra_vrf *zvrf,
					       zebra_mac_t *old_zmac,
					       zebra_mac_t *new_zmac,
					       zebra_neigh_t *nbr);
static int remote_neigh_count(zebra_mac_t *zmac);
static void zvni_deref_ip2mac(zebra_vni_t *zvni, zebra_mac_t *mac);
static int zebra_vxlan_dad_mac_auto_recovery_exp(struct thread *t);
static int zebra_vxlan_dad_ip_auto_recovery_exp(struct thread *t);
static void zebra_vxlan_dup_addr_detect_for_neigh(struct zebra_vrf *zvrf,
						  zebra_neigh_t *nbr,
						  struct in_addr vtep_ip,
						  bool do_dad,
						  bool *is_dup_detect,
						  bool is_local);
static void zebra_vxlan_dup_addr_detect_for_mac(struct zebra_vrf *zvrf,
						zebra_mac_t *mac,
						struct in_addr vtep_ip,
						bool do_dad,
						bool *is_dup_detect,
						bool is_local);
static unsigned int zebra_vxlan_sg_hash_key_make(const void *p);
static bool zebra_vxlan_sg_hash_eq(const void *p1, const void *p2);
static void zebra_vxlan_sg_do_deref(struct zebra_vrf *zvrf,
		struct in_addr sip, struct in_addr mcast_grp);
static zebra_vxlan_sg_t *zebra_vxlan_sg_do_ref(struct zebra_vrf *vrf,
				struct in_addr sip, struct in_addr mcast_grp);
static void zebra_vxlan_sg_deref(struct in_addr local_vtep_ip,
				struct in_addr mcast_grp);
static void zebra_vxlan_sg_ref(struct in_addr local_vtep_ip,
				struct in_addr mcast_grp);
static void zebra_vxlan_sg_cleanup(struct hash_bucket *bucket, void *arg);

static void zvni_send_mac_to_client(zebra_vni_t *zvn);
static void zvni_send_neigh_to_client(zebra_vni_t *zvni);
static void zebra_vxlan_rem_mac_del(zebra_vni_t *zvni,
		zebra_mac_t *zmac);
static inline void zebra_vxlan_mac_stop_hold_timer(zebra_mac_t *mac);
static inline bool zebra_vxlan_mac_is_static(zebra_mac_t *mac);
static void zebra_vxlan_local_neigh_ref_mac(zebra_neigh_t *n,
		struct ethaddr *macaddr, zebra_mac_t *mac,
		bool send_mac_update);
static void zebra_vxlan_local_neigh_deref_mac(zebra_neigh_t *n,
		bool send_mac_update);
static inline bool zebra_vxlan_neigh_is_ready_for_bgp(zebra_neigh_t *n);
static inline bool zebra_vxlan_neigh_clear_sync_info(zebra_neigh_t *n);
static void zebra_vxlan_sync_neigh_dp_install(zebra_neigh_t *n,
		bool set_inactive, bool force_clear_static, const char *caller);
static inline bool zebra_vxlan_neigh_is_static(zebra_neigh_t *neigh);
static void zebra_vxlan_neigh_send_add_del_to_client(zebra_neigh_t *n,
		bool old_bgp_ready, bool new_bgp_ready);

/* Private functions */
static int host_rb_entry_compare(const struct host_rb_entry *hle1,
				 const struct host_rb_entry *hle2)
{
	if (hle1->p.family < hle2->p.family)
		return -1;

	if (hle1->p.family > hle2->p.family)
		return 1;

	if (hle1->p.prefixlen < hle2->p.prefixlen)
		return -1;

	if (hle1->p.prefixlen > hle2->p.prefixlen)
		return 1;

	if (hle1->p.family == AF_INET) {
		if (hle1->p.u.prefix4.s_addr < hle2->p.u.prefix4.s_addr)
			return -1;

		if (hle1->p.u.prefix4.s_addr > hle2->p.u.prefix4.s_addr)
			return 1;

		return 0;
	} else if (hle1->p.family == AF_INET6) {
		return memcmp(&hle1->p.u.prefix6, &hle2->p.u.prefix6,
			      IPV6_MAX_BYTELEN);
	} else {
		zlog_debug("%s: Unexpected family type: %d", __func__,
			   hle1->p.family);
		return 0;
	}
}
RB_GENERATE(host_rb_tree_entry, host_rb_entry, hl_entry, host_rb_entry_compare);

static uint32_t rb_host_count(struct host_rb_tree_entry *hrbe)
{
	struct host_rb_entry *hle;
	uint32_t count = 0;

	RB_FOREACH (hle, host_rb_tree_entry, hrbe)
		count++;

	return count;
}

/*
 * Return number of valid MACs in a VNI's MAC hash table - all
 * remote MACs and non-internal (auto) local MACs count.
 */
static uint32_t num_valid_macs(zebra_vni_t *zvni)
{
	unsigned int i;
	uint32_t num_macs = 0;
	struct hash *hash;
	struct hash_bucket *hb;
	zebra_mac_t *mac;

	hash = zvni->mac_table;
	if (!hash)
		return num_macs;
	for (i = 0; i < hash->size; i++) {
		for (hb = hash->index[i]; hb; hb = hb->next) {
			mac = (zebra_mac_t *)hb->data;
			if (CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE)
			    || CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL)
			    || !CHECK_FLAG(mac->flags, ZEBRA_MAC_AUTO))
				num_macs++;
		}
	}

	return num_macs;
}

static uint32_t num_dup_detected_macs(zebra_vni_t *zvni)
{
	unsigned int i;
	uint32_t num_macs = 0;
	struct hash *hash;
	struct hash_bucket *hb;
	zebra_mac_t *mac;

	hash = zvni->mac_table;
	if (!hash)
		return num_macs;
	for (i = 0; i < hash->size; i++) {
		for (hb = hash->index[i]; hb; hb = hb->next) {
			mac = (zebra_mac_t *)hb->data;
			if (CHECK_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE))
				num_macs++;
		}
	}

	return num_macs;
}

static uint32_t num_dup_detected_neighs(zebra_vni_t *zvni)
{
	unsigned int i;
	uint32_t num_neighs = 0;
	struct hash *hash;
	struct hash_bucket *hb;
	zebra_neigh_t *nbr;

	hash = zvni->neigh_table;
	if (!hash)
		return num_neighs;
	for (i = 0; i < hash->size; i++) {
		for (hb = hash->index[i]; hb; hb = hb->next) {
			nbr = (zebra_neigh_t *)hb->data;
			if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE))
				num_neighs++;
		}
	}

	return num_neighs;
}

static int advertise_gw_macip_enabled(zebra_vni_t *zvni)
{
	struct zebra_vrf *zvrf;

	zvrf = zebra_vrf_get_evpn();
	if (zvrf && zvrf->advertise_gw_macip)
		return 1;

	if (zvni && zvni->advertise_gw_macip)
		return 1;

	return 0;
}

static int advertise_svi_macip_enabled(zebra_vni_t *zvni)
{
	struct zebra_vrf *zvrf;

	zvrf = zebra_vrf_get_evpn();
	if (zvrf && zvrf->advertise_svi_macip)
		return 1;

	if (zvni && zvni->advertise_svi_macip)
		return 1;

	return 0;
}

/* As part Duplicate Address Detection (DAD) for IP mobility
 * MAC binding changes, ensure to inherit duplicate flag
 * from MAC.
 */
static int zebra_vxlan_ip_inherit_dad_from_mac(struct zebra_vrf *zvrf,
					       zebra_mac_t *old_zmac,
					       zebra_mac_t *new_zmac,
					       zebra_neigh_t *nbr)
{
	bool is_old_mac_dup = false;
	bool is_new_mac_dup = false;

	if (!zvrf->dup_addr_detect)
		return 0;
	/* Check old or new MAC is detected as duplicate
	 * mark this neigh as duplicate
	 */
	if (old_zmac)
		is_old_mac_dup = CHECK_FLAG(old_zmac->flags,
					    ZEBRA_MAC_DUPLICATE);
	if (new_zmac)
		is_new_mac_dup = CHECK_FLAG(new_zmac->flags,
					    ZEBRA_MAC_DUPLICATE);
	/* Old and/or new MAC can be in duplicate state,
	 * based on that IP/Neigh Inherits the flag.
	 * If New MAC is marked duplicate, inherit to the IP.
	 * If old MAC is duplicate but new MAC is not, clear
	 * duplicate flag for IP and reset detection params
	 * and let IP DAD retrigger.
	 */
	if (is_new_mac_dup && !CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE)) {
		SET_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE);
		/* Capture Duplicate detection time */
		nbr->dad_dup_detect_time = monotime(NULL);
		/* Mark neigh inactive */
		ZEBRA_NEIGH_SET_INACTIVE(nbr);

		return 1;
	} else if (is_old_mac_dup && !is_new_mac_dup) {
		UNSET_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE);
		nbr->dad_count = 0;
		nbr->detect_start_time.tv_sec = 0;
		nbr->detect_start_time.tv_usec = 0;
	}
	return 0;
}

static void zebra_vxlan_dup_addr_detect_for_mac(struct zebra_vrf *zvrf,
						zebra_mac_t *mac,
						struct in_addr vtep_ip,
						bool do_dad,
						bool *is_dup_detect,
						bool is_local)
{
	zebra_neigh_t *nbr;
	struct listnode *node = NULL;
	struct timeval elapsed = {0, 0};
	char buf[ETHER_ADDR_STRLEN];
	char buf1[INET6_ADDRSTRLEN];
	bool reset_params = false;

	if (!(zvrf->dup_addr_detect && do_dad))
		return;

	/* MAC is detected as duplicate,
	 * Local MAC event -> hold on advertising to BGP.
	 * Remote MAC event -> hold on installing it.
	 */
	if (CHECK_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE)) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"%s: duplicate addr MAC %s flags 0x%x skip update to client, learn count %u recover time %u",
				__func__,
				prefix_mac2str(&mac->macaddr, buf, sizeof(buf)),
				mac->flags, mac->dad_count,
				zvrf->dad_freeze_time);

		/* For duplicate MAC do not update
		 * client but update neigh due to
		 * this MAC update.
		 */
		if (zvrf->dad_freeze)
			*is_dup_detect = true;

		return;
	}

	/* Check if detection time (M-secs) expired.
	 * Reset learn count and detection start time.
	 */
	monotime_since(&mac->detect_start_time, &elapsed);
	reset_params = (elapsed.tv_sec > zvrf->dad_time);
	if (is_local && !reset_params) {
		/* RFC-7432: A PE/VTEP that detects a MAC mobility
		 * event via LOCAL learning starts an M-second timer.
		 *
		 * NOTE: This is the START of the probe with count is
		 * 0 during LOCAL learn event.
		 * (mac->dad_count == 0 || elapsed.tv_sec >= zvrf->dad_time)
		 */
		reset_params = !mac->dad_count;
	}

	if (reset_params) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"%s: duplicate addr MAC %s flags 0x%x detection time passed, reset learn count %u",
				__func__,
				prefix_mac2str(&mac->macaddr, buf, sizeof(buf)),
				mac->flags, mac->dad_count);

		mac->dad_count = 0;
		/* Start dup. addr detection (DAD) start time,
		 * ONLY during LOCAL learn.
		 */
		if (is_local)
			monotime(&mac->detect_start_time);

	} else if (!is_local) {
		/* For REMOTE MAC, increment detection count
		 * ONLY while in probe window, once window passed,
		 * next local learn event should trigger DAD.
		 */
		mac->dad_count++;
	}

	/* For LOCAL MAC learn event, once count is reset above via either
	 * initial/start detection time or passed the probe time, the count
	 * needs to be incremented.
	 */
	if (is_local)
		mac->dad_count++;

	if (mac->dad_count >= zvrf->dad_max_moves) {
		flog_warn(EC_ZEBRA_DUP_MAC_DETECTED,
			  "VNI %u: MAC %s detected as duplicate during %s VTEP %s",
			  mac->zvni->vni,
			  prefix_mac2str(&mac->macaddr, buf, sizeof(buf)),
			  is_local ? "local update, last" :
			  "remote update, from", inet_ntoa(vtep_ip));

		SET_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE);

		/* Capture Duplicate detection time */
		mac->dad_dup_detect_time = monotime(NULL);

		/* Mark all IPs/Neighs as duplicate
		 * associcated with this MAC
		 */
		for (ALL_LIST_ELEMENTS_RO(mac->neigh_list, node, nbr)) {

			/* Ony Mark IPs which are Local */
			if (!CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_LOCAL))
				continue;

			SET_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE);

			nbr->dad_dup_detect_time = monotime(NULL);

			flog_warn(EC_ZEBRA_DUP_IP_INHERIT_DETECTED,
				  "VNI %u: MAC %s IP %s detected as duplicate during %s update, inherit duplicate from MAC",
				  mac->zvni->vni,
				  prefix_mac2str(&mac->macaddr,
						 buf, sizeof(buf)),
				  ipaddr2str(&nbr->ip, buf1, sizeof(buf1)),
				  is_local ? "local" : "remote");
		}

		/* Start auto recovery timer for this MAC */
		THREAD_OFF(mac->dad_mac_auto_recovery_timer);
		if (zvrf->dad_freeze && zvrf->dad_freeze_time) {
			if (IS_ZEBRA_DEBUG_VXLAN)
				zlog_debug(
					"%s: duplicate addr MAC %s flags 0x%x auto recovery time %u start",
					__func__,
					prefix_mac2str(&mac->macaddr, buf,
						       sizeof(buf)),
					mac->flags, zvrf->dad_freeze_time);

			thread_add_timer(zrouter.master,
					 zebra_vxlan_dad_mac_auto_recovery_exp,
					 mac, zvrf->dad_freeze_time,
					 &mac->dad_mac_auto_recovery_timer);
		}

		/* In case of local update, do not inform to client (BGPd),
		 * upd_neigh for neigh sequence change.
		 */
		if (zvrf->dad_freeze)
			*is_dup_detect = true;
	}
}

static void zebra_vxlan_dup_addr_detect_for_neigh(struct zebra_vrf *zvrf,
						  zebra_neigh_t *nbr,
						  struct in_addr vtep_ip,
						  bool do_dad,
						  bool *is_dup_detect,
						  bool is_local)
{

	struct timeval elapsed = {0, 0};
	char buf[ETHER_ADDR_STRLEN];
	char buf1[INET6_ADDRSTRLEN];
	bool reset_params = false;

	if (!zvrf->dup_addr_detect)
		return;

	/* IP is detected as duplicate or inherit dup
	 * state, hold on to install as remote entry
	 * only if freeze is enabled.
	 */
	if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE)) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"%s: duplicate addr MAC %s IP %s flags 0x%x skip installing, learn count %u recover time %u",
				__func__,
				prefix_mac2str(&nbr->emac, buf, sizeof(buf)),
				ipaddr2str(&nbr->ip, buf1, sizeof(buf1)),
				nbr->flags, nbr->dad_count,
				zvrf->dad_freeze_time);

		if (zvrf->dad_freeze)
			*is_dup_detect = true;

		/* warn-only action, neigh will be installed.
		 * freeze action, it wil not be installed.
		 */
		return;
	}

	if (!do_dad)
		return;

	/* Check if detection time (M-secs) expired.
	 * Reset learn count and detection start time.
	 * During remote mac add, count should already be 1
	 * via local learning.
	 */
	monotime_since(&nbr->detect_start_time, &elapsed);
	reset_params = (elapsed.tv_sec > zvrf->dad_time);

	if (is_local && !reset_params) {
		/* RFC-7432: A PE/VTEP that detects a MAC mobility
		 * event via LOCAL learning starts an M-second timer.
		 *
		 * NOTE: This is the START of the probe with count is
		 * 0 during LOCAL learn event.
		 */
		reset_params = !nbr->dad_count;
	}

	if (reset_params) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"%s: duplicate addr MAC %s IP %s flags 0x%x detection time passed, reset learn count %u",
				__func__,
				prefix_mac2str(&nbr->emac, buf, sizeof(buf)),
				ipaddr2str(&nbr->ip, buf1, sizeof(buf1)),
				nbr->flags, nbr->dad_count);
		/* Reset learn count but do not start detection
		 * during REMOTE learn event.
		 */
		nbr->dad_count = 0;
		/* Start dup. addr detection (DAD) start time,
		 * ONLY during LOCAL learn.
		 */
		if (is_local)
			monotime(&nbr->detect_start_time);

	} else if (!is_local) {
		/* For REMOTE IP/Neigh, increment detection count
		 * ONLY while in probe window, once window passed,
		 * next local learn event should trigger DAD.
		 */
		nbr->dad_count++;
	}

	/* For LOCAL IP/Neigh learn event, once count is reset above via either
	 * initial/start detection time or passed the probe time, the count
	 * needs to be incremented.
	 */
	if (is_local)
		nbr->dad_count++;

	if (nbr->dad_count >= zvrf->dad_max_moves) {
		flog_warn(EC_ZEBRA_DUP_IP_DETECTED,
			  "VNI %u: MAC %s IP %s detected as duplicate during %s VTEP %s",
			  nbr->zvni->vni,
			  prefix_mac2str(&nbr->emac, buf, sizeof(buf)),
			  ipaddr2str(&nbr->ip, buf1, sizeof(buf1)),
			  is_local ? "local update, last" :
			  "remote update, from",
			  inet_ntoa(vtep_ip));

		SET_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE);

		/* Capture Duplicate detection time */
		nbr->dad_dup_detect_time = monotime(NULL);

		/* Start auto recovery timer for this IP */
		THREAD_OFF(nbr->dad_ip_auto_recovery_timer);
		if (zvrf->dad_freeze && zvrf->dad_freeze_time) {
			if (IS_ZEBRA_DEBUG_VXLAN)
				zlog_debug(
					"%s: duplicate addr MAC %s IP %s flags 0x%x auto recovery time %u start",
					__func__,
					prefix_mac2str(&nbr->emac, buf,
						       sizeof(buf)),
					ipaddr2str(&nbr->ip, buf1,
						   sizeof(buf1)),
					nbr->flags, zvrf->dad_freeze_time);

			thread_add_timer(zrouter.master,
				zebra_vxlan_dad_ip_auto_recovery_exp,
				nbr, zvrf->dad_freeze_time,
				&nbr->dad_ip_auto_recovery_timer);
		}
		if (zvrf->dad_freeze)
			*is_dup_detect = true;
	}
}

/*
 * Helper function to determine maximum width of neighbor IP address for
 * display - just because we're dealing with IPv6 addresses that can
 * widely vary.
 */
static void zvni_find_neigh_addr_width(struct hash_bucket *bucket, void *ctxt)
{
	zebra_neigh_t *n;
	char buf[INET6_ADDRSTRLEN];
	struct neigh_walk_ctx *wctx = ctxt;
	int width;

	n = (zebra_neigh_t *)bucket->data;

	ipaddr2str(&n->ip, buf, sizeof(buf));
	width = strlen(buf);
	if (width > wctx->addr_width)
		wctx->addr_width = width;

}

/*
 * Print a specific neighbor entry.
 */
static void zvni_print_neigh(zebra_neigh_t *n, void *ctxt, json_object *json)
{
	struct vty *vty;
	char buf1[ETHER_ADDR_STRLEN];
	char buf2[INET6_ADDRSTRLEN];
	const char *type_str;
	const char *state_str;
	bool flags_present = false;
	struct zebra_vrf *zvrf = NULL;
	struct timeval detect_start_time = {0, 0};
	char timebuf[MONOTIME_STRLEN];
	char thread_buf[THREAD_TIMER_STRLEN];

	zvrf = zebra_vrf_get_evpn();
	if (!zvrf)
		return;

	ipaddr2str(&n->ip, buf2, sizeof(buf2));
	prefix_mac2str(&n->emac, buf1, sizeof(buf1));
	type_str = CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL) ?
						"local" : "remote";
	state_str = IS_ZEBRA_NEIGH_ACTIVE(n) ? "active" : "inactive";
	vty = (struct vty *)ctxt;
	if (json == NULL) {
		bool sync_info = false;

		vty_out(vty, "IP: %s\n",
				ipaddr2str(&n->ip, buf2, sizeof(buf2)));
		vty_out(vty, " Type: %s\n", type_str);
		vty_out(vty, " State: %s\n", state_str);
		vty_out(vty, " MAC: %s\n",
				prefix_mac2str(&n->emac, buf1, sizeof(buf1)));
		vty_out(vty, " Sync-info:");
		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL_INACTIVE)) {
			vty_out(vty, " local-inactive");
			sync_info = true;
		}
		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_ES_PEER_PROXY)) {
			vty_out(vty, " peer-proxy");
			sync_info = true;
		}
		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_ES_PEER_ACTIVE)) {
			vty_out(vty, " peer-active");
			sync_info = true;
		}
		if (n->hold_timer) {
			vty_out(vty, " (ht: %s)",
					thread_timer_to_hhmmss(
						thread_buf,
						sizeof(thread_buf),
						n->hold_timer));
			sync_info = true;
		}
		if (!sync_info)
			vty_out(vty, " -");
		vty_out(vty, "\n");
	} else {
		json_object_string_add(json, "ip", buf2);
		json_object_string_add(json, "type", type_str);
		json_object_string_add(json, "state", state_str);
		json_object_string_add(json, "mac", buf1);
		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL_INACTIVE))
			json_object_boolean_true_add(json,
					"localInactive");
		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_ES_PEER_PROXY))
			json_object_boolean_true_add(json,
					"peerProxy");
		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_ES_PEER_ACTIVE))
			json_object_boolean_true_add(json,
					"peerActive");
		if (n->hold_timer)
			json_object_string_add(json, "peerActiveHold",
					thread_timer_to_hhmmss(
						thread_buf,
						sizeof(thread_buf),
						n->hold_timer));
	}
	if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_REMOTE)) {
		if (n->mac->es) {
			if (json)
				json_object_string_add(json, "remoteEs",
						n->mac->es->esi_str);
			else
				vty_out(vty, " Remote ES: %s\n",
						n->mac->es->esi_str);
		} else {
			if (json)
				json_object_string_add(json, "remoteVtep",
						inet_ntoa(n->r_vtep_ip));
			else
				vty_out(vty, " Remote VTEP: %s\n",
						inet_ntoa(n->r_vtep_ip));
		}
	}
	if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_DEF_GW)) {
		if (!json) {
			vty_out(vty, " Flags: Default-gateway");
			flags_present = true;
		} else
			json_object_boolean_true_add(json, "defaultGateway");
	}
	if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_ROUTER_FLAG)) {
		if (!json) {
			vty_out(vty,
				flags_present ? " ,Router" : " Flags: Router");
			flags_present = true;
		}
	}
	if (json == NULL) {
		if (flags_present)
			vty_out(vty, "\n");
		vty_out(vty, " Local Seq: %u Remote Seq: %u\n",
			n->loc_seq, n->rem_seq);

		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_DUPLICATE)) {
			vty_out(vty, " Duplicate, detected at %s",
				time_to_string(n->dad_dup_detect_time,
					       timebuf));
		} else if (n->dad_count) {
			monotime_since(&n->detect_start_time,
				       &detect_start_time);
			if (detect_start_time.tv_sec <= zvrf->dad_time) {
				time_to_string(n->detect_start_time.tv_sec,
					       timebuf);
				vty_out(vty,
					" Duplicate detection started at %s, detection count %u\n",
					timebuf, n->dad_count);
			}
		}
	} else {
		json_object_int_add(json, "localSequence", n->loc_seq);
		json_object_int_add(json, "remoteSequence", n->rem_seq);
		json_object_int_add(json, "detectionCount",
				    n->dad_count);
		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_DUPLICATE))
			json_object_boolean_true_add(json, "isDuplicate");
		else
			json_object_boolean_false_add(json, "isDuplicate");


	}
}

static void zvni_print_neigh_hdr(struct vty *vty,
		struct neigh_walk_ctx *wctx)
{
	vty_out(vty,
		"Flags: I=local-inactive, P=peer-active, X=peer-proxy\n");
	vty_out(vty, "%*s %-6s %-5s %-8s %-17s %-30s %s\n",
		-wctx->addr_width, "Neighbor", "Type", "Flags",
		"State", "MAC", "Remote ES/VTEP", "Seq #'s");
}

static char *zvni_print_neigh_flags(zebra_neigh_t *n, char *flags_buf,
		uint32_t flags_buf_sz)
{
	snprintf(flags_buf, flags_buf_sz, "%s%s%s",
			(n->flags & ZEBRA_NEIGH_ES_PEER_ACTIVE) ?
			"P" : "",
			(n->flags & ZEBRA_NEIGH_ES_PEER_PROXY) ?
			"X" : "",
			(n->flags & ZEBRA_NEIGH_LOCAL_INACTIVE) ?
			"I" : "");

	return flags_buf;
}

/*
 * Print neighbor hash entry - called for display of all neighbors.
 */
static void zvni_print_neigh_hash(struct hash_bucket *bucket, void *ctxt)
{
	struct vty *vty;
	json_object *json_vni = NULL, *json_row = NULL;
	zebra_neigh_t *n;
	char buf1[ETHER_ADDR_STRLEN];
	char buf2[INET6_ADDRSTRLEN];
	struct neigh_walk_ctx *wctx = ctxt;
	const char *state_str;
	char flags_buf[6];

	vty = wctx->vty;
	json_vni = wctx->json;
	n = (zebra_neigh_t *)bucket->data;

	if (json_vni)
		json_row = json_object_new_object();

	prefix_mac2str(&n->emac, buf1, sizeof(buf1));
	ipaddr2str(&n->ip, buf2, sizeof(buf2));
	state_str = IS_ZEBRA_NEIGH_ACTIVE(n) ? "active" : "inactive";
	if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL)) {
		if (wctx->flags & SHOW_REMOTE_NEIGH_FROM_VTEP)
			return;

		if (json_vni == NULL) {
			vty_out(vty, "%*s %-6s %-5s %-8s %-17s %-30s %u/%u\n",
				-wctx->addr_width, buf2, "local",
				zvni_print_neigh_flags(n, flags_buf,
					sizeof(flags_buf)), state_str,
				buf1, "", n->loc_seq, n->rem_seq);
		} else {
			json_object_string_add(json_row, "type", "local");
			json_object_string_add(json_row, "state", state_str);
			json_object_string_add(json_row, "mac", buf1);
			if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_DEF_GW))
				json_object_boolean_true_add(
						json_row, "defaultGateway");
			json_object_int_add(json_row, "localSequence",
					    n->loc_seq);
			json_object_int_add(json_row, "remoteSequence",
					    n->rem_seq);
			json_object_int_add(json_row, "detectionCount",
					    n->dad_count);
			if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_DUPLICATE))
				json_object_boolean_true_add(json_row,
							     "isDuplicate");
			else
				json_object_boolean_false_add(json_row,
							      "isDuplicate");
		}
		wctx->count++;
	} else if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_REMOTE)) {
		if ((wctx->flags & SHOW_REMOTE_NEIGH_FROM_VTEP) &&
		    !IPV4_ADDR_SAME(&n->r_vtep_ip, &wctx->r_vtep_ip))
			return;

		if (json_vni == NULL) {
			if ((wctx->flags & SHOW_REMOTE_NEIGH_FROM_VTEP) &&
			    (wctx->count == 0))
				zvni_print_neigh_hdr(vty, wctx);
			vty_out(vty, "%*s %-6s %-5s %-8s %-17s %-30s %u/%u\n",
				-wctx->addr_width, buf2, "remote",
				zvni_print_neigh_flags(n, flags_buf,
					sizeof(flags_buf)),
				state_str, buf1,
				n->mac->es ? n->mac->es->esi_str :
				inet_ntoa(n->r_vtep_ip),
				n->loc_seq, n->rem_seq);
		} else {
			json_object_string_add(json_row, "type", "remote");
			json_object_string_add(json_row, "state", state_str);
			json_object_string_add(json_row, "mac", buf1);
			if (n->mac->es)
				json_object_string_add(json_row, "remoteEs",
						n->mac->es->esi_str);
			else
				json_object_string_add(json_row, "remoteVtep",
						inet_ntoa(n->r_vtep_ip));
			if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_DEF_GW))
				json_object_boolean_true_add(json_row,
							     "defaultGateway");
			json_object_int_add(json_row, "localSequence",
					    n->loc_seq);
			json_object_int_add(json_row, "remoteSequence",
					    n->rem_seq);
			json_object_int_add(json_row, "detectionCount",
					    n->dad_count);
			if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_DUPLICATE))
				json_object_boolean_true_add(json_row,
							     "isDuplicate");
			else
				json_object_boolean_false_add(json_row,
							      "isDuplicate");
		}
		wctx->count++;
	}

	if (json_vni)
		json_object_object_add(json_vni, buf2, json_row);
}

/*
 * Print neighbor hash entry in detail - called for display of all neighbors.
 */
static void zvni_print_neigh_hash_detail(struct hash_bucket *bucket, void *ctxt)
{
	struct vty *vty;
	json_object *json_vni = NULL, *json_row = NULL;
	zebra_neigh_t *n;
	char buf[INET6_ADDRSTRLEN];
	struct neigh_walk_ctx *wctx = ctxt;

	vty = wctx->vty;
	json_vni = wctx->json;
	n = (zebra_neigh_t *)bucket->data;
	if (!n)
		return;

	ipaddr2str(&n->ip, buf, sizeof(buf));
	if (json_vni)
		json_row = json_object_new_object();

	zvni_print_neigh(n, vty, json_row);

	if (json_vni)
		json_object_object_add(json_vni, buf, json_row);
}

/*
 * Print neighbors for all VNI.
 */
static void zvni_print_neigh_hash_all_vni(struct hash_bucket *bucket,
					  void **args)
{
	struct vty *vty;
	json_object *json = NULL, *json_vni = NULL;
	zebra_vni_t *zvni;
	uint32_t num_neigh;
	struct neigh_walk_ctx wctx;
	char vni_str[VNI_STR_LEN];
	uint32_t print_dup;

	vty = (struct vty *)args[0];
	json = (json_object *)args[1];
	print_dup = (uint32_t)(uintptr_t)args[2];

	zvni = (zebra_vni_t *)bucket->data;

	num_neigh = hashcount(zvni->neigh_table);

	if (print_dup)
		num_neigh = num_dup_detected_neighs(zvni);

	if (json == NULL) {
		vty_out(vty,
			"\nVNI %u #ARP (IPv4 and IPv6, local and remote) %u\n\n",
			zvni->vni, num_neigh);
	} else {
		json_vni = json_object_new_object();
		json_object_int_add(json_vni, "numArpNd", num_neigh);
		snprintf(vni_str, sizeof(vni_str), "%u", zvni->vni);
	}

	if (!num_neigh) {
		if (json)
			json_object_object_add(json, vni_str, json_vni);
		return;
	}

	/* Since we have IPv6 addresses to deal with which can vary widely in
	 * size, we try to be a bit more elegant in display by first computing
	 * the maximum width.
	 */
	memset(&wctx, 0, sizeof(struct neigh_walk_ctx));
	wctx.zvni = zvni;
	wctx.vty = vty;
	wctx.addr_width = 15;
	wctx.json = json_vni;
	hash_iterate(zvni->neigh_table, zvni_find_neigh_addr_width, &wctx);

	if (json == NULL)
		zvni_print_neigh_hdr(vty, &wctx);

	if (print_dup)
		hash_iterate(zvni->neigh_table, zvni_print_dad_neigh_hash,
			     &wctx);
	else
		hash_iterate(zvni->neigh_table, zvni_print_neigh_hash, &wctx);

	if (json)
		json_object_object_add(json, vni_str, json_vni);
}

static void zvni_print_dad_neigh_hash(struct hash_bucket *bucket, void *ctxt)
{
	zebra_neigh_t *nbr;

	nbr = (zebra_neigh_t *)bucket->data;
	if (!nbr)
		return;

	if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE))
		zvni_print_neigh_hash(bucket, ctxt);
}

static void zvni_print_dad_neigh_hash_detail(struct hash_bucket *bucket,
					     void *ctxt)
{
	zebra_neigh_t *nbr;

	nbr = (zebra_neigh_t *)bucket->data;
	if (!nbr)
		return;

	if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE))
		zvni_print_neigh_hash_detail(bucket, ctxt);
}

/*
 * Print neighbors for all VNIs in detail.
 */
static void zvni_print_neigh_hash_all_vni_detail(struct hash_bucket *bucket,
						 void **args)
{
	struct vty *vty;
	json_object *json = NULL, *json_vni = NULL;
	zebra_vni_t *zvni;
	uint32_t num_neigh;
	struct neigh_walk_ctx wctx;
	char vni_str[VNI_STR_LEN];
	uint32_t print_dup;

	vty = (struct vty *)args[0];
	json = (json_object *)args[1];
	print_dup = (uint32_t)(uintptr_t)args[2];

	zvni = (zebra_vni_t *)bucket->data;
	if (!zvni) {
		if (json)
			vty_out(vty, "{}\n");
		return;
	}
	num_neigh = hashcount(zvni->neigh_table);

	if (print_dup && num_dup_detected_neighs(zvni) == 0)
		return;

	if (json == NULL) {
		vty_out(vty,
			"\nVNI %u #ARP (IPv4 and IPv6, local and remote) %u\n\n",
			zvni->vni, num_neigh);
	} else {
		json_vni = json_object_new_object();
		json_object_int_add(json_vni, "numArpNd", num_neigh);
		snprintf(vni_str, sizeof(vni_str), "%u", zvni->vni);
	}
	if (!num_neigh) {
		if (json)
			json_object_object_add(json, vni_str, json_vni);
		return;
	}

	memset(&wctx, 0, sizeof(struct neigh_walk_ctx));
	wctx.zvni = zvni;
	wctx.vty = vty;
	wctx.addr_width = 15;
	wctx.json = json_vni;

	if (print_dup)
		hash_iterate(zvni->neigh_table,
			     zvni_print_dad_neigh_hash_detail, &wctx);
	else
		hash_iterate(zvni->neigh_table, zvni_print_neigh_hash_detail,
			     &wctx);

	if (json)
		json_object_object_add(json, vni_str, json_vni);
}

/* print a specific next hop for an l3vni */
static void zl3vni_print_nh(zebra_neigh_t *n, struct vty *vty,
			    json_object *json)
{
	char buf1[ETHER_ADDR_STRLEN];
	char buf2[INET6_ADDRSTRLEN];
	json_object *json_hosts = NULL;
	struct host_rb_entry *hle;

	if (!json) {
		vty_out(vty, "Ip: %s\n",
			ipaddr2str(&n->ip, buf2, sizeof(buf2)));
		vty_out(vty, "  RMAC: %s\n",
			prefix_mac2str(&n->emac, buf1, sizeof(buf1)));
		vty_out(vty, "  Refcount: %d\n",
			rb_host_count(&n->host_rb));
		vty_out(vty, "  Prefixes:\n");
		RB_FOREACH (hle, host_rb_tree_entry, &n->host_rb)
			vty_out(vty, "    %s\n",
				prefix2str(&hle->p, buf2, sizeof(buf2)));
	} else {
		json_hosts = json_object_new_array();
		json_object_string_add(
			json, "ip", ipaddr2str(&(n->ip), buf2, sizeof(buf2)));
		json_object_string_add(
			json, "routerMac",
			prefix_mac2str(&n->emac, buf2, sizeof(buf2)));
		json_object_int_add(json, "refCount",
				    rb_host_count(&n->host_rb));
		RB_FOREACH (hle, host_rb_tree_entry, &n->host_rb)
			json_object_array_add(json_hosts,
					      json_object_new_string(prefix2str(
										&hle->p, buf2, sizeof(buf2))));
		json_object_object_add(json, "prefixList", json_hosts);
	}
}

/* Print a specific RMAC entry */
static void zl3vni_print_rmac(zebra_mac_t *zrmac, struct vty *vty,
			      json_object *json)
{
	char buf1[ETHER_ADDR_STRLEN];
	char buf2[PREFIX_STRLEN];
	json_object *json_hosts = NULL;
	struct host_rb_entry *hle;

	if (!json) {
		vty_out(vty, "MAC: %s\n",
			prefix_mac2str(&zrmac->macaddr, buf1, sizeof(buf1)));
		vty_out(vty, " Remote VTEP: %s\n",
			inet_ntoa(zrmac->fwd_info.r_vtep_ip));
		vty_out(vty, " Refcount: %d\n", rb_host_count(&zrmac->host_rb));
		vty_out(vty, "  Prefixes:\n");
		RB_FOREACH (hle, host_rb_tree_entry, &zrmac->host_rb)
			vty_out(vty, "    %s\n",
				prefix2str(&hle->p, buf2, sizeof(buf2)));
	} else {
		json_hosts = json_object_new_array();
		json_object_string_add(
			json, "routerMac",
			prefix_mac2str(&zrmac->macaddr, buf1, sizeof(buf1)));
		json_object_string_add(json, "vtepIp",
				       inet_ntoa(zrmac->fwd_info.r_vtep_ip));
		json_object_int_add(json, "refCount",
				    rb_host_count(&zrmac->host_rb));
		json_object_int_add(json, "localSequence", zrmac->loc_seq);
		json_object_int_add(json, "remoteSequence", zrmac->rem_seq);
		RB_FOREACH (hle, host_rb_tree_entry, &zrmac->host_rb)
			json_object_array_add(
				json_hosts,
				json_object_new_string(prefix2str(
					&hle->p, buf2, sizeof(buf2))));
		json_object_object_add(json, "prefixList", json_hosts);
	}
}

static void
zebra_vxlan_mac_get_access_info(zebra_mac_t *mac,
		struct interface **ifpP, vlanid_t *vid)
{
	/* if the mac is associated with an ES we must get the access
	 * info from the ES
	 */
	if (mac->es) {
		struct zebra_if *zif;

		/* get the access port from the es */
		*ifpP = mac->es->zif ? mac->es->zif->ifp : NULL;
		/* get the vlan from the VNI */
		if (mac->zvni->vxlan_if) {
			zif = mac->zvni->vxlan_if->info;
			*vid = zif->l2info.vxl.access_vlan;
		} else {
			*vid = 0;
		}
	} else {
		struct zebra_ns *zns;

		*vid = mac->fwd_info.local.vid;
		zns = zebra_ns_lookup(NS_DEFAULT);
		*ifpP = if_lookup_by_index_per_ns(zns,
				mac->fwd_info.local.ifindex);
	}
}

/*
 * Print a specific MAC entry.
 */
static void zvni_print_mac(zebra_mac_t *mac, void *ctxt, json_object *json)
{
	struct vty *vty;
	zebra_neigh_t *n = NULL;
	struct listnode *node = NULL;
	char buf1[ETHER_ADDR_STRLEN];
	char buf2[INET6_ADDRSTRLEN];
	struct zebra_vrf *zvrf;
	struct timeval detect_start_time = {0, 0};
	char timebuf[MONOTIME_STRLEN];
	char thread_buf[THREAD_TIMER_STRLEN];

	zvrf = zebra_vrf_get_evpn();
	if (!zvrf)
		return;

	vty = (struct vty *)ctxt;
	prefix_mac2str(&mac->macaddr, buf1, sizeof(buf1));

	if (json) {
		json_object *json_mac = json_object_new_object();

		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL)) {
			struct interface *ifp;
			vlanid_t vid;

			zebra_vxlan_mac_get_access_info(mac,
					&ifp, &vid);
			json_object_string_add(json_mac, "type", "local");
			if (ifp) {
				json_object_string_add(json_mac,
						"intf", ifp->name);
				json_object_int_add(json_mac,
						"ifindex", ifp->ifindex);
			}
			if (vid)
				json_object_int_add(json_mac, "vlan",
						vid);
		} else if (CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE)) {
			json_object_string_add(json_mac, "type", "remote");
			json_object_string_add(
				json_mac, "remoteVtep",
				inet_ntoa(mac->fwd_info.r_vtep_ip));
		} else if (CHECK_FLAG(mac->flags, ZEBRA_MAC_AUTO))
			json_object_string_add(json_mac, "type", "auto");

		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_STICKY))
			json_object_boolean_true_add(json_mac, "stickyMac");

		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_DEF_GW))
			json_object_boolean_true_add(json_mac,
						     "defaultGateway");

		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE_DEF_GW))
			json_object_boolean_true_add(json_mac,
						     "remoteGatewayMac");

		json_object_int_add(json_mac, "localSequence", mac->loc_seq);
		json_object_int_add(json_mac, "remoteSequence", mac->rem_seq);

		json_object_int_add(json_mac, "detectionCount", mac->dad_count);
		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE))
			json_object_boolean_true_add(json_mac, "isDuplicate");
		else
			json_object_boolean_false_add(json_mac, "isDuplicate");

		json_object_int_add(json_mac, "syncNeighCount", mac->sync_neigh_cnt);
		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL_INACTIVE))
			json_object_boolean_true_add(json_mac,
					"localInactive");
		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_ES_PEER_PROXY))
			json_object_boolean_true_add(json_mac,
					"peerProxy");
		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_ES_PEER_ACTIVE))
			json_object_boolean_true_add(json_mac,
					"peerActive");
		if (mac->hold_timer)
			json_object_string_add(json_mac, "peerActiveHold",
					thread_timer_to_hhmmss(
						thread_buf,
						sizeof(thread_buf),
						mac->hold_timer));
		/* print all the associated neigh */
		if (!listcount(mac->neigh_list))
			json_object_string_add(json_mac, "neighbors", "none");
		else {
			json_object *json_active_nbrs = json_object_new_array();
			json_object *json_inactive_nbrs =
				json_object_new_array();
			json_object *json_nbrs = json_object_new_object();

			for (ALL_LIST_ELEMENTS_RO(mac->neigh_list, node, n)) {
				if (IS_ZEBRA_NEIGH_ACTIVE(n))
					json_object_array_add(
						json_active_nbrs,
						json_object_new_string(
							ipaddr2str(
								&n->ip, buf2,
								sizeof(buf2))));
				else
					json_object_array_add(
						json_inactive_nbrs,
						json_object_new_string(
							ipaddr2str(
								&n->ip, buf2,
								sizeof(buf2))));
			}

			json_object_object_add(json_nbrs, "active",
					       json_active_nbrs);
			json_object_object_add(json_nbrs, "inactive",
					       json_inactive_nbrs);
			json_object_object_add(json_mac, "neighbors",
					       json_nbrs);
		}

		json_object_object_add(json, buf1, json_mac);
	} else {
		vty_out(vty, "MAC: %s\n", buf1);

		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL)) {
			struct interface *ifp;
			vlanid_t vid;

			zebra_vxlan_mac_get_access_info(mac,
					&ifp, &vid);

			if (mac->es)
				vty_out(vty, " ESI: %s\n", mac->es->esi_str);

			if (ifp)
				vty_out(vty, " Intf: %s(%u)",
						ifp->name, ifp->ifindex);
			else
				vty_out(vty, " Intf: -");
			vty_out(vty, " VLAN: %u", vid);
		} else if (CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE)) {
			if (mac->es)
				vty_out(vty, " Remote ES: %s",
						mac->es->esi_str);
			else
				vty_out(vty, " Remote VTEP: %s",
					inet_ntoa(mac->fwd_info.r_vtep_ip));
		} else if (CHECK_FLAG(mac->flags, ZEBRA_MAC_AUTO)) {
			vty_out(vty, " Auto Mac ");
		}

		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_STICKY))
			vty_out(vty, " Sticky Mac ");

		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_DEF_GW))
			vty_out(vty, " Default-gateway Mac ");

		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE_DEF_GW))
			vty_out(vty, " Remote-gateway Mac ");

		vty_out(vty, "\n");
		vty_out(vty, " Sync-info: neigh#: %u", mac->sync_neigh_cnt);
		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL_INACTIVE))
			vty_out(vty, " local-inactive");
		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_ES_PEER_PROXY))
			vty_out(vty, " peer-proxy");
		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_ES_PEER_ACTIVE))
			vty_out(vty, " peer-active");
		if (mac->hold_timer)
			vty_out(vty, " (ht: %s)",
					thread_timer_to_hhmmss(
						thread_buf,
						sizeof(thread_buf),
						mac->hold_timer));
		vty_out(vty, "\n");
		vty_out(vty, " Local Seq: %u Remote Seq: %u",
			mac->loc_seq, mac->rem_seq);
		vty_out(vty, "\n");

		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE)) {
			vty_out(vty, " Duplicate, detected at %s",
				time_to_string(mac->dad_dup_detect_time,
					       timebuf));
		} else if (mac->dad_count) {
			monotime_since(&mac->detect_start_time,
			       &detect_start_time);
			if (detect_start_time.tv_sec <= zvrf->dad_time) {
				time_to_string(mac->detect_start_time.tv_sec,
					       timebuf);
				vty_out(vty,
					" Duplicate detection started at %s, detection count %u\n",
					timebuf, mac->dad_count);
			}
		}

		/* print all the associated neigh */
		vty_out(vty, " Neighbors:\n");
		if (!listcount(mac->neigh_list))
			vty_out(vty, "    No Neighbors\n");
		else {
			for (ALL_LIST_ELEMENTS_RO(mac->neigh_list, node, n)) {
				vty_out(vty, "    %s %s\n",
					ipaddr2str(&n->ip, buf2, sizeof(buf2)),
					(IS_ZEBRA_NEIGH_ACTIVE(n)
						 ? "Active"
						 : "Inactive"));
			}
		}

		vty_out(vty, "\n");
	}
}

static char *zvni_print_mac_flags(zebra_mac_t *mac, char *flags_buf,
	uint32_t flags_buf_sz)
{
	snprintf(flags_buf, flags_buf_sz, "%s%s%s%s",
			mac->sync_neigh_cnt ?
			"N" : "",
			(mac->flags & ZEBRA_MAC_ES_PEER_ACTIVE) ?
			"P" : "",
			(mac->flags & ZEBRA_MAC_ES_PEER_PROXY) ?
			"X" : "",
			(mac->flags & ZEBRA_MAC_LOCAL_INACTIVE) ?
			"I" : "");

	return flags_buf;
}

/*
 * Print MAC hash entry - called for display of all MACs.
 */
static void zvni_print_mac_hash(struct hash_bucket *bucket, void *ctxt)
{
	struct vty *vty;
	json_object *json_mac_hdr = NULL, *json_mac = NULL;
	zebra_mac_t *mac;
	char buf1[ETHER_ADDR_STRLEN];
	struct mac_walk_ctx *wctx = ctxt;
	char flags_buf[6];

	vty = wctx->vty;
	json_mac_hdr = wctx->json;
	mac = (zebra_mac_t *)bucket->data;

	prefix_mac2str(&mac->macaddr, buf1, sizeof(buf1));

	if (json_mac_hdr)
		json_mac = json_object_new_object();

	if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL)) {
		struct interface *ifp;
		vlanid_t vid;

		if (wctx->flags & SHOW_REMOTE_MAC_FROM_VTEP)
			return;

		zebra_vxlan_mac_get_access_info(mac,
				&ifp, &vid);
		if (json_mac_hdr == NULL) {
			vty_out(vty, "%-17s %-6s %-5s %-30s", buf1, "local",
					zvni_print_mac_flags(mac, flags_buf,
						sizeof(flags_buf)),
					ifp ? ifp->name : "-");
		} else {
			json_object_string_add(json_mac, "type", "local");
			if (ifp)
				json_object_string_add(json_mac,
						"intf", ifp->name);
		}
		if (vid) {
			if (json_mac_hdr == NULL)
				vty_out(vty, " %-5u", vid);
			else
				json_object_int_add(json_mac, "vlan", vid);
		} else /* No vid? fill out the space */
			if (json_mac_hdr == NULL)
				vty_out(vty, " %-5s", "");
		if (json_mac_hdr == NULL) {
			vty_out(vty, " %u/%u", mac->loc_seq, mac->rem_seq);
			vty_out(vty, "\n");
		} else {
			json_object_int_add(json_mac, "localSequence",
					    mac->loc_seq);
			json_object_int_add(json_mac, "remoteSequence",
					    mac->rem_seq);
			json_object_int_add(json_mac, "detectionCount",
					    mac->dad_count);
			if (CHECK_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE))
				json_object_boolean_true_add(json_mac,
							     "isDuplicate");
			else
				json_object_boolean_false_add(json_mac,
							     "isDuplicate");
			json_object_object_add(json_mac_hdr, buf1, json_mac);
		}

		wctx->count++;

	} else if (CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE)) {

		if ((wctx->flags & SHOW_REMOTE_MAC_FROM_VTEP) &&
		    !IPV4_ADDR_SAME(&mac->fwd_info.r_vtep_ip,
				    &wctx->r_vtep_ip))
			return;

		if (json_mac_hdr == NULL) {
			if ((wctx->flags & SHOW_REMOTE_MAC_FROM_VTEP) &&
					(wctx->count == 0)) {
				vty_out(vty, "\nVNI %u\n\n", wctx->zvni->vni);
				vty_out(vty, "%-17s %-6s %-5s%-30s %-5s %s\n",
					"MAC", "Type", "Flags",
					"Intf/Remote ES/VTEP",
					"VLAN", "Seq #'s");
			}
			vty_out(vty, "%-17s %-6s %-5s %-30s %-5s %u/%u\n", buf1,
				"remote",
				zvni_print_mac_flags(mac, flags_buf,
					sizeof(flags_buf)),
				mac->es ?  mac->es->esi_str :
				inet_ntoa(mac->fwd_info.r_vtep_ip),
				"", mac->loc_seq, mac->rem_seq);
		} else {
			json_object_string_add(json_mac, "type", "remote");
			json_object_string_add(json_mac, "remoteVtep",
					inet_ntoa(mac->fwd_info.r_vtep_ip));
			json_object_object_add(json_mac_hdr, buf1, json_mac);
			json_object_int_add(json_mac, "localSequence",
					    mac->loc_seq);
			json_object_int_add(json_mac, "remoteSequence",
					    mac->rem_seq);
			json_object_int_add(json_mac, "detectionCount",
					    mac->dad_count);
			if (CHECK_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE))
				json_object_boolean_true_add(json_mac,
							     "isDuplicate");
			else
				json_object_boolean_false_add(json_mac,
							      "isDuplicate");

		}

		wctx->count++;
	}
}

/* Print Duplicate MAC */
static void zvni_print_dad_mac_hash(struct hash_bucket *bucket, void *ctxt)
{
	zebra_mac_t *mac;

	mac = (zebra_mac_t *)bucket->data;
	if (!mac)
		return;

	if (CHECK_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE))
		zvni_print_mac_hash(bucket, ctxt);
}

/*
 * Print MAC hash entry in detail - called for display of all MACs.
 */
static void zvni_print_mac_hash_detail(struct hash_bucket *bucket, void *ctxt)
{
	struct vty *vty;
	json_object *json_mac_hdr = NULL;
	zebra_mac_t *mac;
	struct mac_walk_ctx *wctx = ctxt;
	char buf1[ETHER_ADDR_STRLEN];

	vty = wctx->vty;
	json_mac_hdr = wctx->json;
	mac = (zebra_mac_t *)bucket->data;
	if (!mac)
		return;

	wctx->count++;
	prefix_mac2str(&mac->macaddr, buf1, sizeof(buf1));

	zvni_print_mac(mac, vty, json_mac_hdr);
}

/* Print Duplicate MAC in detail */
static void zvni_print_dad_mac_hash_detail(struct hash_bucket *bucket,
					   void *ctxt)
{
	zebra_mac_t *mac;

	mac = (zebra_mac_t *)bucket->data;
	if (!mac)
		return;

	if (CHECK_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE))
		zvni_print_mac_hash_detail(bucket, ctxt);
}

/*
 * Print MACs for all VNI.
 */
static void zvni_print_mac_hash_all_vni(struct hash_bucket *bucket, void *ctxt)
{
	struct vty *vty;
	json_object *json = NULL, *json_vni = NULL;
	json_object *json_mac = NULL;
	zebra_vni_t *zvni;
	uint32_t num_macs;
	struct mac_walk_ctx *wctx = ctxt;
	char vni_str[VNI_STR_LEN];

	vty = wctx->vty;
	json = wctx->json;

	zvni = (zebra_vni_t *)bucket->data;
	wctx->zvni = zvni;

	/*We are iterating over a new VNI, set the count to 0*/
	wctx->count = 0;

	num_macs = num_valid_macs(zvni);
	if (!num_macs)
		return;

	if (wctx->print_dup)
		num_macs = num_dup_detected_macs(zvni);

	if (json) {
		json_vni = json_object_new_object();
		json_mac = json_object_new_object();
		snprintf(vni_str, sizeof(vni_str), "%u", zvni->vni);
	}

	if (!CHECK_FLAG(wctx->flags, SHOW_REMOTE_MAC_FROM_VTEP)) {
		if (json == NULL) {
			vty_out(vty, "\nVNI %u #MACs (local and remote) %u\n\n",
				zvni->vni, num_macs);
			vty_out(vty,
				"Flags: N=sync-neighs, I=local-inactive, P=peer-active, X=peer-proxy\n");
			vty_out(vty, "%-17s %-6s %-5s %-30s %-5s %s\n", "MAC",
				"Type", "Flags", "Intf/Remote ES/VTEP",
				"VLAN", "Seq #'s");
		} else
			json_object_int_add(json_vni, "numMacs", num_macs);
	}

	if (!num_macs) {
		if (json) {
			json_object_int_add(json_vni, "numMacs", num_macs);
			json_object_object_add(json, vni_str, json_vni);
		}
		return;
	}

	/* assign per-vni to wctx->json object to fill macs
	 * under the vni. Re-assign primary json object to fill
	 * next vni information.
	 */
	wctx->json = json_mac;
	if (wctx->print_dup)
		hash_iterate(zvni->mac_table, zvni_print_dad_mac_hash, wctx);
	else
		hash_iterate(zvni->mac_table, zvni_print_mac_hash, wctx);
	wctx->json = json;
	if (json) {
		if (wctx->count)
			json_object_object_add(json_vni, "macs", json_mac);
		json_object_object_add(json, vni_str, json_vni);
	}
}

/*
 * Print MACs in detail for all VNI.
 */
static void zvni_print_mac_hash_all_vni_detail(struct hash_bucket *bucket,
					       void *ctxt)
{
	struct vty *vty;
	json_object *json = NULL, *json_vni = NULL;
	json_object *json_mac = NULL;
	zebra_vni_t *zvni;
	uint32_t num_macs;
	struct mac_walk_ctx *wctx = ctxt;
	char vni_str[VNI_STR_LEN];

	vty = wctx->vty;
	json = wctx->json;

	zvni = (zebra_vni_t *)bucket->data;
	if (!zvni) {
		if (json)
			vty_out(vty, "{}\n");
		return;
	}
	wctx->zvni = zvni;

	/*We are iterating over a new VNI, set the count to 0*/
	wctx->count = 0;

	num_macs = num_valid_macs(zvni);
	if (!num_macs)
		return;

	if (wctx->print_dup && (num_dup_detected_macs(zvni) == 0))
		return;

	if (json) {
		json_vni = json_object_new_object();
		json_mac = json_object_new_object();
		snprintf(vni_str, sizeof(vni_str), "%u", zvni->vni);
	}

	if (!CHECK_FLAG(wctx->flags, SHOW_REMOTE_MAC_FROM_VTEP)) {
		if (json == NULL) {
			vty_out(vty, "\nVNI %u #MACs (local and remote) %u\n\n",
				zvni->vni, num_macs);
		} else
			json_object_int_add(json_vni, "numMacs", num_macs);
	}
	/* assign per-vni to wctx->json object to fill macs
	 * under the vni. Re-assign primary json object to fill
	 * next vni information.
	 */
	wctx->json = json_mac;
	if (wctx->print_dup)
		hash_iterate(zvni->mac_table, zvni_print_dad_mac_hash_detail,
			     wctx);
	else
		hash_iterate(zvni->mac_table, zvni_print_mac_hash_detail, wctx);
	wctx->json = json;
	if (json) {
		if (wctx->count)
			json_object_object_add(json_vni, "macs", json_mac);
		json_object_object_add(json, vni_str, json_vni);
	}
}

static void zl3vni_print_nh_hash(struct hash_bucket *bucket, void *ctx)
{
	struct nh_walk_ctx *wctx = NULL;
	struct vty *vty = NULL;
	struct json_object *json_vni = NULL;
	struct json_object *json_nh = NULL;
	zebra_neigh_t *n = NULL;
	char buf1[ETHER_ADDR_STRLEN];
	char buf2[INET6_ADDRSTRLEN];

	wctx = (struct nh_walk_ctx *)ctx;
	vty = wctx->vty;
	json_vni = wctx->json;
	if (json_vni)
		json_nh = json_object_new_object();
	n = (zebra_neigh_t *)bucket->data;

	if (!json_vni) {
		vty_out(vty, "%-15s %-17s\n",
			ipaddr2str(&(n->ip), buf2, sizeof(buf2)),
			prefix_mac2str(&n->emac, buf1, sizeof(buf1)));
	} else {
		json_object_string_add(json_nh, "nexthopIp",
				       ipaddr2str(&n->ip, buf2, sizeof(buf2)));
		json_object_string_add(
			json_nh, "routerMac",
			prefix_mac2str(&n->emac, buf1, sizeof(buf1)));
		json_object_object_add(json_vni,
				       ipaddr2str(&(n->ip), buf2, sizeof(buf2)),
				       json_nh);
	}
}

static void zl3vni_print_nh_hash_all_vni(struct hash_bucket *bucket,
					 void **args)
{
	struct vty *vty = NULL;
	json_object *json = NULL;
	json_object *json_vni = NULL;
	zebra_l3vni_t *zl3vni = NULL;
	uint32_t num_nh = 0;
	struct nh_walk_ctx wctx;
	char vni_str[VNI_STR_LEN];

	vty = (struct vty *)args[0];
	json = (struct json_object *)args[1];

	zl3vni = (zebra_l3vni_t *)bucket->data;

	num_nh = hashcount(zl3vni->nh_table);
	if (!num_nh)
		return;

	if (json) {
		json_vni = json_object_new_object();
		snprintf(vni_str, sizeof(vni_str), "%u", zl3vni->vni);
	}

	if (json == NULL) {
		vty_out(vty, "\nVNI %u #Next-Hops %u\n\n", zl3vni->vni, num_nh);
		vty_out(vty, "%-15s %-17s\n", "IP", "RMAC");
	} else
		json_object_int_add(json_vni, "numNextHops", num_nh);

	memset(&wctx, 0, sizeof(struct nh_walk_ctx));
	wctx.vty = vty;
	wctx.json = json_vni;
	hash_iterate(zl3vni->nh_table, zl3vni_print_nh_hash, &wctx);
	if (json)
		json_object_object_add(json, vni_str, json_vni);
}

static void zl3vni_print_rmac_hash_all_vni(struct hash_bucket *bucket,
					   void **args)
{
	struct vty *vty = NULL;
	json_object *json = NULL;
	json_object *json_vni = NULL;
	zebra_l3vni_t *zl3vni = NULL;
	uint32_t num_rmacs;
	struct rmac_walk_ctx wctx;
	char vni_str[VNI_STR_LEN];

	vty = (struct vty *)args[0];
	json = (struct json_object *)args[1];

	zl3vni = (zebra_l3vni_t *)bucket->data;

	num_rmacs = hashcount(zl3vni->rmac_table);
	if (!num_rmacs)
		return;

	if (json) {
		json_vni = json_object_new_object();
		snprintf(vni_str, sizeof(vni_str), "%u", zl3vni->vni);
	}

	if (json == NULL) {
		vty_out(vty, "\nVNI %u #RMACs %u\n\n", zl3vni->vni, num_rmacs);
		vty_out(vty, "%-17s %-21s\n", "RMAC", "Remote VTEP");
	} else
		json_object_int_add(json_vni, "numRmacs", num_rmacs);

	/* assign per-vni to wctx->json object to fill macs
	 * under the vni. Re-assign primary json object to fill
	 * next vni information.
	 */
	memset(&wctx, 0, sizeof(struct rmac_walk_ctx));
	wctx.vty = vty;
	wctx.json = json_vni;
	hash_iterate(zl3vni->rmac_table, zl3vni_print_rmac_hash, &wctx);
	if (json)
		json_object_object_add(json, vni_str, json_vni);
}

static void zl3vni_print_rmac_hash(struct hash_bucket *bucket, void *ctx)
{
	zebra_mac_t *zrmac = NULL;
	struct rmac_walk_ctx *wctx = NULL;
	struct vty *vty = NULL;
	struct json_object *json = NULL;
	struct json_object *json_rmac = NULL;
	char buf[ETHER_ADDR_STRLEN];

	wctx = (struct rmac_walk_ctx *)ctx;
	vty = wctx->vty;
	json = wctx->json;
	if (json)
		json_rmac = json_object_new_object();
	zrmac = (zebra_mac_t *)bucket->data;

	if (!json) {
		vty_out(vty, "%-17s %-21s\n",
			prefix_mac2str(&zrmac->macaddr, buf, sizeof(buf)),
			inet_ntoa(zrmac->fwd_info.r_vtep_ip));
	} else {
		json_object_string_add(
			json_rmac, "routerMac",
			prefix_mac2str(&zrmac->macaddr, buf, sizeof(buf)));
		json_object_string_add(json_rmac, "vtepIp",
				       inet_ntoa(zrmac->fwd_info.r_vtep_ip));
		json_object_object_add(
			json, prefix_mac2str(&zrmac->macaddr, buf, sizeof(buf)),
			json_rmac);
	}
}

/* print a specific L3 VNI entry */
static void zl3vni_print(zebra_l3vni_t *zl3vni, void **ctx)
{
	char buf[ETHER_ADDR_STRLEN];
	struct vty *vty = NULL;
	json_object *json = NULL;
	zebra_vni_t *zvni = NULL;
	json_object *json_vni_list = NULL;
	struct listnode *node = NULL, *nnode = NULL;

	vty = ctx[0];
	json = ctx[1];

	if (!json) {
		vty_out(vty, "VNI: %u\n", zl3vni->vni);
		vty_out(vty, "  Type: %s\n", "L3");
		vty_out(vty, "  Tenant VRF: %s\n", zl3vni_vrf_name(zl3vni));
		vty_out(vty, "  Local Vtep Ip: %s\n",
			inet_ntoa(zl3vni->local_vtep_ip));
		vty_out(vty, "  Vxlan-Intf: %s\n",
			zl3vni_vxlan_if_name(zl3vni));
		vty_out(vty, "  SVI-If: %s\n", zl3vni_svi_if_name(zl3vni));
		vty_out(vty, "  State: %s\n", zl3vni_state2str(zl3vni));
		vty_out(vty, "  VNI Filter: %s\n",
			CHECK_FLAG(zl3vni->filter, PREFIX_ROUTES_ONLY)
				? "prefix-routes-only"
				: "none");
		vty_out(vty, "  System MAC: %s\n",
			zl3vni_sysmac2str(zl3vni, buf, sizeof(buf)));
		vty_out(vty, "  Router MAC: %s\n",
			zl3vni_rmac2str(zl3vni, buf, sizeof(buf)));
		vty_out(vty, "  L2 VNIs: ");
		for (ALL_LIST_ELEMENTS(zl3vni->l2vnis, node, nnode, zvni))
			vty_out(vty, "%u ", zvni->vni);
		vty_out(vty, "\n");
	} else {
		json_vni_list = json_object_new_array();
		json_object_int_add(json, "vni", zl3vni->vni);
		json_object_string_add(json, "type", "L3");
		json_object_string_add(json, "localVtepIp",
				       inet_ntoa(zl3vni->local_vtep_ip));
		json_object_string_add(json, "vxlanIntf",
				       zl3vni_vxlan_if_name(zl3vni));
		json_object_string_add(json, "sviIntf",
				       zl3vni_svi_if_name(zl3vni));
		json_object_string_add(json, "state", zl3vni_state2str(zl3vni));
		json_object_string_add(json, "vrf", zl3vni_vrf_name(zl3vni));
		json_object_string_add(
			json, "sysMac",
			zl3vni_sysmac2str(zl3vni, buf, sizeof(buf)));
		json_object_string_add(
			json, "routerMac",
			zl3vni_rmac2str(zl3vni, buf, sizeof(buf)));
		json_object_string_add(
			json, "vniFilter",
			CHECK_FLAG(zl3vni->filter, PREFIX_ROUTES_ONLY)
				? "prefix-routes-only"
				: "none");
		for (ALL_LIST_ELEMENTS(zl3vni->l2vnis, node, nnode, zvni)) {
			json_object_array_add(json_vni_list,
					      json_object_new_int(zvni->vni));
		}
		json_object_object_add(json, "l2Vnis", json_vni_list);
	}
}

/*
 * Print a specific VNI entry.
 */
static void zvni_print(zebra_vni_t *zvni, void **ctxt)
{
	struct vty *vty;
	zebra_vtep_t *zvtep;
	uint32_t num_macs;
	uint32_t num_neigh;
	json_object *json = NULL;
	json_object *json_vtep_list = NULL;
	json_object *json_ip_str = NULL;

	vty = ctxt[0];
	json = ctxt[1];

	if (json == NULL) {
		vty_out(vty, "VNI: %u\n", zvni->vni);
		vty_out(vty, " Type: %s\n", "L2");
		vty_out(vty, " Tenant VRF: %s\n", vrf_id_to_name(zvni->vrf_id));
	} else {
		json_object_int_add(json, "vni", zvni->vni);
		json_object_string_add(json, "type", "L2");
		json_object_string_add(json, "vrf",
				       vrf_id_to_name(zvni->vrf_id));
	}

	if (!zvni->vxlan_if) { // unexpected
		if (json == NULL)
			vty_out(vty, " VxLAN interface: unknown\n");
		return;
	}
	num_macs = num_valid_macs(zvni);
	num_neigh = hashcount(zvni->neigh_table);
	if (json == NULL) {
		vty_out(vty, " VxLAN interface: %s\n", zvni->vxlan_if->name);
		vty_out(vty, " VxLAN ifIndex: %u\n", zvni->vxlan_if->ifindex);
		vty_out(vty, " Local VTEP IP: %s\n",
			inet_ntoa(zvni->local_vtep_ip));
		vty_out(vty, " Mcast group: %s\n",
				inet_ntoa(zvni->mcast_grp));
	} else {
		json_object_string_add(json, "vxlanInterface",
				       zvni->vxlan_if->name);
		json_object_int_add(json, "ifindex", zvni->vxlan_if->ifindex);
		json_object_string_add(json, "vtepIp",
				       inet_ntoa(zvni->local_vtep_ip));
		json_object_string_add(json, "mcastGroup",
				inet_ntoa(zvni->mcast_grp));
		json_object_string_add(json, "advertiseGatewayMacip",
				       zvni->advertise_gw_macip ? "Yes" : "No");
		json_object_int_add(json, "numMacs", num_macs);
		json_object_int_add(json, "numArpNd", num_neigh);
	}
	if (!zvni->vteps) {
		if (json == NULL)
			vty_out(vty, " No remote VTEPs known for this VNI\n");
	} else {
		if (json == NULL)
			vty_out(vty, " Remote VTEPs for this VNI:\n");
		else
			json_vtep_list = json_object_new_array();
		for (zvtep = zvni->vteps; zvtep; zvtep = zvtep->next) {
			const char *flood_str = lookup_msg(zvtep_flood_str,
					zvtep->flood_control,
					VXLAN_FLOOD_STR_DEFAULT);

			if (json == NULL) {
				vty_out(vty, "  %s flood: %s\n",
						inet_ntoa(zvtep->vtep_ip),
						flood_str);
			} else {
				json_ip_str = json_object_new_string(
						inet_ntoa(zvtep->vtep_ip));
				json_object_array_add(json_vtep_list,
						json_ip_str);
			}
		}
		if (json)
			json_object_object_add(json, "numRemoteVteps",
					       json_vtep_list);
	}
	if (json == NULL) {
		vty_out(vty,
			" Number of MACs (local and remote) known for this VNI: %u\n",
			num_macs);
		vty_out(vty,
			" Number of ARPs (IPv4 and IPv6, local and remote) known for this VNI: %u\n",
			num_neigh);
		vty_out(vty, " Advertise-gw-macip: %s\n",
			zvni->advertise_gw_macip ? "Yes" : "No");
	}
}

/* print a L3 VNI hash entry */
static void zl3vni_print_hash(struct hash_bucket *bucket, void *ctx[])
{
	struct vty *vty = NULL;
	json_object *json = NULL;
	json_object *json_vni = NULL;
	zebra_l3vni_t *zl3vni = NULL;

	vty = (struct vty *)ctx[0];
	json = (json_object *)ctx[1];

	zl3vni = (zebra_l3vni_t *)bucket->data;

	if (!json) {
		vty_out(vty, "%-10u %-4s %-21s %-8lu %-8lu %-15s %-37s\n",
			zl3vni->vni, "L3", zl3vni_vxlan_if_name(zl3vni),
			hashcount(zl3vni->rmac_table),
			hashcount(zl3vni->nh_table), "n/a",
			zl3vni_vrf_name(zl3vni));
	} else {
		char vni_str[VNI_STR_LEN];

		snprintf(vni_str, sizeof(vni_str), "%u", zl3vni->vni);
		json_vni = json_object_new_object();
		json_object_int_add(json_vni, "vni", zl3vni->vni);
		json_object_string_add(json_vni, "vxlanIf",
				       zl3vni_vxlan_if_name(zl3vni));
		json_object_int_add(json_vni, "numMacs",
				    hashcount(zl3vni->rmac_table));
		json_object_int_add(json_vni, "numArpNd",
				    hashcount(zl3vni->nh_table));
		json_object_string_add(json_vni, "numRemoteVteps", "n/a");
		json_object_string_add(json_vni, "type", "L3");
		json_object_string_add(json_vni, "tenantVrf",
				       zl3vni_vrf_name(zl3vni));
		json_object_object_add(json, vni_str, json_vni);
	}
}

/* Private Structure to pass callback data for hash iterator */
struct zvni_evpn_show {
	struct vty *vty;
	json_object *json;
	struct zebra_vrf *zvrf;
	bool use_json;
};

/* print a L3 VNI hash entry in detail*/
static void zl3vni_print_hash_detail(struct hash_bucket *bucket, void *data)
{
	struct vty *vty = NULL;
	zebra_l3vni_t *zl3vni = NULL;
	json_object *json_array = NULL;
	bool use_json = false;
	struct zvni_evpn_show *zes = data;

	vty = zes->vty;
	json_array = zes->json;
	use_json = zes->use_json;

	zl3vni = (zebra_l3vni_t *)bucket->data;

	zebra_vxlan_print_vni(vty, zes->zvrf, zl3vni->vni,
		use_json, json_array);

	if (!use_json)
		vty_out(vty, "\n");
}


/*
 * Print a VNI hash entry - called for display of all VNIs.
 */
static void zvni_print_hash(struct hash_bucket *bucket, void *ctxt[])
{
	struct vty *vty;
	zebra_vni_t *zvni;
	zebra_vtep_t *zvtep;
	uint32_t num_vteps = 0;
	uint32_t num_macs = 0;
	uint32_t num_neigh = 0;
	json_object *json = NULL;
	json_object *json_vni = NULL;
	json_object *json_ip_str = NULL;
	json_object *json_vtep_list = NULL;

	vty = ctxt[0];
	json = ctxt[1];

	zvni = (zebra_vni_t *)bucket->data;

	zvtep = zvni->vteps;
	while (zvtep) {
		num_vteps++;
		zvtep = zvtep->next;
	}

	num_macs = num_valid_macs(zvni);
	num_neigh = hashcount(zvni->neigh_table);
	if (json == NULL)
		vty_out(vty, "%-10u %-4s %-21s %-8u %-8u %-15u %-37s\n",
			zvni->vni, "L2",
			zvni->vxlan_if ? zvni->vxlan_if->name : "unknown",
			num_macs, num_neigh, num_vteps,
			vrf_id_to_name(zvni->vrf_id));
	else {
		char vni_str[VNI_STR_LEN];
		snprintf(vni_str, sizeof(vni_str), "%u", zvni->vni);
		json_vni = json_object_new_object();
		json_object_int_add(json_vni, "vni", zvni->vni);
		json_object_string_add(json_vni, "type", "L2");
		json_object_string_add(json_vni, "vxlanIf",
				       zvni->vxlan_if ? zvni->vxlan_if->name
						      : "unknown");
		json_object_int_add(json_vni, "numMacs", num_macs);
		json_object_int_add(json_vni, "numArpNd", num_neigh);
		json_object_int_add(json_vni, "numRemoteVteps", num_vteps);
		json_object_string_add(json_vni, "tenantVrf",
				       vrf_id_to_name(zvni->vrf_id));
		if (num_vteps) {
			json_vtep_list = json_object_new_array();
			for (zvtep = zvni->vteps; zvtep; zvtep = zvtep->next) {
				json_ip_str = json_object_new_string(
					inet_ntoa(zvtep->vtep_ip));
				json_object_array_add(json_vtep_list,
						      json_ip_str);
			}
			json_object_object_add(json_vni, "remoteVteps",
					       json_vtep_list);
		}
		json_object_object_add(json, vni_str, json_vni);
	}
}

/*
 * Print a VNI hash entry in detail - called for display of all VNIs.
 */
static void zvni_print_hash_detail(struct hash_bucket *bucket, void *data)
{
	struct vty *vty;
	zebra_vni_t *zvni;
	json_object *json_array = NULL;
	bool use_json = false;
	struct zvni_evpn_show *zes = data;

	vty = zes->vty;
	json_array = zes->json;
	use_json = zes->use_json;

	zvni = (zebra_vni_t *)bucket->data;

	zebra_vxlan_print_vni(vty, zes->zvrf, zvni->vni, use_json, json_array);

	if (!use_json)
		vty_out(vty, "\n");
}

/*
 * Inform BGP about local MACIP.
 */
static int zvni_macip_send_msg_to_client(vni_t vni, struct ethaddr *macaddr,
					 struct ipaddr *ip, uint8_t flags,
					 uint32_t seq, int state,
					 struct zebra_evpn_es *es,
					 uint16_t cmd)
{
	char buf[ETHER_ADDR_STRLEN];
	char buf2[INET6_ADDRSTRLEN];
	int ipa_len;
	struct zserv *client = NULL;
	struct stream *s = NULL;
	esi_t *esi = es ? &es->esi : zero_esi;

	client = zserv_find_client(ZEBRA_ROUTE_BGP, 0);
	/* BGP may not be running. */
	if (!client)
		return 0;

	s = stream_new(ZEBRA_MAX_PACKET_SIZ);

	zclient_create_header(s, cmd, zebra_vrf_get_evpn_id());
	stream_putl(s, vni);
	stream_put(s, macaddr->octet, ETH_ALEN);
	if (ip) {
		ipa_len = 0;
		if (IS_IPADDR_V4(ip))
			ipa_len = IPV4_MAX_BYTELEN;
		else if (IS_IPADDR_V6(ip))
			ipa_len = IPV6_MAX_BYTELEN;

		stream_putl(s, ipa_len); /* IP address length */
		if (ipa_len)
			stream_put(s, &ip->ip.addr, ipa_len); /* IP address */
	} else
		stream_putl(s, 0); /* Just MAC. */

	if (cmd == ZEBRA_MACIP_ADD) {
		stream_putc(s, flags); /* sticky mac/gateway mac */
		stream_putl(s, seq); /* sequence number */
		stream_put(s, esi, sizeof(esi_t));
	} else {
		stream_putl(s, state); /* state - active/inactive */
	}


	/* Write packet size. */
	stream_putw_at(s, 0, stream_get_endp(s));

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug(
			"Send MACIP %s f 0x%x MAC %s IP %s seq %u L2-VNI %u ESI %s to %s",
			(cmd == ZEBRA_MACIP_ADD) ? "Add" : "Del", flags,
			prefix_mac2str(macaddr, buf, sizeof(buf)),
			ipaddr2str(ip, buf2, sizeof(buf2)), seq, vni,
			es ? es->esi_str : "-",
			zebra_route_string(client->proto));

	if (cmd == ZEBRA_MACIP_ADD)
		client->macipadd_cnt++;
	else
		client->macipdel_cnt++;

	return zserv_send_message(client, s);
}

/*
 * Make hash key for neighbors.
 */
static unsigned int neigh_hash_keymake(const void *p)
{
	const zebra_neigh_t *n = p;
	const struct ipaddr *ip = &n->ip;

	if (IS_IPADDR_V4(ip))
		return jhash_1word(ip->ipaddr_v4.s_addr, 0);

	return jhash2(ip->ipaddr_v6.s6_addr32,
		      array_size(ip->ipaddr_v6.s6_addr32), 0);
}

/*
 * Compare two neighbor hash structures.
 */
static bool neigh_cmp(const void *p1, const void *p2)
{
	const zebra_neigh_t *n1 = p1;
	const zebra_neigh_t *n2 = p2;

	if (n1 == NULL && n2 == NULL)
		return true;

	if (n1 == NULL || n2 == NULL)
		return false;

	return (memcmp(&n1->ip, &n2->ip, sizeof(struct ipaddr)) == 0);
}

static int neigh_list_cmp(void *p1, void *p2)
{
	const zebra_neigh_t *n1 = p1;
	const zebra_neigh_t *n2 = p2;

	return memcmp(&n1->ip, &n2->ip, sizeof(struct ipaddr));
}

/*
 * Callback to allocate neighbor hash entry.
 */
static void *zvni_neigh_alloc(void *p)
{
	const zebra_neigh_t *tmp_n = p;
	zebra_neigh_t *n;

	n = XCALLOC(MTYPE_NEIGH, sizeof(zebra_neigh_t));
	*n = *tmp_n;

	return ((void *)n);
}

/*
 * Add neighbor entry.
 */
static zebra_neigh_t *zvni_neigh_add(zebra_vni_t *zvni, struct ipaddr *ip,
		struct ethaddr *mac, zebra_mac_t *zmac,
		uint32_t n_flags)
{
	zebra_neigh_t tmp_n;
	zebra_neigh_t *n = NULL;

	memset(&tmp_n, 0, sizeof(zebra_neigh_t));
	memcpy(&tmp_n.ip, ip, sizeof(struct ipaddr));
	n = hash_get(zvni->neigh_table, &tmp_n, zvni_neigh_alloc);
	assert(n);

	n->state = ZEBRA_NEIGH_INACTIVE;
	n->zvni = zvni;
	n->dad_ip_auto_recovery_timer = NULL;
	n->flags = n_flags;

	if (!zmac)
		zmac = zvni_mac_lookup(zvni, mac);
	zebra_vxlan_local_neigh_ref_mac(n, mac,
			zmac, false /* send_mac_update */);

	return n;
}

/*
 * Delete neighbor entry.
 */
static int zvni_neigh_del(zebra_vni_t *zvni, zebra_neigh_t *n)
{
	zebra_neigh_t *tmp_n;

	if (n->mac)
		listnode_delete(n->mac->neigh_list, n);

	/* Cancel auto recovery */
	THREAD_OFF(n->dad_ip_auto_recovery_timer);

	/* Free the VNI hash entry and allocated memory. */
	tmp_n = hash_release(zvni->neigh_table, n);
	XFREE(MTYPE_NEIGH, tmp_n);

	return 0;
}

/*
 * Free neighbor hash entry (callback)
 */
static void zvni_neigh_del_hash_entry(struct hash_bucket *bucket, void *arg)
{
	struct neigh_walk_ctx *wctx = arg;
	zebra_neigh_t *n = bucket->data;

	if (((wctx->flags & DEL_LOCAL_NEIGH) && (n->flags & ZEBRA_NEIGH_LOCAL))
	    || ((wctx->flags & DEL_REMOTE_NEIGH)
		&& (n->flags & ZEBRA_NEIGH_REMOTE))
	    || ((wctx->flags & DEL_REMOTE_NEIGH_FROM_VTEP)
		&& (n->flags & ZEBRA_NEIGH_REMOTE)
		&& IPV4_ADDR_SAME(&n->r_vtep_ip, &wctx->r_vtep_ip))) {
		if (wctx->upd_client && (n->flags & ZEBRA_NEIGH_LOCAL))
			zvni_neigh_send_del_to_client(wctx->zvni->vni, &n->ip,
					&n->emac, n->flags, n->state,
					false /*force*/);

		if (wctx->uninstall) {
			if (zebra_vxlan_neigh_is_static(n))
				zebra_vxlan_sync_neigh_dp_install(n,
					false /* set_inactive */,
					true /* force_clear_static */,
					__func__);
			if ((n->flags & ZEBRA_NEIGH_REMOTE))
				zvni_neigh_uninstall(wctx->zvni, n);
		}

		zvni_neigh_del(wctx->zvni, n);
	}

	return;
}

/*
 * Delete all neighbor entries for this VNI.
 */
static void zvni_neigh_del_all(zebra_vni_t *zvni, int uninstall, int upd_client,
			       uint32_t flags)
{
	struct neigh_walk_ctx wctx;

	if (!zvni->neigh_table)
		return;

	memset(&wctx, 0, sizeof(struct neigh_walk_ctx));
	wctx.zvni = zvni;
	wctx.uninstall = uninstall;
	wctx.upd_client = upd_client;
	wctx.flags = flags;

	hash_iterate(zvni->neigh_table, zvni_neigh_del_hash_entry, &wctx);
}

/*
 * Look up neighbor hash entry.
 */
static zebra_neigh_t *zvni_neigh_lookup(zebra_vni_t *zvni, struct ipaddr *ip)
{
	zebra_neigh_t tmp;
	zebra_neigh_t *n;

	memset(&tmp, 0, sizeof(tmp));
	memcpy(&tmp.ip, ip, sizeof(struct ipaddr));
	n = hash_lookup(zvni->neigh_table, &tmp);

	return n;
}

/*
 * Process all neighbors associated with a MAC upon the MAC being learnt
 * locally or undergoing any other change (such as sequence number).
 */
static void zvni_process_neigh_on_local_mac_change(zebra_vni_t *zvni,
		zebra_mac_t *zmac, bool seq_change, bool es_change)
{
	zebra_neigh_t *n = NULL;
	struct listnode *node = NULL;
	struct zebra_vrf *zvrf = NULL;
	char buf[ETHER_ADDR_STRLEN];

	zvrf = vrf_info_lookup(zvni->vxlan_if->vrf_id);

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("Processing neighbors on local MAC %s %s, VNI %u",
			   prefix_mac2str(&zmac->macaddr, buf, sizeof(buf)),
			   seq_change ? "CHANGE" : "ADD", zvni->vni);

	/* Walk all neighbors and mark any inactive local neighbors as
	 * active and/or update sequence number upon a move, and inform BGP.
	 * The action for remote neighbors is TBD.
	 * NOTE: We can't simply uninstall remote neighbors as the kernel may
	 * accidentally end up deleting a just-learnt local neighbor.
	 */
	for (ALL_LIST_ELEMENTS_RO(zmac->neigh_list, node, n)) {
		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL)) {
			if (IS_ZEBRA_NEIGH_INACTIVE(n) || seq_change ||
					es_change) {
				ZEBRA_NEIGH_SET_ACTIVE(n);
				n->loc_seq = zmac->loc_seq;
				if (!(zvrf->dup_addr_detect &&
				      zvrf->dad_freeze && !!CHECK_FLAG(n->flags,
						ZEBRA_NEIGH_DUPLICATE)))
					zvni_neigh_send_add_to_client(
						zvni->vni, &n->ip, &n->emac,
						n->mac, n->flags, n->loc_seq);
			}
		}
	}
}

/*
 * Process all neighbors associated with a local MAC upon the MAC being
 * deleted.
 */
static void zvni_process_neigh_on_local_mac_del(zebra_vni_t *zvni,
						zebra_mac_t *zmac)
{
	zebra_neigh_t *n = NULL;
	struct listnode *node = NULL;
	char buf[ETHER_ADDR_STRLEN];

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("Processing neighbors on local MAC %s DEL, VNI %u",
			   prefix_mac2str(&zmac->macaddr, buf, sizeof(buf)),
			   zvni->vni);

	/* Walk all local neighbors and mark as inactive and inform
	 * BGP, if needed.
	 * TBD: There is currently no handling for remote neighbors. We
	 * don't expect them to exist, if they do, do we install the MAC
	 * as a remote MAC and the neighbor as remote?
	 */
	for (ALL_LIST_ELEMENTS_RO(zmac->neigh_list, node, n)) {
		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL)) {
			if (IS_ZEBRA_NEIGH_ACTIVE(n)) {
				ZEBRA_NEIGH_SET_INACTIVE(n);
				n->loc_seq = 0;
				zvni_neigh_send_del_to_client(zvni->vni, &n->ip,
						&n->emac, n->flags,
						ZEBRA_NEIGH_ACTIVE,
						false /*force*/);
			}
		}
	}
}

/*
 * Process all neighbors associated with a MAC upon the MAC being remotely
 * learnt.
 */
static void zvni_process_neigh_on_remote_mac_add(zebra_vni_t *zvni,
						 zebra_mac_t *zmac)
{
	zebra_neigh_t *n = NULL;
	struct listnode *node = NULL;
	char buf[ETHER_ADDR_STRLEN];

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("Processing neighbors on remote MAC %s ADD, VNI %u",
			   prefix_mac2str(&zmac->macaddr, buf, sizeof(buf)),
			   zvni->vni);

	/* Walk all local neighbors and mark as inactive and inform
	 * BGP, if needed.
	 */
	for (ALL_LIST_ELEMENTS_RO(zmac->neigh_list, node, n)) {
		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL)) {
			if (IS_ZEBRA_NEIGH_ACTIVE(n)) {
				ZEBRA_NEIGH_SET_INACTIVE(n);
				n->loc_seq = 0;
				zvni_neigh_send_del_to_client(zvni->vni, &n->ip,
						&n->emac, n->flags,
						ZEBRA_NEIGH_ACTIVE,
						false /* force */);
			}
		}
	}
}

/*
 * Process all neighbors associated with a remote MAC upon the MAC being
 * deleted.
 */
static void zvni_process_neigh_on_remote_mac_del(zebra_vni_t *zvni,
						 zebra_mac_t *zmac)
{
	/* NOTE: Currently a NO-OP. */
}

static void zvni_probe_neigh_on_mac_add(zebra_vni_t *zvni, zebra_mac_t *zmac)
{
	zebra_neigh_t *nbr = NULL;
	struct listnode *node = NULL;

	for (ALL_LIST_ELEMENTS_RO(zmac->neigh_list, node, nbr)) {
		if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_LOCAL) &&
		    IS_ZEBRA_NEIGH_INACTIVE(nbr))
			zvni_neigh_probe(zvni, nbr);
	}
}

/*
 * Inform BGP about local neighbor addition.
 */
static int zvni_neigh_send_add_to_client(vni_t vni, struct ipaddr *ip,
					 struct ethaddr *macaddr,
					 zebra_mac_t *zmac,
					 uint32_t neigh_flags,
					 uint32_t seq)
{
	uint8_t flags = 0;

	if (CHECK_FLAG(neigh_flags, ZEBRA_NEIGH_LOCAL_INACTIVE)) {
		/* host reachability has not been verified locally */

		/* if no ES peer is claiming reachability we can't advertise
		 * the entry
		 */
		if (!CHECK_FLAG(neigh_flags, ZEBRA_NEIGH_ES_PEER_ACTIVE))
			return 0;

		/* ES peers are claiming reachability; we will
		 * advertise the entry but with a proxy flag
		 */
		SET_FLAG(flags, ZEBRA_MACIP_TYPE_PROXY_ADVERT);
	}

	if (CHECK_FLAG(neigh_flags, ZEBRA_NEIGH_DEF_GW))
		SET_FLAG(flags, ZEBRA_MACIP_TYPE_GW);
	/* Set router flag (R-bit) based on local neigh entry add */
	if (CHECK_FLAG(neigh_flags, ZEBRA_NEIGH_ROUTER_FLAG))
		SET_FLAG(flags, ZEBRA_MACIP_TYPE_ROUTER_FLAG);
	if (CHECK_FLAG(neigh_flags, ZEBRA_NEIGH_SVI_IP))
		SET_FLAG(flags, ZEBRA_MACIP_TYPE_SVI_IP);

	return zvni_macip_send_msg_to_client(vni, macaddr, ip, flags,
			seq, ZEBRA_NEIGH_ACTIVE,
			zmac ? zmac->es : NULL,
			ZEBRA_MACIP_ADD);
}

/*
 * Inform BGP about local neighbor deletion.
 */
static int zvni_neigh_send_del_to_client(vni_t vni, struct ipaddr *ip,
		struct ethaddr *macaddr, uint32_t flags,
		int state, bool force)
{
	if (!force) {
		if (CHECK_FLAG(flags, ZEBRA_NEIGH_LOCAL_INACTIVE) &&
			!CHECK_FLAG(flags, ZEBRA_NEIGH_ES_PEER_ACTIVE))
			/* the neigh was not advertised - nothing  to delete */
			return 0;
	}

	return zvni_macip_send_msg_to_client(vni, macaddr, ip, flags,
			0, state, NULL, ZEBRA_MACIP_DEL);
}

/*
 * Install remote neighbor into the kernel.
 */
static int zvni_rem_neigh_install(zebra_vni_t *zvni, zebra_neigh_t *n,
		bool was_static)
{
	struct zebra_if *zif;
	struct zebra_l2info_vxlan *vxl;
	struct interface *vlan_if;
	int flags;
	int ret = 0;

	if (!(n->flags & ZEBRA_NEIGH_REMOTE))
		return 0;

	zif = zvni->vxlan_if->info;
	if (!zif)
		return -1;
	vxl = &zif->l2info.vxl;

	vlan_if = zvni_map_to_svi(vxl->access_vlan, zif->brslave_info.br_if);
	if (!vlan_if)
		return -1;

	flags = DPLANE_NTF_EXT_LEARNED;
	if (n->flags & ZEBRA_NEIGH_ROUTER_FLAG)
		flags |= DPLANE_NTF_ROUTER;
	ZEBRA_NEIGH_SET_ACTIVE(n);

	dplane_rem_neigh_add(vlan_if, &n->ip, &n->emac, flags,
			was_static);

	return ret;
}

/*
 * Uninstall remote neighbor from the kernel.
 */
static int zvni_neigh_uninstall(zebra_vni_t *zvni, zebra_neigh_t *n)
{
	struct zebra_if *zif;
	struct zebra_l2info_vxlan *vxl;
	struct interface *vlan_if;

	if (!(n->flags & ZEBRA_NEIGH_REMOTE))
		return 0;

	if (!zvni->vxlan_if) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("VNI %u hash %p couldn't be uninstalled - no intf",
				   zvni->vni, zvni);
		return -1;
	}

	zif = zvni->vxlan_if->info;
	if (!zif)
		return -1;
	vxl = &zif->l2info.vxl;
	vlan_if = zvni_map_to_svi(vxl->access_vlan, zif->brslave_info.br_if);
	if (!vlan_if)
		return -1;

	ZEBRA_NEIGH_SET_INACTIVE(n);
	n->loc_seq = 0;

	dplane_rem_neigh_delete(vlan_if, &n->ip);

	return 0;
}

/*
 * Probe neighbor from the kernel.
 */
static int zvni_neigh_probe(zebra_vni_t *zvni, zebra_neigh_t *n)
{
	struct zebra_if *zif;
	struct zebra_l2info_vxlan *vxl;
	struct interface *vlan_if;

	zif = zvni->vxlan_if->info;
	if (!zif)
		return -1;
	vxl = &zif->l2info.vxl;

	vlan_if = zvni_map_to_svi(vxl->access_vlan, zif->brslave_info.br_if);
	if (!vlan_if)
		return -1;

	dplane_rem_neigh_update(vlan_if, &n->ip, &n->emac);

	return 0;
}

/*
 * Install neighbor hash entry - called upon access VLAN change.
 */
static void zvni_install_neigh_hash(struct hash_bucket *bucket, void *ctxt)
{
	zebra_neigh_t *n;
	struct neigh_walk_ctx *wctx = ctxt;

	n = (zebra_neigh_t *)bucket->data;

	if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_REMOTE))
		zvni_rem_neigh_install(wctx->zvni, n, false /*was_static*/);
}

/* Get the VRR interface for SVI if any */
struct interface *zebra_get_vrr_intf_for_svi(struct interface *ifp)
{
	struct zebra_vrf *zvrf = NULL;
	struct interface *tmp_if = NULL;
	struct zebra_if *zif = NULL;

	zvrf = vrf_info_lookup(ifp->vrf_id);
	assert(zvrf);

	FOR_ALL_INTERFACES (zvrf->vrf, tmp_if) {
		zif = tmp_if->info;
		if (!zif)
			continue;

		if (!IS_ZEBRA_IF_MACVLAN(tmp_if))
			continue;

		if (zif->link == ifp)
			return tmp_if;
	}

	return NULL;
}

static int zvni_del_macip_for_intf(struct interface *ifp, zebra_vni_t *zvni)
{
	struct listnode *cnode = NULL, *cnnode = NULL;
	struct connected *c = NULL;
	struct ethaddr macaddr;

	memcpy(&macaddr.octet, ifp->hw_addr, ETH_ALEN);

	for (ALL_LIST_ELEMENTS(ifp->connected, cnode, cnnode, c)) {
		struct ipaddr ip;

		memset(&ip, 0, sizeof(struct ipaddr));
		if (!CHECK_FLAG(c->conf, ZEBRA_IFC_REAL))
			continue;

		if (c->address->family == AF_INET) {
			ip.ipa_type = IPADDR_V4;
			memcpy(&(ip.ipaddr_v4), &(c->address->u.prefix4),
			       sizeof(struct in_addr));
		} else if (c->address->family == AF_INET6) {
			ip.ipa_type = IPADDR_V6;
			memcpy(&(ip.ipaddr_v6), &(c->address->u.prefix6),
			       sizeof(struct in6_addr));
		} else {
			continue;
		}

		zvni_gw_macip_del(ifp, zvni, &ip);
	}

	return 0;
}

static int zvni_add_macip_for_intf(struct interface *ifp, zebra_vni_t *zvni)
{
	struct listnode *cnode = NULL, *cnnode = NULL;
	struct connected *c = NULL;
	struct ethaddr macaddr;

	memcpy(&macaddr.octet, ifp->hw_addr, ETH_ALEN);

	for (ALL_LIST_ELEMENTS(ifp->connected, cnode, cnnode, c)) {
		struct ipaddr ip;

		memset(&ip, 0, sizeof(struct ipaddr));
		if (!CHECK_FLAG(c->conf, ZEBRA_IFC_REAL))
			continue;

		if (c->address->family == AF_INET) {
			ip.ipa_type = IPADDR_V4;
			memcpy(&(ip.ipaddr_v4), &(c->address->u.prefix4),
			       sizeof(struct in_addr));
		} else if (c->address->family == AF_INET6) {
			ip.ipa_type = IPADDR_V6;
			memcpy(&(ip.ipaddr_v6), &(c->address->u.prefix6),
			       sizeof(struct in6_addr));
		} else {
			continue;
		}

		zvni_gw_macip_add(ifp, zvni, &macaddr, &ip);
	}
	return 0;
}


static int zvni_advertise_subnet(zebra_vni_t *zvni, struct interface *ifp,
				 int advertise)
{
	struct listnode *cnode = NULL, *cnnode = NULL;
	struct connected *c = NULL;
	struct ethaddr macaddr;

	memcpy(&macaddr.octet, ifp->hw_addr, ETH_ALEN);

	for (ALL_LIST_ELEMENTS(ifp->connected, cnode, cnnode, c)) {
		struct prefix p;

		memcpy(&p, c->address, sizeof(struct prefix));

		/* skip link local address */
		if (IN6_IS_ADDR_LINKLOCAL(&p.u.prefix6))
			continue;

		apply_mask(&p);
		if (advertise)
			ip_prefix_send_to_client(ifp->vrf_id, &p,
						 ZEBRA_IP_PREFIX_ROUTE_ADD);
		else
			ip_prefix_send_to_client(ifp->vrf_id, &p,
						 ZEBRA_IP_PREFIX_ROUTE_DEL);
	}
	return 0;
}

/*
 * zvni_gw_macip_add_to_client
 */
static int zvni_gw_macip_add(struct interface *ifp, zebra_vni_t *zvni,
			     struct ethaddr *macaddr, struct ipaddr *ip)
{
	char buf[ETHER_ADDR_STRLEN];
	char buf2[INET6_ADDRSTRLEN];
	zebra_neigh_t *n = NULL;
	zebra_mac_t *mac = NULL;
	struct zebra_if *zif = NULL;
	struct zebra_l2info_vxlan *vxl = NULL;

	zif = zvni->vxlan_if->info;
	if (!zif)
		return -1;

	vxl = &zif->l2info.vxl;

	mac = zvni_mac_lookup(zvni, macaddr);
	if (!mac) {
		mac = zvni_mac_add(zvni, macaddr);
		if (!mac) {
			flog_err(EC_ZEBRA_MAC_ADD_FAILED,
				 "Failed to add MAC %s intf %s(%u) VID %u",
				 prefix_mac2str(macaddr, buf, sizeof(buf)),
				 ifp->name, ifp->ifindex, vxl->access_vlan);
			return -1;
		}
	}

	/* Set "local" forwarding info. */
	SET_FLAG(mac->flags, ZEBRA_MAC_LOCAL);
	SET_FLAG(mac->flags, ZEBRA_MAC_AUTO);
	SET_FLAG(mac->flags, ZEBRA_MAC_DEF_GW);
	memset(&mac->fwd_info, 0, sizeof(mac->fwd_info));
	mac->fwd_info.local.ifindex = ifp->ifindex;
	mac->fwd_info.local.vid = vxl->access_vlan;

	n = zvni_neigh_lookup(zvni, ip);
	if (!n) {
		n = zvni_neigh_add(zvni, ip, macaddr, mac, 0);
		if (!n) {
			flog_err(
				EC_ZEBRA_MAC_ADD_FAILED,
				"Failed to add neighbor %s MAC %s intf %s(%u) -> VNI %u",
				ipaddr2str(ip, buf2, sizeof(buf2)),
				prefix_mac2str(macaddr, buf, sizeof(buf)),
				ifp->name, ifp->ifindex, zvni->vni);
			return -1;
		}
	}

	/* Set "local" forwarding info. */
	SET_FLAG(n->flags, ZEBRA_NEIGH_LOCAL);
	ZEBRA_NEIGH_SET_ACTIVE(n);
	memcpy(&n->emac, macaddr, ETH_ALEN);
	n->ifindex = ifp->ifindex;

	/* Only advertise in BGP if the knob is enabled */
	if (advertise_gw_macip_enabled(zvni)) {

		SET_FLAG(mac->flags, ZEBRA_MAC_DEF_GW);
		SET_FLAG(n->flags, ZEBRA_NEIGH_DEF_GW);
		/* Set Router flag (R-bit) */
		if (ip->ipa_type == IPADDR_V6)
			SET_FLAG(n->flags, ZEBRA_NEIGH_ROUTER_FLAG);

		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
			"SVI %s(%u) L2-VNI %u, sending GW MAC %s IP %s add to BGP with flags 0x%x",
			ifp->name, ifp->ifindex, zvni->vni,
			prefix_mac2str(macaddr, buf, sizeof(buf)),
			ipaddr2str(ip, buf2, sizeof(buf2)), n->flags);

		zvni_neigh_send_add_to_client(zvni->vni, ip, &n->emac, n->mac,
					      n->flags, n->loc_seq);
	} else if (advertise_svi_macip_enabled(zvni)) {

		SET_FLAG(n->flags, ZEBRA_NEIGH_SVI_IP);
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
			"SVI %s(%u) L2-VNI %u, sending SVI MAC %s IP %s add to BGP with flags 0x%x",
			ifp->name, ifp->ifindex, zvni->vni,
			prefix_mac2str(macaddr, buf, sizeof(buf)),
			ipaddr2str(ip, buf2, sizeof(buf2)), n->flags);

		zvni_neigh_send_add_to_client(zvni->vni, ip, &n->emac, n->mac,
					      n->flags, n->loc_seq);
	}

	return 0;
}

/*
 * zvni_gw_macip_del_from_client
 */
static int zvni_gw_macip_del(struct interface *ifp, zebra_vni_t *zvni,
			     struct ipaddr *ip)
{
	char buf1[ETHER_ADDR_STRLEN];
	char buf2[INET6_ADDRSTRLEN];
	zebra_neigh_t *n = NULL;
	zebra_mac_t *mac = NULL;

	/* If the neigh entry is not present nothing to do*/
	n = zvni_neigh_lookup(zvni, ip);
	if (!n)
		return 0;

	/* mac entry should be present */
	mac = zvni_mac_lookup(zvni, &n->emac);
	if (!mac) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("MAC %s doesn't exist for neigh %s on VNI %u",
				   prefix_mac2str(&n->emac,
						  buf1, sizeof(buf1)),
				   ipaddr2str(ip, buf2, sizeof(buf2)),
				   zvni->vni);
		return -1;
	}

	/* If the entry is not local nothing to do*/
	if (!CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL))
		return -1;

	/* only need to delete the entry from bgp if we sent it before */
	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug(
			"%u:SVI %s(%u) VNI %u, sending GW MAC %s IP %s del to BGP",
			ifp->vrf_id, ifp->name, ifp->ifindex, zvni->vni,
			prefix_mac2str(&(n->emac), buf1, sizeof(buf1)),
			ipaddr2str(ip, buf2, sizeof(buf2)));

	/* Remove neighbor from BGP. */
	zvni_neigh_send_del_to_client(zvni->vni, &n->ip, &n->emac,
				      n->flags, ZEBRA_NEIGH_ACTIVE,
				      false /*force*/);

	/* Delete this neighbor entry. */
	zvni_neigh_del(zvni, n);

	/* see if the mac needs to be deleted as well*/
	if (mac)
		zvni_deref_ip2mac(zvni, mac);

	return 0;
}

static void zvni_gw_macip_del_for_vni_hash(struct hash_bucket *bucket,
					   void *ctxt)
{
	zebra_vni_t *zvni = NULL;
	struct zebra_if *zif = NULL;
	struct zebra_l2info_vxlan zl2_info;
	struct interface *vlan_if = NULL;
	struct interface *vrr_if = NULL;
	struct interface *ifp;

	/* Add primary SVI MAC*/
	zvni = (zebra_vni_t *)bucket->data;

	/* Global (Zvrf) advertise-default-gw is disabled,
	 * but zvni advertise-default-gw is enabled
	 */
	if (zvni->advertise_gw_macip) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("VNI: %u GW-MACIP enabled, retain gw-macip",
				   zvni->vni);
		return;
	}

	ifp = zvni->vxlan_if;
	if (!ifp)
		return;
	zif = ifp->info;

	/* If down or not mapped to a bridge, we're done. */
	if (!if_is_operative(ifp) || !zif->brslave_info.br_if)
		return;

	zl2_info = zif->l2info.vxl;

	vlan_if =
		zvni_map_to_svi(zl2_info.access_vlan, zif->brslave_info.br_if);
	if (!vlan_if)
		return;

	/* Del primary MAC-IP */
	zvni_del_macip_for_intf(vlan_if, zvni);

	/* Del VRR MAC-IP - if any*/
	vrr_if = zebra_get_vrr_intf_for_svi(vlan_if);
	if (vrr_if)
		zvni_del_macip_for_intf(vrr_if, zvni);

	return;
}

static void zvni_gw_macip_add_for_vni_hash(struct hash_bucket *bucket,
					   void *ctxt)
{
	zebra_vni_t *zvni = NULL;
	struct zebra_if *zif = NULL;
	struct zebra_l2info_vxlan zl2_info;
	struct interface *vlan_if = NULL;
	struct interface *vrr_if = NULL;
	struct interface *ifp = NULL;

	zvni = (zebra_vni_t *)bucket->data;

	ifp = zvni->vxlan_if;
	if (!ifp)
		return;
	zif = ifp->info;

	/* If down or not mapped to a bridge, we're done. */
	if (!if_is_operative(ifp) || !zif->brslave_info.br_if)
		return;
	zl2_info = zif->l2info.vxl;

	vlan_if =
		zvni_map_to_svi(zl2_info.access_vlan, zif->brslave_info.br_if);
	if (!vlan_if)
		return;

	/* Add primary SVI MAC-IP */
	zvni_add_macip_for_intf(vlan_if, zvni);

	if (advertise_gw_macip_enabled(zvni)) {
		/* Add VRR MAC-IP - if any*/
		vrr_if = zebra_get_vrr_intf_for_svi(vlan_if);
		if (vrr_if)
			zvni_add_macip_for_intf(vrr_if, zvni);
	}

	return;
}

static void zvni_svi_macip_del_for_vni_hash(struct hash_bucket *bucket,
					   void *ctxt)
{
	zebra_vni_t *zvni = NULL;
	struct zebra_if *zif = NULL;
	struct zebra_l2info_vxlan zl2_info;
	struct interface *vlan_if = NULL;
	struct interface *ifp;

	/* Add primary SVI MAC*/
	zvni = (zebra_vni_t *)bucket->data;
	if (!zvni)
		return;

	/* Global(vrf) advertise-svi-ip disabled, but zvni advertise-svi-ip
	 * enabled
	 */
	if (zvni->advertise_svi_macip) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("VNI: %u SVI-MACIP enabled, retain svi-macip",
				   zvni->vni);
		return;
	}

	ifp = zvni->vxlan_if;
	if (!ifp)
		return;
	zif = ifp->info;

	/* If down or not mapped to a bridge, we're done. */
	if (!if_is_operative(ifp) || !zif->brslave_info.br_if)
		return;

	zl2_info = zif->l2info.vxl;

	vlan_if = zvni_map_to_svi(zl2_info.access_vlan,
				  zif->brslave_info.br_if);
	if (!vlan_if)
		return;

	/* Del primary MAC-IP */
	zvni_del_macip_for_intf(vlan_if, zvni);

	return;
}

static inline void zvni_local_neigh_update_log(const char *pfx,
		zebra_neigh_t *n, bool is_router, bool local_inactive,
		bool old_bgp_ready, bool new_bgp_ready,
		bool inform_dataplane, bool inform_bgp, const char *sfx)
{
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];

	if (!IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
		return;

	zlog_debug("%s neigh vni %u ip %s mac %s f 0x%x%s%s%s%s%s%s %s",
		pfx, n->zvni->vni,
		ipaddr2str(&n->ip, ipbuf, sizeof(ipbuf)),
		prefix_mac2str(&n->emac, macbuf, sizeof(macbuf)),
		n->flags, is_router ? " router" : "",
		local_inactive ? " local-inactive" : "",
		old_bgp_ready ?  " old_bgp_ready" : "",
		new_bgp_ready ?  " new_bgp_ready" : "",
		inform_dataplane ?  " inform_dp" : "",
		inform_bgp ?  " inform_bgp" : "",
		sfx);
}

static int zvni_local_neigh_update(zebra_vni_t *zvni,
				   struct interface *ifp,
				   struct ipaddr *ip,
				   struct ethaddr *macaddr,
				   bool is_router,
				   bool local_inactive, bool dp_static)
{
	char buf[ETHER_ADDR_STRLEN];
	char buf2[INET6_ADDRSTRLEN];
	struct zebra_vrf *zvrf;
	zebra_neigh_t *n = NULL;
	zebra_mac_t *zmac = NULL, *old_zmac = NULL;
	uint32_t old_mac_seq = 0, mac_new_seq = 0;
	bool upd_mac_seq = false;
	bool neigh_mac_change = false;
	bool neigh_on_hold = false;
	bool neigh_was_remote = false;
	bool do_dad = false;
	struct in_addr vtep_ip = {.s_addr = 0};
	bool inform_dataplane = false;
	bool created = false;
	bool new_static = false;
	bool old_bgp_ready = false;
	bool new_bgp_ready;

	/* Check if the MAC exists. */
	zmac = zvni_mac_lookup(zvni, macaddr);
	if (!zmac) {
		/* create a dummy MAC if the MAC is not already present */
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"AUTO MAC %s created for neigh %s on VNI %u",
				prefix_mac2str(macaddr, buf, sizeof(buf)),
				ipaddr2str(ip, buf2, sizeof(buf2)), zvni->vni);

		zmac = zvni_mac_add(zvni, macaddr);
		if (!zmac) {
			zlog_debug("Failed to add MAC %s VNI %u",
				   prefix_mac2str(macaddr, buf, sizeof(buf)),
				   zvni->vni);
			return -1;
		}

		memset(&zmac->fwd_info, 0, sizeof(zmac->fwd_info));
		memset(&zmac->flags, 0, sizeof(uint32_t));
		SET_FLAG(zmac->flags, ZEBRA_MAC_AUTO);
	} else {
		if (CHECK_FLAG(zmac->flags, ZEBRA_MAC_REMOTE)) {
			/*
			 * We don't change the MAC to local upon a neighbor
			 * learn event, we wait for the explicit local MAC
			 * learn. However, we have to compute its sequence
			 * number in preparation for when it actually turns
			 * local.
			 */
			upd_mac_seq = true;
		}
	}

	zvrf = vrf_info_lookup(zvni->vxlan_if->vrf_id);
	if (!zvrf) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("        Unable to find vrf for: %d",
				   zvni->vxlan_if->vrf_id);
		return -1;
	}

	/* Check if the neighbor exists. */
	n = zvni_neigh_lookup(zvni, ip);
	if (!n) {
		/* New neighbor - create */
		n = zvni_neigh_add(zvni, ip, macaddr, zmac, 0);
		if (!n) {
			flog_err(
				EC_ZEBRA_MAC_ADD_FAILED,
				"Failed to add neighbor %s MAC %s intf %s(%u) -> VNI %u",
				ipaddr2str(ip, buf2, sizeof(buf2)),
				prefix_mac2str(macaddr, buf, sizeof(buf)),
				ifp->name, ifp->ifindex, zvni->vni);
			return -1;
		}
		/* Set "local" forwarding info. */
		SET_FLAG(n->flags, ZEBRA_NEIGH_LOCAL);
		n->ifindex = ifp->ifindex;
		created = true;
	} else {
		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL)) {
			bool mac_different;
			bool cur_is_router;
			bool old_local_inactive;
 
			old_local_inactive = !!CHECK_FLAG(n->flags,
					ZEBRA_NEIGH_LOCAL_INACTIVE);

			old_bgp_ready =
				zebra_vxlan_neigh_is_ready_for_bgp(n);

			/* Note any changes and see if of interest to BGP. */
			mac_different = !!memcmp(&n->emac,
					macaddr, ETH_ALEN);
			cur_is_router = !!CHECK_FLAG(n->flags,
						     ZEBRA_NEIGH_ROUTER_FLAG);
			new_static = zebra_vxlan_neigh_is_static(n);
			if (!mac_different && is_router == cur_is_router &&
					old_local_inactive == local_inactive &&
					dp_static != new_static) {
				if (IS_ZEBRA_DEBUG_VXLAN)
					zlog_debug(
						"        Ignoring entry mac is the same and is_router == cur_is_router");
				n->ifindex = ifp->ifindex;
				return 0;
			}

			old_zmac = n->mac;
			if (!mac_different) {
				/* XXX - cleanup this code duplication */
				bool is_neigh_freezed = false;

				/* Only the router flag has changed. */
				if (is_router)
					SET_FLAG(n->flags,
						ZEBRA_NEIGH_ROUTER_FLAG);
				else
					UNSET_FLAG(n->flags,
						ZEBRA_NEIGH_ROUTER_FLAG);

				if (local_inactive)
					SET_FLAG(n->flags,
						ZEBRA_NEIGH_LOCAL_INACTIVE);
				else
					UNSET_FLAG(n->flags,
						ZEBRA_NEIGH_LOCAL_INACTIVE);
				new_bgp_ready =
					zebra_vxlan_neigh_is_ready_for_bgp(n);

				/* Neigh is in freeze state and freeze action
				 * is enabled, do not send update to client.
				 */
				is_neigh_freezed = (zvrf->dup_addr_detect &&
						    zvrf->dad_freeze &&
						    CHECK_FLAG(n->flags,
							ZEBRA_NEIGH_DUPLICATE));

				zvni_local_neigh_update_log("local", n,
					is_router, local_inactive,
					old_bgp_ready, new_bgp_ready,
					false, false, "flag-update");

				/* if the neigh can no longer be advertised
				 * remove it from bgp
				 */
				if (!is_neigh_freezed) {
					zebra_vxlan_neigh_send_add_del_to_client(
						n, old_bgp_ready, new_bgp_ready);
				} else {
					if (IS_ZEBRA_DEBUG_VXLAN &&
						IS_ZEBRA_NEIGH_ACTIVE(n))
						zlog_debug(
							"        Neighbor active and frozen");
				}
				return 0;
			}

			/* The MAC has changed, need to issue a delete
			 * first as this means a different MACIP route.
			 * Also, need to do some unlinking/relinking.
			 * We also need to update the MAC's sequence number
			 * in different situations.
			 */
			if (old_bgp_ready) {
				zvni_neigh_send_del_to_client(zvni->vni, &n->ip,
					      &n->emac, n->flags, n->state,
					      false /*force*/);
				old_bgp_ready = false;
			}
			if (old_zmac) {
				old_mac_seq = CHECK_FLAG(old_zmac->flags,
							 ZEBRA_MAC_REMOTE) ?
					old_zmac->rem_seq : old_zmac->loc_seq;
				neigh_mac_change = upd_mac_seq = true;
				zebra_vxlan_local_neigh_deref_mac(n,
						true /* send_mac_update */);
			}

			/* if mac changes abandon peer flags and tell
			 * dataplane to clear the static flag
			 */
			if (zebra_vxlan_neigh_clear_sync_info(n))
				inform_dataplane = true;
			/* Update the forwarding info. */
			n->ifindex = ifp->ifindex;

			/* Link to new MAC */
			zebra_vxlan_local_neigh_ref_mac(n, macaddr, zmac,
					true /* send_mac_update */);
		} else if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_REMOTE)) {
			/*
			 * Neighbor has moved from remote to local. Its
			 * MAC could have also changed as part of the move.
			 */
			if (memcmp(n->emac.octet, macaddr->octet,
				   ETH_ALEN) != 0) {
				old_zmac = n->mac;
				if (old_zmac) {
					old_mac_seq = CHECK_FLAG(
							old_zmac->flags,
							ZEBRA_MAC_REMOTE) ?
							old_zmac->rem_seq :
							old_zmac->loc_seq;
					neigh_mac_change = upd_mac_seq = true;
					zebra_vxlan_local_neigh_deref_mac(n,
							true /* send_update */);
				}

				/* Link to new MAC */
				zebra_vxlan_local_neigh_ref_mac(n, macaddr,
					zmac, true /*send_update*/);
			}
			/* Based on Mobility event Scenario-B from the
			 * draft, neigh's previous state was remote treat this
			 * event for DAD.
			 */
			neigh_was_remote = true;
			vtep_ip = n->r_vtep_ip;
			/* Mark appropriately */
			UNSET_FLAG(n->flags, ZEBRA_NEIGH_REMOTE);
			n->r_vtep_ip.s_addr = INADDR_ANY;
			SET_FLAG(n->flags, ZEBRA_NEIGH_LOCAL);
			n->ifindex = ifp->ifindex;
		}
	}

	/* If MAC was previously remote, or the neighbor had a different
	 * MAC earlier, recompute the sequence number.
	 */
	if (upd_mac_seq) {
		uint32_t seq1, seq2;

		seq1 = CHECK_FLAG(zmac->flags, ZEBRA_MAC_REMOTE) ?
		       zmac->rem_seq + 1 : zmac->loc_seq;
		seq2 = neigh_mac_change ? old_mac_seq + 1 : 0;
		mac_new_seq = zmac->loc_seq < MAX(seq1, seq2) ?
			      MAX(seq1, seq2) : zmac->loc_seq;
	}

	if (local_inactive)
		SET_FLAG(n->flags, ZEBRA_NEIGH_LOCAL_INACTIVE);
	else
		UNSET_FLAG(n->flags, ZEBRA_NEIGH_LOCAL_INACTIVE);

	/* Mark Router flag (R-bit) */
	if (is_router)
		SET_FLAG(n->flags, ZEBRA_NEIGH_ROUTER_FLAG);
	else
		UNSET_FLAG(n->flags, ZEBRA_NEIGH_ROUTER_FLAG);

	/* if the dataplane thinks that this is a sync entry but
	 * zebra doesn't we need to re-concile the diff
	 * by re-installing the dataplane entry
	 */
	if (dp_static) {
		new_static = zebra_vxlan_neigh_is_static(n);
		if (!new_static)
			inform_dataplane = true;
	}

	/* Check old and/or new MAC detected as duplicate mark
	 * the neigh as duplicate
	 */
	if (zebra_vxlan_ip_inherit_dad_from_mac(zvrf, old_zmac, zmac, n)) {
		flog_warn(EC_ZEBRA_DUP_IP_INHERIT_DETECTED,
			"VNI %u: MAC %s IP %s detected as duplicate during local update, inherit duplicate from MAC",
			zvni->vni,
			prefix_mac2str(macaddr, buf, sizeof(buf)),
			ipaddr2str(&n->ip, buf2, sizeof(buf2)));
	}

	/* For IP Duplicate Address Detection (DAD) is trigger,
	 * when the event is extended mobility based on scenario-B
	 * from the draft, IP/Neigh's MAC binding changed and
	 * neigh's previous state was remote.
	 */
	if (neigh_mac_change && neigh_was_remote)
		do_dad = true;

	zebra_vxlan_dup_addr_detect_for_neigh(zvrf, n, vtep_ip, do_dad,
					      &neigh_on_hold, true);

	if (inform_dataplane)
		zebra_vxlan_sync_neigh_dp_install(n, false /* set_inactive */,
				false /* force_clear_static */, __func__);

	/* Before we program this in BGP, we need to check if MAC is locally
	 * learnt. If not, force neighbor to be inactive and reset its seq.
	 */
	if (!CHECK_FLAG(zmac->flags, ZEBRA_MAC_LOCAL)) {
		zvni_local_neigh_update_log("local",
				n, is_router, local_inactive,
				false, false, inform_dataplane, false,
				"auto-mac");
		ZEBRA_NEIGH_SET_INACTIVE(n);
		n->loc_seq = 0;
		zmac->loc_seq = mac_new_seq;
		return 0;
	}

	zvni_local_neigh_update_log("local",
		n, is_router, local_inactive, false, false, inform_dataplane,
		true, created ? "created" : "updated");

	/* If the MAC's sequence number has changed, inform the MAC and all
	 * neighbors associated with the MAC to BGP, else just inform this
	 * neighbor.
	 */
	if (upd_mac_seq && zmac->loc_seq != mac_new_seq) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("Seq changed for MAC %s VNI %u - old %u new %u",
				   prefix_mac2str(macaddr, buf, sizeof(buf)),
				   zvni->vni, zmac->loc_seq, mac_new_seq);
		zmac->loc_seq = mac_new_seq;
		if (zvni_mac_send_add_to_client(zvni->vni, macaddr,
					zmac->flags, zmac->loc_seq, zmac->es))
			return -1;
		zvni_process_neigh_on_local_mac_change(zvni, zmac, 1,
				0 /*es_change*/);
		return 0;
	}

	n->loc_seq = zmac->loc_seq;

	if (!neigh_on_hold) {
		ZEBRA_NEIGH_SET_ACTIVE(n);
		new_bgp_ready =
			zebra_vxlan_neigh_is_ready_for_bgp(n);
		zebra_vxlan_neigh_send_add_del_to_client(n,
				old_bgp_ready, new_bgp_ready);
	} else {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("        Neighbor on hold not sending");
	}
	return 0;
}

static int zvni_remote_neigh_update(zebra_vni_t *zvni,
				    struct interface *ifp,
				    struct ipaddr *ip,
				    struct ethaddr *macaddr,
				    uint16_t state)
{
	char buf[ETHER_ADDR_STRLEN];
	char buf2[INET6_ADDRSTRLEN];
	zebra_neigh_t *n = NULL;
	zebra_mac_t *zmac = NULL;

	/* If the neighbor is unknown, there is no further action. */
	n = zvni_neigh_lookup(zvni, ip);
	if (!n)
		return 0;

	/* If a remote entry, see if it needs to be refreshed */
	if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_REMOTE)) {
#ifdef GNU_LINUX
		if (state & NUD_STALE)
			zvni_rem_neigh_install(zvni, n, false /*was_static*/);
#endif
	} else {
		/* We got a "remote" neighbor notification for an entry
		 * we think is local. This can happen in a multihoming
		 * scenario - but only if the MAC is already "remote".
		 * Just mark our entry as "remote".
		 */
		zmac = zvni_mac_lookup(zvni, macaddr);
		if (!zmac || !CHECK_FLAG(zmac->flags, ZEBRA_MAC_REMOTE)) {
			zlog_debug(
				"Ignore remote neigh %s (MAC %s) on L2-VNI %u - MAC unknown or local",
				ipaddr2str(&n->ip, buf2, sizeof(buf2)),
				prefix_mac2str(macaddr, buf, sizeof(buf)),
				zvni->vni);
			return -1;
		}

		UNSET_FLAG(n->flags, ZEBRA_NEIGH_ALL_LOCAL_FLAGS);
		SET_FLAG(n->flags, ZEBRA_NEIGH_REMOTE);
		ZEBRA_NEIGH_SET_ACTIVE(n);
		n->r_vtep_ip = zmac->fwd_info.r_vtep_ip;
	}

	return 0;
}

/*
 * Make hash key for MAC.
 */
static unsigned int mac_hash_keymake(const void *p)
{
	const zebra_mac_t *pmac = p;
	const void *pnt = (void *)pmac->macaddr.octet;

	return jhash(pnt, ETH_ALEN, 0xa5a5a55a);
}

/*
 * Compare two MAC addresses.
 */
static bool mac_cmp(const void *p1, const void *p2)
{
	const zebra_mac_t *pmac1 = p1;
	const zebra_mac_t *pmac2 = p2;

	if (pmac1 == NULL && pmac2 == NULL)
		return true;

	if (pmac1 == NULL || pmac2 == NULL)
		return false;

	return (memcmp(pmac1->macaddr.octet, pmac2->macaddr.octet, ETH_ALEN)
		== 0);
}

/*
 * Callback to allocate MAC hash entry.
 */
static void *zvni_mac_alloc(void *p)
{
	const zebra_mac_t *tmp_mac = p;
	zebra_mac_t *mac;

	mac = XCALLOC(MTYPE_MAC, sizeof(zebra_mac_t));
	*mac = *tmp_mac;

	return ((void *)mac);
}

/*
 * Add MAC entry.
 */
static zebra_mac_t *zvni_mac_add(zebra_vni_t *zvni, struct ethaddr *macaddr)
{
	zebra_mac_t tmp_mac;
	zebra_mac_t *mac = NULL;

	memset(&tmp_mac, 0, sizeof(zebra_mac_t));
	memcpy(&tmp_mac.macaddr, macaddr, ETH_ALEN);
	mac = hash_get(zvni->mac_table, &tmp_mac, zvni_mac_alloc);
	assert(mac);

	mac->zvni = zvni;
	mac->dad_mac_auto_recovery_timer = NULL;

	mac->neigh_list = list_new();
	mac->neigh_list->cmp = neigh_list_cmp;

	if (IS_ZEBRA_DEBUG_VXLAN || IS_ZEBRA_DEBUG_EVPN_MH_MAC) {
		char buf[ETHER_ADDR_STRLEN];

		zlog_debug("%s: MAC %s flags 0x%x",
				__func__,
				prefix_mac2str(&mac->macaddr,
					buf, sizeof(buf)),
				mac->flags);
	}
	return mac;
}

/*
 * Delete MAC entry.
 */
static int zvni_mac_del(zebra_vni_t *zvni, zebra_mac_t *mac)
{
	zebra_mac_t *tmp_mac;

	if (IS_ZEBRA_DEBUG_VXLAN || IS_ZEBRA_DEBUG_EVPN_MH_MAC) {
		char buf[ETHER_ADDR_STRLEN];

		zlog_debug("%s: MAC %s flags 0x%x",
				__func__,
				prefix_mac2str(&mac->macaddr,
					buf, sizeof(buf)),
				mac->flags);
	}

	/* force de-ref any ES entry linked to the MAC */
	zebra_evpn_es_mac_deref_entry(mac);

	/* Cancel proxy hold timer */
	zebra_vxlan_mac_stop_hold_timer(mac);

	/* Cancel auto recovery */
	THREAD_OFF(mac->dad_mac_auto_recovery_timer);

	list_delete(&mac->neigh_list);

	/* Free the VNI hash entry and allocated memory. */
	tmp_mac = hash_release(zvni->mac_table, mac);
	XFREE(MTYPE_MAC, tmp_mac);

	return 0;
}

static bool zvni_check_mac_del_from_db(struct mac_walk_ctx *wctx,
				       zebra_mac_t *mac)
{
	if ((wctx->flags & DEL_LOCAL_MAC) &&
	    (mac->flags & ZEBRA_MAC_LOCAL))
		return true;
	else if ((wctx->flags & DEL_REMOTE_MAC) &&
		 (mac->flags & ZEBRA_MAC_REMOTE))
		return true;
	else if ((wctx->flags & DEL_REMOTE_MAC_FROM_VTEP) &&
		 (mac->flags & ZEBRA_MAC_REMOTE) &&
		 IPV4_ADDR_SAME(&mac->fwd_info.r_vtep_ip, &wctx->r_vtep_ip))
		return true;
	else if ((wctx->flags & DEL_LOCAL_MAC) &&
		 (mac->flags & ZEBRA_MAC_AUTO) &&
		 !listcount(mac->neigh_list)) {
		if (IS_ZEBRA_DEBUG_VXLAN) {
			char buf[ETHER_ADDR_STRLEN];

			zlog_debug(
				"%s: Del MAC %s flags 0x%x", __func__,
				prefix_mac2str(&mac->macaddr, buf, sizeof(buf)),
				mac->flags);
		}
		wctx->uninstall = 0;

		return true;
	}

	return false;
}

/*
 * Free MAC hash entry (callback)
 */
static void zvni_mac_del_hash_entry(struct hash_bucket *bucket, void *arg)
{
	struct mac_walk_ctx *wctx = arg;
	zebra_mac_t *mac = bucket->data;

	if (zvni_check_mac_del_from_db(wctx, mac)) {
		if (wctx->upd_client && (mac->flags & ZEBRA_MAC_LOCAL)) {
			zvni_mac_send_del_to_client(wctx->zvni->vni,
					&mac->macaddr, mac->flags, false);
		}
		if (wctx->uninstall) {
			if (zebra_vxlan_mac_is_static(mac))
				zebra_vxlan_sync_mac_dp_install(mac,
						false /* set_inactive */,
						true /* force_clear_static */,
						__func__);

			if (mac->flags & ZEBRA_MAC_REMOTE)
				zvni_rem_mac_uninstall(wctx->zvni, mac);
		}

		zvni_mac_del(wctx->zvni, mac);
	}

	return;
}

/*
 * Delete all MAC entries for this VNI.
 */
static void zvni_mac_del_all(zebra_vni_t *zvni, int uninstall, int upd_client,
			     uint32_t flags)
{
	struct mac_walk_ctx wctx;

	if (!zvni->mac_table)
		return;

	memset(&wctx, 0, sizeof(struct mac_walk_ctx));
	wctx.zvni = zvni;
	wctx.uninstall = uninstall;
	wctx.upd_client = upd_client;
	wctx.flags = flags;

	hash_iterate(zvni->mac_table, zvni_mac_del_hash_entry, &wctx);
}

/*
 * Look up MAC hash entry.
 */
static zebra_mac_t *zvni_mac_lookup(zebra_vni_t *zvni, struct ethaddr *mac)
{
	zebra_mac_t tmp;
	zebra_mac_t *pmac;

	memset(&tmp, 0, sizeof(tmp));
	memcpy(&tmp.macaddr, mac, ETH_ALEN);
	pmac = hash_lookup(zvni->mac_table, &tmp);

	return pmac;
}

/*
 * Inform BGP about local MAC addition.
 */
static int zvni_mac_send_add_to_client(vni_t vni, struct ethaddr *macaddr,
		uint32_t mac_flags, uint32_t seq, struct zebra_evpn_es *es)
{
	uint8_t flags = 0;

	if (CHECK_FLAG(mac_flags, ZEBRA_MAC_LOCAL_INACTIVE)) {
		/* host reachability has not been verified locally */

		/* if no ES peer is claiming reachability we can't advertise the
		 * entry
		 */
		if (!CHECK_FLAG(mac_flags, ZEBRA_MAC_ES_PEER_ACTIVE))
			return 0;

		/* ES peers are claiming reachability; we will
		 * advertise the entry but with a proxy flag
		 */
		SET_FLAG(flags, ZEBRA_MACIP_TYPE_PROXY_ADVERT);
	}

	if (CHECK_FLAG(mac_flags, ZEBRA_MAC_STICKY))
		SET_FLAG(flags, ZEBRA_MACIP_TYPE_STICKY);
	if (CHECK_FLAG(mac_flags, ZEBRA_MAC_DEF_GW))
		SET_FLAG(flags, ZEBRA_MACIP_TYPE_GW);

	return zvni_macip_send_msg_to_client(vni, macaddr, NULL, flags,
			seq, ZEBRA_NEIGH_ACTIVE, es,
			ZEBRA_MACIP_ADD);
}

/*
 * Inform BGP about local MAC deletion.
 */
static int zvni_mac_send_del_to_client(vni_t vni, struct ethaddr *macaddr,
		uint32_t flags, bool force)
{
	if (!force) {
		if (CHECK_FLAG(flags, ZEBRA_MAC_LOCAL_INACTIVE) &&
				!CHECK_FLAG(flags, ZEBRA_MAC_ES_PEER_ACTIVE))
			/* the host was not advertised - nothing  to delete */
			return 0;
	}

	return zvni_macip_send_msg_to_client(vni, macaddr, NULL, 0 /* flags */,
			0 /* seq */, ZEBRA_NEIGH_ACTIVE, NULL,
			ZEBRA_MACIP_DEL);
}

/*
 * Map port or (port, VLAN) to a VNI. This is invoked upon getting MAC
 * notifications, to see if they are of interest.
 */
static zebra_vni_t *zvni_map_vlan(struct interface *ifp,
				  struct interface *br_if, vlanid_t vid)
{
	struct zebra_ns *zns;
	struct route_node *rn;
	struct interface *tmp_if = NULL;
	struct zebra_if *zif;
	struct zebra_l2info_bridge *br;
	struct zebra_l2info_vxlan *vxl = NULL;
	uint8_t bridge_vlan_aware;
	zebra_vni_t *zvni;
	int found = 0;

	/* Determine if bridge is VLAN-aware or not */
	zif = br_if->info;
	assert(zif);
	br = &zif->l2info.br;
	bridge_vlan_aware = br->vlan_aware;

	/* See if this interface (or interface plus VLAN Id) maps to a VxLAN */
	/* TODO: Optimize with a hash. */
	zns = zebra_ns_lookup(NS_DEFAULT);
	for (rn = route_top(zns->if_table); rn; rn = route_next(rn)) {
		tmp_if = (struct interface *)rn->info;
		if (!tmp_if)
			continue;
		zif = tmp_if->info;
		if (!zif || zif->zif_type != ZEBRA_IF_VXLAN)
			continue;
		if (!if_is_operative(tmp_if))
			continue;
		vxl = &zif->l2info.vxl;

		if (zif->brslave_info.br_if != br_if)
			continue;

		if (!bridge_vlan_aware || vxl->access_vlan == vid) {
			found = 1;
			break;
		}
	}

	if (!found)
		return NULL;

	zvni = zvni_lookup(vxl->vni);
	return zvni;
}

/*
 * Map SVI and associated bridge to a VNI. This is invoked upon getting
 * neighbor notifications, to see if they are of interest.
 */
static zebra_vni_t *zvni_from_svi(struct interface *ifp,
				  struct interface *br_if)
{
	struct zebra_ns *zns;
	struct route_node *rn;
	struct interface *tmp_if = NULL;
	struct zebra_if *zif;
	struct zebra_l2info_bridge *br;
	struct zebra_l2info_vxlan *vxl = NULL;
	uint8_t bridge_vlan_aware;
	vlanid_t vid = 0;
	zebra_vni_t *zvni;
	int found = 0;

	if (!br_if)
		return NULL;

	/* Make sure the linked interface is a bridge. */
	if (!IS_ZEBRA_IF_BRIDGE(br_if))
		return NULL;

	/* Determine if bridge is VLAN-aware or not */
	zif = br_if->info;
	assert(zif);
	br = &zif->l2info.br;
	bridge_vlan_aware = br->vlan_aware;
	if (bridge_vlan_aware) {
		struct zebra_l2info_vlan *vl;

		if (!IS_ZEBRA_IF_VLAN(ifp))
			return NULL;

		zif = ifp->info;
		assert(zif);
		vl = &zif->l2info.vl;
		vid = vl->vid;
	}

	/* See if this interface (or interface plus VLAN Id) maps to a VxLAN */
	/* TODO: Optimize with a hash. */
	zns = zebra_ns_lookup(NS_DEFAULT);
	for (rn = route_top(zns->if_table); rn; rn = route_next(rn)) {
		tmp_if = (struct interface *)rn->info;
		if (!tmp_if)
			continue;
		zif = tmp_if->info;
		if (!zif || zif->zif_type != ZEBRA_IF_VXLAN)
			continue;
		if (!if_is_operative(tmp_if))
			continue;
		vxl = &zif->l2info.vxl;

		if (zif->brslave_info.br_if != br_if)
			continue;

		if (!bridge_vlan_aware || vxl->access_vlan == vid) {
			found = 1;
			break;
		}
	}

	if (!found)
		return NULL;

	zvni = zvni_lookup(vxl->vni);
	return zvni;
}

/* Map to SVI on bridge corresponding to specified VLAN. This can be one
 * of two cases:
 * (a) In the case of a VLAN-aware bridge, the SVI is a L3 VLAN interface
 * linked to the bridge
 * (b) In the case of a VLAN-unaware bridge, the SVI is the bridge interface
 * itself
 */
static struct interface *zvni_map_to_svi(vlanid_t vid, struct interface *br_if)
{
	struct zebra_ns *zns;
	struct route_node *rn;
	struct interface *tmp_if = NULL;
	struct zebra_if *zif;
	struct zebra_l2info_bridge *br;
	struct zebra_l2info_vlan *vl;
	uint8_t bridge_vlan_aware;
	int found = 0;

	/* Defensive check, caller expected to invoke only with valid bridge. */
	if (!br_if)
		return NULL;

	/* Determine if bridge is VLAN-aware or not */
	zif = br_if->info;
	assert(zif);
	br = &zif->l2info.br;
	bridge_vlan_aware = br->vlan_aware;

	/* Check oper status of the SVI. */
	if (!bridge_vlan_aware)
		return if_is_operative(br_if) ? br_if : NULL;

	/* Identify corresponding VLAN interface. */
	/* TODO: Optimize with a hash. */
	zns = zebra_ns_lookup(NS_DEFAULT);
	for (rn = route_top(zns->if_table); rn; rn = route_next(rn)) {
		tmp_if = (struct interface *)rn->info;
		/* Check oper status of the SVI. */
		if (!tmp_if || !if_is_operative(tmp_if))
			continue;
		zif = tmp_if->info;
		if (!zif || zif->zif_type != ZEBRA_IF_VLAN
		    || zif->link != br_if)
			continue;
		vl = &zif->l2info.vl;

		if (vl->vid == vid) {
			found = 1;
			break;
		}
	}

	return found ? tmp_if : NULL;
}

/* Map to MAC-VLAN interface corresponding to specified SVI interface.
 */
static struct interface *zvni_map_to_macvlan(struct interface *br_if,
					     struct interface *svi_if)
{
	struct zebra_ns *zns;
	struct route_node *rn;
	struct interface *tmp_if = NULL;
	struct zebra_if *zif;
	int found = 0;

	/* Defensive check, caller expected to invoke only with valid bridge. */
	if (!br_if)
		return NULL;

	if (!svi_if) {
		zlog_debug("svi_if is not passed.");
		return NULL;
	}

	/* Determine if bridge is VLAN-aware or not */
	zif = br_if->info;
	assert(zif);

	/* Identify corresponding VLAN interface. */
	zns = zebra_ns_lookup(NS_DEFAULT);
	for (rn = route_top(zns->if_table); rn; rn = route_next(rn)) {
		tmp_if = (struct interface *)rn->info;
		/* Check oper status of the SVI. */
		if (!tmp_if || !if_is_operative(tmp_if))
			continue;
		zif = tmp_if->info;

		if (!zif || zif->zif_type != ZEBRA_IF_MACVLAN)
			continue;

		if (zif->link == svi_if) {
			found = 1;
			break;
		}
	}

	return found ? tmp_if : NULL;
}


/*
 * Install remote MAC into the forwarding plane.
 */
static int zvni_rem_mac_install(zebra_vni_t *zvni, zebra_mac_t *mac,
		bool was_static)
{
	const struct zebra_if *zif, *br_zif;
	const struct zebra_l2info_vxlan *vxl;
	bool sticky;
	enum zebra_dplane_result res;
	const struct interface *br_ifp;
	vlanid_t vid;
	uint32_t nhg_id;
	struct in_addr vtep_ip;

	if (!(mac->flags & ZEBRA_MAC_REMOTE))
		return 0;

	zif = zvni->vxlan_if->info;
	if (!zif)
		return -1;

	br_ifp = zif->brslave_info.br_if;
	if (br_ifp == NULL)
		return -1;

	vxl = &zif->l2info.vxl;

	sticky = !!CHECK_FLAG(mac->flags,
			 (ZEBRA_MAC_STICKY | ZEBRA_MAC_REMOTE_DEF_GW));

	/* If nexthop group for the FDB entry is inactive (not programmed in
	 * the dataplane) the MAC entry cannot be installed
	 */
	if (mac->es) {
		if (!(mac->es->flags & ZEBRA_EVPNES_NHG_ACTIVE))
			return -1;
		nhg_id = mac->es->nhg_id;
		vtep_ip.s_addr = 0;
	} else {
		nhg_id = 0;
		vtep_ip = mac->fwd_info.r_vtep_ip;
	}

	br_zif = (const struct zebra_if *)(br_ifp->info);

	if (IS_ZEBRA_IF_BRIDGE_VLAN_AWARE(br_zif))
		vid = vxl->access_vlan;
	else
		vid = 0;

	res = dplane_rem_mac_add(zvni->vxlan_if, br_ifp, vid,
			     &mac->macaddr, vtep_ip, sticky,
				 nhg_id, was_static);
	if (res != ZEBRA_DPLANE_REQUEST_FAILURE)
		return 0;
	else
		return -1;
}

/*
 * Uninstall remote MAC from the forwarding plane.
 */
static int zvni_rem_mac_uninstall(zebra_vni_t *zvni, zebra_mac_t *mac)
{
	const struct zebra_if *zif, *br_zif;
	const struct zebra_l2info_vxlan *vxl;
	struct in_addr vtep_ip;
	const struct interface *ifp, *br_ifp;
	vlanid_t vid;
	enum zebra_dplane_result res;

	if (!(mac->flags & ZEBRA_MAC_REMOTE))
		return 0;

	if (!zvni->vxlan_if) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("VNI %u hash %p couldn't be uninstalled - no intf",
				   zvni->vni, zvni);
		return -1;
	}

	zif = zvni->vxlan_if->info;
	if (!zif)
		return -1;

	br_ifp = zif->brslave_info.br_if;
	if (br_ifp == NULL)
		return -1;

	vxl = &zif->l2info.vxl;

	br_zif = (const struct zebra_if *)br_ifp->info;

	if (IS_ZEBRA_IF_BRIDGE_VLAN_AWARE(br_zif))
		vid = vxl->access_vlan;
	else
		vid = 0;

	ifp = zvni->vxlan_if;
	vtep_ip = mac->fwd_info.r_vtep_ip;

	res = dplane_rem_mac_del(ifp, br_ifp, vid, &mac->macaddr, vtep_ip);
	if (res != ZEBRA_DPLANE_REQUEST_FAILURE)
		return 0;
	else
		return -1;
}

/*
 * Install MAC hash entry - called upon access VLAN change.
 */
static void zvni_install_mac_hash(struct hash_bucket *bucket, void *ctxt)
{
	zebra_mac_t *mac;
	struct mac_walk_ctx *wctx = ctxt;

	mac = (zebra_mac_t *)bucket->data;

	if (CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE))
		zvni_rem_mac_install(wctx->zvni, mac, false);
}

/*
 * Count of remote neighbors referencing this MAC.
 */
static int remote_neigh_count(zebra_mac_t *zmac)
{
	zebra_neigh_t *n = NULL;
	struct listnode *node = NULL;
	int count = 0;

	for (ALL_LIST_ELEMENTS_RO(zmac->neigh_list, node, n)) {
		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_REMOTE))
			count++;
	}

	return count;
}

/*
 * Decrement neighbor refcount of MAC; uninstall and free it if
 * appropriate.
 */
static void zvni_deref_ip2mac(zebra_vni_t *zvni, zebra_mac_t *mac)
{
	if (!CHECK_FLAG(mac->flags, ZEBRA_MAC_AUTO))
		return;

	/* If all remote neighbors referencing a remote MAC go away,
	 * we need to uninstall the MAC.
	 */
	if (CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE) &&
	    remote_neigh_count(mac) == 0) {
		zvni_rem_mac_uninstall(zvni, mac);
		zebra_evpn_es_mac_deref_entry(mac);
		UNSET_FLAG(mac->flags, ZEBRA_MAC_REMOTE);
	}

	/* If no neighbors, delete the MAC. */
	if (list_isempty(mac->neigh_list))
		zvni_mac_del(zvni, mac);
}

/*
 * Read and populate local MACs and neighbors corresponding to this VNI.
 */
static void zvni_read_mac_neigh(zebra_vni_t *zvni, struct interface *ifp)
{
	struct zebra_ns *zns;
	struct zebra_if *zif;
	struct interface *vlan_if;
	struct zebra_l2info_vxlan *vxl;
	struct interface *vrr_if;

	zif = ifp->info;
	vxl = &zif->l2info.vxl;
	zns = zebra_ns_lookup(NS_DEFAULT);

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug(
			"Reading MAC FDB and Neighbors for intf %s(%u) VNI %u master %u",
			ifp->name, ifp->ifindex, zvni->vni,
			zif->brslave_info.bridge_ifindex);

	macfdb_read_for_bridge(zns, ifp, zif->brslave_info.br_if);
	vlan_if = zvni_map_to_svi(vxl->access_vlan, zif->brslave_info.br_if);
	if (vlan_if) {

		/* Add SVI MAC-IP */
		zvni_add_macip_for_intf(vlan_if, zvni);

		/* Add VRR MAC-IP - if any*/
		vrr_if = zebra_get_vrr_intf_for_svi(vlan_if);
		if (vrr_if)
			zvni_add_macip_for_intf(vrr_if, zvni);

		neigh_read_for_vlan(zns, vlan_if);
	}
}

/*
 * Hash function for VNI.
 */
static unsigned int vni_hash_keymake(const void *p)
{
	const zebra_vni_t *zvni = p;

	return (jhash_1word(zvni->vni, 0));
}

/*
 * Compare 2 VNI hash entries.
 */
static bool vni_hash_cmp(const void *p1, const void *p2)
{
	const zebra_vni_t *zvni1 = p1;
	const zebra_vni_t *zvni2 = p2;

	return (zvni1->vni == zvni2->vni);
}

int vni_list_cmp(void *p1, void *p2)
{
	const zebra_vni_t *zvni1 = p1;
	const zebra_vni_t *zvni2 = p2;

	if (zvni1->vni == zvni2->vni)
		return 0;
	return (zvni1->vni < zvni2->vni) ? -1 : 1;
}

/*
 * Callback to allocate VNI hash entry.
 */
static void *zvni_alloc(void *p)
{
	const zebra_vni_t *tmp_vni = p;
	zebra_vni_t *zvni;

	zvni = XCALLOC(MTYPE_ZVNI, sizeof(zebra_vni_t));
	zvni->vni = tmp_vni->vni;
	return ((void *)zvni);
}

/*
 * Look up VNI hash entry.
 */
zebra_vni_t *zvni_lookup(vni_t vni)
{
	struct zebra_vrf *zvrf;
	zebra_vni_t tmp_vni;
	zebra_vni_t *zvni = NULL;

	zvrf = zebra_vrf_get_evpn();
	assert(zvrf);
	memset(&tmp_vni, 0, sizeof(zebra_vni_t));
	tmp_vni.vni = vni;
	zvni = hash_lookup(zvrf->vni_table, &tmp_vni);

	return zvni;
}

/*
 * Add VNI hash entry.
 */
static zebra_vni_t *zvni_add(vni_t vni)
{
	struct zebra_vrf *zvrf;
	zebra_vni_t tmp_zvni;
	zebra_vni_t *zvni = NULL;

	zvrf = zebra_vrf_get_evpn();
	assert(zvrf);
	memset(&tmp_zvni, 0, sizeof(zebra_vni_t));
	tmp_zvni.vni = vni;
	zvni = hash_get(zvrf->vni_table, &tmp_zvni, zvni_alloc);
	assert(zvni);

	zebra_evpn_vni_es_init(zvni);

	/* Create hash table for MAC */
	zvni->mac_table =
		hash_create(mac_hash_keymake, mac_cmp, "Zebra VNI MAC Table");

	/* Create hash table for neighbors */
	zvni->neigh_table = hash_create(neigh_hash_keymake, neigh_cmp,
					"Zebra VNI Neighbor Table");

	return zvni;
}

/* vni<=>vxlan_zif association */
static void zvni_vxlan_if_set(zebra_vni_t *zvni, struct interface *ifp,
		bool set)
{
	struct zebra_if *zif;

	if (set) {
		if (zvni->vxlan_if == ifp)
			return;
		zvni->vxlan_if = ifp;
	} else {
		if (!zvni->vxlan_if)
			return;
		zvni->vxlan_if = NULL;
	}

	if (ifp)
		zif = ifp->info;
	else
		zif = NULL;

	zebra_evpn_vxl_vni_set(zif, zvni, set);
}

/*
 * Delete VNI hash entry.
 */
static int zvni_del(zebra_vni_t *zvni)
{
	struct zebra_vrf *zvrf;
	zebra_vni_t *tmp_zvni;

	zvrf = zebra_vrf_get_evpn();
	assert(zvrf);

	zvni_vxlan_if_set(zvni, zvni->vxlan_if, false /* set */);

	/* Remove references to the BUM mcast grp */
	zebra_vxlan_sg_deref(zvni->local_vtep_ip, zvni->mcast_grp);

	/* Free the neighbor hash table. */
	hash_free(zvni->neigh_table);
	zvni->neigh_table = NULL;

	/* Free the MAC hash table. */
	hash_free(zvni->mac_table);
	zvni->mac_table = NULL;

	zebra_evpn_vni_es_cleanup(zvni);

	/* Free the VNI hash entry and allocated memory. */
	tmp_zvni = hash_release(zvrf->vni_table, zvni);
	XFREE(MTYPE_ZVNI, tmp_zvni);

	return 0;
}

/*
 * Inform BGP about local VNI addition.
 */
static int zvni_send_add_to_client(zebra_vni_t *zvni)
{
	struct zserv *client;
	struct stream *s;
	int rc;

	client = zserv_find_client(ZEBRA_ROUTE_BGP, 0);
	/* BGP may not be running. */
	if (!client)
		return 0;

	s = stream_new(ZEBRA_MAX_PACKET_SIZ);

	zclient_create_header(s, ZEBRA_VNI_ADD, zebra_vrf_get_evpn_id());
	stream_putl(s, zvni->vni);
	stream_put_in_addr(s, &zvni->local_vtep_ip);
	stream_put(s, &zvni->vrf_id, sizeof(vrf_id_t)); /* tenant vrf */
	stream_put_in_addr(s, &zvni->mcast_grp);

	/* Write packet size. */
	stream_putw_at(s, 0, stream_get_endp(s));

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("Send VNI_ADD %u %s tenant vrf %s to %s", zvni->vni,
			   inet_ntoa(zvni->local_vtep_ip),
			   vrf_id_to_name(zvni->vrf_id),
			   zebra_route_string(client->proto));

	client->vniadd_cnt++;
	rc = zserv_send_message(client, s);

	if (!(zvni->flags & ZVNI_READY_FOR_BGP)) {
		zvni->flags |= ZVNI_READY_FOR_BGP;
		/* once the VNI is sent the ES-EVIs can also be replayed
		 * to BGP
		 */
		zebra_evpn_vni_update_all_es(zvni);
	}
	return rc;
}

/*
 * Inform BGP about local VNI deletion.
 */
static int zvni_send_del_to_client(zebra_vni_t *zvni)
{
	struct zserv *client;
	struct stream *s;

	client = zserv_find_client(ZEBRA_ROUTE_BGP, 0);
	/* BGP may not be running. */
	if (!client)
		return 0;

	if (zvni->flags & ZVNI_READY_FOR_BGP) {
		zvni->flags &= ~ZVNI_READY_FOR_BGP;
		/* the ES-EVIs must be removed from BGP before the VNI is */
		zebra_evpn_vni_update_all_es(zvni);
	}

	s = stream_new(ZEBRA_MAX_PACKET_SIZ);
	stream_reset(s);

	zclient_create_header(s, ZEBRA_VNI_DEL, zebra_vrf_get_evpn_id());
	stream_putl(s, zvni->vni);

	/* Write packet size. */
	stream_putw_at(s, 0, stream_get_endp(s));

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("Send VNI_DEL %u to %s", zvni->vni,
			   zebra_route_string(client->proto));

	client->vnidel_cnt++;
	return zserv_send_message(client, s);
}

/*
 * Build the VNI hash table by going over the VxLAN interfaces. This
 * is called when EVPN (advertise-all-vni) is enabled.
 */
static void zvni_build_hash_table(void)
{
	struct zebra_ns *zns;
	struct route_node *rn;
	struct interface *ifp;

	/* Walk VxLAN interfaces and create VNI hash. */
	zns = zebra_ns_lookup(NS_DEFAULT);
	for (rn = route_top(zns->if_table); rn; rn = route_next(rn)) {
		vni_t vni;
		zebra_vni_t *zvni = NULL;
		zebra_l3vni_t *zl3vni = NULL;
		struct zebra_if *zif;
		struct zebra_l2info_vxlan *vxl;

		ifp = (struct interface *)rn->info;
		if (!ifp)
			continue;
		zif = ifp->info;
		if (!zif || zif->zif_type != ZEBRA_IF_VXLAN)
			continue;

		vxl = &zif->l2info.vxl;
		vni = vxl->vni;

		/* L3-VNI and L2-VNI are handled seperately */
		zl3vni = zl3vni_lookup(vni);
		if (zl3vni) {

			if (IS_ZEBRA_DEBUG_VXLAN)
				zlog_debug(
					"create L3-VNI hash for Intf %s(%u) L3-VNI %u",
					ifp->name, ifp->ifindex, vni);

			/* associate with vxlan_if */
			zl3vni->local_vtep_ip = vxl->vtep_ip;
			zl3vni->vxlan_if = ifp;

			/*
			 * we need to associate with SVI.
			 * we can associate with svi-if only after association
			 * with vxlan-intf is complete
			 */
			zl3vni->svi_if = zl3vni_map_to_svi_if(zl3vni);

			/* Associate l3vni to mac-vlan and extract VRR MAC */
			zl3vni->mac_vlan_if = zl3vni_map_to_mac_vlan_if(zl3vni);

			if (IS_ZEBRA_DEBUG_VXLAN)
				zlog_debug("create l3vni %u svi_if %s mac_vlan_if %s",
				   vni, zl3vni->svi_if ? zl3vni->svi_if->name
				   : "NIL",
				   zl3vni->mac_vlan_if ?
				   zl3vni->mac_vlan_if->name : "NIL");

			if (is_l3vni_oper_up(zl3vni))
				zebra_vxlan_process_l3vni_oper_up(zl3vni);

		} else {
			struct interface *vlan_if = NULL;

			if (IS_ZEBRA_DEBUG_VXLAN)
				zlog_debug(
					"Create L2-VNI hash for intf %s(%u) L2-VNI %u local IP %s",
					ifp->name, ifp->ifindex, vni,
					inet_ntoa(vxl->vtep_ip));

			/* VNI hash entry is expected to exist, if the BGP process is killed */
			zvni = zvni_lookup(vni);
			if (zvni) {
				zlog_debug(
					"VNI hash already present for IF %s(%u) L2-VNI %u",
					ifp->name, ifp->ifindex, vni);

				/*
				 * Inform BGP if intf is up and mapped to
				 * bridge.
				 */
				if (if_is_operative(ifp) &&
					zif->brslave_info.br_if)
					zvni_send_add_to_client(zvni);

				/* Send Local MAC-entries to client */
				zvni_send_mac_to_client(zvni);

				/* Send Loval Neighbor entries to client */
				zvni_send_neigh_to_client(zvni);
			} else {
				zvni = zvni_add(vni);
				if (!zvni) {
					zlog_debug(
						"Failed to add VNI hash, IF %s(%u) L2-VNI %u",
						ifp->name, ifp->ifindex, vni);
					return;
				}

				if (zvni->local_vtep_ip.s_addr !=
					vxl->vtep_ip.s_addr ||
					zvni->mcast_grp.s_addr !=
					vxl->mcast_grp.s_addr) {
					zebra_vxlan_sg_deref(
						zvni->local_vtep_ip,
						zvni->mcast_grp);
					zebra_vxlan_sg_ref(vxl->vtep_ip,
						vxl->mcast_grp);
					zvni->local_vtep_ip = vxl->vtep_ip;
					zvni->mcast_grp = vxl->mcast_grp;
					/* on local vtep-ip check if ES
					 * orig-ip needs to be updated
					 */
					zebra_evpn_es_set_base_vni(zvni);
				}
				zvni_vxlan_if_set(zvni, ifp, true /* set */);
				vlan_if = zvni_map_to_svi(vxl->access_vlan,
						zif->brslave_info.br_if);
				if (vlan_if) {
					zvni->vrf_id = vlan_if->vrf_id;
					zl3vni = zl3vni_from_vrf(
							vlan_if->vrf_id);
					if (zl3vni)
						listnode_add_sort(
							zl3vni->l2vnis, zvni);
				}

				/*
				 * Inform BGP if intf is up and mapped to
				 * bridge.
				 */
				if (if_is_operative(ifp) &&
					zif->brslave_info.br_if)
					zvni_send_add_to_client(zvni);
			}
		}
	}
}

/*
 * See if remote VTEP matches with prefix.
 */
static int zvni_vtep_match(struct in_addr *vtep_ip, zebra_vtep_t *zvtep)
{
	return (IPV4_ADDR_SAME(vtep_ip, &zvtep->vtep_ip));
}

/*
 * Locate remote VTEP in VNI hash table.
 */
static zebra_vtep_t *zvni_vtep_find(zebra_vni_t *zvni, struct in_addr *vtep_ip)
{
	zebra_vtep_t *zvtep;

	if (!zvni)
		return NULL;

	for (zvtep = zvni->vteps; zvtep; zvtep = zvtep->next) {
		if (zvni_vtep_match(vtep_ip, zvtep))
			break;
	}

	return zvtep;
}

/*
 * Add remote VTEP to VNI hash table.
 */
static zebra_vtep_t *zvni_vtep_add(zebra_vni_t *zvni, struct in_addr *vtep_ip,
		int flood_control)

{
	zebra_vtep_t *zvtep;

	zvtep = XCALLOC(MTYPE_ZVNI_VTEP, sizeof(zebra_vtep_t));

	zvtep->vtep_ip = *vtep_ip;
	zvtep->flood_control = flood_control;

	if (zvni->vteps)
		zvni->vteps->prev = zvtep;
	zvtep->next = zvni->vteps;
	zvni->vteps = zvtep;

	return zvtep;
}

/*
 * Remove remote VTEP from VNI hash table.
 */
static int zvni_vtep_del(zebra_vni_t *zvni, zebra_vtep_t *zvtep)
{
	if (zvtep->next)
		zvtep->next->prev = zvtep->prev;
	if (zvtep->prev)
		zvtep->prev->next = zvtep->next;
	else
		zvni->vteps = zvtep->next;

	zvtep->prev = zvtep->next = NULL;
	XFREE(MTYPE_ZVNI_VTEP, zvtep);

	return 0;
}

/*
 * Delete all remote VTEPs for this VNI (upon VNI delete). Also
 * uninstall from kernel if asked to.
 */
static int zvni_vtep_del_all(zebra_vni_t *zvni, int uninstall)
{
	zebra_vtep_t *zvtep, *zvtep_next;

	if (!zvni)
		return -1;

	for (zvtep = zvni->vteps; zvtep; zvtep = zvtep_next) {
		zvtep_next = zvtep->next;
		if (uninstall)
			zvni_vtep_uninstall(zvni, &zvtep->vtep_ip);
		zvni_vtep_del(zvni, zvtep);
	}

	return 0;
}

/*
 * Install remote VTEP into the kernel if the remote VTEP has asked
 * for head-end-replication.
 */
static int zvni_vtep_install(zebra_vni_t *zvni, zebra_vtep_t *zvtep)
{
	if (is_vxlan_flooding_head_end() &&
	    (zvtep->flood_control == VXLAN_FLOOD_HEAD_END_REPL)) {
		if (ZEBRA_DPLANE_REQUEST_FAILURE ==
		    dplane_vtep_add(zvni->vxlan_if,
				    &zvtep->vtep_ip, zvni->vni))
			return -1;
	}

	return 0;
}

/*
 * Uninstall remote VTEP from the kernel.
 */
static int zvni_vtep_uninstall(zebra_vni_t *zvni, struct in_addr *vtep_ip)
{
	if (!zvni->vxlan_if) {
		zlog_debug("VNI %u hash %p couldn't be uninstalled - no intf",
			   zvni->vni, zvni);
		return -1;
	}

	if (ZEBRA_DPLANE_REQUEST_FAILURE ==
	    dplane_vtep_delete(zvni->vxlan_if, vtep_ip, zvni->vni))
		return -1;

	return 0;
}

/*
 * Install or uninstall flood entries in the kernel corresponding to
 * remote VTEPs. This is invoked upon change to BUM handling.
 */
static void zvni_handle_flooding_remote_vteps(struct hash_bucket *bucket,
					      void *zvrf)
{
	zebra_vni_t *zvni;
	zebra_vtep_t *zvtep;

	zvni = (zebra_vni_t *)bucket->data;
	if (!zvni)
		return;

	for (zvtep = zvni->vteps; zvtep; zvtep = zvtep->next) {
		if (is_vxlan_flooding_head_end())
			zvni_vtep_install(zvni, zvtep);
		else
			zvni_vtep_uninstall(zvni, &zvtep->vtep_ip);
	}
}

/*
 * Cleanup VNI/VTEP and update kernel
 */
static void zvni_cleanup_all(struct hash_bucket *bucket, void *arg)
{
	zebra_vni_t *zvni = NULL;
	zebra_l3vni_t *zl3vni = NULL;
	struct zebra_vrf *zvrf = (struct zebra_vrf *)arg;

	zvni = (zebra_vni_t *)bucket->data;

	/* remove from l3-vni list */
	if (zvrf->l3vni)
		zl3vni = zl3vni_lookup(zvrf->l3vni);
	if (zl3vni)
		listnode_delete(zl3vni->l2vnis, zvni);

	/* Free up all neighbors and MACs, if any. */
	zvni_neigh_del_all(zvni, 1, 0, DEL_ALL_NEIGH);
	zvni_mac_del_all(zvni, 1, 0, DEL_ALL_MAC);

	/* Free up all remote VTEPs, if any. */
	zvni_vtep_del_all(zvni, 1);

	/* Delete the hash entry. */
	zvni_del(zvni);
}

/* cleanup L3VNI */
static void zl3vni_cleanup_all(struct hash_bucket *bucket, void *args)
{
	zebra_l3vni_t *zl3vni = NULL;

	zl3vni = (zebra_l3vni_t *)bucket->data;

	zebra_vxlan_process_l3vni_oper_down(zl3vni);
}

static void rb_find_or_add_host(struct host_rb_tree_entry *hrbe,
				const struct prefix *host)
{
	struct host_rb_entry lookup;
	struct host_rb_entry *hle;

	memset(&lookup, 0, sizeof(lookup));
	memcpy(&lookup.p, host, sizeof(*host));

	hle = RB_FIND(host_rb_tree_entry, hrbe, &lookup);
	if (hle)
		return;

	hle = XCALLOC(MTYPE_HOST_PREFIX, sizeof(struct host_rb_entry));
	memcpy(hle, &lookup, sizeof(lookup));

	RB_INSERT(host_rb_tree_entry, hrbe, hle);
}

static void rb_delete_host(struct host_rb_tree_entry *hrbe, struct prefix *host)
{
	struct host_rb_entry lookup;
	struct host_rb_entry *hle;

	memset(&lookup, 0, sizeof(lookup));
	memcpy(&lookup.p, host, sizeof(*host));

	hle = RB_FIND(host_rb_tree_entry, hrbe, &lookup);
	if (hle) {
		RB_REMOVE(host_rb_tree_entry, hrbe, hle);
		XFREE(MTYPE_HOST_PREFIX, hle);
	}

	return;
}

/*
 * Look up MAC hash entry.
 */
static zebra_mac_t *zl3vni_rmac_lookup(zebra_l3vni_t *zl3vni,
				       const struct ethaddr *rmac)
{
	zebra_mac_t tmp;
	zebra_mac_t *pmac;

	memset(&tmp, 0, sizeof(tmp));
	memcpy(&tmp.macaddr, rmac, ETH_ALEN);
	pmac = hash_lookup(zl3vni->rmac_table, &tmp);

	return pmac;
}

/*
 * Callback to allocate RMAC hash entry.
 */
static void *zl3vni_rmac_alloc(void *p)
{
	const zebra_mac_t *tmp_rmac = p;
	zebra_mac_t *zrmac;

	zrmac = XCALLOC(MTYPE_MAC, sizeof(zebra_mac_t));
	*zrmac = *tmp_rmac;

	return ((void *)zrmac);
}

/*
 * Add RMAC entry to l3-vni
 */
static zebra_mac_t *zl3vni_rmac_add(zebra_l3vni_t *zl3vni,
				    const struct ethaddr *rmac)
{
	zebra_mac_t tmp_rmac;
	zebra_mac_t *zrmac = NULL;

	memset(&tmp_rmac, 0, sizeof(zebra_mac_t));
	memcpy(&tmp_rmac.macaddr, rmac, ETH_ALEN);
	zrmac = hash_get(zl3vni->rmac_table, &tmp_rmac, zl3vni_rmac_alloc);
	assert(zrmac);

	RB_INIT(host_rb_tree_entry, &zrmac->host_rb);

	SET_FLAG(zrmac->flags, ZEBRA_MAC_REMOTE);
	SET_FLAG(zrmac->flags, ZEBRA_MAC_REMOTE_RMAC);

	return zrmac;
}

/*
 * Delete MAC entry.
 */
static int zl3vni_rmac_del(zebra_l3vni_t *zl3vni, zebra_mac_t *zrmac)
{
	zebra_mac_t *tmp_rmac;
	struct host_rb_entry *hle;

	while (!RB_EMPTY(host_rb_tree_entry, &zrmac->host_rb)) {
		hle = RB_ROOT(host_rb_tree_entry, &zrmac->host_rb);

		RB_REMOVE(host_rb_tree_entry, &zrmac->host_rb, hle);
		XFREE(MTYPE_HOST_PREFIX, hle);
	}

	tmp_rmac = hash_release(zl3vni->rmac_table, zrmac);
	XFREE(MTYPE_MAC, tmp_rmac);

	return 0;
}

/*
 * Install remote RMAC into the forwarding plane.
 */
static int zl3vni_rmac_install(zebra_l3vni_t *zl3vni, zebra_mac_t *zrmac)
{
	const struct zebra_if *zif = NULL, *br_zif = NULL;
	const struct zebra_l2info_vxlan *vxl = NULL;
	const struct interface *br_ifp;
	enum zebra_dplane_result res;
	vlanid_t vid;

	if (!(CHECK_FLAG(zrmac->flags, ZEBRA_MAC_REMOTE))
	    || !(CHECK_FLAG(zrmac->flags, ZEBRA_MAC_REMOTE_RMAC)))
		return 0;

	zif = zl3vni->vxlan_if->info;
	if (!zif)
		return -1;

	br_ifp = zif->brslave_info.br_if;
	if (br_ifp == NULL)
		return -1;

	vxl = &zif->l2info.vxl;

	br_zif = (const struct zebra_if *)br_ifp->info;

	if (IS_ZEBRA_IF_BRIDGE_VLAN_AWARE(br_zif))
		vid = vxl->access_vlan;
	else
		vid = 0;

	res = dplane_rem_mac_add(zl3vni->vxlan_if, br_ifp, vid,
			     &zrmac->macaddr, zrmac->fwd_info.r_vtep_ip, 0, 0,
				 false /*was_static*/);
	if (res != ZEBRA_DPLANE_REQUEST_FAILURE)
		return 0;
	else
		return -1;
}

/*
 * Uninstall remote RMAC from the forwarding plane.
 */
static int zl3vni_rmac_uninstall(zebra_l3vni_t *zl3vni, zebra_mac_t *zrmac)
{
	char buf[ETHER_ADDR_STRLEN];
	const struct zebra_if *zif = NULL, *br_zif;
	const struct zebra_l2info_vxlan *vxl = NULL;
	const struct interface *br_ifp;
	vlanid_t vid;
	enum zebra_dplane_result res;

	if (!(CHECK_FLAG(zrmac->flags, ZEBRA_MAC_REMOTE))
	    || !(CHECK_FLAG(zrmac->flags, ZEBRA_MAC_REMOTE_RMAC)))
		return 0;

	if (!zl3vni->vxlan_if) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"RMAC %s on L3-VNI %u hash %p couldn't be uninstalled - no vxlan_if",
				prefix_mac2str(&zrmac->macaddr,
					       buf, sizeof(buf)),
				zl3vni->vni, zl3vni);
		return -1;
	}

	zif = zl3vni->vxlan_if->info;
	if (!zif)
		return -1;

	br_ifp = zif->brslave_info.br_if;
	if (br_ifp == NULL)
		return -1;

	vxl = &zif->l2info.vxl;

	br_zif = (const struct zebra_if *)br_ifp->info;
	if (IS_ZEBRA_IF_BRIDGE_VLAN_AWARE(br_zif))
		vid = vxl->access_vlan;
	else
		vid = 0;

	res = dplane_rem_mac_del(zl3vni->vxlan_if, br_ifp, vid,
			     &zrmac->macaddr, zrmac->fwd_info.r_vtep_ip);
	if (res != ZEBRA_DPLANE_REQUEST_FAILURE)
		return 0;
	else
		return -1;
}

/* handle rmac add */
static int zl3vni_remote_rmac_add(zebra_l3vni_t *zl3vni,
				  const struct ethaddr *rmac,
				  const struct ipaddr *vtep_ip,
				  const struct prefix *host_prefix)
{
	char buf[ETHER_ADDR_STRLEN];
	char buf1[INET6_ADDRSTRLEN];
	char buf2[PREFIX_STRLEN];
	zebra_mac_t *zrmac = NULL;

	zrmac = zl3vni_rmac_lookup(zl3vni, rmac);
	if (!zrmac) {

		 /* Create the RMAC entry, or update its vtep, if necessary. */
		zrmac = zl3vni_rmac_add(zl3vni, rmac);
		if (!zrmac) {
			zlog_debug(
				"Failed to add RMAC %s L3VNI %u Remote VTEP %s, prefix %s",
				prefix_mac2str(rmac, buf, sizeof(buf)),
				zl3vni->vni,
				ipaddr2str(vtep_ip, buf1, sizeof(buf1)),
				prefix2str(host_prefix, buf2, sizeof(buf2)));
			return -1;
		}
		memset(&zrmac->fwd_info, 0, sizeof(zrmac->fwd_info));
		zrmac->fwd_info.r_vtep_ip = vtep_ip->ipaddr_v4;

		/* Send RMAC for FPM processing */
		hook_call(zebra_rmac_update, zrmac, zl3vni, false,
			  "new RMAC added");

		/* install rmac in kernel */
		zl3vni_rmac_install(zl3vni, zrmac);
	} else if (!IPV4_ADDR_SAME(&zrmac->fwd_info.r_vtep_ip,
				   &vtep_ip->ipaddr_v4)) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"L3VNI %u Remote VTEP change(%s -> %s) for RMAC %s, prefix %s",
				zl3vni->vni,
				inet_ntoa(zrmac->fwd_info.r_vtep_ip),
				ipaddr2str(vtep_ip, buf1, sizeof(buf1)),
				prefix_mac2str(rmac, buf, sizeof(buf)),
				prefix2str(host_prefix, buf2, sizeof(buf2)));

		zrmac->fwd_info.r_vtep_ip = vtep_ip->ipaddr_v4;

		/* install rmac in kernel */
		zl3vni_rmac_install(zl3vni, zrmac);
	}

	rb_find_or_add_host(&zrmac->host_rb, host_prefix);

	return 0;
}


/* handle rmac delete */
static void zl3vni_remote_rmac_del(zebra_l3vni_t *zl3vni, zebra_mac_t *zrmac,
				  struct prefix *host_prefix)
{
	rb_delete_host(&zrmac->host_rb, host_prefix);

	if (RB_EMPTY(host_rb_tree_entry, &zrmac->host_rb)) {
		/* uninstall from kernel */
		zl3vni_rmac_uninstall(zl3vni, zrmac);

		/* Send RMAC for FPM processing */
		hook_call(zebra_rmac_update, zrmac, zl3vni, true,
			  "RMAC deleted");

		/* del the rmac entry */
		zl3vni_rmac_del(zl3vni, zrmac);
	}
}

/*
 * Look up nh hash entry on a l3-vni.
 */
static zebra_neigh_t *zl3vni_nh_lookup(zebra_l3vni_t *zl3vni,
				       const struct ipaddr *ip)
{
	zebra_neigh_t tmp;
	zebra_neigh_t *n;

	memset(&tmp, 0, sizeof(tmp));
	memcpy(&tmp.ip, ip, sizeof(struct ipaddr));
	n = hash_lookup(zl3vni->nh_table, &tmp);

	return n;
}


/*
 * Callback to allocate NH hash entry on L3-VNI.
 */
static void *zl3vni_nh_alloc(void *p)
{
	const zebra_neigh_t *tmp_n = p;
	zebra_neigh_t *n;

	n = XCALLOC(MTYPE_NEIGH, sizeof(zebra_neigh_t));
	*n = *tmp_n;

	return ((void *)n);
}

/*
 * Add neighbor entry.
 */
static zebra_neigh_t *zl3vni_nh_add(zebra_l3vni_t *zl3vni,
				    const struct ipaddr *ip,
				    const struct ethaddr *mac)
{
	zebra_neigh_t tmp_n;
	zebra_neigh_t *n = NULL;

	memset(&tmp_n, 0, sizeof(zebra_neigh_t));
	memcpy(&tmp_n.ip, ip, sizeof(struct ipaddr));
	n = hash_get(zl3vni->nh_table, &tmp_n, zl3vni_nh_alloc);
	assert(n);

	RB_INIT(host_rb_tree_entry, &n->host_rb);

	memcpy(&n->emac, mac, ETH_ALEN);
	SET_FLAG(n->flags, ZEBRA_NEIGH_REMOTE);
	SET_FLAG(n->flags, ZEBRA_NEIGH_REMOTE_NH);

	return n;
}

/*
 * Delete neighbor entry.
 */
static int zl3vni_nh_del(zebra_l3vni_t *zl3vni, zebra_neigh_t *n)
{
	zebra_neigh_t *tmp_n;
	struct host_rb_entry *hle;

	while (!RB_EMPTY(host_rb_tree_entry, &n->host_rb)) {
		hle = RB_ROOT(host_rb_tree_entry, &n->host_rb);

		RB_REMOVE(host_rb_tree_entry, &n->host_rb, hle);
		XFREE(MTYPE_HOST_PREFIX, hle);
	}

	tmp_n = hash_release(zl3vni->nh_table, n);
	XFREE(MTYPE_NEIGH, tmp_n);

	return 0;
}

/*
 * Install remote nh as neigh into the kernel.
 */
static int zl3vni_nh_install(zebra_l3vni_t *zl3vni, zebra_neigh_t *n)
{
	uint8_t flags;
	int ret = 0;

	if (!is_l3vni_oper_up(zl3vni))
		return -1;

	if (!(n->flags & ZEBRA_NEIGH_REMOTE)
	    || !(n->flags & ZEBRA_NEIGH_REMOTE_NH))
		return 0;

	flags = DPLANE_NTF_EXT_LEARNED;
	if (n->flags & ZEBRA_NEIGH_ROUTER_FLAG)
		flags |= DPLANE_NTF_ROUTER;

	dplane_rem_neigh_add(zl3vni->svi_if, &n->ip, &n->emac, flags,
			false /*was_static*/);

	return ret;
}

/*
 * Uninstall remote nh from the kernel.
 */
static int zl3vni_nh_uninstall(zebra_l3vni_t *zl3vni, zebra_neigh_t *n)
{
	if (!(n->flags & ZEBRA_NEIGH_REMOTE)
	    || !(n->flags & ZEBRA_NEIGH_REMOTE_NH))
		return 0;

	if (!zl3vni->svi_if || !if_is_operative(zl3vni->svi_if))
		return 0;

	dplane_rem_neigh_delete(zl3vni->svi_if, &n->ip);

	return 0;
}

/* add remote vtep as a neigh entry */
static int zl3vni_remote_nh_add(zebra_l3vni_t *zl3vni,
				const struct ipaddr *vtep_ip,
				const struct ethaddr *rmac,
				const struct prefix *host_prefix)
{
	char buf[ETHER_ADDR_STRLEN];
	char buf1[ETHER_ADDR_STRLEN];
	char buf2[INET6_ADDRSTRLEN];
	char buf3[PREFIX_STRLEN];
	zebra_neigh_t *nh = NULL;

	/* Create the next hop entry, or update its mac, if necessary. */
	nh = zl3vni_nh_lookup(zl3vni, vtep_ip);
	if (!nh) {
		nh = zl3vni_nh_add(zl3vni, vtep_ip, rmac);
		if (!nh) {
			zlog_debug(
				"Failed to add NH %s as Neigh (RMAC %s L3-VNI %u prefix %s)",
				ipaddr2str(vtep_ip, buf1, sizeof(buf2)),
				prefix_mac2str(rmac, buf, sizeof(buf)),
				zl3vni->vni,
				prefix2str(host_prefix, buf2, sizeof(buf2)));
			return -1;
		}

		/* install the nh neigh in kernel */
		zl3vni_nh_install(zl3vni, nh);
	} else if (memcmp(&nh->emac, rmac, ETH_ALEN) != 0) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("L3VNI %u RMAC change(%s --> %s) for nexthop %s, prefix %s",
				   zl3vni->vni,
				   prefix_mac2str(&nh->emac, buf, sizeof(buf)),
				   prefix_mac2str(rmac, buf1, sizeof(buf1)),
				   ipaddr2str(vtep_ip, buf2, sizeof(buf2)),
				   prefix2str(host_prefix, buf3, sizeof(buf3)));

		memcpy(&nh->emac, rmac, ETH_ALEN);
		/* install (update) the nh neigh in kernel */
		zl3vni_nh_install(zl3vni, nh);
	}

	rb_find_or_add_host(&nh->host_rb, host_prefix);

	return 0;
}

/* handle nh neigh delete */
static void zl3vni_remote_nh_del(zebra_l3vni_t *zl3vni, zebra_neigh_t *nh,
				 struct prefix *host_prefix)
{
	rb_delete_host(&nh->host_rb, host_prefix);

	if (RB_EMPTY(host_rb_tree_entry, &nh->host_rb)) {
		/* uninstall from kernel */
		zl3vni_nh_uninstall(zl3vni, nh);

		/* delete the nh entry */
		zl3vni_nh_del(zl3vni, nh);
	}
}

/* handle neigh update from kernel - the only thing of interest is to
 * readd stale entries.
 */
static int zl3vni_local_nh_add_update(zebra_l3vni_t *zl3vni, struct ipaddr *ip,
				      uint16_t state)
{
#ifdef GNU_LINUX
	zebra_neigh_t *n = NULL;

	n = zl3vni_nh_lookup(zl3vni, ip);
	if (!n)
		return 0;

	/* all next hop neigh are remote and installed by frr.
	 * If the kernel has aged this entry, re-install.
	 */
	if (state & NUD_STALE)
		zl3vni_nh_install(zl3vni, n);
#endif
	return 0;
}

/* handle neigh delete from kernel */
static int zl3vni_local_nh_del(zebra_l3vni_t *zl3vni, struct ipaddr *ip)
{
	zebra_neigh_t *n = NULL;

	n = zl3vni_nh_lookup(zl3vni, ip);
	if (!n)
		return 0;

	/* all next hop neigh are remote and installed by frr.
	 * If we get an age out notification for these neigh entries, we have to
	 * install it back
	 */
	zl3vni_nh_install(zl3vni, n);

	return 0;
}

/*
 * Hash function for L3 VNI.
 */
static unsigned int l3vni_hash_keymake(const void *p)
{
	const zebra_l3vni_t *zl3vni = p;

	return jhash_1word(zl3vni->vni, 0);
}

/*
 * Compare 2 L3 VNI hash entries.
 */
static bool l3vni_hash_cmp(const void *p1, const void *p2)
{
	const zebra_l3vni_t *zl3vni1 = p1;
	const zebra_l3vni_t *zl3vni2 = p2;

	return (zl3vni1->vni == zl3vni2->vni);
}

/*
 * Callback to allocate L3 VNI hash entry.
 */
static void *zl3vni_alloc(void *p)
{
	zebra_l3vni_t *zl3vni = NULL;
	const zebra_l3vni_t *tmp_l3vni = p;

	zl3vni = XCALLOC(MTYPE_ZL3VNI, sizeof(zebra_l3vni_t));
	zl3vni->vni = tmp_l3vni->vni;
	return ((void *)zl3vni);
}

/*
 * Look up L3 VNI hash entry.
 */
static zebra_l3vni_t *zl3vni_lookup(vni_t vni)
{
	zebra_l3vni_t tmp_l3vni;
	zebra_l3vni_t *zl3vni = NULL;

	memset(&tmp_l3vni, 0, sizeof(zebra_l3vni_t));
	tmp_l3vni.vni = vni;
	zl3vni = hash_lookup(zrouter.l3vni_table, &tmp_l3vni);

	return zl3vni;
}

/*
 * Add L3 VNI hash entry.
 */
static zebra_l3vni_t *zl3vni_add(vni_t vni, vrf_id_t vrf_id)
{
	zebra_l3vni_t tmp_zl3vni;
	zebra_l3vni_t *zl3vni = NULL;

	memset(&tmp_zl3vni, 0, sizeof(zebra_l3vni_t));
	tmp_zl3vni.vni = vni;

	zl3vni = hash_get(zrouter.l3vni_table, &tmp_zl3vni, zl3vni_alloc);
	assert(zl3vni);

	zl3vni->vrf_id = vrf_id;
	zl3vni->svi_if = NULL;
	zl3vni->vxlan_if = NULL;
	zl3vni->l2vnis = list_new();
	zl3vni->l2vnis->cmp = vni_list_cmp;

	/* Create hash table for remote RMAC */
	zl3vni->rmac_table = hash_create(mac_hash_keymake, mac_cmp,
					 "Zebra L3-VNI RMAC-Table");

	/* Create hash table for neighbors */
	zl3vni->nh_table = hash_create(neigh_hash_keymake, neigh_cmp,
				       "Zebra L3-VNI next-hop table");

	return zl3vni;
}

/*
 * Delete L3 VNI hash entry.
 */
static int zl3vni_del(zebra_l3vni_t *zl3vni)
{
	zebra_l3vni_t *tmp_zl3vni;

	/* free the list of l2vnis */
	list_delete(&zl3vni->l2vnis);
	zl3vni->l2vnis = NULL;

	/* Free the rmac table */
	hash_free(zl3vni->rmac_table);
	zl3vni->rmac_table = NULL;

	/* Free the nh table */
	hash_free(zl3vni->nh_table);
	zl3vni->nh_table = NULL;

	/* Free the VNI hash entry and allocated memory. */
	tmp_zl3vni = hash_release(zrouter.l3vni_table, zl3vni);
	XFREE(MTYPE_ZL3VNI, tmp_zl3vni);

	return 0;
}

struct interface *zl3vni_map_to_vxlan_if(zebra_l3vni_t *zl3vni)
{
	struct zebra_ns *zns = NULL;
	struct route_node *rn = NULL;
	struct interface *ifp = NULL;

	/* loop through all vxlan-interface */
	zns = zebra_ns_lookup(NS_DEFAULT);
	for (rn = route_top(zns->if_table); rn; rn = route_next(rn)) {

		struct zebra_if *zif = NULL;
		struct zebra_l2info_vxlan *vxl = NULL;

		ifp = (struct interface *)rn->info;
		if (!ifp)
			continue;

		zif = ifp->info;
		if (!zif || zif->zif_type != ZEBRA_IF_VXLAN)
			continue;

		vxl = &zif->l2info.vxl;
		if (vxl->vni == zl3vni->vni) {
			zl3vni->local_vtep_ip = vxl->vtep_ip;
			return ifp;
		}
	}

	return NULL;
}

struct interface *zl3vni_map_to_svi_if(zebra_l3vni_t *zl3vni)
{
	struct zebra_if *zif = NULL;	   /* zebra_if for vxlan_if */
	struct zebra_l2info_vxlan *vxl = NULL; /* l2 info for vxlan_if */

	if (!zl3vni)
		return NULL;

	if (!zl3vni->vxlan_if)
		return NULL;

	zif = zl3vni->vxlan_if->info;
	if (!zif)
		return NULL;

	vxl = &zif->l2info.vxl;

	return zvni_map_to_svi(vxl->access_vlan, zif->brslave_info.br_if);
}

struct interface *zl3vni_map_to_mac_vlan_if(zebra_l3vni_t *zl3vni)
{
	struct zebra_if *zif = NULL;	   /* zebra_if for vxlan_if */

	if (!zl3vni)
		return NULL;

	if (!zl3vni->vxlan_if)
		return NULL;

	zif = zl3vni->vxlan_if->info;
	if (!zif)
		return NULL;

	return zvni_map_to_macvlan(zif->brslave_info.br_if, zl3vni->svi_if);
}


zebra_l3vni_t *zl3vni_from_vrf(vrf_id_t vrf_id)
{
	struct zebra_vrf *zvrf = NULL;

	zvrf = zebra_vrf_lookup_by_id(vrf_id);
	if (!zvrf)
		return NULL;

	return zl3vni_lookup(zvrf->l3vni);
}

/*
 * Map SVI and associated bridge to a VNI. This is invoked upon getting
 * neighbor notifications, to see if they are of interest.
 */
static zebra_l3vni_t *zl3vni_from_svi(struct interface *ifp,
				      struct interface *br_if)
{
	int found = 0;
	vlanid_t vid = 0;
	uint8_t bridge_vlan_aware = 0;
	zebra_l3vni_t *zl3vni = NULL;
	struct zebra_ns *zns = NULL;
	struct route_node *rn = NULL;
	struct zebra_if *zif = NULL;
	struct interface *tmp_if = NULL;
	struct zebra_l2info_bridge *br = NULL;
	struct zebra_l2info_vxlan *vxl = NULL;

	if (!br_if)
		return NULL;

	/* Make sure the linked interface is a bridge. */
	if (!IS_ZEBRA_IF_BRIDGE(br_if))
		return NULL;

	/* Determine if bridge is VLAN-aware or not */
	zif = br_if->info;
	assert(zif);
	br = &zif->l2info.br;
	bridge_vlan_aware = br->vlan_aware;
	if (bridge_vlan_aware) {
		struct zebra_l2info_vlan *vl;

		if (!IS_ZEBRA_IF_VLAN(ifp))
			return NULL;

		zif = ifp->info;
		assert(zif);
		vl = &zif->l2info.vl;
		vid = vl->vid;
	}

	/* See if this interface (or interface plus VLAN Id) maps to a VxLAN */
	/* TODO: Optimize with a hash. */
	zns = zebra_ns_lookup(NS_DEFAULT);
	for (rn = route_top(zns->if_table); rn; rn = route_next(rn)) {
		tmp_if = (struct interface *)rn->info;
		if (!tmp_if)
			continue;
		zif = tmp_if->info;
		if (!zif || zif->zif_type != ZEBRA_IF_VXLAN)
			continue;
		if (!if_is_operative(tmp_if))
			continue;
		vxl = &zif->l2info.vxl;

		if (zif->brslave_info.br_if != br_if)
			continue;

		if (!bridge_vlan_aware || vxl->access_vlan == vid) {
			found = 1;
			break;
		}
	}

	if (!found)
		return NULL;

	zl3vni = zl3vni_lookup(vxl->vni);
	return zl3vni;
}

static inline void zl3vni_get_vrr_rmac(zebra_l3vni_t *zl3vni,
				       struct ethaddr *rmac)
{
	if (!zl3vni)
		return;

	if (!is_l3vni_oper_up(zl3vni))
		return;

	if (zl3vni->mac_vlan_if && if_is_operative(zl3vni->mac_vlan_if))
		memcpy(rmac->octet, zl3vni->mac_vlan_if->hw_addr, ETH_ALEN);
}

/*
 * Inform BGP about l3-vni.
 */
static int zl3vni_send_add_to_client(zebra_l3vni_t *zl3vni)
{
	struct stream *s = NULL;
	struct zserv *client = NULL;
	struct ethaddr svi_rmac, vrr_rmac = {.octet = {0} };
	struct zebra_vrf *zvrf;
	char buf[ETHER_ADDR_STRLEN];
	char buf1[ETHER_ADDR_STRLEN];
	bool is_anycast_mac = true;

	client = zserv_find_client(ZEBRA_ROUTE_BGP, 0);
	/* BGP may not be running. */
	if (!client)
		return 0;

	zvrf = zebra_vrf_lookup_by_id(zl3vni->vrf_id);
	assert(zvrf);

	/* get the svi and vrr rmac values */
	memset(&svi_rmac, 0, sizeof(struct ethaddr));
	zl3vni_get_svi_rmac(zl3vni, &svi_rmac);
	zl3vni_get_vrr_rmac(zl3vni, &vrr_rmac);

	/* In absence of vrr mac use svi mac as anycast MAC value */
	if (is_zero_mac(&vrr_rmac)) {
		memcpy(&vrr_rmac, &svi_rmac, ETH_ALEN);
		is_anycast_mac = false;
	}

	s = stream_new(ZEBRA_MAX_PACKET_SIZ);

	/* The message is used for both vni add and/or update like
	 * vrr mac is added for l3vni SVI.
	 */
	zclient_create_header(s, ZEBRA_L3VNI_ADD, zl3vni_vrf_id(zl3vni));
	stream_putl(s, zl3vni->vni);
	stream_put(s, &svi_rmac, sizeof(struct ethaddr));
	stream_put_in_addr(s, &zl3vni->local_vtep_ip);
	stream_put(s, &zl3vni->filter, sizeof(int));
	stream_putl(s, zl3vni->svi_if->ifindex);
	stream_put(s, &vrr_rmac, sizeof(struct ethaddr));
	stream_putl(s, is_anycast_mac);

	/* Write packet size. */
	stream_putw_at(s, 0, stream_get_endp(s));

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug(
			"Send L3_VNI_ADD %u VRF %s RMAC %s VRR %s local-ip %s filter %s to %s",
			zl3vni->vni, vrf_id_to_name(zl3vni_vrf_id(zl3vni)),
			prefix_mac2str(&svi_rmac, buf, sizeof(buf)),
			prefix_mac2str(&vrr_rmac, buf1, sizeof(buf1)),
			inet_ntoa(zl3vni->local_vtep_ip),
			CHECK_FLAG(zl3vni->filter, PREFIX_ROUTES_ONLY)
				? "prefix-routes-only"
				: "none",
			zebra_route_string(client->proto));

	client->l3vniadd_cnt++;
	return zserv_send_message(client, s);
}

/*
 * Inform BGP about local l3-VNI deletion.
 */
static int zl3vni_send_del_to_client(zebra_l3vni_t *zl3vni)
{
	struct stream *s = NULL;
	struct zserv *client = NULL;

	client = zserv_find_client(ZEBRA_ROUTE_BGP, 0);
	/* BGP may not be running. */
	if (!client)
		return 0;

	s = stream_new(ZEBRA_MAX_PACKET_SIZ);

	zclient_create_header(s, ZEBRA_L3VNI_DEL, zl3vni_vrf_id(zl3vni));
	stream_putl(s, zl3vni->vni);

	/* Write packet size. */
	stream_putw_at(s, 0, stream_get_endp(s));

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("Send L3_VNI_DEL %u VRF %s to %s", zl3vni->vni,
			   vrf_id_to_name(zl3vni_vrf_id(zl3vni)),
			   zebra_route_string(client->proto));

	client->l3vnidel_cnt++;
	return zserv_send_message(client, s);
}

static void zebra_vxlan_process_l3vni_oper_up(zebra_l3vni_t *zl3vni)
{
	if (!zl3vni)
		return;

	/* send l3vni add to BGP */
	zl3vni_send_add_to_client(zl3vni);
}

static void zebra_vxlan_process_l3vni_oper_down(zebra_l3vni_t *zl3vni)
{
	if (!zl3vni)
		return;

	/* send l3-vni del to BGP*/
	zl3vni_send_del_to_client(zl3vni);
}

static void zvni_add_to_l3vni_list(struct hash_bucket *bucket, void *ctxt)
{
	zebra_vni_t *zvni = (zebra_vni_t *)bucket->data;
	zebra_l3vni_t *zl3vni = (zebra_l3vni_t *)ctxt;

	if (zvni->vrf_id == zl3vni_vrf_id(zl3vni))
		listnode_add_sort(zl3vni->l2vnis, zvni);
}

/*
 *  handle transition of vni from l2 to l3 and vice versa
 */
static int zebra_vxlan_handle_vni_transition(struct zebra_vrf *zvrf, vni_t vni,
					     int add)
{
	zebra_vni_t *zvni = NULL;

	/* There is a possibility that VNI notification was already received
	 * from kernel and we programmed it as L2-VNI
	 * In such a case we need to delete this L2-VNI first, so
	 * that it can be reprogrammed as L3-VNI in the system. It is also
	 * possible that the vrf-vni mapping is removed from FRR while the vxlan
	 * interface is still present in kernel. In this case to keep it
	 * symmetric, we will delete the l3-vni and reprogram it as l2-vni
	 */
	if (add) {
		/* Locate hash entry */
		zvni = zvni_lookup(vni);
		if (!zvni)
			return 0;

		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("Del L2-VNI %u - transition to L3-VNI", vni);

		/* Delete VNI from BGP. */
		zvni_send_del_to_client(zvni);

		/* Free up all neighbors and MAC, if any. */
		zvni_neigh_del_all(zvni, 0, 0, DEL_ALL_NEIGH);
		zvni_mac_del_all(zvni, 0, 0, DEL_ALL_MAC);

		/* Free up all remote VTEPs, if any. */
		zvni_vtep_del_all(zvni, 0);

		/* Delete the hash entry. */
		if (zvni_del(zvni)) {
			flog_err(EC_ZEBRA_VNI_DEL_FAILED,
				 "Failed to del VNI hash %p, VNI %u", zvni,
				 zvni->vni);
			return -1;
		}
	} else {
		/* TODO_MITESH: This needs to be thought through. We don't have
		 * enough information at this point to reprogram the vni as
		 * l2-vni. One way is to store the required info in l3-vni and
		 * used it solely for this purpose
		 */
	}

	return 0;
}

/* delete and uninstall rmac hash entry */
static void zl3vni_del_rmac_hash_entry(struct hash_bucket *bucket, void *ctx)
{
	zebra_mac_t *zrmac = NULL;
	zebra_l3vni_t *zl3vni = NULL;

	zrmac = (zebra_mac_t *)bucket->data;
	zl3vni = (zebra_l3vni_t *)ctx;
	zl3vni_rmac_uninstall(zl3vni, zrmac);

	/* Send RMAC for FPM processing */
	hook_call(zebra_rmac_update, zrmac, zl3vni, true, "RMAC deleted");

	zl3vni_rmac_del(zl3vni, zrmac);
}

/* delete and uninstall nh hash entry */
static void zl3vni_del_nh_hash_entry(struct hash_bucket *bucket, void *ctx)
{
	zebra_neigh_t *n = NULL;
	zebra_l3vni_t *zl3vni = NULL;

	n = (zebra_neigh_t *)bucket->data;
	zl3vni = (zebra_l3vni_t *)ctx;
	zl3vni_nh_uninstall(zl3vni, n);
	zl3vni_nh_del(zl3vni, n);
}

static int ip_prefix_send_to_client(vrf_id_t vrf_id, struct prefix *p,
				    uint16_t cmd)
{
	struct zserv *client = NULL;
	struct stream *s = NULL;
	char buf[PREFIX_STRLEN];

	client = zserv_find_client(ZEBRA_ROUTE_BGP, 0);
	/* BGP may not be running. */
	if (!client)
		return 0;

	s = stream_new(ZEBRA_MAX_PACKET_SIZ);

	zclient_create_header(s, cmd, vrf_id);
	stream_put(s, p, sizeof(struct prefix));

	/* Write packet size. */
	stream_putw_at(s, 0, stream_get_endp(s));

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("Send ip prefix %s %s on vrf %s",
			   prefix2str(p, buf, sizeof(buf)),
			   (cmd == ZEBRA_IP_PREFIX_ROUTE_ADD) ? "ADD" : "DEL",
			   vrf_id_to_name(vrf_id));

	if (cmd == ZEBRA_IP_PREFIX_ROUTE_ADD)
		client->prefixadd_cnt++;
	else
		client->prefixdel_cnt++;

	return zserv_send_message(client, s);
}

/* re-add remote rmac if needed */
static int zebra_vxlan_readd_remote_rmac(zebra_l3vni_t *zl3vni,
					 struct ethaddr *rmac)
{
	char buf[ETHER_ADDR_STRLEN];
	zebra_mac_t *zrmac = NULL;

	zrmac = zl3vni_rmac_lookup(zl3vni, rmac);
	if (!zrmac)
		return 0;

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("Del remote RMAC %s L3VNI %u - readd",
			   prefix_mac2str(rmac, buf, sizeof(buf)), zl3vni->vni);

	zl3vni_rmac_install(zl3vni, zrmac);
	return 0;
}

/**************************** SYNC MAC handling *****************************/
/* if the mac has been added of a mac-route from the peer
 * or if it is being referenced by a neigh added by the
 * peer we cannot let it age out i.e. we set the static bit
 * in the dataplane
 */
static inline bool zebra_vxlan_mac_is_static(zebra_mac_t *mac)
{
	return ((mac->flags & ZEBRA_MAC_ALL_PEER_FLAGS) ||
			mac->sync_neigh_cnt);
}

/* mac needs to be locally active or active on an ES peer */
static inline bool zebra_vxlan_mac_is_ready_for_bgp(uint32_t flags)
{
	return (flags & ZEBRA_MAC_LOCAL) &&
		(!(flags & ZEBRA_MAC_LOCAL_INACTIVE) ||
		 (flags & ZEBRA_MAC_ES_PEER_ACTIVE));
}

/* program sync mac flags in the dataplane  */
void zebra_vxlan_sync_mac_dp_install(zebra_mac_t *mac, bool set_inactive,
		bool force_clear_static, const char *caller)
{
	char macbuf[ETHER_ADDR_STRLEN];
	struct interface *ifp;
	bool sticky;
	bool set_static;
	zebra_vni_t *zvni = mac->zvni;
	vlanid_t vid;
	struct zebra_if *zif;
	struct interface *br_ifp;

	/* get the access vlan from the vxlan_device */
	zebra_vxlan_mac_get_access_info(mac,
			&ifp, &vid);

	if (!ifp) {
		if (IS_ZEBRA_DEBUG_EVPN_MH_MAC)
			zlog_debug("%s: dp-install sync-mac vni %u mac %s es %s 0x%x %sskipped, no access-port",
					caller,
					zvni->vni,
					prefix_mac2str(&mac->macaddr, macbuf,
						sizeof(macbuf)),
					mac->es ?
					mac->es->esi_str : "-",
					mac->flags,
					set_inactive ? "inactive " : "");
		return;
	}

	zif = ifp->info;
	br_ifp = zif->brslave_info.br_if;
	if (!br_ifp) {
		if (IS_ZEBRA_DEBUG_EVPN_MH_MAC)
			zlog_debug("%s: dp-install sync-mac vni %u mac %s es %s 0x%x %sskipped, no br",
					caller,
					zvni->vni,
					prefix_mac2str(&mac->macaddr, macbuf,
						sizeof(macbuf)),
					mac->es ?
					mac->es->esi_str : "-",
					mac->flags,
					set_inactive ? "inactive " : "");
		return;
	}

	sticky = !!CHECK_FLAG(mac->flags, ZEBRA_MAC_STICKY);
	if (force_clear_static)
		set_static = false;
	else
		set_static = zebra_vxlan_mac_is_static(mac);

	if (IS_ZEBRA_DEBUG_EVPN_MH_MAC)
		zlog_debug("dp-install sync-mac vni %u mac %s es %s 0x%x %s%s",
				zvni->vni,
				prefix_mac2str(&mac->macaddr, macbuf,
					sizeof(macbuf)),
				mac->es ?
				mac->es->esi_str : "-", mac->flags,
				set_static ? "static " : "",
				set_inactive ? "inactive " : "");

	dplane_local_mac_add(ifp, br_ifp, vid, &mac->macaddr, sticky,
			set_static, set_inactive);

}

static void zebra_vxlan_mac_send_add_del_to_client(zebra_mac_t *mac,
	bool old_bgp_ready, bool new_bgp_ready)
{
	if (new_bgp_ready)
		zvni_mac_send_add_to_client(mac->zvni->vni,
				&mac->macaddr, mac->flags,
				mac->loc_seq, mac->es);
	else if (old_bgp_ready)
		zvni_mac_send_del_to_client(mac->zvni->vni,
				&mac->macaddr, mac->flags,
				true /* force */);
}

/* MAC hold timer is used to age out peer-active flag.
 *
 * During this wait time we expect the dataplane component or an
 * external neighmgr daemon to probe existing hosts to independently
 * establish their presence on the ES.
 */
static int zebra_vxlan_mac_hold_exp_cb(struct thread *t)
{
	zebra_mac_t *mac;
	bool old_bgp_ready;
	bool new_bgp_ready;
	bool old_static;
	bool new_static;
	char macbuf[ETHER_ADDR_STRLEN];

	mac = THREAD_ARG(t);
	/* the purpose of the hold timer is to age out the peer-active
	 * flag
	 */
	if (!CHECK_FLAG(mac->flags, ZEBRA_MAC_ES_PEER_ACTIVE))
		return 0;

	old_bgp_ready = zebra_vxlan_mac_is_ready_for_bgp(mac->flags);
	old_static = zebra_vxlan_mac_is_static(mac);
	UNSET_FLAG(mac->flags, ZEBRA_MAC_ES_PEER_ACTIVE);
	new_bgp_ready = zebra_vxlan_mac_is_ready_for_bgp(mac->flags);
	new_static = zebra_vxlan_mac_is_static(mac);

	if (IS_ZEBRA_DEBUG_EVPN_MH_MAC)
		zlog_debug("sync-mac vni %u mac %s es %s 0x%x hold expired",
				mac->zvni->vni,
				prefix_mac2str(&mac->macaddr, macbuf,
					sizeof(macbuf)),
				mac->es ?
				mac->es->esi_str : "-",
				mac->flags);

	/* re-program the local mac in the dataplane if the mac is no
	 * longer static
	 */
	if (old_static != new_static)
		zebra_vxlan_sync_mac_dp_install(mac, false /* set_inactive */,
				false /* force_clear_static */, __func__);

	/* inform bgp if needed */
	if (old_bgp_ready != new_bgp_ready)
		zebra_vxlan_mac_send_add_del_to_client(mac,
				old_bgp_ready, new_bgp_ready);

	return 0;
}

static inline void zebra_vxlan_mac_start_hold_timer(zebra_mac_t *mac)
{
	char macbuf[ETHER_ADDR_STRLEN];

	if (mac->hold_timer)
		return;

	if (IS_ZEBRA_DEBUG_EVPN_MH_MAC)
		zlog_debug("sync-mac vni %u mac %s es %s 0x%x hold started",
				mac->zvni->vni,
				prefix_mac2str(&mac->macaddr, macbuf,
					sizeof(macbuf)),
				mac->es ?
				mac->es->esi_str : "-",
				mac->flags);
	thread_add_timer(zrouter.master,
			zebra_vxlan_mac_hold_exp_cb,
			mac, zmh_info->mac_hold_time,
			&mac->hold_timer);
}

static inline void zebra_vxlan_mac_stop_hold_timer(zebra_mac_t *mac)
{
	char macbuf[ETHER_ADDR_STRLEN];

	if (!mac->hold_timer)
		return;

	if (IS_ZEBRA_DEBUG_EVPN_MH_MAC)
		zlog_debug("sync-mac vni %u mac %s es %s 0x%x hold stopped",
				mac->zvni->vni,
				prefix_mac2str(&mac->macaddr, macbuf,
					sizeof(macbuf)),
				mac->es ?
				mac->es->esi_str : "-",
				mac->flags);
	THREAD_OFF(mac->hold_timer);
}

static inline void zebra_vxlan_mac_clear_sync_info(zebra_mac_t *mac)
{
	UNSET_FLAG(mac->flags, ZEBRA_MAC_ALL_PEER_FLAGS);
	zebra_vxlan_mac_stop_hold_timer(mac);
}

static void zebra_vxlan_sync_mac_del(zebra_mac_t *mac)
{
	char macbuf[ETHER_ADDR_STRLEN];
	bool old_static;
	bool new_static;

	if (IS_ZEBRA_DEBUG_EVPN_MH_MAC)
		zlog_debug("sync-mac del vni %u mac %s es %s seq %d f 0x%x",
				mac->zvni->vni,
				prefix_mac2str(&mac->macaddr,
					macbuf, sizeof(macbuf)),
				mac->es ? mac->es->esi_str : "-",
				mac->loc_seq,
				mac->flags);
	old_static = zebra_vxlan_mac_is_static(mac);
	UNSET_FLAG(mac->flags, ZEBRA_MAC_ES_PEER_PROXY);
	if (CHECK_FLAG(mac->flags, ZEBRA_MAC_ES_PEER_ACTIVE))
		zebra_vxlan_mac_start_hold_timer(mac);
	new_static = zebra_vxlan_mac_is_static(mac);

	if (old_static != new_static)
		/* program the local mac in the kernel */
		zebra_vxlan_sync_mac_dp_install(mac, false /* set_inactive */,
				false /* force_clear_static */, __func__);
}

static inline bool zebra_vxlan_mac_is_bgp_seq_ok(zebra_vni_t *zvni,
		zebra_mac_t *mac, uint32_t seq, uint16_t ipa_len,
		struct ipaddr *ipaddr)
{
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];
	uint32_t tmp_seq;

	if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL))
		tmp_seq = mac->loc_seq;
	else
		tmp_seq = mac->rem_seq;

	if (seq < tmp_seq) {
		/* if the mac was never advertised to bgp we must accept
		 * whatever sequence number bgp sends
		 * XXX - check with Vivek
		 */
		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL) &&
				!zebra_vxlan_mac_is_ready_for_bgp(mac->flags)) {
			if (IS_ZEBRA_DEBUG_EVPN_MH_MAC)
				zlog_debug("sync-macip accept vni %u mac %s%s%s lower seq %u f 0x%x",
						zvni->vni,
						prefix_mac2str(&mac->macaddr,
							macbuf, sizeof(macbuf)),
						ipa_len ? " IP " : "",
						ipa_len ?
						ipaddr2str(ipaddr,
							ipbuf, sizeof(ipbuf)) : "",
						tmp_seq, mac->flags);
			return true;
		}

		if (IS_ZEBRA_DEBUG_EVPN_MH_MAC)
			zlog_debug("sync-macip ignore vni %u mac %s%s%s as existing has higher seq %u f 0x%x",
					zvni->vni,
					prefix_mac2str(&mac->macaddr,
						macbuf, sizeof(macbuf)),
					ipa_len ? " IP " : "",
					ipa_len ?
					ipaddr2str(ipaddr,
						ipbuf, sizeof(ipbuf)) : "",
					tmp_seq, mac->flags);
		return false;
	}

	return true;
}

/* sync-path that is active on an ES peer */
static zebra_mac_t *zebra_vxlan_proc_sync_mac_update(zebra_vni_t *zvni,
		struct ethaddr *macaddr, uint16_t ipa_len,
		struct ipaddr *ipaddr, uint8_t flags,
		uint32_t seq, esi_t *esi,
		struct sync_mac_ip_ctx *ctx)
{
	zebra_mac_t *mac;
	bool inform_bgp = false;
	bool inform_dataplane = false;
	bool seq_change = false;
	bool es_change = false;
	uint32_t tmp_seq;
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];
	bool old_local = false;
	bool old_bgp_ready;
	bool new_bgp_ready;

	mac = zvni_mac_lookup(zvni, macaddr);
	if (!mac) {
		/* if it is a new local path we need to inform both
		 * the control protocol and the data-plane
		 */
		inform_bgp = true;
		inform_dataplane = true;
		ctx->mac_created = true;
		ctx->mac_inactive =  true;

		/* create the MAC and associate it with the dest ES */
		mac = zvni_mac_add(zvni, macaddr);
		zebra_evpn_es_mac_ref(mac, esi);

		/* local mac activated by an ES peer */
		SET_FLAG(mac->flags, ZEBRA_MAC_LOCAL);
		/* if mac-only route setup peer flags */
		if (!ipa_len) {
			if (CHECK_FLAG(flags, ZEBRA_MACIP_TYPE_PROXY_ADVERT))
				SET_FLAG(mac->flags, ZEBRA_MAC_ES_PEER_PROXY);
			else
				SET_FLAG(mac->flags, ZEBRA_MAC_ES_PEER_ACTIVE);
		}
		SET_FLAG(mac->flags, ZEBRA_MAC_LOCAL_INACTIVE);
		old_bgp_ready = false;
		new_bgp_ready = zebra_vxlan_mac_is_ready_for_bgp(mac->flags);
	} else {
		uint32_t old_flags;
		uint32_t new_flags;
		bool old_static;
		bool new_static;
		bool sticky;
		bool remote_gw;

		old_flags = mac->flags;
		sticky = !!CHECK_FLAG(old_flags, ZEBRA_MAC_STICKY);
		remote_gw = !!CHECK_FLAG(old_flags, ZEBRA_MAC_REMOTE_DEF_GW);
		if (sticky || remote_gw) {
			if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
				zlog_debug("Ignore sync-macip vni %u mac %s%s%s%s%s",
					zvni->vni,
					prefix_mac2str(macaddr,
						macbuf, sizeof(macbuf)),
					ipa_len ? " IP " : "",
					ipa_len ?
					ipaddr2str(ipaddr, ipbuf,
						sizeof(ipbuf)) : "",
					sticky ? " sticky" : "",
					remote_gw ? " remote_gw" : "");
			ctx->ignore_macip = true;
			return NULL;
		}
		if (!zebra_vxlan_mac_is_bgp_seq_ok(zvni, mac, seq,
					ipa_len, ipaddr)) {
			ctx->ignore_macip = true;
			return NULL;
		}

		old_local = !!CHECK_FLAG(old_flags, ZEBRA_MAC_LOCAL);
		old_static = zebra_vxlan_mac_is_static(mac);

		/* re-build the mac flags */
		new_flags = 0;
		SET_FLAG(new_flags, ZEBRA_MAC_LOCAL);
		/* retain old local activity flag */
		if (old_flags & ZEBRA_MAC_LOCAL) {
			new_flags |= (old_flags & ZEBRA_MAC_LOCAL_INACTIVE);
		} else {
			new_flags |= ZEBRA_MAC_LOCAL_INACTIVE;
			ctx->mac_inactive =  true;
		}
		if (ipa_len) {
			/* if mac-ip route do NOT update the peer flags
			 * i.e. retain only flags as is
			 */
			new_flags |= (old_flags & ZEBRA_MAC_ALL_PEER_FLAGS);
		} else {
			/* if mac-only route update peer flags */
			if (CHECK_FLAG(flags, ZEBRA_MACIP_TYPE_PROXY_ADVERT)) {
				SET_FLAG(new_flags, ZEBRA_MAC_ES_PEER_PROXY);
				/* if the mac was peer-active previously we
				 * need to keep the flag and start the
				 * holdtimer on it. the peer-active flag is
				 * cleared on holdtimer expiry.
				 */
				if (CHECK_FLAG(old_flags,
						ZEBRA_MAC_ES_PEER_ACTIVE)) {
					SET_FLAG(new_flags,
						ZEBRA_MAC_ES_PEER_ACTIVE);
					zebra_vxlan_mac_start_hold_timer(mac);
				}
			} else {
				SET_FLAG(new_flags, ZEBRA_MAC_ES_PEER_ACTIVE);
				/* stop hold timer if a peer has verified
				 * reachability
				 */
				zebra_vxlan_mac_stop_hold_timer(mac);
			}
		}
		mac->rem_seq = 0;
		memset(&mac->fwd_info, 0, sizeof(mac->fwd_info));
		mac->flags = new_flags;

		if (IS_ZEBRA_DEBUG_EVPN_MH_MAC &&
				(old_flags != new_flags))
			zlog_debug("sync-mac vni %u mac %s old_f 0x%x new_f 0x%x",
					zvni->vni,
					prefix_mac2str(macaddr,
						macbuf, sizeof(macbuf)),
					old_flags, mac->flags);

		/* update es */
		es_change = zebra_evpn_es_mac_ref(mac, esi);
		/* if mac dest change - inform both sides */
		if (es_change) {
			inform_bgp = true;
			inform_dataplane = true;
			ctx->mac_inactive =  true;
		}
		/* if peer-flag is being set notify dataplane that the
		 * entry must not be expired because of local inactivity
		 */
		new_static = zebra_vxlan_mac_is_static(mac);
		if (old_static != new_static)
			inform_dataplane = true;

		old_bgp_ready = zebra_vxlan_mac_is_ready_for_bgp(old_flags);
		new_bgp_ready = zebra_vxlan_mac_is_ready_for_bgp(mac->flags);
		if (old_bgp_ready != new_bgp_ready)
			inform_bgp = true;
	}


	/* update sequence number; if that results in a new local sequence
	 * inform bgp
	 */
	tmp_seq = MAX(mac->loc_seq, seq);
	if (tmp_seq != mac->loc_seq) {
		mac->loc_seq = tmp_seq;
		seq_change = true;
		inform_bgp = true;
	}

	if (IS_ZEBRA_DEBUG_EVPN_MH_MAC)
		zlog_debug("sync-mac %s vni %u mac %s es %s seq %d f 0x%x%s%s",
				ctx->mac_created ?
				"created" : "updated",
				zvni->vni,
				prefix_mac2str(macaddr,
					macbuf, sizeof(macbuf)),
				mac->es ? mac->es->esi_str : "-",
				mac->loc_seq, mac->flags,
				inform_bgp ? " inform_bgp" : "",
				inform_dataplane ? " inform_dp" : "");

	if (inform_bgp)
		zebra_vxlan_mac_send_add_del_to_client(mac,
				old_bgp_ready, new_bgp_ready);

	/* neighs using the mac may need to be re-sent to
	 * bgp with updated info
	 */
	if (seq_change || es_change || !old_local)
		zvni_process_neigh_on_local_mac_change(zvni, mac,
				seq_change, es_change);

	if (inform_dataplane) {
		if (ipa_len)
			/* if the mac is being created as a part of MAC-IP
			 * route wait for the neigh to be updated or
			 * created before programming the mac
			 */
			ctx->mac_dp_update_deferred = true;
		else
			/* program the local mac in the kernel. when the ES
			 * change we need to force the dataplane to reset
			 * the activity as we are yet to establish activity
			 * locally
			 */
			zebra_vxlan_sync_mac_dp_install(mac,
					ctx->mac_inactive,
					false /* force_clear_static */,
					__func__);
	}

	return mac;
}

/**************************** SYNC neigh handling **************************/
static inline bool zebra_vxlan_neigh_is_static(zebra_neigh_t *neigh)
{
	return !!(neigh->flags & ZEBRA_NEIGH_ALL_PEER_FLAGS);
}

static inline bool zebra_vxlan_neigh_is_ready_for_bgp(zebra_neigh_t *n)
{
	bool mac_ready;
	bool neigh_ready;

	mac_ready = !!(n->mac->flags & ZEBRA_MAC_LOCAL);
	neigh_ready = ((n->flags & ZEBRA_NEIGH_LOCAL) &&
			IS_ZEBRA_NEIGH_ACTIVE(n) &&
			(!(n->flags & ZEBRA_NEIGH_LOCAL_INACTIVE) ||
			 (n->flags & ZEBRA_NEIGH_ES_PEER_ACTIVE))) ?
		true : false;

	return mac_ready && neigh_ready;
}

static void zebra_vxlan_sync_neigh_dp_install(zebra_neigh_t *n,
		bool set_inactive, bool force_clear_static, const char *caller)
{
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];
	struct zebra_ns *zns;
	struct interface *ifp;
	bool set_static;
	bool set_router;

	zns = zebra_ns_lookup(NS_DEFAULT);
	ifp = if_lookup_by_index_per_ns(zns, n->ifindex);
	if (!ifp) {
		if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
			zlog_debug("%s: dp-install sync-neigh vni %u ip %s mac %s if %d f 0x%x skipped",
				caller, n->zvni->vni,
				ipaddr2str(&n->ip, ipbuf, sizeof(ipbuf)),
				prefix_mac2str(&n->emac, macbuf,
					sizeof(macbuf)),
				n->ifindex, n->flags);
		return;
	}

	if (force_clear_static)
		set_static = false;
	else
		set_static = zebra_vxlan_neigh_is_static(n);

	set_router = !!CHECK_FLAG(n->flags, ZEBRA_NEIGH_ROUTER_FLAG);

	/* XXX - this will change post integration with the new kernel */
	if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL_INACTIVE))
		set_inactive = true;

	if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
		zlog_debug("%s: dp-install sync-neigh vni %u ip %s mac %s if %s(%d) f 0x%x%s%s%s",
				caller, n->zvni->vni,
				ipaddr2str(&n->ip, ipbuf, sizeof(ipbuf)),
				prefix_mac2str(&n->emac, macbuf,
					sizeof(macbuf)),
				ifp->name, n->ifindex, n->flags,
				set_router ? " router":"",
				set_static ? " static":"",
				set_inactive ? " inactive":"");
	dplane_local_neigh_add(ifp, &n->ip,
			&n->emac, set_router, set_static, set_inactive);
}

static void zebra_vxlan_neigh_send_add_del_to_client(zebra_neigh_t *n,
		bool old_bgp_ready, bool new_bgp_ready)
{
	if (new_bgp_ready)
		zvni_neigh_send_add_to_client(n->zvni->vni, &n->ip,
			&n->emac, n->mac, n->flags, n->loc_seq);
	else if (old_bgp_ready)
		zvni_neigh_send_del_to_client(n->zvni->vni, &n->ip,
			&n->emac, n->flags, n->state, true /*force*/);
}

/* if the static flag associated with the neigh changes we need
 * to update the sync-neigh references against the MAC
 * and inform the dataplane about the static flag changes.
 */
static void zebra_vxlan_sync_neigh_static_chg(zebra_neigh_t *n,
		bool old_n_static, bool new_n_static,
		bool defer_n_dp, bool defer_mac_dp,
		const char *caller)
{
	zebra_mac_t *mac = n->mac;
	bool old_mac_static;
	bool new_mac_static;
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];

	if (old_n_static == new_n_static)
		return;

	/* update the neigh sync references in the dataplane. if
	 * the neigh is in the middle of updates the caller can
	 * request for a defer
	 */
	if (!defer_n_dp)
		zebra_vxlan_sync_neigh_dp_install(n, false /* set_inactive */,
				false /* force_clear_static */, __func__);

	if (!mac)
		return;

	/* update the mac sync ref cnt */
	old_mac_static = zebra_vxlan_mac_is_static(mac);
	if (new_n_static) {
		++mac->sync_neigh_cnt;
	} else if (old_n_static) {
		if (mac->sync_neigh_cnt)
			--mac->sync_neigh_cnt;
	}
	new_mac_static = zebra_vxlan_mac_is_static(mac);

	/* update the mac sync references in the dataplane */
	if ((old_mac_static != new_mac_static) && !defer_mac_dp)
		zebra_vxlan_sync_mac_dp_install(mac,
				false /* set_inactive */,
				false /* force_clear_static */,
				__func__);

	if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
		zlog_debug("sync-neigh ref-chg vni %u ip %s mac %s f 0x%x %d%s%s%s%s by %s",
			n->zvni->vni,
			ipaddr2str(&n->ip, ipbuf, sizeof(ipbuf)),
			prefix_mac2str(&n->emac, macbuf,
				sizeof(macbuf)),
			n->flags, mac->sync_neigh_cnt,
			old_n_static ? " old_n_static" : "",
			new_n_static ? " new_n_static" : "",
			old_mac_static ? " old_mac_static" : "",
			new_mac_static ? " new_mac_static" : "",
			caller);
}

/* Neigh hold timer is used to age out peer-active flag.
 *
 * During this wait time we expect the dataplane component or an
 * external neighmgr daemon to probe existing hosts to independently
 * establish their presence on the ES.
 */
static int zebra_vxlan_neigh_hold_exp_cb(struct thread *t)
{
	zebra_neigh_t *n;
	bool old_bgp_ready;
	bool new_bgp_ready;
	bool old_n_static;
	bool new_n_static;
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];

	n = THREAD_ARG(t);
	/* the purpose of the hold timer is to age out the peer-active
	 * flag
	 */
	if (!CHECK_FLAG(n->flags, ZEBRA_NEIGH_ES_PEER_ACTIVE))
		return 0;

	old_bgp_ready = zebra_vxlan_neigh_is_ready_for_bgp(n);
	old_n_static = zebra_vxlan_neigh_is_static(n);
	UNSET_FLAG(n->flags, ZEBRA_NEIGH_ES_PEER_ACTIVE);
	new_bgp_ready = zebra_vxlan_neigh_is_ready_for_bgp(n);
	new_n_static = zebra_vxlan_neigh_is_static(n);

	if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
		zlog_debug("sync-neigh vni %u ip %s mac %s 0x%x hold expired",
				n->zvni->vni,
				ipaddr2str(&n->ip, ipbuf, sizeof(ipbuf)),
				prefix_mac2str(&n->emac, macbuf,
					sizeof(macbuf)),
				n->flags);

	/* re-program the local neigh in the dataplane if the neigh is no
	 * longer static
	 */
	if (old_n_static != new_n_static)
		zebra_vxlan_sync_neigh_static_chg(n, old_n_static,
			new_n_static, false /*defer_n_dp*/,
			false /*defer_mac_dp*/, __func__);

	/* inform bgp if needed */
	if (old_bgp_ready != new_bgp_ready)
		zebra_vxlan_neigh_send_add_del_to_client(n,
				old_bgp_ready, new_bgp_ready);

	return 0;
}

static inline void zebra_vxlan_neigh_start_hold_timer(zebra_neigh_t *n)
{
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];

	if (n->hold_timer)
		return;

	if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
		zlog_debug("sync-neigh vni %u ip %s mac %s 0x%x hold start",
				n->zvni->vni,
				ipaddr2str(&n->ip, ipbuf, sizeof(ipbuf)),
				prefix_mac2str(&n->emac, macbuf,
					sizeof(macbuf)),
				n->flags);
	thread_add_timer(zrouter.master,
			zebra_vxlan_neigh_hold_exp_cb,
			n, zmh_info->neigh_hold_time,
			&n->hold_timer);
}

static inline void zebra_vxlan_neigh_stop_hold_timer(zebra_neigh_t *n)
{
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];

	if (!n->hold_timer)
		return;

	if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
		zlog_debug("sync-neigh vni %u ip %s mac %s 0x%x hold stop",
				n->zvni->vni,
				ipaddr2str(&n->ip, ipbuf, sizeof(ipbuf)),
				prefix_mac2str(&n->emac, macbuf,
					sizeof(macbuf)),
				n->flags);
	THREAD_OFF(n->hold_timer);
}

static inline bool zebra_vxlan_neigh_clear_sync_info(zebra_neigh_t *n)
{
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];
	bool old_n_static = false;
	bool new_n_static = false;

	if (n->flags & ZEBRA_NEIGH_ALL_PEER_FLAGS) {
		if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
			zlog_debug("sync-neigh vni %u ip %s mac %s 0x%x clear",
				n->zvni->vni,
				ipaddr2str(&n->ip, ipbuf, sizeof(ipbuf)),
				prefix_mac2str(&n->emac, macbuf,
					sizeof(macbuf)),
				n->flags);

		old_n_static = zebra_vxlan_neigh_is_static(n);
		UNSET_FLAG(n->flags, ZEBRA_NEIGH_ALL_PEER_FLAGS);
		new_n_static = zebra_vxlan_neigh_is_static(n);
		if (old_n_static != new_n_static)
			zebra_vxlan_sync_neigh_static_chg(n, old_n_static,
				new_n_static, true /*defer_dp)*/,
				false/*defer_mac_dp*/, __func__);
	}
	zebra_vxlan_neigh_stop_hold_timer(n);

	/* if the neigh static flag changed inform that a dp
	 * re-install maybe needed
	 */
	return old_n_static != new_n_static;
}

static void zebra_vxlan_local_neigh_deref_mac(zebra_neigh_t *n,
		bool send_mac_update)
{
	zebra_mac_t *mac = n->mac;
	zebra_vni_t *zvni = n->zvni;
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];
	bool old_static;
	bool new_static;

	n->mac = NULL;
	if (!mac)
		return;

	if ((n->flags & ZEBRA_NEIGH_ALL_PEER_FLAGS) &&
			mac->sync_neigh_cnt){
		old_static = zebra_vxlan_mac_is_static(mac);
		--mac->sync_neigh_cnt;
		new_static = zebra_vxlan_mac_is_static(mac);
		if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
			zlog_debug("sync-neigh deref mac vni %u ip %s mac %s ref %d",
					n->zvni->vni,
					ipaddr2str(&n->ip, ipbuf,
						sizeof(ipbuf)),
					prefix_mac2str(&n->emac, macbuf,
						sizeof(macbuf)),
					mac->sync_neigh_cnt);
		if ((old_static != new_static) && send_mac_update)
			/* program the local mac in the kernel */
			zebra_vxlan_sync_mac_dp_install(mac,
					false /* set_inactive */,
					false /* force_clear_static */,
					__func__);
	}

	listnode_delete(mac->neigh_list, n);
	zvni_deref_ip2mac(zvni, mac);
}

static void zebra_vxlan_local_neigh_ref_mac(zebra_neigh_t *n,
		struct ethaddr *macaddr, zebra_mac_t *mac,
		bool send_mac_update)
{
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];
	bool old_static;
	bool new_static;

	memcpy(&n->emac, macaddr, ETH_ALEN);
	n->mac = mac;

	/* Link to new MAC */
	if (!mac)
		return;

	listnode_add_sort(mac->neigh_list, n);
	if (n->flags & ZEBRA_NEIGH_ALL_PEER_FLAGS) {
		old_static = zebra_vxlan_mac_is_static(mac);
		++mac->sync_neigh_cnt;
		new_static = zebra_vxlan_mac_is_static(mac);
		if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
			zlog_debug("sync-neigh ref mac vni %u ip %s mac %s ref %d",
					n->zvni->vni,
					ipaddr2str(&n->ip, ipbuf,
						sizeof(ipbuf)),
					prefix_mac2str(&n->emac, macbuf,
						sizeof(macbuf)),
					mac->sync_neigh_cnt);
		if ((old_static != new_static) && send_mac_update)
			/* program the local mac in the kernel */
			zebra_vxlan_sync_mac_dp_install(mac,
					false /*set_inactive*/,
					false /*force_clear_static*/,
					__func__);
	}
}

static inline bool zebra_vxlan_neigh_is_bgp_seq_ok(zebra_vni_t *zvni,
		zebra_neigh_t *n, struct ethaddr *macaddr, uint32_t seq)
{
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];
	uint32_t tmp_seq;

	if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL))
		tmp_seq = n->loc_seq;
	else
		tmp_seq = n->rem_seq;

	if (seq < tmp_seq) {
		/* if the neigh was never advertised to bgp we must accept
		 * whatever sequence number bgp sends
		 * XXX - check with Vivek
		 */
		if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL) &&
				!zebra_vxlan_neigh_is_ready_for_bgp(n)) {
			if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
				zlog_debug("sync-macip accept vni %u mac %s IP %s lower seq %u f 0x%x",
					zvni->vni,
					prefix_mac2str(macaddr,
						macbuf, sizeof(macbuf)),
					ipaddr2str(&n->ip,
						ipbuf, sizeof(ipbuf)),
					tmp_seq, n->flags);
			return true;
		}

		if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
			zlog_debug("sync-macip ignore vni %u mac %s IP %s as existing has higher seq %u f 0x%x",
				zvni->vni,
				prefix_mac2str(macaddr,
					macbuf, sizeof(macbuf)),
				ipaddr2str(&n->ip,
						ipbuf, sizeof(ipbuf)),
				tmp_seq, n->flags);
		return false;
	}

	return true;
}

static void zebra_vxlan_sync_neigh_del(zebra_neigh_t *n)
{
	bool old_n_static;
	bool new_n_static;
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];

	if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
		zlog_debug("sync-neigh del vni %u ip %s mac %s f 0x%x",
				n->zvni->vni,
				ipaddr2str(&n->ip, ipbuf, sizeof(ipbuf)),
				prefix_mac2str(&n->emac, macbuf,
					sizeof(macbuf)),
				n->flags);

	old_n_static = zebra_vxlan_neigh_is_static(n);
	UNSET_FLAG(n->flags, ZEBRA_NEIGH_ES_PEER_PROXY);
	if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_ES_PEER_ACTIVE))
		zebra_vxlan_neigh_start_hold_timer(n);
	new_n_static = zebra_vxlan_neigh_is_static(n);

	if (old_n_static != new_n_static)
		zebra_vxlan_sync_neigh_static_chg(n, old_n_static,
			new_n_static, false /*defer-dp*/,
			false /*defer_mac_dp*/, __func__);
}

static zebra_neigh_t *zebra_vxlan_proc_sync_neigh_update(zebra_vni_t *zvni,
		zebra_neigh_t *n, uint16_t ipa_len,
		struct ipaddr *ipaddr, uint8_t flags, uint32_t seq,
		esi_t *esi, struct sync_mac_ip_ctx *ctx)
{
	struct interface *ifp = NULL;
	bool is_router;
	zebra_mac_t *mac = ctx->mac;
	uint32_t tmp_seq;
	bool old_router = false;
	bool old_bgp_ready = false;
	bool new_bgp_ready;
	bool inform_dataplane = false;
	bool inform_bgp = false;
	bool old_mac_static;
	bool new_mac_static;
	bool set_dp_inactive = false;
	struct zebra_if *zif;
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];
	bool created;
	ifindex_t ifindex = 0;

	/* locate l3-svi */
	zif = zvni->vxlan_if->info;
	if (zif) {
		struct zebra_l2info_vxlan *vxl;

		vxl = &zif->l2info.vxl;
		ifp = zvni_map_to_svi(vxl->access_vlan,
				zif->brslave_info.br_if);
		if (ifp)
			ifindex = ifp->ifindex;
	}

	is_router = !!CHECK_FLAG(flags, ZEBRA_MACIP_TYPE_ROUTER_FLAG);
	old_mac_static = zebra_vxlan_mac_is_static(mac);

	if (!n) {
		uint32_t n_flags = 0;

		/* New neighbor - create */
		SET_FLAG(n_flags, ZEBRA_NEIGH_LOCAL);
		if (CHECK_FLAG(flags, ZEBRA_MACIP_TYPE_PROXY_ADVERT))
			SET_FLAG(n_flags, ZEBRA_NEIGH_ES_PEER_PROXY);
		else
			SET_FLAG(n_flags, ZEBRA_NEIGH_ES_PEER_ACTIVE);
		SET_FLAG(n_flags, ZEBRA_NEIGH_LOCAL_INACTIVE);

		n = zvni_neigh_add(zvni, ipaddr, &mac->macaddr, mac,
				n_flags);
		n->ifindex = ifindex;
		ZEBRA_NEIGH_SET_ACTIVE(n);

		created = true;
		inform_dataplane = true;
		inform_bgp = true;
		set_dp_inactive = true;
	} else {
		bool mac_change;
		uint32_t old_flags = n->flags;
		bool old_n_static;
		bool new_n_static;

		created = false;
		old_n_static = zebra_vxlan_neigh_is_static(n);
		old_bgp_ready = zebra_vxlan_neigh_is_ready_for_bgp(n);
		old_router = !!CHECK_FLAG(n->flags,
				ZEBRA_NEIGH_ROUTER_FLAG);

		mac_change = !!memcmp(&n->emac, &mac->macaddr, ETH_ALEN);

		/* deref and clear old info */
		if (mac_change) {
			if (old_bgp_ready) {
				zvni_neigh_send_del_to_client(zvni->vni, &n->ip,
						&n->emac, n->flags, n->state,
						false /*force*/);
				old_bgp_ready = false;
			}
			if (n->mac)
				zebra_vxlan_local_neigh_deref_mac(n,
						false /*send_mac_update*/);
		}
		/* clear old fwd info */
		n->rem_seq = 0;
		n->r_vtep_ip.s_addr = 0;

		/* setup new flags */
		n->flags = 0;
		SET_FLAG(n->flags, ZEBRA_NEIGH_LOCAL);
		/* retain activity flag if the neigh was
		 * previously local
		 */
		if (old_flags & ZEBRA_NEIGH_LOCAL) {
			n->flags |= (old_flags & ZEBRA_NEIGH_LOCAL_INACTIVE);
		} else {
			inform_dataplane = true;
			set_dp_inactive = true;
			n->flags |= ZEBRA_NEIGH_LOCAL_INACTIVE;
		}

		if (CHECK_FLAG(flags, ZEBRA_MACIP_TYPE_PROXY_ADVERT))
			SET_FLAG(n->flags, ZEBRA_NEIGH_ES_PEER_PROXY);
		else
			SET_FLAG(n->flags, ZEBRA_NEIGH_ES_PEER_ACTIVE);

		if (CHECK_FLAG(flags, ZEBRA_MACIP_TYPE_PROXY_ADVERT)) {
			SET_FLAG(n->flags, ZEBRA_NEIGH_ES_PEER_PROXY);
			/* if the neigh was peer-active previously we
			 * need to keep the flag and start the
			 * holdtimer on it. the peer-active flag is
			 * cleared on holdtimer expiry.
			 */
			if (CHECK_FLAG(old_flags,
						ZEBRA_NEIGH_ES_PEER_ACTIVE)) {
				SET_FLAG(n->flags,
						ZEBRA_NEIGH_ES_PEER_ACTIVE);
				zebra_vxlan_neigh_start_hold_timer(n);
			}
		} else {
			SET_FLAG(n->flags, ZEBRA_NEIGH_ES_PEER_ACTIVE);
			/* stop hold timer if a peer has verified
			 * reachability
			 */
			zebra_vxlan_neigh_stop_hold_timer(n);
		}
		ZEBRA_NEIGH_SET_ACTIVE(n);

		if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH &&
				(old_flags != n->flags))
			zlog_debug("sync-neigh vni %u ip %s mac %s old_f 0x%x new_f 0x%x",
				n->zvni->vni,
				ipaddr2str(&n->ip, ipbuf, sizeof(ipbuf)),
				prefix_mac2str(&n->emac, macbuf,
					sizeof(macbuf)),
				old_flags, n->flags);

		new_n_static = zebra_vxlan_neigh_is_static(n);
		if (mac_change) {
			set_dp_inactive = true;
			n->flags |= ZEBRA_NEIGH_LOCAL_INACTIVE;
			inform_dataplane = true;
			zebra_vxlan_local_neigh_ref_mac(n, &mac->macaddr,
					mac, false /*send_mac_update*/);
		} else if (old_n_static != new_n_static) {
			inform_dataplane = true;
			/* if static flags have changed without a mac change
			 * we need to create the correct sync-refs against
			 * the existing mac
			 */
			zebra_vxlan_sync_neigh_static_chg(n,
				old_n_static, new_n_static,
				true /*defer_dp*/, true /*defer_mac_dp*/,
				__func__);
		}

		/* Update the forwarding info. */
		if (n->ifindex != ifindex) {
			n->ifindex = ifindex;
			inform_dataplane = true;
		}
	}

	/* update the neigh seq. we don't bother with the mac seq as
	 * sync_mac_update already took care of that
	 */
	tmp_seq = MAX(n->loc_seq, seq);
	if (tmp_seq != n->loc_seq) {
		n->loc_seq = tmp_seq;
		inform_bgp = true;
	}

	/* Mark Router flag (R-bit) */
	if (is_router)
		SET_FLAG(n->flags, ZEBRA_NEIGH_ROUTER_FLAG);
	else
		UNSET_FLAG(n->flags, ZEBRA_NEIGH_ROUTER_FLAG);

	if (old_router != is_router)
		inform_dataplane = true;

	new_bgp_ready = zebra_vxlan_neigh_is_ready_for_bgp(n);
	if (old_bgp_ready != new_bgp_ready)
		inform_bgp = true;

	new_mac_static = zebra_vxlan_mac_is_static(mac);
	if ((old_mac_static != new_mac_static) ||
			ctx->mac_dp_update_deferred)
		zebra_vxlan_sync_mac_dp_install(mac,
				ctx->mac_inactive,
				false /* force_clear_static */,
				__func__);

	if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
		zlog_debug("sync-neigh %s vni %u ip %s mac %s if %s(%d) seq %d f 0x%x%s%s",
				created ?
				"created" : "updated",
				n->zvni->vni,
				ipaddr2str(&n->ip, ipbuf, sizeof(ipbuf)),
				prefix_mac2str(&n->emac, macbuf,
					sizeof(macbuf)),
				ifp ? ifp->name : "", ifindex,
				n->loc_seq, n->flags,
				inform_bgp ? " inform_bgp" : "",
				inform_dataplane ? " inform_dp" : "");

	if (inform_dataplane)
		zebra_vxlan_sync_neigh_dp_install(n, set_dp_inactive,
				false /* force_clear_static */, __func__);

	if (inform_bgp)
		zebra_vxlan_neigh_send_add_del_to_client(n,
				old_bgp_ready, new_bgp_ready);

	return n;
}

static void zebra_vxlan_process_sync_macip_add(zebra_vni_t *zvni,
				     struct ethaddr *macaddr,
				     uint16_t ipa_len,
				     struct ipaddr *ipaddr,
				     uint8_t flags,
				     uint32_t seq,
				     esi_t *esi)
{
	struct sync_mac_ip_ctx ctx;
	char macbuf[ETHER_ADDR_STRLEN];
	char ipbuf[INET6_ADDRSTRLEN];
	bool sticky;
	bool remote_gw;
	zebra_neigh_t *n = NULL;

	sticky = !!CHECK_FLAG(flags, ZEBRA_MACIP_TYPE_STICKY);
	remote_gw = !!CHECK_FLAG(flags, ZEBRA_MACIP_TYPE_GW);
	/* if sticky or remote-gw ignore updates from the peer */
	if (sticky || remote_gw) {
		if (IS_ZEBRA_DEBUG_VXLAN || IS_ZEBRA_DEBUG_EVPN_MH_NEIGH ||
				IS_ZEBRA_DEBUG_EVPN_MH_MAC)
			zlog_debug("Ignore sync-macip vni %u mac %s%s%s%s%s",
				zvni->vni,
				prefix_mac2str(macaddr, macbuf, sizeof(macbuf)),
				ipa_len ? " IP " : "",
				ipa_len ?
				ipaddr2str(ipaddr, ipbuf, sizeof(ipbuf)) : "",
				sticky ? " sticky" : "",
				remote_gw ? " remote_gw" : "");
		return;
	}

	if (ipa_len) {
		n = zvni_neigh_lookup(zvni, ipaddr);
		if (n &&
				!zebra_vxlan_neigh_is_bgp_seq_ok(zvni,
					n, macaddr, seq))
			return;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.mac = zebra_vxlan_proc_sync_mac_update(zvni, macaddr, ipa_len,
			ipaddr, flags, seq, esi, &ctx);
	if (ctx.ignore_macip || !ctx.mac || !ipa_len)
		return;

	zebra_vxlan_proc_sync_neigh_update(zvni, n, ipa_len,
			ipaddr, flags, seq, esi, &ctx);
}

/************************** remote mac-ip handling **************************/
/* Process a remote MACIP add from BGP. */
static void process_remote_macip_add(vni_t vni,
				     struct ethaddr *macaddr,
				     uint16_t ipa_len,
				     struct ipaddr *ipaddr,
				     uint8_t flags,
				     uint32_t seq,
				     struct in_addr vtep_ip,
				     esi_t *esi)
{
	zebra_vni_t *zvni;
	zebra_vtep_t *zvtep;
	zebra_mac_t *mac = NULL, *old_mac = NULL;
	zebra_neigh_t *n = NULL;
	int update_mac = 0, update_neigh = 0;
	char buf[ETHER_ADDR_STRLEN];
	char buf1[INET6_ADDRSTRLEN];
	struct interface *ifp = NULL;
	struct zebra_if *zif = NULL;
	struct zebra_vrf *zvrf;
	uint32_t tmp_seq;
	bool sticky;
	bool remote_gw;
	bool is_router;
	bool do_dad = false;
	bool is_dup_detect = false;
	esi_t *old_esi;
	bool old_static = false;

	/* Locate VNI hash entry - expected to exist. */
	zvni = zvni_lookup(vni);
	if (!zvni) {
		zlog_warn("Unknown VNI %u upon remote MACIP ADD", vni);
		return;
	}

	ifp = zvni->vxlan_if;
	if (ifp)
		zif = ifp->info;
	if (!ifp ||
	    !if_is_operative(ifp) ||
	    !zif ||
	    !zif->brslave_info.br_if) {
		zlog_warn("Ignoring remote MACIP ADD VNI %u, invalid interface state or info",
			  vni);
		return;
	}

	/* Type-2 routes from another PE can be interpreted as remote or
	 * SYNC based on the destination ES -
	 * SYNC - if ES is local
	 * REMOTE - if ES is not local
	 */
	if (flags & ZEBRA_MACIP_TYPE_SYNC_PATH) {
		zebra_vxlan_process_sync_macip_add(zvni, macaddr, ipa_len,
				ipaddr, flags, seq, esi);
		return;
	}

	/* The remote VTEP specified should normally exist, but it is
	 * possible that when peering comes up, peer may advertise MACIP
	 * routes before advertising type-3 routes.
	 */
	if (vtep_ip.s_addr) {
		zvtep = zvni_vtep_find(zvni, &vtep_ip);
		if (!zvtep) {
			zvtep = zvni_vtep_add(zvni, &vtep_ip,
					VXLAN_FLOOD_DISABLED);
			if (!zvtep) {
				flog_err(
					EC_ZEBRA_VTEP_ADD_FAILED,
					"Failed to add remote VTEP, VNI %u zvni %p upon remote MACIP ADD",
					vni, zvni);
				return;
			}

			zvni_vtep_install(zvni, zvtep);
		}
	}

	sticky = !!CHECK_FLAG(flags, ZEBRA_MACIP_TYPE_STICKY);
	remote_gw = !!CHECK_FLAG(flags, ZEBRA_MACIP_TYPE_GW);
	is_router = !!CHECK_FLAG(flags, ZEBRA_MACIP_TYPE_ROUTER_FLAG);

	mac = zvni_mac_lookup(zvni, macaddr);

	/* Ignore if the mac is already present as a gateway mac */
	if (mac &&
	    CHECK_FLAG(mac->flags, ZEBRA_MAC_DEF_GW) &&
	    CHECK_FLAG(flags, ZEBRA_MACIP_TYPE_GW)) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("Ignore remote MACIP ADD VNI %u MAC %s%s%s as MAC is already configured as gateway MAC",
				   vni,
				   prefix_mac2str(macaddr, buf, sizeof(buf)),
				   ipa_len ? " IP " : "",
				   ipa_len ?
				   ipaddr2str(ipaddr, buf1, sizeof(buf1)) : "");
		return;
	}

	zvrf = vrf_info_lookup(zvni->vxlan_if->vrf_id);
	if (!zvrf)
		return;

	old_esi = (mac && mac->es) ? &mac->es->esi : zero_esi;

	/* check if the remote MAC is unknown or has a change.
	 * If so, that needs to be updated first. Note that client could
	 * install MAC and MACIP separately or just install the latter.
	 */
	if (!mac
	    || !CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE)
	    || sticky != !!CHECK_FLAG(mac->flags, ZEBRA_MAC_STICKY)
	    || remote_gw != !!CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE_DEF_GW)
	    || !IPV4_ADDR_SAME(&mac->fwd_info.r_vtep_ip, &vtep_ip)
	    || memcmp(old_esi, esi, sizeof(esi_t))
	    || seq != mac->rem_seq)
		update_mac = 1;

	if (update_mac) {
		if (!mac) {
			mac = zvni_mac_add(zvni, macaddr);
			if (!mac) {
				zlog_warn(
					"Failed to add MAC %s VNI %u Remote VTEP %s",
					prefix_mac2str(macaddr, buf,
						       sizeof(buf)),
					vni, inet_ntoa(vtep_ip));
				return;
			}

			zebra_evpn_es_mac_ref(mac, esi);

			/* Is this MAC created for a MACIP? */
			if (ipa_len)
				SET_FLAG(mac->flags, ZEBRA_MAC_AUTO);
		} else {
			zebra_evpn_es_mac_ref(mac, esi);

			/* When host moves but changes its (MAC,IP)
			 * binding, BGP may install a MACIP entry that
			 * corresponds to "older" location of the host
			 * in transient situations (because {IP1,M1}
			 * is a different route from {IP1,M2}). Check
			 * the sequence number and ignore this update
			 * if appropriate.
			 */
			if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL))
				tmp_seq = mac->loc_seq;
			else
				tmp_seq = mac->rem_seq;

			if (seq < tmp_seq) {
				if (IS_ZEBRA_DEBUG_VXLAN)
					zlog_debug("Ignore remote MACIP ADD VNI %u MAC %s%s%s as existing MAC has higher seq %u flags 0x%x",
					vni,
					prefix_mac2str(macaddr,
						       buf, sizeof(buf)),
					ipa_len ? " IP " : "",
					ipa_len ?
					ipaddr2str(ipaddr,
						   buf1, sizeof(buf1)) : "",
					tmp_seq, mac->flags);
				return;
			}
		}

		/* Check MAC's curent state is local (this is the case
		 * where MAC has moved from L->R) and check previous
		 * detection started via local learning.
		 * RFC-7432: A PE/VTEP that detects a MAC mobility
		 * event via local learning starts an M-second timer.
		 *
		 * VTEP-IP or seq. change alone is not considered
		 * for dup. detection.
		 *
		 * MAC is already marked duplicate set dad, then
		 * is_dup_detect will be set to not install the entry.
		 */
		if ((!CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE) &&
		    mac->dad_count) ||
		    CHECK_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE))
			do_dad = true;

		/* Remove local MAC from BGP. */
		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL)) {
			/* force drop the sync flags */
			old_static = zebra_vxlan_mac_is_static(mac);
			if (IS_ZEBRA_DEBUG_EVPN_MH_MAC)
				zlog_debug("sync-mac->remote vni %u mac %s es %s seq %d f 0x%x",
						zvni->vni,
						prefix_mac2str(macaddr,
							buf, sizeof(buf)),
						mac->es ?
						mac->es->esi_str : "-",
						mac->loc_seq,
						mac->flags);
			zebra_vxlan_mac_clear_sync_info(mac);
			zvni_mac_send_del_to_client(zvni->vni, macaddr,
					mac->flags, false /* force */);
		}

		/* Set "auto" and "remote" forwarding info. */
		UNSET_FLAG(mac->flags, ZEBRA_MAC_ALL_LOCAL_FLAGS);
		memset(&mac->fwd_info, 0, sizeof(mac->fwd_info));
		SET_FLAG(mac->flags, ZEBRA_MAC_REMOTE);
		mac->fwd_info.r_vtep_ip = vtep_ip;

		if (sticky)
			SET_FLAG(mac->flags, ZEBRA_MAC_STICKY);
		else
			UNSET_FLAG(mac->flags, ZEBRA_MAC_STICKY);

		if (remote_gw)
			SET_FLAG(mac->flags, ZEBRA_MAC_REMOTE_DEF_GW);
		else
			UNSET_FLAG(mac->flags, ZEBRA_MAC_REMOTE_DEF_GW);

		zebra_vxlan_dup_addr_detect_for_mac(zvrf, mac,
						    mac->fwd_info.r_vtep_ip,
						    do_dad, &is_dup_detect,
						    false);

		if (!is_dup_detect) {
			zvni_process_neigh_on_remote_mac_add(zvni, mac);
			/* Install the entry. */
			zvni_rem_mac_install(zvni, mac, old_static);
		}
	}

	/* Update seq number. */
	mac->rem_seq = seq;

	/* If there is no IP, return after clearing AUTO flag of MAC. */
	if (!ipa_len) {
		UNSET_FLAG(mac->flags, ZEBRA_MAC_AUTO);
		return;
	}

	/* Reset flag */
	do_dad = false;
	old_static = false;

	/* Check if the remote neighbor itself is unknown or has a
	 * change. If so, create or update and then install the entry.
	 */
	n = zvni_neigh_lookup(zvni, ipaddr);
	if (!n
	    || !CHECK_FLAG(n->flags, ZEBRA_NEIGH_REMOTE)
	    || is_router != !!CHECK_FLAG(n->flags, ZEBRA_NEIGH_ROUTER_FLAG)
	    || (memcmp(&n->emac, macaddr, sizeof(*macaddr)) != 0)
	    || !IPV4_ADDR_SAME(&n->r_vtep_ip, &vtep_ip)
	    || seq != n->rem_seq)
		update_neigh = 1;

	if (update_neigh) {
		if (!n) {
			n = zvni_neigh_add(zvni, ipaddr, macaddr, mac, 0);
			if (!n) {
				zlog_warn(
					"Failed to add Neigh %s MAC %s VNI %u Remote VTEP %s",
					ipaddr2str(ipaddr, buf1,
						   sizeof(buf1)),
					prefix_mac2str(macaddr, buf,
						       sizeof(buf)),
					vni, inet_ntoa(vtep_ip));
				return;
			}

		} else {
			const char *n_type;

			/* When host moves but changes its (MAC,IP)
			 * binding, BGP may install a MACIP entry that
			 * corresponds to "older" location of the host
			 * in transient situations (because {IP1,M1}
			 * is a different route from {IP1,M2}). Check
			 * the sequence number and ignore this update
			 * if appropriate.
			 */
			if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL)) {
				tmp_seq = n->loc_seq;
				n_type = "local";
			} else {
				tmp_seq = n->rem_seq;
				n_type = "remote";
			}
			if (seq < tmp_seq) {
				if (IS_ZEBRA_DEBUG_VXLAN)
					zlog_debug("Ignore remote MACIP ADD VNI %u MAC %s%s%s as existing %s Neigh has higher seq %u",
					vni,
					prefix_mac2str(macaddr,
						       buf, sizeof(buf)),
					" IP ",
					ipaddr2str(ipaddr, buf1, sizeof(buf1)),
					n_type,
					tmp_seq);
				return;
			}
			if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL)) {
				old_static = zebra_vxlan_neigh_is_static(n);
				if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
					zlog_debug("sync->remote neigh vni %u ip %s mac %s seq %d f0x%x",
						n->zvni->vni,
						ipaddr2str(&n->ip, buf1,
							sizeof(buf1)),
						prefix_mac2str(&n->emac, buf,
							sizeof(buf)),
						seq, n->flags);
				zebra_vxlan_neigh_clear_sync_info(n);
				if (IS_ZEBRA_NEIGH_ACTIVE(n))
					zvni_mac_send_del_to_client(zvni->vni,
						macaddr, mac->flags,
						false /*force*/);
			}
			if (memcmp(&n->emac, macaddr, sizeof(*macaddr)) != 0) {
				/* update neigh list for macs */
				old_mac = zvni_mac_lookup(zvni, &n->emac);
				if (old_mac) {
					listnode_delete(old_mac->neigh_list, n);
					n->mac = NULL;
					zvni_deref_ip2mac(zvni, old_mac);
				}
				n->mac = mac;
				listnode_add_sort(mac->neigh_list, n);
				memcpy(&n->emac, macaddr, ETH_ALEN);

				/* Check Neigh's curent state is local
				 * (this is the case where neigh/host has  moved
				 * from L->R) and check previous detction
				 * started via local learning.
				 *
				 * RFC-7432: A PE/VTEP that detects a MAC
				 * mobilit event via local learning starts
				 * an M-second timer.
				 * VTEP-IP or seq. change along is not
				 * considered for dup. detection.
				 *
				 * Mobilty event scenario-B IP-MAC binding
				 * changed.
				 */
				if ((!CHECK_FLAG(n->flags, ZEBRA_NEIGH_REMOTE))
				    && n->dad_count)
					do_dad = true;

			}
		}

		/* Set "remote" forwarding info. */
		UNSET_FLAG(n->flags, ZEBRA_NEIGH_ALL_LOCAL_FLAGS);
		n->r_vtep_ip = vtep_ip;
		SET_FLAG(n->flags, ZEBRA_NEIGH_REMOTE);

		/* Set router flag (R-bit) to this Neighbor entry */
		if (CHECK_FLAG(flags, ZEBRA_MACIP_TYPE_ROUTER_FLAG))
			SET_FLAG(n->flags, ZEBRA_NEIGH_ROUTER_FLAG);
		else
			UNSET_FLAG(n->flags, ZEBRA_NEIGH_ROUTER_FLAG);

		/* Check old or new MAC detected as duplicate,
		 * inherit duplicate flag to this neigh.
		 */
		if (zebra_vxlan_ip_inherit_dad_from_mac(zvrf, old_mac,
							mac, n)) {
			flog_warn(EC_ZEBRA_DUP_IP_INHERIT_DETECTED,
				"VNI %u: MAC %s IP %s detected as duplicate during remote update, inherit duplicate from MAC",
				zvni->vni,
				prefix_mac2str(&mac->macaddr, buf, sizeof(buf)),
				ipaddr2str(&n->ip, buf1, sizeof(buf1)));
		}

		/* Check duplicate address detection for IP */
		zebra_vxlan_dup_addr_detect_for_neigh(zvrf, n,
						      n->r_vtep_ip,
						      do_dad,
						      &is_dup_detect,
						      false);
		/* Install the entry. */
		if (!is_dup_detect)
			zvni_rem_neigh_install(zvni, n, old_static);
	}

	zvni_probe_neigh_on_mac_add(zvni, mac);

	/* Update seq number. */
	n->rem_seq = seq;
}

static void zebra_vxlan_rem_mac_del(zebra_vni_t *zvni,
		zebra_mac_t *mac)
{
	zvni_process_neigh_on_remote_mac_del(zvni, mac);
	/* the remote sequence number in the auto mac entry
	 * needs to be reset to 0 as the mac entry may have
	 * been removed on all VTEPs (including
	 * the originating one)
	 */
	mac->rem_seq = 0;

	/* If all remote neighbors referencing a remote MAC
	 * go away, we need to uninstall the MAC.
	 */
	if (remote_neigh_count(mac) == 0) {
		zvni_rem_mac_uninstall(zvni, mac);
		zebra_evpn_es_mac_deref_entry(mac);
		UNSET_FLAG(mac->flags, ZEBRA_MAC_REMOTE);
	}

	if (list_isempty(mac->neigh_list))
		zvni_mac_del(zvni, mac);
	else
		SET_FLAG(mac->flags, ZEBRA_MAC_AUTO);
}

/* Process a remote MACIP delete from BGP. */
static void process_remote_macip_del(vni_t vni,
				     struct ethaddr *macaddr,
				     uint16_t ipa_len,
				     struct ipaddr *ipaddr,
				     struct in_addr vtep_ip)
{
	zebra_vni_t *zvni;
	zebra_mac_t *mac = NULL;
	zebra_neigh_t *n = NULL;
	struct interface *ifp = NULL;
	struct zebra_if *zif = NULL;
	struct zebra_ns *zns;
	struct zebra_l2info_vxlan *vxl;
	struct zebra_vrf *zvrf;
	char buf[ETHER_ADDR_STRLEN];
	char buf1[INET6_ADDRSTRLEN];

	/* Locate VNI hash entry - expected to exist. */
	zvni = zvni_lookup(vni);
	if (!zvni) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("Unknown VNI %u upon remote MACIP DEL", vni);
		return;
	}

	ifp = zvni->vxlan_if;
	if (ifp)
		zif = ifp->info;
	if (!ifp ||
	    !if_is_operative(ifp) ||
	    !zif ||
	    !zif->brslave_info.br_if) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("Ignoring remote MACIP DEL VNI %u, invalid interface state or info",
				   vni);
		return;
	}
	zns = zebra_ns_lookup(NS_DEFAULT);
	vxl = &zif->l2info.vxl;

	mac = zvni_mac_lookup(zvni, macaddr);
	if (ipa_len)
		n = zvni_neigh_lookup(zvni, ipaddr);

	if (n && !mac) {
		zlog_warn("Failed to locate MAC %s for neigh %s VNI %u upon remote MACIP DEL",
			  prefix_mac2str(macaddr, buf, sizeof(buf)),
			  ipaddr2str(ipaddr, buf1, sizeof(buf1)), vni);
		return;
	}

	/* If the remote mac or neighbor doesn't exist there is nothing
	 * more to do. Otherwise, uninstall the entry and then remove it.
	 */
	if (!mac && !n)
		return;

	zvrf = vrf_info_lookup(zvni->vxlan_if->vrf_id);

	/* Ignore the delete if this mac is a gateway mac-ip */
	if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL)
	    && CHECK_FLAG(mac->flags, ZEBRA_MAC_DEF_GW)) {
		zlog_warn(
			"Ignore remote MACIP DEL VNI %u MAC %s%s%s as MAC is already configured as gateway MAC",
			vni,
			prefix_mac2str(macaddr, buf, sizeof(buf)),
			ipa_len ? " IP " : "",
			ipa_len ?
			ipaddr2str(ipaddr, buf1, sizeof(buf1)) : "");
		return;
	}

	/* Uninstall remote neighbor or MAC. */
	if (n) {
		if (zvrf->dad_freeze &&
		    CHECK_FLAG(n->flags, ZEBRA_NEIGH_DUPLICATE) &&
		    CHECK_FLAG(n->flags, ZEBRA_NEIGH_REMOTE) &&
		    (memcmp(n->emac.octet, macaddr->octet, ETH_ALEN) == 0)) {
			struct interface *vlan_if;

			vlan_if = zvni_map_to_svi(vxl->access_vlan,
					zif->brslave_info.br_if);
			if (IS_ZEBRA_DEBUG_VXLAN)
				zlog_debug(
					"%s: IP %s (flags 0x%x intf %s) is remote and duplicate, read kernel for local entry",
					__func__,
					ipaddr2str(ipaddr, buf1, sizeof(buf1)),
					n->flags,
					vlan_if ? vlan_if->name : "Unknown");
			if (vlan_if)
				neigh_read_specific_ip(ipaddr, vlan_if);
		}

		/* When the MAC changes for an IP, it is possible the
		 * client may update the new MAC before trying to delete the
		 * "old" neighbor (as these are two different MACIP routes).
		 * Do the delete only if the MAC matches.
		 */
		if (!memcmp(n->emac.octet, macaddr->octet, ETH_ALEN)) {
			if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL)) {
				zebra_vxlan_sync_neigh_del(n);
			} else if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_REMOTE)) {
				zvni_neigh_uninstall(zvni, n);
				zvni_neigh_del(zvni, n);
				zvni_deref_ip2mac(zvni, mac);
			}
		}
	} else {
		/* DAD: when MAC is freeze state as remote learn event,
		 * remote mac-ip delete event is received will result in freeze
		 * entry removal, first fetch kernel for the same entry present
		 * as LOCAL and reachable, avoid deleting this entry instead
		 * use kerenel local entry to update during unfreeze time.
		 */
		if (zvrf->dad_freeze &&
		    CHECK_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE) &&
		    CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE)) {
			if (IS_ZEBRA_DEBUG_VXLAN)
				zlog_debug(
					"%s: MAC %s (flags 0x%x) is remote and duplicate, read kernel for local entry",
					__func__,
					prefix_mac2str(macaddr, buf,
						       sizeof(buf)),
					mac->flags);
			macfdb_read_specific_mac(zns, zif->brslave_info.br_if,
						 macaddr, vxl->access_vlan);
		}

		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL)) {
			if (!ipa_len)
				zebra_vxlan_sync_mac_del(mac);
		} else if (CHECK_FLAG(mac->flags, ZEBRA_NEIGH_REMOTE)) {
			zebra_vxlan_rem_mac_del(zvni, mac);
		}
	}
}


/* Public functions */

int is_l3vni_for_prefix_routes_only(vni_t vni)
{
	zebra_l3vni_t *zl3vni = NULL;

	zl3vni = zl3vni_lookup(vni);
	if (!zl3vni)
		return 0;

	return CHECK_FLAG(zl3vni->filter, PREFIX_ROUTES_ONLY) ? 1 : 0;
}

/* handle evpn route in vrf table */
void zebra_vxlan_evpn_vrf_route_add(vrf_id_t vrf_id, const struct ethaddr *rmac,
				    const struct ipaddr *vtep_ip,
				    const struct prefix *host_prefix)
{
	zebra_l3vni_t *zl3vni = NULL;
	struct ipaddr ipv4_vtep;

	zl3vni = zl3vni_from_vrf(vrf_id);
	if (!zl3vni || !is_l3vni_oper_up(zl3vni))
		return;

	/*
	 * add the next hop neighbor -
	 * neigh to be installed is the ipv6 nexthop neigh
	 */
	zl3vni_remote_nh_add(zl3vni, vtep_ip, rmac, host_prefix);

	/*
	 * if the remote vtep is a ipv4 mapped ipv6 address convert it to ipv4
	 * address. Rmac is programmed against the ipv4 vtep because we only
	 * support ipv4 tunnels in the h/w right now
	 */
	memset(&ipv4_vtep, 0, sizeof(struct ipaddr));
	ipv4_vtep.ipa_type = IPADDR_V4;
	if (vtep_ip->ipa_type == IPADDR_V6)
		ipv4_mapped_ipv6_to_ipv4(&vtep_ip->ipaddr_v6,
					 &(ipv4_vtep.ipaddr_v4));
	else
		memcpy(&(ipv4_vtep.ipaddr_v4), &vtep_ip->ipaddr_v4,
		       sizeof(struct in_addr));

	/*
	 * add the rmac - remote rmac to be installed is against the ipv4
	 * nexthop address
	 */
	zl3vni_remote_rmac_add(zl3vni, rmac, &ipv4_vtep, host_prefix);
}

/* handle evpn vrf route delete */
void zebra_vxlan_evpn_vrf_route_del(vrf_id_t vrf_id,
				    struct ipaddr *vtep_ip,
				    struct prefix *host_prefix)
{
	zebra_l3vni_t *zl3vni = NULL;
	zebra_neigh_t *nh = NULL;
	zebra_mac_t *zrmac = NULL;

	zl3vni = zl3vni_from_vrf(vrf_id);
	if (!zl3vni)
		return;

	/* find the next hop entry and rmac entry */
	nh = zl3vni_nh_lookup(zl3vni, vtep_ip);
	if (!nh)
		return;
	zrmac = zl3vni_rmac_lookup(zl3vni, &nh->emac);

	/* delete the next hop entry */
	zl3vni_remote_nh_del(zl3vni, nh, host_prefix);

	/* delete the rmac entry */
	if (zrmac)
		zl3vni_remote_rmac_del(zl3vni, zrmac, host_prefix);

}

void zebra_vxlan_print_specific_rmac_l3vni(struct vty *vty, vni_t l3vni,
					   struct ethaddr *rmac, bool use_json)
{
	zebra_l3vni_t *zl3vni = NULL;
	zebra_mac_t *zrmac = NULL;
	json_object *json = NULL;

	if (!is_evpn_enabled()) {
		if (use_json)
			vty_out(vty, "{}\n");
		return;
	}

	if (use_json)
		json = json_object_new_object();

	zl3vni = zl3vni_lookup(l3vni);
	if (!zl3vni) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty, "%% L3-VNI %u doesn't exist\n", l3vni);
		return;
	}

	zrmac = zl3vni_rmac_lookup(zl3vni, rmac);
	if (!zrmac) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty,
				"%% Requested RMAC doesn't exist in L3-VNI %u",
				l3vni);
		return;
	}

	zl3vni_print_rmac(zrmac, vty, json);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

void zebra_vxlan_print_rmacs_l3vni(struct vty *vty, vni_t l3vni, bool use_json)
{
	zebra_l3vni_t *zl3vni;
	uint32_t num_rmacs;
	struct rmac_walk_ctx wctx;
	json_object *json = NULL;

	if (!is_evpn_enabled())
		return;

	zl3vni = zl3vni_lookup(l3vni);
	if (!zl3vni) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty, "%% L3-VNI %u does not exist\n", l3vni);
		return;
	}
	num_rmacs = hashcount(zl3vni->rmac_table);
	if (!num_rmacs)
		return;

	if (use_json)
		json = json_object_new_object();

	memset(&wctx, 0, sizeof(struct rmac_walk_ctx));
	wctx.vty = vty;
	wctx.json = json;
	if (!use_json) {
		vty_out(vty, "Number of Remote RMACs known for this VNI: %u\n",
			num_rmacs);
		vty_out(vty, "%-17s %-21s\n", "MAC", "Remote VTEP");
	} else
		json_object_int_add(json, "numRmacs", num_rmacs);

	hash_iterate(zl3vni->rmac_table, zl3vni_print_rmac_hash, &wctx);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

void zebra_vxlan_print_rmacs_all_l3vni(struct vty *vty, bool use_json)
{
	json_object *json = NULL;
	void *args[2];

	if (!is_evpn_enabled()) {
		if (use_json)
			vty_out(vty, "{}\n");
		return;
	}

	if (use_json)
		json = json_object_new_object();

	args[0] = vty;
	args[1] = json;
	hash_iterate(zrouter.l3vni_table,
		     (void (*)(struct hash_bucket *,
			       void *))zl3vni_print_rmac_hash_all_vni,
		     args);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

void zebra_vxlan_print_specific_nh_l3vni(struct vty *vty, vni_t l3vni,
					 struct ipaddr *ip, bool use_json)
{
	zebra_l3vni_t *zl3vni = NULL;
	zebra_neigh_t *n = NULL;
	json_object *json = NULL;

	if (!is_evpn_enabled()) {
		if (use_json)
			vty_out(vty, "{}\n");
		return;
	}

	if (use_json)
		json = json_object_new_object();

	zl3vni = zl3vni_lookup(l3vni);
	if (!zl3vni) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty, "%% L3-VNI %u does not exist\n", l3vni);
		return;
	}

	n = zl3vni_nh_lookup(zl3vni, ip);
	if (!n) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty,
				"%% Requested next-hop not present for L3-VNI %u",
				l3vni);
		return;
	}

	zl3vni_print_nh(n, vty, json);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

void zebra_vxlan_print_nh_l3vni(struct vty *vty, vni_t l3vni, bool use_json)
{
	uint32_t num_nh;
	struct nh_walk_ctx wctx;
	json_object *json = NULL;
	zebra_l3vni_t *zl3vni = NULL;

	if (!is_evpn_enabled())
		return;

	zl3vni = zl3vni_lookup(l3vni);
	if (!zl3vni) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty, "%% L3-VNI %u does not exist\n", l3vni);
		return;
	}

	num_nh = hashcount(zl3vni->nh_table);
	if (!num_nh)
		return;

	if (use_json)
		json = json_object_new_object();

	wctx.vty = vty;
	wctx.json = json;
	if (!use_json) {
		vty_out(vty, "Number of NH Neighbors known for this VNI: %u\n",
			num_nh);
		vty_out(vty, "%-15s %-17s\n", "IP", "RMAC");
	} else
		json_object_int_add(json, "numNextHops", num_nh);

	hash_iterate(zl3vni->nh_table, zl3vni_print_nh_hash, &wctx);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

void zebra_vxlan_print_nh_all_l3vni(struct vty *vty, bool use_json)
{
	json_object *json = NULL;
	void *args[2];

	if (!is_evpn_enabled()) {
		if (use_json)
			vty_out(vty, "{}\n");
		return;
	}

	if (use_json)
		json = json_object_new_object();

	args[0] = vty;
	args[1] = json;
	hash_iterate(zrouter.l3vni_table,
		     (void (*)(struct hash_bucket *,
			       void *))zl3vni_print_nh_hash_all_vni,
		     args);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

/*
 * Display L3 VNI information (VTY command handler).
 */
void zebra_vxlan_print_l3vni(struct vty *vty, vni_t vni, bool use_json)
{
	void *args[2];
	json_object *json = NULL;
	zebra_l3vni_t *zl3vni = NULL;

	if (!is_evpn_enabled()) {
		if (use_json)
			vty_out(vty, "{}\n");
		return;
	}

	zl3vni = zl3vni_lookup(vni);
	if (!zl3vni) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty, "%% VNI %u does not exist\n", vni);
		return;
	}

	if (use_json)
		json = json_object_new_object();

	args[0] = vty;
	args[1] = json;
	zl3vni_print(zl3vni, (void *)args);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

void zebra_vxlan_print_vrf_vni(struct vty *vty, struct zebra_vrf *zvrf,
			       json_object *json_vrfs)
{
	char buf[ETHER_ADDR_STRLEN];
	zebra_l3vni_t *zl3vni = NULL;

	zl3vni = zl3vni_lookup(zvrf->l3vni);
	if (!zl3vni)
		return;

	if (!json_vrfs) {
		vty_out(vty, "%-37s %-10u %-20s %-20s %-5s %-18s\n",
			zvrf_name(zvrf), zl3vni->vni,
			zl3vni_vxlan_if_name(zl3vni),
			zl3vni_svi_if_name(zl3vni), zl3vni_state2str(zl3vni),
			zl3vni_rmac2str(zl3vni, buf, sizeof(buf)));
	} else {
		json_object *json_vrf = NULL;

		json_vrf = json_object_new_object();
		json_object_string_add(json_vrf, "vrf", zvrf_name(zvrf));
		json_object_int_add(json_vrf, "vni", zl3vni->vni);
		json_object_string_add(json_vrf, "vxlanIntf",
				       zl3vni_vxlan_if_name(zl3vni));
		json_object_string_add(json_vrf, "sviIntf",
				       zl3vni_svi_if_name(zl3vni));
		json_object_string_add(json_vrf, "state",
				       zl3vni_state2str(zl3vni));
		json_object_string_add(
			json_vrf, "routerMac",
			zl3vni_rmac2str(zl3vni, buf, sizeof(buf)));
		json_object_array_add(json_vrfs, json_vrf);
	}
}

/*
 * Display Neighbors for a VNI (VTY command handler).
 */
void zebra_vxlan_print_neigh_vni(struct vty *vty, struct zebra_vrf *zvrf,
				 vni_t vni, bool use_json)
{
	zebra_vni_t *zvni;
	uint32_t num_neigh;
	struct neigh_walk_ctx wctx;
	json_object *json = NULL;

	if (!is_evpn_enabled())
		return;
	zvni = zvni_lookup(vni);
	if (!zvni) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty, "%% VNI %u does not exist\n", vni);
		return;
	}
	num_neigh = hashcount(zvni->neigh_table);
	if (!num_neigh)
		return;

	if (use_json)
		json = json_object_new_object();

	/* Since we have IPv6 addresses to deal with which can vary widely in
	 * size, we try to be a bit more elegant in display by first computing
	 * the maximum width.
	 */
	memset(&wctx, 0, sizeof(struct neigh_walk_ctx));
	wctx.zvni = zvni;
	wctx.vty = vty;
	wctx.addr_width = 15;
	wctx.json = json;
	hash_iterate(zvni->neigh_table, zvni_find_neigh_addr_width, &wctx);

	if (!use_json) {
		vty_out(vty,
			"Number of ARPs (local and remote) known for this VNI: %u\n",
			num_neigh);
		zvni_print_neigh_hdr(vty, &wctx);
	} else
		json_object_int_add(json, "numArpNd", num_neigh);

	hash_iterate(zvni->neigh_table, zvni_print_neigh_hash, &wctx);
	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

/*
 * Display neighbors across all VNIs (VTY command handler).
 */
void zebra_vxlan_print_neigh_all_vni(struct vty *vty, struct zebra_vrf *zvrf,
				     bool print_dup, bool use_json)
{
	json_object *json = NULL;
	void *args[3];

	if (!is_evpn_enabled())
		return;

	if (use_json)
		json = json_object_new_object();

	args[0] = vty;
	args[1] = json;
	args[2] = (void *)(ptrdiff_t)print_dup;

	hash_iterate(zvrf->vni_table,
		     (void (*)(struct hash_bucket *,
			       void *))zvni_print_neigh_hash_all_vni,
		     args);
	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

/*
 * Display neighbors across all VNIs in detail(VTY command handler).
 */
void zebra_vxlan_print_neigh_all_vni_detail(struct vty *vty,
					    struct zebra_vrf *zvrf,
					    bool print_dup, bool use_json)
{
	json_object *json = NULL;
	void *args[3];

	if (!is_evpn_enabled())
		return;

	if (use_json)
		json = json_object_new_object();

	args[0] = vty;
	args[1] = json;
	args[2] = (void *)(ptrdiff_t)print_dup;

	hash_iterate(zvrf->vni_table,
		     (void (*)(struct hash_bucket *,
			       void *))zvni_print_neigh_hash_all_vni_detail,
		     args);
	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

/*
 * Display specific neighbor for a VNI, if present (VTY command handler).
 */
void zebra_vxlan_print_specific_neigh_vni(struct vty *vty,
					  struct zebra_vrf *zvrf, vni_t vni,
					  struct ipaddr *ip, bool use_json)
{
	zebra_vni_t *zvni;
	zebra_neigh_t *n;
	json_object *json = NULL;

	if (!is_evpn_enabled())
		return;
	zvni = zvni_lookup(vni);
	if (!zvni) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty, "%% VNI %u does not exist\n", vni);
		return;
	}
	n = zvni_neigh_lookup(zvni, ip);
	if (!n) {
		if (!use_json)
			vty_out(vty,
				"%% Requested neighbor does not exist in VNI %u\n",
				vni);
		return;
	}
	if (use_json)
		json = json_object_new_object();

	zvni_print_neigh(n, vty, json);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

/*
 * Display neighbors for a VNI from specific VTEP (VTY command handler).
 * By definition, these are remote neighbors.
 */
void zebra_vxlan_print_neigh_vni_vtep(struct vty *vty, struct zebra_vrf *zvrf,
				      vni_t vni, struct in_addr vtep_ip,
				      bool use_json)
{
	zebra_vni_t *zvni;
	uint32_t num_neigh;
	struct neigh_walk_ctx wctx;
	json_object *json = NULL;

	if (!is_evpn_enabled())
		return;
	zvni = zvni_lookup(vni);
	if (!zvni) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty, "%% VNI %u does not exist\n", vni);
		return;
	}
	num_neigh = hashcount(zvni->neigh_table);
	if (!num_neigh)
		return;

	if (use_json)
		json = json_object_new_object();

	memset(&wctx, 0, sizeof(struct neigh_walk_ctx));
	wctx.zvni = zvni;
	wctx.vty = vty;
	wctx.addr_width = 15;
	wctx.flags = SHOW_REMOTE_NEIGH_FROM_VTEP;
	wctx.r_vtep_ip = vtep_ip;
	wctx.json = json;
	hash_iterate(zvni->neigh_table, zvni_find_neigh_addr_width, &wctx);
	hash_iterate(zvni->neigh_table, zvni_print_neigh_hash, &wctx);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

/*
 * Display Duplicate detected Neighbors for a VNI
 * (VTY command handler).
 */
void zebra_vxlan_print_neigh_vni_dad(struct vty *vty,
				     struct zebra_vrf *zvrf,
				     vni_t vni,
				     bool use_json)
{
	zebra_vni_t *zvni;
	uint32_t num_neigh;
	struct neigh_walk_ctx wctx;
	json_object *json = NULL;

	if (!is_evpn_enabled())
		return;

	zvni = zvni_lookup(vni);
	if (!zvni) {
		vty_out(vty, "%% VNI %u does not exist\n", vni);
		return;
	}

	num_neigh = hashcount(zvni->neigh_table);
	if (!num_neigh)
		return;

	num_neigh = num_dup_detected_neighs(zvni);
	if (!num_neigh)
		return;

	if (use_json)
		json = json_object_new_object();

	/* Since we have IPv6 addresses to deal with which can vary widely in
	 * size, we try to be a bit more elegant in display by first computing
	 * the maximum width.
	 */
	memset(&wctx, 0, sizeof(struct neigh_walk_ctx));
	wctx.zvni = zvni;
	wctx.vty = vty;
	wctx.addr_width = 15;
	wctx.json = json;
	hash_iterate(zvni->neigh_table, zvni_find_neigh_addr_width, &wctx);

	if (!use_json) {
		vty_out(vty,
			"Number of ARPs (local and remote) known for this VNI: %u\n",
			num_neigh);
		vty_out(vty, "%*s %-6s %-8s %-17s %-30s\n",
			-wctx.addr_width, "IP", "Type",
			"State", "MAC", "Remote ES/VTEP");
	} else
		json_object_int_add(json, "numArpNd", num_neigh);

	hash_iterate(zvni->neigh_table, zvni_print_dad_neigh_hash, &wctx);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

/*
 * Display MACs for a VNI (VTY command handler).
 */
void zebra_vxlan_print_macs_vni(struct vty *vty, struct zebra_vrf *zvrf,
				vni_t vni, bool use_json)
{
	zebra_vni_t *zvni;
	uint32_t num_macs;
	struct mac_walk_ctx wctx;
	json_object *json = NULL;
	json_object *json_mac = NULL;

	if (!is_evpn_enabled())
		return;
	zvni = zvni_lookup(vni);
	if (!zvni) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty, "%% VNI %u does not exist\n", vni);
		return;
	}
	num_macs = num_valid_macs(zvni);
	if (!num_macs)
		return;

	if (use_json) {
		json = json_object_new_object();
		json_mac = json_object_new_object();
	}

	memset(&wctx, 0, sizeof(struct mac_walk_ctx));
	wctx.zvni = zvni;
	wctx.vty = vty;
	wctx.json = json_mac;

	if (!use_json) {
		vty_out(vty,
			"Number of MACs (local and remote) known for this VNI: %u\n",
			num_macs);
			vty_out(vty,
				"Flags: N=sync-neighs, I=local-inactive, P=peer-active, X=peer-proxy\n");
			vty_out(vty, "%-17s %-6s %-5s %-30s %-5s %s\n", "MAC",
				"Type", "Flags", "Intf/Remote ES/VTEP",
				"VLAN", "Seq #'s");
	} else
		json_object_int_add(json, "numMacs", num_macs);

	hash_iterate(zvni->mac_table, zvni_print_mac_hash, &wctx);

	if (use_json) {
		json_object_object_add(json, "macs", json_mac);
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

/*
 * Display MACs for all VNIs (VTY command handler).
 */
void zebra_vxlan_print_macs_all_vni(struct vty *vty, struct zebra_vrf *zvrf,
				    bool print_dup, bool use_json)
{
	struct mac_walk_ctx wctx;
	json_object *json = NULL;

	if (!is_evpn_enabled()) {
		if (use_json)
			vty_out(vty, "{}\n");
		return;
	}
	if (use_json)
		json = json_object_new_object();

	memset(&wctx, 0, sizeof(struct mac_walk_ctx));
	wctx.vty = vty;
	wctx.json = json;
	wctx.print_dup = print_dup;
	hash_iterate(zvrf->vni_table, zvni_print_mac_hash_all_vni, &wctx);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

/*
 * Display MACs in detail for all VNIs (VTY command handler).
 */
void zebra_vxlan_print_macs_all_vni_detail(struct vty *vty,
					   struct zebra_vrf *zvrf,
					   bool print_dup, bool use_json)
{
	struct mac_walk_ctx wctx;
	json_object *json = NULL;

	if (!is_evpn_enabled()) {
		if (use_json)
			vty_out(vty, "{}\n");
		return;
	}
	if (use_json)
		json = json_object_new_object();

	memset(&wctx, 0, sizeof(struct mac_walk_ctx));
	wctx.vty = vty;
	wctx.json = json;
	wctx.print_dup = print_dup;
	hash_iterate(zvrf->vni_table, zvni_print_mac_hash_all_vni_detail,
		     &wctx);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

/*
 * Display MACs for all VNIs (VTY command handler).
 */
void zebra_vxlan_print_macs_all_vni_vtep(struct vty *vty,
					 struct zebra_vrf *zvrf,
					 struct in_addr vtep_ip, bool use_json)
{
	struct mac_walk_ctx wctx;
	json_object *json = NULL;

	if (!is_evpn_enabled())
		return;

	if (use_json)
		json = json_object_new_object();

	memset(&wctx, 0, sizeof(struct mac_walk_ctx));
	wctx.vty = vty;
	wctx.flags = SHOW_REMOTE_MAC_FROM_VTEP;
	wctx.r_vtep_ip = vtep_ip;
	wctx.json = json;
	hash_iterate(zvrf->vni_table, zvni_print_mac_hash_all_vni, &wctx);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

/*
 * Display specific MAC for a VNI, if present (VTY command handler).
 */
void zebra_vxlan_print_specific_mac_vni(struct vty *vty, struct zebra_vrf *zvrf,
					vni_t vni, struct ethaddr *macaddr,
					bool use_json)
{
	zebra_vni_t *zvni;
	zebra_mac_t *mac;
	json_object *json = NULL;

	if (!is_evpn_enabled())
		return;

	zvni = zvni_lookup(vni);
	if (!zvni) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty, "%% VNI %u does not exist\n", vni);
		return;
	}
	mac = zvni_mac_lookup(zvni, macaddr);
	if (!mac) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty,
				"%% Requested MAC does not exist in VNI %u\n",
				vni);
		return;
	}

	if (use_json)
		json = json_object_new_object();

	zvni_print_mac(mac, vty, json);
	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

/* Print Duplicate MACs per VNI */
void zebra_vxlan_print_macs_vni_dad(struct vty *vty,
				    struct zebra_vrf *zvrf,
				    vni_t vni, bool use_json)
{
	zebra_vni_t *zvni;
	struct mac_walk_ctx wctx;
	uint32_t num_macs;
	json_object *json = NULL;
	json_object *json_mac = NULL;

	if (!is_evpn_enabled())
		return;

	zvni = zvni_lookup(vni);
	if (!zvni) {
		vty_out(vty, "%% VNI %u does not exist\n", vni);
		return;
	}

	num_macs = num_valid_macs(zvni);
	if (!num_macs)
		return;

	num_macs = num_dup_detected_macs(zvni);
	if (!num_macs)
		return;

	if (use_json) {
		json = json_object_new_object();
		json_mac = json_object_new_object();
	}

	memset(&wctx, 0, sizeof(struct mac_walk_ctx));
	wctx.zvni = zvni;
	wctx.vty = vty;
	wctx.json = json_mac;

	if (!use_json) {
		vty_out(vty,
		"Number of MACs (local and remote) known for this VNI: %u\n",
			num_macs);
		vty_out(vty, "%-17s %-6s %-5s %-30s %-5s\n", "MAC", "Type",
			"Flags", "Intf/Remote ES/VTEP", "VLAN");
	} else
		json_object_int_add(json, "numMacs", num_macs);

	hash_iterate(zvni->mac_table, zvni_print_dad_mac_hash, &wctx);

	if (use_json) {
		json_object_object_add(json, "macs", json_mac);
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}

}

int zebra_vxlan_clear_dup_detect_vni_mac(struct zebra_vrf *zvrf, vni_t vni,
					 struct ethaddr *macaddr)
{
	zebra_vni_t *zvni;
	zebra_mac_t *mac;
	struct listnode *node = NULL;
	zebra_neigh_t *nbr = NULL;

	if (!is_evpn_enabled())
		return 0;

	zvni = zvni_lookup(vni);
	if (!zvni) {
		zlog_warn("VNI %u does not exist\n", vni);
		return -1;
	}

	mac = zvni_mac_lookup(zvni, macaddr);
	if (!mac) {
		zlog_warn("Requested MAC does not exist in VNI %u\n", vni);
		return -1;
	}

	if (!CHECK_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE)) {
		zlog_warn("Requested MAC is not duplicate detected\n");
		return -1;
	}

	/* Remove all IPs as duplicate associcated with this MAC */
	for (ALL_LIST_ELEMENTS_RO(mac->neigh_list, node, nbr)) {
		/* For local neigh mark inactive so MACIP update is generated
		 * to BGP. This is a scenario where MAC update received
		 * and detected as duplicate which marked neigh as duplicate.
		 * Later local neigh update did not get a chance to relay
		 * to BGP. Similarly remote macip update, neigh needs to be
		 * installed locally.
		 */
		if (zvrf->dad_freeze &&
		    CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE)) {
			if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_LOCAL))
				ZEBRA_NEIGH_SET_INACTIVE(nbr);
			else if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_REMOTE))
				zvni_rem_neigh_install(zvni, nbr,
					false /*was_static*/);
		}

		UNSET_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE);
		nbr->dad_count = 0;
		nbr->detect_start_time.tv_sec = 0;
		nbr->dad_dup_detect_time = 0;
	}

	UNSET_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE);
	mac->dad_count = 0;
	mac->detect_start_time.tv_sec = 0;
	mac->detect_start_time.tv_usec = 0;
	mac->dad_dup_detect_time = 0;
	THREAD_OFF(mac->dad_mac_auto_recovery_timer);

	/* warn-only action return */
	if (!zvrf->dad_freeze)
		return 0;

	/* Local: Notify Peer VTEPs, Remote: Install the entry */
	if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL)) {
		/* Inform to BGP */
		if (zvni_mac_send_add_to_client(zvni->vni,
					&mac->macaddr,
					mac->flags,
					mac->loc_seq, mac->es))
			return 0;

		/* Process all neighbors associated with this MAC. */
		zvni_process_neigh_on_local_mac_change(zvni, mac, 0,
				0 /*es_change*/);

	} else if (CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE)) {
		zvni_process_neigh_on_remote_mac_add(zvni, mac);

		/* Install the entry. */
		zvni_rem_mac_install(zvni, mac, false /* was_static */);
	}

	return 0;
}

int zebra_vxlan_clear_dup_detect_vni_ip(struct zebra_vrf *zvrf, vni_t vni,
					struct ipaddr *ip)
{
	zebra_vni_t *zvni;
	zebra_neigh_t *nbr;
	zebra_mac_t *mac;
	char buf[INET6_ADDRSTRLEN];
	char buf2[ETHER_ADDR_STRLEN];

	if (!is_evpn_enabled())
		return 0;

	zvni = zvni_lookup(vni);
	if (!zvni) {
		zlog_debug("VNI %u does not exist\n", vni);
		return -1;
	}

	nbr = zvni_neigh_lookup(zvni, ip);
	if (!nbr) {
		zlog_warn("Requested host IP does not exist in VNI %u\n", vni);
		return -1;
	}

	ipaddr2str(&nbr->ip, buf, sizeof(buf));

	if (!CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE)) {
		zlog_warn("Requested host IP %s is not duplicate detected\n",
			  buf);
		return -1;
	}

	mac = zvni_mac_lookup(zvni, &nbr->emac);

	if (CHECK_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE)) {
		zlog_warn(
			"Requested IP's associated MAC %s is still in duplicate state\n",
			prefix_mac2str(&nbr->emac, buf2, sizeof(buf2)));
		return -1;
	}

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("%s: clear neigh %s in dup state, flags 0x%x seq %u",
			   __func__, buf, nbr->flags, nbr->loc_seq);

	UNSET_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE);
	nbr->dad_count = 0;
	nbr->detect_start_time.tv_sec = 0;
	nbr->detect_start_time.tv_usec = 0;
	nbr->dad_dup_detect_time = 0;
	THREAD_OFF(nbr->dad_ip_auto_recovery_timer);

	if (!!CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_LOCAL)) {
		zvni_neigh_send_add_to_client(zvni->vni, ip,
					      &nbr->emac, nbr->mac,
					      nbr->flags, nbr->loc_seq);
	} else if (!!CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_REMOTE)) {
		zvni_rem_neigh_install(zvni, nbr, false /*was_static*/);
	}

	return 0;
}

static void zvni_clear_dup_mac_hash(struct hash_bucket *bucket, void *ctxt)
{
	struct mac_walk_ctx *wctx = ctxt;
	zebra_mac_t *mac;
	zebra_vni_t *zvni;
	struct listnode *node = NULL;
	zebra_neigh_t *nbr = NULL;

	mac = (zebra_mac_t *)bucket->data;
	if (!mac)
		return;

	zvni = wctx->zvni;

	if (!CHECK_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE))
		return;

	UNSET_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE);
	mac->dad_count = 0;
	mac->detect_start_time.tv_sec = 0;
	mac->detect_start_time.tv_usec = 0;
	mac->dad_dup_detect_time = 0;
	THREAD_OFF(mac->dad_mac_auto_recovery_timer);

	/* Remove all IPs as duplicate associcated with this MAC */
	for (ALL_LIST_ELEMENTS_RO(mac->neigh_list, node, nbr)) {
		if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_LOCAL)
		    && nbr->dad_count)
			ZEBRA_NEIGH_SET_INACTIVE(nbr);

		UNSET_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE);
		nbr->dad_count = 0;
		nbr->detect_start_time.tv_sec = 0;
		nbr->dad_dup_detect_time = 0;
	}

	/* Local: Notify Peer VTEPs, Remote: Install the entry */
	if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL)) {
		/* Inform to BGP */
		if (zvni_mac_send_add_to_client(zvni->vni,
					&mac->macaddr,
					mac->flags, mac->loc_seq, mac->es))
			return;

		/* Process all neighbors associated with this MAC. */
		zvni_process_neigh_on_local_mac_change(zvni, mac, 0,
				0 /*es_change*/);

	} else if (CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE)) {
		zvni_process_neigh_on_remote_mac_add(zvni, mac);

		/* Install the entry. */
		zvni_rem_mac_install(zvni, mac, false /* was_static */);
	}
}

static void zvni_clear_dup_neigh_hash(struct hash_bucket *bucket, void *ctxt)
{
	struct neigh_walk_ctx *wctx = ctxt;
	zebra_neigh_t *nbr;
	zebra_vni_t *zvni;
	char buf[INET6_ADDRSTRLEN];

	nbr = (zebra_neigh_t *)bucket->data;
	if (!nbr)
		return;

	zvni = wctx->zvni;

	if (!CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE))
		return;

	if (IS_ZEBRA_DEBUG_VXLAN) {
		ipaddr2str(&nbr->ip, buf, sizeof(buf));
		zlog_debug("%s: clear neigh %s dup state, flags 0x%x seq %u",
			   __func__, buf, nbr->flags, nbr->loc_seq);
	}

	UNSET_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE);
	nbr->dad_count = 0;
	nbr->detect_start_time.tv_sec = 0;
	nbr->detect_start_time.tv_usec = 0;
	nbr->dad_dup_detect_time = 0;
	THREAD_OFF(nbr->dad_ip_auto_recovery_timer);

	if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_LOCAL)) {
		zvni_neigh_send_add_to_client(zvni->vni, &nbr->ip,
					      &nbr->emac, nbr->mac,
					      nbr->flags, nbr->loc_seq);
	} else if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_REMOTE)) {
		zvni_rem_neigh_install(zvni, nbr, false /*was_static*/);
	}
}

static void zvni_clear_dup_detect_hash_vni_all(struct hash_bucket *bucket,
					    void **args)
{
	zebra_vni_t *zvni;
	struct zebra_vrf *zvrf;
	struct mac_walk_ctx m_wctx;
	struct neigh_walk_ctx n_wctx;

	zvni = (zebra_vni_t *)bucket->data;
	if (!zvni)
		return;

	zvrf = (struct zebra_vrf *)args[0];

	if (hashcount(zvni->neigh_table)) {
		memset(&n_wctx, 0, sizeof(struct neigh_walk_ctx));
		n_wctx.zvni = zvni;
		n_wctx.zvrf = zvrf;
		hash_iterate(zvni->neigh_table, zvni_clear_dup_neigh_hash,
			     &n_wctx);
	}

	if (num_valid_macs(zvni)) {
		memset(&m_wctx, 0, sizeof(struct mac_walk_ctx));
		m_wctx.zvni = zvni;
		m_wctx.zvrf = zvrf;
		hash_iterate(zvni->mac_table, zvni_clear_dup_mac_hash, &m_wctx);
	}

}

int zebra_vxlan_clear_dup_detect_vni_all(struct zebra_vrf *zvrf)
{
	void *args[1];

	if (!is_evpn_enabled())
		return 0;

	args[0] = zvrf;

	hash_iterate(zvrf->vni_table,
		     (void (*)(struct hash_bucket *, void *))
		     zvni_clear_dup_detect_hash_vni_all, args);

	return 0;
}

int zebra_vxlan_clear_dup_detect_vni(struct zebra_vrf *zvrf, vni_t vni)
{
	zebra_vni_t *zvni;
	struct mac_walk_ctx m_wctx;
	struct neigh_walk_ctx n_wctx;

	if (!is_evpn_enabled())
		return 0;

	zvni = zvni_lookup(vni);
	if (!zvni) {
		zlog_warn("VNI %u does not exist\n", vni);
		return -1;
	}

	if (hashcount(zvni->neigh_table)) {
		memset(&n_wctx, 0, sizeof(struct neigh_walk_ctx));
		n_wctx.zvni = zvni;
		n_wctx.zvrf = zvrf;
		hash_iterate(zvni->neigh_table, zvni_clear_dup_neigh_hash,
			     &n_wctx);
	}

	if (num_valid_macs(zvni)) {
		memset(&m_wctx, 0, sizeof(struct mac_walk_ctx));
		m_wctx.zvni = zvni;
		m_wctx.zvrf = zvrf;
		hash_iterate(zvni->mac_table, zvni_clear_dup_mac_hash, &m_wctx);
	}

	return 0;
}

/*
 * Display MACs for a VNI from specific VTEP (VTY command handler).
 */
void zebra_vxlan_print_macs_vni_vtep(struct vty *vty, struct zebra_vrf *zvrf,
				     vni_t vni, struct in_addr vtep_ip,
				     bool use_json)
{
	zebra_vni_t *zvni;
	uint32_t num_macs;
	struct mac_walk_ctx wctx;
	json_object *json = NULL;
	json_object *json_mac = NULL;

	if (!is_evpn_enabled())
		return;
	zvni = zvni_lookup(vni);
	if (!zvni) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty, "%% VNI %u does not exist\n", vni);
		return;
	}
	num_macs = num_valid_macs(zvni);
	if (!num_macs)
		return;

	if (use_json) {
		json = json_object_new_object();
		json_mac = json_object_new_object();
	}

	memset(&wctx, 0, sizeof(struct mac_walk_ctx));
	wctx.zvni = zvni;
	wctx.vty = vty;
	wctx.flags = SHOW_REMOTE_MAC_FROM_VTEP;
	wctx.r_vtep_ip = vtep_ip;
	wctx.json = json_mac;
	hash_iterate(zvni->mac_table, zvni_print_mac_hash, &wctx);

	if (use_json) {
		json_object_int_add(json, "numMacs", wctx.count);
		if (wctx.count)
			json_object_object_add(json, "macs", json_mac);
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}


/*
 * Display VNI information (VTY command handler).
 *
 * use_json flag indicates that output should be in JSON format.
 * json_array is non NULL when JSON output needs to be aggregated (by the
 * caller) and then printed, otherwise, JSON evpn vni info is printed
 * right away.
 */
void zebra_vxlan_print_vni(struct vty *vty, struct zebra_vrf *zvrf, vni_t vni,
			   bool use_json, json_object *json_array)
{
	json_object *json = NULL;
	void *args[2];
	zebra_l3vni_t *zl3vni = NULL;
	zebra_vni_t *zvni = NULL;

	if (!is_evpn_enabled())
		return;

	if (use_json)
		json = json_object_new_object();

	args[0] = vty;
	args[1] = json;

	zl3vni = zl3vni_lookup(vni);
	if (zl3vni) {
		zl3vni_print(zl3vni, (void *)args);
	} else {
		zvni = zvni_lookup(vni);
		if (zvni)
			zvni_print(zvni, (void *)args);
		else if (!json)
			vty_out(vty, "%% VNI %u does not exist\n", vni);
	}

	if (use_json) {
		/*
		 * Each "json" object contains info about 1 VNI.
		 * When "json_array" is non-null, we aggreggate the json output
		 * into json_array and print it as a JSON array.
		 */
		if (json_array)
			json_object_array_add(json_array, json);
		else {
			vty_out(vty, "%s\n", json_object_to_json_string_ext(
				json, JSON_C_TO_STRING_PRETTY));
			json_object_free(json);
		}
	}
}

/* Display all global details for EVPN */
void zebra_vxlan_print_evpn(struct vty *vty, bool uj)
{
	int num_l2vnis = 0;
	int num_l3vnis = 0;
	int num_vnis = 0;
	json_object *json = NULL;
	struct zebra_vrf *zvrf = NULL;

	if (!is_evpn_enabled())
		return;

	zvrf = zebra_vrf_get_evpn();
	if (!zvrf)
		return;

	num_l3vnis = hashcount(zrouter.l3vni_table);
	num_l2vnis = hashcount(zvrf->vni_table);
	num_vnis = num_l2vnis + num_l3vnis;

	if (uj) {
		json = json_object_new_object();
		json_object_string_add(json, "advertiseGatewayMacip",
				       zvrf->advertise_gw_macip ? "Yes" : "No");
		json_object_int_add(json, "numVnis", num_vnis);
		json_object_int_add(json, "numL2Vnis", num_l2vnis);
		json_object_int_add(json, "numL3Vnis", num_l3vnis);
		if (zvrf->dup_addr_detect)
			json_object_boolean_true_add(json,
						"isDuplicateAddrDetection");
		else
			json_object_boolean_false_add(json,
						"isDuplicateAddrDetection");
		json_object_int_add(json, "maxMoves", zvrf->dad_max_moves);
		json_object_int_add(json, "detectionTime", zvrf->dad_time);
		json_object_int_add(json, "detectionFreezeTime",
				    zvrf->dad_freeze_time);

	} else {
		vty_out(vty, "L2 VNIs: %u\n", num_l2vnis);
		vty_out(vty, "L3 VNIs: %u\n", num_l3vnis);
		vty_out(vty, "Advertise gateway mac-ip: %s\n",
			zvrf->advertise_gw_macip ? "Yes" : "No");
		vty_out(vty, "Advertise svi mac-ip: %s\n",
			zvrf->advertise_svi_macip ? "Yes" : "No");
		vty_out(vty, "Duplicate address detection: %s\n",
			zvrf->dup_addr_detect ? "Enable" : "Disable");
		vty_out(vty, "  Detection max-moves %u, time %d\n",
			zvrf->dad_max_moves, zvrf->dad_time);
		if (zvrf->dad_freeze) {
			if (zvrf->dad_freeze_time)
				vty_out(vty, "  Detection freeze %u\n",
					zvrf->dad_freeze_time);
			else
				vty_out(vty, "  Detection freeze %s\n",
					"permanent");
		}
	}

	if (uj) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

/*
 * Display VNI hash table (VTY command handler).
 */
void zebra_vxlan_print_vnis(struct vty *vty, struct zebra_vrf *zvrf,
			    bool use_json)
{
	json_object *json = NULL;
	void *args[2];

	if (!is_evpn_enabled())
		return;

	if (use_json)
		json = json_object_new_object();
	else
		vty_out(vty, "%-10s %-4s %-21s %-8s %-8s %-15s %-37s\n", "VNI",
			"Type", "VxLAN IF", "# MACs", "# ARPs",
			"# Remote VTEPs", "Tenant VRF");

	args[0] = vty;
	args[1] = json;

	/* Display all L2-VNIs */
	hash_iterate(zvrf->vni_table,
		     (void (*)(struct hash_bucket *, void *))zvni_print_hash,
		     args);

	/* Display all L3-VNIs */
	hash_iterate(zrouter.l3vni_table,
		     (void (*)(struct hash_bucket *, void *))zl3vni_print_hash,
		     args);

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}
}

void zebra_vxlan_dup_addr_detection(ZAPI_HANDLER_ARGS)
{
	struct stream *s;
	int time = 0;
	uint32_t max_moves = 0;
	uint32_t freeze_time = 0;
	bool dup_addr_detect = false;
	bool freeze = false;

	s = msg;
	STREAM_GETL(s, dup_addr_detect);
	STREAM_GETL(s, time);
	STREAM_GETL(s, max_moves);
	STREAM_GETL(s, freeze);
	STREAM_GETL(s, freeze_time);

	/* DAD previous state was enabled, and new state is disable,
	 * clear all duplicate detected addresses.
	 */
	if (zvrf->dup_addr_detect && !dup_addr_detect)
		zebra_vxlan_clear_dup_detect_vni_all(zvrf);

	zvrf->dup_addr_detect = dup_addr_detect;
	zvrf->dad_time = time;
	zvrf->dad_max_moves = max_moves;
	zvrf->dad_freeze = freeze;
	zvrf->dad_freeze_time = freeze_time;

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug(
			"VRF %s duplicate detect %s max_moves %u timeout %u freeze %s freeze_time %u",
			vrf_id_to_name(zvrf->vrf->vrf_id),
			zvrf->dup_addr_detect ? "enable" : "disable",
			zvrf->dad_max_moves,
			zvrf->dad_time,
			zvrf->dad_freeze ? "enable" : "disable",
			zvrf->dad_freeze_time);

stream_failure:
	return;
}

/*
 * Display VNI hash table in detail(VTY command handler).
 */
void zebra_vxlan_print_vnis_detail(struct vty *vty, struct zebra_vrf *zvrf,
				   bool use_json)
{
	json_object *json_array = NULL;
	struct zebra_ns *zns = NULL;
	struct zvni_evpn_show zes;

	if (!is_evpn_enabled())
		return;

	zns = zebra_ns_lookup(NS_DEFAULT);
	if (!zns)
		return;

	if (use_json)
		json_array = json_object_new_array();

	zes.vty = vty;
	zes.json = json_array;
	zes.zvrf = zvrf;
	zes.use_json = use_json;

	/* Display all L2-VNIs */
	hash_iterate(
		zvrf->vni_table,
		(void (*)(struct hash_bucket *, void *))zvni_print_hash_detail,
		&zes);

	/* Display all L3-VNIs */
	hash_iterate(zrouter.l3vni_table,
		     (void (*)(struct hash_bucket *,
			       void *))zl3vni_print_hash_detail,
		     &zes);

	if (use_json) {
		vty_out(vty, "%s\n",
			json_object_to_json_string_ext(
				json_array, JSON_C_TO_STRING_PRETTY));
		json_object_free(json_array);
	}
}

/*
 * Handle neighbor delete notification from the kernel (on a VLAN device
 * / L3 interface). This may result in either the neighbor getting deleted
 * from our database or being re-added to the kernel (if it is a valid
 * remote neighbor).
 */
int zebra_vxlan_handle_kernel_neigh_del(struct interface *ifp,
					struct interface *link_if,
					struct ipaddr *ip)
{
	char buf[INET6_ADDRSTRLEN];
	char buf2[ETHER_ADDR_STRLEN];
	zebra_neigh_t *n = NULL;
	zebra_vni_t *zvni = NULL;
	zebra_mac_t *zmac = NULL;
	zebra_l3vni_t *zl3vni = NULL;
	struct zebra_vrf *zvrf;
	bool old_bgp_ready;
	bool new_bgp_ready;

	/* check if this is a remote neigh entry corresponding to remote
	 * next-hop
	 */
	zl3vni = zl3vni_from_svi(ifp, link_if);
	if (zl3vni)
		return zl3vni_local_nh_del(zl3vni, ip);

	/* We are only interested in neighbors on an SVI that resides on top
	 * of a VxLAN bridge.
	 */
	zvni = zvni_from_svi(ifp, link_if);
	if (!zvni) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"%s: Del neighbor %s VNI is not present for interface %s",
				__func__, ipaddr2str(ip, buf, sizeof(buf)),
				ifp->name);
		return 0;
	}

	if (!zvni->vxlan_if) {
		zlog_debug(
			"VNI %u hash %p doesn't have intf upon local neighbor DEL",
			zvni->vni, zvni);
		return -1;
	}

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("Del neighbor %s intf %s(%u) -> L2-VNI %u",
			   ipaddr2str(ip, buf, sizeof(buf)), ifp->name,
			   ifp->ifindex, zvni->vni);

	/* If entry doesn't exist, nothing to do. */
	n = zvni_neigh_lookup(zvni, ip);
	if (!n)
		return 0;

	zmac = zvni_mac_lookup(zvni, &n->emac);
	if (!zmac) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"Trying to del a neigh %s without a mac %s on VNI %u",
				ipaddr2str(ip, buf, sizeof(buf)),
				prefix_mac2str(&n->emac, buf2, sizeof(buf2)),
				zvni->vni);

		return 0;
	}

	/* If it is a remote entry, the kernel has aged this out or someone has
	 * deleted it, it needs to be re-installed as Quagga is the owner.
	 */
	if (CHECK_FLAG(n->flags, ZEBRA_NEIGH_REMOTE)) {
		zvni_rem_neigh_install(zvni, n, false /*was_static*/);
		return 0;
	}

	/* if this is a sync entry it cannot be dropped re-install it in
	 * the dataplane
	 */
	old_bgp_ready =
		zebra_vxlan_neigh_is_ready_for_bgp(n);
	if (zebra_vxlan_neigh_is_static(n)) {
		if (IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
			zlog_debug("re-add sync neigh vni %u ip %s mac %s 0x%x",
					n->zvni->vni,
					ipaddr2str(&n->ip, buf, sizeof(buf)),
					prefix_mac2str(&n->emac, buf2,
						sizeof(buf2)),
					n->flags);

		if (!CHECK_FLAG(n->flags, ZEBRA_NEIGH_LOCAL_INACTIVE))
			SET_FLAG(n->flags, ZEBRA_NEIGH_LOCAL_INACTIVE);
		/* inform-bgp about change in local-activity if any */
		new_bgp_ready =
			zebra_vxlan_neigh_is_ready_for_bgp(n);
		zebra_vxlan_neigh_send_add_del_to_client(n,
				old_bgp_ready, new_bgp_ready);

		/* re-install the entry in the kernel */
		zebra_vxlan_sync_neigh_dp_install(n, false /* set_inactive */,
				false /* force_clear_static */, __func__);

		return 0;
	}

	zvrf = vrf_info_lookup(zvni->vxlan_if->vrf_id);
	if (!zvrf) {
		zlog_debug("%s: VNI %u vrf lookup failed.", __func__,
			   zvni->vni);
		return -1;
	}

	/* In case of feeze action, if local neigh is in duplicate state,
	 * Mark the Neigh as inactive before sending delete request to BGPd,
	 * If BGPd has remote entry, it will re-install
	 */
	if (zvrf->dad_freeze &&
	    CHECK_FLAG(n->flags, ZEBRA_NEIGH_DUPLICATE))
	    ZEBRA_NEIGH_SET_INACTIVE(n);

	/* Remove neighbor from BGP. */
	zvni_neigh_send_del_to_client(zvni->vni, &n->ip,
			&n->emac, n->flags, n->state,
			false /* force */);

	/* Delete this neighbor entry. */
	zvni_neigh_del(zvni, n);

	/* see if the AUTO mac needs to be deleted */
	if (CHECK_FLAG(zmac->flags, ZEBRA_MAC_AUTO)
	    && !listcount(zmac->neigh_list))
		zvni_mac_del(zvni, zmac);

	return 0;
}

/*
 * Handle neighbor add or update notification from the kernel (on a VLAN
 * device / L3 interface). This is typically for a local neighbor but can
 * also be for a remote neighbor (e.g., ageout notification). It could
 * also be a "move" scenario.
 */
int zebra_vxlan_handle_kernel_neigh_update(struct interface *ifp,
					   struct interface *link_if,
					   struct ipaddr *ip,
					   struct ethaddr *macaddr,
					   uint16_t state,
					   bool is_ext,
					   bool is_router,
					   bool local_inactive, bool dp_static)
{
	char buf[ETHER_ADDR_STRLEN];
	char buf2[INET6_ADDRSTRLEN];
	zebra_vni_t *zvni = NULL;
	zebra_l3vni_t *zl3vni = NULL;

	/* check if this is a remote neigh entry corresponding to remote
	 * next-hop
	 */
	zl3vni = zl3vni_from_svi(ifp, link_if);
	if (zl3vni)
		return zl3vni_local_nh_add_update(zl3vni, ip, state);

	/* We are only interested in neighbors on an SVI that resides on top
	 * of a VxLAN bridge.
	 */
	zvni = zvni_from_svi(ifp, link_if);
	if (!zvni)
		return 0;

	if (IS_ZEBRA_DEBUG_VXLAN || IS_ZEBRA_DEBUG_EVPN_MH_NEIGH)
		zlog_debug(
			"Add/Update neighbor %s MAC %s intf %s(%u) state 0x%x %s%s%s-> L2-VNI %u",
			ipaddr2str(ip, buf2, sizeof(buf2)),
			prefix_mac2str(macaddr, buf, sizeof(buf)), ifp->name,
			ifp->ifindex, state, is_ext ? "ext-learned " : "",
			is_router ? "router " : "",
			local_inactive ? "local_inactive " : "",
			zvni->vni);

	/* Is this about a local neighbor or a remote one? */
	if (!is_ext)
		return zvni_local_neigh_update(zvni, ifp, ip, macaddr,
				is_router, local_inactive, dp_static);

	return zvni_remote_neigh_update(zvni, ifp, ip, macaddr, state);
}

static int32_t
zebra_vxlan_remote_macip_helper(bool add, struct stream *s, vni_t *vni,
				struct ethaddr *macaddr, uint16_t *ipa_len,
				struct ipaddr *ip, struct in_addr *vtep_ip,
				uint8_t *flags, uint32_t *seq, esi_t *esi)
{
	uint16_t l = 0;

	/*
	 * Obtain each remote MACIP and process.
	 * Message contains VNI, followed by MAC followed by IP (if any)
	 * followed by remote VTEP IP.
	 */
	memset(ip, 0, sizeof(*ip));
	STREAM_GETL(s, *vni);
	STREAM_GET(macaddr->octet, s, ETH_ALEN);
	STREAM_GETL(s, *ipa_len);

	if (*ipa_len) {
		if (*ipa_len == IPV4_MAX_BYTELEN)
			ip->ipa_type = IPADDR_V4;
		else if (*ipa_len == IPV6_MAX_BYTELEN)
			ip->ipa_type = IPADDR_V6;
		else {
			if (IS_ZEBRA_DEBUG_VXLAN)
				zlog_debug(
					"ipa_len *must* be %d or %d bytes in length not %d",
					IPV4_MAX_BYTELEN, IPV6_MAX_BYTELEN,
					*ipa_len);
			goto stream_failure;
		}

		STREAM_GET(&ip->ip.addr, s, *ipa_len);
	}
	l += 4 + ETH_ALEN + 4 + *ipa_len;
	STREAM_GET(&vtep_ip->s_addr, s, IPV4_MAX_BYTELEN);
	l += IPV4_MAX_BYTELEN;

	if (add) {
		STREAM_GETC(s, *flags);
		STREAM_GETL(s, *seq);
		l += 5;
		STREAM_GET(esi, s, sizeof(esi_t));
		l += sizeof(esi_t);
	}

	return l;

stream_failure:
	return -1;
}

/*
 * Handle message from client to delete a remote MACIP for a VNI.
 */
void zebra_vxlan_remote_macip_del(ZAPI_HANDLER_ARGS)
{
	struct stream *s;
	vni_t vni;
	struct ethaddr macaddr;
	struct ipaddr ip;
	struct in_addr vtep_ip;
	uint16_t l = 0, ipa_len;
	char buf[ETHER_ADDR_STRLEN];
	char buf1[INET6_ADDRSTRLEN];

	memset(&macaddr, 0, sizeof(struct ethaddr));
	memset(&ip, 0, sizeof(struct ipaddr));
	memset(&vtep_ip, 0, sizeof(struct in_addr));

	s = msg;

	while (l < hdr->length) {
		int res_length = zebra_vxlan_remote_macip_helper(
			false, s, &vni, &macaddr, &ipa_len, &ip, &vtep_ip, NULL,
			NULL, NULL);

		if (res_length == -1)
			goto stream_failure;

		l += res_length;
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"Recv MACIP DEL VNI %u MAC %s%s%s Remote VTEP %s from %s",
				vni,
				prefix_mac2str(&macaddr, buf, sizeof(buf)),
				ipa_len ? " IP " : "",
				ipa_len ?
				ipaddr2str(&ip, buf1, sizeof(buf1)) : "",
				inet_ntoa(vtep_ip),
				zebra_route_string(client->proto));

		process_remote_macip_del(vni, &macaddr, ipa_len, &ip, vtep_ip);
	}

stream_failure:
	return;
}

/*
 * Handle message from client to add a remote MACIP for a VNI. This
 * could be just the add of a MAC address or the add of a neighbor
 * (IP+MAC).
 */
void zebra_vxlan_remote_macip_add(ZAPI_HANDLER_ARGS)
{
	struct stream *s;
	vni_t vni;
	struct ethaddr macaddr;
	struct ipaddr ip;
	struct in_addr vtep_ip;
	uint16_t l = 0, ipa_len;
	uint8_t flags = 0;
	uint32_t seq;
	char buf[ETHER_ADDR_STRLEN];
	char buf1[INET6_ADDRSTRLEN];
	esi_t esi;
	char esi_buf[ESI_STR_LEN];

	memset(&macaddr, 0, sizeof(struct ethaddr));
	memset(&ip, 0, sizeof(struct ipaddr));
	memset(&vtep_ip, 0, sizeof(struct in_addr));

	if (!EVPN_ENABLED(zvrf)) {
		zlog_debug("EVPN not enabled, ignoring remote MACIP ADD");
		return;
	}

	s = msg;

	while (l < hdr->length) {
		int res_length = zebra_vxlan_remote_macip_helper(
			true, s, &vni, &macaddr, &ipa_len, &ip, &vtep_ip,
			&flags, &seq, &esi);

		if (res_length == -1)
			goto stream_failure;

		l += res_length;
		if (IS_ZEBRA_DEBUG_VXLAN) {
			if (memcmp(&esi, zero_esi, sizeof(esi_t)))
				esi_to_str(&esi, esi_buf, sizeof(esi_buf));
			else
				strlcpy(esi_buf, "-", ESI_STR_LEN);
			zlog_debug(
				"Recv %sMACIP ADD VNI %u MAC %s%s%s flags 0x%x seq %u VTEP %s ESI %s from %s",
				(flags & ZEBRA_MACIP_TYPE_SYNC_PATH) ?
				"sync-" : "",
				vni,
				prefix_mac2str(&macaddr, buf, sizeof(buf)),
				ipa_len ? " IP " : "",
				ipa_len ?
				ipaddr2str(&ip, buf1, sizeof(buf1)) : "",
				flags, seq, inet_ntoa(vtep_ip), esi_buf,
				zebra_route_string(client->proto));
		}

		process_remote_macip_add(vni, &macaddr, ipa_len, &ip,
					 flags, seq, vtep_ip, &esi);
	}

stream_failure:
	return;
}

/*
 * Handle remote vtep delete by kernel; re-add the vtep if we have it
 */
int zebra_vxlan_check_readd_vtep(struct interface *ifp,
				 struct in_addr vtep_ip)
{
	struct zebra_if *zif;
	struct zebra_vrf *zvrf = NULL;
	struct zebra_l2info_vxlan *vxl;
	vni_t vni;
	zebra_vni_t *zvni = NULL;
	zebra_vtep_t *zvtep = NULL;

	zif = ifp->info;
	assert(zif);
	vxl = &zif->l2info.vxl;
	vni = vxl->vni;

	/* If EVPN is not enabled, nothing to do. */
	if (!is_evpn_enabled())
		return 0;

	/* Locate VRF corresponding to interface. */
	zvrf = vrf_info_lookup(ifp->vrf_id);
	if (!zvrf)
		return -1;

	/* Locate hash entry; it is expected to exist. */
	zvni = zvni_lookup(vni);
	if (!zvni)
		return 0;

	/* If the remote vtep entry doesn't exists nothing to do */
	zvtep = zvni_vtep_find(zvni, &vtep_ip);
	if (!zvtep)
		return 0;

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug(
			"Del MAC for remote VTEP %s intf %s(%u) VNI %u - readd",
			inet_ntoa(vtep_ip), ifp->name, ifp->ifindex, vni);

	zvni_vtep_install(zvni, zvtep);
	return 0;
}

/*
 * Handle notification of MAC add/update over VxLAN. If the kernel is notifying
 * us, this must involve a multihoming scenario. Treat this as implicit delete
 * of any prior local MAC.
 */
int zebra_vxlan_check_del_local_mac(struct interface *ifp,
				    struct interface *br_if,
				    struct ethaddr *macaddr, vlanid_t vid)
{
	struct zebra_if *zif;
	struct zebra_l2info_vxlan *vxl;
	vni_t vni;
	zebra_vni_t *zvni;
	zebra_mac_t *mac;
	char buf[ETHER_ADDR_STRLEN];

	zif = ifp->info;
	assert(zif);
	vxl = &zif->l2info.vxl;
	vni = vxl->vni;

	/* Check if EVPN is enabled. */
	if (!is_evpn_enabled())
		return 0;

	/* Locate hash entry; it is expected to exist. */
	zvni = zvni_lookup(vni);
	if (!zvni)
		return 0;

	/* If entry doesn't exist, nothing to do. */
	mac = zvni_mac_lookup(zvni, macaddr);
	if (!mac)
		return 0;

	/* Is it a local entry? */
	if (!CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL))
		return 0;

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug(
			"Add/update remote MAC %s intf %s(%u) VNI %u flags 0x%x - del local",
			prefix_mac2str(macaddr, buf, sizeof(buf)), ifp->name,
			ifp->ifindex, vni, mac->flags);

	/* Remove MAC from BGP. */
	zvni_mac_send_del_to_client(zvni->vni, macaddr,
			mac->flags, false /* force */);

	/*
	 * If there are no neigh associated with the mac delete the mac
	 * else mark it as AUTO for forward reference
	 */
	if (!listcount(mac->neigh_list)) {
		zvni_mac_del(zvni, mac);
	} else {
		UNSET_FLAG(mac->flags, ZEBRA_MAC_ALL_LOCAL_FLAGS);
		UNSET_FLAG(mac->flags, ZEBRA_MAC_STICKY);
		SET_FLAG(mac->flags, ZEBRA_MAC_AUTO);
	}

	return 0;
}

/*
 * Handle remote MAC delete by kernel; readd the remote MAC if we have it.
 * This can happen because the remote MAC entries are also added as "dynamic",
 * so the kernel can ageout the entry.
 */
int zebra_vxlan_check_readd_remote_mac(struct interface *ifp,
				       struct interface *br_if,
				       struct ethaddr *macaddr, vlanid_t vid)
{
	struct zebra_if *zif = NULL;
	struct zebra_l2info_vxlan *vxl = NULL;
	vni_t vni;
	zebra_vni_t *zvni = NULL;
	zebra_l3vni_t *zl3vni = NULL;
	zebra_mac_t *mac = NULL;
	char buf[ETHER_ADDR_STRLEN];

	zif = ifp->info;
	assert(zif);
	vxl = &zif->l2info.vxl;
	vni = vxl->vni;

	/* Check if EVPN is enabled. */
	if (!is_evpn_enabled())
		return 0;

	/* check if this is a remote RMAC and readd simillar to remote macs */
	zl3vni = zl3vni_lookup(vni);
	if (zl3vni)
		return zebra_vxlan_readd_remote_rmac(zl3vni, macaddr);

	/* Locate hash entry; it is expected to exist. */
	zvni = zvni_lookup(vni);
	if (!zvni)
		return 0;

	/* If entry doesn't exist, nothing to do. */
	mac = zvni_mac_lookup(zvni, macaddr);
	if (!mac)
		return 0;

	/* Is it a remote entry? */
	if (!CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE))
		return 0;

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("Del remote MAC %s intf %s(%u) VNI %u - readd",
			   prefix_mac2str(macaddr, buf, sizeof(buf)), ifp->name,
			   ifp->ifindex, vni);

	zvni_rem_mac_install(zvni, mac, false /* was_static */);
	return 0;
}

/*
 * Handle local MAC delete (on a port or VLAN corresponding to this VNI).
 */
int zebra_vxlan_local_mac_del(struct interface *ifp, struct interface *br_if,
			      struct ethaddr *macaddr, vlanid_t vid)
{
	zebra_vni_t *zvni;
	zebra_mac_t *mac;
	char buf[ETHER_ADDR_STRLEN];
	bool old_bgp_ready;
	bool new_bgp_ready;

	/* We are interested in MACs only on ports or (port, VLAN) that
	 * map to a VNI.
	 */
	zvni = zvni_map_vlan(ifp, br_if, vid);
	if (!zvni)
		return 0;
	if (!zvni->vxlan_if) {
		zlog_debug(
			"VNI %u hash %p doesn't have intf upon local MAC DEL",
			zvni->vni, zvni);
		return -1;
	}

	/* If entry doesn't exist, nothing to do. */
	mac = zvni_mac_lookup(zvni, macaddr);
	if (!mac)
		return 0;

	/* Is it a local entry? */
	if (!CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL))
		return 0;

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("DEL MAC %s intf %s(%u) VID %u -> VNI %u seq %u flags 0x%x nbr count %u",
			   prefix_mac2str(macaddr, buf, sizeof(buf)), ifp->name,
			   ifp->ifindex, vid, zvni->vni, mac->loc_seq,
			   mac->flags, listcount(mac->neigh_list));

	old_bgp_ready = zebra_vxlan_mac_is_ready_for_bgp(mac->flags);
	if (zebra_vxlan_mac_is_static(mac)) {
		/* this is a synced entry and can only be removed when the
		 * es-peers stop advertising it.
		 */
		memset(&mac->fwd_info, 0, sizeof(mac->fwd_info));

		if (IS_ZEBRA_DEBUG_EVPN_MH_MAC)
			zlog_debug("re-add sync-mac vni %u mac %s es %s seq %d f 0x%x",
					zvni->vni,
					prefix_mac2str(macaddr,
						buf, sizeof(buf)),
					mac->es ? mac->es->esi_str : "-",
					mac->loc_seq,
					mac->flags);

		/* inform-bgp about change in local-activity if any */
		if (!CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL_INACTIVE)) {
			SET_FLAG(mac->flags, ZEBRA_MAC_LOCAL_INACTIVE);
			new_bgp_ready = zebra_vxlan_mac_is_ready_for_bgp(mac->flags);
			zebra_vxlan_mac_send_add_del_to_client(mac,
					old_bgp_ready, new_bgp_ready);
		}

		/* re-install the entry in the kernel */
		zebra_vxlan_sync_mac_dp_install(mac, false /* set_inactive */,
				false /* force_clear_static */,
				__func__);

		return 0;
	}

	/* Update all the neigh entries associated with this mac */
	zvni_process_neigh_on_local_mac_del(zvni, mac);

	/* Remove MAC from BGP. */
	zvni_mac_send_del_to_client(zvni->vni, macaddr,
			mac->flags, false /* force */);

	zebra_evpn_es_mac_deref_entry(mac);

	/*
	 * If there are no neigh associated with the mac delete the mac
	 * else mark it as AUTO for forward reference
	 */
	if (!listcount(mac->neigh_list)) {
		zvni_mac_del(zvni, mac);
	} else {
		UNSET_FLAG(mac->flags, ZEBRA_MAC_ALL_LOCAL_FLAGS);
		UNSET_FLAG(mac->flags, ZEBRA_MAC_STICKY);
		SET_FLAG(mac->flags, ZEBRA_MAC_AUTO);
	}

	return 0;
}

/* update local fowarding info. return true if a dest-ES change
 * is detected
 */
static bool zebra_vxlan_local_mac_update_fwd_info(zebra_mac_t *mac,
		struct interface *ifp, vlanid_t vid)
{
	struct zebra_if *zif = ifp->info;
	bool es_change;

	memset(&mac->fwd_info, 0, sizeof(mac->fwd_info));

	es_change = zebra_evpn_es_mac_ref_entry(mac, zif->es_info.es);

	if (!mac->es) {
		/* if es is set fwd_info is not-relevant/taped-out */
		mac->fwd_info.local.ifindex = ifp->ifindex;
		mac->fwd_info.local.vid = vid;
	}

	return es_change;
}

/*
 * Handle local MAC add (on a port or VLAN corresponding to this VNI).
 */
int zebra_vxlan_local_mac_add_update(struct interface *ifp,
				     struct interface *br_if,
				     struct ethaddr *macaddr, vlanid_t vid,
					 bool sticky, bool local_inactive,
					 bool dp_static)
{
	zebra_vni_t *zvni;
	zebra_mac_t *mac;
	struct zebra_vrf *zvrf;
	char buf[ETHER_ADDR_STRLEN];
	bool mac_sticky = false;
	bool inform_client = false;
	bool upd_neigh = false;
	bool is_dup_detect = false;
	struct in_addr vtep_ip = {.s_addr = 0};
	bool es_change = false;
	bool new_bgp_ready;
	/* assume inactive if not present or if not local */
	bool old_local_inactive = true;
	bool old_bgp_ready = false;
	bool inform_dataplane = false;
	bool new_static = false;

	/* We are interested in MACs only on ports or (port, VLAN) that
	 * map to a VNI.
	 */
	zvni = zvni_map_vlan(ifp, br_if, vid);
	if (!zvni) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"        Add/Update %sMAC %s intf %s(%u) VID %u, could not find VNI",
				sticky ? "sticky " : "",
				prefix_mac2str(macaddr, buf, sizeof(buf)),
				ifp->name, ifp->ifindex, vid);
		return 0;
	}

	if (!zvni->vxlan_if) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"        VNI %u hash %p doesn't have intf upon local MAC ADD",
				zvni->vni, zvni);
		return -1;
	}

	zvrf = vrf_info_lookup(zvni->vxlan_if->vrf_id);
	if (!zvrf) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("        No Vrf found for vrf_id: %d",
				   zvni->vxlan_if->vrf_id);
		return -1;
	}

	/* Check if we need to create or update or it is a NO-OP. */
	mac = zvni_mac_lookup(zvni, macaddr);
	if (!mac) {
		if (IS_ZEBRA_DEBUG_VXLAN || IS_ZEBRA_DEBUG_EVPN_MH_MAC)
			zlog_debug(
				"ADD %sMAC %s intf %s(%u) VID %u -> VNI %u%s",
				sticky ? "sticky " : "",
				prefix_mac2str(macaddr, buf, sizeof(buf)),
				ifp->name, ifp->ifindex, vid, zvni->vni,
				local_inactive ? " local-inactive" : "");

		mac = zvni_mac_add(zvni, macaddr);
		if (!mac) {
			flog_err(
				EC_ZEBRA_MAC_ADD_FAILED,
				"Failed to add MAC %s intf %s(%u) VID %u VNI %u",
				prefix_mac2str(macaddr, buf, sizeof(buf)),
				ifp->name, ifp->ifindex, vid, zvni->vni);
			return -1;
		}
		SET_FLAG(mac->flags, ZEBRA_MAC_LOCAL);
		es_change = zebra_vxlan_local_mac_update_fwd_info(mac,
				ifp, vid);
		if (sticky)
			SET_FLAG(mac->flags, ZEBRA_MAC_STICKY);
		inform_client = true;
	} else {
		if (IS_ZEBRA_DEBUG_VXLAN || IS_ZEBRA_DEBUG_EVPN_MH_MAC)
			zlog_debug(
				"UPD %sMAC %s intf %s(%u) VID %u -> VNI %u %scurFlags 0x%x",
				sticky ? "sticky " : "",
				prefix_mac2str(macaddr, buf, sizeof(buf)),
				ifp->name, ifp->ifindex, vid, zvni->vni,
				local_inactive ? "local-inactive " : "",
				mac->flags);

		if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL)) {
			struct interface *old_ifp;
			vlanid_t old_vid;
			bool old_static;

			zebra_vxlan_mac_get_access_info(mac,
					&old_ifp, &old_vid);
			old_bgp_ready = zebra_vxlan_mac_is_ready_for_bgp(
					mac->flags);
			old_local_inactive = !!(mac->flags &
					ZEBRA_MAC_LOCAL_INACTIVE);
			old_static = zebra_vxlan_mac_is_static(mac);
			if (CHECK_FLAG(mac->flags, ZEBRA_MAC_STICKY))
				mac_sticky = true;

			/*
			 * Update any changes and if changes are relevant to
			 * BGP, note it.
			 */
			if (mac_sticky == sticky
			    && old_ifp == ifp
			    && old_vid == vid
				&& old_local_inactive == local_inactive
				&& dp_static == old_static) {
				if (IS_ZEBRA_DEBUG_VXLAN)
					zlog_debug(
						"        Add/Update %sMAC %s intf %s(%u) VID %u -> VNI %u%s, entry exists and has not changed ",
						sticky ? "sticky " : "",
						prefix_mac2str(macaddr, buf,
							       sizeof(buf)),
						ifp->name, ifp->ifindex, vid,
						zvni->vni,
						local_inactive ?
						" local_inactive" : "");
				return 0;
			}
			if (mac_sticky != sticky) {
				if (sticky)
					SET_FLAG(mac->flags,
						 ZEBRA_MAC_STICKY);
				else
					UNSET_FLAG(mac->flags,
						   ZEBRA_MAC_STICKY);
				inform_client = true;
			}

			es_change = zebra_vxlan_local_mac_update_fwd_info(mac,
					ifp, vid);
			/* If an es_change is detected we need to advertise
			 * the route with a sequence that is one
			 * greater. This is need to indicate a mac-move
			 * to the ES peers
			 */
			if (es_change) {
				mac->loc_seq = mac->loc_seq + 1;
				/* force drop the peer/sync info as it is
				 * simply no longer relevant
				 */
				if (CHECK_FLAG(mac->flags,
					ZEBRA_MAC_ALL_PEER_FLAGS)) {
					zebra_vxlan_mac_clear_sync_info(mac);
					new_static =
						zebra_vxlan_mac_is_static(mac);
					/* if we clear peer-flags we
					 * also need to notify the dataplane
					 * to drop the static flag
					 */
					if (old_static != new_static)
						inform_dataplane = true;
				}
			}
		} else if (CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE) ||
			   CHECK_FLAG(mac->flags, ZEBRA_MAC_AUTO)) {
			bool do_dad = false;

			/*
			 * MAC has either moved or was "internally" created due
			 * to a neighbor learn and is now actually learnt. If
			 * it was learnt as a remote sticky MAC, this is an
			 * operator error.
			 */
			if (CHECK_FLAG(mac->flags, ZEBRA_MAC_STICKY)) {
				flog_warn(
					EC_ZEBRA_STICKY_MAC_ALREADY_LEARNT,
					"MAC %s already learnt as remote sticky MAC behind VTEP %s VNI %u",
					prefix_mac2str(macaddr, buf,
						       sizeof(buf)),
					inet_ntoa(mac->fwd_info.r_vtep_ip),
					zvni->vni);
				return 0;
			}

			/* If an actual move, compute MAC's seq number */
			if (CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE)) {
				mac->loc_seq = MAX(mac->rem_seq + 1,
						   mac->loc_seq);
				vtep_ip = mac->fwd_info.r_vtep_ip;
				/* Trigger DAD for remote MAC */
				do_dad = true;
			}

			UNSET_FLAG(mac->flags, ZEBRA_MAC_REMOTE);
			UNSET_FLAG(mac->flags, ZEBRA_MAC_AUTO);
			SET_FLAG(mac->flags, ZEBRA_MAC_LOCAL);
			es_change = zebra_vxlan_local_mac_update_fwd_info(mac,
					ifp, vid);
			if (sticky)
				SET_FLAG(mac->flags, ZEBRA_MAC_STICKY);
			else
				UNSET_FLAG(mac->flags, ZEBRA_MAC_STICKY);
			/*
			 * We have to inform BGP of this MAC as well as process
			 * all neighbors.
			 */
			inform_client = true;
			upd_neigh = true;

			zebra_vxlan_dup_addr_detect_for_mac(zvrf, mac, vtep_ip,
							    do_dad,
							    &is_dup_detect,
							    true);
			if (is_dup_detect) {
				inform_client = false;
				upd_neigh = false;
			}
		}
	}

	/* if the dataplane thinks the entry is sync but it is
	 * not sync in zebra we need to re-install to fixup
	 */
	if (dp_static) {
		new_static = zebra_vxlan_mac_is_static(mac);
		if (!new_static)
			inform_dataplane = true;
	}

	if (local_inactive)
		SET_FLAG(mac->flags, ZEBRA_MAC_LOCAL_INACTIVE);
	else
		UNSET_FLAG(mac->flags, ZEBRA_MAC_LOCAL_INACTIVE);

	new_bgp_ready = zebra_vxlan_mac_is_ready_for_bgp(mac->flags);
	/* if local-activity has changed we need update bgp
	 * even if bgp already knows about the mac
	 */
	if ((old_local_inactive != local_inactive) ||
			(new_bgp_ready != old_bgp_ready)) {
		if (IS_ZEBRA_DEBUG_EVPN_MH_MAC)
			zlog_debug("local mac vni %u mac %s es %s seq %d f 0x%x%s",
					zvni->vni,
					prefix_mac2str(macaddr,
						buf, sizeof(buf)),
					mac->es ? mac->es->esi_str : "",
					mac->loc_seq,
					mac->flags,
					local_inactive ?
					" local-inactive" : "");
		inform_client = true;
	}

	if (es_change) {
		inform_client = true;
		upd_neigh = true;
	}

	/* Inform dataplane if required. */
	if (inform_dataplane)
		zebra_vxlan_sync_mac_dp_install(mac, false /* set_inactive */,
				false /* force_clear_static */, __func__);

	/* Inform BGP if required. */
	if (inform_client)
		zebra_vxlan_mac_send_add_del_to_client(mac,
				old_bgp_ready, new_bgp_ready);

	/* Process all neighbors associated with this MAC, if required. */
	if (upd_neigh)
		zvni_process_neigh_on_local_mac_change(zvni, mac, 0, es_change);

	return 0;
}

/*
 * Handle message from client to delete a remote VTEP for a VNI.
 */
void zebra_vxlan_remote_vtep_del(ZAPI_HANDLER_ARGS)
{
	struct stream *s;
	unsigned short l = 0;
	vni_t vni;
	struct in_addr vtep_ip;
	zebra_vni_t *zvni;
	zebra_vtep_t *zvtep;
	struct interface *ifp;
	struct zebra_if *zif;

	if (!is_evpn_enabled()) {
		zlog_debug(
			"%s: EVPN is not enabled yet we have received a vtep del command",
			__func__);
		return;
	}

	if (!EVPN_ENABLED(zvrf)) {
		zlog_debug("Recv MACIP DEL for non-EVPN VRF %u",
			  zvrf_id(zvrf));
		return;
	}

	s = msg;

	while (l < hdr->length) {
		int flood_control __attribute__((unused));

		/* Obtain each remote VTEP and process. */
		STREAM_GETL(s, vni);
		l += 4;
		STREAM_GET(&vtep_ip.s_addr, s, IPV4_MAX_BYTELEN);
		l += IPV4_MAX_BYTELEN;

		/* Flood control is intentionally ignored right now */
		STREAM_GETL(s, flood_control);
		l += 4;

		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("Recv VTEP_DEL %s VNI %u from %s",
				   inet_ntoa(vtep_ip), vni,
				   zebra_route_string(client->proto));

		/* Locate VNI hash entry - expected to exist. */
		zvni = zvni_lookup(vni);
		if (!zvni) {
			if (IS_ZEBRA_DEBUG_VXLAN)
				zlog_debug(
					"Failed to locate VNI hash upon remote VTEP DEL, VNI %u",
					vni);
			continue;
		}

		ifp = zvni->vxlan_if;
		if (!ifp) {
			zlog_debug(
				"VNI %u hash %p doesn't have intf upon remote VTEP DEL",
				zvni->vni, zvni);
			continue;
		}
		zif = ifp->info;

		/* If down or not mapped to a bridge, we're done. */
		if (!if_is_operative(ifp) || !zif->brslave_info.br_if)
			continue;

		/* If the remote VTEP does not exist, there's nothing more to
		 * do.
		 * Otherwise, uninstall any remote MACs pointing to this VTEP
		 * and
		 * then, the VTEP entry itself and remove it.
		 */
		zvtep = zvni_vtep_find(zvni, &vtep_ip);
		if (!zvtep)
			continue;

		zvni_vtep_uninstall(zvni, &vtep_ip);
		zvni_vtep_del(zvni, zvtep);
	}

stream_failure:
	return;
}

/*
 * Handle message from client to add a remote VTEP for a VNI.
 */
void zebra_vxlan_remote_vtep_add(ZAPI_HANDLER_ARGS)
{
	struct stream *s;
	unsigned short l = 0;
	vni_t vni;
	struct in_addr vtep_ip;
	zebra_vni_t *zvni;
	struct interface *ifp;
	struct zebra_if *zif;
	int flood_control;
	zebra_vtep_t *zvtep;

	if (!is_evpn_enabled()) {
		zlog_debug(
			"%s: EVPN not enabled yet we received a vtep_add zapi call",
			__func__);
		return;
	}

	if (!EVPN_ENABLED(zvrf)) {
		zlog_debug("Recv MACIP ADD for non-EVPN VRF %u",
			  zvrf_id(zvrf));
		return;
	}

	s = msg;

	while (l < hdr->length) {
		/* Obtain each remote VTEP and process. */
		STREAM_GETL(s, vni);
		l += 4;
		STREAM_GET(&vtep_ip.s_addr, s, IPV4_MAX_BYTELEN);
		STREAM_GETL(s, flood_control);
		l += IPV4_MAX_BYTELEN + 4;

		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("Recv VTEP_ADD %s VNI %u flood %d from %s",
					inet_ntoa(vtep_ip), vni, flood_control,
					zebra_route_string(client->proto));

		/* Locate VNI hash entry - expected to exist. */
		zvni = zvni_lookup(vni);
		if (!zvni) {
			flog_err(
				EC_ZEBRA_VTEP_ADD_FAILED,
				"Failed to locate VNI hash upon remote VTEP ADD, VNI %u",
				vni);
			continue;
		}

		ifp = zvni->vxlan_if;
		if (!ifp) {
			flog_err(
				EC_ZEBRA_VTEP_ADD_FAILED,
				"VNI %u hash %p doesn't have intf upon remote VTEP ADD",
				zvni->vni, zvni);
			continue;
		}

		zif = ifp->info;

		/* If down or not mapped to a bridge, we're done. */
		if (!if_is_operative(ifp) || !zif->brslave_info.br_if)
			continue;

		zvtep = zvni_vtep_find(zvni, &vtep_ip);
		if (zvtep) {
			/* If the remote VTEP already exists check if
			 * the flood mode has changed
			 */
			if (zvtep->flood_control != flood_control) {
				if (zvtep->flood_control
						== VXLAN_FLOOD_DISABLED)
					/* old mode was head-end-replication but
					 * is no longer; get rid of the HER fdb
					 * entry installed before
					 */
					zvni_vtep_uninstall(zvni, &vtep_ip);
				zvtep->flood_control = flood_control;
				zvni_vtep_install(zvni, zvtep);
			}
		} else {
			zvtep = zvni_vtep_add(zvni, &vtep_ip, flood_control);
			if (zvtep)
				zvni_vtep_install(zvni, zvtep);
			else
				flog_err(EC_ZEBRA_VTEP_ADD_FAILED,
					"Failed to add remote VTEP, VNI %u zvni %p",
					vni, zvni);
		}
	}

stream_failure:
	return;
}

/*
 * Add/Del gateway macip to evpn
 * g/w can be:
 *  1. SVI interface on a vlan aware bridge
 *  2. SVI interface on a vlan unaware bridge
 *  3. vrr interface (MACVLAN) associated to a SVI
 * We advertise macip routes for an interface if it is associated to VxLan vlan
 */
int zebra_vxlan_add_del_gw_macip(struct interface *ifp, struct prefix *p,
				 int add)
{
	struct ipaddr ip;
	struct ethaddr macaddr;
	zebra_vni_t *zvni = NULL;

	memset(&ip, 0, sizeof(struct ipaddr));
	memset(&macaddr, 0, sizeof(struct ethaddr));

	/* Check if EVPN is enabled. */
	if (!is_evpn_enabled())
		return 0;

	if (IS_ZEBRA_IF_MACVLAN(ifp)) {
		struct interface *svi_if =
			NULL; /* SVI corresponding to the MACVLAN */
		struct zebra_if *ifp_zif =
			NULL; /* Zebra daemon specific info for MACVLAN */
		struct zebra_if *svi_if_zif =
			NULL; /* Zebra daemon specific info for SVI*/

		ifp_zif = ifp->info;
		if (!ifp_zif)
			return -1;

		/*
		 * for a MACVLAN interface the link represents the svi_if
		 */
		svi_if = if_lookup_by_index_per_ns(zebra_ns_lookup(NS_DEFAULT),
						   ifp_zif->link_ifindex);
		if (!svi_if) {
			zlog_debug("MACVLAN %s(%u) without link information",
				   ifp->name, ifp->ifindex);
			return -1;
		}

		if (IS_ZEBRA_IF_VLAN(svi_if)) {
			/*
			 * If it is a vlan aware bridge then the link gives the
			 * bridge information
			 */
			struct interface *svi_if_link = NULL;

			svi_if_zif = svi_if->info;
			if (svi_if_zif) {
				svi_if_link = if_lookup_by_index_per_ns(
					zebra_ns_lookup(NS_DEFAULT),
					svi_if_zif->link_ifindex);
				zvni = zvni_from_svi(svi_if, svi_if_link);
			}
		} else if (IS_ZEBRA_IF_BRIDGE(svi_if)) {
			/*
			 * If it is a vlan unaware bridge then svi is the bridge
			 * itself
			 */
			zvni = zvni_from_svi(svi_if, svi_if);
		}
	} else if (IS_ZEBRA_IF_VLAN(ifp)) {
		struct zebra_if *svi_if_zif =
			NULL; /* Zebra daemon specific info for SVI */
		struct interface *svi_if_link =
			NULL; /* link info for the SVI = bridge info */

		svi_if_zif = ifp->info;
		if (svi_if_zif) {
			svi_if_link = if_lookup_by_index_per_ns(
				zebra_ns_lookup(NS_DEFAULT),
				svi_if_zif->link_ifindex);
			if (svi_if_link)
				zvni = zvni_from_svi(ifp, svi_if_link);
		}
	} else if (IS_ZEBRA_IF_BRIDGE(ifp)) {
		zvni = zvni_from_svi(ifp, ifp);
	}

	if (!zvni)
		return 0;

	if (!zvni->vxlan_if) {
		zlog_debug("VNI %u hash %p doesn't have intf upon MACVLAN up",
			   zvni->vni, zvni);
		return -1;
	}


	memcpy(&macaddr.octet, ifp->hw_addr, ETH_ALEN);

	if (p->family == AF_INET) {
		ip.ipa_type = IPADDR_V4;
		memcpy(&(ip.ipaddr_v4), &(p->u.prefix4),
		       sizeof(struct in_addr));
	} else if (p->family == AF_INET6) {
		ip.ipa_type = IPADDR_V6;
		memcpy(&(ip.ipaddr_v6), &(p->u.prefix6),
		       sizeof(struct in6_addr));
	}


	if (add)
		zvni_gw_macip_add(ifp, zvni, &macaddr, &ip);
	else
		zvni_gw_macip_del(ifp, zvni, &ip);

	return 0;
}

/*
 * Handle SVI interface going down.
 * SVI can be associated to either L3-VNI or L2-VNI.
 * For L2-VNI: At this point, this is a NOP since
 *	the kernel deletes the neighbor entries on this SVI (if any).
 *      We only need to update the vrf corresponding to zvni.
 * For L3-VNI: L3-VNI is operationally down, update mac-ip routes and delete
 *	from bgp
 */
int zebra_vxlan_svi_down(struct interface *ifp, struct interface *link_if)
{
	zebra_l3vni_t *zl3vni = NULL;

	zl3vni = zl3vni_from_svi(ifp, link_if);
	if (zl3vni) {

		/* process l3-vni down */
		zebra_vxlan_process_l3vni_oper_down(zl3vni);

		/* remove association with svi-if */
		zl3vni->svi_if = NULL;
	} else {
		zebra_vni_t *zvni = NULL;

		/* since we dont have svi corresponding to zvni, we associate it
		 * to default vrf. Note: the corresponding neigh entries on the
		 * SVI would have already been deleted */
		zvni = zvni_from_svi(ifp, link_if);
		if (zvni) {
			zvni->vrf_id = VRF_DEFAULT;

			/* update the tenant vrf in BGP */
			zvni_send_add_to_client(zvni);
		}
	}
	return 0;
}

/*
 * Handle SVI interface coming up.
 * SVI can be associated to L3-VNI (l3vni vxlan interface) or L2-VNI (l2-vni
 * vxlan intf).
 * For L2-VNI: we need to install any remote neighbors entried (used for
 * apr-suppression)
 * For L3-VNI: SVI will be used to get the rmac to be used with L3-VNI
 */
int zebra_vxlan_svi_up(struct interface *ifp, struct interface *link_if)
{
	zebra_vni_t *zvni = NULL;
	zebra_l3vni_t *zl3vni = NULL;

	zl3vni = zl3vni_from_svi(ifp, link_if);
	if (zl3vni) {

		/* associate with svi */
		zl3vni->svi_if = ifp;

		/* process oper-up */
		if (is_l3vni_oper_up(zl3vni))
			zebra_vxlan_process_l3vni_oper_up(zl3vni);
	} else {

		/* process SVI up for l2-vni */
		struct neigh_walk_ctx n_wctx;

		zvni = zvni_from_svi(ifp, link_if);
		if (!zvni)
			return 0;

		if (!zvni->vxlan_if) {
			zlog_debug(
				"VNI %u hash %p doesn't have intf upon SVI up",
				zvni->vni, zvni);
			return -1;
		}

		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"SVI %s(%u) VNI %u VRF %s is UP, installing neighbors",
				ifp->name, ifp->ifindex, zvni->vni,
				vrf_id_to_name(ifp->vrf_id));

		/* update the vrf information for l2-vni and inform bgp */
		zvni->vrf_id = ifp->vrf_id;
		zvni_send_add_to_client(zvni);

		/* Install any remote neighbors for this VNI. */
		memset(&n_wctx, 0, sizeof(struct neigh_walk_ctx));
		n_wctx.zvni = zvni;
		hash_iterate(zvni->neigh_table, zvni_install_neigh_hash,
			     &n_wctx);
	}

	return 0;
}

/*
 * Handle MAC-VLAN interface going down.
 * L3VNI: When MAC-VLAN interface goes down,
 * find its associated SVI and update type2/type-5 routes
 * with SVI as RMAC
 */
void zebra_vxlan_macvlan_down(struct interface *ifp)
{
	zebra_l3vni_t *zl3vni = NULL;
	struct zebra_if *zif, *link_zif;
	struct interface *link_ifp, *link_if;

	zif = ifp->info;
	assert(zif);
	link_ifp = zif->link;
	if (!link_ifp) {
		if (IS_ZEBRA_DEBUG_VXLAN) {
			struct interface *ifp;

			ifp = if_lookup_by_index_all_vrf(zif->link_ifindex);
			zlog_debug("macvlan parent link is not found. Parent index %d ifp %s",
				zif->link_ifindex, ifp ? ifp->name : " ");
		}
		return;
	}
	link_zif = link_ifp->info;
	assert(link_zif);

	link_if = if_lookup_by_index_per_ns(zebra_ns_lookup(NS_DEFAULT),
					    link_zif->link_ifindex);

	zl3vni = zl3vni_from_svi(link_ifp, link_if);
	if (zl3vni) {
		zl3vni->mac_vlan_if = NULL;
		if (is_l3vni_oper_up(zl3vni))
			zebra_vxlan_process_l3vni_oper_up(zl3vni);
	}
}

/*
 * Handle MAC-VLAN interface going up.
 * L3VNI: When MAC-VLAN interface comes up,
 * find its associated SVI and update type-2 routes
 * with MAC-VLAN's MAC as RMAC and for type-5 routes
 * use SVI's MAC as RMAC.
 */
void zebra_vxlan_macvlan_up(struct interface *ifp)
{
	zebra_l3vni_t *zl3vni = NULL;
	struct zebra_if *zif, *link_zif;
	struct interface *link_ifp, *link_if;

	zif = ifp->info;
	assert(zif);
	link_ifp = zif->link;
	link_zif = link_ifp->info;
	assert(link_zif);

	link_if = if_lookup_by_index_per_ns(zebra_ns_lookup(NS_DEFAULT),
					    link_zif->link_ifindex);
	zl3vni = zl3vni_from_svi(link_ifp, link_if);
	if (zl3vni) {
		/* associate with macvlan (VRR) interface */
		zl3vni->mac_vlan_if = ifp;

		/* process oper-up */
		if (is_l3vni_oper_up(zl3vni))
			zebra_vxlan_process_l3vni_oper_up(zl3vni);
	}
}

/*
 * Handle VxLAN interface down
 */
int zebra_vxlan_if_down(struct interface *ifp)
{
	vni_t vni;
	struct zebra_if *zif = NULL;
	struct zebra_l2info_vxlan *vxl = NULL;
	zebra_l3vni_t *zl3vni = NULL;
	zebra_vni_t *zvni;

	/* Check if EVPN is enabled. */
	if (!is_evpn_enabled())
		return 0;

	zif = ifp->info;
	assert(zif);
	vxl = &zif->l2info.vxl;
	vni = vxl->vni;

	zl3vni = zl3vni_lookup(vni);
	if (zl3vni) {
		/* process-if-down for l3-vni */
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("Intf %s(%u) L3-VNI %u is DOWN", ifp->name,
				   ifp->ifindex, vni);

		zebra_vxlan_process_l3vni_oper_down(zl3vni);
	} else {
		/* process if-down for l2-vni */
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("Intf %s(%u) L2-VNI %u is DOWN", ifp->name,
				   ifp->ifindex, vni);

		/* Locate hash entry; it is expected to exist. */
		zvni = zvni_lookup(vni);
		if (!zvni) {
			zlog_debug(
				"Failed to locate VNI hash at DOWN, IF %s(%u) VNI %u",
				ifp->name, ifp->ifindex, vni);
			return -1;
		}

		assert(zvni->vxlan_if == ifp);

		/* Delete this VNI from BGP. */
		zvni_send_del_to_client(zvni);

		/* Free up all neighbors and MACs, if any. */
		zvni_neigh_del_all(zvni, 1, 0, DEL_ALL_NEIGH);
		zvni_mac_del_all(zvni, 1, 0, DEL_ALL_MAC);

		/* Free up all remote VTEPs, if any. */
		zvni_vtep_del_all(zvni, 1);
	}
	return 0;
}

/*
 * Handle VxLAN interface up - update BGP if required.
 */
int zebra_vxlan_if_up(struct interface *ifp)
{
	vni_t vni;
	struct zebra_if *zif = NULL;
	struct zebra_l2info_vxlan *vxl = NULL;
	zebra_vni_t *zvni = NULL;
	zebra_l3vni_t *zl3vni = NULL;

	/* Check if EVPN is enabled. */
	if (!is_evpn_enabled())
		return 0;

	zif = ifp->info;
	assert(zif);
	vxl = &zif->l2info.vxl;
	vni = vxl->vni;

	zl3vni = zl3vni_lookup(vni);
	if (zl3vni) {
		/* we need to associate with SVI, if any, we can associate with
		 * svi-if only after association with vxlan-intf is complete
		 */
		zl3vni->svi_if = zl3vni_map_to_svi_if(zl3vni);
		zl3vni->mac_vlan_if = zl3vni_map_to_mac_vlan_if(zl3vni);

		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("Intf %s(%u) L3-VNI %u is UP svi_if %s mac_vlan_if %s"
				, ifp->name, ifp->ifindex, vni,
				zl3vni->svi_if ? zl3vni->svi_if->name : "NIL",
				zl3vni->mac_vlan_if ?
				zl3vni->mac_vlan_if->name : "NIL");

		if (is_l3vni_oper_up(zl3vni))
			zebra_vxlan_process_l3vni_oper_up(zl3vni);
	} else {
		/* Handle L2-VNI add */
		struct interface *vlan_if = NULL;

		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("Intf %s(%u) L2-VNI %u is UP", ifp->name,
				   ifp->ifindex, vni);

		/* Locate hash entry; it is expected to exist. */
		zvni = zvni_lookup(vni);
		if (!zvni) {
			zlog_debug(
				"Failed to locate VNI hash at UP, IF %s(%u) VNI %u",
				ifp->name, ifp->ifindex, vni);
			return -1;
		}

		assert(zvni->vxlan_if == ifp);
		vlan_if = zvni_map_to_svi(vxl->access_vlan,
					  zif->brslave_info.br_if);
		if (vlan_if) {
			zvni->vrf_id = vlan_if->vrf_id;
			zl3vni = zl3vni_from_vrf(vlan_if->vrf_id);
			if (zl3vni)
				listnode_add_sort(zl3vni->l2vnis, zvni);
		}

		/* If part of a bridge, inform BGP about this VNI. */
		/* Also, read and populate local MACs and neighbors. */
		if (zif->brslave_info.br_if) {
			zvni_send_add_to_client(zvni);
			zvni_read_mac_neigh(zvni, ifp);
		}
	}

	return 0;
}

/*
 * Handle VxLAN interface delete. Locate and remove entry in hash table
 * and update BGP, if required.
 */
int zebra_vxlan_if_del(struct interface *ifp)
{
	vni_t vni;
	struct zebra_if *zif = NULL;
	struct zebra_l2info_vxlan *vxl = NULL;
	zebra_vni_t *zvni = NULL;
	zebra_l3vni_t *zl3vni = NULL;

	/* Check if EVPN is enabled. */
	if (!is_evpn_enabled())
		return 0;

	zif = ifp->info;
	assert(zif);
	vxl = &zif->l2info.vxl;
	vni = vxl->vni;

	zl3vni = zl3vni_lookup(vni);
	if (zl3vni) {

		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("Del L3-VNI %u intf %s(%u)", vni, ifp->name,
				   ifp->ifindex);

		/* process oper-down for l3-vni */
		zebra_vxlan_process_l3vni_oper_down(zl3vni);

		/* remove the association with vxlan_if */
		memset(&zl3vni->local_vtep_ip, 0, sizeof(struct in_addr));
		zl3vni->vxlan_if = NULL;
	} else {

		/* process if-del for l2-vni*/
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("Del L2-VNI %u intf %s(%u)", vni, ifp->name,
				   ifp->ifindex);

		/* Locate hash entry; it is expected to exist. */
		zvni = zvni_lookup(vni);
		if (!zvni) {
			zlog_debug(
				"Failed to locate VNI hash at del, IF %s(%u) VNI %u",
				ifp->name, ifp->ifindex, vni);
			return 0;
		}

		/* remove from l3-vni list */
		zl3vni = zl3vni_from_vrf(zvni->vrf_id);
		if (zl3vni)
			listnode_delete(zl3vni->l2vnis, zvni);
		/* Delete VNI from BGP. */
		zvni_send_del_to_client(zvni);

		/* Free up all neighbors and MAC, if any. */
		zvni_neigh_del_all(zvni, 0, 0, DEL_ALL_NEIGH);
		zvni_mac_del_all(zvni, 0, 0, DEL_ALL_MAC);

		/* Free up all remote VTEPs, if any. */
		zvni_vtep_del_all(zvni, 0);

		/* Delete the hash entry. */
		if (zvni_del(zvni)) {
			flog_err(EC_ZEBRA_VNI_DEL_FAILED,
				 "Failed to del VNI hash %p, IF %s(%u) VNI %u",
				 zvni, ifp->name, ifp->ifindex, zvni->vni);
			return -1;
		}
	}
	return 0;
}

/*
 * Handle VxLAN interface update - change to tunnel IP, master or VLAN.
 */
int zebra_vxlan_if_update(struct interface *ifp, uint16_t chgflags)
{
	vni_t vni;
	struct zebra_if *zif = NULL;
	struct zebra_l2info_vxlan *vxl = NULL;
	zebra_vni_t *zvni = NULL;
	zebra_l3vni_t *zl3vni = NULL;

	/* Check if EVPN is enabled. */
	if (!is_evpn_enabled())
		return 0;

	zif = ifp->info;
	assert(zif);
	vxl = &zif->l2info.vxl;
	vni = vxl->vni;

	zl3vni = zl3vni_lookup(vni);
	if (zl3vni) {

		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"Update L3-VNI %u intf %s(%u) VLAN %u local IP %s master %u chg 0x%x",
				vni, ifp->name, ifp->ifindex, vxl->access_vlan,
				inet_ntoa(vxl->vtep_ip),
				zif->brslave_info.bridge_ifindex, chgflags);

		/* Removed from bridge? Cleanup and return */
		if ((chgflags & ZEBRA_VXLIF_MASTER_CHANGE)
		    && (zif->brslave_info.bridge_ifindex == IFINDEX_INTERNAL)) {
			zebra_vxlan_process_l3vni_oper_down(zl3vni);
			return 0;
		}

		/* access-vlan change - process oper down, associate with new
		 * svi_if and then process oper up again
		 */
		if (chgflags & ZEBRA_VXLIF_VLAN_CHANGE) {
			if (if_is_operative(ifp)) {
				zebra_vxlan_process_l3vni_oper_down(zl3vni);
				zl3vni->svi_if = NULL;
				zl3vni->svi_if = zl3vni_map_to_svi_if(zl3vni);
				zl3vni->mac_vlan_if =
					zl3vni_map_to_mac_vlan_if(zl3vni);
				zl3vni->local_vtep_ip = vxl->vtep_ip;
				if (is_l3vni_oper_up(zl3vni))
					zebra_vxlan_process_l3vni_oper_up(
						zl3vni);
			}
		}

		/*
		 * local-ip change - process oper down, associate with new
		 * local-ip and then process oper up again
		 */
		if (chgflags & ZEBRA_VXLIF_LOCAL_IP_CHANGE) {
			if (if_is_operative(ifp)) {
				zebra_vxlan_process_l3vni_oper_down(zl3vni);
				zl3vni->local_vtep_ip = vxl->vtep_ip;
				if (is_l3vni_oper_up(zl3vni))
					zebra_vxlan_process_l3vni_oper_up(
						zl3vni);
			}
		}

		/* Update local tunnel IP. */
		zl3vni->local_vtep_ip = vxl->vtep_ip;

		/* if we have a valid new master, process l3-vni oper up */
		if (chgflags & ZEBRA_VXLIF_MASTER_CHANGE) {
			if (if_is_operative(ifp) && is_l3vni_oper_up(zl3vni))
				zebra_vxlan_process_l3vni_oper_up(zl3vni);
		}
	} else {

		/* Update VNI hash. */
		zvni = zvni_lookup(vni);
		if (!zvni) {
			zlog_debug(
				"Failed to find L2-VNI hash on update, IF %s(%u) VNI %u",
				ifp->name, ifp->ifindex, vni);
			return -1;
		}

		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"Update L2-VNI %u intf %s(%u) VLAN %u local IP %s master %u chg 0x%x",
				vni, ifp->name, ifp->ifindex, vxl->access_vlan,
				inet_ntoa(vxl->vtep_ip),
				zif->brslave_info.bridge_ifindex, chgflags);

		/* Removed from bridge? Cleanup and return */
		if ((chgflags & ZEBRA_VXLIF_MASTER_CHANGE)
		    && (zif->brslave_info.bridge_ifindex == IFINDEX_INTERNAL)) {
			/* Delete from client, remove all remote VTEPs */
			/* Also, free up all MACs and neighbors. */
			zvni_send_del_to_client(zvni);
			zvni_neigh_del_all(zvni, 1, 0, DEL_ALL_NEIGH);
			zvni_mac_del_all(zvni, 1, 0, DEL_ALL_MAC);
			zvni_vtep_del_all(zvni, 1);
			return 0;
		}

		/* Handle other changes. */
		if (chgflags & ZEBRA_VXLIF_VLAN_CHANGE) {
			/* Remove all existing local neigh and MACs for this VNI
			 * (including from BGP)
			 */
			zvni_neigh_del_all(zvni, 0, 1, DEL_LOCAL_MAC);
			zvni_mac_del_all(zvni, 0, 1, DEL_LOCAL_MAC);
		}

		if (zvni->local_vtep_ip.s_addr != vxl->vtep_ip.s_addr ||
			zvni->mcast_grp.s_addr != vxl->mcast_grp.s_addr) {
			zebra_vxlan_sg_deref(zvni->local_vtep_ip,
				zvni->mcast_grp);
			zebra_vxlan_sg_ref(vxl->vtep_ip, vxl->mcast_grp);
			zvni->local_vtep_ip = vxl->vtep_ip;
			zvni->mcast_grp = vxl->mcast_grp;
			/* on local vtep-ip check if ES orig-ip
			 * needs to be updated
			 */
			zebra_evpn_es_set_base_vni(zvni);
		}
		zvni_vxlan_if_set(zvni, ifp, true /* set */);
		/* Take further actions needed.
		 * Note that if we are here, there is a change of interest.
		 */
		/* If down or not mapped to a bridge, we're done. */
		if (!if_is_operative(ifp) || !zif->brslave_info.br_if)
			return 0;

		/* Inform BGP, if there is a change of interest. */
		if (chgflags
			& (ZEBRA_VXLIF_MASTER_CHANGE |
			   ZEBRA_VXLIF_LOCAL_IP_CHANGE |
			   ZEBRA_VXLIF_MCAST_GRP_CHANGE))
			zvni_send_add_to_client(zvni);

		/* If there is a valid new master or a VLAN mapping change,
		 * read and populate local MACs and neighbors.
		 * Also, reinstall any remote MACs and neighbors
		 * for this VNI (based on new VLAN).
		 */
		if (chgflags & ZEBRA_VXLIF_MASTER_CHANGE)
			zvni_read_mac_neigh(zvni, ifp);
		else if (chgflags & ZEBRA_VXLIF_VLAN_CHANGE) {
			struct mac_walk_ctx m_wctx;
			struct neigh_walk_ctx n_wctx;

			zvni_read_mac_neigh(zvni, ifp);

			memset(&m_wctx, 0, sizeof(struct mac_walk_ctx));
			m_wctx.zvni = zvni;
			hash_iterate(zvni->mac_table, zvni_install_mac_hash,
				     &m_wctx);

			memset(&n_wctx, 0, sizeof(struct neigh_walk_ctx));
			n_wctx.zvni = zvni;
			hash_iterate(zvni->neigh_table, zvni_install_neigh_hash,
				     &n_wctx);
		}
	}

	return 0;
}

/*
 * Handle VxLAN interface add.
 */
int zebra_vxlan_if_add(struct interface *ifp)
{
	vni_t vni;
	struct zebra_if *zif = NULL;
	struct zebra_l2info_vxlan *vxl = NULL;
	zebra_vni_t *zvni = NULL;
	zebra_l3vni_t *zl3vni = NULL;

	/* Check if EVPN is enabled. */
	if (!is_evpn_enabled())
		return 0;

	zif = ifp->info;
	assert(zif);
	vxl = &zif->l2info.vxl;
	vni = vxl->vni;

	zl3vni = zl3vni_lookup(vni);
	if (zl3vni) {

		/* process if-add for l3-vni*/
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"Add L3-VNI %u intf %s(%u) VLAN %u local IP %s master %u",
				vni, ifp->name, ifp->ifindex, vxl->access_vlan,
				inet_ntoa(vxl->vtep_ip),
				zif->brslave_info.bridge_ifindex);

		/* associate with vxlan_if */
		zl3vni->local_vtep_ip = vxl->vtep_ip;
		zl3vni->vxlan_if = ifp;

		/* Associate with SVI, if any. We can associate with svi-if only
		 * after association with vxlan_if is complete */
		zl3vni->svi_if = zl3vni_map_to_svi_if(zl3vni);

		zl3vni->mac_vlan_if = zl3vni_map_to_mac_vlan_if(zl3vni);

		if (is_l3vni_oper_up(zl3vni))
			zebra_vxlan_process_l3vni_oper_up(zl3vni);
	} else {

		/* process if-add for l2-vni */
		struct interface *vlan_if = NULL;

		/* Create or update VNI hash. */
		zvni = zvni_lookup(vni);
		if (!zvni) {
			zvni = zvni_add(vni);
			if (!zvni) {
				flog_err(
					EC_ZEBRA_VNI_ADD_FAILED,
					"Failed to add VNI hash, IF %s(%u) VNI %u",
					ifp->name, ifp->ifindex, vni);
				return -1;
			}
		}

		if (zvni->local_vtep_ip.s_addr != vxl->vtep_ip.s_addr ||
			zvni->mcast_grp.s_addr != vxl->mcast_grp.s_addr) {
			zebra_vxlan_sg_deref(zvni->local_vtep_ip,
				zvni->mcast_grp);
			zebra_vxlan_sg_ref(vxl->vtep_ip, vxl->mcast_grp);
			zvni->local_vtep_ip = vxl->vtep_ip;
			zvni->mcast_grp = vxl->mcast_grp;
			/* on local vtep-ip check if ES orig-ip
			 * needs to be updated
			 */
			zebra_evpn_es_set_base_vni(zvni);
		}
		zvni_vxlan_if_set(zvni, ifp, true /* set */);
		vlan_if = zvni_map_to_svi(vxl->access_vlan,
					  zif->brslave_info.br_if);
		if (vlan_if) {
			zvni->vrf_id = vlan_if->vrf_id;
			zl3vni = zl3vni_from_vrf(vlan_if->vrf_id);
			if (zl3vni)
				listnode_add_sort(zl3vni->l2vnis, zvni);
		}

		if (IS_ZEBRA_DEBUG_VXLAN) {
			char addr_buf1[INET_ADDRSTRLEN];
			char addr_buf2[INET_ADDRSTRLEN];

			inet_ntop(AF_INET, &vxl->vtep_ip,
					addr_buf1, INET_ADDRSTRLEN);
			inet_ntop(AF_INET, &vxl->mcast_grp,
					addr_buf2, INET_ADDRSTRLEN);

			zlog_debug(
				"Add L2-VNI %u VRF %s intf %s(%u) VLAN %u local IP %s mcast_grp %s master %u",
				vni,
				vlan_if ? vrf_id_to_name(vlan_if->vrf_id)
					: VRF_DEFAULT_NAME,
				ifp->name, ifp->ifindex, vxl->access_vlan,
				addr_buf1, addr_buf2,
				zif->brslave_info.bridge_ifindex);
		}

		/* If down or not mapped to a bridge, we're done. */
		if (!if_is_operative(ifp) || !zif->brslave_info.br_if)
			return 0;

		/* Inform BGP */
		zvni_send_add_to_client(zvni);

		/* Read and populate local MACs and neighbors */
		zvni_read_mac_neigh(zvni, ifp);
	}

	return 0;
}

int zebra_vxlan_process_vrf_vni_cmd(struct zebra_vrf *zvrf, vni_t vni,
				    char *err, int err_str_sz, int filter,
				    int add)
{
	zebra_l3vni_t *zl3vni = NULL;
	struct zebra_vrf *zvrf_evpn = NULL;

	zvrf_evpn = zebra_vrf_get_evpn();
	if (!zvrf_evpn)
		return -1;

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("vrf %s vni %u %s", zvrf_name(zvrf), vni,
			   add ? "ADD" : "DEL");

	if (add) {

		zebra_vxlan_handle_vni_transition(zvrf, vni, add);

		/* check if the vni is already present under zvrf */
		if (zvrf->l3vni) {
			snprintf(err, err_str_sz,
				 "VNI is already configured under the vrf");
			return -1;
		}

		/* check if this VNI is already present in the system */
		zl3vni = zl3vni_lookup(vni);
		if (zl3vni) {
			snprintf(err, err_str_sz,
				 "VNI is already configured as L3-VNI");
			return -1;
		}

		/* add the L3-VNI to the global table */
		zl3vni = zl3vni_add(vni, zvrf_id(zvrf));
		if (!zl3vni) {
			snprintf(err, err_str_sz, "Could not add L3-VNI");
			return -1;
		}

		/* associate the vrf with vni */
		zvrf->l3vni = vni;

		/* set the filter in l3vni to denote if we are using l3vni only
		 * for prefix routes
		 */
		if (filter)
			SET_FLAG(zl3vni->filter, PREFIX_ROUTES_ONLY);

		/* associate with vxlan-intf;
		 * we need to associate with the vxlan-intf first
		 */
		zl3vni->vxlan_if = zl3vni_map_to_vxlan_if(zl3vni);

		/* associate with corresponding SVI interface, we can associate
		 * with svi-if only after vxlan interface association is
		 * complete
		 */
		zl3vni->svi_if = zl3vni_map_to_svi_if(zl3vni);

		zl3vni->mac_vlan_if = zl3vni_map_to_mac_vlan_if(zl3vni);

		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"%s: l3vni %u svi_if %s mac_vlan_if %s",
				__func__, vni,
				zl3vni->svi_if ? zl3vni->svi_if->name : "NIL",
				zl3vni->mac_vlan_if ? zl3vni->mac_vlan_if->name
						    : "NIL");

		/* formulate l2vni list */
		hash_iterate(zvrf_evpn->vni_table, zvni_add_to_l3vni_list,
			     zl3vni);

		if (is_l3vni_oper_up(zl3vni))
			zebra_vxlan_process_l3vni_oper_up(zl3vni);

	} else {
		zl3vni = zl3vni_lookup(vni);
		if (!zl3vni) {
			snprintf(err, err_str_sz, "VNI doesn't exist");
			return -1;
		}

		if (zvrf->l3vni != vni) {
			snprintf(err, err_str_sz,
					"VNI %d doesn't exist in VRF: %s",
					vni, zvrf->vrf->name);
			return -1;
		}

		if (filter && !CHECK_FLAG(zl3vni->filter, PREFIX_ROUTES_ONLY)) {
			snprintf(err, ERR_STR_SZ,
				 "prefix-routes-only is not set for the vni");
			return -1;
		}

		zebra_vxlan_process_l3vni_oper_down(zl3vni);

		/* delete and uninstall all rmacs */
		hash_iterate(zl3vni->rmac_table, zl3vni_del_rmac_hash_entry,
			     zl3vni);

		/* delete and uninstall all next-hops */
		hash_iterate(zl3vni->nh_table, zl3vni_del_nh_hash_entry,
			     zl3vni);

		zvrf->l3vni = 0;
		zl3vni_del(zl3vni);

		zebra_vxlan_handle_vni_transition(zvrf, vni, add);
	}
	return 0;
}

int zebra_vxlan_vrf_enable(struct zebra_vrf *zvrf)
{
	zebra_l3vni_t *zl3vni = NULL;

	if (zvrf->l3vni)
		zl3vni = zl3vni_lookup(zvrf->l3vni);
	if (!zl3vni)
		return 0;

	zl3vni->vrf_id = zvrf_id(zvrf);
	if (is_l3vni_oper_up(zl3vni))
		zebra_vxlan_process_l3vni_oper_up(zl3vni);
	return 0;
}

int zebra_vxlan_vrf_disable(struct zebra_vrf *zvrf)
{
	zebra_l3vni_t *zl3vni = NULL;

	if (zvrf->l3vni)
		zl3vni = zl3vni_lookup(zvrf->l3vni);
	if (!zl3vni)
		return 0;

	zebra_vxlan_process_l3vni_oper_down(zl3vni);

	/* delete and uninstall all rmacs */
	hash_iterate(zl3vni->rmac_table, zl3vni_del_rmac_hash_entry, zl3vni);
	/* delete and uninstall all next-hops */
	hash_iterate(zl3vni->nh_table, zl3vni_del_nh_hash_entry, zl3vni);

	zl3vni->vrf_id = VRF_UNKNOWN;

	return 0;
}

int zebra_vxlan_vrf_delete(struct zebra_vrf *zvrf)
{
	zebra_l3vni_t *zl3vni = NULL;
	vni_t vni;

	if (zvrf->l3vni)
		zl3vni = zl3vni_lookup(zvrf->l3vni);
	if (!zl3vni)
		return 0;

	vni = zl3vni->vni;
	zl3vni_del(zl3vni);
	zebra_vxlan_handle_vni_transition(zvrf, vni, 0);

	return 0;
}

/*
 * Handle message from client to specify the flooding mechanism for
 * BUM packets. The default is to do head-end (ingress) replication
 * and the other supported option is to disable it. This applies to
 * all BUM traffic and disabling it applies to both the transmit and
 * receive direction.
 */
void zebra_vxlan_flood_control(ZAPI_HANDLER_ARGS)
{
	struct stream *s;
	enum vxlan_flood_control flood_ctrl;

	if (!EVPN_ENABLED(zvrf)) {
		zlog_err("EVPN flood control for non-EVPN VRF %u",
			 zvrf_id(zvrf));
		return;
	}

	s = msg;
	STREAM_GETC(s, flood_ctrl);

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("EVPN flood control %u, currently %u",
			   flood_ctrl, zvrf->vxlan_flood_ctrl);

	if (zvrf->vxlan_flood_ctrl == flood_ctrl)
		return;

	zvrf->vxlan_flood_ctrl = flood_ctrl;

	/* Install or uninstall flood entries corresponding to
	 * remote VTEPs.
	 */
	hash_iterate(zvrf->vni_table, zvni_handle_flooding_remote_vteps,
		     zvrf);

stream_failure:
	return;
}

/*
 * Handle message from client to enable/disable advertisement of svi macip
 * routes
 */
void zebra_vxlan_advertise_svi_macip(ZAPI_HANDLER_ARGS)
{
	struct stream *s;
	int advertise;
	vni_t vni = 0;
	zebra_vni_t *zvni = NULL;
	struct interface *ifp = NULL;

	if (!EVPN_ENABLED(zvrf)) {
		zlog_debug("EVPN SVI-MACIP Adv for non-EVPN VRF %u",
			  zvrf_id(zvrf));
		return;
	}

	s = msg;
	STREAM_GETC(s, advertise);
	STREAM_GETL(s, vni);

	if (!vni) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("EVPN SVI-MACIP Adv %s, currently %s",
				   advertise ? "enabled" : "disabled",
				   advertise_svi_macip_enabled(NULL)
					   ? "enabled"
					   : "disabled");

		if (zvrf->advertise_svi_macip == advertise)
			return;


		if (advertise) {
			zvrf->advertise_svi_macip = advertise;
			hash_iterate(zvrf->vni_table,
				     zvni_gw_macip_add_for_vni_hash, NULL);
		} else {
			hash_iterate(zvrf->vni_table,
				     zvni_svi_macip_del_for_vni_hash, NULL);
			zvrf->advertise_svi_macip = advertise;
		}

	} else {
		struct zebra_if *zif = NULL;
		struct zebra_l2info_vxlan zl2_info;
		struct interface *vlan_if = NULL;

		zvni = zvni_lookup(vni);
		if (!zvni)
			return;

		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"EVPN SVI macip Adv %s on VNI %d , currently %s",
				advertise ? "enabled" : "disabled", vni,
				advertise_svi_macip_enabled(zvni)
					? "enabled"
					: "disabled");

		if (zvni->advertise_svi_macip == advertise)
			return;

		/* Store flag even though SVI is not present.
		 * Once SVI comes up triggers self MAC-IP route add.
		 */
		zvni->advertise_svi_macip = advertise;

		ifp = zvni->vxlan_if;
		if (!ifp)
			return;

		zif = ifp->info;

		/* If down or not mapped to a bridge, we're done. */
		if (!if_is_operative(ifp) || !zif->brslave_info.br_if)
			return;

		zl2_info = zif->l2info.vxl;
		vlan_if = zvni_map_to_svi(zl2_info.access_vlan,
					  zif->brslave_info.br_if);
		if (!vlan_if)
			return;

		if (advertise) {
			/* Add primary SVI MAC-IP */
			zvni_add_macip_for_intf(vlan_if, zvni);
		} else {
			/* Del primary SVI MAC-IP */
			zvni_del_macip_for_intf(vlan_if, zvni);
		}
	}

stream_failure:
	return;
}

/*
 * Handle message from client to enable/disable advertisement of g/w macip
 * routes
 */
void zebra_vxlan_advertise_subnet(ZAPI_HANDLER_ARGS)
{
	struct stream *s;
	int advertise;
	vni_t vni = 0;
	zebra_vni_t *zvni = NULL;
	struct interface *ifp = NULL;
	struct zebra_if *zif = NULL;
	struct zebra_l2info_vxlan zl2_info;
	struct interface *vlan_if = NULL;

	if (!EVPN_ENABLED(zvrf)) {
		zlog_debug("EVPN GW-MACIP Adv for non-EVPN VRF %u",
			  zvrf_id(zvrf));
		return;
	}

	s = msg;
	STREAM_GETC(s, advertise);
	STREAM_GET(&vni, s, 3);

	zvni = zvni_lookup(vni);
	if (!zvni)
		return;

	if (zvni->advertise_subnet == advertise)
		return;

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("EVPN subnet Adv %s on VNI %d , currently %s",
			   advertise ? "enabled" : "disabled", vni,
			   zvni->advertise_subnet ? "enabled" : "disabled");


	zvni->advertise_subnet = advertise;

	ifp = zvni->vxlan_if;
	if (!ifp)
		return;

	zif = ifp->info;

	/* If down or not mapped to a bridge, we're done. */
	if (!if_is_operative(ifp) || !zif->brslave_info.br_if)
		return;

	zl2_info = zif->l2info.vxl;

	vlan_if =
		zvni_map_to_svi(zl2_info.access_vlan, zif->brslave_info.br_if);
	if (!vlan_if)
		return;

	if (zvni->advertise_subnet)
		zvni_advertise_subnet(zvni, vlan_if, 1);
	else
		zvni_advertise_subnet(zvni, vlan_if, 0);

stream_failure:
	return;
}

/*
 * Handle message from client to enable/disable advertisement of g/w macip
 * routes
 */
void zebra_vxlan_advertise_gw_macip(ZAPI_HANDLER_ARGS)
{
	struct stream *s;
	int advertise;
	vni_t vni = 0;
	zebra_vni_t *zvni = NULL;
	struct interface *ifp = NULL;

	if (!EVPN_ENABLED(zvrf)) {
		zlog_debug("EVPN GW-MACIP Adv for non-EVPN VRF %u",
			   zvrf_id(zvrf));
		return;
	}

	s = msg;
	STREAM_GETC(s, advertise);
	STREAM_GETL(s, vni);

	if (!vni) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("EVPN gateway macip Adv %s, currently %s",
				   advertise ? "enabled" : "disabled",
				   advertise_gw_macip_enabled(NULL)
					   ? "enabled"
					   : "disabled");

		if (zvrf->advertise_gw_macip == advertise)
			return;

		zvrf->advertise_gw_macip = advertise;

		if (advertise_gw_macip_enabled(zvni))
			hash_iterate(zvrf->vni_table,
				     zvni_gw_macip_add_for_vni_hash, NULL);
		else
			hash_iterate(zvrf->vni_table,
				     zvni_gw_macip_del_for_vni_hash, NULL);

	} else {
		struct zebra_if *zif = NULL;
		struct zebra_l2info_vxlan zl2_info;
		struct interface *vlan_if = NULL;
		struct interface *vrr_if = NULL;

		zvni = zvni_lookup(vni);
		if (!zvni)
			return;

		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug(
				"EVPN gateway macip Adv %s on VNI %d , currently %s",
				advertise ? "enabled" : "disabled", vni,
				advertise_gw_macip_enabled(zvni) ? "enabled"
								 : "disabled");

		if (zvni->advertise_gw_macip == advertise)
			return;

		zvni->advertise_gw_macip = advertise;

		ifp = zvni->vxlan_if;
		if (!ifp)
			return;

		zif = ifp->info;

		/* If down or not mapped to a bridge, we're done. */
		if (!if_is_operative(ifp) || !zif->brslave_info.br_if)
			return;

		zl2_info = zif->l2info.vxl;

		vlan_if = zvni_map_to_svi(zl2_info.access_vlan,
					  zif->brslave_info.br_if);
		if (!vlan_if)
			return;

		if (advertise_gw_macip_enabled(zvni)) {
			/* Add primary SVI MAC-IP */
			zvni_add_macip_for_intf(vlan_if, zvni);

			/* Add VRR MAC-IP - if any*/
			vrr_if = zebra_get_vrr_intf_for_svi(vlan_if);
			if (vrr_if)
				zvni_add_macip_for_intf(vrr_if, zvni);
		} else {
			/* Del primary MAC-IP */
			zvni_del_macip_for_intf(vlan_if, zvni);

			/* Del VRR MAC-IP - if any*/
			vrr_if = zebra_get_vrr_intf_for_svi(vlan_if);
			if (vrr_if)
				zvni_del_macip_for_intf(vrr_if, zvni);
		}
	}

stream_failure:
	return;
}


/*
 * Handle message from client to learn (or stop learning) about VNIs and MACs.
 * When enabled, the VNI hash table will be built and MAC FDB table read;
 * when disabled, the entries should be deleted and remote VTEPs and MACs
 * uninstalled from the kernel.
 * This also informs the setting for BUM handling at the time this change
 * occurs; it is relevant only when specifying "learn".
 */
void zebra_vxlan_advertise_all_vni(ZAPI_HANDLER_ARGS)
{
	struct stream *s = NULL;
	int advertise = 0;
	enum vxlan_flood_control flood_ctrl;

	/* Mismatch between EVPN VRF and current VRF (should be prevented by
	 * bgpd's cli) */
	if (is_evpn_enabled() && !EVPN_ENABLED(zvrf))
		return;

	s = msg;
	STREAM_GETC(s, advertise);
	STREAM_GETC(s, flood_ctrl);

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("EVPN VRF %s(%u) VNI Adv %s, currently %s, flood control %u",
			   zvrf_name(zvrf), zvrf_id(zvrf),
			   advertise ? "enabled" : "disabled",
			   is_evpn_enabled() ? "enabled" : "disabled",
			   flood_ctrl);

	if (zvrf->advertise_all_vni == advertise)
		return;

	zvrf->advertise_all_vni = advertise;
	if (EVPN_ENABLED(zvrf)) {
		zrouter.evpn_vrf = zvrf;

		/* Note BUM handling */
		zvrf->vxlan_flood_ctrl = flood_ctrl;

		/* Replay all ESs */
		zebra_evpn_es_send_all_to_client(true /* add */);

		/* Build VNI hash table and inform BGP. */
		zvni_build_hash_table();

		/* Add all SVI (L3 GW) MACs to BGP*/
		hash_iterate(zvrf->vni_table, zvni_gw_macip_add_for_vni_hash,
			     NULL);

		/* Read the MAC FDB */
		macfdb_read(zvrf->zns);

		/* Read neighbors */
		neigh_read(zvrf->zns);
	} else {
		/* Cleanup VTEPs for all VNIs - uninstall from
		 * kernel and free entries.
		 */
		hash_iterate(zvrf->vni_table, zvni_cleanup_all, zvrf);

		/* Delete all ESs in BGP */
		zebra_evpn_es_send_all_to_client(false /* add */);

		/* cleanup all l3vnis */
		hash_iterate(zrouter.l3vni_table, zl3vni_cleanup_all, NULL);

		/* Mark as "no EVPN VRF" */
		zrouter.evpn_vrf = NULL;
	}

stream_failure:
	return;
}

/*
 * Allocate VNI hash table for this VRF and do other initialization.
 * NOTE: Currently supported only for default VRF.
 */
void zebra_vxlan_init_tables(struct zebra_vrf *zvrf)
{
	if (!zvrf)
		return;
	zvrf->vni_table = hash_create(vni_hash_keymake, vni_hash_cmp,
				      "Zebra VRF VNI Table");
	zvrf->vxlan_sg_table = hash_create(zebra_vxlan_sg_hash_key_make,
			zebra_vxlan_sg_hash_eq, "Zebra VxLAN SG Table");
}

/* Cleanup VNI info, but don't free the table. */
void zebra_vxlan_cleanup_tables(struct zebra_vrf *zvrf)
{
	struct zebra_vrf *evpn_zvrf = zebra_vrf_get_evpn();

	if (!zvrf)
		return;
	hash_iterate(zvrf->vni_table, zvni_cleanup_all, zvrf);
	hash_iterate(zvrf->vxlan_sg_table, zebra_vxlan_sg_cleanup, NULL);

	if (zvrf == evpn_zvrf)
		zebra_evpn_es_cleanup();
}

/* Close all VNI handling */
void zebra_vxlan_close_tables(struct zebra_vrf *zvrf)
{
	if (!zvrf)
		return;
	hash_iterate(zvrf->vni_table, zvni_cleanup_all, zvrf);
	hash_free(zvrf->vni_table);
}

/* init the l3vni table */
void zebra_vxlan_init(void)
{
	zrouter.l3vni_table = hash_create(l3vni_hash_keymake, l3vni_hash_cmp,
					  "Zebra VRF L3 VNI table");
	zrouter.evpn_vrf = NULL;
	zebra_evpn_mh_init();
}

/* free l3vni table */
void zebra_vxlan_disable(void)
{
	hash_free(zrouter.l3vni_table);
	zebra_evpn_mh_terminate();
}

/* get the l3vni svi ifindex */
ifindex_t get_l3vni_svi_ifindex(vrf_id_t vrf_id)
{
	zebra_l3vni_t *zl3vni = NULL;

	zl3vni = zl3vni_from_vrf(vrf_id);
	if (!zl3vni || !is_l3vni_oper_up(zl3vni))
		return 0;

	return zl3vni->svi_if->ifindex;
}

static int zebra_vxlan_dad_ip_auto_recovery_exp(struct thread *t)
{
	struct zebra_vrf *zvrf = NULL;
	zebra_neigh_t *nbr = NULL;
	zebra_vni_t *zvni = NULL;
	char buf1[INET6_ADDRSTRLEN];
	char buf2[ETHER_ADDR_STRLEN];

	nbr = THREAD_ARG(t);

	/* since this is asynchronous we need sanity checks*/
	zvrf = vrf_info_lookup(nbr->zvni->vrf_id);
	if (!zvrf)
		return 0;

	zvni = zvni_lookup(nbr->zvni->vni);
	if (!zvni)
		return 0;

	nbr = zvni_neigh_lookup(zvni, &nbr->ip);
	if (!nbr)
		return 0;

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug(
			"%s: duplicate addr MAC %s IP %s flags 0x%x learn count %u vni %u auto recovery expired",
			__func__,
			prefix_mac2str(&nbr->emac, buf2, sizeof(buf2)),
			ipaddr2str(&nbr->ip, buf1, sizeof(buf1)), nbr->flags,
			nbr->dad_count, zvni->vni);

	UNSET_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE);
	nbr->dad_count = 0;
	nbr->detect_start_time.tv_sec = 0;
	nbr->detect_start_time.tv_usec = 0;
	nbr->dad_dup_detect_time = 0;
	nbr->dad_ip_auto_recovery_timer = NULL;
	ZEBRA_NEIGH_SET_ACTIVE(nbr);

	/* Send to BGP */
	if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_LOCAL)) {
		zvni_neigh_send_add_to_client(zvni->vni, &nbr->ip, &nbr->emac,
				nbr->mac, nbr->flags, nbr->loc_seq);
	} else if (!!CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_REMOTE)) {
		zvni_rem_neigh_install(zvni, nbr, false /*was_static*/);
	}

	return 0;
}

static int zebra_vxlan_dad_mac_auto_recovery_exp(struct thread *t)
{
	struct zebra_vrf *zvrf = NULL;
	zebra_mac_t *mac = NULL;
	zebra_vni_t *zvni = NULL;
	struct listnode *node = NULL;
	zebra_neigh_t *nbr = NULL;
	char buf[ETHER_ADDR_STRLEN];

	mac = THREAD_ARG(t);

	/* since this is asynchronous we need sanity checks*/
	zvrf = vrf_info_lookup(mac->zvni->vrf_id);
	if (!zvrf)
		return 0;

	zvni = zvni_lookup(mac->zvni->vni);
	if (!zvni)
		return 0;

	mac = zvni_mac_lookup(zvni, &mac->macaddr);
	if (!mac)
		return 0;

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug(
			"%s: duplicate addr mac %s flags 0x%x learn count %u host count %u auto recovery expired",
			__func__,
			prefix_mac2str(&mac->macaddr, buf, sizeof(buf)),
			mac->flags, mac->dad_count, listcount(mac->neigh_list));

	/* Remove all IPs as duplicate associcated with this MAC */
	for (ALL_LIST_ELEMENTS_RO(mac->neigh_list, node, nbr)) {
		if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE)) {
			if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_LOCAL))
				ZEBRA_NEIGH_SET_INACTIVE(nbr);
			else if (CHECK_FLAG(nbr->flags, ZEBRA_NEIGH_REMOTE))
				zvni_rem_neigh_install(zvni, nbr,
					false /*was_static*/);
		}

		UNSET_FLAG(nbr->flags, ZEBRA_NEIGH_DUPLICATE);
		nbr->dad_count = 0;
		nbr->detect_start_time.tv_sec = 0;
		nbr->dad_dup_detect_time = 0;
	}

	UNSET_FLAG(mac->flags, ZEBRA_MAC_DUPLICATE);
	mac->dad_count = 0;
	mac->detect_start_time.tv_sec = 0;
	mac->detect_start_time.tv_usec = 0;
	mac->dad_dup_detect_time = 0;
	mac->dad_mac_auto_recovery_timer = NULL;

	if (CHECK_FLAG(mac->flags, ZEBRA_MAC_LOCAL)) {
		/* Inform to BGP */
		if (zvni_mac_send_add_to_client(zvni->vni, &mac->macaddr,
					mac->flags, mac->loc_seq, mac->es))
			return -1;

		/* Process all neighbors associated with this MAC. */
		zvni_process_neigh_on_local_mac_change(zvni, mac, 0,
			0 /*es_change*/);

	} else if (CHECK_FLAG(mac->flags, ZEBRA_MAC_REMOTE)) {
		zvni_process_neigh_on_remote_mac_add(zvni, mac);

		/* Install the entry. */
		zvni_rem_mac_install(zvni, mac, false /* was_static */);
	}

	return 0;
}

/************************** vxlan SG cache management ************************/
/* Inform PIM about the mcast group */
static int zebra_vxlan_sg_send(struct zebra_vrf *zvrf,
		struct prefix_sg *sg,
		char *sg_str, uint16_t cmd)
{
	struct zserv *client = NULL;
	struct stream *s = NULL;

	client = zserv_find_client(ZEBRA_ROUTE_PIM, 0);
	if (!client)
		return 0;

	if (!CHECK_FLAG(zvrf->flags, ZEBRA_PIM_SEND_VXLAN_SG))
		return 0;

	s = stream_new(ZEBRA_MAX_PACKET_SIZ);

	zclient_create_header(s, cmd, VRF_DEFAULT);
	stream_putl(s, IPV4_MAX_BYTELEN);
	stream_put(s, &sg->src.s_addr, IPV4_MAX_BYTELEN);
	stream_put(s, &sg->grp.s_addr, IPV4_MAX_BYTELEN);

	/* Write packet size. */
	stream_putw_at(s, 0, stream_get_endp(s));

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug(
			"Send %s %s to %s",
			(cmd == ZEBRA_VXLAN_SG_ADD) ? "add" : "del", sg_str,
			zebra_route_string(client->proto));

	if (cmd == ZEBRA_VXLAN_SG_ADD)
		client->vxlan_sg_add_cnt++;
	else
		client->vxlan_sg_del_cnt++;

	return zserv_send_message(client, s);
}

static unsigned int zebra_vxlan_sg_hash_key_make(const void *p)
{
	const zebra_vxlan_sg_t *vxlan_sg = p;

	return (jhash_2words(vxlan_sg->sg.src.s_addr,
				vxlan_sg->sg.grp.s_addr, 0));
}

static bool zebra_vxlan_sg_hash_eq(const void *p1, const void *p2)
{
	const zebra_vxlan_sg_t *sg1 = p1;
	const zebra_vxlan_sg_t *sg2 = p2;

	return ((sg1->sg.src.s_addr == sg2->sg.src.s_addr)
		&& (sg1->sg.grp.s_addr == sg2->sg.grp.s_addr));
}

static zebra_vxlan_sg_t *zebra_vxlan_sg_new(struct zebra_vrf *zvrf,
		struct prefix_sg *sg)
{
	zebra_vxlan_sg_t *vxlan_sg;

	vxlan_sg = XCALLOC(MTYPE_ZVXLAN_SG, sizeof(*vxlan_sg));

	vxlan_sg->zvrf = zvrf;
	vxlan_sg->sg = *sg;
	prefix_sg2str(sg, vxlan_sg->sg_str);

	vxlan_sg = hash_get(zvrf->vxlan_sg_table, vxlan_sg, hash_alloc_intern);

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("vxlan SG %s created", vxlan_sg->sg_str);

	return vxlan_sg;
}

static zebra_vxlan_sg_t *zebra_vxlan_sg_find(struct zebra_vrf *zvrf,
					    struct prefix_sg *sg)
{
	zebra_vxlan_sg_t lookup;

	lookup.sg = *sg;
	return hash_lookup(zvrf->vxlan_sg_table, &lookup);
}

static zebra_vxlan_sg_t *zebra_vxlan_sg_add(struct zebra_vrf *zvrf,
					   struct prefix_sg *sg)
{
	zebra_vxlan_sg_t *vxlan_sg;
	zebra_vxlan_sg_t *parent = NULL;
	struct in_addr sip;

	vxlan_sg = zebra_vxlan_sg_find(zvrf, sg);
	if (vxlan_sg)
		return vxlan_sg;

	/* create a *G entry for every BUM group implicitly -
	 * 1. The SG entry is used by pimd to setup the vxlan-origination-mroute
	 * 2. the XG entry is used by pimd to setup the
	 * vxlan-termination-mroute
	 */
	if (sg->src.s_addr != INADDR_ANY) {
		memset(&sip, 0, sizeof(sip));
		parent = zebra_vxlan_sg_do_ref(zvrf, sip, sg->grp);
		if (!parent)
			return NULL;
	}

	vxlan_sg = zebra_vxlan_sg_new(zvrf, sg);
	if (!vxlan_sg) {
		if (parent)
			zebra_vxlan_sg_do_deref(zvrf, sip, sg->grp);
		return vxlan_sg;
	}

	zebra_vxlan_sg_send(zvrf, sg, vxlan_sg->sg_str,
			ZEBRA_VXLAN_SG_ADD);

	return vxlan_sg;
}

static void zebra_vxlan_sg_del(zebra_vxlan_sg_t *vxlan_sg)
{
	struct in_addr sip;
	struct zebra_vrf *zvrf;

	zvrf = vrf_info_lookup(VRF_DEFAULT);
	if (!zvrf)
		return;

	/* On SG entry deletion remove the reference to its parent XG
	 * entry
	 */
	if (vxlan_sg->sg.src.s_addr != INADDR_ANY) {
		memset(&sip, 0, sizeof(sip));
		zebra_vxlan_sg_do_deref(zvrf, sip, vxlan_sg->sg.grp);
	}

	zebra_vxlan_sg_send(zvrf, &vxlan_sg->sg,
			vxlan_sg->sg_str, ZEBRA_VXLAN_SG_DEL);

	hash_release(vxlan_sg->zvrf->vxlan_sg_table, vxlan_sg);

	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("VXLAN SG %s deleted", vxlan_sg->sg_str);

	XFREE(MTYPE_ZVXLAN_SG, vxlan_sg);
}

static void zebra_vxlan_sg_do_deref(struct zebra_vrf *zvrf,
		struct in_addr sip, struct in_addr mcast_grp)
{
	zebra_vxlan_sg_t *vxlan_sg;
	struct prefix_sg sg;

	sg.family = AF_INET;
	sg.prefixlen = IPV4_MAX_BYTELEN;
	sg.src = sip;
	sg.grp = mcast_grp;
	vxlan_sg = zebra_vxlan_sg_find(zvrf, &sg);
	if (!vxlan_sg)
		return;

	if (vxlan_sg->ref_cnt)
		--vxlan_sg->ref_cnt;

	if (!vxlan_sg->ref_cnt)
		zebra_vxlan_sg_del(vxlan_sg);
}

static zebra_vxlan_sg_t *zebra_vxlan_sg_do_ref(struct zebra_vrf *zvrf,
				struct in_addr sip, struct in_addr mcast_grp)
{
	zebra_vxlan_sg_t *vxlan_sg;
	struct prefix_sg sg;

	sg.family = AF_INET;
	sg.prefixlen = IPV4_MAX_BYTELEN;
	sg.src = sip;
	sg.grp = mcast_grp;
	vxlan_sg = zebra_vxlan_sg_add(zvrf, &sg);
	if (vxlan_sg)
		++vxlan_sg->ref_cnt;

	return vxlan_sg;
}

static void zebra_vxlan_sg_deref(struct in_addr local_vtep_ip,
		struct in_addr mcast_grp)
{
	struct zebra_vrf *zvrf;

	if (local_vtep_ip.s_addr == INADDR_ANY
	    || mcast_grp.s_addr == INADDR_ANY)
		return;

	zvrf = vrf_info_lookup(VRF_DEFAULT);
	if (!zvrf)
		return;

	zebra_vxlan_sg_do_deref(zvrf, local_vtep_ip, mcast_grp);
}

static void zebra_vxlan_sg_ref(struct in_addr local_vtep_ip,
				struct in_addr mcast_grp)
{
	struct zebra_vrf *zvrf;

	if (local_vtep_ip.s_addr == INADDR_ANY
	    || mcast_grp.s_addr == INADDR_ANY)
		return;

	zvrf = vrf_info_lookup(VRF_DEFAULT);
	if (!zvrf)
		return;
	zebra_vxlan_sg_do_ref(zvrf, local_vtep_ip, mcast_grp);
}

static void zebra_vxlan_sg_cleanup(struct hash_bucket *backet, void *arg)
{
	zebra_vxlan_sg_t *vxlan_sg = (zebra_vxlan_sg_t *)backet->data;

	zebra_vxlan_sg_del(vxlan_sg);
}

static void zebra_vxlan_sg_replay_send(struct hash_bucket *backet, void *arg)
{
	zebra_vxlan_sg_t *vxlan_sg = (zebra_vxlan_sg_t *)backet->data;

	zebra_vxlan_sg_send(vxlan_sg->zvrf, &vxlan_sg->sg,
			vxlan_sg->sg_str, ZEBRA_VXLAN_SG_ADD);
}

/* Handle message from client to replay vxlan SG entries */
void zebra_vxlan_sg_replay(ZAPI_HANDLER_ARGS)
{
	if (IS_ZEBRA_DEBUG_VXLAN)
		zlog_debug("VxLAN SG updates to PIM, start");

	SET_FLAG(zvrf->flags, ZEBRA_PIM_SEND_VXLAN_SG);

	if (!EVPN_ENABLED(zvrf)) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("VxLAN SG replay request on unexpected vrf %d",
				   zvrf->vrf->vrf_id);
		return;
	}

	hash_iterate(zvrf->vxlan_sg_table, zebra_vxlan_sg_replay_send, NULL);
}

/************************** EVPN BGP config management ************************/
/* Notify Local MACs to the clienti, skips GW MAC */
static void zvni_send_mac_hash_entry_to_client(struct hash_bucket *bucket,
					       void *arg)
{
	struct mac_walk_ctx *wctx = arg;
	zebra_mac_t *zmac = bucket->data;

	if (CHECK_FLAG(zmac->flags, ZEBRA_MAC_DEF_GW))
		return;

	if (CHECK_FLAG(zmac->flags, ZEBRA_MAC_LOCAL))
		zvni_mac_send_add_to_client(wctx->zvni->vni, &zmac->macaddr,
				zmac->flags, zmac->loc_seq, zmac->es);
}

/* Iterator to Notify Local MACs of a L2VNI */
static void zvni_send_mac_to_client(zebra_vni_t *zvni)
{
	struct mac_walk_ctx wctx;

	if (!zvni->mac_table)
		return;

	memset(&wctx, 0, sizeof(struct mac_walk_ctx));
	wctx.zvni = zvni;

	hash_iterate(zvni->mac_table, zvni_send_mac_hash_entry_to_client,
			&wctx);
}

/* Notify Neighbor entries to the Client, skips the GW entry */
static void zvni_send_neigh_hash_entry_to_client(struct hash_bucket *bucket,
						 void *arg)
{
	struct mac_walk_ctx *wctx = arg;
	zebra_neigh_t *zn = bucket->data;
	zebra_mac_t *zmac = NULL;

	if (CHECK_FLAG(zn->flags, ZEBRA_NEIGH_DEF_GW))
		return;

	if (CHECK_FLAG(zn->flags, ZEBRA_NEIGH_LOCAL) &&
		IS_ZEBRA_NEIGH_ACTIVE(zn)) {
		zmac = zvni_mac_lookup(wctx->zvni, &zn->emac);
		if (!zmac)
			return;

		zvni_neigh_send_add_to_client(wctx->zvni->vni, &zn->ip,
						&zn->emac, zn->mac, zn->flags,
						zn->loc_seq);
	}
}

/* Iterator of a specific L2VNI */
static void zvni_send_neigh_to_client(zebra_vni_t *zvni)
{
	struct neigh_walk_ctx wctx;

	memset(&wctx, 0, sizeof(struct neigh_walk_ctx));
	wctx.zvni = zvni;

	hash_iterate(zvni->neigh_table, zvni_send_neigh_hash_entry_to_client,
			&wctx);
}

static void zvni_evpn_cfg_cleanup(struct hash_bucket *bucket, void *ctxt)
{
	zebra_vni_t *zvni = NULL;

	zvni = (zebra_vni_t *)bucket->data;
	zvni->advertise_gw_macip = 0;
	zvni->advertise_svi_macip = 0;
	zvni->advertise_subnet = 0;

	zvni_neigh_del_all(zvni, 1, 0,
			   DEL_REMOTE_NEIGH | DEL_REMOTE_NEIGH_FROM_VTEP);
	zvni_mac_del_all(zvni, 1, 0,
			 DEL_REMOTE_MAC | DEL_REMOTE_MAC_FROM_VTEP);
	zvni_vtep_del_all(zvni, 1);
}

/* Cleanup EVPN configuration of a specific VRF */
static void zebra_evpn_vrf_cfg_cleanup(struct zebra_vrf *zvrf)
{
	zebra_l3vni_t *zl3vni = NULL;

	zvrf->advertise_all_vni = 0;
	zvrf->advertise_gw_macip = 0;
	zvrf->advertise_svi_macip = 0;
	zvrf->vxlan_flood_ctrl = VXLAN_FLOOD_HEAD_END_REPL;

	hash_iterate(zvrf->vni_table, zvni_evpn_cfg_cleanup, NULL);

	if (zvrf->l3vni)
		zl3vni = zl3vni_lookup(zvrf->l3vni);
	if (zl3vni) {
		/* delete and uninstall all rmacs */
		hash_iterate(zl3vni->rmac_table, zl3vni_del_rmac_hash_entry,
			     zl3vni);
		/* delete and uninstall all next-hops */
		hash_iterate(zl3vni->nh_table, zl3vni_del_nh_hash_entry,
			     zl3vni);
	}
}

/* Cleanup BGP EVPN configuration upon client disconnect */
static int zebra_evpn_bgp_cfg_clean_up(struct zserv *client)
{
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

	RB_FOREACH (vrf, vrf_id_head, &vrfs_by_id) {
		zvrf = vrf->info;
		if (zvrf)
			zebra_evpn_vrf_cfg_cleanup(zvrf);
	}

	return 0;
}

static int zebra_evpn_pim_cfg_clean_up(struct zserv *client)
{
	struct zebra_vrf *zvrf = zebra_vrf_get_evpn();

	if (zvrf && CHECK_FLAG(zvrf->flags, ZEBRA_PIM_SEND_VXLAN_SG)) {
		if (IS_ZEBRA_DEBUG_VXLAN)
			zlog_debug("VxLAN SG updates to PIM, stop");
		UNSET_FLAG(zvrf->flags, ZEBRA_PIM_SEND_VXLAN_SG);
	}

	return 0;
}

static int zebra_evpn_cfg_clean_up(struct zserv *client)
{
	if (client->proto == ZEBRA_ROUTE_BGP)
		return zebra_evpn_bgp_cfg_clean_up(client);

	if (client->proto == ZEBRA_ROUTE_PIM)
		return zebra_evpn_pim_cfg_clean_up(client);

	return 0;
}

/*
 * Handle results for vxlan dataplane operations.
 */
extern void zebra_vxlan_handle_result(struct zebra_dplane_ctx *ctx)
{
	/* TODO -- anything other than freeing the context? */
	dplane_ctx_fini(&ctx);
}

/* Cleanup BGP EVPN configuration upon client disconnect */
extern void zebra_evpn_init(void)
{
	hook_register(zserv_client_close, zebra_evpn_cfg_clean_up);
}
