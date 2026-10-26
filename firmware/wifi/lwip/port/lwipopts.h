/* SPDX-License-Identifier: GPL-3.0-only
 * lwIP 2.1.3's options for fm1wifi_net (../../fm1wifi_net.c, docs/WIFI.md stage 7): one WiFi station interface,
 * IPv4 with ARP, DHCP and ICMP echo, UDP; no TCP, no IPv6, no RTOS. NO_SYS=1 and the raw API: the firmware's main
 * loop hands in received frames (fm1wifi_net_input) and runs the timers (fm1wifi_net_poll), nothing runs from an
 * interrupt. Anything not set here takes lwIP's default from lwip/opt.h.
 *
 * RAM, all static (lwIP takes no memory from anywhere else, no malloc), as JieLi clang lays it out for the FM-1
 * (pi32v2, 32-bit; measured from the objects with nm, README.md here):
 *   MEM_SIZE                 4096 B   lwIP's heap (4,115 B with its headers): PBUF_RAM pbufs. DHCP messages about
 *                                     600 B, ARP 60 B, an ICMP reply when the request has no headroom,
 *                                     fm1wifi_net's copy of a chained frame up to 1,532 B. Two full frames fit.
 *   PBUF_POOL_SIZE x BUFSIZE 4 x 512  received frames (2,115 B): fm1wifi_net_input copies each into pool pbufs, a
 *                                     1,500 B packet takes 3, chained; freed before fm1wifi_net_input returns
 *   MEMP_NUM_PBUF            4        pbuf headers for PBUF_REF/ROM, lwIP's own use (67 B)
 *   MEMP_NUM_UDP_PCB         2        DHCP's and one for the firmware (67 B)
 *   MEMP_NUM_SYS_TIMEOUT     3+1      ARP's and DHCP's two timers (lwIP's count) and one for the firmware (67 B)
 *   ARP_TABLE_SIZE           4        the gateway, the DHCP server and two peers (96 B)
 *   fm1wifi_net.c                     the netif, struct dhcp, counters (168 B)
 * In all 6,839 B of .bss with lwIP's other globals. No TX buffer: a frame goes out from the pbuf lwIP built, in front
 * of which PBUF_LINK_ENCAPSULATION_HLEN leaves room for the 802.11 header.
 */
#pragma once

/* the system: no RTOS, raw API from the main loop */
#define NO_SYS                          1
#define LWIP_TIMERS                     1
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_SINGLE_NETIF               1

/* memory */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
#define MEM_ALIGNMENT                   8   /* a 64-bit host (the tests): pbufs hold pointers */
#else
#define MEM_ALIGNMENT                   4
#endif
#define MEM_SIZE                        4096
#define PBUF_POOL_SIZE                  4
#define PBUF_POOL_BUFSIZE               512
#define MEMP_NUM_PBUF                   4
#define MEMP_NUM_UDP_PCB                2
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 1)

/* the link: Ethernet frames inside 802.11 data frames. PBUF_LINK_ENCAPSULATION_HLEN leaves room in front of every
 * frame lwIP builds for the 802.11 header and the LLC/SNAP header (24 + 8 B) where the Ethernet header (14 B) was,
 * so fm1wifi_net sends it without copying. */
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define ETH_PAD_SIZE                    0
#define PBUF_LINK_HLEN                  14
#define PBUF_LINK_ENCAPSULATION_HLEN    18
#define ARP_TABLE_SIZE                  4
#define ARP_QUEUEING                    0
#define ETHARP_SUPPORT_STATIC_ENTRIES   0

/* IPv4, DHCP, ICMP, UDP */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define IP_REASSEMBLY                   0
#define IP_FRAG                         0
#define LWIP_ICMP                       1
#define LWIP_BROADCAST_PING             0
#define LWIP_MULTICAST_PING             0
#define LWIP_RAW                        0
#define LWIP_UDP                        1
#define LWIP_TCP                        0
#define LWIP_DHCP                       1
#define DHCP_DOES_ARP_CHECK             0
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0
#define LWIP_DNS                        0
#define LWIP_HAVE_LOOPIF                0
#define LWIP_NETIF_LOOPBACK             0

/* checksums all in software (the MAC does not compute IP checksums) */
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_ICMP               1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_ICMP             1

/* no statistics or debug output */
#define LWIP_STATS                      0
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_NETIF_HOSTNAME             0
#ifndef FM1WIFI_NET_DEBUG
#define LWIP_NOASSERT                   1
#endif
