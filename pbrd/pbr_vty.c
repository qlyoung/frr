/*
 * PBR - vty code
 * Copyright (C) Cumulus Networks, Inc.
 *               Donald Sharp
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
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <zebra.h>

#include "vty.h"
#include "command.h"
#include "prefix.h"
#include "vrf.h"
#include "nexthop.h"
#include "nexthop_group.h"
#include "log.h"
#include "json.h"
#include "debug.h"

#include "pbrd/pbr_nht.h"
#include "pbrd/pbr_map.h"
#include "pbrd/pbr_zebra.h"
#include "pbrd/pbr_vty.h"
#include "pbrd/pbr_event.h"
#include "pbrd/pbr_debug.h"
#ifndef VTYSH_EXTRACT_PL
#include "pbrd/pbr_vty_clippy.c"
#endif

DEFUN_NOSH(pbr_map, pbr_map_cmd, "pbr-map WORD seq (1-1000)",
	   "Create pbr-map or enter pbr-map command mode\n"
	   "The name of the PBR MAP\n"
	   "Sequence to insert in existing pbr-map entry\n"
	   "Sequence number\n")
{
	const char *pbrm_name = argv[1]->arg;
	uint32_t seqno = atoi(argv[3]->arg);
	struct pbr_map_sequence *pbrms;

	pbrms = pbrms_get(pbrm_name, seqno);
	VTY_PUSH_CONTEXT(PBRMAP_NODE, pbrms);

	return CMD_SUCCESS;
}

DEFUN_NOSH(no_pbr_map, no_pbr_map_cmd, "no pbr-map WORD [seq (1-65535)]",
	   NO_STR
	   "Delete pbr-map\n"
	   "The name of the PBR MAP\n"
	   "Sequence to delete from existing pbr-map entry\n"
	   "Sequence number\n")
{
	const char *pbrm_name = argv[2]->arg;
	uint32_t seqno = 0;
	struct pbr_map *pbrm = pbrm_find(pbrm_name);
	struct pbr_event *pbre;
	struct pbr_map_sequence *pbrms;
	struct listnode *node, *next_node;

	if (argc > 3)
		seqno = atoi(argv[4]->arg);

	if (!pbrm) {
		vty_out(vty, "pbr-map %s not found\n", pbrm_name);
		return CMD_SUCCESS;
	}

	if (seqno) {
		pbrms = pbrms_get(pbrm->name, seqno);
		pbrms->reason |= PBR_MAP_DEL_SEQUENCE_NUMBER;
	} else {
		for (ALL_LIST_ELEMENTS(pbrm->seqnumbers, node, next_node,
				       pbrms)) {
			if (pbrms)
				pbrms->reason |= PBR_MAP_DEL_SEQUENCE_NUMBER;
		}
	}

	pbre = pbr_event_new();
	pbre->event = PBR_MAP_DELETE;
	pbre->seqno = seqno;
	strlcpy(pbre->name, pbrm_name, sizeof(pbre->name));
	pbr_event_enqueue(pbre);

	return CMD_SUCCESS;
}

DEFPY(pbr_map_match_src, pbr_map_match_src_cmd,
	"[no] match src-ip <A.B.C.D/M|X:X::X:X/M>$prefix",
	NO_STR
	"Match the rest of the command\n"
	"Choose the src ip or ipv6 prefix to use\n"
	"v4 Prefix\n"
	"v6 Prefix\n")
{
	struct pbr_map_sequence *pbrms = VTY_GET_CONTEXT(pbr_map_sequence);
	struct pbr_event *pbre;

	if (!no) {
		if (!pbrms->src)
			pbrms->src = prefix_new();
		prefix_copy(pbrms->src, prefix);
	} else {
		prefix_free(pbrms->src);
		pbrms->src = 0;
	}

	pbre = pbr_event_new();
	pbre->event = PBR_MAP_MODIFY;
	pbre->seqno = pbrms->seqno;
	strlcpy(pbre->name, pbrms->parent->name, sizeof(pbre->name));
	pbr_event_enqueue(pbre);

	return CMD_SUCCESS;
}

DEFPY(pbr_map_match_dst, pbr_map_match_dst_cmd,
	"[no] match dst-ip <A.B.C.D/M|X:X::X:X/M>$prefix",
	NO_STR
	"Match the rest of the command\n"
	"Choose the src ip or ipv6 prefix to use\n"
	"v4 Prefix\n"
	"v6 Prefix\n")
{
	struct pbr_map_sequence *pbrms = VTY_GET_CONTEXT(pbr_map_sequence);
	struct pbr_event *pbre;

	if (!no) {
		if (!pbrms->dst)
			pbrms->dst = prefix_new();
		prefix_copy(pbrms->dst, prefix);
	} else {
		prefix_free(pbrms->dst);
		pbrms->dst = 0;
	}

	pbre = pbr_event_new();
	pbre->event = PBR_MAP_MODIFY;
	pbre->seqno = pbrms->seqno;
	strlcpy(pbre->name, pbrms->parent->name, sizeof(pbre->name));
	pbr_event_enqueue(pbre);

	return CMD_SUCCESS;
}

DEFPY(pbr_map_nexthop_group, pbr_map_nexthop_group_cmd,
	"[no] set nexthop-group NAME$name",
	NO_STR
	"Set for the PBR-MAP\n"
	"nexthop-group to use\n"
	"The name of the nexthop-group\n")
{
	struct pbr_map_sequence *pbrms = VTY_GET_CONTEXT(pbr_map_sequence);
	struct nexthop_group_cmd *nhgc;
	struct pbr_event *pbre;

	nhgc = nhgc_find(name);
	if (!nhgc) {
		vty_out(vty, "Specified nexthop-group %s does not exist\n",
			name);
		vty_out(vty, "PBR-MAP will not be applied until it is created\n");
	}

	if (pbrms->nhgrp_name)
		XFREE(MTYPE_TMP, pbrms->nhgrp_name);

	if (!no)
		pbrms->nhgrp_name = XSTRDUP(MTYPE_TMP, name);

	pbre = pbr_event_new();
	pbre->event = PBR_MAP_MODIFY;
	pbre->seqno = pbrms->seqno;
	strlcpy(pbre->name, pbrms->parent->name, sizeof(pbre->name));
	pbr_event_enqueue(pbre);

	return CMD_SUCCESS;
}

DEFPY(pbr_map_nexthop, pbr_map_nexthop_cmd,
      "[no] nexthop <A.B.C.D|X:X::X:X>$addr [INTERFACE]$intf [nexthop-vrf NAME$name]",
      NO_STR
      "Specify one of the nexthops in this ECMP group\n"
      "v4 Address\n"
      "v6 Address\n"
      "Interface to use\n"
      "If the nexthop is in a different vrf tell us\n"
      "The nexthop-vrf Name\n")
{
	struct pbr_map_sequence *pbrms = VTY_GET_CONTEXT(pbr_map_sequence);
	struct vrf *vrf;
	struct nexthop nhop;
	struct nexthop *nh;
	struct pbr_event *pbre;

	if (pbrms->nhgrp_name) {
		vty_out(vty,
			"Please unconfigure the nexthop group before adding an individual nexthop");
		return CMD_WARNING;
	}

	if (name)
		vrf = vrf_lookup_by_name(name);
	else
		vrf = vrf_lookup_by_id(VRF_DEFAULT);

	if (!vrf) {
		vty_out(vty, "Specified: %s is non-existent\n", name);
		return CMD_WARNING;
	}

	memset(&nhop, 0, sizeof(nhop));
	nhop.vrf_id = vrf->vrf_id;

	if (addr->sa.sa_family == AF_INET) {
		nhop.gate.ipv4.s_addr = addr->sin.sin_addr.s_addr;
		if (intf) {
			nhop.type = NEXTHOP_TYPE_IPV4_IFINDEX;
			nhop.ifindex = ifname2ifindex(intf, vrf->vrf_id);
			if (nhop.ifindex == IFINDEX_INTERNAL) {
				vty_out(vty,
					"Specified Intf %s does not exist in vrf: %s\n",
					intf, vrf->name);
				return CMD_WARNING;
			}
		} else
			nhop.type = NEXTHOP_TYPE_IPV4;
	} else {
		memcpy(&nhop.gate.ipv6, &addr->sin6.sin6_addr, 16);
		if (intf) {
			nhop.type = NEXTHOP_TYPE_IPV6_IFINDEX;
			nhop.ifindex = ifname2ifindex(intf, vrf->vrf_id);
			if (nhop.ifindex == IFINDEX_INTERNAL) {
				vty_out(vty,
					"Specified Intf %s does not exist in vrf: %s\n",
					intf, vrf->name);
				return CMD_WARNING;
			}
		} else
			nhop.type = NEXTHOP_TYPE_IPV6;
	}

	if (pbrms->nhg)
		nh = nexthop_exists(pbrms->nhg, &nhop);
	else {
		if (no) {
			vty_out(vty, "No nexthops to delete");
			return CMD_WARNING;
		}

		pbrms->nhg = nexthop_group_new();
		nh = NULL;
	}

	if (no) {
		if (nh) {
			// nexthop_del(pbrms->nhg, nh);
			// nexthop_free(nh);

			pbre = pbr_event_new();
			pbre->event = PBR_NEXTHOP_DELETE;
			pbre->seqno = pbrms->seqno;
			strlcpy(pbre->name, pbrms->parent->name,
				sizeof(pbre->name));
			pbr_event_enqueue(pbre);
		}
	} else if (!nh) {

		if (pbrms->nhg->nexthop) {
			vty_out(vty,
				"If you would like more than one nexthop please use nexthop-groups");
			return CMD_WARNING;
		}

		/* must be adding new nexthop since !no and !nexthop_exists */
		nh = nexthop_new();

		memcpy(nh, &nhop, sizeof(nhop));
		nexthop_add(&pbrms->nhg->nexthop, nh);

		pbre = pbr_event_new();
		pbre->event = PBR_NEXTHOP_ADD;
		pbre->seqno = pbrms->seqno;
		strlcpy(pbre->name, pbrms->parent->name, sizeof(pbre->name));
		pbr_event_enqueue(pbre);
	}

	return CMD_SUCCESS;
}

DEFPY (pbr_table_range,
       pbr_table_range_cmd,
       "[no]$no pbr table range (10000-65535)$start (11000-65535)$end",
       NO_STR
       "Policy based routing\n"
       "Policy based routing table\n"
       "Table range\n"
       "Initial value of range\n"
       "Final value of range\n")
{
	if (no)
		pbr_nht_set_tableid_range(PBR_NHT_DEFAULT_LOW_TABLEID,
					  PBR_NHT_DEFAULT_HIGH_TABLEID);
	else
		pbr_nht_set_tableid_range(start, end);

	return CMD_SUCCESS;
}

DEFPY (pbr_rule_range,
	pbr_rule_range_cmd,
	"[no] pbr rule range (300-1300)$start (400-1400)$end",
	NO_STR
	"Policy based routing\n"
	"Policy based routing rule\n"
	"Rule range\n"
	"Initial value of range\n"
	"Final value of range\n")
{
	if (no)
		pbr_nht_set_rule_range(PBR_NHT_DEFAULT_LOW_RULE,
				       PBR_NHT_DEFAULT_HIGH_RULE);
	else
		pbr_nht_set_rule_range(start, end);

	return CMD_SUCCESS;
}

DEFPY (pbr_policy,
	pbr_policy_cmd,
	"[no] pbr-policy NAME$mapname",
	NO_STR
	"Policy to use\n"
	"Name of the pbr-map to apply\n")
{
	VTY_DECLVAR_CONTEXT(interface, ifp);
	struct pbr_map *pbrm, *old_pbrm;
	struct pbr_interface *pbr_ifp = ifp->info;

	pbrm = pbrm_find(mapname);

	if (no) {
		if (strcmp(pbr_ifp->mapname, mapname) == 0) {
			strcpy(pbr_ifp->mapname, "");

			if (pbrm)
				pbr_map_interface_delete(pbrm, ifp);
		}
	} else {
		if (strcmp(pbr_ifp->mapname, "") == 0) {
			strcpy(pbr_ifp->mapname, mapname);

			if (pbrm)
				pbr_map_add_interface(pbrm, ifp);
		} else {
			if (!(strcmp(pbr_ifp->mapname, mapname) == 0)) {
				old_pbrm = pbrm_find(pbr_ifp->mapname);
				if (old_pbrm)
					pbr_map_interface_delete(old_pbrm, ifp);
				strcpy(pbr_ifp->mapname, mapname);
				if (pbrm)
					pbr_map_add_interface(pbrm, ifp);
			}
		}
	}

	return CMD_SUCCESS;
}

DEFPY (show_pbr,
	show_pbr_cmd,
	"show pbr [json$json]",
	SHOW_STR
	"Policy Based Routing\n"
	JSON_STR)
{
	pbr_nht_write_table_range(vty);
	pbr_nht_write_rule_range(vty);

	return CMD_SUCCESS;
}

DEFPY (show_pbr_map,
	show_pbr_map_cmd,
	"show pbr map [NAME$name] [detail$detail] [json$json]",
	SHOW_STR
	"Policy Based Routing\n"
	"PBR Map\n"
	"PBR Map Name\n"
	"Detailed information\n"
	JSON_STR)
{
	struct pbr_map_sequence *pbrms;
	struct pbr_map *pbrm;
	struct listnode *node;
	char buf[PREFIX_STRLEN];
	char rbuf[64];

	RB_FOREACH (pbrm, pbr_map_entry_head, &pbr_maps) {
		if (name && strcmp(name, pbrm->name) != 0)
			continue;

		vty_out(vty, "  pbr-map %s valid: %d\n", pbrm->name,
			pbrm->valid);

		for (ALL_LIST_ELEMENTS_RO(pbrm->seqnumbers, node, pbrms)) {
			if (pbrms->reason)
				pbr_map_reason_string(pbrms->reason, rbuf,
						      sizeof(rbuf));
			vty_out(vty,
				"    Seq: %u rule: %u Installed: %d(%u) Reason: %s\n",
				pbrms->seqno, pbrms->ruleno, pbrms->installed,
				pbrms->unique, pbrms->reason ? rbuf : "Valid");

			if (pbrms->src)
				vty_out(vty, "\tSRC Match: %s\n",
					prefix2str(pbrms->src, buf,
						   sizeof(buf)));
			if (pbrms->dst)
				vty_out(vty, "\tDST Match: %s\n",
					prefix2str(pbrms->dst, buf,
						   sizeof(buf)));

			if (pbrms->nhgrp_name) {
				vty_out(vty,
					"\tNexthop-Group: %s(%u) Installed: %u(%d)\n",
					pbrms->nhgrp_name,
					pbr_nht_get_table(pbrms->nhgrp_name),
					pbrms->nhs_installed,
					pbr_nht_get_installed(
						pbrms->nhgrp_name));
			} else if (pbrms->nhg) {
				vty_out(vty, "\t");
				nexthop_group_write_nexthop(
					vty, pbrms->nhg->nexthop);
				vty_out(vty,
					"\t\tInstalled: %u(%d) Tableid: %d\n",
					pbrms->nhs_installed,
					pbr_nht_get_installed(
						pbrms->internal_nhg_name),
					pbr_nht_get_table(
						pbrms->internal_nhg_name));
			} else {
				vty_out(vty,
					"\tNexthop-Group: Unknown Installed: 0(0)\n");
			}
		}
	}
	return CMD_SUCCESS;
}

DEFPY (show_pbr_interface,
	show_pbr_interface_cmd,
	"show pbr interface [NAME$name] [json$json]",
	SHOW_STR
	"Policy Based Routing\n"
	"PBR Interface\n"
	"PBR Interface Name\n"
	JSON_STR)
{
        struct interface *ifp;
	struct vrf *vrf;
	struct pbr_interface *pbr_ifp;

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name) {
		FOR_ALL_INTERFACES(vrf, ifp) {
			struct pbr_map *pbrm;

			if (name && strcmp(ifp->name, name) != 0)
				continue;

			pbr_ifp = ifp->info;

			if (strcmp(pbr_ifp->mapname, "") == 0)
				continue;

			pbrm = pbrm_find(pbr_ifp->mapname);
			vty_out(vty, "  %s(%d) with pbr-policy %s", ifp->name,
				ifp->ifindex, pbr_ifp->mapname);
			if (!pbrm)
				vty_out(vty, " (map doesn't exist)");
			vty_out(vty, "\n");
		}
	}

	return CMD_SUCCESS;
}

static struct cmd_node interface_node = {
	INTERFACE_NODE, "%s(config-if)# ", 1 /* vtysh ? yes */
};

static int pbr_interface_config_write(struct vty *vty)
{
	struct interface *ifp;
	struct vrf *vrf;

	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		FOR_ALL_INTERFACES (vrf, ifp) {
			if (vrf->vrf_id == VRF_DEFAULT)
				vty_frame(vty, "interface %s\n", ifp->name);
			else
				vty_frame(vty, "interface %s vrf %s\n",
					  ifp->name, vrf->name);

			pbr_map_write_interfaces(vty, ifp);

			vty_endframe(vty, "!\n");
		}
	}

	return 1;
}

/* PBR map node structure. */
static struct cmd_node pbr_map_node = {PBRMAP_NODE, "%s(config-pbr-map)# ", 1};

static int pbr_vty_map_config_write_sequence(struct vty *vty,
					     struct pbr_map *pbrm,
					     struct pbr_map_sequence *pbrms)
{
	char buff[PREFIX_STRLEN];

	vty_out (vty, "pbr-map %s seq %u\n",
		 pbrm->name, pbrms->seqno);

	if (pbrms->src)
		vty_out(vty, "  match src-ip %s\n",
			prefix2str(pbrms->src, buff, sizeof buff));

	if (pbrms->dst)
		vty_out(vty, "  match dst-ip %s\n",
			prefix2str(pbrms->dst, buff, sizeof buff));

	if (pbrms->nhgrp_name)
		vty_out(vty, "  set nexthop-group %s\n", pbrms->nhgrp_name);

	if (pbrms->nhg)
		nexthop_group_write_nexthop(vty, pbrms->nhg->nexthop);

	vty_out (vty, "!\n");
	return 1;
}

static int pbr_vty_map_config_write(struct vty *vty)
{
	struct pbr_map *pbrm;

	pbr_nht_write_table_range(vty);
	pbr_nht_write_rule_range(vty);

	RB_FOREACH(pbrm, pbr_map_entry_head, &pbr_maps) {
		struct pbr_map_sequence *pbrms;
		struct listnode *node;

		for (ALL_LIST_ELEMENTS_RO(pbrm->seqnumbers, node, pbrms)) {
			pbr_vty_map_config_write_sequence(vty, pbrm, pbrms);
		}
	}

	return 1;
}

void pbr_vty_init(void)
{
	install_node(&interface_node,
		     pbr_interface_config_write);
	if_cmd_init();

	install_node(&pbr_map_node,
		     pbr_vty_map_config_write);

	install_default(PBRMAP_NODE);

	install_element(CONFIG_NODE, &pbr_map_cmd);
	install_element(CONFIG_NODE, &no_pbr_map_cmd);
	install_element(INTERFACE_NODE, &pbr_policy_cmd);
	install_element(CONFIG_NODE, &pbr_table_range_cmd);
	install_element(CONFIG_NODE, &pbr_rule_range_cmd);
	install_element(PBRMAP_NODE, &pbr_map_match_src_cmd);
	install_element(PBRMAP_NODE, &pbr_map_match_dst_cmd);
	install_element(PBRMAP_NODE, &pbr_map_nexthop_group_cmd);
	install_element(PBRMAP_NODE, &pbr_map_nexthop_cmd);
	install_element(VIEW_NODE, &show_pbr_cmd);
	install_element(VIEW_NODE, &show_pbr_map_cmd);
	install_element(VIEW_NODE, &show_pbr_interface_cmd);

	/* debugs */
	install_element(VIEW_NODE, &debug_pbr_map_cmd);
	install_element(CONFIG_NODE, &debug_pbr_map_cmd);

	return;
}
