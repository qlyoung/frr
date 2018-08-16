/*
 * BGP-specific error messages.
 * Copyright (C) 2018 Cumulus Networks, Inc.
 *               Don Slice
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

#include "lib/ferr.h"
#include "bgp_errors.h"

/* clang-format off */
static struct log_ref ferr_bgp_warn[] = {
	{
		.code = BGP_WARN_ASPATH_FEWER_HOPS,
		.title = "BGP AS-path conversion has failed",
		.description = "BGP has attempted to convert a AS2 to AS4 path and has failed",
		.suggestion = "Open an Issue with all relevant log files and restart FRR"
	},
	{
		.code = BGP_WARN_DEFUNCT_SNPA_LEN,
		.title = "BGP has received a value in a reserved field",
		.description = "BGP has received a non-zero value in a reserved field that was used for SNPA-length at one point in time",
		.suggestion = "BGP has peered with either a router that is attempting to send SNPA data or it has received a corrupted packet.  If we are peering with a SNPA aware router(unlikely) upgrade that router, else open an Issue after gathering relevant log files",
	},
	{
		.code = BGP_WARN_MISSING_ATTRIBUTE,
		.title = "BGP has received an update with missing a missing attribute",
		.description = "BGP received update packets must have some minimum attribute information within them",
		.suggestion = "Gather log data from this and remote peer and open an Issue with this data",
	},
	{
		.code = BGP_WARN_ATTRIBUTE_TOO_SMALL,
		.title = "BGP udate packet with attribute data that is too small",
		.description = "BGP has received an update packet that is too small to parse a given attribute.  This typically means that something has gone wrong between us and the remote peer",
		.suggestion = "Gather log data from this and remote peer and open an Issue with this data",
	},
	{
		.code = BGP_WARN_EXT_ATTRIBUTE_TOO_SMALL,
		.title = "BGP udate packet with extended attribute data that is too small",
		.description = "BGP has received an update packet that is too small to parse a given extended attribute.  This typically means that something has gone wrong between us and the remote peer",
		.suggestion = "Gather log data from this and remote peer and open an Issue with this data",
	},
	{
		.code = BGP_WARN_ATTRIBUTE_REPEATED,
		.title = "BGP update packet received with a repeated attribute",
		.description = "BGP has received an update packet with a attribute that is repeated more than one time for a particular route.  This typically means that something has gone wrong between us and the remote peer",
		.suggestion = "Gather log data from this and remote peer and open an Issue with this data",
	},
	{
		.code = BGP_WARN_ATTRIBUTE_TOO_LARGE,
		.title = "BGP udate packet with attribute data that is too large",
		.description = "BGP has received an update packet that has too much data in a particular attribute.  This typically means that something has gone wrong between us and the remote peer",
		.suggestion = "Gather log data from this and remote peer and open an Issue with this data",
	},
	{
		.code = BGP_WARN_ATTRIBUTE_PARSE_ERROR,
		.title = "BGP update packet with attribute data has a parse error, specific to the attribute",
		.description = "BGP has received an update packet with an attribute that when parsed does not make sense in some manner",
		.suggestion = "Gather log data from this and remote peer and open an Issue with this data",
	},
	{
		.code = BGP_WARN_ATTRIBUTE_PARSE_WITHDRAW,
		.title = "BGP update packet with a broken optional attribute has caused a withdraw of associated routes",
		.description = "BGP has received a update packet with optional attributes that did not parse correctly, instead of resetting the peer, withdraw associated routes and note that this has happened",
		.suggestion = "Gather log data from this and remote peer and open an Issue with this data",
	},
	{
		.code = BGP_WARN_ATTRIBUTE_FETCH_ERROR,
		.title = "BGP update packet with a broken length",
		.description = "BGP has received a update packet with an attribute that has an incorrect length",
		.suggestion = "Gather log data from this and remote peer and open an Issue with this data",
	},
	{
		.code = BGP_WARN_ATTRIBUTES_MISMATCH,
		.title = "BGP update packet with a length different than attribute data length",
		.description = "BGP has received a update packet with attributes that when parsed do not correctly add up to packet data length",
		.suggestion = "Gather log data from this and remote peer and open an Issue with this data",
	},
	{
		.code = BGP_WARN_DUMP,
		.title = "BGP MRT dump subsystem has encountered an issue",
		.description = "BGP has found that the attempted write of MRT data to a dump file has failed",
		.suggestion = "Ensure BGP has permissions to write the specified file",
	},
	{
		.code = BGP_WARN_UPDATE_PACKET_SHORT,
		.title = "BGP Update Packet is to Small",
		.description = "The update packet received from a peer is to small",
		.suggestion = "Determine the source of the update packet and examine that peer for what has gone wrong",
	},
	{
		.code = BGP_WARN_UPDATE_PACKET_LONG,
		.title = "BGP Update Packet is to large",
		.description = "The update packet received from a peer is to large",
		.suggestion = "Determine the source of the update packet and examine that peer for what has gone wrong",
	},
	{
		.code = BGP_WARN_UNRECOGNIZED_CAPABILITY,
		.title = "Unknown BGP Capability Received",
		.description = "The negotiation of capabilities has received a capability that we do not know what to do with",
		.suggestion = "Determine the source of the capability and remove the capability from what is sent",
	},
	{
		.code = BGP_WARN_NO_TCP_MD5,
		.title = "Unable to set TCP MD5 option on socket",
		.description = "BGP attempted to setup TCP MD5 configuration on the socket as per configuration but was unable to",
		.suggestion = "Please collect log files and open Issue",
	},
	{
		.code = BGP_WARN_NO_SOCKOPT_MARK,
		.title = "Unable to set socket MARK option",
		.description = "BGP attempted to set the SO_MARK option for a socket and was unable to do so",
		.suggestion = "Please collect log files and open Issue",
	},
	{
		.code = BGP_WARN_EVPN_PMSI_PRESENT,
		.title = "BGP Received a EVPN NLRI with PMSI included",
		.description = "BGP has received a type-3 NLRI with PMSI information.  At this time FRR is not capable of properly handling this NLRI type",
		.suggestion = "Setup peer to not send this type of data to FRR"
	},
	{
		.code = BGP_WARN_EVPN_VPN_VNI,
		.title = "BGP has received a local macip and cannot properly handle it",
		.description = "BGP has received a local macip from zebra and has no way to properly handle the macip because the vni is not setup properly",
		.suggestion = "Ensure proper setup of BGP EVPN",
	},
	{
		.code = BGP_WARN_EVPN_ESI,
		.title = "BGP has received a local ESI for deletion",
		.description = "BGP has received a local ESI for deletion but when attempting to find the stored data internally was unable to find the information for deletion",
		.suggestion = "Gather logging and open an Issue",
	},
	{
		.code = BGP_WARN_INVALID_LABEL_STACK,
		.title = "BGP has received a label stack in a NLRI that does not have the BOS marked",
		.description = "BGP when it receives a NLRI with a label stack should have the BOS marked, this received packet does not have this",
		.suggestion = "Gather log information from here and remote peer and open an Issue",
	},
	{
		.code = BGP_WARN_ZEBRA_SEND,
		.title = "BGP has attempted to send data to zebra and has failed to do so",
		.description = "BGP has attempted to send data to zebra but has been unable to do so",
		.suggestion = "Gather log data, open an Issue and restart FRR"
	},
	{
		.code = BGP_WARN_CAPABILITY_INVALID_LENGTH,
		.title = "BGP has received a capability with an invalid length",
		.description = "BGP has received a capability from it's peer who's size is wrong",
		.suggestion = "Gather log files from here and from peer and open an Issue",
	},
	{
		.code = BGP_WARN_CAPABILITY_INVALID_DATA,
		.title = "BGP has received capability data with invalid information",
		.description = "BGP has noticed that during processing of capability information that data was wrong",
		.suggestion = "Gather log files from here and from peer and open an Issue",
	},
	{
		.code = BGP_WARN_CAPABILITY_VENDOR,
		.title = "BGP has recieved capability data specific to a particular vendor",
		.description = "BGP has received a capability that is vendor specific and as such we have no knowledge of how to use this capability in FRR",
		.suggestion = "On peer turn off this feature"
	},
	{
		.code = BGP_WARN_CAPABILITY_UNKNOWN,
		.title = "BGP has received capability data for a unknown capability",
		.description = "BGP has received a capability that it does not know how to decode.  This may be due to a new feature that has not been coded into FRR or it may be a bug in the remote peer",
		.suggestion = "Gather log files from here and from peer and open an Issue",
	},
	{
		.code = END_FERR,
	}
};

static struct log_ref ferr_bgp_err[] = {
	{
		.code = BGP_ERR_ATTR_FLAG,
		.title = "BGP attribute flag is incorrect",
		.description = "BGP attribute flag is set to the wrong value (Optional/Transitive/Partial)",
		.suggestion = "Determine the soure of the attribute and determine why the attribute flag has been set incorrectly"
	},
	{
		.code = BGP_ERR_ATTR_LEN,
		.title = "BGP attribute length is incorrect",
		.description = "BGP attribute length is incorrect",
		.suggestion = "Determine the soure of the attribute and determine why the attribute length has been set incorrectly"
	},
	{
		.code = BGP_ERR_ATTR_ORIGIN,
		.title = "BGP attribute origin value invalid",
		.description = "BGP attribute origin value is invalid",
		.suggestion = "Determine the soure of the attribute and determine why the origin attribute has been set incorrectly"
	},
	{
		.code = BGP_ERR_ATTR_MAL_AS_PATH,
		.title = "BGP as path is invalid",
		.description = "BGP as path has been malformed",
		.suggestion = "Determine the soure of the update and determine why the as path has been set incorrectly"
	},
	{
		.code = BGP_ERR_ATTR_FIRST_AS,
		.title = "BGP as path first as is invalid",
		.description = "BGP update has invalid first as in as path",
		.suggestion = "Determine the soure of the update and determine why the as path first as value has been set incorrectly"
	},
	{
		.code = BGP_ERR_ATTR_PMSI_TYPE,
		.title = "BGP PMSI tunnel attribute type is invalid",
		.description = "BGP update has invalid type for PMSI tunnel",
		.suggestion = "Determine the soure of the update and determine why the PMSI tunnel attribute type has been set incorrectly"
	},
	{
		.code = BGP_ERR_ATTR_PMSI_LEN,
		.title = "BGP PMSI tunnel attribute length is invalid",
		.description = "BGP update has invalid length for PMSI tunnel",
		.suggestion = "Determine the soure of the update and determine why the PMSI tunnel attribute length has been set incorrectly"
	},
	{
		.code = BGP_ERR_PEER_GROUP,
		.title = "BGP peergroup operated on in error",
		.description = "BGP operating on peer-group instead of peers included",
		.suggestion = "Ensure the config doesn't contain peergroups contained within peergroups"
	},
	{
		.code = BGP_ERR_PEER_DELETE,
		.title = "BGP failed to delete peer structure",
		.description = "BGP was unable to delete peer structure when address-family removed",
		.suggestion = "Determine if all expected peers are removed and restart FRR if not. Most likely a bug"
	},
	{
		.code = BGP_ERR_TABLE_CHUNK,
		.title = "BGP failed to get table chunk memory",
		.description = "BGP unable to get chunk memory for table manager",
		.suggestion = "Ensure there is adequate memory on the device to support the table requirements"
	},
	{
		.code = BGP_ERR_MACIP_LEN,
		.title = "BGP received MACIP with invalid IP addr len",
		.description = "BGP received MACIP with invalid IP addr len from Zebra",
		.suggestion = "Verify MACIP entries inserted in Zebra are correct.  Most likely a bug"
	},
	{
		.code = BGP_ERR_LM_ERROR,
		.title = "BGP received invalid label manager message",
		.description = "BGP received nvalid label manager message from label manager",
		.suggestion = "Label manager sent invalid essage to BGP for wrong protocol, instance, etc. Most likely a bug"
	},
	{
		.code = BGP_ERR_JSON_MEM_ERROR,
		.title = "BGP unable to allocate memory for JSON output",
		.description = "BGP attempted to generate JSON output and was unable to allocate the memory required",
		.suggestion = "Ensure that the device has adequate memory to suport the required functions"
	},
	{
		.code = BGP_ERR_UPDGRP_ATTR_LEN,
		.title = "BGP update had attributes too long to send",
		.description = "BGP attempted to send an update but the attributes were too long to fit",
		.suggestion = "Most likely a bug. If the problem persists, report the problem for troubleshooting"
	},
	{
		.code = BGP_ERR_UPDGRP_CREATE,
		.title = "BGP update group creation failed",
		.description = "BGP attempted to create an update group but was unable to",
		.suggestion = "Most likely a bug. If the problem persists, report the problem for troubleshooting"
	},
	{
		.code = BGP_ERR_UPDATE_SND,
		.title = "BGP error creating update packet",
		.description = "BGP attempted to create an update packet but was unable to",
		.suggestion = "Most likely a bug. If the problem persists, report the problem for troubleshooting"
	},
	{
		.code = BGP_ERR_PKT_OPEN,
		.title = "BGP error receiving open packet",
		.description = "BGP received an open from a peer that was invalid",
		.suggestion = "Determine the sending peer and correct his invalid open packet"
	},
	{
		.code = BGP_ERR_SND_FAIL,
		.title = "BGP error sending to peer",
		.description = "BGP attempted to respond to open from a peer and failed",
		.suggestion = "BGP attempted to respond to an open and could not sene the packet. Check local IP address for source"
	},
	{
		.code = BGP_ERR_INVALID_STATUS,
		.title = "BGP error receiving from  peer",
		.description = "BGP received an update from a peer but status was incorrect",
		.suggestion = "Most likely a bug. If the problem persists, report the problem for troubleshooting"
	},
	{
		.code = BGP_ERR_UPDATE_RCV,
		.title = "BGP error receiving update packet",
		.description = "BGP received an invalid update packet",
		.suggestion = "Determine the source of the update and resolve the invalid update being sent"
	},
	{
		.code = BGP_ERR_NO_CAP,
		.title = "BGP error due to capability not enabled",
		.description = "BGP attempted a function that did not have the capability enabled",
		.suggestion = "Enable the capability if this functionality is desired"
	},
	{
		.code = BGP_ERR_NOTIFY_RCV,
		.title = "BGP error receiving notify message",
		.description = "BGP unable to process notification message",
		.suggestion = "BGP notify received while in stopped state. If the problem persists, report for troubleshooting"
	},
	{
		.code = BGP_ERR_KEEP_RCV,
		.title = "BGP error receiving keepalive packet",
		.description = "BGP unable to process keepalive packet",
		.suggestion = "BGP keepalive received while in stopped state. If the problem persists, report for troubleshooting"
	},
	{
		.code = BGP_ERR_RFSH_RCV,
		.title = "BGP error receiving route refresh message",
		.description = "BGP unable to process route refresh message",
		.suggestion = "BGP route refresh received while in stopped state. If the problem persists, report for troubleshooting"},
	{
		.code = BGP_ERR_CAP_RCV,
		.title = "BGP error capability message",
		.description = "BGP unable to process received capability",
		 .suggestion = "BGP capability message received while in stopped state. If the problem persists, report for troubleshooting"
	},
	{
		.code = BGP_ERR_NH_UPD,
		.title = "BGP error with nexthopo update",
		.description = "BGP unable to process nexthop update",
		.suggestion = "BGP received nexthop update but nexthop is not reachable in this bgp instance. Report for troubleshooting"
	},
	{
		.code = BGP_ERR_LABEL,
		.title = "Failure to apply label",
		.description = "BGP attempted to attempted to apply a label but could not",
		.suggestion = "Most likely a bug. If the problem persists, report the problem for troubleshooting"
	},
	{
		.code = BGP_ERR_MULTIPATH,
		.title = "Multipath specified is invalid",
		.description = "BGP was started with an invalid ecmp/multipath value",
		.suggestion = "Correct the ecmp/multipath value supplied when starting the BGP daemon"
	},
	{
		.code = BGP_ERR_PKT_PROCESS,
		.title = "Failure to process a packet",
		.description = "BGP attempted to process a received packet but could not",
		.suggestion = "Most likely a bug. If the problem persists, report the problem for troubleshooting"
	},
	{
		.code = BGP_ERR_CONNECT,
		.title = "Failure to connect to peer",
		.description = "BGP attempted to send open to peer but couldn't connect",
		.suggestion = "Most likely a bug. If the problem persists, report the problem for troubleshooting"
	},
	{
		.code = BGP_ERR_FSM,
		.title = "BGP FSM issue",
		.description = "BGP neighbor transition problem",
		.suggestion = "Most likely a bug. If the problem persists, report the problem for troubleshooting"
	},
	{
		.code = BGP_ERR_VNI,
		.title = "BGP VNI creation issue",
		.description = "BGP could not create a new VNI",
		.suggestion = "Most likely a bug. If the problem persists, report the problem for troubleshooting"
	},
	{
		.code = BGP_ERR_NO_DFLT,
		.title = "BGP default instance missing",
		.description = "BGP could not find default instance",
		.suggestion = "Define a default instance of BGP since some feature requires it's existence"
	},
	{
		.code = BGP_ERR_VTEP_INVALID,
		.title = "BGP remote VTEP invalid",
		.description = "BGP remote VTEP is invalid and cannot be used",
		.suggestion = "Correct remote VTEP configuration or resolve the source of the problem"
	},
	{
		.code = BGP_ERR_ES_INVALID,
		.title = "BGP ES route error",
		.description = "BGP ES route incorrect, learned both local and remote",
		.suggestion = "Correct configuration or addressing so that same not learned both local and remote"
	},
	{
		.code = BGP_ERR_EVPN_ROUTE_DELETE,
		.title = "BGP EVPN route delete error",
		.description = "BGP attempted to delete an EVPN route and failed",
		.suggestion = "Most likely a bug. If the problem persists, report the problem for troubleshooting"
	},
	{
		.code = BGP_ERR_EVPN_FAIL,
		.title = "BGP EVPN install/uninstall error",
		.description = "BGP attempted to install or uninstall an EVPN prefix and failed",
		.suggestion = "Most likely a bug. If the problem persists, report the problem for troubleshooting"
	},
	{
		.code = BGP_ERR_EVPN_ROUTE_INVALID,
		.title = "BGP EVPN route received with invalid contents",
		.description = "BGP received an EVPN route with invalid contents",
		.suggestion = "Determine the source of the EVPN route and resolve whatever is causing invalid contents"
	},
	{
		.code = BGP_ERR_EVPN_ROUTE_CREATE,
		.title = "BGP EVPN route create error",
		.description = "BGP attempted to create an EVPN route and failed",
		.suggestion = "Most likely a bug. If the problem persists, report the problem for troubleshooting"
	},
	{
		.code = BGP_ERR_ES_CREATE,
		.title = "BGP EVPN ES entry create error",
		.description = "BGP attempted to create an EVPN ES entry and failed",
		.suggestion = "Most likely a bug. If the problem persists, report the problem for troubleshooting"
	},
	{
		.code = BGP_ERR_MULTI_INSTANCE,
		.title = "BGP config multi-instance issue",
		.description = "BGP configuration attempting multiple instances without enabling the feature",
		.suggestion = "Correct the configuration so that bgp multiple-instance is enabled if desired"
	},
	{
		.code = BGP_ERR_EVPN_AS_MISMATCH,
		.title = "BGP AS configuration issue",
		.description = "BGP configuration attempted for a different AS than currently configured",
		.suggestion = "Correct the configuration so that the correct BGP AS number is used"
	},
	{
		.code = BGP_ERR_EVPN_INSTANCE_MISMATCH,
		.title = "BGP EVPN AS and process name mismatch",
		.description = "BGP configuration has AS and process name mismatch",
		.suggestion = "Correct the configuration so that the BGP AS number and instance name are consistent"
	},
	{
		.code = END_FERR,
	}
};
/* clang-format on */

void bgp_error_init(void)
{
	log_ref_add(ferr_bgp_warn);
	log_ref_add(ferr_bgp_err);
	log_ref_add(ferr_bgp_warn);
}
