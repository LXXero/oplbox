/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi_net: lwIP's network interface on the WiFi station link (fm1wifi_net.h; docs/WIFI.md stage 7). The frame
 * formats are IEEE 802.11-2020 9.3.2.1 (data frames: the address fields by ToDS/FromDS, table 9-30; QoS control;
 * HT control when the Order bit is set in a QoS data frame) and RFC 1042 (IP and ARP over 802 networks: the SNAP
 * header AA AA 03 00 00 00 and the ethertype). lwIP sees an Ethernet interface; this file turns its frames into
 * 802.11 data frames and back.
 */
#include "fm1wifi_net.h"
#include "fm1wifi_port.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip/prot/dhcp.h"
#include "netif/ethernet.h"

#define HDR 24u                                   /* the 802.11 data header without QoS: FC, duration, 3 addresses, seq */
#define SNAP 8u                                   /* AA AA 03 00 00 00 + ethertype */
#define ETH 14u
#define ENCAP (HDR + SNAP - ETH)                  /* = PBUF_LINK_ENCAPSULATION_HLEN in lwipopts.h */
#define ETHERTYPE_EAPOL 0x888Eu

#if ENCAP != PBUF_LINK_ENCAPSULATION_HLEN
#error "lwipopts.h: PBUF_LINK_ENCAPSULATION_HLEN must be 18 (802.11 + SNAP header in place of the Ethernet header)"
#endif

static struct netif nif;
static struct dhcp dhcp_st;
static uint8_t lwip_ready, running, dhcp_on;
static uint8_t own[6], bss[6];
static fm1wifi_net_tx_fn tx_hook;
static fm1wifi_net_eapol_fn eapol_hook;
static void *hook_ctx;
static uint16_t seq;
static uint32_t rnd = 1;
static fm1wifi_net_counts cnt;

static void cpy(uint8_t *d, const uint8_t *s, unsigned n) { while (n--) *d++ = *s++; }
static int same(const uint8_t *a, const uint8_t *b) {
    for (unsigned i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* lwIP's LWIP_RAND (arch/cc.h): DHCP's transaction id. xorshift32, seeded from the MAC and the clock at init. */
uint32_t fm1wifi_net_rand(void) {
    rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
    return rnd;
}

/* The 802.11 data header and SNAP header for a frame to the AP: f gets HDR + SNAP bytes. */
static void put_hdr(uint8_t *f, const uint8_t dst[6], unsigned type) {
    f[0] = 0x08; f[1] = 0x01;                     /* data, subtype 0; ToDS */
    f[2] = f[3] = 0;                              /* duration: the hardware's */
    cpy(f + 4, bss, 6); cpy(f + 10, own, 6); cpy(f + 16, dst, 6);
    f[22] = (uint8_t)(seq << 4); f[23] = (uint8_t)(seq >> 4);   /* sequence number, fragment 0 */
    seq = (uint16_t)((seq + 1) & 0x0FFF);
    f[24] = 0xAA; f[25] = 0xAA; f[26] = 0x03; f[27] = f[28] = f[29] = 0;
    f[30] = (uint8_t)(type >> 8); f[31] = (uint8_t)type;
}

static int tx_frame(const uint8_t *f, unsigned len) {
    cnt.tx++;
    if (tx_hook(f, len, hook_ctx) == 0) return FM1WIFI_NET_OK;
    cnt.tx_err++;
    return FM1WIFI_NET_ETX;
}

/* lwIP's linkoutput: p is an Ethernet frame. A frame in one pbuf (lwIP builds most so) gets the 802.11 header written
 * in the room PBUF_LINK_ENCAPSULATION_HLEN left in front of it and goes out from there; afterwards the pbuf is put
 * back as lwIP handed it (header pointer and Ethernet header). A chained frame is copied into one PBUF_RAM pbuf
 * from lwIP's heap first. */
static err_t linkoutput(struct netif *n, struct pbuf *p) {
    (void)n;
    uint8_t eth[ETH];
    if (!running) return ERR_IF;
    if (p->tot_len < ETH || p->tot_len - ETH > FM1WIFI_NET_MTU) { cnt.tx_err++; return ERR_VAL; }
    pbuf_copy_partial(p, eth, ETH, 0);
    unsigned len = ENCAP + p->tot_len;
    struct pbuf *q = p;
    if (p->next != NULL || p->len != p->tot_len || pbuf_add_header(p, ENCAP) != 0) {
        q = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_RAM);
        if (q == NULL) { cnt.tx_err++; return ERR_MEM; }
        pbuf_copy_partial(p, (uint8_t *)q->payload + HDR + SNAP, (u16_t)(p->tot_len - ETH), ETH);
    }
    put_hdr(q->payload, eth, (unsigned)eth[12] << 8 | eth[13]);
    int r = tx_frame(q->payload, len);
    if (q == p) {
        pbuf_remove_header(p, ENCAP);
        cpy(p->payload, eth, ETH);
    } else {
        pbuf_free(q);
    }
    return r == FM1WIFI_NET_OK ? ERR_OK : ERR_IF;
}

static err_t nif_init(struct netif *n) {
    n->name[0] = 'w'; n->name[1] = 'l';
    n->output = etharp_output;
    n->linkoutput = linkoutput;
    n->mtu = FM1WIFI_NET_MTU;
    n->hwaddr_len = ETH_HWADDR_LEN;
    cpy(n->hwaddr, own, 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

static void stop_dhcp(void) {
    if (dhcp_on) dhcp_release_and_stop(&nif);    /* a DHCPRELEASE when it had a lease and the link is up */
    dhcp_on = 0;
}

int fm1wifi_net_init(const uint8_t mac[6], const uint8_t bssid[6], fm1wifi_net_tx_fn tx,
                     fm1wifi_net_eapol_fn eapol, void *ctx) {
    if (mac == NULL || bssid == NULL || tx == NULL || (mac[0] & 1)) return FM1WIFI_NET_ERANGE;
    if (lwip_ready) fm1wifi_net_down();
    cpy(own, mac, 6); cpy(bss, bssid, 6);
    tx_hook = tx; eapol_hook = eapol; hook_ctx = ctx;
    rnd = sys_now() ^ ((uint32_t)mac[2] << 24 | (uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | mac[5]);
    if (rnd == 0) rnd = 1;
    if (!lwip_ready) {
        lwip_init();
        if (netif_add(&nif, NULL, NULL, NULL, NULL, nif_init, ethernet_input) == NULL) return FM1WIFI_NET_ESTATE;
        netif_set_default(&nif);
        dhcp_set_struct(&nif, &dhcp_st);
        lwip_ready = 1;
    } else {
        cpy(nif.hwaddr, own, 6);
    }
    running = 1;
    netif_set_up(&nif);
    netif_set_link_up(&nif);
    return FM1WIFI_NET_OK;
}

void fm1wifi_net_down(void) {
    if (!lwip_ready) return;
    running = 0;                                  /* nothing goes out from here on, the DHCPRELEASE neither */
    stop_dhcp();
    netif_set_link_down(&nif);
    netif_set_down(&nif);                         /* also empties the ARP table */
    netif_set_addr(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
}

void fm1wifi_net_input(const uint8_t *f, unsigned len) {
    cnt.rx++;
    if (!running || f == NULL || len < HDR) goto drop;
    unsigned sub = f[0] >> 4, fc1 = f[1];
    if ((f[0] & 0x0F) != 0x08 || (sub != 0 && sub != 8)) goto drop;   /* version 0, data or QoS data */
    if ((fc1 & 0x03) != 0x02) goto drop;          /* FromDS only */
    if (fc1 & 0x44) goto drop;                    /* more fragments, protected */
    if (f[22] & 0x0F) goto drop;                  /* a fragment number */
    const uint8_t *a1 = f + 4, *a2 = f + 10, *a3 = f + 16;
    if (!(a1[0] & 1) && !same(a1, own)) goto drop;
    if (!same(a2, bss) || same(a3, own)) goto drop;
    unsigned h = HDR;
    if (sub == 8) {
        if (len < HDR + 2 || (f[HDR] & 0x80)) goto drop;   /* QoS control; A-MSDU present */
        h += 2;
        if (fc1 & 0x80) h += 4;                   /* +HTC */
    }
    if (len < h + SNAP) goto drop;
    const uint8_t *s = f + h;
    if (s[0] != 0xAA || s[1] != 0xAA || s[2] != 0x03 || s[3] || s[4] || s[5]) goto drop;
    unsigned type = (unsigned)s[6] << 8 | s[7], n = len - h - SNAP;
    if (type == ETHERTYPE_EAPOL) {
        if (eapol_hook == NULL) goto drop;
        cnt.rx_eapol++;
        eapol_hook(a3, s + SNAP, n, hook_ctx);
        return;
    }
    if (n > FM1WIFI_NET_MTU) goto drop;
    struct pbuf *p = pbuf_alloc(PBUF_RAW_TX, (u16_t)(ETH + n), PBUF_POOL);   /* room in front for a reply */
    if (p == NULL) goto drop;
    uint8_t eth[ETH];
    cpy(eth, a1, 6); cpy(eth + 6, a3, 6); eth[12] = s[6]; eth[13] = s[7];
    pbuf_take(p, eth, ETH);
    pbuf_take_at(p, s + SNAP, (u16_t)n, ETH);
    cnt.rx_ip++;
    if (nif.input(p, &nif) != ERR_OK) pbuf_free(p);
    return;
drop:
    cnt.rx_dropped++;
}

void fm1wifi_net_poll(void) {
    if (lwip_ready) sys_check_timeouts();
}

int fm1wifi_net_dhcp_start(void) {
    if (!running) return FM1WIFI_NET_ESTATE;
    err_t e = dhcp_start(&nif);
    if (e == ERR_MEM) return FM1WIFI_NET_EMEM;
    if (e != ERR_OK) return FM1WIFI_NET_ESTATE;
    dhcp_on = 1;
    return FM1WIFI_NET_OK;
}

void fm1wifi_net_dhcp_stop(void) {
    if (lwip_ready) stop_dhcp();
}

int fm1wifi_net_eapol_send(const uint8_t *body, unsigned len) {
    if (!running) return FM1WIFI_NET_ESTATE;
    if (body == NULL || len == 0 || len > FM1WIFI_NET_MTU) return FM1WIFI_NET_ERANGE;
    struct pbuf *q = pbuf_alloc(PBUF_RAW, (u16_t)(HDR + SNAP + len), PBUF_RAM);
    if (q == NULL) { cnt.tx_err++; return FM1WIFI_NET_EMEM; }
    uint8_t *f = q->payload;
    put_hdr(f, bss, ETHERTYPE_EAPOL);
    cpy(f + HDR + SNAP, body, len);
    int r = tx_frame(f, HDR + SNAP + len);
    pbuf_free(q);
    return r;
}

int fm1wifi_net_state(void) {
    if (!running) return FM1WIFI_NET_DOWN;
    if (!ip4_addr_isany_val(*netif_ip4_addr(&nif))) return FM1WIFI_NET_UP;
    return dhcp_on && dhcp_st.state != DHCP_STATE_OFF ? FM1WIFI_NET_DHCP : FM1WIFI_NET_LINK;
}

static void put_ip(uint8_t d[4], const ip4_addr_t *a) {
    if (d == NULL) return;
    uint32_t v = running ? ip4_addr_get_u32(a) : 0;   /* network order in memory */
    cpy(d, (const uint8_t *)&v, 4);
}

void fm1wifi_net_addr(uint8_t addr[4], uint8_t mask[4], uint8_t gw[4]) {
    put_ip(addr, netif_ip4_addr(&nif));
    put_ip(mask, netif_ip4_netmask(&nif));
    put_ip(gw, netif_ip4_gw(&nif));
}

void fm1wifi_net_get_counts(fm1wifi_net_counts *c) {
    if (c != NULL) *c = cnt;
}
