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

/* Lua VRRP object methods ------------------------------------------------- */

/*
 * Set priority of a VRRP instance.
 *
 * Argument stack:
 *    2 | priority
 *    1 | struct vrrp_router *
 */
static int vrrp_tracking_vr_set_priority(lua_State *L)
{
	zlog_err("%s called with %d arguments", __func__, lua_gettop(L));

	struct vrrp_vrouter *vr = (*(void**) lua_touserdata(L, -2));
	int prio = lua_tointeger(L, -1);

	zlog_err("priority = %d", prio);

	vrrp_set_priority(vr, prio);

	return 0;
}

/*
 * Functions to be installed in vrouter metatable.
 */
static const luaL_Reg vr_funcs[] = {
	{"set_priority", vrrp_tracking_vr_set_priority},
	{},
};

/*
 * Compute a unique key to use for storing any and all data related to the
 * vrouter in the registry.
 *
 * vr
 *    Virtual router to compute key for
 *
 * buf
 *    Buffer to store the key in
 *
 * buflen
 *    Size of buf
 */
static void vrrp_vrouter_regkey(const struct vrrp_vrouter *vr, char *buf,
				size_t buflen)
{
	snprintf(buf, buflen, "vrouter-%s@%" PRIu8, vr->ifp->name, vr->vrid);
}

/*
 * Create a userdata containing a pointer to a virtual router. The userdata's
 * metatable is then populated with various attrbutes and methods. These
 * attributes are copied from the struct; changing them in Lua will not change
 * the underlying values. The methods call back into C functions.
 *
 * Suppose 'vr' is the name of the pushed userdata within Lua. This object will
 * have the following attributes and functions:
 *
 *    vr.priority
 *    vr.vrid
 *    vr.iface
 *    vr.version
 *    vr:set_priority(int priority)
 */
static void vrrp_lua_pushvrouter(lua_State *L, const struct vrrp_vrouter *vr)
{
	zlog_debug("pushing vrrp_vrouter");

	char key[IFNAMSIZ + 64];

	snprintf(key, sizeof(key), "vrouter-metatable-%s@%" PRIu8,
		 vr->ifp->name, vr->vrid);

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

/* Object Tracking ---------------------------------------------------------- */

static lua_State *L;

/*
 * The following three tables together with their wrappers provide the following functions:
 *
 * - Adding, removing, and looking up assocations between objects and the
 *   virtual routers tracking them
 * - Adding, removing, and looking up associations between virtual routers and
 *   the objects they are tracking
 * - Adding, removing, and looking up assocations between virtual routers and
 *   their lua_States
 *
 * Essentially these are structure extensions implemented as hash tables. There
 * are a few reasons for doing it like this:
 *
 * - Keeps tracking data isolated
 * - Keeps bloat out of struct vrrp_vrouter
 * - It's fast
 *
 * Ideally no statics would be used but the mapping for objects must be stored
 * statically anyway, so might as well make it easy.
 */

/*
 * This hash maps a tracked object to a linked list of the virtual routers that
 * are tracking it.
 *
 * Key: struct tracked_object
 * Val: struct vrrp_objvr_hash_entry
 */
static struct hash *vrrp_objvr_hash;

struct vrrp_objvr_hash_entry {
	/* Tracked object */
	struct tracked_object *obj;
	/* List of vrouters tracking the object */
	struct list *tracklist;
};

static unsigned int vrrp_objvr_hash_key(const void *obj)
{
	const struct vrrp_objvr_hash_entry *e = obj;

	return e->obj->id;
}

static bool vrrp_objvr_hash_cmp(const void *val1, const void *val2)
{
	const struct vrrp_objvr_hash_entry *e1 = val1;
	const struct vrrp_objvr_hash_entry *e2 = val2;

	return e1->obj->id == e2->obj->id;
}

/*
 * This hash maps a virtual router to a linked list of ojects that it is
 * tracking. It is the reverse mapping of the above hash.
 *
 * Key: virtual router
 * Val: struct vrrp_objtrack_hash_entry
 */
static struct hash *vrrp_vrobj_hash;

struct vrrp_vrobj_hash_entry {
	/* Virtual router */
	struct vrrp_vrouter *vr;
	/* List of objects this VR is tracking */
	struct list *tracklist;
};

static unsigned int vrrp_vrobj_hash_key(const void *data)
{
	const struct vrrp_vrobj_hash_entry *e = data;

	return vrrp_hash_key(e->vr);
}

static bool vrrp_vrobj_hash_cmp(const void *val1, const void *val2)
{
	const struct vrrp_vrobj_hash_entry *e1 = val1;
	const struct vrrp_vrobj_hash_entry *e2 = val2;

	return e1->vr == e2->vr;
}

/* Wrappers for the above three hashes */
static void *vrrp_objvr_hash_alloc(void *arg)
{
	struct vrrp_objvr_hash_entry *the = arg;
	struct vrrp_objvr_hash_entry *e =
		XCALLOC(MTYPE_TMP, sizeof(struct vrrp_objvr_hash_entry));

	/* XXX: Fixme */
	e->obj = XCALLOC(MTYPE_TMP, sizeof(struct tracked_object));
	memcpy(e->obj, the->obj, sizeof(struct tracked_object));
	e->tracklist = list_new();

	return e;
}


static void *vrrp_vrobj_hash_alloc(void *arg)
{
	struct vrrp_vrobj_hash_entry vohe, *v;
	vohe.vr = arg;

	v = XCALLOC(MTYPE_TMP, sizeof(struct vrrp_vrobj_hash_entry));
	v->vr = arg;
	v->tracklist = list_new();

	return v;
}

static struct list *vrrp_tracking_get_objects(struct vrrp_vrouter *vr)
{
	struct vrrp_vrobj_hash_entry vohe, *v;
	vohe.vr = vr;

	v = hash_get(vrrp_vrobj_hash, &vohe, vrrp_vrobj_hash_alloc);

	if (!v) {
		zlog_err(VRRP_LOGPFX VRRP_LOGPFX_VRID
			 "VRRP router not registered with tracking",
			 vr->vrid);
		return NULL;
	}

	return v->tracklist;
}

static struct list *vrrp_tracking_get_vrs(struct tracked_object *obj)
{
	struct vrrp_objvr_hash_entry ovhe, *v;
	ovhe.obj = obj;

	v = hash_get(vrrp_objvr_hash, &ovhe, vrrp_objvr_hash_alloc);

	return v->tracklist;
}

static void vrrp_tracking_add_object(struct vrrp_vrouter *vr, struct tracked_object *obj)
{
	struct list *objvrlist = vrrp_tracking_get_objects(vr);
	struct list *vrobjlist = vrrp_tracking_get_vrs(obj);

	/* XXX: check for duplicates */
	listnode_add(vrobjlist, obj);
	listnode_add(objvrlist, vr);
}

static void vrrp_tracking_remove_object(struct vrrp_vrouter *vr, struct tracked_object *obj)
{
	struct list *vrobjlist = vrrp_tracking_get_objects(vr);
	struct list *objvrlist = vrrp_tracking_get_vrs(obj);

	listnode_delete(vrobjlist, obj);
	listnode_delete(objvrlist, vr);

	/* XXX: If vrobjlist empty, delete entry from hash */
	/* XXX: If objvrlist empty, delete entry from hash */

	/* If vrobjlist empty, delete router from Lua registry */
}

/*
 * Push the table associated wth VRRP vrouter onto the stack
 */
static bool vrrp_tracking_getregtable(lua_State *L, struct vrrp_vrouter *vr)
{
	char key[BUFSIZ];

	vrrp_vrouter_regkey(vr, key, sizeof(key));
	bool created = !luaL_getsubtable(L, LUA_REGISTRYINDEX, key);

	if (created)
		zlog_info("Created new registry subtable %s", key);

	return created;
}

/*
 * Builtin action chunks.
 *
 * Rather than a dual-backend approach, where the default tracking actions -
 * decrement and increment - are implemented totally in C, while everything
 * else happens in Lua, it's cleaner and cooler to always hit Lua for our
 * tracking actions. We do this by hardcoding Lua snippets corresponding to
 * each action. We give Lua access to configuration variables by exporting them
 * to the Lua environment in hardcoded variable names used in the snippets.
 *
 * Ideally the entire environment would be encoded into this array but for now
 * there's still a bit of glue code below it that needs to be poked to add more
 * builtins here.
 */
#define BUILTIN_PRIO_ADJUST "__PRIO_ADJUST"

const char *vrrp_tracking_builtin_actions[] = {
	[VRRP_TRACKING_ACTION_DECREMENT] = "\
	if (obj.state == OBJ_DOWN) then\
	    vr:set_priority(vr.priority - " BUILTIN_PRIO_ADJUST ") \
	end",
	[VRRP_TRACKING_ACTION_INCREMENT] = "\
	if (obj.state == OBJ_DOWN) then\
	    vr:set_priority(vr.priority + " BUILTIN_PRIO_ADJUST ") \
	end",
};

static void vrrp_tracking_set_builtin(lua_State *L, struct vrrp_vrouter *vr,
				      enum vrrp_tracking_actiontype tt,
				      const void *actionarg)
{
	/* Get or create registry table for this vrouter */
	vrrp_tracking_getregtable(L, vr);

	/* Set script as "action" field */
	assert(lua_istable(L, -1));

	lua_setfield(L, -2, "action");
}

/*
 * Load the given script as a function and store it in the registry at a unique
 * key we can compute from the vrouter.
 *
 * When an object tracking event occurs, we will fetch the Lua function, give
 * it a nice environment with lots of VRRP information and an object it can use
 * to manipulate the vrouter, and run it.
 */
static void vrrp_tracking_set_script(lua_State *L, struct vrrp_vrouter *vr,
				     const char *path)
{
	/* Get or create registry table for this vrouter */
	vrrp_tracking_getregtable(L, vr);

	/* Set script as "action" field */
	assert(lua_istable(L, -1));
	lua_pushstring(L, path);
	lua_setfield(L, -2, "action");

	/* Pop vrouter regtable */
	assert(lua_istable(L, -1));
	lua_pop(L, -1);
}

/* Some object event has occurred; handle it */
static int vrrp_tracking_handle(lua_State *L, struct tracked_object *obj,
			   struct vrrp_vrouter *vr)
{
	int err;

	/* Get regsubtable for this vrouter */
	bool created = !vrrp_tracking_getregtable(L, vr);
	assert(!created);

	/* Get action */
	lua_pushliteral(L, "action");
	lua_gettable(L, -2);
	lua_remove(L, -2);
	assert(lua_isstring(L, -1) || lua_isfunction(L, -1));

	if (lua_isstring(L, -1)) {
		const char *path = lua_tostring(L, -1);
		const char *errstring = NULL;

		assert(path);
		err = luaL_loadfile(L, path);
		/* Remove path */
		lua_remove(L, -2);

		if (err != LUA_OK)
			errstring = lua_tostring(L, -1);

		switch (err) {
		case LUA_OK:
			break;
		case LUA_ERRSYNTAX:
		case LUA_ERRMEM:
		case LUA_ERRGCMM:
		default:
			zlog_warn(VRRP_LOGPFX "Unable to load script at %s: %s",
				  path, errstring);
			/* Error string cannot be used after this point! */
			lua_pop(L, 1);
			break;
		}

		/* Stack must be clean at this point! */
		if (err != LUA_OK)
			return err;

		zlog_info(VRRP_LOGPFX "Loaded %s", path);
	}

	assert(lua_isfunction(L, -1));

	/* Set up the script environment */

	/* Set 'vr' as global variable */
	vrrp_lua_pushvrouter(L, vr);
	lua_setglobal(L, "vr");

	/* Set 'obj' as global variable */
	objtrack_lua_pushtrackedobject(L, obj);
	lua_setglobal(L, "obj");

	/* Set state constants as global variables */
	lua_pushinteger(L, OBJ_UP);
	lua_setglobal(L, "OBJ_UP");
	lua_pushinteger(L, OBJ_DOWN);
	lua_setglobal(L, "OBJ_DOWN");

	/* Call handler */
	err = lua_pcall(L, 0, 1, 0);

	/* Clean environment */
	lua_pushnil(L);
	lua_setglobal(L, "vr");
	lua_pushnil(L);
	lua_setglobal(L, "obj");
	lua_pushnil(L);
	lua_setglobal(L, "OBJ_UP");
	lua_pushnil(L);
	lua_setglobal(L, "OBJ_DOWN");

	return err;
}

/* Tracking API ------------------------------------------------------------ */

/*
 * Handler for object tracking events. Invoke Lua handler for every vrouter
 * tracking this object.
 *
 * obj
 *    The object that has changed
 */
void vrrp_tracking_event(struct tracked_object *obj)
{
	struct listnode *ln;
	struct vrrp_vrouter *vr;

	struct vrrp_objvr_hash_entry e = {
		.obj = obj,
	};
	struct vrrp_objvr_hash_entry *v;

	v = hash_lookup(vrrp_objvr_hash, &e);

	if (!v)
		return;

	for (ALL_LIST_ELEMENTS_RO(v->tracklist, ln, vr)) {
		vrrp_tracking_handle(L, obj, vr);
	}
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
	const char *script;

	vrrp_tracking_add_object(vr, obj);

	switch (actiontype) {
		case VRRP_TRACKING_ACTION_SCRIPT:
			script = actionarg;
			vrrp_tracking_set_script(L, vr, script);
			break;
		default:
			vrrp_tracking_set_builtin(L, vr, actiontype, actionarg);
			break;
	}
}

void vrrp_untrack_object(struct vrrp_vrouter *vr, struct tracked_object *obj)
{
	vrrp_tracking_remove_object(vr, obj);
}

void vrrp_tracking_init(char *script)
{
	vrrp_objvr_hash = hash_create(vrrp_objvr_hash_key, vrrp_objvr_hash_cmp,
				      "VRRP reverse object tracking table");
	vrrp_vrobj_hash = hash_create(vrrp_vrobj_hash_key, vrrp_vrobj_hash_cmp,
				      "VRRP object tracking table");

	L = frrlua_initialize(script);

	zlog_notice("Initialized VRRP object tracking");
}
