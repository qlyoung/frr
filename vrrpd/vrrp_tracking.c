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

#include "vrrp.h"
#include "vrrp_debug.h"
#include "vrrp_tracking.h"

#define VRRP_LOGPFX "[TRACK] "

enum tracked_object_type {
	TRACKED_INTERFACE,
	TRACKED_ROUTE,
	TRACKED_IPSLA,
};

struct tracked_object {
	enum tracked_object_type type;
	union {
		struct interface *ifp;
	} obj;
};

struct tracked_instance {
	struct tracked_object obj;
	struct vrrp_vrouter *vr;
};

char script[MAXPATHLEN];
static lua_State *L;

static void vrrp_handle_lua_err(lua_State *L, int err) {
	if (err) {
		const char *errstring = lua_tostring(L, -1);
		fprintf(stderr, "Error: %d - %s\n", err, errstring);
	}
	else {
		fprintf(stderr, "Ok\n");
	}
}

/* Some object event has occurred; handle it */
static void vrrp_ot_handle(struct tracked_instance *ti);

void vrrp_tracking_init(void)
{
	L = frrlua_initialize(script);

	if (!L)
		return;

	struct tracked_instance ti = {
		.obj = {
			.type = TRACKED_INTERFACE,
		}
	};
	vrrp_ot_handle(&ti);
#if 0
	int err = 0;

	fprintf(stderr, "Script file: %s\n", script);

	err = luaL_loadfile(L, script);
	vrrp_handle_lua_err(L, err);
	err = lua_pcall(L, 0, 0, 0);
	vrrp_handle_lua_err(L, err);
#endif

}

/*
 * Called from Lua to set priority of a VRRP instance.
 */
static int vrrp_ot_vr_set_priority(lua_State *L)
{
	zlog_err("%s called with %d arguments", __func__, lua_gettop(L));

	struct tracked_instance *ti = (*(void**) lua_touserdata(L, -2));
	int prio = lua_tointeger(L, -1);

	zlog_err("ti: %p", ti);
	zlog_err("Tracking type: %d", ti->obj.type);
	zlog_err("priority = %d", prio);

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
 * The Lua function names we will call for different object types.
 */
const char *vrrp_ot_handlers[] = {
	[TRACKED_INTERFACE] = "vrrp_ot_interface",
	[TRACKED_ROUTE] = "vrrp_ot_route",
	[TRACKED_IPSLA] = "vrrp_ot_ipsla",
};

/*
 * The metatable for VRRP instances that we pass to the Lua handlers.
 */
static const luaL_Reg vr_funcs[] __attribute__((unused)) = {
	{"set_priority", vrrp_ot_vr_set_priority},
	{"set_state", vrrp_ot_vr_set_state},
	{},
};

/* Some object event has occurred; handle it */
static void __attribute__((unused)) vrrp_ot_handle(struct tracked_instance *ti)
{
	int err;

	/* Setup metatable */
	if (luaL_newmetatable(L, "vr_metatable") == 1) {
		luaL_setfuncs(L, vr_funcs, 0);
		/* Use the new metatable as its own __index table */
		lua_pushstring(L, "__index");
      		lua_pushvalue(L, -2);  /* pushes the metatable */
        	lua_settable(L, -3);  /* metatable.__index = metatable */
	}

	/* Push Lua handler for this object type */
	lua_getglobal(L, vrrp_ot_handlers[ti->obj.type]);

	/* Push ti */
	void *ptrdata = lua_newuserdata(L, sizeof(ti));
	memcpy(ptrdata, &ti, sizeof(ti));

	zlog_err("ti: %p", ti);
	zlog_err("ptrdata: %p", *((void**) ptrdata));

	/* Set metatable of ti to vr_metatable */
	luaL_setmetatable(L, "vr_metatable");

	/* Call handler */
	err = lua_pcall(L, 1, 0, 0);
	vrrp_handle_lua_err(L, err);

	const char *result = lua_tostring(L, -1);
	fprintf(stderr, "result: %s\n", result);
}
