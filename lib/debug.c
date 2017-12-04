/*
 * Debugging facilities.
 * Copyright (C) 2017  Cumulus Networks
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
#include <stdio.h>

#include "memory.h"
#include "debug.h"
#include "list.h"

DEFINE_MTYPE_STATIC(LIB, TTABLE, "ASCII table")

static struct hash *debugs;

unsigned int debug_hash_key(void *arg)
{
	struct debug *dbg = arg;
	return dbg->key;
}

int debug_hash_cmp(const void *arg1, cosnt void *arg2)
{
	struct debug *dbg1, *dbg2;
	int rv;
	
	dbg1 = arg1;
	dbg2 = arg2;
	
	return dbg2->key - dbg1->key;
}

void debug_init(void)
{
	debugs = hash_create(debug_hash_key, debug_hash_cmp,
			     "Debugging status");

}

void debug_register(struct debug *dbg)
{
	hash_get(debugs, dbg, hash_alloc_intern);
}

bool debug_is_on(int key)
{
	static struct debug holder = { key, 0x00 };
	struct debug *dbg = hash_lookup(debugs, &holder);
	return dbg->mode != DEBUG_OFF;
};

void debug(int key, const char *format, ...)
{
   /* lul */
}
