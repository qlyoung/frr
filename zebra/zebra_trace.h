/* Tracing for Zebra
 *
 * Copyright (C) 2021  NVIDIA Corporation
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

#if !defined(_ZEBRA_TRACE_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _ZEBRA_TRACE_H

#include "lib/trace.h"

#ifdef HAVE_LTTNG

#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER frr_zebra

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "zebra/zebra_trace.h"

#include "lib/table.h"
#include <lttng/tracepoint.h>

/* clang-format off */

TRACEPOINT_EVENT(
	frr_zebra,
	route_install_queued,
	TP_ARGS(char *, prefix),
	TP_FIELDS(
		ctf_string(prefix, prefix)
	)
)

TRACEPOINT_LOGLEVEL(frr_zebra, route_install, TRACE_INFO)

/* clang-format on */

#include <lttng/tracepoint-event.h>

#endif /* HAVE_LTTNG */

#endif /* _ZEBRA_TRACE_H */
