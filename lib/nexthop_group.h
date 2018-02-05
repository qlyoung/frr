/*
 * Nexthop Group structure definition.
 * Copyright (C) 2018 Cumulus Networks, Inc.
 *                    Donald Sharp
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

#ifndef __NEXTHOP_GROUP__
#define __NEXTHOP_GROUP__

/*
 * What is a nexthop group?
 *
 * A nexthop group is a collection of nexthops that make up
 * the ECMP path for the route.
 *
 * This module provides a proper abstraction to this idea.
 */
struct nexthop_group {
	struct nexthop *nexthop;
};

/*
 * Initialize nexthop_groups.  If you are interested in when
 * a nexthop_group is added/deleted/modified, then set the
 * appropriate callback functions to handle it in your
 * code
 */
void nexthop_group_init(void (*add)(const char *name),
			void (*delete)(const char *name),
			void (*modify)(const char *name));

void nexthop_add(struct nexthop **target, struct nexthop *nexthop);
void copy_nexthops(struct nexthop **tnh, struct nexthop *nh,
		   struct nexthop *rparent);

/* The following for loop allows to iterate over the nexthop
 * structure of routes.
 *
 * head:      The pointer to the first nexthop in the chain.
 *
 * nexthop:   The pointer to the current nexthop, either in the
 *            top-level chain or in a resolved chain.
 */
#define ALL_NEXTHOPS(head, nhop)					\
	(nhop) = (head.nexthop);					\
	(nhop);								\
	(nhop) = nexthop_next(nhop)

struct nexthop_group_cmd {

	RB_ENTRY(nexthop_group_cmd) nhgc_entry;

	char name[80];

	struct nexthop_group nhg;

	QOBJ_FIELDS
};
RB_HEAD(nhgc_entry_head, nexthp_group_cmd);
RB_PROTOTYPE(nhgc_entry_head, nexthop_group_cmd, nhgc_entry,
	     nexthop_group_cmd_compare)
DECLARE_QOBJ_TYPE(nexthop_group_cmd)

extern struct nexthop_group_cmd *nhgc_find(const char *name);

#endif
