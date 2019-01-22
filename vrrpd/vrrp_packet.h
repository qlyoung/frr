/*
 * VRRPD packet crafting
 * Copyright (C) 2018 Cumulus Networks, Inc.
 *               Quentin Young
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

#include "memory.h"
#include "ipaddr.h"
#include "prefix.h"

#define VRRP_VERSION 3
#define VRRP_TYPE_ADVERTISEMENT 1

extern const char *vrrp_packet_names[16];

/*
 * Shared header for VRRPv2/v3 packets.
 */
struct vrrp_hdr {
	/*
	 * H  L H  L
	 * 0000 0000
	 * ver  type
	 */
	uint8_t vertype;
	uint8_t vrid;
	uint8_t priority;
	uint8_t naddr;
	union {
		struct {
			uint8_t auth_type;
			/* advertisement interval (in sec) */
			uint8_t adver_int;
		} v2;
		struct {
			/*
			 * advertisement interval (in centiseconds)
			 * H  L H          L
			 * 0000 000000000000
			 * rsvd adver_int
			 */
			uint16_t adver_int;
		} v3;
	};
	uint16_t chksum;
} __attribute__((packed));

#define VRRP_HDR_SIZE sizeof(struct vrrp_hdr)

struct vrrp_pkt {
	struct vrrp_hdr hdr;
	/*
	 * When used, this is actually an array of one or the other, not an
	 * array of union. If N v4 addresses are stored then
	 * sizeof(addrs) == N * sizeof(struct in_addr).
	 */
	union {
		struct in_addr v4;
		struct in6_addr v6;
	} addrs[];
} __attribute__((packed));

#define VRRP_PKT_SIZE(_f, _naddr)                                              \
	({                                                                     \
		size_t _asz = ((_f) == AF_INET) ? sizeof(struct in_addr)       \
						: sizeof(struct in6_addr);     \
		sizeof(struct vrrp_hdr) + (_asz * (_naddr));                   \
	})

#define VRRP_MIN_PKT_SIZE_V4 VRRP_PKT_SIZE(AF_INET, 1)
#define VRRP_MAX_PKT_SIZE_V4 VRRP_PKT_SIZE(AF_INET, 255)
#define VRRP_MIN_PKT_SIZE_V6 VRRP_PKT_SIZE(AF_INET6, 1)
#define VRRP_MAX_PKT_SIZE_V6 VRRP_PKT_SIZE(AF_INET6, 255)

#define VRRP_MIN_PKT_SIZE VRRP_MIN_PKT_SIZE_V4
#define VRRP_MAX_PKT_SIZE VRRP_MAX_PKT_SIZE_V6

/*
 * Builds a VRRP packet.
 *
 * pkt
 *    Pointer to store pointer to result buffer in
 *
 * vrid
 *    Virtual Router Identifier
 *
 * prio
 *    Virtual Router Priority
 *
 * max_adver_int
 *    time between ADVERTISEMENTs
 *
 * v6
 *    whether 'ips' is an array of v4 or v6 addresses
 *
 * numip
 *    number of IPvX addresses in 'ips'
 *
 * ips
 *    array of pointer to either struct in_addr (v6 = false) or struct in6_addr
 *    (v6 = true)
 */
ssize_t vrrp_pkt_build(struct vrrp_pkt **pkt, struct ipaddr *src, uint8_t vrid,
		       uint8_t prio, uint16_t max_adver_int, uint8_t numip,
		       struct ipaddr **ips);

/*
 * Dumps a VRRP packet to a string.
 *
 * Currently only dumps the header.
 *
 * buf
 *    Buffer to store string representation
 *
 * buflen
 *    Size of buf
 *
 * pkt
 *    Packet to dump to a string
 *
 * Returns:
 *    # bytes written to buf
 */
size_t vrrp_pkt_dump(char *buf, size_t buflen, struct vrrp_pkt *pkt);


/*
 * Parses a VRRP packet, checking for illegal or invalid data.
 *
 * This function does not check that the local router is not the IPvX owner for
 * the addresses received; that should be done by the caller.
 *
 * family
 *    Address family of received packet
 *
 * m
 *    msghdr containing results of recvmsg() on VRRP router socket
 *
 * read
 *    return value of recvmsg() on VRRP router socket; must be non-negative
 *
 * pkt
 *    Pointer to pointer to set to location of VRRP packet within buf
 *
 * errmsg
 *    Buffer to store human-readable error message in case of error; may be
 *    NULL, in which case no message will be stored
 *
 * errmsg_len
 *    Size of errmsg
 *
 * Returns:
 *    Size of VRRP packet, or -1 upon error
 */
ssize_t vrrp_parse_datagram(int family, struct msghdr *m, size_t read,
			    struct vrrp_pkt **pkt, char *errmsg,
			    size_t errmsg_len);
