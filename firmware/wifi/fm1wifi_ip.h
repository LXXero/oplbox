/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi_ip: IPv4 on a joined WPA2 station (stage 9, docs/WIFI_STAGE9.md): fm1wifi_net.c's lwIP interface fed with
 * the data frames the station receives, its frames sent protected through the MAC's key tables, and the steps the
 * WiFi test page runs on it: DHCP, ARP for the gateway, pings to the gateway, and a window in which echo requests
 * from the network are counted as they are answered. What each step saw is kept for the page and its report: the DHCP
 * messages' fields, the first data frames either way, the round trips, the frames the MAC did not decrypt.
 *
 * Two layers, as fm1wifi_wpa.h has them:
 *  - fm1wifi_ip_dhcp_parse, fm1wifi_ip_echo_parse, fm1wifi_ip_data: pure code on byte arrays (tests/wifi_ip.c);
 *  - fm1wifi_ip_start .. _stop: the driver, on fm1wifi_wpa_poll (the station keeps answering beacons, group key
 *    handshakes and deauthentications meanwhile) and fm1wifi_net.
 * Received data frames go to lwIP only from the AP (FromDS, addr2 the BSSID) to us or a group address, and, once the
 * PTK is loaded, only as the MAC decrypted them (RXD DECRYPTED, no cipher error): a frame in the clear is then dropped
 * and counted. EAPOL frames are the station's (fm1wifi_wpa.c), never lwIP's. Frames out carry the Protected bit and go
 * with FM1WIFI_TX_CRYPT on the AP's WCID (the MAC adds the CCMP header and MIC), and take their sequence numbers from
 * the station's counter, so management, EAPOL and data frames share one. Nothing here sees a key: the frames kept are
 * the MAC's plaintext (received: IV and MIC taken off; sent: before the MAC encrypts them).
 *
 * Time is sys_now() (fm1wifi_port.h), the clock lwIP's timers run on; every wait also ends when the library's own
 * clock (its delays) has run that long, so a port clock that stands still cannot hang a step. Main loop only.
 * Own code, from RFC 2131 / 2132 (DHCP), RFC 792 (ICMP echo), RFC 826 (ARP) and RFC 1042. Not run on an FM-1 yet.
 */
#pragma once
#include <stdint.h>
#include "fm1wifi.h"
#include "fm1wifi_wpa.h"
#include "fm1wifi_net.h"

/* ---- what the packets say (pure code) */

/* A DHCP message (RFC 2131 section 2; options RFC 2132) in an IPv4 packet ip of n bytes */
enum { FM1WIFI_DHCP_DISCOVER = 1, FM1WIFI_DHCP_OFFER, FM1WIFI_DHCP_REQUEST, FM1WIFI_DHCP_DECLINE, FM1WIFI_DHCP_ACK,
       FM1WIFI_DHCP_NAK, FM1WIFI_DHCP_RELEASE, FM1WIFI_DHCP_INFORM };
const char *fm1wifi_dhcp_name(unsigned type);   /* "DISCOVER" .. "INFORM", "?" */
typedef struct {
    uint8_t op;               /* 1 BOOTREQUEST (client to server), 2 BOOTREPLY */
    uint8_t type;             /* option 53, FM1WIFI_DHCP_* (0: none) */
    uint32_t xid;
    uint8_t chaddr[6];        /* the client's MAC */
    uint8_t ciaddr[4], yiaddr[4], siaddr[4];   /* the client's own, the one offered or acknowledged, the next server's */
    uint8_t src[4], dst[4];   /* the IPv4 header's addresses */
    uint8_t server[4];        /* option 54, the server identifier */
    uint8_t mask[4];          /* option 1 */
    uint8_t router[4];        /* option 3, the first */
    uint8_t dns[2][4];        /* option 6, the first two */
    uint8_t n_dns;            /* addresses option 6 held (all, not only the two kept) */
    uint8_t requested[4];     /* option 50 (a REQUEST's) */
    uint32_t lease, t1, t2;   /* options 51, 58, 59, seconds (0: absent) */
} fm1wifi_dhcp_msg;
/* 1 when ip holds a DHCP message: IPv4 without fragments, UDP from port 67 to 68 or 68 to 67, a BOOTP body with the
 * magic cookie, an option 53 that is not cut short; m gets its fields. 0 for anything else (m is then all zero). */
int fm1wifi_ip_dhcp_parse(const uint8_t *ip, unsigned n, fm1wifi_dhcp_msg *m);

/* An ICMP echo request (type 8) or reply (type 0) in an IPv4 packet: 1 with e filled when the IPv4 header and the
 * ICMP checksum are right, 0 for anything else. */
typedef struct { uint8_t type; uint16_t id, seq, len; uint8_t src[4], dst[4]; } fm1wifi_icmp_echo;
int fm1wifi_ip_echo_parse(const uint8_t *ip, unsigned n, fm1wifi_icmp_echo *e);

/* A data or QoS data frame's payload: the header's length (24, 26, 30 with HT control), RFC 1042's SNAP header after it
 * and the ethertype. *type and the payload's offset; 0 when the frame is no data frame or has no SNAP header. */
unsigned fm1wifi_ip_data(const uint8_t *f, unsigned n, unsigned *type);

/* ---- the driver */

#define FM1WIFI_IP_REC_N 8u           /* data frames kept: the first six of any kind, the first ICMP frame either way */
#define FM1WIFI_IP_REC_LEN 400u       /* a DHCP message (about 340 bytes in a frame) fits whole */
#define FM1WIFI_IP_PING_N 10u         /* pings to the gateway at most */
enum { FM1WIFI_IP_KIND_OTHER, FM1WIFI_IP_KIND_ARP, FM1WIFI_IP_KIND_ICMP, FM1WIFI_IP_KIND_IP,
       FM1WIFI_IP_KIND_DHCP = 8 };   /* DHCP: 8 + the message type */
const char *fm1wifi_ip_kind_name(unsigned kind);  /* "ARP", "ICMP", "IP", "DHCP OFFER", ..., "OTHER" */
typedef struct {
    uint8_t dir;              /* 0 from the AP, 1 to it */
    uint8_t kind;             /* FM1WIFI_IP_KIND_* */
    uint16_t len;             /* the frame's length (no FCS); the first FM1WIFI_IP_REC_LEN of it are in b */
    uint16_t t;               /* ms after fm1wifi_ip_start (saturates) */
    uint8_t b[FM1WIFI_IP_REC_LEN];
} fm1wifi_ip_rec;

typedef struct {
    fm1wifi_wpa *w;           /* the joined station it runs on */
    fm1wifi_frames *fr;
    void (*idle)(void *);     /* called every FM1WIFI_IP_IDLE_MS during the waits (the firmware's USB), or 0 */
    void *idle_arg;
    int err;                  /* the first FIFO, send or interface error (FM1WIFI_OK: none) */
    int net_err;              /* fm1wifi_net's first error (FM1WIFI_NET_*) */
    uint32_t t0;              /* sys_now() at the start */
    uint8_t listening;        /* inside fm1wifi_ip_listen */
    /* DHCP */
    uint32_t t_dhcp;          /* ms from the DHCP start to an address (0: none) */
    uint32_t dhcp_tx[9], dhcp_rx[9];   /* messages by type, sent / received (index FM1WIFI_DHCP_*; 0 no type) */
    uint8_t have_offer, have_ack;
    fm1wifi_dhcp_msg offer, ack;   /* the first OFFER, the ACK that gave the address (or the last NAK) */
    uint8_t addr[4], mask[4], gw[4];   /* lwIP's once it has an address */
    /* ARP for the gateway */
    uint32_t t_arp;           /* ms from the request to the reply (0: none) */
    uint8_t gw_mac[6], arp_ok;
    uint32_t arp_tx, arp_rx;  /* ARP frames sent / received */
    /* pings to the gateway */
    uint16_t ping_id;
    uint32_t ping_wait;       /* ms a reply may take to count */
    uint8_t ping_n;           /* sent */
    uint32_t ping_sent_at[FM1WIFI_IP_PING_N];
    uint32_t ping_rtt[FM1WIFI_IP_PING_N];   /* ms, FM1WIFI_IP_NO_REPLY when no reply came */
    uint32_t ping_got, ping_min, ping_max, ping_sum, ping_late;   /* replies, their round trips; replies after the wait */
    /* echo requests from the network, and lwIP's replies */
    uint32_t echo_in, echo_out;            /* during the whole run */
    uint32_t listen_ms, listen_in, listen_out;   /* the listening window, and those inside it */
    uint32_t listen_at;       /* sys_now() when it began */
    uint32_t listen_enough;   /* answers that end it early (0: none do) */
    /* data frames */
    uint32_t rx_data;         /* data frames from the AP to us or a group address (EAPOL not counted) */
    uint32_t rx_net;          /* of those, handed to lwIP */
    uint32_t rx_clear;        /* not protected while the PTK is loaded: dropped */
    uint32_t rx_undecrypted;  /* protected, but the MAC did not decrypt them (no RXD DECRYPTED, no cipher error) */
    uint32_t rx_icv, rx_mic, rx_key;   /* the MAC's cipher errors (RXD bits 10-9: 1, 2, 3) */
    uint32_t rx_iv_left;      /* decrypted, but the CCMP header still in front of the SNAP header: taken off here */
    uint32_t rx_group;        /* handed to lwIP, group addressed (the GTK's) */
    uint32_t tx_data, tx_err; /* frames lwIP sent; sends that failed */
    uint8_t n_rec;
    fm1wifi_ip_rec rec[FM1WIFI_IP_REC_N];
} fm1wifi_ip;
#define FM1WIFI_IP_NO_REPLY 0xFFFFFFFFu
#define FM1WIFI_IP_IDLE_MS 20u

/* Start IP on w (FM1WIFI_WPA_UP after fm1wifi_wpa_join; its frames counted in fr): the interface up with the station's
 * address and the BSSID, no address yet. idle (or 0) runs during the waits. FM1WIFI_ESTATE when w is not UP. */
int fm1wifi_ip_start(fm1wifi_ip *ip, fm1wifi_wpa *w, fm1wifi_frames *fr, void (*idle)(void *), void *idle_arg);
/* DHCP until lwIP has an address (DISCOVER, OFFER, REQUEST, ACK; lwIP retries DISCOVER after 2, 4, ... s), at most
 * ms milliseconds. FM1WIFI_OK with an address, ETIMEOUT without, ESTATE when the station left, or the FIFO's error. */
int fm1wifi_ip_dhcp(fm1wifi_ip *ip, unsigned ms);
/* ARP for the gateway DHCP gave (an ARP request every 500 ms), until the reply, at most ms milliseconds. ERANGE when
 * there is no gateway. */
int fm1wifi_ip_arp(fm1wifi_ip *ip, unsigned ms);
/* n (1-FM1WIFI_IP_PING_N) echo requests to the gateway, len bytes of data each, gap_ms apart; each waits for its
 * reply up to wait_ms. ERANGE when there is no gateway. */
int fm1wifi_ip_ping(fm1wifi_ip *ip, unsigned n, unsigned len, unsigned gap_ms, unsigned wait_ms);
/* Answer echo requests for ms milliseconds, or until enough (0: the whole window) have been answered. */
int fm1wifi_ip_listen(fm1wifi_ip *ip, unsigned ms, unsigned enough);
/* The listening window's milliseconds left (0 outside it). */
uint32_t fm1wifi_ip_listen_left(const fm1wifi_ip *ip);
/* Stop: DHCP releases its lease (a DHCPRELEASE while the station is still joined), the interface goes down. */
void fm1wifi_ip_stop(fm1wifi_ip *ip);
