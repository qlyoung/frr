/* mlag generic code.
 * Copyright (C) 2018 Cumulus Networks, Inc.
 *                    Donald Sharp
 *
 * This file is part of FRR.
 *
 * FRR is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * FRR is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FRR; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */
#include <zebra.h>

#include <mlag.h>

char *mlag_role2str(enum mlag_role role, char *buf, size_t size)
{
	switch (role) {
	case MLAG_ROLE_NONE:
		snprintf(buf, size, "NONE");
		break;
	case MLAG_ROLE_PRIMARY:
		snprintf(buf, size, "PRIMARY");
		break;
	case MLAG_ROLE_SECONDARY:
		snprintf(buf, size, "SECONDARY");
		break;
	}

	return buf;
}

char *zebra_mlag_lib_msgid_to_str(enum mlag_msg_type msg_type, char *buf,
				  size_t size)
{
	switch (msg_type) {
	case MLAG_REGISTER:
		snprintf(buf, size, "Register");
		break;
	case MLAG_DEREGISTER:
		snprintf(buf, size, "De-Register");
		break;
	case MLAG_MROUTE_ADD:
		snprintf(buf, size, "Mroute add");
		break;
	case MLAG_MROUTE_DEL:
		snprintf(buf, size, "Mroute del");
		break;
	case MLAG_DUMP:
		snprintf(buf, size, "Mlag Replay");
		break;
	case MLAG_MROUTE_ADD_BULK:
		snprintf(buf, size, "Mroute Add Batch");
		break;
	case MLAG_MROUTE_DEL_BULK:
		snprintf(buf, size, "Mroute Del Batch");
		break;
	case MLAG_PIM_STATUS_UPDATE:
		snprintf(buf, size, "Mlag PIM Status");
		break;
	default:
		snprintf(buf, size, "Unknown");
		break;
	}
	return buf;
}


int zebra_mlag_lib_decode_mlag_hdr(struct stream *s, struct mlag_msg *msg)
{
	if (s == NULL || msg == NULL)
		return (-1);

	STREAM_GETL(s, msg->msg_type);
	STREAM_GETW(s, msg->data_len);
	STREAM_GETW(s, msg->msg_cnt);
	return (0);
stream_failure:
	return (-1);
}

int zebra_mlag_lib_decode_mroute_add(struct stream *s,
				     struct mlag_mroute_add *msg)
{
	if (s == NULL || msg == NULL)
		return (-1);

	STREAM_GET(msg->vrf_name, s, VRF_NAMSIZ);
	STREAM_GETL(s, msg->source_ip);
	STREAM_GETL(s, msg->group_ip);
	STREAM_GETL(s, msg->cost_to_rp);
	STREAM_GETL(s, msg->vni_id);
	STREAM_GETC(s, msg->am_i_dr);
	STREAM_GETC(s, msg->am_i_dual_active);
	STREAM_GETL(s, msg->vrf_id);
	STREAM_GET(msg->intf_name, s, INTERFACE_NAMSIZ);
	return (0);
stream_failure:
	return (-1);
}

int zebra_mlag_lib_decode_mroute_del(struct stream *s,
				     struct mlag_mroute_del *msg)
{
	if (s == NULL || msg == NULL)
		return (-1);

	STREAM_GET(msg->vrf_name, s, VRF_NAMSIZ);
	STREAM_GETL(s, msg->source_ip);
	STREAM_GETL(s, msg->group_ip);
	STREAM_GETL(s, msg->vni_id);
	STREAM_GETL(s, msg->vrf_id);
	STREAM_GET(msg->intf_name, s, INTERFACE_NAMSIZ);
	return (0);
stream_failure:
	return (-1);
}

int zebra_mlag_lib_decode_pim_status(struct stream *s,
				     struct mlag_pim_status *msg)
{
	if (s == NULL || msg == NULL)
		return (-1);

	STREAM_GETL(s, msg->switchd_state);
	STREAM_GETL(s, msg->svi_state);
	return (0);
stream_failure:
	return (-1);
}

int zebra_mlag_lib_decode_mlag_status(struct stream *s, struct mlag_status *msg)
{
	if (s == NULL || msg == NULL)
		return (-1);

	STREAM_GET(msg->peerlink_name, s, VRF_NAMSIZ);
	STREAM_GETL(s, msg->my_role);
	STREAM_GETL(s, msg->peer_state);
	STREAM_GETL(s, msg->anycast_ip);
	return (0);
stream_failure:
	return (-1);
}
