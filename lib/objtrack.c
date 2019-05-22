/*
 * Objtrack global definitions and state machine.
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
#include <dirent.h>

#include "lib/log.h"
#include "lib/frrlua.h"
#include "lib/frrstr.h"
#include "lib/hash.h"
#include "lib/jhash.h"
#include "lib/vrf.h"

#include "objtrack.h"

#define OBJTRACK_LOGPFX "[OBJTRACK] "

struct lua_State *L;

char luadir[MAXPATHLEN];

/*
 * We provide an API to lua to call back to us, using us as an information
 * base. objtrackd becomes the server for getting general purpose routing
 * information out of FRR.
 */

/* Tracked object */
struct hash *objhash;

static unsigned int objhash_key(const void *data)
{
	const struct object *obj = data;
	char buf[strlen(obj->type) + 1 + strlen(obj->name) + 1];

	snprintf(buf, sizeof(buf), "%s@%s", obj->type, obj->name);

	return string_hash_make(buf);
}

static bool objhash_cmp(const void *data1, const void *data2)
{
	const struct object *obj1 = data1;
	const struct object *obj2 = data2;

	return strmatch(obj1->type, obj2->type)
	       && strmatch(obj1->name, obj2->name);
}

static void *objhash_alloc(void *data)
{
	const struct object *obj = data;

	struct object *newobj = XCALLOC(MTYPE_TMP, sizeof(struct object));
	memcpy(newobj, obj, sizeof(struct object));

	zlog_warn(OBJTRACK_LOGPFX "Created new object: %s@%s", obj->type, obj->name);

	return newobj;
}

static void objtrack_update_objhash(void)
{
	/* Copy each table element locally */
	struct object obj = {};

	int len = luaL_len(L, -1);
	for (int i = 1; i <= len; i++) {
		lua_geti(L, -1, i);
		assert(lua_istable(L, -1));
		lua_getfield(L, -1, "name");
		lua_getfield(L, -2, "type");
		lua_getfield(L, -3, "state");
		const char *name = lua_tostring(L, -3);
		const char *type = lua_tostring(L, -2);
		const char *state = lua_tostring(L, -1);
		lua_pop(L, 4);

		strlcpy(obj.type, type, sizeof(obj.type));
		strlcpy(obj.name, name, sizeof(obj.name));

		struct object *o = hash_get(objhash, &obj, &objhash_alloc);

		zlog_warn(OBJTRACK_LOGPFX "Found object: %s@%s", o->type,
			  o->name);

		zlog_warn(OBJTRACK_LOGPFX "Old state: %s", o->state);
		zlog_warn(OBJTRACK_LOGPFX "New state: %s", state);

		if (!strmatch(o->state, state)) {
			strlcpy(o->state, state, sizeof(o->state));
			zlog_warn(OBJTRACK_LOGPFX
				  "State changed, calling handler %p", o->cb);
			if (o->cb)
				o->cb(o);
		}
	}
}

/* Lua callbacks ----------------------------------------------------------- */

static int objtrack_get_interfaces(lua_State *L)
{
	struct vrf *vrf = vrf_lookup_by_id(VRF_DEFAULT);
	struct interface *ifp;


	/* Push all interfaces on the stack */
	lua_newtable(L);
	int idx = 0;
	FOR_ALL_INTERFACES(vrf, ifp) {
		frrlua_newtable_interface(L, ifp);
		lua_seti(L, -2, ++idx);
	}

	return 1;
}

/* Utilities --------------------------------------------------------------- */

/*
 * Sets the _ENV for the function at -1 on the stack.
 */
static void objtrack_push_env(lua_State *L)
{
	/* Create a clean environment table for the chunk */
	lua_newtable(L);
	{
		/* allow os library */
		lua_getglobal(L, "os");
		lua_setfield(L, -2, "os");

		/* Add some constants */
		lua_pushinteger(L, IFF_UP);
		lua_setfield(L, -2, "IFF_UP");
		lua_pushinteger(L, IFF_RUNNING);
		lua_setfield(L, -2, "IFF_RUNNING");

		/* add our internal bindings */
		lua_pushcfunction(L, objtrack_get_interfaces);
		lua_setfield(L, -2, "get_interfaces");
	}
	const char *uvname = lua_setupvalue(L, -2, 1);

	zlog_info(OBJTRACK_LOGPFX "uvname: %s", uvname);

	assert(!strncmp(uvname, "_ENV", strlen("_ENV")));
}

static int objtrack_run(struct thread *thread)
{
	DIR *d;
	struct dirent *dir;
	char fullpath[MAXPATHLEN];
	d = opendir(luadir);
	if (d) {
		while ((dir = readdir(d)) != NULL) {
			if (frrstr_endswith(dir->d_name, ".lua")) {
				snprintf(fullpath, sizeof(fullpath), "%s/%s",
					 luadir, dir->d_name);
				zlog_info(OBJTRACK_LOGPFX "Loading script %s",
					  fullpath);
				int err = luaL_loadfile(L, fullpath);
				if (err != LUA_OK) {
					zlog_warn(OBJTRACK_LOGPFX
						  "Failed to load %s",
						  fullpath);
					continue;
				}
				zlog_info(OBJTRACK_LOGPFX
					  "Loading environment for function");
				objtrack_push_env(L);
				assert(lua_isfunction(L, -1));
				/* We expect an array of objects back */
				if (lua_pcall(L, 0, 1, 0) != LUA_OK){
					zlog_warn(OBJTRACK_LOGPFX
						  "Call failed: %s",
						  lua_tostring(L, -1));
				}
				if (lua_istable(L, -1)) {
					/* Update our object hash */
					objtrack_update_objhash();
				} else {
					zlog_warn(OBJTRACK_LOGPFX
						  "Didn't get a table");
				}
			};
		}
		closedir(d);
	}

	thread_add_timer_msec(thread->master, objtrack_run, NULL, 500, NULL);

	return 0;
}

struct object *objtrack_lookup(const char *name)
{
	struct object obj = {};

	strlcpy(obj.name, name, sizeof(obj.name));
	strlcpy(obj.type, "interface", sizeof(obj.type));

	return hash_lookup(objhash, &obj);
}

void objtrack_track(const char *name, void (*cb)(struct object *))
{
	struct object *obj = objtrack_lookup(name);

	obj->cb = cb;
}

void objtrack_pushobject(lua_State *L, const struct object *obj)
{
	lua_newtable(L);
	lua_pushstring(L, obj->type);
	lua_setfield(L, -2, "type");
	lua_pushstring(L, obj->name);
	lua_setfield(L, -2, "name");
	lua_pushstring(L, obj->state);
	lua_setfield(L, -2, "state");
}

void objtrack_pushobject_name(lua_State *L, const char *name)
{
	struct object *val = objtrack_lookup(name);

	if (val)
		objtrack_pushobject(L, val);
}

void objtrack_init(struct thread_master *master)
{
	L = frrlua_initialize(NULL);

	strlcpy(luadir, "/etc/frr/lua", sizeof(luadir));
	zlog_warn(OBJTRACK_LOGPFX "Using script directory '%s'", luadir);

	objhash = hash_create(objhash_key, objhash_cmp, "Object hash");

	thread_add_timer_msec(master, objtrack_run, NULL, 500, NULL);

	zlog_warn(OBJTRACK_LOGPFX "Initialized object tracking");
}
