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
#include "lib/objtrack.h"

#include "vrrp.h"
#include "vrrp_debug.h"
#include "vrrp_tracking.h"

#define VRRP_LOGPFX "[TRACK] "

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
	zlog_info(VRRP_LOGPFX "%s called with %d arguments", __func__,
		  lua_gettop(L));

	struct vrrp_vrouter *vr = (*(void**) lua_touserdata(L, -2));
	int prio = lua_tointeger(L, -1);

	zlog_err(VRRP_LOGPFX "priority = %d", prio);

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
 * (obj, vrouter) tuple in the registry.
 *
 * vr
 *    Virtual router to compute key for
 *
 * obj
 *    Object associated with this vrouter
 *
 * buf
 *    Buffer to store the key in
 *
 * buflen
 *    Size of buf
 */
static void vrrp_vrouter_regkey(const struct vrrp_vrouter *vr, const char *name,
				char *buf, size_t buflen)
{
	snprintf(buf, buflen, "%s@%s@%" PRIu8, name, vr->ifp->name, vr->vrid);
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
	zlog_debug(VRRP_LOGPFX "pushing vrrp_vrouter");

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

/*
 * The global Lua state for VRRPD.
 *
 * Specific data for each (obj, vrouter) tuple is stored in a table in the Lua
 * registry. To get this table use vrrp_tracking_getregtable(vr). This table
 * has the following format:
 *
 * "<objid>@<ifname>@<vrid>" = {
 *    "action" = "path/to/script" || <compiled chunk>,
 *    "args" = {
 *       1 = arg1,
 *       2 = arg2,
 *       ...
 *       N = argN,
 *    },
 *    "env" = {
 *       "var1" = val1,
 *       "var2" = val2,
 *       ...
 *       "varN" = valN,
 *    }
 * }
 *
 * When a user requests that a particular vrouter track a particular object, we
 * store the desired action - a path to a Lua script if the user specified one,
 * or a compiled chunk if they specified one of VRRP's built in actions - in
 * the "action" field. Any arguments are stored in the "args" array and any
 * environment variables are stored in the "env" table. nil is not allowed in
 * "args" or "env".
 *
 * Suppose the table for a particular vrouter is t. When an event occurs, this
 * table is examined. t["action"] may be a Lua function or a script. If it is a
 * string, it is taken to be a path (full or relative to the cwd) to the Lua
 * script, which is then loaded and compiled.  The function is then run with
 * its environment equal to t["env"], and with the arguments specified in
 * t["args"], pushed in order starting from 1. For instance, if:
 *
 *    t["args"] = {
 *       1 = "dog",
 *       2 = "cat",
 *    }
 *
 * Then in the chunk, unpacking the varargs:
 *
 *    a,b = ...
 *
 * We have:
 *
 *    a = "dog"
 *    b = "cat
 *
 * Similarly if
 *
 *    t["env"] = {
 *       'vr' = userdata,
 *       'OBJ_UP' = 1,
 *       'OBJ_DOWN' = 2,
 *    }
 *
 * Then the chunk will have access to variables named vr, OBJ_UP and OBJ_DOWN.
 */
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
	const char *objname;
	/* List of vrouters tracking the object */
	struct list *tracklist;
};

static unsigned int vrrp_objvr_hash_key(const void *obj)
{
	const struct vrrp_objvr_hash_entry *e = obj;

	return string_hash_make(e->objname);
}

static bool vrrp_objvr_hash_cmp(const void *val1, const void *val2)
{
	const struct vrrp_objvr_hash_entry *e1 = val1;
	const struct vrrp_objvr_hash_entry *e2 = val2;

	return strmatch(e1->objname, e2->objname);
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

	e->objname = XSTRDUP(MTYPE_TMP, the->objname);
	e->tracklist = list_new();

	return e;
}

static void *vrrp_vrobj_hash_alloc(void *arg)
{
	struct vrrp_vrobj_hash_entry *vohe, *v;
	vohe = arg;

	v = XCALLOC(MTYPE_TMP, sizeof(struct vrrp_vrobj_hash_entry));
	v->vr = vohe->vr;
	v->tracklist = list_new();

	return v;
}

static struct list *vrrp_tracking_get_objects(struct vrrp_vrouter *vr)
{
	struct vrrp_vrobj_hash_entry vohe = {}, *v;
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

static struct list *vrrp_tracking_get_vrs(const char *name)
{
	struct vrrp_objvr_hash_entry ovhe, *v;
	ovhe.objname = name;

	v = hash_get(vrrp_objvr_hash, &ovhe, vrrp_objvr_hash_alloc);

	return v->tracklist;
}

static void vrrp_tracking_add_object(struct vrrp_vrouter *vr, const char *name)
{
	struct list *objvrlist = vrrp_tracking_get_vrs(name);
	struct list *vrobjlist = vrrp_tracking_get_objects(vr);
	struct listnode *ln;

	const char *ename;
	bool exists = false;

	for (ALL_LIST_ELEMENTS_RO(vrobjlist, ln, ename)) {
		exists = strmatch(ename, name);
		if (exists)
			break;
	}

	if (!exists) {
		listnode_add(vrobjlist, XSTRDUP(MTYPE_TMP, name));
		listnode_add(objvrlist, vr);
	}
}

static void vrrp_tracking_remove_object(struct vrrp_vrouter *vr,
					const char *name)
{
	struct vrrp_vrobj_hash_entry vohe = {}, *r1;
	struct vrrp_objvr_hash_entry ovhe = {}, *r2;

	vohe.vr = vr;
	ovhe.objname = name;

	r1 = hash_lookup(vrrp_vrobj_hash, &vohe);
	r2 = hash_lookup(vrrp_objvr_hash, &ovhe);

	/* Consistency check: make sure both lists exist if the primary does */
	if (!r1)
		return;

	assert(r2);

	struct list *vrobjlist = r1->tracklist;
	struct list *objvrlist = r2->tracklist;

	/* Consistency check: make sure our reverse mapping is present */
	bool found = false;
	struct listnode *ln, *nn;
	struct vrrp_vrouter *vr2;
	for (ALL_LIST_ELEMENTS_RO(objvrlist, ln, vr2)) {
		found = (vr == vr2);
		if (found)
			break;
	}
	assert(found);

	char *ename;

	for (ALL_LIST_ELEMENTS(vrobjlist, ln, nn, ename)) {
		if (strmatch(name, ename)) {
			list_delete_node(vrobjlist, nn);
			XFREE(MTYPE_TMP, ename);
			break;
		}
	}
	listnode_delete(objvrlist, vr);

	/* If this vrouter isn't tracking anymore objects, delete it */
	if (vrobjlist->count == 0) {
		struct vrrp_vrobj_hash_entry vohe = {};
		vohe.vr = vr;
		/* Remove list from hash */
		hash_release(vrrp_vrobj_hash, &vohe);
		/* Unset registry table for vrouter */
		char key[BUFSIZ];
		vrrp_vrouter_regkey(vr, name, key, sizeof(key));
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, key);
		zlog_info(VRRP_LOGPFX VRRP_LOGPFX_VRID
			  "Destroyed registry subtable %s",
			  vr->vrid, key);
		/* Delete list */
		list_delete(&vrobjlist);
	}

	/* If nobody is tracking this object anymore, delete it */
	if (objvrlist->count == 0) {
		struct vrrp_objvr_hash_entry ovhe = {};
		ovhe.objname = name;
		hash_release(vrrp_objvr_hash, &ovhe);
		list_delete(&vrobjlist);
	}
}

/*
 * Push the table associated wth this (obj, vrouter) tuple onto the stack. If
 * the table does not exist, it is created.
 *
 * vr
 *    Virtual router
 *
 * obj
 *    Tracked object
 */
static bool vrrp_tracking_getregtable(lua_State *L, struct vrrp_vrouter *vr,
				      const char *name)
{
	char key[BUFSIZ];

	vrrp_vrouter_regkey(vr, name, key, sizeof(key));
	bool created = !luaL_getsubtable(L, LUA_REGISTRYINDEX, key);

	if (created)
		zlog_info(VRRP_LOGPFX VRRP_LOGPFX_VRID
			  "Created new registry subtable %s",
			  vr->vrid, key);

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
 * to the Lua environment in hardcoded variable names used in the hardcoded
 * snippets.
 *
 * Ideally the entire environment would be encoded into this array but for now
 * there's still a bit of glue code below it that needs to be poked to add more
 * builtins here.
 *
 * We pass additional data as argument(s), which are stored in the regsubtable
 * for the (obj, vrouter). The action setter function is responsible for
 * storing those arguments in the expected order and number so that the
 * snippets below work.
 */
const char *vrrp_tracking_builtin_actions[] = {
	[VRRP_TRACKING_ACTION_DECREMENT] = "\
	prio = ... \
	if (obj.state == OBJ_DOWN) then\
	    vr:set_priority(vr.priority - prio) \
	end",
	[VRRP_TRACKING_ACTION_INCREMENT] = "\
	prio = ... \
	if (obj.state == OBJ_DOWN) then\
	    vr:set_priority(vr.priority + prio) \
	end",
};

/*
 * Set the action in a vrouter's regsubtable for a tracking event.
 *
 * When an object tracking event occurs, we will fetch the Lua function, give
 * it a nice environment with lots of VRRP information and an object it can use
 * to manipulate the vrouter, and run it.
 *
 * vr
 *    Virtual router
 *
 * obj
 *    Tracked object
 *
 * at
 *    One of the possible action types. If VRRP_TRACKING_ACTION_SCRIPT, then
 *    'arg' specifies the path of the script. Otherwise this value specifies
 *    which builtin to use.
 *
 * arg
 *    If 'at' is VRRP_TRACKING_ACTION_SCRIPT, this is the path to the script.
 *    Otherwise it is the argument to the specified builtin.
 */
static void vrrp_tracking_set_action(struct vrrp_vrouter *vr, const char *name,
				     enum vrrp_tracking_actiontype at,
				     const void *arg)
{
	/* Get or create registry table for this (obj, vrouter) */
	vrrp_tracking_getregtable(L, vr, name);
	assert(lua_istable(L, -1));

	/* Set script as "action" field */
	switch (at) {
		case VRRP_TRACKING_ACTION_DECREMENT:
		case VRRP_TRACKING_ACTION_INCREMENT:
			luaL_loadstring(L, vrrp_tracking_builtin_actions[at]);
			lua_setfield(L, -2, "action");
			/* Save argument */
			luaL_getsubtable(L, -1, "args");
			int idx = luaL_len(L, -1) + 1;
			lua_pushinteger(L, idx);
			lua_pushinteger(L, *(int *)arg);
			lua_settable(L, -3);
			idx = luaL_len(L, -1);
			lua_pop(L, 1);
			break;
		case VRRP_TRACKING_ACTION_SCRIPT:
			lua_pushstring(L, arg);
			lua_setfield(L, -2, "action");
			break;
	}

	/* Pop vrouter regtable */
	assert(lua_istable(L, -1));
	lua_pop(L, 1);
}

/*
 * Call the action for this (obj, vrouter) tuple.
 *
 * This function first retrieves the regsubtable for this (obj, vrouter). The
 * "action" field may be either a function or a string. If the it is a string,
 * we load the path represented by that string as a Lua chunk. We then export
 * the object and vrouter as variables 'obj' and 'vr' in the chunk environment,
 * and push any arguments saved in the "args" field of the regsubtable. The
 * return code is the result of lua_pcall().
 *
 * obj
 *    Tracked object
 *
 * vr
 *    Virtual router
 */
static int vrrp_tracking_handle(lua_State *L, struct object *obj,
				struct vrrp_vrouter *vr)
{
	int err;

	/* Get regsubtable for this vrouter */
	bool created = vrrp_tracking_getregtable(L, vr, obj->name);
	assert(!created);

	/* Get action */
	lua_pushliteral(L, "action");
	lua_gettable(L, -2);
	lua_remove(L, -2);
	assert(lua_isstring(L, -1) || lua_isfunction(L, -1));

	/* If it's a file path, load the chunk in that file */
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

	/* Create a clean environment table for the chunk */
	lua_newtable(L);
	{
		/* Add 'vr' to the environment */
		vrrp_lua_pushvrouter(L, vr);
		lua_setfield(L, -2, "vr");

		/* Add 'obj' to the environment */
		zlog_debug(VRRP_LOGPFX "pushing tracked_object");
		objtrack_pushobject(L, obj);
		lua_setfield(L, -2, "obj");
	}
	const char *uvname = lua_setupvalue(L, -2, 1);

	/* Make sure we did that right */
	assert(!strncmp(uvname, "_ENV", strlen("_ENV")));
	assert(lua_isfunction(L, -1));

	/* Push args */
	vrrp_tracking_getregtable(L, vr, obj->name);
	luaL_getsubtable(L, -1, "args");
	int len = luaL_len(L, -1);
	for (int i = 1; i <= len; i++) {
		lua_pushinteger(L, i);
		lua_gettable(L, -(i + 1));
	}
	/* Remove args table & vrouter table */
	lua_remove(L, -(len + 1));
	lua_remove(L, -(len + 1));

	/*
	 * Stack is now:
	 *
	 * argN | -1
	 * ...  | -2
	 * arg1 | -(len)
	 * func | -(len + 1)
	 */
	assert(lua_isfunction(L, -(len + 1)));

	/* Call handler */
	zlog_info(VRRP_LOGPFX VRRP_LOGPFX_VRID
		  "Calling handler chunk with %d arguments",
		  vr->vrid, len);
	err = lua_pcall(L, len, 0, 0);

	return err;
}

/* Tracking API ------------------------------------------------------------ */

void vrrp_tracking_event(struct object *obj)
{
	struct listnode *ln;
	struct vrrp_vrouter *vr;
	struct list *tracklist;
	int err;
	const char *errstring;

	tracklist = vrrp_tracking_get_vrs(obj->name);

	for (ALL_LIST_ELEMENTS_RO(tracklist, ln, vr)) {
		err = vrrp_tracking_handle(L, obj, vr);
		errstring = NULL;

		switch (err) {
		case LUA_OK:
			zlog_warn("Okay");
			errstring = lua_tostring(L, -1);
			break;
		case LUA_ERRRUN:
		case LUA_ERRMEM:
		case LUA_ERRERR:
		case LUA_ERRGCMM:
		default:
			errstring = lua_tostring(L, -1);
			zlog_warn(VRRP_LOGPFX "Call failed: %s", errstring);
			/* Error string cannot be used after this point! */
			lua_pop(L, 1);
			break;
		}
	}
}

void vrrp_track_object(struct vrrp_vrouter *vr, const char *name,
		       enum vrrp_tracking_actiontype actiontype,
		       const void *actionarg)
{
	vrrp_tracking_add_object(vr, name);
	vrrp_tracking_set_action(vr, name, actiontype, actionarg);

	objtrack_track(name, vrrp_tracking_event);
}

void vrrp_untrack_object(struct vrrp_vrouter *vr, const char *name)
{
	vrrp_tracking_remove_object(vr, name);

	objtrack_track(name, NULL);
}

void vrrp_tracking_init(char *script)
{
	objtrack_init(master);

	vrrp_objvr_hash = hash_create(vrrp_objvr_hash_key, vrrp_objvr_hash_cmp,
				      "VRRP reverse object tracking table");
	vrrp_vrobj_hash = hash_create(vrrp_vrobj_hash_key, vrrp_vrobj_hash_cmp,
				      "VRRP object tracking table");

	L = frrlua_initialize(script);

	zlog_notice("Initialized VRRP object tracking");
}
