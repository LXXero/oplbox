/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of IP on the WPA2 station (stage 9, firmware/wifi/fm1wifi_ip.c, docs/WIFI_STAGE9.md).
 *   1. The packet readers alone: a DHCP message's fields (every option the page shows, cut-short options, the
 *      refusals: another port, a fragment, no magic cookie, no message type), ICMP echo (checksums), the data frame
 *      payload (data, QoS data, QoS with HT control; null and four-address frames refused).
 *   2. The driver against the register model (tests/wifi/sdio_model.c), whose WPA2 AP has a router behind it
 *      (sdio_model.h "ap ... net="): joined with the 4-way handshake, then DHCP (the OFFER broadcast under the GTK, the
 *      ACK unicast under the PTK), ARP for the gateway, ten pings, a host that ARPs for us and pings us while we
 *      listen, a group key handshake in the middle, the DHCPRELEASE at the end. Every frame we send must reach the AP
 *      encrypted with the TK (the model refuses anything else), and every one the AP sends reaches lwIP only as the
 *      MAC decrypted it. Then what fails says so: a silent DHCP server, a NAK, no ping replies, replies after the wait,
 *      a wrong GTK in the key table (the broadcast OFFER does not decrypt), frames in the clear (dropped), a CCMP
 *      header the core left in (taken off), the AP deauthenticating during DHCP.
 *   wifi_ip <dir with core_model.txt>
 * Exits 1 when a check fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fm1wifi.h"
#include "fm1wifi_ip.h"
#include "sdio_model.h"
#include "fm1wifi_port.h"

static int bad, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fprintf(stderr, "FAIL: %s\n", what); bad = 1; } }

static void w16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static unsigned sum(const uint8_t *p, unsigned n, uint32_t s)
{
    for (unsigned i = 0; i + 1 < n; i += 2) s += (uint32_t)p[i] << 8 | p[i + 1];
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return ~s & 0xFFFF;
}
static void iphdr(uint8_t *p, unsigned proto, unsigned payload)
{
    static const uint8_t src[4] = { 10, 0, 0, 1 }, dst[4] = { 10, 0, 0, 9 };
    memset(p, 0, 20);
    p[0] = 0x45; w16(p + 2, 20 + payload); p[8] = 64; p[9] = (uint8_t)proto;
    memcpy(p + 12, src, 4); memcpy(p + 16, dst, 4);
    w16(p + 10, sum(p, 20, 0));
}

/* ---- 1. the packet readers */

/* a DHCP OFFER from 67 to 68 with options; returns the packet's length */
static unsigned offer(uint8_t *p, const uint8_t *opts, unsigned no)
{
    uint8_t *u = p + 20, *d = u + 8;
    memset(p, 0, 600);
    d[0] = 2; d[1] = 1; d[2] = 6; d[4] = 0xA1; d[5] = 0xB2; d[6] = 0xC3; d[7] = 0xD4;
    d[16] = 10; d[17] = 0; d[18] = 0; d[19] = 9; d[20] = 10; d[23] = 1;
    for (int i = 0; i < 6; i++) d[28 + i] = (uint8_t)(0x10 + i);
    d[236] = 99; d[237] = 130; d[238] = 83; d[239] = 99;
    memcpy(d + 240, opts, no);
    unsigned ul = 8 + 240 + no;
    w16(u, 67); w16(u + 2, 68); w16(u + 4, ul);
    iphdr(p, 17, ul);
    return 20 + ul;
}

static void test_readers(void)
{
    static const uint8_t opts[] = { 53, 1, 2, 0, 0, 54, 4, 10, 0, 0, 1, 51, 4, 0, 1, 0x51, 0x80, 1, 4, 255, 255, 255, 0,
                                    3, 8, 10, 0, 0, 1, 10, 0, 0, 2, 6, 12, 10, 0, 0, 1, 8, 8, 8, 8, 9, 9, 9, 9,
                                    58, 4, 0, 0, 0xA8, 0xC0, 59, 4, 0, 1, 0x27, 0x50, 255 };
    uint8_t p[600], f[200];
    fm1wifi_dhcp_msg m;
    fm1wifi_icmp_echo e;
    unsigned n = offer(p, opts, sizeof opts), t;
    static const uint8_t ten9[4] = { 10, 0, 0, 9 }, ten1[4] = { 10, 0, 0, 1 }, ten2[4] = { 10, 0, 0, 2 }, m24[4] = { 255, 255, 255, 0 };
    static const uint8_t g8[4] = { 8, 8, 8, 8 };
    check(fm1wifi_ip_dhcp_parse(p, n, &m) == 1 && m.op == 2 && m.type == FM1WIFI_DHCP_OFFER && m.xid == 0xA1B2C3D4u,
          "dhcp: an OFFER, its xid");
    check(!memcmp(m.yiaddr, ten9, 4) && !memcmp(m.siaddr, ten1, 4) && m.chaddr[0] == 0x10 && m.chaddr[5] == 0x15,
          "dhcp: yiaddr, siaddr, chaddr");
    check(!memcmp(m.server, ten1, 4) && !memcmp(m.mask, m24, 4) && !memcmp(m.router, ten1, 4) && m.lease == 86400u &&
          m.t1 == 43200u && m.t2 == 75600u, "dhcp: server, mask, the first router, lease, T1, T2");
    check(m.n_dns == 3 && !memcmp(m.dns[0], ten1, 4) && !memcmp(m.dns[1], g8, 4), "dhcp: three DNS servers, the first two kept");
    (void)ten2;
    check(!strcmp(fm1wifi_dhcp_name(m.type), "OFFER") && !strcmp(fm1wifi_ip_kind_name(FM1WIFI_IP_KIND_DHCP + 5), "DHCP ACK"),
          "dhcp: the names");
    /* cut short in the middle of an option: what came before is read */
    n = offer(p, opts, 20);
    check(fm1wifi_ip_dhcp_parse(p, n, &m) == 1 && m.lease == 86400u && m.mask[0] == 0, "dhcp: an option cut short ends the reading");
    n = offer(p, opts, sizeof opts);
    p[20 + 1] = 69;                                                   /* to port 69 */
    check(fm1wifi_ip_dhcp_parse(p, n, &m) == 0 && m.type == 0, "dhcp: another UDP port: not DHCP");
    n = offer(p, opts, sizeof opts);
    p[6] = 0x20;                                                      /* more fragments */
    check(fm1wifi_ip_dhcp_parse(p, n, &m) == 0, "dhcp: a fragment: not read");
    n = offer(p, opts, sizeof opts);
    p[28 + 236] = 98;
    check(fm1wifi_ip_dhcp_parse(p, n, &m) == 0, "dhcp: no magic cookie");
    {   static const uint8_t no53[] = { 54, 4, 10, 0, 0, 1, 255 };
        n = offer(p, no53, sizeof no53);
        check(fm1wifi_ip_dhcp_parse(p, n, &m) == 0, "dhcp: no message type"); }
    n = offer(p, opts, sizeof opts);
    check(fm1wifi_ip_dhcp_parse(p, 200, &m) == 0, "dhcp: a packet shorter than its IPv4 length");

    /* ICMP echo */
    uint8_t *c = p + 20;
    memset(p, 0, 100);
    c[0] = 8; w16(c + 4, 0x1234); w16(c + 6, 7);
    for (int i = 0; i < 16; i++) c[8 + i] = (uint8_t)i;
    w16(c + 2, sum(c, 24, 0));
    iphdr(p, 1, 24);
    check(fm1wifi_ip_echo_parse(p, 44, &e) == 1 && e.type == 8 && e.id == 0x1234 && e.seq == 7 && e.len == 16 && e.dst[3] == 9,
          "icmp: an echo request");
    c[9] ^= 1;
    check(fm1wifi_ip_echo_parse(p, 44, &e) == 0, "icmp: a wrong ICMP checksum refused");
    c[9] ^= 1; p[10] ^= 1;
    check(fm1wifi_ip_echo_parse(p, 44, &e) == 0, "icmp: a wrong IPv4 header checksum refused");
    p[10] ^= 1; c[0] = 3; w16(c + 2, 0); w16(c + 2, sum(c, 24, 0));
    check(fm1wifi_ip_echo_parse(p, 44, &e) == 0, "icmp: not an echo (type 3)");

    /* data frames */
    static const uint8_t snap[8] = { 0xAA, 0xAA, 3, 0, 0, 0, 0x08, 0x06 };
    memset(f, 0, sizeof f);
    f[0] = 0x08; f[1] = 0x02; memcpy(f + 24, snap, 8);
    check(fm1wifi_ip_data(f, 60, &t) == 32 && t == 0x0806, "data: a data frame's payload after 24 + 8");
    f[0] = 0x88; memmove(f + 26, snap, 8); f[24] = f[25] = 0;
    check(fm1wifi_ip_data(f, 60, &t) == 34 && t == 0x0806, "data: QoS data: 26 + 8");
    f[1] = 0x82; memmove(f + 30, snap, 8); memset(f + 26, 0, 4);
    check(fm1wifi_ip_data(f, 60, &t) == 38, "data: QoS data with HT control (Order): 30 + 8");
    f[0] = 0xC8;
    check(fm1wifi_ip_data(f, 60, &t) == 0, "data: a QoS null frame refused");
    f[0] = 0x08; f[1] = 0x03;
    check(fm1wifi_ip_data(f, 60, &t) == 0, "data: four addresses refused");
    f[1] = 0x02; f[24] = 0xAB;
    check(fm1wifi_ip_data(f, 60, &t) == 0, "data: no SNAP header refused");
}

/* ---- 2. the driver against the register model */

static const uint8_t ME[6] = { 0x02, 0x46, 0x4D, 0x31, 0xAB, 0xCD };
static const char *dir;
static fm1wifi_frames fr;
static fm1wifi_wpa w;
static fm1wifi_ip ip;
static int idles;
static void idle(void *arg) { (void)arg; idles++; }

static int nonce_cb(void *arg, uint8_t out[32])
{
    static int k;
    (void)arg;
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(0x50 + i + k);
    k++;
    return 0;
}

/* the model with this AP line, the radio and link up, XNET found and joined (UP) */
static sdio_model *joined(const char *ap)
{
    char path[512], script[512];
    fm1wifi_radio r;
    fm1wifi_scan_list list;
    const fm1wifi_bss *b;
    uint8_t pmk[32];
    struct wpa_pmk_job j;
    sdio_model *m = sdio_model_new();
    snprintf(path, sizeof path, "%s/core_model.txt", dir);
    snprintf(script, sizeof script, "reg 0x3043c 0xe0\nap ch=6 bssid=02:00:00:00:00:06 ssid=XNET sec=wpa2 psk=correct_horse rssi=-57 %s\n", ap);
    if (sdio_model_load(m, path, stderr) || sdio_model_script(m, script, "test", stderr)) exit(1);
    sdio_model_raw_log(m, 0);
    sdio_model_wlog_on(m, 0);
    sdio_model_use(m);
    if (fm1wifi_radio_up(&fm1wifi_cal_default, &r) != FM1WIFI_OK || fm1wifi_link_up(ME, 1) != FM1WIFI_OK) { fprintf(stderr, "FAIL: radio and link up\n"); exit(1); }
    memset(&fr, 0, sizeof fr);
    memset(&w, 0, sizeof w);
    fm1wifi_scan_clear(&list);
    fm1wifi_scan_channel(6, 120, 0, 0, &list, &fr);
    if (!(b = fm1wifi_scan_find(&list, "XNET"))) { fprintf(stderr, "FAIL: XNET not heard\n"); exit(1); }
    wpa_pmk_begin(&j, "correct_horse", 13, (const uint8_t *)"XNET", 4);
    while (!wpa_pmk_step(&j, 4096)) {}
    wpa_pmk_take(&j, pmk);
    if (fm1wifi_wpa_join(&w, b, pmk, nonce_cb, 0, 7000, &fr) != FM1WIFI_OK || w.state != FM1WIFI_WPA_UP) { fprintf(stderr, "FAIL: not joined\n"); exit(1); }
    idles = 0;
    check(fm1wifi_ip_start(&ip, &w, &fr, idle, 0) == FM1WIFI_OK, "driver: IP started on the joined station");
    return m;
}

static void done(sdio_model *m)
{
    fm1wifi_ip_stop(&ip);
    fm1wifi_wpa_leave_now(&w, &fr);
    fm1wifi_power_down();
    check(sdio_model_bad(m) == 0, "driver: no access outside the WiFi blocks");
    sdio_model_free(m);
}

/* the sequence numbers of the station's frames after the 4-way handshake's msg 4 (the model's frame log): each one more
 * than the one before (management, EAPOL and data frames share the station's counter) */
static int seq_in_order(sdio_model *m, int *n)
{
    const wifi_fix *fx = sdio_model_frames(m);
    int from = -1, ok = 1;
    unsigned last = 0;
    *n = 0;
    for (int i = 0; i < fx->n; i++) {
        const wifi_rec *r = &fx->r[i];
        if (r->write && r->n > 40 && (r->data[0] & 0xFC) == 0x08 && r->data[30] == 0x88 && r->data[31] == 0x8E) from = i;
    }
    for (int i = from + 1; from >= 0 && i < fx->n; i++) {
        const wifi_rec *r = &fx->r[i];
        unsigned sq;
        if (!r->write || r->n < 24 || (r->data[0] & 0x0C) == 0x04) continue;   /* control frames have none */
        sq = (r->data[22] | (unsigned)r->data[23] << 8) >> 4;
        if (*n && sq != ((last + 1) & 0xFFF)) ok = 0;
        last = sq;
        (*n)++;
    }
    return ok;
}

/* the station's data frames in the model's frame log after the 4-way handshake: all with the Protected bit */
static int all_sent_protected(sdio_model *m, int *n)
{
    const wifi_fix *fx = sdio_model_frames(m);
    int ok = 1;
    *n = 0;
    for (int i = 0; i < fx->n; i++) {
        const wifi_rec *r = &fx->r[i];
        if (!r->write || r->n < 32 || (r->data[0] & 0xFC) != 0x08 || r->data[30] == 0x88) continue;   /* not EAPOL */
        (*n)++;
        if (!(r->data[1] & 0x40)) ok = 0;
    }
    return ok;
}

static void test_driver(void)
{
    static const uint8_t addr[4] = { 192, 168, 7, 23 }, gw[4] = { 192, 168, 7, 1 }, mask[4] = { 255, 255, 255, 0 };
    static const uint8_t gw_mac[6] = { 0x02, 0x47, 0x57, 0x00, 0x00, 0x01 }, one[4] = { 1, 1, 1, 1 };
    sdio_model_net ns;
    sdio_model *m;
    int done_, gdone, n;
    uint32_t refused;

    /* the whole run: DHCP, ARP, 10 pings, 3 s listening while a host pings us every 300 ms, a rekey at 600 ms */
    m = joined("net=192.168.7.1 pingin=300 rekey=600 bcast=400");
    check(fm1wifi_ip_dhcp(&ip, 10000) == FM1WIFI_OK && fm1wifi_net_state() == FM1WIFI_NET_UP, "dhcp: an address");
    check(!memcmp(ip.addr, addr, 4) && !memcmp(ip.mask, mask, 4) && !memcmp(ip.gw, gw, 4), "dhcp: 192.168.7.23/24, gateway .1");
    check(ip.dhcp_tx[FM1WIFI_DHCP_DISCOVER] == 1 && ip.dhcp_tx[FM1WIFI_DHCP_REQUEST] == 1 && ip.dhcp_rx[FM1WIFI_DHCP_OFFER] == 1 &&
          ip.dhcp_rx[FM1WIFI_DHCP_ACK] == 1, "dhcp: DISCOVER, OFFER, REQUEST, ACK once each");
    check(ip.have_offer && ip.offer.type == FM1WIFI_DHCP_OFFER && !memcmp(ip.offer.yiaddr, addr, 4) && ip.offer.dst[0] == 255,
          "dhcp: the OFFER kept, broadcast to 255.255.255.255");
    check(ip.have_ack && ip.ack.type == FM1WIFI_DHCP_ACK && !memcmp(ip.ack.server, gw, 4) && ip.ack.lease == 86400u &&
          ip.ack.n_dns == 2 && !memcmp(ip.ack.dns[0], gw, 4) && !memcmp(ip.ack.dns[1], one, 4) && !memcmp(ip.ack.dst, addr, 4),
          "dhcp: the ACK kept: server, lease, two DNS servers, unicast to our address");
    check(ip.t_dhcp >= 1 && ip.rx_group >= 1 && ip.rx_net >= 2, "dhcp: the time to an address; the OFFER came group addressed (the GTK)");
    sdio_model_ap_net(m, 0, &ns);
    check(ns.discover == 1 && ns.request == 1, "dhcp: the AP's router heard DISCOVER and REQUEST (encrypted with the TK)");

    check(fm1wifi_ip_arp(&ip, 2000) == FM1WIFI_OK && ip.arp_ok && !memcmp(ip.gw_mac, gw_mac, 6) && ip.t_arp >= 1,
          "arp: the gateway's MAC");
    check(fm1wifi_ip_ping(&ip, 10, 56, 200, 1000) == FM1WIFI_OK && ip.ping_n == 10 && ip.ping_got == 10 && ip.ping_late == 0,
          "ping: 10 sent, 10 answered");
    check(ip.ping_max <= 1 && ip.ping_sum <= 10, "ping: the model answers at once");
    sdio_model_ap_net(m, 0, &ns);
    check(ns.ping_req == 10 && ns.ping_reply == 10 && ns.bad_icmp == 0 && ns.arp_gw >= 1, "ping: the gateway took 10 good requests");

    check(fm1wifi_ip_listen(&ip, 3000, 0) == FM1WIFI_OK && ip.listen_in >= 8 && ip.listen_out == ip.listen_in,
          "listen: the host's echo requests in 3 s, each answered");
    sdio_model_ap_net(m, 0, &ns);
    check(ns.arp_reply >= 1 && ns.pingin_got >= 8 && ns.pingin_got + 1 >= ns.pingin_sent, "listen: our ARP reply and echo replies reached the host");
    check(ip.echo_in >= ip.listen_in && ip.echo_out == ip.echo_in, "listen: every echo request of the run answered");
    check(fm1wifi_ip_listen_left(&ip) == 0, "listen: no window left afterwards");
    sdio_model_ap_wpa(m, 0, &done_, &gdone, &refused, 0);
    check(w.group_handshakes == 1 && gdone == 1 && w.state == FM1WIFI_WPA_UP, "the group key handshake during IP answered, still UP");
    check(refused == 0, "the AP refused none of our frames (all encrypted with the TK)");
    check(all_sent_protected(m, &n) && n >= 20 && (int)ip.tx_data == n && ip.tx_err == 0, "every data frame we sent had the Protected bit");
    check(seq_in_order(m, &n) && n >= 20, "our frames' sequence numbers: the station's counter, one by one");
    check(ip.rx_undecrypted == 0 && ip.rx_icv == 0 && ip.rx_mic == 0 && ip.rx_key == 0 && ip.rx_clear == 0 && ip.rx_iv_left == 0,
          "every frame from the AP decrypted by the MAC");
    check(ip.n_rec == FM1WIFI_IP_REC_N && ip.rec[0].dir == 1 && ip.rec[0].kind == FM1WIFI_IP_KIND_DHCP + FM1WIFI_DHCP_DISCOVER &&
          (ip.rec[0].b[1] & 0x40) && ip.rec[1].dir == 0 && ip.rec[1].kind == FM1WIFI_IP_KIND_DHCP + FM1WIFI_DHCP_OFFER &&
          !(ip.rec[1].b[1] & 0x40), "frames kept: DISCOVER (Protected) first, the OFFER as decrypted (bit cleared)");
    check(ip.rec[6].kind == FM1WIFI_IP_KIND_ICMP && ip.rec[7].kind == FM1WIFI_IP_KIND_ICMP && ip.rec[6].dir != ip.rec[7].dir,
          "frames kept: the first ICMP frame either way in the last two");
    check(idles >= 100, "the idle hook ran during the waits (USB)");
    fm1wifi_ip_stop(&ip);
    sdio_model_ap_net(m, 0, &ns);
    check(ns.release == 1 && fm1wifi_net_state() == FM1WIFI_NET_DOWN, "stop: a DHCPRELEASE, the interface down");
    done(m);

    /* a silent DHCP server: DISCOVER retried, no address */
    m = joined("net=10.1.1.1 dhcp=silent");
    check(fm1wifi_ip_dhcp(&ip, 5000) == FM1WIFI_ETIMEOUT && !ip.have_offer && ip.dhcp_tx[FM1WIFI_DHCP_DISCOVER] >= 2 && ip.t_dhcp == 0,
          "dhcp, silent server: DISCOVER retried, ETIMEOUT, no OFFER");
    check(fm1wifi_ip_arp(&ip, 500) == FM1WIFI_ERANGE && fm1wifi_ip_ping(&ip, 3, 56, 100, 200) == FM1WIFI_ERANGE,
          "no gateway: ARP and ping refused (ERANGE)");
    done(m);

    /* every REQUEST NAKed */
    m = joined("net=10.1.1.1 dhcp=nak");
    check(fm1wifi_ip_dhcp(&ip, 3000) == FM1WIFI_ETIMEOUT && ip.have_offer && ip.have_ack && ip.ack.type == FM1WIFI_DHCP_NAK &&
          ip.dhcp_rx[FM1WIFI_DHCP_NAK] >= 1, "dhcp, NAK: no address, the NAK kept");
    done(m);

    /* another client's OFFER first (same xid, broadcast): not ours, not kept; an address all the same */
    m = joined("net=10.1.1.1 others=1");
    check(fm1wifi_ip_dhcp(&ip, 5000) == FM1WIFI_OK && ip.dhcp_rx[FM1WIFI_DHCP_OFFER] == 2 && ip.offer.yiaddr[3] == 23 && ip.addr[3] == 23,
          "dhcp: another client's OFFER seen, not kept as ours");
    /* the gateway ignores the first ARP request: asked again 500 ms later */
    done(m);
    m = joined("net=10.1.1.1 arpdrop=1 dup=1");
    fm1wifi_ip_dhcp(&ip, 5000);
    check(fm1wifi_ip_arp(&ip, 2000) == FM1WIFI_OK && ip.t_arp >= 500 && ip.arp_tx >= 2, "arp: the first request unanswered, asked again");
    /* each echo reply twice: each counted once */
    check(fm1wifi_ip_ping(&ip, 4, 56, 100, 500) == FM1WIFI_OK && ip.ping_got == 4 && ip.ping_late == 0, "ping: duplicate replies counted once");
    done(m);

    /* a gateway that does not answer pings, then one that answers 1.5 s late */
    m = joined("net=10.1.1.1 ping=off");
    fm1wifi_ip_dhcp(&ip, 5000);
    check(fm1wifi_ip_ping(&ip, 3, 56, 100, 300) == FM1WIFI_OK && ip.ping_n == 3 && ip.ping_got == 0 &&
          ip.ping_rtt[0] == FM1WIFI_IP_NO_REPLY && ip.ping_rtt[2] == FM1WIFI_IP_NO_REPLY, "ping, no replies: 0 of 3");
    done(m);
    m = joined("net=10.1.1.1 delay=1500");
    fm1wifi_ip_dhcp(&ip, 5000);
    fm1wifi_ip_ping(&ip, 3, 56, 100, 1000);
    fm1wifi_ip_listen(&ip, 2000, 0);
    check(ip.ping_got == 0 && ip.ping_late == 3 && ip.ping_rtt[0] >= 1500 && ip.ping_rtt[0] < 1600, "ping, replies after the wait: counted late, not answered");
    done(m);

    /* a wrong GTK in the key table: the broadcast OFFER arrives with a key error, so no address */
    m = joined("net=10.1.1.1");
    {   uint8_t bad_gtk[16];
        memset(bad_gtk, 0x77, sizeof bad_gtk);
        fm1wifi_key_group(FM1WIFI_WCID_GROUP, w.gtk_idx, bad_gtk, 0); }
    check(fm1wifi_ip_dhcp(&ip, 1500) == FM1WIFI_ETIMEOUT && !ip.have_offer && ip.rx_key >= 1 && ip.rx_net == 0,
          "wrong GTK: the OFFER not decrypted (key error counted), never handed to lwIP");
    done(m);

    /* frames in the clear after the 4-way handshake: dropped, not handed to lwIP */
    m = joined("net=10.1.1.1 clear=200");
    fm1wifi_ip_dhcp(&ip, 5000);
    fm1wifi_ip_listen(&ip, 1000, 0);
    sdio_model_ap_net(m, 0, &ns);
    check(ns.clear_sent >= 4 && ip.rx_clear == ns.clear_sent, "frames in the clear: each dropped and counted");
    check(ns.arp_req == 1, "frames in the clear: lwIP never answered them (only its gratuitous ARP went out)");
    done(m);

    /* a core that leaves the CCMP header in decrypted frames: taken off, DHCP still works */
    m = joined("net=10.1.1.1 ivleft=1");
    check(fm1wifi_ip_dhcp(&ip, 5000) == FM1WIFI_OK && ip.rx_iv_left >= 2, "CCMP header left in: taken off, an address all the same");
    done(m);

    /* the AP deauthenticates during DHCP */
    m = joined("net=10.1.1.1 dhcp=silent kick=2500");
    check(fm1wifi_ip_dhcp(&ip, 5000) == FM1WIFI_ESTATE && w.state == FM1WIFI_WPA_DOWN && w.sta.why == FM1WIFI_WHY_DEAUTH,
          "deauthenticated during DHCP: ESTATE, the station DOWN");
    done(m);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fputs("usage: wifi_ip <dir with core_model.txt>\n", stderr); return 2; }
    dir = argv[1];
    test_readers();
    test_driver();
    if (bad) return 1;
    printf("wifi_ip: %d checks passed (DHCP, ARP, ping, echo answered over the WPA2 link against the model)\n", checks);
    return 0;
}
