/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi_net (firmware/wifi/fm1wifi_net.c, docs/WIFI.md stage 7) and lwIP 2.1.3 against replayed 802.11 data
 * frames: the test plays the AP, a DHCP server behind it and two hosts that ping, all as frames built here, and
 * reads every frame the interface sends back through the driver's send hook. Checked: DHCP DISCOVER, OFFER,
 * REQUEST, ACK gives the address (and DISCOVER is retried while the server is silent), ARP requests for our address
 * are answered, ICMP echo requests are answered (plain and QoS data frames, a small and a chained 1,400-byte one,
 * one from a host lwIP must ARP for first), with correct checksums and 802.11 header fields (ToDS, addr1 = BSSID,
 * addr2 = own MAC, addr3 = destination); frames for another MAC or BSSID, ToDS frames, protected frames, fragments,
 * our own broadcasts coming back, A-MSDUs and LLC headers other than RFC 1042 SNAP are dropped; EAPOL frames reach the EAPOL callback and not lwIP,
 * and go out with fm1wifi_net_eapol_send; our own ARP requests and echo requests (stage 9: the gateway looked up, an
 * echo reply not answered, one to an unknown host held for its ARP reply); DHCP stop sends a RELEASE; down and a
 * restart with a new BSSID.
 * The clock is sys_now() here (the port's, fm1wifi_port.h), moved by the test.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fm1wifi_net.h"
#include "fm1wifi_port.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                 printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the port's clock */
static uint32_t now_ms = 5000;
uint32_t sys_now(void) { return now_ms; }

/* ---- the driver: frames sent, the EAPOL callback */
#define TXQ 16
static uint8_t txf[TXQ][1600];
static unsigned txlen[TXQ], ntx;
static int tx_refuse;
static int hook_tx(const uint8_t *f, unsigned len, void *ctx) {
    (void)ctx;
    if (tx_refuse) return -1;
    if (ntx < TXQ && len <= sizeof txf[0]) { memcpy(txf[ntx], f, len); txlen[ntx] = len; }
    ntx++;
    return 0;
}
static uint8_t eap_src[6], eap_body[256];
static unsigned eap_len, eap_calls;
static void hook_eapol(const uint8_t src[6], const uint8_t *body, unsigned len, void *ctx) {
    CHECK(ctx == (void *)&eap_calls, "EAPOL callback context");
    memcpy(eap_src, src, 6);
    eap_len = len < sizeof eap_body ? len : sizeof eap_body;
    memcpy(eap_body, body, eap_len);
    eap_calls++;
}

/* ---- the network */
static const uint8_t MAC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};      /* the station (us) */
static const uint8_t BSSID[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};    /* the AP */
static const uint8_t BSSID2[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEF};   /* another AP */
static const uint8_t ROUTER[6] = {0x02, 0x52, 0x52, 0x52, 0x52, 0x01};   /* the DHCP server and gateway */
static const uint8_t PINGER[6] = {0x02, 0x50, 0x50, 0x50, 0x50, 0x50};
static const uint8_t PINGER2[6] = {0x02, 0x60, 0x60, 0x60, 0x60, 0x60};  /* not in our ARP table */
static const uint8_t OTHER[6] = {0x02, 0x99, 0x99, 0x99, 0x99, 0x99};    /* another station */
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t ZERO6[6] = {0};
static const uint8_t IP_ROUTER[4] = {192, 168, 4, 1}, IP_US[4] = {192, 168, 4, 23}, MASK[4] = {255, 255, 255, 0};
static const uint8_t IP_PINGER[4] = {192, 168, 4, 50}, IP_PINGER2[4] = {192, 168, 4, 60};
static const uint8_t IP_BCAST[4] = {255, 255, 255, 255}, IP_ZERO[4] = {0};
static const uint8_t *AP = BSSID;                    /* the AP we are on */

static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* RFC 1071 */
static uint32_t sum16(const uint8_t *p, unsigned n, uint32_t s) {
    for (unsigned i = 0; i + 1 < n; i += 2) s += get16(p + i);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    return s;
}
static unsigned fold(uint32_t s) {
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return ~s & 0xFFFF;
}
static unsigned udp_sum(const uint8_t *ip) {        /* over the pseudo header and the UDP datagram: 0 when right */
    unsigned ulen = get16(ip + 24);
    uint32_t s = sum16(ip + 12, 8, 0) + 17 + ulen;
    return fold(sum16(ip + 20, ulen, s));
}

/* an IPv4 header at p with a correct checksum */
static void ip_hdr(uint8_t *p, const uint8_t src[4], const uint8_t dst[4], unsigned proto, unsigned payload) {
    static unsigned id = 0x100;
    memset(p, 0, 20);
    p[0] = 0x45; put16(p + 2, 20 + payload); put16(p + 4, id++); p[8] = 64; p[9] = (uint8_t)proto;
    memcpy(p + 12, src, 4); memcpy(p + 16, dst, 4);
    put16(p + 10, fold(sum16(p, 20, 0)));
}

/* a frame from the AP (FromDS): addr1 dst, addr2 the BSSID, addr3 src; fc1 extra flag bits; qos: a QoS data frame */
static unsigned frame(uint8_t *f, const uint8_t dst[6], const uint8_t bssid[6], const uint8_t src[6], unsigned type,
                      const uint8_t *body, unsigned n, int qos, unsigned fc1) {
    static unsigned sq;
    unsigned h = 24;
    f[0] = qos ? 0x88 : 0x08; f[1] = (uint8_t)(0x02 | fc1); f[2] = f[3] = 0;
    memcpy(f + 4, dst, 6); memcpy(f + 10, bssid, 6); memcpy(f + 16, src, 6);
    f[22] = (uint8_t)(sq << 4); f[23] = (uint8_t)(sq >> 4); sq++;
    if (qos) { f[24] = 0x05; f[25] = 0; h = 26; }       /* TID 5 */
    const uint8_t snap[6] = {0xAA, 0xAA, 0x03, 0, 0, 0};
    memcpy(f + h, snap, 6); put16(f + h + 6, type);
    memcpy(f + h + 8, body, n);
    return h + 8 + n;
}
static void rx(const uint8_t dst[6], const uint8_t src[6], unsigned type, const uint8_t *body, unsigned n, int qos) {
    static uint8_t f[1600];
    fm1wifi_net_input(f, frame(f, dst, AP, src, type, body, n, qos, 0));
}

/* a frame we sent: check the 802.11 and SNAP headers, give the payload */
static const uint8_t *tx_body(unsigned i, const uint8_t dst[6], unsigned type, unsigned *n) {
    const uint8_t *f = txf[i];
    *n = 0;
    CHECK(i < ntx && i < TXQ, "frame %u was not sent (%u sent)", i, ntx);
    if (i >= ntx || i >= TXQ || txlen[i] < 32) return NULL;
    CHECK(f[0] == 0x08, "frame %u: FC %02x, want 08 (data)", i, f[0]);
    CHECK(f[1] == 0x01, "frame %u: flags %02x, want ToDS only", i, f[1]);
    CHECK(memcmp(f + 4, AP, 6) == 0, "frame %u: addr1 is not the BSSID", i);
    CHECK(memcmp(f + 10, MAC, 6) == 0, "frame %u: addr2 is not our MAC", i);
    CHECK(memcmp(f + 16, dst, 6) == 0, "frame %u: addr3 %02x:..:%02x, want %02x:..:%02x", i, f[16], f[21], dst[0], dst[5]);
    CHECK((f[22] & 0x0F) == 0, "frame %u: fragment number", i);
    const uint8_t snap[6] = {0xAA, 0xAA, 0x03, 0, 0, 0};
    CHECK(memcmp(f + 24, snap, 6) == 0, "frame %u: no RFC 1042 SNAP header", i);
    CHECK(get16(f + 30) == type, "frame %u: ethertype %04x, want %04x", i, get16(f + 30), type);
    *n = txlen[i] - 32;
    return f + 32;
}

/* an IPv4 packet we sent: header checksum, addresses, protocol */
static int ip_ok(const uint8_t *ip, unsigned n, const uint8_t src[4], const uint8_t dst[4], unsigned proto) {
    if (ip == NULL || n < 20) { CHECK(0, "no IP packet"); return 0; }
    CHECK(ip[0] == 0x45, "IP version/IHL %02x", ip[0]);
    CHECK(get16(ip + 2) == n, "IP total length %u, frame payload %u", get16(ip + 2), n);
    CHECK(fold(sum16(ip, 20, 0)) == 0, "IP header checksum wrong");
    CHECK(memcmp(ip + 12, src, 4) == 0, "IP source %u.%u.%u.%u", ip[12], ip[13], ip[14], ip[15]);
    CHECK(memcmp(ip + 16, dst, 4) == 0, "IP destination %u.%u.%u.%u", ip[16], ip[17], ip[18], ip[19]);
    CHECK(ip[9] == proto, "IP protocol %u, want %u", ip[9], proto);
    return get16(ip + 2) == n && ip[9] == proto;
}

/* ---- DHCP */
static const uint8_t *dhcp_opt(const uint8_t *d, unsigned n, unsigned tag, unsigned *len) {
    for (unsigned i = 240; i < n;) {
        if (d[i] == 255) break;
        if (d[i] == 0) { i++; continue; }
        if (i + 1 >= n || i + 2 + d[i + 1] > n) break;
        if (d[i] == tag) { *len = d[i + 1]; return d + i + 2; }
        i += 2u + d[i + 1];
    }
    return NULL;
}

/* our DHCP message in frame i: the checks every one must pass, its message type, its xid */
static int dhcp_sent(unsigned i, const uint8_t dst_mac[6], const uint8_t src_ip[4], const uint8_t dst_ip[4],
                     uint32_t *xid) {
    unsigned n, l;
    const uint8_t *ip = tx_body(i, dst_mac, 0x0800, &n);
    if (!ip_ok(ip, n, src_ip, dst_ip, 17)) return -1;
    const uint8_t *u = ip + 20, *d = u + 8;
    CHECK(get16(u) == 68 && get16(u + 2) == 67, "DHCP ports %u -> %u", get16(u), get16(u + 2));
    CHECK(get16(u + 6) == 0 || udp_sum(ip) == 0, "DHCP UDP checksum wrong");
    CHECK(get16(u + 6) != 0, "DHCP sent without a UDP checksum");
    unsigned dn = n - 28;
    CHECK(dn >= 244 && d[0] == 1 && d[1] == 1 && d[2] == 6, "not a BOOTREQUEST");
    CHECK(memcmp(d + 28, MAC, 6) == 0, "DHCP chaddr is not our MAC");
    CHECK(d[236] == 99 && d[237] == 130 && d[238] == 83 && d[239] == 99, "no DHCP magic cookie");
    *xid = (uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 8 | d[7];
    const uint8_t *t = dhcp_opt(d, dn, 53, &l);
    return t && l == 1 ? t[0] : -1;
}
static const uint8_t *dhcp_sent_opt(unsigned i, unsigned tag, unsigned *len) {
    unsigned n;
    const uint8_t *f = txf[i];
    if (i >= ntx || txlen[i] < 32 + 28 + 240) return NULL;
    n = txlen[i] - 32 - 28;
    return dhcp_opt(f + 32 + 28, n, tag, len);
}

/* the server's reply (OFFER 2, ACK 5) for xid; broadcast to ff:ff:ff:ff:ff:ff / 255.255.255.255 or unicast to us */
static void dhcp_reply(unsigned type, uint32_t xid, int bcast) {
    uint8_t p[400] = {0}, *u = p + 20, *d = u + 8;
    d[0] = 2; d[1] = 1; d[2] = 6;
    d[4] = (uint8_t)(xid >> 24); d[5] = (uint8_t)(xid >> 16); d[6] = (uint8_t)(xid >> 8); d[7] = (uint8_t)xid;
    memcpy(d + 16, IP_US, 4); memcpy(d + 20, IP_ROUTER, 4); memcpy(d + 28, MAC, 6);
    d[236] = 99; d[237] = 130; d[238] = 83; d[239] = 99;
    uint8_t *o = d + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    *o++ = 54; *o++ = 4; memcpy(o, IP_ROUTER, 4); o += 4;
    *o++ = 51; *o++ = 4; *o++ = 0; *o++ = 0; *o++ = 0x0E; *o++ = 0x10;   /* lease 3600 s */
    *o++ = 1; *o++ = 4; memcpy(o, MASK, 4); o += 4;
    *o++ = 3; *o++ = 4; memcpy(o, IP_ROUTER, 4); o += 4;
    *o++ = 255;
    unsigned dn = (unsigned)(o - d), ulen = 8 + dn;
    put16(u, 67); put16(u + 2, 68); put16(u + 4, ulen); put16(u + 6, 0);
    ip_hdr(p, IP_ROUTER, bcast ? IP_BCAST : IP_US, 17, ulen);
    unsigned s = udp_sum(p);
    put16(u + 6, s ? s : 0xFFFF);
    CHECK(udp_sum(p) == 0, "test's own UDP checksum");
    rx(bcast ? BCAST : MAC, ROUTER, 0x0800, p, 20 + ulen, 0);
}

/* ---- ARP */
static void arp(unsigned op, const uint8_t dst[6], const uint8_t smac[6], const uint8_t sip[4], const uint8_t tmac[6],
                const uint8_t tip[4]) {
    uint8_t a[28] = {0, 1, 8, 0, 6, 4, 0, 0};
    a[7] = (uint8_t)op;
    memcpy(a + 8, smac, 6); memcpy(a + 14, sip, 4); memcpy(a + 18, tmac, 6); memcpy(a + 24, tip, 4);
    rx(dst, smac, 0x0806, a, 28, 0);
}
static void arp_sent(unsigned i, const uint8_t dst[6], unsigned op, const uint8_t tmac[6], const uint8_t tip[4]) {
    unsigned n;
    const uint8_t *a = tx_body(i, dst, 0x0806, &n);
    if (a == NULL) return;
    CHECK(n >= 28 && get16(a) == 1 && get16(a + 2) == 0x0800 && a[4] == 6 && a[5] == 4, "ARP header");
    CHECK(get16(a + 6) == op, "ARP op %u, want %u", get16(a + 6), op);
    CHECK(memcmp(a + 8, MAC, 6) == 0 && memcmp(a + 14, IP_US, 4) == 0, "ARP sender is not us");
    CHECK(memcmp(a + 18, tmac, 6) == 0, "ARP target MAC");
    CHECK(memcmp(a + 24, tip, 4) == 0, "ARP target IP %u.%u.%u.%u", a[24], a[25], a[26], a[27]);
}

/* ---- ICMP echo */
static unsigned echo_req(uint8_t *p, const uint8_t src[4], unsigned id, unsigned sq, unsigned n, int bad_sum) {
    uint8_t *c = p + 20;
    c[0] = 8; c[1] = 0; put16(c + 2, 0); put16(c + 4, id); put16(c + 6, sq);
    for (unsigned i = 0; i < n; i++) c[8 + i] = (uint8_t)(i * 7 + sq);
    put16(c + 2, fold(sum16(c, 8 + n, 0)) ^ (bad_sum ? 1u : 0u));
    ip_hdr(p, src, IP_US, 1, 8 + n);
    return 20 + 8 + n;
}
static void echo_reply_sent(unsigned i, const uint8_t dst[6], const uint8_t dst_ip[4], unsigned id, unsigned sq,
                            unsigned n) {
    unsigned len;
    const uint8_t *ip = tx_body(i, dst, 0x0800, &len);
    if (!ip_ok(ip, len, IP_US, dst_ip, 1)) return;
    const uint8_t *c = ip + 20;
    CHECK(len == 20 + 8 + n, "echo reply length %u, want %u", len, 20 + 8 + n);
    CHECK(c[0] == 0 && c[1] == 0, "ICMP type %u code %u, want echo reply 0 0", c[0], c[1]);
    CHECK(fold(sum16(c, len - 20, 0)) == 0, "ICMP checksum wrong");
    CHECK(get16(c + 4) == id && get16(c + 6) == sq, "echo id/seq %u/%u, want %u/%u", get16(c + 4), get16(c + 6), id, sq);
    int same = 1;
    for (unsigned k = 0; k < n && 8 + k < len - 20; k++) same &= c[8 + k] == (uint8_t)(k * 7 + sq);
    CHECK(same, "echo reply data differs from the request's");
}

static void run(unsigned ms) {                       /* the main loop for ms, in 10 ms passes */
    for (unsigned t = 0; t < ms; t += 10) { now_ms += 10; fm1wifi_net_poll(); }
}

int main(void) {
    uint8_t addr[4], mask[4], gw[4], pkt[1600];
    uint32_t xid = 0, xid2 = 0;
    fm1wifi_net_counts c0, c1;

    /* nothing before init */
    CHECK(fm1wifi_net_state() == FM1WIFI_NET_DOWN, "state before init");
    CHECK(fm1wifi_net_dhcp_start() == FM1WIFI_NET_ESTATE, "DHCP before init");
    CHECK(fm1wifi_net_init(BCAST, BSSID, hook_tx, NULL, NULL) == FM1WIFI_NET_ERANGE, "a group MAC refused");
    CHECK(fm1wifi_net_init(MAC, BSSID, NULL, NULL, NULL) == FM1WIFI_NET_ERANGE, "no send hook refused");

    CHECK(fm1wifi_net_init(MAC, BSSID, hook_tx, hook_eapol, &eap_calls) == FM1WIFI_NET_OK, "init");
    CHECK(fm1wifi_net_state() == FM1WIFI_NET_LINK, "state after init: %d", fm1wifi_net_state());
    fm1wifi_net_addr(addr, mask, gw);
    CHECK(memcmp(addr, IP_ZERO, 4) == 0, "an address before DHCP");
    run(3000);
    CHECK(ntx == 0, "%u frames sent with no address and no DHCP", ntx);

    /* DHCP: DISCOVER now, then retried while the server is silent, 2 s and 4 s later (lwIP: 2^tries s, 0.5 s ticks) */
    CHECK(fm1wifi_net_dhcp_start() == FM1WIFI_NET_OK, "DHCP start");
    CHECK(fm1wifi_net_state() == FM1WIFI_NET_DHCP, "state while asking: %d", fm1wifi_net_state());
    CHECK(ntx == 1, "DHCP start sent %u frames, want the DISCOVER", ntx);
    CHECK(dhcp_sent(0, BCAST, IP_ZERO, IP_BCAST, &xid) == 1, "first frame is not a DISCOVER");
    uint32_t t_sent[3] = {now_ms, 0, 0};
    for (unsigned k = 1; k < 3; k++) {
        unsigned before = ntx;
        for (unsigned t = 0; t < 10000 && ntx == before; t += 10) { now_ms += 10; fm1wifi_net_poll(); }
        t_sent[k] = now_ms;
        uint32_t x;
        CHECK(ntx == before + 1, "no DISCOVER retry %u", k);
        CHECK(dhcp_sent(ntx - 1, BCAST, IP_ZERO, IP_BCAST, &x) == 1, "retry %u is not a DISCOVER", k);
        xid2 = x;
    }
    unsigned gap1 = t_sent[1] - t_sent[0], gap2 = t_sent[2] - t_sent[1];
    CHECK(gap1 >= 1500 && gap1 <= 2500, "first DISCOVER retry after %u ms, want about 2000", gap1);
    CHECK(gap2 >= 3500 && gap2 <= 4500, "second DISCOVER retry after %u ms, want about 4000", gap2);
    printf("DHCP: DISCOVER, silent server: retried after %u ms and %u ms\n", gap1, gap2);
    CHECK(fm1wifi_net_state() == FM1WIFI_NET_DHCP, "state while the server is silent");

    /* OFFER (broadcast) for another xid is ignored; for ours, REQUEST; ACK (unicast) gives the address */
    ntx = 0;
    dhcp_reply(2, xid2 ^ 0x5A5A5A5A, 1);
    CHECK(ntx == 0, "an OFFER for another xid answered");
    dhcp_reply(2, xid2, 1);
    CHECK(ntx == 1, "OFFER answered with %u frames, want the REQUEST", ntx);
    uint32_t xr;
    CHECK(dhcp_sent(0, BCAST, IP_ZERO, IP_BCAST, &xr) == 3, "the answer to the OFFER is not a REQUEST");
    CHECK(xr == xid2, "REQUEST xid %08x, want the OFFER's %08x", (unsigned)xr, (unsigned)xid2);
    unsigned l;
    const uint8_t *o = dhcp_sent_opt(0, 50, &l);
    CHECK(o && l == 4 && memcmp(o, IP_US, 4) == 0, "REQUEST without the offered address (option 50)");
    o = dhcp_sent_opt(0, 54, &l);
    CHECK(o && l == 4 && memcmp(o, IP_ROUTER, 4) == 0, "REQUEST without the server id (option 54)");
    CHECK(fm1wifi_net_state() == FM1WIFI_NET_DHCP, "an address before the ACK");
    ntx = 0;
    dhcp_reply(5, xid2, 0);
    CHECK(fm1wifi_net_state() == FM1WIFI_NET_UP, "no address after the ACK (state %d)", fm1wifi_net_state());
    fm1wifi_net_addr(addr, mask, gw);
    CHECK(memcmp(addr, IP_US, 4) == 0, "address %u.%u.%u.%u, want 192.168.4.23", addr[0], addr[1], addr[2], addr[3]);
    CHECK(memcmp(mask, MASK, 4) == 0, "netmask %u.%u.%u.%u", mask[0], mask[1], mask[2], mask[3]);
    CHECK(memcmp(gw, IP_ROUTER, 4) == 0, "gateway %u.%u.%u.%u", gw[0], gw[1], gw[2], gw[3]);
    /* lwIP announces the new address with a gratuitous ARP request */
    CHECK(ntx == 1, "%u frames after the ACK, want the gratuitous ARP", ntx);
    arp_sent(0, BCAST, 1, ZERO6, IP_US);
    printf("DHCP: OFFER, REQUEST, ACK: %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u\n", addr[0], addr[1], addr[2], addr[3],
           mask[0], mask[1], mask[2], mask[3], gw[0], gw[1], gw[2], gw[3]);

    /* ARP: a request for our address is answered to the asker; one for another address is not */
    ntx = 0;
    arp(1, BCAST, PINGER, IP_PINGER, ZERO6, IP_US);
    CHECK(ntx == 1, "ARP request answered with %u frames", ntx);
    arp_sent(0, PINGER, 2, PINGER, IP_PINGER);
    ntx = 0;
    arp(1, BCAST, PINGER, IP_PINGER, ZERO6, IP_ROUTER);
    CHECK(ntx == 0, "an ARP request for another address answered");

    /* ICMP echo: a small one in a QoS data frame, a chained 1,400-byte one in a plain data frame */
    ntx = 0;
    unsigned n = echo_req(pkt, IP_PINGER, 0x1234, 1, 56, 0);
    rx(MAC, PINGER, 0x0800, pkt, n, 1);
    CHECK(ntx == 1, "echo request (QoS data) answered with %u frames", ntx);
    echo_reply_sent(0, PINGER, IP_PINGER, 0x1234, 1, 56);
    ntx = 0;
    n = echo_req(pkt, IP_PINGER, 0x1234, 2, 1400, 0);
    rx(MAC, PINGER, 0x0800, pkt, n, 0);
    CHECK(ntx == 1, "1,400-byte echo request answered with %u frames", ntx);
    echo_reply_sent(0, PINGER, IP_PINGER, 0x1234, 2, 1400);
    /* a bad ICMP checksum: lwIP drops it */
    ntx = 0;
    n = echo_req(pkt, IP_PINGER, 0x1234, 3, 56, 1);
    rx(MAC, PINGER, 0x0800, pkt, n, 0);
    CHECK(ntx == 0, "an echo request with a bad checksum answered");
    /* a QoS data frame with HT control (Order bit): 4 more header bytes */
    {
        uint8_t f[200];
        n = echo_req(pkt, IP_PINGER, 0x1234, 4, 32, 0);
        unsigned fl = frame(f, MAC, BSSID, PINGER, 0x0800, pkt, n, 1, 0x80);
        memmove(f + 30, f + 26, fl - 26);
        memset(f + 26, 0, 4);
        ntx = 0;
        fm1wifi_net_input(f, fl + 4);
        CHECK(ntx == 1, "echo request in a QoS data frame with HT control not answered");
        echo_reply_sent(0, PINGER, IP_PINGER, 0x1234, 4, 32);
    }
    /* from a host not in the ARP table: lwIP asks for its MAC first, then sends the reply it held */
    ntx = 0;
    n = echo_req(pkt, IP_PINGER2, 0x77, 9, 100, 0);
    rx(MAC, PINGER2, 0x0800, pkt, n, 0);
    CHECK(ntx == 1, "echo from an unknown host: %u frames, want an ARP request", ntx);
    arp_sent(0, BCAST, 1, ZERO6, IP_PINGER2);
    arp(2, MAC, PINGER2, IP_PINGER2, MAC, IP_US);
    CHECK(ntx == 2, "the held echo reply not sent after the ARP reply (%u frames)", ntx);
    echo_reply_sent(1, PINGER2, IP_PINGER2, 0x77, 9, 100);
    printf("ARP and ICMP echo: answered, checksums and 802.11 headers right\n");

    /* our own ARP requests and pings (stage 9, fm1wifi_ip.c): a gateway's MAC asked for and looked up; an echo request
     * to it, checksums right; its reply goes nowhere (lwIP drops echo replies); one to a host not in the table goes out
     * after the ARP reply lwIP waited for */
    {
        static const uint8_t IP_FAR[4] = {192, 168, 4, 77}, FAR[6] = {0x02, 0x77, 0x77, 0x77, 0x77, 0x77};
        static const uint8_t IP_GW[4] = {192, 168, 4, 2}, GW[6] = {0x02, 0x47, 0x57, 0x00, 0x00, 0x02};   /* not the
                                                                    * router: the DHCP stop below must still ARP for it */
        uint8_t m6[6];
        unsigned len;
        CHECK(fm1wifi_net_arp_lookup(IP_GW, m6) == 0, "a host in the ARP table before any ARP reply from it");
        ntx = 0;
        CHECK(fm1wifi_net_arp_request(IP_GW) == FM1WIFI_NET_OK && ntx == 1, "ARP request not sent");
        arp_sent(0, BCAST, 1, ZERO6, IP_GW);
        arp(2, MAC, GW, IP_GW, MAC, IP_US);
        CHECK(fm1wifi_net_arp_lookup(IP_GW, m6) == 1 && memcmp(m6, GW, 6) == 0, "the MAC not looked up after the reply");
        ntx = 0;
        CHECK(fm1wifi_net_ping(IP_GW, 0xBEEF, 3, 56) == FM1WIFI_NET_OK && ntx == 1, "ping not sent");
        const uint8_t *ip = tx_body(0, GW, 0x0800, &len);
        if (ip_ok(ip, len, IP_US, IP_GW, 1)) {
            const uint8_t *c = ip + 20;
            int d = 1;
            for (unsigned k = 0; k < 56; k++) d &= c[8 + k] == (uint8_t)k;
            CHECK(len == 20 + 8 + 56 && c[0] == 8 && c[1] == 0 && fold(sum16(c, len - 20, 0)) == 0 &&
                  get16(c + 4) == 0xBEEF && get16(c + 6) == 3 && d && ip[8] == 64, "our echo request's fields or checksum");
            uint8_t r[100];
            memcpy(r, ip, len);
            memcpy(r + 12, IP_GW, 4); memcpy(r + 16, IP_US, 4);
            r[10] = r[11] = 0; put16(r + 10, fold(sum16(r, 20, 0)));
            r[20] = 0; r[22] = r[23] = 0; put16(r + 22, fold(sum16(r + 20, len - 20, 0)));
            ntx = 0;
            rx(MAC, GW, 0x0800, r, len, 0);
            CHECK(ntx == 0, "lwIP answered an echo reply");
        }
        CHECK(fm1wifi_net_ping(IP_GW, 1, 1, FM1WIFI_NET_PING_MAX + 1) == FM1WIFI_NET_ERANGE, "a ping too long taken");
        ntx = 0;
        CHECK(fm1wifi_net_ping(IP_FAR, 0xBEEF, 4, 8) == FM1WIFI_NET_OK && ntx == 1, "ping to an unknown host: no ARP request");
        arp_sent(0, BCAST, 1, ZERO6, IP_FAR);
        arp(2, MAC, FAR, IP_FAR, MAC, IP_US);
        CHECK(ntx == 2, "the held echo request not sent after the ARP reply (%u frames)", ntx);
        ip = tx_body(1, FAR, 0x0800, &len);
        CHECK(ip_ok(ip, len, IP_US, IP_FAR, 1) && ip[20] == 8 && get16(ip + 26) == 4, "the held echo request");
        printf("our ARP request and pings: sent, the gateway's MAC looked up, echo replies not answered\n");
    }

    /* dropped: each of these sends nothing and reaches neither lwIP nor the EAPOL callback */
    n = echo_req(pkt, IP_PINGER, 0x1234, 5, 56, 0);
    struct { const char *what; unsigned fc0, fc1; const uint8_t *a1, *a2, *a3; int snap_bad; } drop[] = {
        {"a frame for another MAC", 0x08, 0x02, OTHER, BSSID, PINGER, 0},
        {"a frame from another BSSID", 0x08, 0x02, MAC, BSSID2, PINGER, 0},
        {"a ToDS frame", 0x08, 0x01, MAC, BSSID, PINGER, 0},
        {"a WDS frame (ToDS and FromDS)", 0x08, 0x03, MAC, BSSID, PINGER, 0},
        {"a frame with neither DS bit", 0x08, 0x00, MAC, BSSID, PINGER, 0},
        {"a protected frame", 0x08, 0x42, MAC, BSSID, PINGER, 0},
        {"a fragment (more fragments)", 0x08, 0x06, MAC, BSSID, PINGER, 0},
        {"our own broadcast coming back", 0x08, 0x02, BCAST, BSSID, MAC, 0},
        {"a null data frame", 0x48, 0x02, MAC, BSSID, PINGER, 0},
        {"a management frame", 0x80, 0x02, MAC, BSSID, PINGER, 0},
        {"a non-SNAP LLC header", 0x08, 0x02, MAC, BSSID, PINGER, 1},
        {"a bridge-tunnel SNAP header (OUI 00 00 F8)", 0x08, 0x02, MAC, BSSID, PINGER, 2},
        {"an A-MSDU (QoS data, A-MSDU present)", 0x88, 0x02, MAC, BSSID, PINGER, 3},
    };
    for (unsigned k = 0; k < sizeof drop / sizeof drop[0]; k++) {
        uint8_t f[200];
        unsigned fl = frame(f, drop[k].a1, drop[k].a2, drop[k].a3, 0x0800, pkt, n, 0, 0);
        f[0] = (uint8_t)drop[k].fc0; f[1] = (uint8_t)drop[k].fc1;
        if (drop[k].snap_bad == 1) f[24] = 0x42;
        if (drop[k].snap_bad == 2) f[29] = 0xF8;
        if (drop[k].snap_bad == 3) {               /* QoS control with bit 7 set in front of the SNAP header */
            memmove(f + 26, f + 24, fl - 24); f[24] = 0x80; f[25] = 0; fl += 2;
        }
        ntx = 0; fm1wifi_net_get_counts(&c0);
        fm1wifi_net_input(f, fl);
        fm1wifi_net_get_counts(&c1);
        CHECK(ntx == 0, "%s: answered", drop[k].what);
        CHECK(c1.rx_dropped == c0.rx_dropped + 1 && c1.rx_ip == c0.rx_ip, "%s: not dropped", drop[k].what);
    }
    {   /* a fragment number, and short frames */
        uint8_t f[200];
        unsigned fl = frame(f, MAC, BSSID, PINGER, 0x0800, pkt, n, 0, 0);
        f[22] |= 1;
        fm1wifi_net_get_counts(&c0);
        ntx = 0;
        fm1wifi_net_input(f, fl);
        fm1wifi_net_input(f, 23);
        fm1wifi_net_input(f, 31);
        fm1wifi_net_get_counts(&c1);
        CHECK(ntx == 0 && c1.rx_dropped == c0.rx_dropped + 3, "a fragment or a short frame not dropped");
    }
    printf("dropped: %u kinds of frame not for us or not handled\n", (unsigned)(sizeof drop / sizeof drop[0]) + 2);

    /* EAPOL: to the callback (QoS too), not to lwIP; and out to the AP */
    {
        uint8_t e[99];
        for (unsigned k = 0; k < sizeof e; k++) e[k] = (uint8_t)(k ^ 0x5C);
        e[0] = 2; e[1] = 3;                          /* 802.1X-2004, EAPOL-Key */
        fm1wifi_net_get_counts(&c0);
        ntx = 0;
        rx(MAC, BSSID, 0x888E, e, sizeof e, 1);
        fm1wifi_net_get_counts(&c1);
        CHECK(eap_calls == 1, "EAPOL callback called %u times", eap_calls);
        CHECK(memcmp(eap_src, BSSID, 6) == 0, "EAPOL source is not the AP");
        CHECK(eap_len == sizeof e && memcmp(eap_body, e, sizeof e) == 0, "EAPOL body differs (%u bytes)", eap_len);
        CHECK(c1.rx_eapol == c0.rx_eapol + 1 && c1.rx_ip == c0.rx_ip, "EAPOL frame reached lwIP");
        CHECK(ntx == 0, "an EAPOL frame answered by lwIP");
        CHECK(fm1wifi_net_eapol_send(e, 95) == FM1WIFI_NET_OK, "EAPOL send");
        unsigned bn;
        const uint8_t *b = tx_body(0, BSSID, 0x888E, &bn);
        CHECK(b && bn == 95 && memcmp(b, e, 95) == 0, "EAPOL frame sent differs");
        CHECK(fm1wifi_net_eapol_send(e, 0) == FM1WIFI_NET_ERANGE, "an empty EAPOL frame sent");
    }
    printf("EAPOL: to the callback, not lwIP; sent to the AP\n");

    /* the send hook refusing: counted, and nothing breaks */
    tx_refuse = 1; ntx = 0;
    fm1wifi_net_get_counts(&c0);
    n = echo_req(pkt, IP_PINGER, 0x1234, 6, 56, 0);
    rx(MAC, PINGER, 0x0800, pkt, n, 0);
    fm1wifi_net_get_counts(&c1);
    CHECK(c1.tx_err == c0.tx_err + 1, "a refused frame not counted");
    tx_refuse = 0;

    /* DHCP stop: a RELEASE, unicast to the server, so lwIP asks for its MAC first and sends the RELEASE on the ARP
     * reply (it holds the packet, though the address is gone by then); the address is gone at once */
    ntx = 0;
    fm1wifi_net_dhcp_stop();
    CHECK(ntx == 1, "DHCP stop sent %u frames, want an ARP request for the server", ntx);
    arp_sent(0, BCAST, 1, ZERO6, IP_ROUTER);
    arp(2, MAC, ROUTER, IP_ROUTER, MAC, IP_US);
    CHECK(ntx == 2, "no RELEASE after the server's ARP reply (%u frames)", ntx);
    CHECK(dhcp_sent(1, ROUTER, IP_US, IP_ROUTER, &xr) == 7, "DHCP stop did not send a RELEASE");
    CHECK(fm1wifi_net_state() == FM1WIFI_NET_LINK, "state after DHCP stop: %d", fm1wifi_net_state());
    fm1wifi_net_addr(addr, NULL, NULL);
    CHECK(memcmp(addr, IP_ZERO, 4) == 0, "an address after DHCP stop");

    /* down: nothing in, nothing out; then a restart on another AP asks again, to the new BSSID */
    CHECK(fm1wifi_net_dhcp_start() == FM1WIFI_NET_OK, "DHCP restart");
    ntx = 0;
    fm1wifi_net_down();
    CHECK(fm1wifi_net_state() == FM1WIFI_NET_DOWN, "state after down");
    CHECK(fm1wifi_net_dhcp_start() == FM1WIFI_NET_ESTATE, "DHCP while down");
    CHECK(fm1wifi_net_ping(IP_ROUTER, 1, 1, 8) == FM1WIFI_NET_ESTATE && fm1wifi_net_arp_request(IP_ROUTER) == FM1WIFI_NET_ESTATE,
          "ping or ARP while down");
    arp(1, BCAST, PINGER, IP_PINGER, ZERO6, IP_US);
    run(10000);
    CHECK(ntx == 0, "%u frames sent while down", ntx);
    CHECK(fm1wifi_net_init(MAC, BSSID2, hook_tx, hook_eapol, &eap_calls) == FM1WIFI_NET_OK, "restart");
    CHECK(fm1wifi_net_dhcp_start() == FM1WIFI_NET_OK, "DHCP after restart");
    AP = BSSID2;
    CHECK(dhcp_sent(0, BCAST, IP_ZERO, IP_BCAST, &xid) == 1, "no DISCOVER to the new BSSID after restart");
    ntx = 0;
    dhcp_reply(2, xid, 1);
    CHECK(dhcp_sent(0, BCAST, IP_ZERO, IP_BCAST, &xr) == 3, "no REQUEST after restart");
    dhcp_reply(5, xid, 0);
    CHECK(fm1wifi_net_state() == FM1WIFI_NET_UP, "no address after restart");

    /* 300 pings, every third chained: all answered, so lwIP's pools and heap do not run dry */
    arp(1, BCAST, PINGER, IP_PINGER, ZERO6, IP_US);
    unsigned answered = 0;
    for (unsigned k = 0; k < 300; k++) {
        unsigned sz = k % 3 ? 64 : 1472 - 8;
        ntx = 0;
        n = echo_req(pkt, IP_PINGER, 0x4321, k, sz, 0);
        rx(MAC, PINGER, 0x0800, pkt, n, k & 1);
        run(100);
        if (ntx == 1) { answered++; if (k % 50 == 0) echo_reply_sent(0, PINGER, IP_PINGER, 0x4321, k, sz); }
    }
    CHECK(answered == 300, "%u of 300 pings answered", answered);
    printf("restart on another AP: DHCP again; 300 pings (100 of 1,500 bytes) answered\n");
    fm1wifi_net_get_counts(&c1);
    printf("counts: rx %u (ip %u, eapol %u, dropped %u), tx %u (refused %u)\n", (unsigned)c1.rx, (unsigned)c1.rx_ip,
           (unsigned)c1.rx_eapol, (unsigned)c1.rx_dropped, (unsigned)c1.tx, (unsigned)c1.tx_err);

    if (fails) { printf("wifi_net: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("wifi_net: %d checks passed\n", checks);
    return 0;
}
