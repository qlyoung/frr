/*
 * Object tracking header
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
#ifndef __OBJTRACK_H__
#define __OBJTRACK_H__

#include <zebra.h>

#include "lib/hash.h"
#include "lib/thread.h"
#include "lib/frrlua.h"

extern struct hash *objhash;

struct object {
	int id;
	char type[64];
	char name[64];
	char state[32];

	void (*cb)(struct object *obj);
};

/*
 * Lookup the object with the specified name
 */
struct object *objtrack_lookup(const char *name);

/*
 * Push a new table containing the tracked object fields
 * Push a tracked object as the following Lua table:
 *
 * +---------+--------------------+
 * | key     | value              |
 * +=========+====================+
 * | "id"    | object id (int)    |
 * +---------+--------------------+
 * | "type"  | object type (int)  |
 * +---------+--------------------+
 * | "state" | object state (int) |
 * +---------+--------------------+
 *
 * L
 *    Lua state to push object onto
 *
 * obj
 *    Name of object to push
 */
void objtrack_pushobject(lua_State *L, const struct object *obj);

/*
 * Convenience function to look up an object by name and push it as per
 * objtrack_pushobject.
 *
 * L
 *    Lua state to push object onto
 *
 * name
 *    Name of object to push
 */
void objtrack_pushobject_name(lua_State *L, const char *name);


/*
 * Track object.
 *
 * name
 *    Name of object to track
 *
 * cb
 *    Function to call when the object state changes
 */
void objtrack_track(const char *name, const char *type, int id);

/*
 * Remove callback function bound to object.
 *
 * name
 *    Name of object to track
 */
void objtrack_untrack(const char *name);

/*
 * Start running object tracking.
 *
 * master
 *    Thread master on which to schedule object tracking tasks
 *
 * interval
 *    Interval to repeat tracking task. If 0, the tracking task runs only once.
 */
void objtrack_start(struct thread_master *master, long interval);

/*
 * Stop running object tracking.
 */
void objtrack_stop(void);

#endif /* __OBJTRACK_H__ */
