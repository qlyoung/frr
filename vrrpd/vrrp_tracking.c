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
#include <zebra.h>

#include "lib/if.h"
#include "lib/frrlua.h"
#include "lib/hash.h"
#include "lib/linklist.h"

#include "vrrp.h"
#include "vrrp_debug.h"
#include "vrrp_tracking.h"

#define VRRP_LOGPFX "[TRACK] "

/* Object tracking mockup --------------------------------------------------- */

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
 */
static void objtrack_lua_pushtrackedobject(lua_State *L, const struct tracked_object *obj)
{
	zlog_debug("pushing tracked_object");

	lua_newtable(L);
	lua_pushinteger(L, obj->id);
	lua_setfield(L, -2, "id");
	lua_pushinteger(L, obj->type);
	lua_setfield(L, -2, "type");
	lua_pushinteger(L, obj->state);
	lua_setfield(L, -2, "state");

}

/* Stuff -------------------------------------------------------------------- */

/*
 * Called from Lua to set priority of a VRRP instance.
 */
static int vrrp_ot_vr_set_priority(lua_State *L)
{
	zlog_err("%s called with %d arguments", __func__, lua_gettop(L));

	struct vrrp_vrouter *vr = (*(void**) lua_touserdata(L, -2));
	int prio = lua_tointeger(L, -1);

	zlog_err("priority = %d", prio);

	vrrp_set_priority(vr, prio);

	return 0;
}

/*
 * Called from Lua to set protocol state of a VRRP instance.
 */
static int vrrp_ot_vr_set_state(lua_State *L)
{
	zlog_err("%s called", __func__);

	return 0;
}

/*
 * The metatable for VRRP instances that we pass to the Lua handlers.
 */
static const luaL_Reg vr_funcs[] = {
	{"set_priority", vrrp_ot_vr_set_priority},
	{"set_state", vrrp_ot_vr_set_state},
	{},
};

/*
 * Compute a unique key to use for storing any and all data related to the
 * vrouter in the registry.
 */
static void vrrp_vrouter_regkey(const struct vrrp_vrouter *vr, char *buf,
				size_t buflen)
{
	snprintf(buf, buflen, "vrouter-%s@%" PRIu8, vr->ifp->name, vr->vrid);
}

/*
 * Push a VRRP router as a userdata containing a pointer to the actual struct
 * vrrp_vrouter, which has the following attributes and functions:
 *
 * vr.priority
 * vr.vrid
 * vr.iface
 * vr.version
 * vr:set_priority(int priority)
 */
static void vrrp_lua_pushvrouter(lua_State *L, const struct vrrp_vrouter *vr)
{
	zlog_debug("pushing vrrp_vrouter");

	char key[IFNAMSIZ + 64];

	snprintf(key, sizeof(key), "vr-metatable-%s@%" PRIu8, vr->ifp->name, vr->vrid);

	/* Setup metatable for our vrouter object */
	if (luaL_newmetatable(L, key) == 1) {
		/* Set metatable's __index to itself */
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
	}

	/* Add object methods */
	luaL_setfuncs(L, vr_funcs, 0);

	/* Add object fields */
	lua_pushinteger(L, vr->priority);
	lua_setfield(L, -2, "priority");
	lua_pushinteger(L, vr->vrid);
	lua_setfield(L, -2, "vrid");
	lua_pushstring(L, vr->ifp->name);
	lua_setfield(L, -2, "iface");
	lua_pushinteger(L, vr->version);
	lua_setfield(L, -2, "version");

	/* Create vrouter userdata */
	void *ptrdata = lua_newuserdata(L, sizeof(vr));
	memcpy(ptrdata, &vr, sizeof(vr));

	/* Set its metatable */
	luaL_setmetatable(L, key);

	/* Pop metatable */
	lua_remove(L, -2);
}

static void vrrp_handle_lua_err(lua_State *L, int err) {
	if (err) {
		const char *errstring = lua_tostring(L, -1);
		fprintf(stderr, "Error: %d - %s\n", err, errstring);
	}
	else {
		fprintf(stderr, "Ok\n");
	}
}

static lua_State *L;

/* VRRP tracking types ----------------------------------------------------- */

/*
 * Key: object ID
 * Val: struct vrrp_vrouter
 */
static struct hash *vrrp_trackhash;

/* Builtin action chunks */
const char *vrrp_tracking_builtin_actions[] = {

[VRRP_TRACKING_ACTION_DECREMENT] = "\
if (obj.state == OBJ_DOWN) then\
    vr:set_priority(vr.priority - %d) \
end",
};


struct vrrp_trackhash_entry {
	/* Tracked object */
	struct tracked_object *obj;
	/* List of vrouters tracking the object */
	struct list *tracklist;
};

static unsigned int vrrp_trackhash_key(void *obj)
{
	struct vrrp_trackhash_entry *e = obj;

	return e->obj->id;
}

static bool vrrp_trackhash_cmp(const void *val1, const void *val2)
{
	const struct vrrp_trackhash_entry *e1 = val1;
	const struct vrrp_trackhash_entry *e2 = val2;

	return e1->obj->id == e2->obj->id;
}

void vrrp_tracking_init(char *script)
{
	if (script)
		fprintf(stderr, "Script file: %s\n", script);

	L = frrlua_initialize(script);

	if (!L)
		return;

	vrrp_trackhash = hash_create(vrrp_trackhash_key, vrrp_trackhash_cmp,
				     "VRRP object tracking table");
}

/* Lua Extensions ---------------------------------------------------------- */

/*
 * The Lua function names we will call for different object types.
 */
const char *vrrp_tracking_lua_entrypoint = "vrrp_tracking_update";

/*
 * Push the table associated wth VRRP vrouter onto the stack
 */
static void vrrp_tracking_getregtable(struct vrrp_vrouter *vr)
{
	char key[BUFSIZ];

	vrrp_vrouter_regkey(vr, key, sizeof(key));
	if (!luaL_getsubtable(L, LUA_REGISTRYINDEX, key))
		zlog_warn("Created new registry subtable %s", key);
}


/* Some object event has occurred; handle it */
static void vrrp_ot_handle(struct tracked_object *obj, struct vrrp_vrouter *vr)
{
	int err;

	/* Get regsubtable for this vrouter */
	vrrp_tracking_getregtable(vr);

	/* Get script path from regsubtable */
	assert(lua_istable(L, -1));
	lua_pushstring(L, "action");
	lua_gettable(L, -2);

	/* Pop regsubtable */
	lua_remove(L, -2);

	/* Load script at path */
	assert(lua_isstring(L, -1));
	const char *path = lua_tostring(L, -1);
	err = luaL_loadfile(L, path);
	vrrp_handle_lua_err(L, err);

	assert(lua_isfunction(L, -1));

	zlog_warn("Loaded %s", path);

	/* Set 'vr' as global variable */
	vrrp_lua_pushvrouter(L, vr);
	lua_setglobal(L, "vr");

	/* Set 'obj' as global variable */
	objtrack_lua_pushtrackedobject(L, obj);
	lua_setglobal(L, "obj");

	/* Set state literals as global variables */
	lua_pushinteger(L, OBJ_UP);
	lua_setglobal(L, "OBJ_UP");
	lua_pushinteger(L, OBJ_DOWN);
	lua_setglobal(L, "OBJ_DOWN");

	/* Call handler */
	err = lua_pcall(L, 0, 1, 0);
	vrrp_handle_lua_err(L, err);

	const char *result = lua_tostring(L, -1);
	fprintf(stderr, "result: %s\n", result);
}

/* VRRP tracking bindings --------------------------------------------------- */

static void *vrrp_trackhash_alloc(void *arg)
{
	struct vrrp_trackhash_entry *the = arg;
	struct vrrp_trackhash_entry *e =
		XCALLOC(MTYPE_TMP, sizeof(struct vrrp_trackhash_entry));
	struct list *tracklist = list_new();

	e->obj = XCALLOC(MTYPE_TMP, sizeof(struct tracked_object));
	memcpy(e->obj, the->obj, sizeof(struct tracked_object));
	e->tracklist = tracklist;

	return e;
}

/*
 * objtrackd has sent us an object whose state has presumably changed; invoke
 * Lua handler for every vrouer tracking this object
 */
void vrrp_tracking_event(struct tracked_object *obj)
{
	struct listnode *ln;
	struct vrrp_vrouter *vr;

	struct vrrp_trackhash_entry e = {
		.obj = obj,
	};
	struct vrrp_trackhash_entry *v;

	v = hash_lookup(vrrp_trackhash, &e);

	if (!v)
		return;

	for (ALL_LIST_ELEMENTS_RO(v->tracklist, ln, vr)) {
		vrrp_ot_handle(obj, vr);
	}
}

/*
 * Load the given script as a function and store it in the registry at a unique
 * key we can compute from the vrouter.
 *
 * When an object tracking event occurs, we will fetch the Lua function, give
 * it a nice environment with lots of VRRP information and an object it can use
 * to manipulate the vrouter, and run it.
 */
static void vrrp_tracking_set_script(struct vrrp_vrouter *vr, const char *path)
{
	/* Get or create registry table for this vrouter */
	vrrp_tracking_getregtable(vr);

	/* Set script as "action" field */
	assert(lua_istable(L, -1));
	lua_pushstring(L, path);
	lua_setfield(L, -2, "action");

	/* Pop vrouter regtable */
	assert(lua_istable(L, -1));
	lua_pop(L, -1);
}

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
		       enum vrrp_tracking_actiontype actiontype,
		       const void *actionarg)
{
	struct vrrp_trackhash_entry e = {};
	struct vrrp_trackhash_entry *v;

	int decrement;
	const char *script;

	e.obj = obj;
	v = hash_get(vrrp_trackhash, &e, vrrp_trackhash_alloc);

	switch (actiontype) {
		case VRRP_TRACKING_ACTION_DECREMENT:
			decrement = *((int *)actionarg);
			break;
		case VRRP_TRACKING_ACTION_SCRIPT:
			script = actionarg;
			vrrp_tracking_set_script(vr, script);
			break;
	}

	if (actionarg == NULL)
		listnode_delete(v->tracklist, vr);
	else
		listnode_add(v->tracklist, vr);
}
