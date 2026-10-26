/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of fm1wifi's stages 4-6 hardware side (docs/WIFI_STAGE4.md) against the register model
 * (tests/wifi/sdio_model.c) with scripted access points: the MAC's defaults and address, the RX and TX FIFO DMA
 * register by register, a passive and an active scan, joining an open network and what ends it (refusals, a
 * deauthentication, lost beacons, leaving, a rejoin), every wait bounded (a DMA that never finishes, a TX queue
 * that never has room, a bad length word), and the crash record's step during the DMA.
 *   wifi_link <dir with core_model.txt>
 * Exits 1 when a check fails (3 from the model on an unbounded wait).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fm1wifi.h"
#include "fm1wifi_port.h"
#include "sdio_model.h"

static int bad, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fprintf(stderr, "FAIL: %s\n", what); bad = 1; } }

static const char *dir;
static const uint8_t ME[6] = { 0x02, 0x46, 0x4D, 0x31, 0xAB, 0xCD };
static fm1wifi_scan_list list;
static fm1wifi_frames fr;
static fm1wifi_sta sta;

/* The made-up core (no access points of its own), the script lines given, the radio up. */
static sdio_model *core(const char *script)
{
    char path[512];
    fm1wifi_radio r;
    sdio_model *m = sdio_model_new();
    snprintf(path, sizeof path, "%s/core_model.txt", dir);
    if (sdio_model_load(m, path, stderr) || sdio_model_script(m, script, "test", stderr)) exit(1);
    sdio_model_raw_log(m, 0);
    sdio_model_use(m);
    if (fm1wifi_radio_up(&fm1wifi_cal_default, &r) != FM1WIFI_OK) { fprintf(stderr, "FAIL: radio up\n"); exit(1); }
    memset(&fr, 0, sizeof fr);
    memset(&sta, 0, sizeof sta);
    fm1wifi_scan_clear(&list);
    return m;
}

static void done(sdio_model *m)
{
    fm1wifi_power_down();
    check(!fm1wifi_link_is_up(), "power-down takes the link down");
    check(sdio_model_bad(m) == 0, "no access outside the WiFi blocks");
    sdio_model_free(m);
}

static const char *APS =
    "reg 0x3043c 0xe0\n"                                  /* PBF_DBG: the TX gate's bits 7-5 (made up) */
    "ap ch=6 bssid=02:00:00:00:00:06 ssid=OPLBOX sec=open rssi=-40\n"
    "ap ch=1 bssid=02:00:00:00:00:01 ssid=HOME sec=wpa2 rssi=-61\n"
    "ap ch=11 bssid=02:00:00:00:00:0b ssid=OLDROUTER sec=wpa rssi=-75\n"
    "ap ch=11 bssid=02:00:00:00:00:0c ssid=SECRET sec=open rssi=-55 hidden\n"
    "ap ch=3 bssid=02:00:00:00:00:03 ssid=CAFE sec=wep rssi=-80\n";

/* Writes in order, from the model's full log, for the FIFO checks. */
static int seq_has(const wifi_fix *log, int from, const uint32_t (*want)[2], int n)
{
    int k = 0;
    for (int i = from; i < log->n && k < n; i++) {
        const wifi_rec *r = &log->r[i];
        if (r->kind != WIFI_REC_WR) continue;
        if (r->addr == want[k][0] && (want[k][1] == 0xFFFFFFFFu || r->val == want[k][1])) k++;
    }
    return k == n;
}

static void link_up(void)
{
    sdio_model *m = core(APS);
    check(fm1wifi_rx_ready() == 0, "no link: nothing ready");
    check(fm1wifi_link_up(ME, 0) == FM1WIFI_ERANGE && fm1wifi_link_up(ME, 14) == FM1WIFI_ERANGE, "link up: channel 1-13");
    check(fm1wifi_link_up(ME, 1) == FM1WIFI_OK && fm1wifi_link_is_up(), "link up");
    check(sdio_model_peek(m, FM1_WIFI_CORE(RX_FILTER_CFG)) == 0x17F97u && sdio_model_peek(m, FM1_WIFI_CORE(TX_RTY_CFG)) == 0x47D01F0Fu &&
          sdio_model_peek(m, FM1_WIFI_CORE(PBF_MAX_PCNT)) == 0x1F3FBF9Fu && sdio_model_peek(m, FM1_WIFI_CORE(PWR_PIN_CFG)) == 3u &&
          sdio_model_peek(m, FM1_WIFI_CORE(BCN_OFFSET0)) == 0xF8F0E8E0u && sdio_model_peek(m, FM1_WIFI_CORE(EXP_ACK_TIME)) == 0x002400CAu,
          "the MAC's defaults (MACRegTable values)");
    check(sdio_model_peek(m, FM1_WIFI_CORE(MAC_ADDR_DW0)) == 0x314D4602u && sdio_model_peek(m, FM1_WIFI_CORE(MAC_ADDR_DW1)) == 0x00FFCDABu,
          "the MAC address in MAC_ADDR_DW0/1, unicast-to-me mask 0xFF");
    check((sdio_model_peek(m, FM1_WF_CON(7)) & 0xFFu) == 102u && sdio_model_peek(m, FM1_WIFI_CORE(TX_PIN_CFG)) == 0x00050302u,
          "tuned to channel 1 as the survey tunes");
    check(!memcmp(fm1wifi_link_mac(), ME, 6), "the link's address");
    check(fm1wifi_set_bssid((const uint8_t[]){ 1, 2, 3, 4, 5, 6 }) == FM1WIFI_OK &&
          sdio_model_peek(m, FM1_WIFI_CORE(MAC_BSSID_DW0)) == 0x04030201u && sdio_model_peek(m, FM1_WIFI_CORE(MAC_BSSID_DW1)) == 0x0605u,
          "the BSSID in MAC_BSSID_DW0/1");
    done(m);
    check(fm1wifi_link_up(ME, 1) == FM1WIFI_ESTATE && fm1wifi_tune(1) == FM1WIFI_ESTATE, "no radio: OFF");
}

static void fifo(void)
{
    static uint32_t buf32[FM1WIFI_RX_BUF / 4u];
    uint8_t *buf = (uint8_t *)buf32;
    uint32_t n = 0;
    fm1wifi_rx f;
    sdio_model *m = core(APS);
    const wifi_fix *log;
    int from;
    fm1wifi_link_up(ME, 6);
    sdio_model_raw_log(m, 1);
    for (int i = 0; i < 2000 && !fm1wifi_rx_ready(); i++) fm1wifi_port_delay_us(100);
    check(fm1wifi_rx_ready(), "a beacon arrives on channel 6 within 200 ms");
    log = sdio_model_log(m);
    from = log->n;
    check(fm1wifi_rx_read(buf + 1, FM1WIFI_RX_BUF, &n) == FM1WIFI_ERANGE && fm1wifi_rx_read(buf, FM1WIFI_RX_BUF - 1u, &n) == FM1WIFI_ERANGE,
          "RX read: a word-aligned buffer of FM1WIFI_RX_BUF bytes");
    check(fm1wifi_rx_read(buf, FM1WIFI_RX_BUF, &n) == FM1WIFI_OK && n > 24u, "RX read: one frame");
    {
        uint32_t bus = fm1wifi_port_bus(buf);
        const uint32_t want[][2] = { { FM1_WIFI_CORE(FM1_WF_REG_0324), 2 }, { FM1_WL30_HOST_04, 0xFFFFFFFFu },
                                     { FM1_WL30_HOST_18, bus }, { FM1_WL30_HOST_1C, 4 }, { FM1_WL30_HOST_04, 0xFFFFFFFFu },
                                     { FM1_WL30_HOST_04, 0xFFFFFFFFu }, { FM1_WL30_HOST_18, bus + 4u }, { FM1_WL30_HOST_1C, n - 4u },
                                     { FM1_WL30_HOST_04, 0xFFFFFFFFu } };
        check(seq_has(sdio_model_log(m), from, want, 9), "RX read: 0x0324 = 2, then a 4-byte DMA, then N + 4 bytes at buf + 4");
        check(sdio_model_peek(m, FM1_WL30_HOST_04) & FM1_WL30_04_RX_CLR, "RX done cleared (bit 11 written)");
    }
    check(fm1wifi_rx_parse(buf, n, &f) == FM1WIFI_OK && FM1WIFI_FC_KIND(f.fc) == FM1WIFI_FC_BEACON && f.rssi == -40 &&
          !memcmp(f.frame + 16, "\x02\0\0\0\0\x06", 6) && (f.info & FM1WIFI_RXD_BROADCAST),
          "the frame read is OPLBOX's beacon: RSSI, BSSID, the trailer in CPU order");
    sdio_model_raw_log(m, 0);
    done(m);
}

static void scan(void)
{
    sdio_model *m = core(APS);
    const fm1wifi_bss *b;
    fm1wifi_link_up(ME, 1);
    check(fm1wifi_scan_channel(0, 100, 0, 0, &list, &fr) == FM1WIFI_ERANGE && fm1wifi_scan_channel(1, 0, 0, 0, &list, &fr) == FM1WIFI_ERANGE &&
          fm1wifi_scan_channel(1, 1001, 0, 0, &list, &fr) == FM1WIFI_ERANGE, "scan: channel 1-13, 1-1000 ms");
    for (unsigned ch = 1; ch <= 13; ch++) check(fm1wifi_scan_channel(ch, 120, 0, 0, &list, &fr) == FM1WIFI_OK, "passive scan, one channel");
    check(list.n == 5 && fr.tx == 0 && fr.bad == 0 && fr.rx_err == FM1WIFI_OK, "passive: five networks, nothing sent");
    check(list.e[4].channel == 11 && !list.e[4].ssid_len && !list.e[4].ssid[0] && list.e[4].rssi == -55,
          "the hidden one beacons without its SSID");
    check(fr.frames == fr.beacons && fr.beacons >= 4 && fr.n_raw == FM1WIFI_RAW_N && fr.raw_ch[0] == 1, "passive: beacons only; the first four frames' bytes kept");
    check(fr.raw[0][0] || fr.raw[0][1] || fr.raw[0][2] || fr.raw[0][3], "raw: the length word");
    b = fm1wifi_scan_find(&list, "OPLBOX");
    check(b && b->channel == 6 && b->sec == FM1WIFI_SEC_OPEN && b->rssi == -40 && !b->probed, "OPLBOX: channel 6, open, -40");
    b = fm1wifi_scan_find(&list, "HOME");
    check(b && b->channel == 1 && b->sec == FM1WIFI_SEC_WPA2 && b->rsn_len == 22, "HOME: WPA2 with its RSN element");
    check(fm1wifi_scan_find(&list, "OLDROUTER") && fm1wifi_scan_find(&list, "OLDROUTER")->sec == FM1WIFI_SEC_WPA &&
          fm1wifi_scan_find(&list, "CAFE") && fm1wifi_scan_find(&list, "CAFE")->sec == FM1WIFI_SEC_WEP, "WPA and WEP");

    for (unsigned ch = 1; ch <= 13; ch++) fm1wifi_scan_channel(ch, 30, 1, "SECRET", &list, &fr);
    check(fr.tx == 26 && fr.tx_err == 0, "active: two probe requests per channel (any, SECRET)");
    check(fr.probe_resps == 5 && fr.to_us == 5 && list.n == 5, "active: five probe responses, all to our address; still five networks");
    b = fm1wifi_scan_find(&list, "SECRET");
    check(b == &list.e[4] && b->channel == 11 && b->probed, "SECRET: a probe for its SSID names the hidden network");
    check(fm1wifi_scan_find(&list, "OPLBOX")->probed && fm1wifi_scan_find(&list, "OPLBOX")->heard >= 2, "OPLBOX merged: probed and heard");
    {
        const wifi_fix *fx = sdio_model_frames(m);
        int probes = 0;
        for (int i = 0; i < fx->n; i++)
            if (fx->r[i].write && fx->r[i].data[0] == 0x40 && !memcmp(fx->r[i].data + 10, ME, 6)) probes++;
        check(probes == 26, "the model heard 26 probe requests from our address");
    }
    {
        fm1wifi_counters c;
        fm1wifi_counters_clear(&c);
        fm1wifi_scan_channel(6, 10, 1, 0, &list, &fr);
        check(fm1wifi_counters_add(&c) == FM1WIFI_OK && c.tx_ok == 1, "counters: TX_STA_CNT1's successes added up");
    }
    done(m);
}

static void join(void)
{
    sdio_model *m = core(APS);
    const fm1wifi_bss *b;
    fm1wifi_link_up(ME, 6);
    fm1wifi_scan_channel(6, 120, 0, 0, &list, &fr);
    b = fm1wifi_scan_find(&list, "OPLBOX");
    check(b != 0, "OPLBOX in the scan");
    if (!b) { done(m); return; }
    check(fm1wifi_join(&sta, b, 2000, &fr) == FM1WIFI_OK && sta.state == FM1WIFI_STA_UP && sta.aid == 1, "joined OPLBOX: UP, AID 1");
    check(sdio_model_peek(m, FM1_WIFI_CORE(MAC_BSSID_DW0)) == 0x00000002u && sdio_model_peek(m, FM1_WIFI_CORE(MAC_BSSID_DW1)) == 0x0600u,
          "the BSSID set before joining");
    check(fm1wifi_listen(&sta, 2000, &fr) == FM1WIFI_OK && sta.state == FM1WIFI_STA_UP && sta.beacons >= 18 && sta.beacons <= 21,
          "2 s joined: UP, about 20 beacons");
    check(fm1wifi_leave(&sta, &fr) == FM1WIFI_OK && sta.state == FM1WIFI_STA_DOWN && sta.why == FM1WIFI_WHY_LEFT, "left");
    {
        const wifi_fix *fx = sdio_model_frames(m);
        const wifi_rec *last = 0;
        for (int i = 0; i < fx->n; i++) if (fx->r[i].write) last = &fx->r[i];
        check(last && last->data[0] == 0xC0 && last->data[24] == 3, "the last frame sent: a deauthentication, reason 3");
    }
    check(fm1wifi_join(&sta, b, 2000, &fr) == FM1WIFI_OK && sta.state == FM1WIFI_STA_UP, "rejoined");
    done(m);

    m = core("reg 0x3043c 0xe0\nap ch=6 bssid=02:00:00:00:00:06 ssid=OPLBOX kick=500\n");
    fm1wifi_link_up(ME, 6);
    fm1wifi_scan_channel(6, 120, 0, 0, &list, &fr);
    fm1wifi_join(&sta, fm1wifi_scan_find(&list, "OPLBOX"), 2000, &fr);
    fm1wifi_listen(&sta, 2000, &fr);
    check(sta.state == FM1WIFI_STA_DOWN && sta.why == FM1WIFI_WHY_DEAUTH && sta.reason == 2, "the AP deauthenticates: DOWN, reason 2");
    done(m);

    m = core("reg 0x3043c 0xe0\nap ch=6 bssid=02:00:00:00:00:06 ssid=OPLBOX gone=300\n");
    fm1wifi_link_up(ME, 6);
    fm1wifi_scan_channel(6, 120, 0, 0, &list, &fr);
    fm1wifi_join(&sta, fm1wifi_scan_find(&list, "OPLBOX"), 2000, &fr);
    fm1wifi_listen(&sta, 3000, &fr);
    check(sta.state == FM1WIFI_STA_DOWN && sta.why == FM1WIFI_WHY_BEACON_LOSS, "the AP goes quiet: beacon loss");
    done(m);

    for (int k = 0; k < 4; k++) {
        static const char *const S[4] = { "auth=refuse", "auth=silent", "assoc=refuse", "assoc=silent" };
        static const uint8_t WHY[4] = { FM1WIFI_WHY_AUTH_REFUSED, FM1WIFI_WHY_AUTH_TIMEOUT, FM1WIFI_WHY_ASSOC_REFUSED, FM1WIFI_WHY_ASSOC_TIMEOUT };
        char sc[200];
        snprintf(sc, sizeof sc, "reg 0x3043c 0xe0\nap ch=6 bssid=02:00:00:00:00:06 ssid=OPLBOX %s\n", S[k]);
        m = core(sc);
        fm1wifi_link_up(ME, 6);
        fm1wifi_scan_channel(6, 120, 0, 0, &list, &fr);
        fm1wifi_join(&sta, fm1wifi_scan_find(&list, "OPLBOX"), 2000, &fr);
        check(sta.state == FM1WIFI_STA_DOWN && sta.why == WHY[k], S[k]);
        done(m);
    }
}

static uint32_t step_at_rx, step_at_tx;
static void on_wr(void *ctx, uint32_t addr, uint32_t v)
{
    uint32_t step = fm1wifi_where.magic == FM1WIFI_WHERE_MAGIC ? fm1wifi_where.step : 0;
    (void)ctx; (void)v;
    if (addr == FM1_WL30_HOST_1C) step_at_rx = step;
    if (addr == FM1_WL30_HOST_14) step_at_tx = step;
}

static void bounded(void)
{
    static uint32_t buf32[FM1WIFI_RX_BUF / 4u];
    uint32_t n;
    sdio_model *m = core("reg 0x3043c 0xe0\nap ch=6 bssid=02:00:00:00:00:06 ssid=OPLBOX\nhang rxdma\n");
    fm1wifi_link_up(ME, 6);
    sdio_model_on_wr(m, on_wr, 0);
    for (int i = 0; i < 2000 && !fm1wifi_rx_ready(); i++) fm1wifi_port_delay_us(100);
    check(fm1wifi_rx_read((uint8_t *)buf32, sizeof buf32, &n) == FM1WIFI_ETIMEOUT && n == 0, "an RX DMA that never ends: TIMEOUT");
    check(step_at_rx == FM1WIFI_STEP_RX_READ && fm1wifi_where.magic == 0, "crash record: RX READ during the DMA, clear after");
    check(!strcmp(fm1wifi_step_name(FM1WIFI_STEP_RX_READ), "RX READ") && !strcmp(fm1wifi_step_name(FM1WIFI_STEP_TX_SEND), "TX SEND"),
          "the steps' names");
    memset(&fr, 0, sizeof fr);
    check(fm1wifi_scan_channel(6, 200, 0, 0, &list, &fr) == FM1WIFI_ETIMEOUT && fr.rx_err == FM1WIFI_ETIMEOUT, "a scan stops at it");
    done(m);

    m = core("reg 0x3043c 0xe0\nap ch=6 bssid=02:00:00:00:00:06 ssid=OPLBOX\nhang txdma\n");
    fm1wifi_link_up(ME, 6);
    sdio_model_on_wr(m, on_wr, 0);
    check(fm1wifi_scan_channel(6, 50, 1, 0, &list, &fr) == FM1WIFI_OK && fr.tx_err == 1 && fr.tx_errno == FM1WIFI_ETIMEOUT,
          "a TX DMA that never ends: TIMEOUT, and the scan listens on");
    check(step_at_tx == FM1WIFI_STEP_TX_SEND, "crash record: TX SEND during the DMA");
    done(m);

    m = core("reg 0x3043c 0\nap ch=6 bssid=02:00:00:00:00:06 ssid=OPLBOX\n");   /* PBF_DBG 0: the queue never has room */
    fm1wifi_link_up(ME, 6);
    memset(&fr, 0, sizeof fr);
    fm1wifi_scan_channel(6, 10, 1, 0, &list, &fr);
    check(fr.tx_errno == FM1WIFI_ETIMEOUT && fr.tx == 0, "no room in the TX queue: TIMEOUT after FM1WIFI_TXQ_TRIES");
    done(m);

    m = core("reg 0x3043c 0xe0\nap ch=6 bssid=02:00:00:00:00:06 ssid=OPLBOX\nrxbad\n");
    fm1wifi_link_up(ME, 6);
    for (int i = 0; i < 2000 && !fm1wifi_rx_ready(); i++) fm1wifi_port_delay_us(100);
    check(fm1wifi_rx_read((uint8_t *)buf32, sizeof buf32, &n) == FM1WIFI_EFRAME, "a length word of 0: BAD FRAME");
    check(!strcmp(fm1wifi_strerror(FM1WIFI_EFRAME), "BAD FRAME"), "its name");
    done(m);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fputs("usage: wifi_link <tests/wifi>\n", stderr); return 2; }
    dir = argv[1];
    link_up();
    fifo();
    scan();
    join();
    bounded();
    if (bad) return 1;
    printf("wifi_link: %d checks passed (link, FIFO DMA, scans, join and what ends it, bounded waits)\n", checks);
    return 0;
}
