/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi_net: IPv4 over the WiFi station link (docs/WIFI.md stage 7), on lwIP 2.1.3 (lwip/, BSD-3-Clause) with
 * NO_SYS=1 and its raw API. One network interface whose frames are 802.11 data frames: received ones become
 * Ethernet frames for lwIP (RFC 1042 LLC/SNAP), lwIP's Ethernet frames go out as 802.11 data frames to the AP.
 * DHCP, ARP and ICMP echo come from lwIP; EAPOL frames (ethertype 0x888E) go to a callback for the WPA2 code instead.
 * No OPLBOX code, no malloc (lwIP's static pools only, sized in lwip/port/lwipopts.h), nothing from an interrupt:
 * call everything from the main loop. lwIP's clock is sys_now() from the port (fm1wifi_port.h).
 *
 * Frames are 802.11 MAC frames without the FCS, as the driver receives and sends them (the hardware checks and
 * adds the FCS). The station side:
 *   out: data (subtype 0), ToDS, addr1 = BSSID, addr2 = own MAC, addr3 = destination, then AA AA 03 00 00 00 and the
 *        ethertype, then the payload; sequence numbers counted here (the hardware may replace them);
 *   in:  data or QoS data (subtype 0 or 8), FromDS only, addr1 = own MAC or a group address, addr2 = BSSID,
 *        addr3 = source. Dropped: other types and subtypes (null, management), ToDS set, another MAC or BSSID, our
 *        own group frames coming back (addr3 = own MAC), fragments, A-MSDUs, frames with the Protected bit (the
 *        driver hands frames decrypted, CCMP header and MIC removed and the bit cleared), LLC headers other than
 *        RFC 1042 SNAP.
 * Built and host-tested only (tests/wifi_net.c); not run on an FM-1, not linked into OPLBOX yet.
 */
#pragma once
#include <stdint.h>

/* OK; ESTATE: not initialised, or no DHCP running; ERANGE: an argument out of range; EMEM: lwIP's pools are full;
 * ETX: the driver's send hook refused the frame. */
enum { FM1WIFI_NET_OK = 0, FM1WIFI_NET_ESTATE = -1, FM1WIFI_NET_ERANGE = -2, FM1WIFI_NET_EMEM = -3,
       FM1WIFI_NET_ETX = -4 };

/* The longest frame either way: 802.11 header (24, 26 with QoS, 30 with QoS and HT control), LLC/SNAP 8, MTU 1500 */
#define FM1WIFI_NET_MTU 1500u
#define FM1WIFI_NET_FRAME_MAX (30u + 8u + FM1WIFI_NET_MTU)

/* The driver's send hook: one 802.11 data frame (no FCS), valid only during the call (copy it). Return 0 when
 * it was queued, anything else when it was not (lwIP then counts it as an error; nothing is retried here). */
typedef int (*fm1wifi_net_tx_fn)(const uint8_t *frame, unsigned len, void *ctx);
/* An EAPOL frame from the AP (ethertype 0x888E): src is the sender (addr3), body the EAPOL PDU after the
 * ethertype (version, type, length, ...), valid only during the call. */
typedef void (*fm1wifi_net_eapol_fn)(const uint8_t src[6], const uint8_t *body, unsigned len, void *ctx);

/* Start (or restart, after a new association) the interface with the station's MAC and the AP's BSSID: the
 * interface comes up with no address, the link up. tx is required; eapol may be NULL (EAPOL frames are then
 * dropped). A restart stops DHCP and forgets the address and the ARP table. The first call initialises lwIP. */
int fm1wifi_net_init(const uint8_t mac[6], const uint8_t bssid[6], fm1wifi_net_tx_fn tx,
                     fm1wifi_net_eapol_fn eapol, void *ctx);
/* The link is gone (disassociated): DHCP stops, the address is dropped, received frames are ignored until init. */
void fm1wifi_net_down(void);

/* Each received 802.11 data frame (no FCS). Filtered as above; IP and ARP go through lwIP before this returns. */
void fm1wifi_net_input(const uint8_t *frame, unsigned len);
/* lwIP's timers (ARP ageing, DHCP's retries and lease): call on every main-loop pass; cheap when nothing is due. */
void fm1wifi_net_poll(void);

/* DHCP: start asking for an address (DISCOVER, retried 2, 4, 8 ... 60 s apart, as lwIP does), or stop it and
 * release the lease. */
int fm1wifi_net_dhcp_start(void);
void fm1wifi_net_dhcp_stop(void);

/* An EAPOL frame to the AP (the WPA2 code's handshake replies): body is the EAPOL PDU; sent unencrypted to the
 * BSSID as an 802.11 data frame with ethertype 0x888E. */
int fm1wifi_net_eapol_send(const uint8_t *body, unsigned len);

enum { FM1WIFI_NET_DOWN, FM1WIFI_NET_LINK, FM1WIFI_NET_DHCP, FM1WIFI_NET_UP };
/* DOWN: not initialised or fm1wifi_net_down; LINK: up without an address and no DHCP; DHCP: asking for one;
 * UP: an address (from DHCP). */
int fm1wifi_net_state(void);
/* The address, netmask and gateway, in network order as bytes (a.b.c.d is addr[0..3]); 0.0.0.0 when there is none.
 * Any pointer may be NULL. */
void fm1wifi_net_addr(uint8_t addr[4], uint8_t mask[4], uint8_t gw[4]);

/* Frames seen by fm1wifi_net_input and what became of them, for the WiFi page and the tests. */
typedef struct {
    uint32_t rx, rx_ip, rx_eapol, rx_dropped;   /* every frame in; to lwIP; to the EAPOL callback; dropped */
    uint32_t tx, tx_err;                        /* frames to the send hook; refused by it or no memory */
} fm1wifi_net_counts;
void fm1wifi_net_get_counts(fm1wifi_net_counts *c);
