/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi_ip: IPv4 on a joined WPA2 station (stage 9): fm1wifi_net.c's lwIP interface on fm1wifi_wpa.c's link, and
 * the WiFi test page's DHCP, ARP, ping and listening steps. See fm1wifi_ip.h. Own code, from RFC 2131 / 2132, RFC 792,
 * RFC 826 and RFC 1042. Not run on an FM-1 yet.
 */
#include "fm1wifi_ip.h"
#include "fm1wifi_port.h"

static void copy(uint8_t *d, const uint8_t *s, uint32_t n) { while (n--) *d++ = *s++; }
static int same(const uint8_t *a, const uint8_t *b, uint32_t n) { while (n--) if (*a++ != *b++) return 0; return 1; }
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int zero4(const uint8_t *a) { return !(a[0] | a[1] | a[2] | a[3]); }

const char *fm1wifi_dhcp_name(unsigned type)
{
    static const char *const N[] = { "?", "DISCOVER", "OFFER", "REQUEST", "DECLINE", "ACK", "NAK", "RELEASE", "INFORM" };
    return type < sizeof N / sizeof N[0] ? N[type] : "?";
}

const char *fm1wifi_ip_kind_name(unsigned kind)
{
    static const char *const D[] = { "DHCP ?", "DHCP DISCOVER", "DHCP OFFER", "DHCP REQUEST", "DHCP DECLINE", "DHCP ACK",
                                     "DHCP NAK", "DHCP RELEASE", "DHCP INFORM" };
    if (kind >= FM1WIFI_IP_KIND_DHCP && kind < FM1WIFI_IP_KIND_DHCP + 9u) return D[kind - FM1WIFI_IP_KIND_DHCP];
    return kind == FM1WIFI_IP_KIND_ARP ? "ARP" : kind == FM1WIFI_IP_KIND_ICMP ? "ICMP" : kind == FM1WIFI_IP_KIND_IP ? "IP" : "OTHER";
}

/* ---- the packets */

/* RFC 1071's sum over n bytes, added to s */
static uint32_t sum16(const uint8_t *p, unsigned n, uint32_t s)
{
    for (unsigned i = 0; i + 1u < n; i += 2u) s += get16(p + i);
    if (n & 1u) s += (uint32_t)p[n - 1u] << 8;
    return s;
}
static int sum_ok(uint32_t s) { while (s >> 16) s = (s & 0xFFFFu) + (s >> 16); return s == 0xFFFFu; }

/* an IPv4 packet with protocol proto, not a fragment: its header's length, *len the bytes it says it has (at most n) */
static unsigned ipv4(const uint8_t *ip, unsigned n, unsigned proto, unsigned *len)
{
    unsigned h, tot;
    if (!ip || n < 20u || ip[0] >> 4 != 4u || (h = (ip[0] & 0x0Fu) * 4u) < 20u || n < h) return 0;
    tot = get16(ip + 2);
    if (tot < h || tot > n || ip[9] != proto || (get16(ip + 6) & 0x3FFFu)) return 0;   /* MF or an offset: a fragment */
    *len = tot;
    return h;
}

int fm1wifi_ip_dhcp_parse(const uint8_t *ip, unsigned n, fm1wifi_dhcp_msg *m)
{
    unsigned tot, h, sp, dp, ul, dn, i;
    const uint8_t *u, *d;
    uint8_t *z = (uint8_t *)m;
    for (i = 0; i < sizeof *m; i++) z[i] = 0;
    if (!(h = ipv4(ip, n, 17u, &tot)) || tot < h + 8u) return 0;
    u = ip + h;
    sp = get16(u); dp = get16(u + 2); ul = get16(u + 4);
    if (!((sp == 67u && dp == 68u) || (sp == 68u && dp == 67u)) || ul < 8u || ul > tot - h) return 0;
    d = u + 8;
    dn = ul - 8u;
    if (dn < 240u || d[236] != 99u || d[237] != 130u || d[238] != 83u || d[239] != 99u) return 0;
    m->op = d[0];
    m->xid = get32(d + 4);
    copy(m->ciaddr, d + 12, 4); copy(m->yiaddr, d + 16, 4); copy(m->siaddr, d + 20, 4); copy(m->chaddr, d + 28, 6);
    copy(m->src, ip + 12, 4); copy(m->dst, ip + 16, 4);
    for (i = 240; i < dn;) {
        unsigned tag = d[i], l;
        const uint8_t *v;
        if (tag == 255u) break;
        if (tag == 0u) { i++; continue; }
        if (i + 2u > dn || i + 2u + d[i + 1] > dn) break;           /* cut short: the rest is not read */
        l = d[i + 1]; v = d + i + 2;
        if (tag == 53u && l == 1u) m->type = v[0];
        else if (tag == 54u && l == 4u) copy(m->server, v, 4);
        else if (tag == 1u && l == 4u) copy(m->mask, v, 4);
        else if (tag == 3u && l >= 4u) copy(m->router, v, 4);
        else if (tag == 6u && l >= 4u) {
            m->n_dns = (uint8_t)(l / 4u);
            copy(m->dns[0], v, 4);
            if (l >= 8u) copy(m->dns[1], v + 4, 4);
        }
        else if (tag == 50u && l == 4u) copy(m->requested, v, 4);
        else if (tag == 51u && l == 4u) m->lease = get32(v);
        else if (tag == 58u && l == 4u) m->t1 = get32(v);
        else if (tag == 59u && l == 4u) m->t2 = get32(v);
        i += 2u + l;
    }
    if (!m->type) { for (i = 0; i < sizeof *m; i++) z[i] = 0; return 0; }
    return 1;
}

int fm1wifi_ip_echo_parse(const uint8_t *ip, unsigned n, fm1wifi_icmp_echo *e)
{
    unsigned tot, h;
    const uint8_t *c;
    if (!(h = ipv4(ip, n, 1u, &tot)) || tot < h + 8u || !sum_ok(sum16(ip, h, 0))) return 0;
    c = ip + h;
    if ((c[0] != 0u && c[0] != 8u) || c[1] != 0u || !sum_ok(sum16(c, tot - h, 0))) return 0;
    e->type = c[0];
    e->id = (uint16_t)get16(c + 4); e->seq = (uint16_t)get16(c + 6);
    e->len = (uint16_t)(tot - h - 8u);
    copy(e->src, ip + 12, 4); copy(e->dst, ip + 16, 4);
    return 1;
}

unsigned fm1wifi_ip_data(const uint8_t *f, unsigned n, unsigned *type)
{
    static const uint8_t SNAP[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
    unsigned h = 24u;
    *type = 0;
    if (!f || n < 24u) return 0;
    if ((f[0] & 0xFCu) == FM1WIFI_FC_QOS_DATA) { h = 26u; if (f[1] & 0x80u) h = 30u; }   /* QoS control; +HTC */
    else if ((f[0] & 0xFCu) != FM1WIFI_FC_DATA) return 0;
    if ((f[1] & 3u) == 3u) return 0;                                 /* four addresses: not a station's */
    if (n < h + 8u || !same(f + h, SNAP, 6)) return 0;
    *type = get16(f + h + 6);
    return h + 8u;
}

/* ---- the driver */

#define ETH_IP 0x0800u
#define ETH_ARP 0x0806u
#define ETH_EAPOL 0x888Eu

static uint8_t rx_buf[FM1WIFI_NET_FRAME_MAX + 8u], tx_buf[FM1WIFI_NET_FRAME_MAX + 8u];

static uint32_t since(const fm1wifi_ip *ip) { return sys_now() - ip->t0; }

/* the first six frames of any kind, then the first ICMP frame in a direction none was kept in */
static void keep(fm1wifi_ip *ip, unsigned dir, unsigned kind, const uint8_t *f, unsigned n)
{
    fm1wifi_ip_rec *r;
    uint32_t t = since(ip);
    if (ip->n_rec >= FM1WIFI_IP_REC_N) return;
    if (ip->n_rec >= FM1WIFI_IP_REC_N - 2u) {
        if (kind != FM1WIFI_IP_KIND_ICMP) return;
        for (unsigned i = 0; i < ip->n_rec; i++) if (ip->rec[i].kind == FM1WIFI_IP_KIND_ICMP && ip->rec[i].dir == dir) return;
    }
    r = &ip->rec[ip->n_rec++];
    r->dir = (uint8_t)dir; r->kind = (uint8_t)kind;
    r->len = (uint16_t)n;
    r->t = (uint16_t)(t > 0xFFFFu ? 0xFFFFu : t);
    copy(r->b, f, n < FM1WIFI_IP_REC_LEN ? n : FM1WIFI_IP_REC_LEN);
}

/* What a frame either way carries (p: its payload after the SNAP header), counted as each step reads it. */
static unsigned look(fm1wifi_ip *ip, unsigned dir, unsigned type, const uint8_t *p, unsigned n)
{
    fm1wifi_dhcp_msg m;
    fm1wifi_icmp_echo e;
    if (type == ETH_ARP) { if (dir) ip->arp_tx++; else ip->arp_rx++; return FM1WIFI_IP_KIND_ARP; }
    if (type != ETH_IP) return FM1WIFI_IP_KIND_OTHER;
    if (fm1wifi_ip_dhcp_parse(p, n, &m)) {
        unsigned t = m.type < 9u ? m.type : 0u;
        if (dir) { ip->dhcp_tx[t]++; return FM1WIFI_IP_KIND_DHCP + t; }
        ip->dhcp_rx[t]++;
        if (m.op == 2u && same(m.chaddr, ip->w->sta.sa, 6)) {        /* a reply to us (not another client's) */
            if (t == FM1WIFI_DHCP_OFFER && !ip->have_offer) { ip->offer = m; ip->have_offer = 1; }
            if (t == FM1WIFI_DHCP_ACK || t == FM1WIFI_DHCP_NAK) { ip->ack = m; ip->have_ack = 1; }
        }
        return FM1WIFI_IP_KIND_DHCP + t;
    }
    if (fm1wifi_ip_echo_parse(p, n, &e)) {
        uint8_t own[4];
        fm1wifi_net_addr(own, 0, 0);
        if (dir && e.type == 0u) { ip->echo_out++; if (ip->listening) ip->listen_out++; }
        if (!dir && same(e.dst, own, 4) && !zero4(own)) {
            if (e.type == 8u) { ip->echo_in++; if (ip->listening) ip->listen_in++; }
            else if (e.id == ip->ping_id && e.seq < ip->ping_n && ip->ping_rtt[e.seq] == FM1WIFI_IP_NO_REPLY) {
                uint32_t rtt = sys_now() - ip->ping_sent_at[e.seq];
                ip->ping_rtt[e.seq] = rtt;
                if (rtt > ip->ping_wait) ip->ping_late++;
                else {
                    if (!ip->ping_got || rtt < ip->ping_min) ip->ping_min = rtt;
                    if (rtt > ip->ping_max) ip->ping_max = rtt;
                    ip->ping_sum += rtt;
                    ip->ping_got++;
                }
            }
        }
        return FM1WIFI_IP_KIND_ICMP;
    }
    return FM1WIFI_IP_KIND_IP;
}

/* lwIP's frame to the AP: protected once the PTK is loaded, the station's sequence number, then the FIFO */
static int tx_hook(const uint8_t *frame, unsigned len, void *ctx)
{
    fm1wifi_ip *ip = ctx;
    fm1wifi_wpa *w = ip->w;
    unsigned type, at, prot = (w->keys & WPA_EAPOL_NEW_PTK) != 0, kind = FM1WIFI_IP_KIND_OTHER;
    uint16_t seq = w->sta.seq++;
    int r;
    if (len > sizeof tx_buf) { ip->tx_err++; return -1; }
    copy(tx_buf, frame, len);
    if (prot) tx_buf[1] |= FM1WIFI_FC_PROTECTED >> 8;
    tx_buf[22] = (uint8_t)(seq << 4); tx_buf[23] = (uint8_t)(seq >> 4);   /* fragment 0 */
    if ((at = fm1wifi_ip_data(tx_buf, len, &type)) != 0) kind = look(ip, 1, type, tx_buf + at, len - at);
    keep(ip, 1, kind, tx_buf, len);
    ip->tx_data++;
    r = fm1wifi_send(tx_buf, len, FM1WIFI_TX_ACK | (prot ? FM1WIFI_TX_CRYPT | FM1WIFI_TX_WCID(FM1WIFI_WCID_AP) : 0u), ip->fr);
    if (r == FM1WIFI_OK) return 0;
    ip->tx_err++;
    if (ip->err == FM1WIFI_OK) ip->err = r;
    return -1;
}

/* A frame the station received: a data frame from the AP to us or a group address goes to lwIP as the MAC decrypted
 * it (header and body together, the Protected bit cleared) */
static void frame_in(fm1wifi_ip *ip, const fm1wifi_rx *f)
{
    static const uint8_t SNAP[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
    const fm1wifi_wpa *w = ip->w;
    const uint8_t *body = f->body;
    unsigned kind = FM1WIFI_FC_KIND(f->fc), blen = f->body_len, n, type, at, k;
    if ((kind != FM1WIFI_FC_DATA && kind != FM1WIFI_FC_QOS_DATA) || f->body_len < 8u) return;
    if ((f->fc & (FM1WIFI_FC_TO_DS | FM1WIFI_FC_FROM_DS)) != FM1WIFI_FC_FROM_DS || !same(f->frame + 10, w->sta.bss.bssid, 6)) return;
    if (!(f->frame[4] & 1u) && !same(f->frame + 4, w->sta.sa, 6)) return;
    if (f->fc & FM1WIFI_FC_PROTECTED) {
        uint32_t ce = (f->info & FM1WIFI_RXD_CIPHER_ERR) >> 9;
        if (ce || !(f->info & FM1WIFI_RXD_DECRYPTED)) {
            ip->rx_data++;
            if (ce == 1u) ip->rx_icv++; else if (ce == 2u) ip->rx_mic++; else if (ce == 3u) ip->rx_key++; else ip->rx_undecrypted++;
            return;
        }
        if (!same(body, SNAP, 6) && blen >= 16u && (body[3] & 0x20u) && same(body + 8, SNAP, 6)) {
            body += 8; blen -= 8u;                                    /* the CCMP header left in: off here */
            ip->rx_iv_left++;
        }
    }
    if (!same(body, SNAP, 6)) return;
    if (get16(body + 6) == ETH_EAPOL) return;                        /* the station's (fm1wifi_wpa.c) */
    ip->rx_data++;
    if (!(f->fc & FM1WIFI_FC_PROTECTED) && (w->keys & WPA_EAPOL_NEW_PTK)) { ip->rx_clear++; return; }
    n = f->hdr_len + blen;
    if (n > sizeof rx_buf) return;
    copy(rx_buf, f->frame, f->hdr_len);
    copy(rx_buf + f->hdr_len, body, blen);
    rx_buf[1] &= (uint8_t)~(FM1WIFI_FC_PROTECTED >> 8);
    k = FM1WIFI_IP_KIND_OTHER;
    if ((at = fm1wifi_ip_data(rx_buf, n, &type)) != 0) k = look(ip, 0, type, rx_buf + at, n - at);
    keep(ip, 0, k, rx_buf, n);
    ip->rx_net++;
    if (f->frame[4] & 1u) ip->rx_group++;
    fm1wifi_net_input(rx_buf, n);
}

int fm1wifi_ip_start(fm1wifi_ip *ip, fm1wifi_wpa *w, fm1wifi_frames *fr, void (*idle)(void *), void *idle_arg)
{
    uint8_t *z = (uint8_t *)ip;
    int r;
    for (unsigned i = 0; i < sizeof *ip; i++) z[i] = 0;
    ip->w = w; ip->fr = fr; ip->idle = idle; ip->idle_arg = idle_arg;
    ip->t0 = sys_now();
    for (unsigned i = 0; i < FM1WIFI_IP_PING_N; i++) ip->ping_rtt[i] = FM1WIFI_IP_NO_REPLY;
    if (w->state != FM1WIFI_WPA_UP || !fm1wifi_link_is_up()) return FM1WIFI_ESTATE;
    r = fm1wifi_net_init(w->sta.sa, w->sta.bss.bssid, tx_hook, 0, ip);   /* EAPOL: the station's, not lwIP's */
    if (r != FM1WIFI_NET_OK) { ip->net_err = r; return FM1WIFI_ESTATE; }
    return FM1WIFI_OK;
}

#define FRAMES_PER_WAIT 50u       /* frames taken without a wait between them, at most (as fm1wifi.c) */

/* Run the station and lwIP until done(ip), at most ms (sys_now, or the library's clock if that runs out first).
 * FM1WIFI_OK when done, ETIMEOUT, ESTATE when the station is no longer UP, or the first FIFO / send error. */
static int wait_for(fm1wifi_ip *ip, unsigned ms, int (*done)(fm1wifi_ip *))
{
    uint32_t t0 = sys_now(), l0 = fm1wifi_now_ms(), idle_at = t0, burst = 0;
    fm1wifi_rx f;
    for (;;) {
        int k = fm1wifi_wpa_poll(ip->w, &f, ip->fr);
        if (k < 0) { if (ip->err == FM1WIFI_OK) ip->err = k; return k; }
        if (k && f.frame) frame_in(ip, &f);
        fm1wifi_net_poll();
        if (ip->err != FM1WIFI_OK) return ip->err;
        if (ip->w->state != FM1WIFI_WPA_UP) return FM1WIFI_ESTATE;
        if (done && done(ip)) return FM1WIFI_OK;
        if (sys_now() - t0 >= ms || fm1wifi_now_ms() - l0 >= ms) return FM1WIFI_ETIMEOUT;
        if (ip->idle && sys_now() - idle_at >= FM1WIFI_IP_IDLE_MS) { idle_at = sys_now(); ip->idle(ip->idle_arg); }
        if (k && burst++ < FRAMES_PER_WAIT) continue;
        burst = 0;
        fm1wifi_wait_us(100);
    }
}

static int bound(fm1wifi_ip *ip) { (void)ip; return fm1wifi_net_state() == FM1WIFI_NET_UP; }

int fm1wifi_ip_dhcp(fm1wifi_ip *ip, unsigned ms)
{
    uint32_t t0 = sys_now();
    int r = fm1wifi_net_dhcp_start();
    if (r != FM1WIFI_NET_OK) { ip->net_err = r; return FM1WIFI_ESTATE; }
    r = wait_for(ip, ms, bound);
    if (r == FM1WIFI_OK) {
        ip->t_dhcp = sys_now() - t0 ? sys_now() - t0 : 1u;
        fm1wifi_net_addr(ip->addr, ip->mask, ip->gw);
    }
    return r;
}

static int arp_done(fm1wifi_ip *ip) { return fm1wifi_net_arp_lookup(ip->gw, ip->gw_mac); }

int fm1wifi_ip_arp(fm1wifi_ip *ip, unsigned ms)
{
    uint32_t t0 = sys_now(), left = ms;
    int r = FM1WIFI_ETIMEOUT;
    if (zero4(ip->gw)) return FM1WIFI_ERANGE;
    while (left) {                                                    /* a request, then up to 500 ms for the reply */
        unsigned slot = left < 500u ? left : 500u;
        int e = fm1wifi_net_arp_request(ip->gw);
        if (e != FM1WIFI_NET_OK && !ip->net_err) ip->net_err = e;
        r = wait_for(ip, slot, arp_done);
        if (r != FM1WIFI_ETIMEOUT) break;
        left -= slot;
    }
    if (r == FM1WIFI_OK) { ip->arp_ok = 1; ip->t_arp = sys_now() - t0 ? sys_now() - t0 : 1u; }
    return r;
}

static int ping_back(fm1wifi_ip *ip) { return ip->ping_n && ip->ping_rtt[ip->ping_n - 1u] != FM1WIFI_IP_NO_REPLY; }

int fm1wifi_ip_ping(fm1wifi_ip *ip, unsigned n, unsigned len, unsigned gap_ms, unsigned wait_ms)
{
    int r = FM1WIFI_OK;
    if (zero4(ip->gw)) return FM1WIFI_ERANGE;
    if (n < 1u || n > FM1WIFI_IP_PING_N || len > FM1WIFI_NET_PING_MAX) return FM1WIFI_ERANGE;
    ip->ping_id = (uint16_t)(0x4F50u ^ (ip->t0 & 0xFFu));
    ip->ping_wait = wait_ms;
    for (unsigned i = 0; i < n; i++) {
        uint32_t at = sys_now();
        int e;
        ip->ping_sent_at[i] = at;
        ip->ping_n = (uint8_t)(i + 1u);
        e = fm1wifi_net_ping(ip->gw, ip->ping_id, (uint16_t)i, len);
        if (e != FM1WIFI_NET_OK && !ip->net_err) ip->net_err = e;
        r = wait_for(ip, wait_ms, ping_back);
        if (r != FM1WIFI_OK && r != FM1WIFI_ETIMEOUT) return r;
        if (i + 1u < n && sys_now() - at < gap_ms) {                  /* the rest of the gap, still serving frames */
            r = wait_for(ip, gap_ms - (sys_now() - at), 0);
            if (r != FM1WIFI_ETIMEOUT) return r;
        }
    }
    return FM1WIFI_OK;
}

static int listen_done(fm1wifi_ip *ip) { return ip->listen_enough && ip->listen_out >= ip->listen_enough; }

int fm1wifi_ip_listen(fm1wifi_ip *ip, unsigned ms, unsigned enough)
{
    int r;
    ip->listen_at = sys_now();
    ip->listen_ms = ms;
    ip->listen_enough = enough;
    ip->listening = 1;
    r = wait_for(ip, ms, listen_done);
    ip->listening = 0;
    return r == FM1WIFI_ETIMEOUT ? FM1WIFI_OK : r;
}

uint32_t fm1wifi_ip_listen_left(const fm1wifi_ip *ip)
{
    uint32_t t = sys_now() - ip->listen_at;
    return ip->listening && t < ip->listen_ms ? ip->listen_ms - t : 0u;
}

void fm1wifi_ip_stop(fm1wifi_ip *ip)
{
    if (ip->w && ip->w->state == FM1WIFI_WPA_UP) fm1wifi_net_dhcp_stop();   /* the DHCPRELEASE goes out now */
    fm1wifi_net_down();
}
