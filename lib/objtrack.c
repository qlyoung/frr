/*
 * Object tracking.
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

#include "log.h"
#include "frrlua.h"
#include "frrstr.h"
#include "hash.h"
#include "jhash.h"
#include "vrf.h"
#include "linklist.h"
#include "termtable.h"
#include "command.h"
#include "frr_pthread.h"

#ifndef VTYSH_EXTRACT_PL
#include "objtrack_clippy.c"
#endif

#include "objtrack.h"

#define OBJTRACK_STR "Object tracking\n"
#define OBJTRACK_LOGPFX "[OBJTRACK] "

/* Directory containing tracking scripts */
char luadir[MAXPATHLEN];

/* Lua state used for running tracking scripts */
struct lua_State *L;

/* Object tracking task, if any */
struct thread *t_objtrack;

/* Interval to re-run task at */
long objtrack_interval;

/* Hash of tracked objects */
struct hash *objhash;
pthread_mutex_t objhash_mtx = PTHREAD_MUTEX_INITIALIZER;

static unsigned int objhash_key(const void *data)
{
	const struct object *obj = data;

	return string_hash_make(obj->name);
}

static bool objhash_cmp(const void *data1, const void *data2)
{
	const struct object *obj1 = data1;
	const struct object *obj2 = data2;

	return strmatch(obj1->name, obj2->name);
}

static void *objhash_alloc(void *data)
{
	const struct object *obj = data;

	struct object *newobj = XCALLOC(MTYPE_TMP, sizeof(struct object));
	memcpy(newobj, obj, sizeof(struct object));

	zlog_warn(OBJTRACK_LOGPFX
		  "Created new object: %s (type '%s' | state '%s')",
		  obj->name, obj->type, obj->state);

	return newobj;
}


/*
 * Update objhash with the latest results from running our tracking scripts.
 *
 * This function expects that the top of the Lua stack contains a contiguous
 * array, i.e. one without any nil entries between array elements. Each array
 * element should be a table in the following form:
 *
 * {
 *    "name" = object name,
 *    "type" = object type,
 *    "state" = object state
 * }
 *
 * Each of these tables will be used to either create or update the
 * corresponding struct object in the objhash.
 */
static void objtrack_update_objhash(void)
{
	pthread_mutex_lock(&objhash_mtx);

	struct object obj = {};
	struct object *o;
	const char *name, *type, *state;

	assert(lua_istable(L, -1));

	int len = luaL_len(L, -1);
	for (int i = 1; i <= len; i++) {
		lua_geti(L, -1, i);
		assert(lua_istable(L, -1));
		lua_getfield(L, -1, "name");
		lua_getfield(L, -2, "type");
		lua_getfield(L, -3, "state");
		name = lua_tostring(L, -3);
		type = lua_tostring(L, -2);
		state = lua_tostring(L, -1);
		lua_pop(L, 4);

		strlcpy(obj.name, name, sizeof(obj.name));

		o = hash_get(objhash, &obj, &objhash_alloc);

		zlog_debug(OBJTRACK_LOGPFX "Updating object '%s' (type '%s')",
			   o->name, o->type);

		zlog_debug(
			OBJTRACK_LOGPFX
			"Old object: { 'name' = '%s', 'type' = '%s', 'state' = '%s' }",
			o->name, o->type, o->state);

		zlog_debug(
			OBJTRACK_LOGPFX
			"New object: { 'name' = '%s', 'type' = '%s', 'state' = '%s' }",
			name, type, state);

		strlcpy(o->type, type, sizeof(o->type));

		if (!strmatch(o->state, state)) {
			strlcpy(o->state, state, sizeof(o->state));
			zlog_debug(
				OBJTRACK_LOGPFX
				"State of object '%s' changed, calling handler %p",
				o->name, o->cb);

			/*
			 * XXX: We need to schedule an event instead of calling
			 * this directly, since we are in another pthread
			 */
			if (o->cb)
				o->cb(o);
		}
	}

	pthread_mutex_unlock(&objhash_mtx);
}

/* Utilities --------------------------------------------------------------- */

/*
 * Sets the _ENV for the function at the given index.
 *
 * The object tracking Lua state has the usual set of Lua libraries preloaded
 * into the global environment. However, we want to restrict the user script to
 * a small subset of this functionality, operating under the reasoning that we
 * shouldn't provide anything more than what the scripts need to do their job.
 * For object tracking, user scripts should be allowed to call a system shell -
 * they'll need this to collect information on arbitrary objects they want to
 * track. We'll also give them access to zlog.
 */
static void objtrack_set_env(lua_State *L, int index)
{
	/* Create a clean environment table for the chunk */
#if 0
	lua_newtable(L);
	{
		/* allow os library */
		lua_getglobal(L, "os");
		lua_setfield(L, -2, "os");

		/* add logging functions */
	}

	/* We pushed a table, so adjust index */
	if (index < 0)
		--index;
#endif

	const char *uvname = lua_getupvalue(L, index, 1);
	zlog_debug("Got upvalue: %s", uvname);
	zlog_debug("%s", frrlua_stackdump(L));
	assert(strmatch(uvname, "_ENV"));
	zlog_debug("Exporting logging");
	frrlua_export_logging(L);
	zlog_debug("%s", frrlua_stackdump(L));
	lua_pop(L, 1);
	zlog_debug("%s", frrlua_stackdump(L));
}

static int objtrack_run(struct thread *thread);

static void *objtrack_run_start(void *arg)
{
	DIR *d;
	struct dirent *dir;
	char fullpath[MAXPATHLEN];
	struct frr_pthread *fpt = arg;
	struct thread_master *m = fpt->data;

	d = opendir(luadir);

	if (!d)
		return NULL;

	while ((dir = readdir(d)) != NULL) {
		if (frrstr_endswith(dir->d_name, ".lua")) {
			snprintf(fullpath, sizeof(fullpath), "%s/%s", luadir,
				 dir->d_name);
			zlog_debug(OBJTRACK_LOGPFX "Loading script %s",
				   fullpath);

			if (luaL_loadfile(L, fullpath) != LUA_OK) {
				zlog_debug(OBJTRACK_LOGPFX
					   "Failed to load %s; skipping",
					   fullpath);
				continue;
			}

			zlog_debug(OBJTRACK_LOGPFX
				   "Loading environment for script");
			objtrack_set_env(L, -1);
			assert(lua_isfunction(L, -1));

			if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
				zlog_debug(OBJTRACK_LOGPFX "Call failed: %s",
					   lua_tostring(L, -1));
			}
			if (lua_istable(L, -1)) {
				objtrack_update_objhash();
			} else {
				zlog_debug(
					OBJTRACK_LOGPFX
					"Return value from '%s' was not a table",
					fullpath);
			}
		};
	}
	closedir(d);

	/* Reschedule self */
	thread_add_timer_msec(m, objtrack_run, NULL, objtrack_interval,
			      &t_objtrack);


	return NULL;
}

static int objtrack_run_stop(struct frr_pthread *fpt, void **ret)
{
	return 0;
};

/*
 * Get a list of Lua scripts in the luadir, and run each of them with the
 * object tracking environment.
 *
 * We expect an array of object tables as the sole return value.
 */
static int objtrack_run(struct thread *thread)
{
	struct frr_pthread_attr attr = {
		.start = objtrack_run_start,
		.stop = objtrack_run_stop,
	};

	static struct frr_pthread *fpt;

	fpt = frr_pthread_new(&attr, "Object tracking script runner", "objtrack");
	fpt->data = thread->master;
	fprintf(stderr, "Running thread...\n");
	frr_pthread_run(fpt, NULL);

	return 0;
}

struct object *objtrack_lookup(const char *name)
{
	struct object obj = {};
	void *result;

	strlcpy(obj.name, name, sizeof(obj.name));

	pthread_mutex_lock(&objhash_mtx);
	{
		result = hash_lookup(objhash, &obj);
	}
	pthread_mutex_unlock(&objhash_mtx);

	return result;
}

void objtrack_track(const char *name, void (*cb)(struct object *))
{
	struct object obj = {};
	struct object *v;

	strlcpy(obj.name, name, sizeof(obj.name));

	pthread_mutex_lock(&objhash_mtx);
	{
		v = hash_get(objhash, &obj, objhash_alloc);
	}
	pthread_mutex_unlock(&objhash_mtx);

	v->cb = cb;
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

DEFPY(objtrack_show_tracking_objects,
      objtrack_show_tracking_objects_cmd,
      "show tracking objects",
      SHOW_STR
      OBJTRACK_STR
      "Show tracked objects\n")
{
	/* XXX: unsychronized access to objhash */
	struct list *objects = hash_to_list(objhash);
	struct listnode *ln;
	struct object *obj;

	struct ttable *tt = ttable_new(&ttable_styles[TTSTYLE_BLANK]);

	ttable_add_row(tt, "%s|%s|%s", "Type", "Name", "State");

	for (ALL_LIST_ELEMENTS_RO(objects, ln, obj)) {
		ttable_add_row(tt, "%s|%s|%s", obj->type, obj->name,
			       obj->state);
	}

	ttable_rowseps(tt, 0, BOTTOM, true, '-');
	char *dump = ttable_dump(tt, "\n");

	vty_out(vty, "\n%s\n", dump);

	XFREE(MTYPE_TMP, dump);

	ttable_del(tt);
	list_delete(&objects);

	return CMD_SUCCESS;
}

void objtrack_start(struct thread_master *master, long interval)
{
	objtrack_interval = interval;

	/*
	 * This is the only time t_objtrack is modified from the main thread.
	 * All subsequent times the lua script runner thread is responsible for
	 * rescheduling itself.
	 */
	thread_add_timer_msec(master, objtrack_run, NULL, interval,
			      &t_objtrack);
}

void objtrack_stop()
{
	THREAD_OFF(t_objtrack);
}

void objtrack_init()
{
	L = luaL_newstate();

	luaL_openlibs(L);

	frr_pthread_init();

	strlcpy(luadir, "/etc/frr/lua", sizeof(luadir));
	zlog_warn(OBJTRACK_LOGPFX "Using script directory '%s'", luadir);

	install_element(VIEW_NODE, &objtrack_show_tracking_objects_cmd);

	objhash = hash_create(objhash_key, objhash_cmp, "Object hash");

	zlog_warn(OBJTRACK_LOGPFX "Initialized object tracking");
}
