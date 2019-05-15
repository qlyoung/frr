/*
 * VRRP object tracking.
 * Copyright (C) 2019 Cumulus Networks, Inc.
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
#ifndef __VRRP_TRACKING_H__
#define __VRRP_TRACKING_H__

#include <zebra.h>

#include "vrrp.h"
#include "lib/if.h"

/* Prototype obj tracking types --------------------------------------------- */

enum tracked_object_type {
	TRACKED_INTERFACE,
	TRACKED_ROUTE,
	TRACKED_IPSLA,
};

enum tracked_object_state {
	OBJ_DOWN,
	OBJ_UP
};

struct tracked_object {
	int id;
	enum tracked_object_type type;
	enum tracked_object_state state;
};

/* VRRP tracking ------------------------------------------------------------ */

struct vrrp_tracking {
	struct vrrp_vrouter *vr;
	struct list *objects;
};

enum vrrp_tracking_actiontype {
	VRRP_TRACKING_ACTION_DECREMENT,
	VRRP_TRACKING_ACTION_INCREMENT,
	VRRP_TRACKING_ACTION_SCRIPT,
};

/*
 * Initialize object tracking subsystem.
 *
 * Creates a Lua state and static datastructures. If provided, the given script
 * is loaded into the Lua environment.
 *
 * script
 *    Script to use when initializing the Lua state.
 */
void vrrp_tracking_init(char *script);

/*
 * Make a virtual router track an object.
 *
 * vr
 *    Virtual router
 *
 * obj
 *    Object that vr should track
 *
 * action
 *    Either a Lua chunk defining the function 'vrrp_tracking_update'.
 */
void vrrp_track_object(struct vrrp_vrouter *vr, struct tracked_object *obj,
		       enum vrrp_tracking_actiontype type, const void *actionarg);

/*
 * Make a virtual router stop tracking an object.
 *
 * vr
 *    Virtual router
 *
 * obj
 *    Object to track
 */
void vrrp_untrack_object(struct vrrp_vrouter *vr, struct tracked_object *obj);

/*
 * Event handler for object tracking. Call this function with the object that
 * has changed. The tracking actions for any virtual routers tracking this
 * object will be called.
 *
 * obj
 *    Object which changed tracking state
 */
void vrrp_tracking_event(struct tracked_object *obj);

#endif /* __VRRP_TRACKING_H__ */
