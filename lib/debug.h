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
#include <frratomic.h>

#include "memory.h"

/*
 * Debugging mode.
 *
 * DEBUG_OFF - debug is off
 * DEBUG_CONF - debug is permanently on
 * DEBUG_TERM - debug is on for duration of session
 * DEBUG_ALL - same as DEBUG_CONF
 */
enum debug_mode {
	DEBUG_OFF,
	DEBUG_CONF,
	DEBUG_TERM,
	DEBUG_ALL
};

/*
 * Debugging info.
 */
struct debug {
	/* Unique integer key */
	_Atomic unsigned int key;
	/* mode */
	_Atomic enum debug_mode mode;
	/* human-readable name */
	const char *name;
	/* extra data */
	void *data;
};

/*
 * Initialize debugging facilities.
 * Must be called prior to using any other functions exposed here.
 */
void debug_init(void);

/*
 * Register debugging information.
 * Must be called in order to use other functions that require a debug key.
 *
 * @param dbg - debugging struct to register
 */
void debug_register(struct debug *dbg);

/*
 * Log debugging message.
 *
 * @param key - integral key identifying what debugging information this message is associated with.
 */
void debug(int key, const char *format, ...) PRINTF_ATTRIBUTE(2, 3);

/*
 * Whether or not a particular debug is on.
 *
 * @return on/off
 */
bool debug_is_on(int key);
