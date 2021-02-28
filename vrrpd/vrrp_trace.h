/* Tracing for VRRP
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

#if !defined(_VRRP_TRACE_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _VRRP_TRACE_H

#include "lib/trace.h"

#ifdef HAVE_LTTNG

#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER frr_bgp

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "vrrpd/vrrp_trace.h"

#include <lttng/tracepoint.h>

#include "vrrpd/vrrp.h"

/* clang-format off */

TRACEPOINT_EVENT_CLASS(
	frr_vrrp,
	vrrp_vrouter_create,
	TP_ARGS(struct vrrp_vrouter *, vr)
	TP_FIELDS(
		ctf_string(peer, ifp->name)
		ctf_integer(uint8_t, vrid, vr->vrid)
		ctf_integer(uint8_t, version, vr->version)
		ctf_integer(uint8_t, priority, vr->priority)
		ctf_integer(uint8_t, preempt_mode, vr->preempt_mode)
		ctf_integer(uint8_t, accept_mode, vr->accept_mode)
		ctf_integer(uint8_t, shutdown, vr->shutdown)
		ctf_integer(uint8_t, advertisement_interval, vr->advertisement_interval)
	)
)

#define PKT_PROCESS_TRACEPOINT_INSTANCE(name)                                  \
	TRACEPOINT_EVENT_INSTANCE(                                             \
		frr_bgp, packet_process, name,                                 \
		TP_ARGS(struct peer *, peer, bgp_size_t, size))                \
	TRACEPOINT_LOGLEVEL(frr_bgp, name, TRACE_INFO)

PKT_PROCESS_TRACEPOINT_INSTANCE(open_process)
PKT_PROCESS_TRACEPOINT_INSTANCE(keepalive_process)
PKT_PROCESS_TRACEPOINT_INSTANCE(update_process)
PKT_PROCESS_TRACEPOINT_INSTANCE(notification_process)
PKT_PROCESS_TRACEPOINT_INSTANCE(capability_process)
PKT_PROCESS_TRACEPOINT_INSTANCE(refresh_process)

TRACEPOINT_EVENT(
	frr_bgp,
	packet_read,
	TP_ARGS(struct peer *, peer, struct stream *, pkt),
	TP_FIELDS(
		ctf_string(peer, PEER_HOSTNAME(peer))
		ctf_sequence_hex(uint8_t, packet, pkt->data, size_t,
				 STREAM_READABLE(pkt))
	)
)

TRACEPOINT_LOGLEVEL(frr_bgp, packet_read, TRACE_INFO)

/* clang-format on */

#include <lttng/tracepoint-event.h>

#endif /* HAVE_LTTNG */

#endif /* _VRRP_TRACE_H */
