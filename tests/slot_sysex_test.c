/* SPDX-License-Identifier: GPL-3.0-only */
/* firmware/src/slot_sysex.c on a computer (TODO 2a): the device's side of pulling and pushing slot files, with
 * ota.c's framing and read requests, against a fake computer that speaks SysEx bytes. Also firmware/src/wifi_sysex.c
 * (the WiFi test page's report and run), composed as oplbox.c does: slot_sysex first, its replies through slot_reply.
 *
 *   slot_sysex_test            the checks below; exit 1 when one fails
 *   slot_sysex_test --serve [N] a fake FM-1 for tests/slot_sysex_tool.py: one SysEx message a line as hex on stdin,
 *                              the FM-1's on stdout; slot N (1..9) is the song in the player, 0 none; --old: a
 *                              firmware without slot transfer (the handshake only)
 *   slot_sysex_test --serve-wifi FILE [edit|running]   a fake FM-1 for tests/wifi_report_tool.py: FILE is the report a
 *                              run makes; no run yet until the run command, which takes 3 report polls (edit: EDIT is
 *                              open; running: a run is already going on). With WIFI_NET_OUT set, the network command's
 *                              SSID and passphrase are written there as hex, one line each (what the test compares)
 *   slot_sysex_test --serve-ble FILE [edit|running]   the same for tests/ble_report_tool.py and firmware/src/ble_sysex.c
 *                              (the Bluetooth report): the run command's wait is written to BLE_WAIT_OUT, if set, as one
 *                              decimal line
 *   slot_sysex_test --dump N FILE   built-in song N (0..) as a .CMF file
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FELUCCA_ID "FM-1_960"
#include "macsop.c"
#include "songs.h"
#include "ota.c"

static int failed;
#define CHECK(c, ...) do { if (!(c)) { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the app's side, as oplbox.c has it on the host */
#define N_SLOTS 9u
#define SLOT_SIZE 0x8000u
#define SLOT_MAGIC 0x534C504Fu
#define SLOT_HDR 16u
static uint8_t slot_buf[SLOT_SIZE];
static uint8_t flash[N_SLOTS][SLOT_SIZE];
static uint32_t in_player = N_SLOTS, stores;
static int flash_fails;
static const char *said = "";
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) { c ^= *p++; for (k = 0; k < 8u; k++) c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1; }
    return ~c;
}
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static const uint8_t *slot_ptr(uint32_t i) { return flash[i]; }
static uint32_t slot_len(uint32_t i)
{
    const uint8_t *p = slot_ptr(i);
    uint32_t n;
    if (rd32(p) != SLOT_MAGIC) return 0;
    n = rd32(p + 4);
    if (n < 5 + 198 + 8 || n > SLOT_SIZE - SLOT_HDR || crc32(p + SLOT_HDR, n) != rd32(p + 8)) return 0;
    return n;
}
static int slot_in_player(uint32_t i) { return i == in_player; }
static int slot_store(uint32_t i, uint32_t n)
{
    stores++;
    if (flash_fails) return -1;
    memset(flash[i], 0xFF, SLOT_SIZE);
    memcpy(flash[i], slot_buf, n + SLOT_HDR);
    return 0;
}
static void say(const char *m) { said = m; }
#include "slot_sysex.c"

/* ---- the WiFi page's side, as oplbox.c has it: a run asked for completes after wpolls report polls (the main loop
 * running the page meanwhile), then the report is wrep with the run's number in its header */
static uint32_t wifi_report_state(void);
static const uint8_t *wifi_report(uint32_t *n, uint32_t *serial);
static int wifi_run_ask(uint32_t *run);
static int wifi_net_set(const uint8_t *ssid, uint32_t ns, const char *pass, uint32_t np);
static uint32_t wifi_live(uint8_t addr[4], uint32_t *left, uint32_t *answered);
static int wifi_reply(uint32_t cmd, const uint8_t *head, uint32_t nh, const uint8_t *data, uint32_t nd)
{
    return slot_reply(cmd, head, nh, data, nd);
}
#include "wifi_sysex.c"
static uint8_t wrep[20000];
static uint32_t wrep_n, wrep_serial, wstate, wruns, wreq, wedit, wpolls, wrun_polls = 3;
static void wifi_run_ends(void)
{
    wreq = 0; wruns++; wstate = WR_DONE; wrep_serial++;
    if (wrep_n >= WR_HEAD) { wrep[6] = (uint8_t)wruns; wrep[7] = (uint8_t)(wruns >> 8); }
}
static uint32_t wifi_report_state(void)
{
    if (wreq && ++wpolls >= wrun_polls) wifi_run_ends();
    return wreq ? (uint32_t)WR_RUNNING : wstate;
}
static const uint8_t *wifi_report(uint32_t *n, uint32_t *serial) { *n = wrep_n; *serial = wrep_serial & 0xFFFFu; return wrep; }
static uint8_t wnet[100];                      /* the network as the command handed it: ssid_len ssid pass_len pass */
static uint32_t wnet_n, wnet_sets;
static int wifi_net_set(const uint8_t *ssid, uint32_t ns, const char *pass, uint32_t np)
{
    const char *path = getenv("WIFI_NET_OUT");
    if (wreq || wstate == WR_RUNNING) return WIFI_E_RUNNING;
    wnet[0] = (uint8_t)ns; memcpy(wnet + 1, ssid, ns);
    wnet[1 + ns] = (uint8_t)np; memcpy(wnet + 2 + ns, pass, np);
    wnet_n = 2 + ns + np;
    wnet_sets++;
    if (path) {
        FILE *f = fopen(path, "w");
        if (f) {
            for (uint32_t i = 0; i < ns; i++) fprintf(f, "%02x", ssid[i]);
            fputc('\n', f);
            for (uint32_t i = 0; i < np; i++) fprintf(f, "%02x", (uint8_t)pass[i]);
            fputc('\n', f);
            fclose(f);
        }
    }
    return WIFI_OK;
}
/* the live state (0x65): no run, then DHCP for the first report poll of a run, then listening at 10.0.0.23 with 4000 ms
 * left and wlive_answered echo requests answered (stage 9's steps as oplbox.c reports them: 2 DHCP, 5 listening) */
static uint32_t wlive_answered;
static uint32_t wifi_live(uint8_t addr[4], uint32_t *left, uint32_t *answered)
{
    static const uint8_t A[4] = { 10, 0, 0, 23 };
    uint32_t step = !wreq ? 0u : wpolls < 2u ? 2u : 5u;
    memcpy(addr, step == 5u ? A : (const uint8_t *)"\0\0\0\0", 4);
    *left = step == 5u ? 4000u : 0u;
    *answered = step == 5u ? wlive_answered++ : 0u;
    return step;
}
static int wifi_run_ask(uint32_t *run)
{
    if (wreq || wstate == WR_RUNNING) { *run = (wruns + wreq) & 0xFFFFu; return WIFI_E_RUNNING; }
    *run = 0;
    if (wedit) return WIFI_E_EDIT;
    wreq = 1; wpolls = 0;
    *run = (wruns + 1u) & 0xFFFFu;
    return WIFI_OK;
}

/* ---- the Bluetooth page's side (ble_sysex.c, composed after wifi_sysex.c as oplbox.c does), the same way */
static uint32_t ble_report_state(void);
static const uint8_t *ble_report(uint32_t *n, uint32_t *serial);
static int ble_run_ask(uint32_t wait_s, uint32_t *run);
#include "ble_sysex.c"
static uint8_t brep[16384];
static uint32_t brep_n, brep_serial, bstate, bruns, breq, bedit, bpolls, brun_polls = 3, bwait = ~0u;
static void ble_run_ends(void)
{
    breq = 0; bruns++; bstate = WR_DONE; brep_serial++;
    if (brep_n >= WR_HEAD) { brep[6] = (uint8_t)bruns; brep[7] = (uint8_t)(bruns >> 8); }
}
static uint32_t ble_report_state(void)
{
    if (breq && ++bpolls >= brun_polls) ble_run_ends();
    return breq ? (uint32_t)WR_RUNNING : bstate;
}
static const uint8_t *ble_report(uint32_t *n, uint32_t *serial) { *n = brep_n; *serial = brep_serial & 0xFFFFu; return brep; }
static int ble_run_ask(uint32_t wait_s, uint32_t *run)
{
    const char *path = getenv("BLE_WAIT_OUT");
    if (breq || bstate == WR_RUNNING) { *run = (bruns + breq) & 0xFFFFu; return WIFI_E_RUNNING; }
    *run = 0;
    if (bedit) return WIFI_E_EDIT;
    breq = 1; bpolls = 0; bwait = wait_s;
    if (path) {
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "%u\n", wait_s); fclose(f); }
    }
    *run = (bruns + 1u) & 0xFFFFu;
    return WIFI_OK;
}

/* ---- the computer: frames to the FM-1 go through a SysEx assembler like usb.c's; what the FM-1 sends is
 * checked as SysEx, answered when it is a read request, kept when it is a reply */
static int serve, old;
static uint8_t q[16][OTA_FRAME_MAX];
static uint32_t q_n[16], q_r, q_w, now;
static const uint8_t *file;                     /* what the computer serves to read requests */
static uint32_t file_len, reads, answer = 1, corrupt_at = ~0u;
static uint8_t reply[OTA_FRAME_MAX];            /* the FM-1's last reply (decoded), 0x60 / 0x61 */
static uint32_t reply_n, max_wire;

static void to_fm1(const uint8_t *w, uint32_t n)   /* F0 .. F7 as bytes on the cable */
{
    uint32_t i, k = 0, on = 0;
    for (i = 0; i < n; i++) {
        if (w[i] == 0xF0) { on = 1; k = 0; continue; }
        if (!on) continue;
        if (w[i] == 0xF7) { if (q_w - q_r < 16u) { q_n[q_w % 16u] = k; q_w++; } on = 0; continue; }
        if (k < OTA_FRAME_MAX) q[q_w % 16u][k++] = w[i];
        else on = 0;                            /* too long: usb.c drops it */
    }
}
static uint32_t wire(uint32_t cmd, const uint8_t *body, uint32_t n, uint8_t *w)
{
    static uint8_t m[OTA_FRAME_MAX + 16];
    uint32_t i, s = 0, k;
    m[0] = 0; m[1] = 0x59; m[2] = (uint8_t)cmd; m[3] = (uint8_t)n; m[4] = (uint8_t)(n >> 8); m[5] = (uint8_t)(n >> 16);
    for (i = 0; i < n; i++) { m[6 + i] = body[i]; s += body[i]; }
    m[6 + n] = (uint8_t)~s;
    w[0] = 0xF0;
    k = 1u + ota_pack7(m, 7u + n, w + 1);
    w[k++] = 0xF7;
    return k;
}
static void hex_out(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) printf("%02X", p[i]);
    printf("\n");
    fflush(stdout);
}
static int hex_in(void)                         /* serve: one message from stdin; 0 at the end */
{
    static char line[4096];
    static uint8_t w[2048];
    uint32_t n = 0, v;
    if (!fgets(line, sizeof line, stdin)) return 0;
    while (n < sizeof w && sscanf(line + 2 * n, "%2x", &v) == 1) w[n++] = (uint8_t)v;
    to_fm1(w, n);
    return 1;
}

static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    static uint8_t d[OTA_FRAME_MAX];
    uint32_t i, ok = n >= 2u && p[0] == 0xF0 && p[n - 1] == 0xF7, dn, s = 0;
    for (i = 1; ok && i + 1u < n; i++) ok = p[i] < 0x80u;
    CHECK(ok, "the FM-1 sent something that is not one SysEx message (%u bytes)", n);
    if (n > max_wire) max_wire = n;
    if (serve) { hex_out(p, n); return 0; }
    dn = ota_unpack7(p + 1, n - 2u, d, sizeof d);
    for (i = 6; i + 1u < dn; i++) s += d[i];
    CHECK(dn >= 7u && d[0] == 0 && d[1] == 0x59 && ota_rd24(d + 3) + 7u == dn && (uint8_t)~s == d[dn - 1u],
          "the FM-1's message is not in the updater's framing");
    if (d[2] == 0x30 && dn == 15u) {            /* a read request: flags addr32 len16 0 */
        uint32_t addr = ota_rd32(d + 7), len = ota_rd16(d + 11);
        static uint8_t b[16 + 512], w[700];
        reads++;
        if (!answer) return 0;
        b[0] = 0;
        ota_wr32(b + 1, addr);
        b[5] = (uint8_t)len; b[6] = (uint8_t)(len >> 8); b[7] = 0;
        for (i = 0; i < len && i < 512u; i++) b[8 + i] = addr + i < file_len ? file[addr + i] : 0;
        if (corrupt_at >= addr && corrupt_at < addr + len) b[8 + corrupt_at - addr] ^= 0x40u;
        to_fm1(w, wire(0x30, b, 8u + len, w));
        return 0;
    }
    memcpy(reply, d, dn);
    reply_n = dn;
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n)
{
    if (q_r == q_w && serve && !hex_in()) exit(0);
    if (q_r == q_w) return 0;
    *p = q[q_r % 16u];
    *n = q_n[q_r % 16u];
    return 1;
}
static void ota_frame_done(void) { q_r++; }
static uint32_t ota_now_ms(void) { return now; }
static void ota_idle(void) { now++; }
static int ota_erase(uint32_t off) { (void)off; return -1; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; return -1; }
static int ota_fread(uint32_t off, void *p, uint32_t n) { (void)off; (void)p; (void)n; return -1; }
static void ota_show(uint32_t step, int32_t code) { (void)step; (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm; }

static void poll_fm1(void)                      /* platform_poll's part: the handshake, or the app's frame */
{
    uint32_t d = ota_take();
    if (d == 7u && ota_dec[2] == 0x11) ota_reply_identity();
    else if (d && !old && !slot_sysex(ota_dec, d) && !wifi_sysex(ota_dec, d)) ble_sysex(ota_dec, d);
}
static void send(uint32_t cmd, const uint8_t *body, uint32_t n)
{
    uint8_t w[2048];
    reply_n = 0;
    to_fm1(w, wire(cmd, body, n, w));
    poll_fm1();
}

static uint32_t push(uint32_t slot, const uint8_t *data, uint32_t len, uint32_t crc)   /* -> status */
{
    uint8_t b[8] = {(uint8_t)slot, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16)};
    wr32(b + 4, crc);
    file = data; file_len = len; reads = 0;
    send(SLOT_CMD_PUSH, b, 8);
    CHECK(reply_n == 9u && reply[2] == SLOT_CMD_PUSH && reply[6] == (slot < N_SLOTS ? slot : 0x7Fu), "push to slot %u: no reply", slot + 1u);
    return reply_n == 9u ? reply[7] : 99u;
}
static uint32_t pull_chunk(uint32_t slot, uint32_t off)  /* -> status; the reply in reply */
{
    uint8_t b[4] = {(uint8_t)slot, (uint8_t)off, (uint8_t)(off >> 8), (uint8_t)(off >> 16)};
    send(SLOT_CMD_PULL, b, 4);
    CHECK(reply_n >= 15u && reply[2] == SLOT_CMD_PULL, "pull from slot %u: no reply", slot + 1u);
    return reply_n >= 15u ? reply[7] : 99u;
}
static uint32_t pull(uint32_t slot, uint8_t *out)         /* -> bytes, the whole file */
{
    uint32_t off = 0, total;
    do {
        if (pull_chunk(slot, off) != SLOT_OK) return 0;
        total = ota_rd24(reply + 8);
        CHECK(ota_rd24(reply + 11) == off, "pull: the reply is for offset %u, not %u", ota_rd24(reply + 11), off);
        memcpy(out + off, reply + 14, reply_n - 15u);
        off += reply_n - 15u;
    } while (off < total && reply_n > 15u);
    return off;
}

/* every built-in song, and broken copies of one: macsop_check gives what macsop_parse gives */
static macsop_song song_tmp;
static void check_agrees(const uint8_t *b, uint32_t n, const char *what)
{
    int c = macsop_check(b, n), p = macsop_parse(&song_tmp, b, n);
    CHECK(c == p, "%s: macsop_check %d, macsop_parse %d", what, c, p);
}
static void test_check(void)
{
    static uint8_t b[40000];
    uint32_t i, n, cut;
    const uint8_t *s = SONG_DATA + SONGS[13].off;   /* KAISER, the smallest */
    for (i = 0; i < N_SONGS; i++) {
        CHECK(macsop_check(SONG_DATA + SONGS[i].off, SONGS[i].len) == 0, "song %u refused", i);
        check_agrees(SONG_DATA + SONGS[i].off, SONGS[i].len, SONGS[i].title);
    }
    n = SONGS[13].len;
    for (cut = 0; cut < n; cut += cut < 300u ? 1u : 37u) check_agrees(s, cut, "cut short");
    memcpy(b, s, n); b[0] = 'B'; check_agrees(b, n, "magic");
    CHECK(macsop_check(b, n) == -1, "a wrong magic is taken");
    memcpy(b, s, n); b[205] = 0; check_agrees(b, n, "speed 0");
    CHECK(macsop_check(b, n) == -2, "speed 0 is taken");
    memcpy(b, s, n); b[205] = 4; check_agrees(b, n, "speed 4");
    memcpy(b, s, n); b[203] = 65; b[204] = 0; check_agrees(b, n, "65 patterns");
    CHECK(macsop_check(b, n) == -2, "65 patterns are taken");
    memcpy(b, s, n); b[209] = 65; b[210] = 0; check_agrees(b, n, "65 instruments");
    memcpy(b, s, n); b[209] = 200; b[210] = 0; check_agrees(b, n, "instruments past the end");
    {   uint32_t p0 = 211u + 69u * (uint32_t)(s[209] | s[210] << 8);   /* the first cell */
        memcpy(b, s, n); b[p0] = 65; check_agrees(b, n, "row 65");
        CHECK(macsop_check(b, n) == -5, "row 65 is taken");
        memcpy(b, s, n); b[p0 + 1] = 11; check_agrees(b, n, "channel 11");
        memcpy(b, s, n); b[p0 + 2] = 0; check_agrees(b, n, "note 0");
        memcpy(b, s, n); b[n - 1] = 0; check_agrees(b, n, "no end mark");
    }
    srand(1);
    for (i = 0; i < 2000u; i++) {               /* random damage, one byte at a time */
        memcpy(b, s, n);
        b[rand() % n] = (uint8_t)rand();
        check_agrees(b, n, "random byte");
    }
}

static void test_transfer(void)
{
    static uint8_t out[SLOT_SIZE], big[SLOT_SIZE];
    const uint8_t *a = SONG_DATA + SONGS[8].off, *k = SONG_DATA + SONGS[13].off;   /* TRADERS INTRO (28510 B), KAISER */
    uint32_t na = SONGS[8].len, nk = SONGS[13].len, i, t0;

    memset(flash, 0xFF, sizeof flash);
    send(0x11, 0, 0);                           /* the updater's handshake still answers */
    CHECK(reply_n == 34u && reply[2] == 0x11 && !memcmp(reply + 6, FELUCCA_ID, 8), "no identity");

    CHECK(push(2, a, na, crc32(a, na)) == SLOT_OK, "push TRADERS INTRO to slot 3");
    CHECK(reads == (na + 511u) / 512u, "push: %u read requests for %u bytes", reads, na);
    CHECK(slot_len(2) == na && !memcmp(flash[2] + SLOT_HDR, a, na), "slot 3 does not hold the file");
    CHECK(!strcmp(said, "USB: SLOT 3 SAVED"), "the screen says \"%s\"", said);
    CHECK(pull(2, out) == na && !memcmp(out, a, na), "pull slot 3: not the file pushed");
    CHECK(pull_chunk(2, na) == SLOT_OK && reply_n == 15u, "pull at the end: not an empty chunk");
    CHECK(pull_chunk(2, na + 1u) == SLOT_E_REQ, "pull past the end is not refused");
    CHECK(pull_chunk(4, 0) == SLOT_E_EMPTY && ota_rd24(reply + 8) == 0, "an empty slot is not reported empty");
    CHECK(pull_chunk(9, 0) == SLOT_E_REQ, "slot 10 is not refused");
    { uint8_t b[2] = {2, 0}; send(SLOT_CMD_PULL, b, 2); CHECK(reply_n == 15u && reply[7] == SLOT_E_REQ, "a short pull is not refused"); }
    CHECK(push(9, k, nk, crc32(k, nk)) == SLOT_E_REQ && !reads, "push to slot 10 is not refused");
    { uint8_t b[3] = {1, 0, 0}; send(SLOT_CMD_PUSH, b, 3); CHECK(reply_n == 9u && reply[7] == SLOT_E_REQ, "a short push is not refused"); }

    /* refusals leave the slot as it was, and the big ones ask the computer for nothing */
    stores = 0;
    CHECK(push(2, big, SLOT_SIZE - SLOT_HDR + 1u, 0) == SLOT_E_BIG && !reads, "32 KiB - 15 is not refused before reading");
    memset(big, 0x55, sizeof big);
    CHECK(push(2, big, SLOT_SIZE - SLOT_HDR, crc32(big, SLOT_SIZE - SLOT_HDR)) == SLOT_E_CMF && reads == 64u,
          "32 KiB - 16 is not read whole and refused as no .CMF");
    CHECK(push(2, k, 210u, crc32(k, 210u)) == SLOT_E_CMF && !reads, "a file shorter than a .CMF header is read");
    in_player = 2;
    CHECK(push(2, k, nk, crc32(k, nk)) == SLOT_E_PLAYING && !reads, "the song in the player is overwritten");
    CHECK(!strcmp(said, "USB: SLOT 3 IS PLAYING"), "the screen says \"%s\"", said);
    in_player = N_SLOTS;
    CHECK(push(2, k, nk, crc32(k, nk) ^ 1u) == SLOT_E_XFER, "a wrong CRC is not refused");
    corrupt_at = 1000u;
    CHECK(push(2, k, nk, crc32(k, nk)) == SLOT_E_XFER, "a byte changed on the way is not caught");
    corrupt_at = ~0u;
    answer = 0; t0 = now;
    CHECK(push(2, k, nk, crc32(k, nk)) == SLOT_E_XFER && reads == 3u, "a computer that stops answering: %u requests", reads);
    CHECK(now - t0 <= 16000u, "a silent computer holds the FM-1 for %u ms", now - t0);
    answer = 1;
    flash_fails = 1;
    CHECK(push(3, k, nk, crc32(k, nk)) == SLOT_E_FLASH, "a failed write is not reported");
    flash_fails = 0;
    CHECK(stores == 1u, "%u writes for refused pushes (want only the failed write)", stores);
    CHECK(slot_len(2) == na && !memcmp(flash[2] + SLOT_HDR, a, na), "a refused push changed slot 3");
    CHECK(!strcmp(said, "USB: SLOT 4 NOT SAVED"), "the screen says \"%s\"", said);

    /* every song in, and out again, through all nine slots */
    for (i = 0; i < N_SONGS; i++) {
        const uint8_t *s = SONG_DATA + SONGS[i].off;
        CHECK(push(i % N_SLOTS, s, SONGS[i].len, crc32(s, SONGS[i].len)) == SLOT_OK, "push %s", SONGS[i].title);
        CHECK(pull(i % N_SLOTS, out) == SONGS[i].len && !memcmp(out, s, SONGS[i].len), "round trip %s", SONGS[i].title);
    }
    CHECK(max_wire <= 605u, "a message of %u bytes (usb.c takes 640)", max_wire);
}

/* the report's layout byte for byte, a buffer too small for it, and the two commands through the framing */
static void test_wifi(void)
{
    static uint8_t b[64], whole[20000];
    static const uint8_t WANT[] = { 'O', 'P', 'L', 'W', 1, 0, 7, 0, 5, 0, 'A', '\n', 'B', 'C', '\n',
                                    2, 1, 0, 0, 0, 0x78, 0x56, 0x34, 0x12,
                                    2, 1, 6, 10, 0, 4, 0, 0xDE, 0xAD, 0xBE, 0xEF, 3, 11, 0x58, 0x02, 2, 0, 0xDE, 0xAD };
    static const uint8_t FR[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    const uint32_t ctr[2] = { 1, 0x12345678u };
    wr_buf w;
    uint32_t i, off, total, n, serial;

    wr_begin(&w, b, sizeof b, 32, 7);
    wr_line(&w, "A"); wr_line(&w, "BC");
    wr_counters(&w, ctr, 2);
    wr_line(&w, "LATE");                        /* after the counters: refused, and said so */
    CHECK(b[5] == WR_CUT_TEXT, "a line after the counters: flags %02X", b[5]);
    b[5] = 0;
    wr_frame(&w, 1, 6, 10, FR, 4);
    wr_frame(&w, 3, 11, 600, FR, 2);
    CHECK(w.n == sizeof WANT && !memcmp(b, WANT, sizeof WANT), "the report's bytes are not the layout (%u bytes)", w.n);
    wr_frame(&w, 1, 1, 100, whole, 40);          /* does not fit: refused whole, flagged, the count unchanged */
    CHECK(w.n == sizeof WANT && b[5] == WR_CUT_FRAMES && b[24] == 2u, "a frame past the buffer: %u bytes, flags %02X", w.n, b[5]);
    wr_begin(&w, b, sizeof b, 4, 0);
    wr_line(&w, "ABC"); wr_line(&w, "D");
    CHECK(b[8] == 4u && b[5] == WR_CUT_TEXT, "text past text_max: %u bytes, flags %02X", b[8], b[5]);
    wr_frame(&w, 1, 1, 1, FR, 1);
    CHECK(b[5] & WR_CUT_FRAMES, "a frame before the counters is not refused");

    /* a report as oplbox.c builds one: 3000 bytes of text, 19 counters, 12 frames of 360 bytes */
    wr_begin(&w, wrep, sizeof wrep, 6144, 0);
    for (i = 0; i < 100u; i++) wr_line(&w, "RAW FRAMES 1-2       7/10 ......");
    { uint32_t c[19]; for (i = 0; i < 19u; i++) c[i] = i * 1000u; wr_counters(&w, c, 19); }
    for (i = 0; i < sizeof whole; i++) whole[i] = (uint8_t)(i * 7u + 3u);
    for (i = 0; i < 12u; i++) wr_frame(&w, 1u + i / 4u, 1u + i, 360, whole + 360u * i, 360);
    CHECK(!wrep[5], "the example report was cut short");
    wrep_n = w.n;

    send(WIFI_CMD_REPORT, (const uint8_t *)"\0\0\0", 3);
    CHECK(reply_n == 16u && reply[2] == WIFI_CMD_REPORT && reply[6] == WIFI_E_NONE, "before any run: not \"no run yet\"");
    wedit = 1;
    send(WIFI_CMD_RUN, 0, 0);
    CHECK(reply_n == 10u && reply[2] == WIFI_CMD_RUN && reply[6] == WIFI_E_EDIT && !wreq, "in EDIT: a run is not refused");
    wedit = 0;
    send(WIFI_CMD_RUN, 0, 0);
    CHECK(reply_n == 10u && reply[6] == WIFI_OK && reply[7] == 1u && wreq, "the run command is not taken as run 1");
    send(WIFI_CMD_RUN, 0, 0);
    CHECK(reply_n == 10u && reply[6] == WIFI_E_RUNNING && reply[7] == 1u, "a second run command: not \"run 1 is running\"");
    send(WIFI_CMD_REPORT, (const uint8_t *)"\0\0\0", 3);
    CHECK(reply_n == 16u && reply[6] == WIFI_E_RUNNING, "while it runs: the report is not \"running\"");
    wifi_run_ends();
    for (off = 0, total = 1; off < total; off += n) {
        uint8_t o[3] = { (uint8_t)off, (uint8_t)(off >> 8), (uint8_t)(off >> 16) };
        send(WIFI_CMD_REPORT, o, 3);
        if (!(reply_n >= 16u && reply[6] == WIFI_OK)) { CHECK(0, "report at %u: status %u", off, reply_n ? reply[6] : 99u); break; }
        total = ota_rd24(reply + 7);
        serial = reply[13] | (uint32_t)reply[14] << 8;
        n = reply_n - 16u;
        CHECK(ota_rd24(reply + 10) == off && n <= WIFI_CHUNK && (n || off == total), "report at %u: %u bytes for offset %u", off, n, ota_rd24(reply + 10));
        CHECK(serial == wrep_serial, "report at %u: serial %u", off, serial);
        if (!n) break;
        memcpy(whole + off, reply + 15, n);
    }
    CHECK(total == wrep_n && !memcmp(whole, wrep, wrep_n) && wrep[6] == 1u, "the report read back is not the one built");
    { uint8_t o[3] = { (uint8_t)(wrep_n + 1u), (uint8_t)((wrep_n + 1u) >> 8), 0 };
      send(WIFI_CMD_REPORT, o, 3);
      CHECK(reply_n == 16u && reply[6] == WIFI_E_REQ, "an offset past the end is not refused"); }
    send(WIFI_CMD_REPORT, (const uint8_t *)"\0\0", 2);
    CHECK(reply_n == 16u && reply[6] == WIFI_E_REQ, "a short report request is not refused");
    send(WIFI_CMD_RUN, (const uint8_t *)"\0", 1);
    CHECK(reply_n == 10u && reply[6] == WIFI_E_REQ && !wreq, "a run command with a body is not refused");
    send(0x68, 0, 0);                          /* 0x66 and 0x67 are the Bluetooth page's */
    CHECK(reply_n == 0u, "an unknown command is answered");

    /* the live state (0x65): none without a run; during one the step, then the address and the window */
    send(WIFI_CMD_LIVE, 0, 0);
    CHECK(reply_n == 17u && reply[2] == WIFI_CMD_LIVE && reply[6] == WIFI_OK && reply[7] == 0u && !reply[8] && !reply[11],
          "live without a run: not step 0, no address");
    send(WIFI_CMD_RUN, 0, 0);
    send(WIFI_CMD_LIVE, 0, 0);
    CHECK(reply_n == 17u && reply[7] == 2u && !reply[8], "live in DHCP: not step 2 without an address");
    wpolls = 2;
    send(WIFI_CMD_LIVE, 0, 0);
    CHECK(reply_n == 17u && reply[7] == 5u && reply[8] == 10u && reply[11] == 23u && (reply[12] | reply[13] << 8) == 4000u &&
          (reply[14] | reply[15] << 8) == 0u, "live while listening: not 10.0.0.23, 4000 ms left, 0 answered");
    send(WIFI_CMD_LIVE, (const uint8_t *)"\0", 1);
    CHECK(reply_n == 17u && reply[6] == WIFI_E_REQ, "a live command with a body is not refused");
    wifi_run_ends();

    /* the network (0x64): taken whole, the reply status and the SSID's length only; every refusal */
    {   static const uint8_t NET[] = { 8, 'X', 'N', 'E', 'T', 'W', 'O', 'R', 'K', 12, 's', 'e', 'c', 'r', 'e', 't', ' ', 'p', 'a', 's', 's', '!' };
        uint8_t bad[100];
        uint32_t k, sets;
        send(WIFI_CMD_NET, NET, sizeof NET);
        CHECK(reply_n == 9u && reply[2] == WIFI_CMD_NET && reply[6] == WIFI_OK && reply[7] == 8u, "the network is not taken");
        CHECK(wnet_n == sizeof NET && !memcmp(wnet, NET, sizeof NET), "the network handed over is not the one sent");
        for (k = 0; k + 4u <= reply_n; k++) CHECK(memcmp(reply + k, "secr", 4), "the reply carries the passphrase");
        sets = wnet_sets;
        memcpy(bad, NET, sizeof NET); bad[9] = 7;                                    /* 7 characters */
        send(WIFI_CMD_NET, bad, sizeof NET - 5);
        CHECK(reply_n == 9u && reply[6] == WIFI_E_REQ && reply[7] == 0u, "a 7-character passphrase is not refused");
        memset(bad, 'a', sizeof bad); bad[0] = 8; bad[9] = 64;                        /* 64 characters */
        send(WIFI_CMD_NET, bad, 10 + 64);
        CHECK(reply_n == 9u && reply[6] == WIFI_E_REQ, "a 64-character passphrase is not refused");
        memcpy(bad, NET, sizeof NET); bad[12] = 0x7F;                                 /* not printable */
        send(WIFI_CMD_NET, bad, sizeof NET);
        CHECK(reply_n == 9u && reply[6] == WIFI_E_REQ, "a passphrase with DEL in it is not refused");
        memset(bad, 'a', sizeof bad); bad[0] = 33; bad[34] = 8;                       /* a 33-byte SSID */
        send(WIFI_CMD_NET, bad, 35 + 8);
        CHECK(reply_n == 9u && reply[6] == WIFI_E_REQ, "a 33-byte SSID is not refused");
        send(WIFI_CMD_NET, NET, sizeof NET - 1);                                      /* lengths not the body's */
        CHECK(reply_n == 9u && reply[6] == WIFI_E_REQ, "a cut-short network is not refused");
        memcpy(bad, NET, sizeof NET); bad[sizeof NET] = 'x';
        send(WIFI_CMD_NET, bad, sizeof NET + 1);
        CHECK(reply_n == 9u && reply[6] == WIFI_E_REQ, "a network with a byte after it is not refused");
        send(WIFI_CMD_NET, 0, 0);
        CHECK(reply_n == 9u && reply[6] == WIFI_E_REQ, "an empty network command is not refused");
        CHECK(wnet_sets == sets, "a refused network reached the app");
        send(WIFI_CMD_RUN, 0, 0);                                                     /* while a run is going on */
        send(WIFI_CMD_NET, NET, sizeof NET);
        CHECK(reply_n == 9u && reply[6] == WIFI_E_RUNNING && wnet_sets == sets, "the network changed while a run went on");
        wifi_run_ends();
        send(WIFI_CMD_NET, (const uint8_t *)"\0", 1);                                 /* forget it */
        CHECK(reply_n == 9u && reply[6] == WIFI_OK && reply[7] == 0u && wnet_n == 2u && wnet[0] == 0u, "the network is not forgotten");
    }
    CHECK(max_wire <= 640u, "a message of %u bytes", max_wire);
}

/* the Bluetooth report's header (the magic), and the two commands: the run with and without a wait, refused waits and bodies */
static void test_ble(void)
{
    static uint8_t b[64];
    wr_buf w;
    uint32_t sets_before;
    wb_begin(&w, b, sizeof b, 32, 5);
    wr_line(&w, "A");
    CHECK(!memcmp(b, "OPLB", 4) && b[4] == 1 && b[6] == 5 && b[8] == 2 && b[10] == 'A', "the Bluetooth report's header is not OPLB 1 run 5");

    bedit = 1;
    send(BLE_CMD_RUN, 0, 0);
    CHECK(reply_n == 10u && reply[2] == BLE_CMD_RUN && reply[6] == WIFI_E_EDIT && !breq, "Bluetooth: in EDIT a run is not refused");
    bedit = 0;
    send(BLE_CMD_RUN, (const uint8_t *)"\x79", 1);                     /* 121 s */
    CHECK(reply_n == 10u && reply[6] == WIFI_E_REQ && !breq, "Bluetooth: a wait of 121 s is not refused");
    send(BLE_CMD_RUN, (const uint8_t *)"\0\0", 2);
    CHECK(reply_n == 10u && reply[6] == WIFI_E_REQ && !breq, "Bluetooth: a two-byte body is not refused");
    send(BLE_CMD_RUN, 0, 0);
    CHECK(reply_n == 10u && reply[6] == WIFI_OK && reply[7] == 1u && breq && bwait == BLE_WAIT_DEFAULT,
          "Bluetooth: a run with no wait is not run 1 with the default wait (%u)", bwait);
    send(BLE_CMD_RUN, (const uint8_t *)"\x05", 1);
    CHECK(reply_n == 10u && reply[6] == WIFI_E_RUNNING && reply[7] == 1u && bwait == BLE_WAIT_DEFAULT,
          "Bluetooth: a second run command changed the run");
    send(BLE_CMD_REPORT, (const uint8_t *)"\0\0\0", 3);
    CHECK(reply_n == 16u && reply[2] == BLE_CMD_REPORT && reply[6] == WIFI_E_RUNNING, "Bluetooth: while it runs the report is not \"running\"");
    ble_run_ends();
    sets_before = bruns;
    send(BLE_CMD_RUN, (const uint8_t *)"\x00", 1);
    CHECK(reply_n == 10u && reply[6] == WIFI_OK && reply[7] == 2u && bwait == 0u && bruns == sets_before, "Bluetooth: a wait of 0 is not taken as run 2");
    breq = 0; bruns = 0; bstate = WR_NONE; brep_n = 0;
    send(BLE_CMD_REPORT, (const uint8_t *)"\0\0\0", 3);
    CHECK(reply_n == 16u && reply[6] == WIFI_E_NONE, "Bluetooth: before any run the report is not \"none\"");
    send(BLE_CMD_REPORT, (const uint8_t *)"\0\0", 2);
    CHECK(reply_n == 16u && reply[6] == WIFI_E_REQ, "Bluetooth: a short report request is not refused");
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--serve-ble")) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) return 2;
        brep_n = (uint32_t)fread(brep, 1, sizeof brep, f);
        fclose(f);
        serve = 1;
        if (argc > 3 && !strcmp(argv[3], "edit")) bedit = 1;
        if (argc > 3 && !strcmp(argv[3], "running")) { breq = 1; brun_polls = 6; }
        for (;;) poll_fm1();
    }
    if (argc > 2 && !strcmp(argv[1], "--serve-wifi")) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) return 2;
        wrep_n = (uint32_t)fread(wrep, 1, sizeof wrep, f);
        fclose(f);
        serve = 1;
        if (argc > 3 && !strcmp(argv[3], "edit")) wedit = 1;
        if (argc > 3 && !strcmp(argv[3], "running")) { wreq = 1; wrun_polls = 6; }
        for (;;) poll_fm1();
    }
    if (argc > 1 && !strcmp(argv[1], "--serve")) {
        serve = 1;
        memset(flash, 0xFF, sizeof flash);
        if (argc > 2) old = !strcmp(argv[2], "--old");
        if (argc > 2 && !old && atoi(argv[2]) > 0) in_player = (uint32_t)atoi(argv[2]) - 1u;
        for (;;) poll_fm1();
    }
    if (argc == 4 && !strcmp(argv[1], "--dump")) {
        uint32_t i = (uint32_t)atoi(argv[2]);
        FILE *f = i < N_SONGS ? fopen(argv[3], "wb") : 0;
        if (!f || fwrite(SONG_DATA + SONGS[i].off, 1, SONGS[i].len, f) != SONGS[i].len) return 2;
        fclose(f);
        return 0;
    }
    test_check();
    test_transfer();
    test_wifi();
    test_ble();
    if (failed) { printf("slot_sysex: %d checks failed\n", failed); return 1; }
    printf("slot_sysex: checks passed (pull, push, refusals, macsop_check == macsop_parse, the WiFi and Bluetooth reports and runs, "
           "messages <= %u bytes)\n", max_wire);
    return 0;
}
