/*
 * Objtrack Zebra interfacing.
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

#include "lib/zclient.h"

#include "objtrack.h"
#include "objtrack_zebra.h"

#define OBJTRACK_LOGPFX "[ZEBRA] "

/* FIXME */
#define ZEBRA_ROUTE_OBJTRACK 255

static struct zclient *zclient;

void objtrack_zebra_init(void)
{
	/* Socket for receiving updates from Zebra daemon */
	zclient = zclient_new(master, &zclient_options_default);

	zclient_init(zclient, ZEBRA_ROUTE_OBJTRACK, 0, &objtrack_privs);

	zlog_notice("%s: zclient socket initialized", __func__);
}
