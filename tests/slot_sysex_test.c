/* SPDX-License-Identifier: GPL-3.0-only */
/* firmware/src/slot_sysex.c on a computer (TODO 2a): the device's side of pulling and pushing slot files, with
 * ota.c's framing and read requests, against a fake computer that speaks SysEx bytes.
 *
 *   slot_sysex_test            the checks below; exit 1 when one fails
 *   slot_sysex_test --serve [N] a fake FM-1 for tests/slot_sysex_tool.py: one SysEx message a line as hex on stdin,
 *                              the FM-1's on stdout; slot N (1..9) is the song in the player, 0 none; --old: a
 *                              firmware without slot transfer (the handshake only)
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
    else if (d && !old) slot_sysex(ota_dec, d);
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

int main(int argc, char **argv)
{
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
    if (failed) { printf("slot_sysex: %d checks failed\n", failed); return 1; }
    printf("slot_sysex: checks passed (pull, push, refusals, macsop_check == macsop_parse, messages <= %u bytes)\n", max_wire);
    return 0;
}
