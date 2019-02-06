/*
 * VRRP CLI commands.
 * Copyright (C) 2018-2019 Cumulus Networks, Inc.
 * Quentin Young
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <zebra.h>

#include "lib/command.h"
#include "lib/if.h"
#include "lib/ipaddr.h"
#include "lib/prefix.h"
#include "lib/termtable.h"
#include "lib/vty.h"

#include "vrrp.h"
#include "vrrp_vty.h"
#include "vrrp_memory.h"
#ifndef VTYSH_EXTRACT_PL
#include "vrrpd/vrrp_vty_clippy.c"
#endif


#define VRRP_STR "Virtual Router Redundancy Protocol\n"
#define VRRP_VRID_STR "Virtual Router ID\n"
#define VRRP_PRIORITY_STR "Virtual Router Priority\n"
#define VRRP_ADVINT_STR "Virtual Router Advertisement Interval\n"
#define VRRP_IP_STR "Virtual Router IPv4 address\n"
#define VRRP_VERSION_STR "VRRP protocol version\n"

#define VROUTER_GET_VTY(_vty, _ifp, _vrid, _vr)                                \
	do {                                                                   \
		_vr = vrrp_lookup(_ifp, _vrid);                                \
		if (!_vr) {                                                    \
			vty_out(_vty,                                          \
				"%% Please configure VRRP instance %u\n",      \
				(unsigned int)_vrid);                          \
			return CMD_WARNING_CONFIG_FAILED;                      \
		}                                                              \
	} while (0);

DEFUN_NOSH (show_debugging_vrrpd,
	    show_debugging_vrrpd_cmd,
	    "show debugging [vrrp]",
	    SHOW_STR
	    DEBUG_STR
	    "VRRP information\n")
{
	vty_out(vty, "VRRP debugging status\n");

	return CMD_SUCCESS;
}

DEFPY(vrrp_vrid,
      vrrp_vrid_cmd,
      "[no] vrrp (1-255)$vrid [version (2-3)]",
      NO_STR
      VRRP_STR
      VRRP_VRID_STR
      VRRP_VERSION_STR
      VRRP_VERSION_STR)
{
	VTY_DECLVAR_CONTEXT(interface, ifp);

	struct vrrp_vrouter *vr = vrrp_lookup(ifp, vrid);

	if (version == 0)
		version = 3;

	if (no && vr)
		vrrp_vrouter_destroy(vr);
	else if (no && !vr)
		vty_out(vty, "%% VRRP instance %ld does not exist on %s\n",
			vrid, ifp->name);
	else if (!vr)
		vrrp_vrouter_create(ifp, vrid, version);
	else if (vr)
		vty_out(vty, "%% VRRP instance %ld already exists on %s\n",
			vrid, ifp->name);

	return CMD_SUCCESS;
}

DEFPY(vrrp_priority,
      vrrp_priority_cmd,
      "[no] vrrp (1-255)$vrid priority (1-254)",
      NO_STR
      VRRP_STR
      VRRP_VRID_STR
      VRRP_PRIORITY_STR
      "Priority value")
{
	VTY_DECLVAR_CONTEXT(interface, ifp);

	struct vrrp_vrouter *vr;
	struct vrrp_router *r;
	bool nr[2] = { false, false };
	int ret = CMD_SUCCESS;
	uint8_t newprio = no ? VRRP_DEFAULT_PRIORITY : priority;

	VROUTER_GET_VTY(vty, ifp, vrid, vr);

	r = vr->v4;
	for (int i = 0; i < 2; i++) {
		nr[i] = r->is_active && r->fsm.state != VRRP_STATE_INITIALIZE
			&& vr->priority != newprio;
		if (nr[i]) {
			vty_out(vty,
				"%% WARNING: Restarting %s Virtual Router %ld to update priority\n",
				family2str(r->family), vrid);
			(void)vrrp_event(r, VRRP_EVENT_SHUTDOWN);
		}
		r = vr->v6;
	}

	vrrp_set_priority(vr, newprio);

	r = vr->v4;
	for (int i = 0; i < 2; i++) {
		if (nr[i]) {
			ret = vrrp_event(r, VRRP_EVENT_STARTUP);
			if (ret < 0)
				vty_out(vty,
					"%% Failed to start Virtual Router %ld (%s)\n",
					vrid, family2str(r->family));
		}
		r = vr->v6;
	}

	return CMD_SUCCESS;
}

DEFPY(vrrp_advertisement_interval,
      vrrp_advertisement_interval_cmd,
      "[no] vrrp (1-255)$vrid advertisement-interval (1-4096)",
      NO_STR VRRP_STR VRRP_VRID_STR VRRP_ADVINT_STR
      "Advertisement interval in centiseconds")
{
	VTY_DECLVAR_CONTEXT(interface, ifp);

	struct vrrp_vrouter *vr;
	uint16_t newadvint = no ? VRRP_DEFAULT_ADVINT : advertisement_interval;

	VROUTER_GET_VTY(vty, ifp, vrid, vr);
	vrrp_set_advertisement_interval(vr, newadvint);

	return CMD_SUCCESS;
}

DEFPY(vrrp_ip,
      vrrp_ip_cmd,
      "[no] vrrp (1-255)$vrid ip A.B.C.D",
      NO_STR
      VRRP_STR
      VRRP_VRID_STR
      "Add IPv4 address\n"
      VRRP_IP_STR)
{
	VTY_DECLVAR_CONTEXT(interface, ifp);

	struct vrrp_vrouter *vr;
	bool deactivated = false;
	bool activated = false;
	bool failed = false;
	int ret = CMD_SUCCESS;

	VROUTER_GET_VTY(vty, ifp, vrid, vr);

	bool will_activate = (vr->v4->fsm.state == VRRP_STATE_INITIALIZE);

	if (no) {
		int oldstate = vr->v4->fsm.state;
		failed = vrrp_del_ipv4(vr, ip, true);
		deactivated = (vr->v4->fsm.state == VRRP_STATE_INITIALIZE
			       && oldstate != VRRP_STATE_INITIALIZE);
	} else {
		int oldstate = vr->v4->fsm.state;
		failed = vrrp_add_ipv4(vr, ip, true);
		activated = (vr->v4->fsm.state != VRRP_STATE_INITIALIZE
			     && oldstate == VRRP_STATE_INITIALIZE);
	}

	if (activated)
		vty_out(vty, "%% Activated IPv4 Virtual Router %ld\n", vrid);
	if (deactivated)
		vty_out(vty, "%% Deactivated IPv4 Virtual Router %ld\n", vrid);
	if (failed) {
		vty_out(vty, "%% Failed to %s virtual IP",
			no ? "remove" : "add");
		ret = CMD_WARNING_CONFIG_FAILED;
		if (will_activate && !activated) {
			vty_out(vty,
				"%% Failed to activate IPv4 Virtual Router %ld\n",
				vrid);
		}
	}

	return ret;
}

DEFPY(vrrp_ip6,
      vrrp_ip6_cmd,
      "[no] vrrp (1-255)$vrid ipv6 X:X::X:X",
      NO_STR
      VRRP_STR
      VRRP_VRID_STR
      "Add IPv6 address\n"
      VRRP_IP_STR)
{
	VTY_DECLVAR_CONTEXT(interface, ifp);

	struct vrrp_vrouter *vr;
	bool deactivated = false;
	bool activated = false;
	bool failed = false;
	int ret = CMD_SUCCESS;

	VROUTER_GET_VTY(vty, ifp, vrid, vr);

	if (vr->version != 3) {
		vty_out(vty,
			"%% Cannot add IPv6 address to VRRPv2 virtual router\n");
		return CMD_WARNING_CONFIG_FAILED;
	}

	bool will_activate = (vr->v6->fsm.state == VRRP_STATE_INITIALIZE);

	if (no) {
		int oldstate = vr->v6->fsm.state;
		failed = vrrp_del_ipv6(vr, ipv6, true);
		deactivated = (vr->v6->fsm.state == VRRP_STATE_INITIALIZE
			       && oldstate != VRRP_STATE_INITIALIZE);
	} else {
		int oldstate = vr->v6->fsm.state;
		failed = vrrp_add_ipv6(vr, ipv6, true);
		activated = (vr->v6->fsm.state != VRRP_STATE_INITIALIZE
			     && oldstate == VRRP_STATE_INITIALIZE);
	}

	if (activated)
		vty_out(vty, "%% Activated IPv6 Virtual Router %ld\n", vrid);
	if (deactivated)
		vty_out(vty, "%% Deactivated IPv6 Virtual Router %ld\n", vrid);
	if (failed) {
		vty_out(vty, "%% Failed to %s virtual IP",
			no ? "remove" : "add");
		ret = CMD_WARNING_CONFIG_FAILED;
		if (will_activate && !activated) {
			vty_out(vty,
				"%% Failed to activate IPv4 Virtual Router %ld\n",
				vrid);
		}
	}

	return ret;
}

DEFPY(vrrp_preempt,
      vrrp_preempt_cmd,
      "[no] vrrp (1-255)$vrid preempt",
      NO_STR
      VRRP_STR
      VRRP_VRID_STR
      "Preempt mode\n")
{
	VTY_DECLVAR_CONTEXT(interface, ifp);

	struct vrrp_vrouter *vr;

	VROUTER_GET_VTY(vty, ifp, vrid, vr);

	vr->preempt_mode = !no;

	return CMD_SUCCESS;
}

static void vrrp_show(struct vty *vty, struct vrrp_vrouter *vr)
{
	char ethstr4[ETHER_ADDR_STRLEN];
	char ethstr6[ETHER_ADDR_STRLEN];
	char ipstr[INET6_ADDRSTRLEN];
	const char *stastr4 = vrrp_state_names[vr->v4->fsm.state];
	const char *stastr6 = vrrp_state_names[vr->v6->fsm.state];
	struct listnode *ln;
	struct ipaddr *ip;

	struct ttable *tt = ttable_new(&ttable_styles[TTSTYLE_BLANK]);

	ttable_add_row(tt, "%s|%" PRIu32, "Virtual Router ID", vr->vrid);
	ttable_add_row(tt, "%s|%" PRIu8, "Protocol Version", vr->version);
	ttable_add_row(tt, "%s|%s", "Interface", vr->ifp->name);
	prefix_mac2str(&vr->v4->vmac, ethstr4, sizeof(ethstr4));
	prefix_mac2str(&vr->v6->vmac, ethstr6, sizeof(ethstr6));
	ttable_add_row(tt, "%s|%s", "VRRP interface (v4)",
		       vr->v4->mvl_ifp ? vr->v4->mvl_ifp->name : "None");
	ttable_add_row(tt, "%s|%s", "VRRP interface (v6)",
		       vr->v6->mvl_ifp ? vr->v6->mvl_ifp->name : "None");
	ttable_add_row(tt, "%s|%s", "Virtual MAC (v4)", ethstr4);
	ttable_add_row(tt, "%s|%s", "Virtual MAC (v6)", ethstr6);
	ttable_add_row(tt, "%s|%s", "Status (v4)", stastr4);
	ttable_add_row(tt, "%s|%s", "Status (v6)", stastr6);
	ttable_add_row(tt, "%s|%" PRIu8, "Priority", vr->priority);
	ttable_add_row(tt, "%s|%" PRIu8, "Effective Priority (v4)",
		       vr->v4->priority);
	ttable_add_row(tt, "%s|%" PRIu8, "Effective Priority (v6)",
		       vr->v6->priority);
	ttable_add_row(tt, "%s|%s", "Preempt Mode",
		       vr->preempt_mode ? "Yes" : "No");
	ttable_add_row(tt, "%s|%s", "Accept Mode",
		       vr->accept_mode ? "Yes" : "No");
	ttable_add_row(tt, "%s|%" PRIu16" cs", "Advertisement Interval",
		       vr->advertisement_interval);
	ttable_add_row(tt, "%s|%" PRIu16" cs", "Master Advertisement Interval (v4)",
		       vr->v4->master_adver_interval);
	ttable_add_row(tt, "%s|%" PRIu16" cs", "Master Advertisement Interval (v6)",
		       vr->v6->master_adver_interval);
	ttable_add_row(tt, "%s|%" PRIu16" cs", "Skew Time (v4)", vr->v4->skew_time);
	ttable_add_row(tt, "%s|%" PRIu16" cs", "Skew Time (v6)", vr->v6->skew_time);
	ttable_add_row(tt, "%s|%" PRIu16" cs", "Master Down Interval (v4)",
		       vr->v4->master_down_interval);
	ttable_add_row(tt, "%s|%" PRIu16" cs", "Master Down Interval (v6)",
		       vr->v6->master_down_interval);
	ttable_add_row(tt, "%s|%u", "IPv4 Addresses", vr->v4->addrs->count);

	char fill[37];
	memset(fill, '.', sizeof(fill));
	fill[sizeof(fill) - 1] = 0x00;
	if (vr->v4->addrs->count) {
		for (ALL_LIST_ELEMENTS_RO(vr->v4->addrs, ln, ip)) {
			inet_ntop(vr->v4->family, &ip->ipaddr_v4, ipstr,
				  sizeof(ipstr));
			ttable_add_row(tt, "%s|%s", fill, ipstr);
		}
	}

	ttable_add_row(tt, "%s|%u", "IPv6 Addresses", vr->v6->addrs->count);

	if (vr->v6->addrs->count) {
		for (ALL_LIST_ELEMENTS_RO(vr->v6->addrs, ln, ip)) {
			inet_ntop(vr->v6->family, &ip->ipaddr_v6, ipstr,
				  sizeof(ipstr));
			ttable_add_row(tt, "%s|%s", fill, ipstr);
		}
	}

	char *table = ttable_dump(tt, "\n");
	vty_out(vty, "\n%s\n", table);
	XFREE(MTYPE_TMP, table);
	ttable_del(tt);

}

DEFPY(vrrp_vrid_show,
      vrrp_vrid_show_cmd,
      "show vrrp [interface INTERFACE$ifn] [(1-255)$vrid]",
      SHOW_STR
      VRRP_STR
      INTERFACE_STR
      "Only show VRRP instances on this interface\n"
      VRRP_VRID_STR)
{
	struct vrrp_vrouter *vr;
	struct listnode *ln;
	struct list *ll = hash_to_list(vrrp_vrouters_hash);

	for (ALL_LIST_ELEMENTS_RO(ll, ln, vr)) {
		if (ifn && !strmatch(ifn, vr->ifp->name))
			continue;
		if (vrid && vrid != vr->vrid)
			continue;

		vrrp_show(vty, vr);
	}

	list_delete_and_null(&ll);

	return CMD_SUCCESS;
}

static struct cmd_node interface_node = {
	INTERFACE_NODE,
	"%s(config-if)# ", 1
};

void vrrp_vty_init(void)
{
	install_node(&interface_node, NULL);
	if_cmd_init();
	install_element(VIEW_NODE, &show_debugging_vrrpd_cmd);
	install_element(VIEW_NODE, &vrrp_vrid_show_cmd);
	install_element(INTERFACE_NODE, &vrrp_vrid_cmd);
	install_element(INTERFACE_NODE, &vrrp_priority_cmd);
	install_element(INTERFACE_NODE, &vrrp_advertisement_interval_cmd);
	install_element(INTERFACE_NODE, &vrrp_ip_cmd);
	install_element(INTERFACE_NODE, &vrrp_ip6_cmd);
	install_element(INTERFACE_NODE, &vrrp_preempt_cmd);
}
