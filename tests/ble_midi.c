/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of firmware/ble/ble_midi.c (docs/BLUETOOTH.md stage 2): the BLE-MIDI profile.
 *
 * The specification (MMA/AMEI RP-052, BLE-MIDI 1.0) gives field tables and figures, not packets in hex, so the
 * byte-for-byte vectors here are worked out by hand from its rules; the section numbers in the comments are its.
 * Then round trips (random messages and SysEx through the encoder at many packet sizes and back through the
 * decoder, timestamps included), what the decoder refuses, the bridge to the app's midi_get(), and random garbage
 * thrown at the decoder. Exits 1 when a check fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble_midi.h"

static int bad, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fprintf(stderr, "FAIL: %s\n", what); bad = 1; } }
static int eq(const uint8_t *a, const uint8_t *b, uint32_t n) { return memcmp(a, b, n) == 0; }

/* ---- the encoder's packets, kept ---- */
#define MAXPK 4000
#define PKLEN 520
static struct { uint8_t b[PKLEN]; uint32_t n; } pk[MAXPK];
static uint32_t npk;
static int send_rc;
static int keep(void *ctx, const uint8_t *p, uint32_t n)
{
    (void)ctx;
    if (send_rc)
        return send_rc;
    if (npk < MAXPK && n <= PKLEN) { memcpy(pk[npk].b, p, n); pk[npk].n = n; }
    npk++;
    return 0;
}
static int is(uint32_t i, const uint8_t *want, uint32_t n) { return i < npk && pk[i].n == n && eq(pk[i].b, want, n); }

static uint8_t ebuf[PKLEN];
static ble_midi_enc E;
static void enc_new(uint32_t cap) { npk = 0; send_rc = 0; check(ble_midi_enc_init(&E, ebuf, cap, keep, NULL) == 0, "enc_init"); }

/* ---- the decoder's events, kept ---- */
typedef struct { int sysex; uint16_t ts, ts_end; uint32_t n; uint8_t b[400]; } ev;
#define MAXEV 4000
static ev evs[MAXEV];
static uint32_t nev;
static uint32_t chunks, sx_at;
static void on_msg(void *ctx, uint16_t ts, const uint8_t *m, uint8_t n)
{
    (void)ctx;
    if (nev < MAXEV) { evs[nev].sysex = 0; evs[nev].ts = ts; evs[nev].n = n; memcpy(evs[nev].b, m, n); }
    nev++;
}
static void on_sysex(void *ctx, uint16_t ts, const uint8_t *p, uint32_t n, int first, int last)
{
    (void)ctx;
    chunks++;
    if (first) {
        sx_at = nev;
        if (nev < MAXEV) { evs[nev].sysex = 1; evs[nev].ts = ts; evs[nev].n = 0; }
        nev++;
    }
    if (sx_at < MAXEV) {
        ev *e = &evs[sx_at];
        if (e->n + n <= sizeof e->b) memcpy(e->b + e->n, p, n);
        e->n += n;
        if (last) e->ts_end = ts;
    }
}
static ble_midi_dec D;
static void dec_new(void) { memset(&D, 0, sizeof D); D.msg = on_msg; D.sysex = on_sysex; nev = 0; chunks = 0; }
static uint32_t feed(const uint8_t *p, uint32_t n) { return ble_midi_dec_packet(&D, p, n); }
static int ev_msg(uint32_t i, uint16_t ts, uint8_t a, uint8_t b, uint8_t c, uint8_t n)
{
    uint8_t w[3] = {a, b, c};
    return i < nev && !evs[i].sysex && evs[i].ts == ts && evs[i].n == n && eq(evs[i].b, w, n);
}

static uint32_t rnd_s = 12345;
static uint32_t rnd(void) { rnd_s = rnd_s * 1664525u + 1013904223u; return rnd_s >> 8; }

/* ---- the GATT database, the advertising data (section 3, 5) ---- */
static void hex_uuid(const char *s, uint8_t *out)       /* "aabbccdd-..." to the 16 bytes least significant first */
{
    uint8_t be[16];
    uint32_t n = 0;
    for (; *s; s++) {
        int v;
        if (*s == '-') continue;
        v = *s >= 'A' ? (*s - 'A' + 10) : *s - '0';
        if (n % 2 == 0) be[n / 2] = (uint8_t)(v << 4); else be[n / 2] |= (uint8_t)v;
        n++;
    }
    for (n = 0; n < 16; n++) out[n] = be[15 - n];
}

static void gatt(void)
{
    uint8_t u[16], o[40];
    uint32_t n;
    static const uint8_t svc[] = { 0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03 };
    hex_uuid(BLE_MIDI_SERVICE_UUID, u);
    check(eq(u, ble_midi_service_uuid, 16), "the service UUID bytes are the specification's string, reversed");
    check(eq(ble_midi_service_uuid, svc, 16), "the service UUID, written out by hand");
    hex_uuid(BLE_MIDI_CHAR_UUID, u);
    check(eq(u, ble_midi_char_uuid, 16), "the characteristic UUID bytes are the specification's string, reversed");
    check(strcmp(BLE_MIDI_SERVICE_UUID, "03B80E5A-EDE8-4B33-A751-6CE34EC4C700") == 0, "service UUID text (section 3)");
    check(strcmp(BLE_MIDI_CHAR_UUID, "7772E5DB-3868-4112-A1A9-F2669D106BF3") == 0, "characteristic UUID text (section 3)");
    check(BLE_MIDI_CHAR_PROPS == (0x02 | 0x04 | 0x10), "read, write without response, notify");
    check(BLE_MIDI_CONN_INTERVAL_MAX * 125 <= 1500, "the connection interval is 15 ms or less (section 4)");
    memset(o, 0xEE, sizeof o);
    check(ble_midi_read_value(o) == 0 && o[0] == 0xEE, "the first read has no payload (section 5)");

    n = ble_midi_adv_data(o);
    check(n == 21 && o[0] == 2 && o[1] == 0x01 && o[2] == 0x06 && o[3] == 17 && o[4] == 0x07 && eq(o + 5, ble_midi_service_uuid, 16),
          "advertising data: flags, then the 128-bit service UUID");
    n = ble_midi_scan_rsp(o, "OPLBOX");
    check(n == 8 && o[0] == 7 && o[1] == 0x09 && eq(o + 2, (const uint8_t *)"OPLBOX", 6), "scan response: the complete name");
    n = ble_midi_scan_rsp(o, "0123456789012345678901234567890123456789");
    check(n == 31 && o[0] == 30 && o[1] == 0x08 && o[2] == '0', "a long name is shortened to fit 31 bytes");
    check(ble_midi_scan_rsp(o, NULL) == 0, "no name, nothing");
}

static void lengths(void)
{
    uint32_t s;
    check(ble_midi_msg_len(0x80) == 3 && ble_midi_msg_len(0x9F) == 3 && ble_midi_msg_len(0xAF) == 3 && ble_midi_msg_len(0xBF) == 3,
          "note off, note on, key pressure and control change are 3 bytes");
    check(ble_midi_msg_len(0xC0) == 2 && ble_midi_msg_len(0xDF) == 2, "program change and channel pressure are 2 bytes");
    check(ble_midi_msg_len(0xE0) == 3 && ble_midi_msg_len(0xEF) == 3, "pitch bend is 3 bytes");
    check(ble_midi_msg_len(0xF1) == 2 && ble_midi_msg_len(0xF2) == 3 && ble_midi_msg_len(0xF3) == 2 && ble_midi_msg_len(0xF6) == 1,
          "System Common lengths");
    check(ble_midi_msg_len(0xF8) == 1 && ble_midi_msg_len(0xFF) == 1, "real time is 1 byte");
    check(!ble_midi_msg_len(0x40) && !ble_midi_msg_len(0xF0) && !ble_midi_msg_len(0xF7) && !ble_midi_msg_len(0xF4) && !ble_midi_msg_len(0xF5),
          "data bytes, SysEx, EOX and the undefined statuses are not messages");
    for (s = 0x80; s < 0x100; s++)
        check(ble_midi_msg_len((uint8_t)s) <= 3, "no message is longer than 3 bytes");
}

/* ---- the encoder, byte for byte ---- */
static void encoder(void)
{
    /* section 7: header 1 0 tttttt (timestampHigh), timestamp byte 1 ttttttt (timestampLow), then the message.
     * 0x123 = 291 ms = high 2, low 0x23: header 0x82, timestamp byte 0xA3 */
    static const uint8_t one[] = { 0x82, 0xA3, 0x90, 0x3C, 0x7F };
    static const uint8_t two[] = { 0x82, 0xA3, 0x90, 0x3C, 0x7F, 0xA5, 0x80, 0x3C, 0x00 };
    static const uint8_t rs_same[] = { 0x82, 0xA3, 0x90, 0x3C, 0x7F, 0x3E, 0x40 };
    static const uint8_t rs_ts[] = { 0x82, 0xA3, 0x90, 0x3C, 0x7F, 0xA5, 0x3E, 0x40 };
    static const uint8_t wrap[] = { 0x80, 0xFF, 0x90, 0x3C, 0x7F, 0x80, 0x80, 0x3C, 0x00 };
    static const uint8_t wrap3[] = { 0x80, 0xFF, 0x90, 0x3C, 0x7F, 0x80, 0x80, 0x3C, 0x00, 0x82, 0x3E, 0x00 };
    static const uint8_t top[] = { 0xBF, 0xFF, 0x90, 0x3C, 0x7F, 0x80, 0x80, 0x3C, 0x00 };
    static const uint8_t far1[] = { 0x80, 0x80, 0x90, 0x3C, 0x7F }, far2[] = { 0x82, 0x80, 0x80, 0x3C, 0x00 };
    static const uint8_t rt[] = { 0x80, 0xA0, 0x90, 0x3C, 0x7F, 0xA1, 0xF8, 0xA2, 0x3E, 0x40 };
    static const uint8_t rt_same[] = { 0x80, 0xA0, 0x90, 0x3C, 0x7F, 0xA0, 0xF8, 0xA0, 0x3E, 0x40 };
    static const uint8_t pc2[] = { 0x80, 0xA0, 0xC1, 0x05, 0xA1, 0x06 };
    static const uint8_t mixed[] = { 0x80, 0x85, 0x90, 0x3C, 0x7F, 0x85, 0xF0, 0x01, 0x85, 0xF7, 0x85, 0x90, 0x3C, 0x7F };
    static const uint8_t sx_a1[] = { 0x80, 0x85, 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05 };
    static const uint8_t sx_a2[] = { 0x80, 0x06, 0x07, 0x08, 0x85, 0xF7 };
    static const uint8_t sx_one[] = { 0x80, 0x85, 0xF0, 0x01, 0x02, 0x03, 0x85, 0xF7 };
    static const uint8_t sx_e1[] = { 0x80, 0x85, 0xF0, 0x01, 0x02, 0x03, 0x04 }, sx_e2[] = { 0x80, 0x85, 0xF7 };
    static const uint8_t sx_empty[] = { 0x80, 0x85, 0xF0, 0x85, 0xF7 };
    static const uint8_t sx_then[] = { 0x80, 0x85, 0xF0, 0x01, 0x85, 0xF7, 0x85, 0x90, 0x3C, 0x7F };
    static const uint8_t sx_big[] = { 0xF0, 1, 2, 3, 4, 5, 6, 7, 8, 0xF7 };
    static const uint8_t sx3[] = { 0xF0, 1, 2, 3, 0xF7 }, sx4[] = { 0xF0, 1, 2, 3, 4, 0xF7 };
    uint8_t big[300];
    uint32_t i;

    enc_new(20);
    check(ble_midi_enc_msg(&E, 0x123, 0x90, 0x3C, 0x7F) == 0 && npk == 0, "a message waits for the flush");
    check(ble_midi_enc_flush(&E) == 0 && npk == 1 && is(0, one, 5), "one message: header 82, timestamp A3, 90 3C 7F");
    check(ble_midi_enc_flush(&E) == 0 && npk == 1, "a flush with nothing waiting sends nothing");

    enc_new(20);
    ble_midi_enc_msg(&E, 0x123, 0x90, 0x3C, 0x7F);
    ble_midi_enc_msg(&E, 0x125, 0x80, 0x3C, 0x00);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, two, 9), "two messages: each has its own timestamp byte (the figure on page 3)");

    enc_new(20);
    ble_midi_enc_msg(&E, 0x123, 0x90, 0x3C, 0x7F);
    ble_midi_enc_msg(&E, 0x123, 0x90, 0x3E, 0x40);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, rs_same, 7), "running status, same time: no status byte and no timestamp byte (section 7 rule 2)");

    enc_new(20);
    ble_midi_enc_msg(&E, 0x123, 0x90, 0x3C, 0x7F);
    ble_midi_enc_msg(&E, 0x125, 0x90, 0x3E, 0x40);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, rs_ts, 8), "running status, later time: a timestamp byte, no status byte");

    enc_new(20);
    ble_midi_enc_msg(&E, 0x123, 0xC1, 0x05, 0);
    ble_midi_enc_msg(&E, 0x123, 0xC1, 0x06, 0);
    ble_midi_enc_flush(&E);
    check(npk == 1 && pk[0].n == 5 && pk[0].b[2] == 0xC1 && pk[0].b[3] == 0x05 && pk[0].b[4] == 0x06, "running status on a 2-byte message");

    enc_new(20);
    ble_midi_enc_msg(&E, 127, 0x90, 0x3C, 0x7F);
    ble_midi_enc_msg(&E, 128, 0x80, 0x3C, 0x00);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, wrap, 9), "a timestamp low that wraps in one packet: the receiver adds one to the high bits (section 9)");

    enc_new(20);
    ble_midi_enc_msg(&E, 127, 0x90, 0x3C, 0x7F);
    ble_midi_enc_msg(&E, 128, 0x80, 0x3C, 0x00);
    ble_midi_enc_msg(&E, 130, 0x80, 0x3E, 0x00);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, wrap3, 12), "after a wrap the receiver's high bits are one more: a later message in the same packet is counted from there");

    enc_new(20);
    ble_midi_enc_msg(&E, 8191, 0x90, 0x3C, 0x7F);
    ble_midi_enc_msg(&E, 0, 0x80, 0x3C, 0x00);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, top, 9), "the 13-bit time wraps from 8191 to 0 inside one packet: timestampHigh goes from 63 to 0");

    enc_new(20);
    ble_midi_enc_msg(&E, 0, 0x90, 0x3C, 0x7F);
    ble_midi_enc_msg(&E, 256, 0x80, 0x3C, 0x00);
    ble_midi_enc_flush(&E);
    check(npk == 2 && is(0, far1, 5) && is(1, far2, 5), "a time the receiver could not rebuild starts a new packet");

    enc_new(20);
    ble_midi_enc_msg(&E, 100, 0x90, 0x3C, 0x7F);
    ble_midi_enc_msg(&E, 50, 0x80, 0x3C, 0x00);
    ble_midi_enc_flush(&E);
    check(npk == 2 && pk[0].b[0] == 0x80 && pk[0].b[1] == 0xE4 && pk[1].b[1] == 0xB2, "a time earlier than the last starts a new packet");

    enc_new(20);
    ble_midi_enc_msg(&E, 0x20, 0x90, 0x3C, 0x7F);
    ble_midi_enc_msg(&E, 0x21, 0xF8, 0, 0);
    ble_midi_enc_msg(&E, 0x22, 0x90, 0x3E, 0x40);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, rt, 10), "real time between running-status messages keeps running status, with a timestamp byte after it");

    enc_new(20);
    ble_midi_enc_msg(&E, 0x20, 0x90, 0x3C, 0x7F);
    ble_midi_enc_msg(&E, 0x20, 0xF8, 0, 0);
    ble_midi_enc_msg(&E, 0x20, 0x90, 0x3E, 0x40);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, rt_same, 10), "after a real-time byte the timestamp byte is there even at the same time (section 7 rule 3)");

    enc_new(20);
    ble_midi_enc_msg(&E, 0x20, 0xC1, 0x05, 0);
    ble_midi_enc_msg(&E, 0x20, 0x90, 0x3C, 0x7F);
    ble_midi_enc_msg(&E, 0x20, 0xC1, 0x06, 0);
    ble_midi_enc_flush(&E);
    check(npk == 1 && pk[0].n == 11 && pk[0].b[5] == 0x90 && pk[0].b[9] == 0xC1, "a status that is not the last one's is written in full");

    enc_new(5);
    ble_midi_enc_msg(&E, 0x123, 0x90, 0x3C, 0x7F);
    ble_midi_enc_msg(&E, 0x123, 0x90, 0x3E, 0x40);
    ble_midi_enc_msg(&E, 0x123, 0x90, 0x40, 0x40);
    ble_midi_enc_flush(&E);
    check(npk == 3 && pk[0].n == 5 && pk[1].n == 5 && pk[2].n == 5, "a packet never grows past the size it was given");
    check(pk[1].b[0] == 0x82 && pk[1].b[2] == 0x90 && pk[1].b[3] == 0x3E, "a new packet has its own header and full status (the end of a packet cancels running status)");

    enc_new(20);
    ble_midi_enc_msg(&E, 0x20, 0xC1, 0x05, 0);
    ble_midi_enc_msg(&E, 0x21, 0xC1, 0x06, 0);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, pc2, 6), "program changes at two times share a packet");

    /* section 8: SysEx. cap 8: header, timestamp, 0xF0, five data bytes fill the first packet; the continuation has no
     * timestamp byte; the last packet has the timestamp byte before 0xF7 */
    enc_new(8);
    check(ble_midi_enc_sysex(&E, 5, sx_big, sizeof sx_big) == 0, "SysEx accepted");
    ble_midi_enc_flush(&E);
    check(npk == 2 && is(0, sx_a1, 8) && is(1, sx_a2, 6), "SysEx over two packets: the continuation has no timestamp byte (section 8)");

    enc_new(8);
    ble_midi_enc_sysex(&E, 5, sx3, sizeof sx3);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, sx_one, 8), "SysEx that ends exactly at the end of the packet");

    enc_new(8);
    ble_midi_enc_sysex(&E, 5, sx4, sizeof sx4);
    ble_midi_enc_flush(&E);
    check(npk == 2 && is(0, sx_e1, 7) && is(1, sx_e2, 3), "the closing timestamp and 0xF7 go in a packet of their own when they do not fit");

    enc_new(20);
    ble_midi_enc_sysex(&E, 5, (const uint8_t[]){ 0xF0, 0xF7 }, 2);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, sx_empty, 5), "an empty SysEx");

    enc_new(20);
    ble_midi_enc_sysex(&E, 5, (const uint8_t[]){ 0xF0, 1, 0xF7 }, 3);
    ble_midi_enc_msg(&E, 5, 0x90, 0x3C, 0x7F);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, sx_then, 10), "a message after a SysEx has its own status and timestamp");

    enc_new(20);
    ble_midi_enc_msg(&E, 5, 0x90, 0x3C, 0x7F);
    ble_midi_enc_sysex(&E, 5, (const uint8_t[]){ 0xF0, 1, 0xF7 }, 3);
    ble_midi_enc_msg(&E, 5, 0x90, 0x3C, 0x7F);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, mixed, 14),
          "a message, a SysEx and a message share a packet: 0xF0 has a timestamp byte, running status does not outlive the SysEx");

    for (i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i % 0x80);
    big[0] = 0xF0; big[sizeof big - 1] = 0xF7;
    enc_new(23);
    check(ble_midi_enc_sysex(&E, 0x1FFF, big, sizeof big) == 0 && ble_midi_enc_flush(&E) == 0, "a 300-byte SysEx at the largest timestamp");
    {
        uint32_t k, total = 0, ok = 1;
        for (k = 0; k < npk; k++) { total += pk[k].n; ok &= pk[k].n <= 23 && (pk[k].b[0] & 0xC0) == 0x80 && pk[k].b[0] == 0xBF; }
        check(ok && npk == 14, "every SysEx packet fits 23 bytes and carries the header's timestampHigh 0x3F");
        check(total == 298 + 14 + 4, "the packets hold the data, one header each, and a timestamp byte before 0xF0 and before 0xF7");
    }

    /* what it refuses */
    enc_new(20);
    check(ble_midi_enc_msg(&E, 0, 0x40, 0, 0) == BLE_MIDI_EBAD, "a data byte is not a status");
    check(ble_midi_enc_msg(&E, 0, 0xF0, 0, 0) == BLE_MIDI_EBAD, "SysEx has its own call");
    check(ble_midi_enc_msg(&E, 0, 0xF7, 0, 0) == BLE_MIDI_EBAD && ble_midi_enc_msg(&E, 0, 0xF4, 0, 0) == BLE_MIDI_EBAD, "EOX and an undefined status");
    check(ble_midi_enc_sysex(&E, 0, (const uint8_t[]){ 0xF0 }, 1) == BLE_MIDI_EBAD, "SysEx of one byte");
    check(ble_midi_enc_sysex(&E, 0, (const uint8_t[]){ 0x01, 0xF7 }, 2) == BLE_MIDI_EBAD, "SysEx not starting with 0xF0");
    check(ble_midi_enc_sysex(&E, 0, (const uint8_t[]){ 0xF0, 0x01 }, 2) == BLE_MIDI_EBAD, "SysEx not ending with 0xF7");
    check(ble_midi_enc_sysex(&E, 0, (const uint8_t[]){ 0xF0, 0x81, 0xF7 }, 3) == BLE_MIDI_EBAD, "a status byte inside SysEx");
    ble_midi_enc_flush(&E);
    check(npk == 0, "nothing refused reaches a packet");
    check(ble_midi_enc_init(&E, ebuf, 4, keep, NULL) == BLE_MIDI_EBAD, "a packet of 4 bytes cannot hold a header, a timestamp and a 3-byte message");
    check(ble_midi_enc_init(&E, ebuf, 5, keep, NULL) == 0, "5 bytes can");
    check(ble_midi_enc_init(&E, ebuf, 20, NULL, NULL) == BLE_MIDI_EBAD, "no send function");

    /* data bytes are masked, only 13 bits of time count */
    enc_new(20);
    ble_midi_enc_msg(&E, 0x123 + 0x2000, 0x90, 0xBC, 0xFF);
    ble_midi_enc_flush(&E);
    check(npk == 1 && is(0, one, 5), "data bytes keep 7 bits and the timestamp keeps 13");

    /* a send that fails drops that packet and counts it */
    enc_new(5);
    send_rc = -1;
    ble_midi_enc_msg(&E, 0, 0x90, 0x3C, 0x7F);
    check(ble_midi_enc_flush(&E) == BLE_MIDI_ESEND && E.dropped == 1 && E.packets == 0 && E.len == 0, "a refused packet is dropped and counted");
    send_rc = 0;
    ble_midi_enc_msg(&E, 0, 0x90, 0x3C, 0x7F);
    check(ble_midi_enc_flush(&E) == 0 && E.packets == 1 && npk == 1, "the next packet goes out");
    enc_new(5);
    ble_midi_enc_msg(&E, 0, 0x90, 0x3C, 0x7F);
    send_rc = -1;
    check(ble_midi_enc_msg(&E, 0, 0x90, 0x3E, 0x40) == BLE_MIDI_ESEND, "a send that fails while a full packet makes room is reported");
    send_rc = 0;
}

/* ---- the decoder, by the specification's rules ---- */
static void decoder(void)
{
    static const uint8_t d1[] = { 0x82, 0xA3, 0x90, 0x3C, 0x7F };
    static const uint8_t d2[] = { 0x82, 0xA3, 0x90, 0x3C, 0x7F, 0x3E, 0x40 };
    static const uint8_t d3[] = { 0x82, 0xA3, 0x90, 0x3C, 0x7F, 0xA5, 0x3E, 0x40 };
    static const uint8_t d4[] = { 0x80, 0xA0, 0x90, 0x3C, 0x7F, 0xA1, 0xF8, 0xA2, 0x3E, 0x40 };
    static const uint8_t d5[] = { 0x80, 0xA0, 0x3C, 0x7F };
    static const uint8_t d6[] = { 0x80, 0xFF, 0x90, 0x3C, 0x7F, 0x80, 0x80, 0x3C, 0x00 };
    static const uint8_t d6b[] = { 0x80, 0xFF, 0x90, 0x3C, 0x7F, 0x80, 0x80, 0x3C, 0x00, 0x82, 0x3E, 0x00 };
    static const uint8_t d6c[] = { 0xBF, 0xFF, 0x90, 0x3C, 0x7F, 0x80, 0x80, 0x3C, 0x00 };
    static const uint8_t d7[] = { 0x80, 0xA0, 0x90, 0x3C };
    static const uint8_t d8[] = { 0x80, 0xA0, 0x90, 0x3C, 0x7F, 0xA1, 0xF8, 0x3E, 0x40 };
    static const uint8_t s1[] = { 0x80, 0x85, 0xF0, 0x01, 0x02 };
    static const uint8_t s2[] = { 0x80, 0x03, 0x86, 0xF8, 0x04, 0x87, 0xF7 };
    static const uint8_t s3[] = { 0x80, 0x03, 0x04 }, s4[] = { 0x80, 0x05, 0x86, 0xF7 };
    static const uint8_t s5[] = { 0x80, 0x85, 0xF0, 0x01, 0x86, 0x90, 0x3C, 0x7F };
    static const uint8_t s6[] = { 0x80, 0x85, 0xF0, 0x01, 0x02, 0x03 };
    static const uint8_t s7[] = { 0x80, 0x85, 0xF0, 0x01, 0x85, 0xF7, 0x85, 0x90, 0x3C, 0x7F };
    static const uint8_t sysex_full[] = { 0xF0, 1, 2, 3, 4, 5, 0xF7 };
    static const uint8_t pcs[] = { 0x80, 0xA0, 0xC1, 0x05, 0x06, 0xA1, 0xD2, 0x07, 0xA1, 0xF1, 0x10 };

    dec_new();
    check(feed(d1, sizeof d1) == 0 && nev == 1 && ev_msg(0, 0x123, 0x90, 0x3C, 0x7F, 3), "one message, its 13-bit time rebuilt from header and timestamp byte");
    dec_new();
    check(feed(d2, sizeof d2) == 0 && nev == 2 && ev_msg(0, 0x123, 0x90, 0x3C, 0x7F, 3) && ev_msg(1, 0x123, 0x90, 0x3E, 0x40, 3),
          "running status with no timestamp byte takes the previous message's time (section 7 rule 2)");
    dec_new();
    check(feed(d3, sizeof d3) == 0 && nev == 2 && ev_msg(1, 0x125, 0x90, 0x3E, 0x40, 3), "running status with its own timestamp byte");
    dec_new();
    check(feed(d4, sizeof d4) == 0 && nev == 3 && ev_msg(0, 0x20, 0x90, 0x3C, 0x7F, 3) && ev_msg(1, 0x21, 0xF8, 0, 0, 1) && ev_msg(2, 0x22, 0x90, 0x3E, 0x40, 3),
          "real time in the middle does not cancel running status (section 7 rule 3)");
    dec_new();
    check(feed(d5, sizeof d5) == 2 && nev == 0, "running status does not carry over to the next packet (rule 4): its data bytes are dropped");
    dec_new();
    check(feed(d6, sizeof d6) == 0 && nev == 2 && ev_msg(0, 127, 0x90, 0x3C, 0x7F, 3) && ev_msg(1, 128, 0x80, 0x3C, 0, 3),
          "a timestamp low smaller than the last adds one to the high bits (section 9)");
    dec_new();
    check(feed(d6b, sizeof d6b) == 0 && nev == 3 && ev_msg(2, 130, 0x80, 0x3E, 0, 3), "and a message after the wrap is counted from the new high bits");
    dec_new();
    check(feed(d6c, sizeof d6c) == 0 && nev == 2 && ev_msg(0, 8191, 0x90, 0x3C, 0x7F, 3) && ev_msg(1, 0, 0x80, 0x3C, 0, 3),
          "timestampHigh wraps from 63 to 0 with the 13-bit time (8191 ms, then 0)");
    dec_new();
    check(feed(d7, sizeof d7) == 1 && nev == 0, "a message cut off by the end of the packet is dropped and counted");
    dec_new();
    check(feed(d8, sizeof d8) == 2 && nev == 2 && ev_msg(1, 0x21, 0xF8, 0, 0, 1), "data bytes with no timestamp after a real-time byte are dropped");
    dec_new();
    check(feed(pcs, sizeof pcs) == 0 && nev == 4 && ev_msg(0, 0x20, 0xC1, 5, 0, 2) && ev_msg(1, 0x20, 0xC1, 6, 0, 2) && ev_msg(2, 0x21, 0xD2, 7, 0, 2)
          && ev_msg(3, 0x21, 0xF1, 0x10, 0, 2), "2-byte messages, running status on them, then System Common");

    /* section 8 */
    dec_new();
    check(feed(s1, sizeof s1) == 0 && nev == 1 && evs[0].sysex && D.in_sysex && evs[0].n == 3, "a SysEx starts");
    check(feed(s2, sizeof s2) == 0 && nev == 2 && !D.in_sysex, "a continuation, a real-time byte inside it, more data, then 0xF7 with a timestamp byte");
    check(evs[0].sysex && evs[0].n == 6 && eq(evs[0].b, (const uint8_t[]){ 0xF0, 1, 2, 3, 4, 0xF7 }, 6) && evs[0].ts == 0x05 && evs[0].ts_end == 0x07,
          "the SysEx is whole, its real-time byte not in it; the start's time and the end's time");
    check(ev_msg(1, 0x06, 0xF8, 0, 0, 1), "the real-time message came out with its own time");

    dec_new();
    feed(s1, sizeof s1);
    check(feed(s3, sizeof s3) == 0 && D.in_sysex && nev == 1, "a continuation of a continuation");
    check(feed(s4, sizeof s4) == 0 && !D.in_sysex && nev == 1 && evs[0].n == 7 && eq(evs[0].b, sysex_full, 7), "and the end");

    dec_new();
    check(feed(s5, sizeof s5) == 1 && !D.in_sysex && nev == 2 && ev_msg(1, 0x06, 0x90, 0x3C, 0x7F, 3), "a channel status ends a SysEx unfinished; the message is kept");
    check(feed(s4, sizeof s4) == 2 && nev == 2, "a stray end of SysEx afterwards is dropped");

    dec_new();
    feed(s6, sizeof s6);
    check(D.in_sysex, "a SysEx with no end stays open across the packet end");
    ble_midi_dec_reset(&D);
    check(!D.in_sysex, "a reset forgets it");
    check(feed(s4, sizeof s4) == 2, "so a continuation after it is dropped");

    dec_new();
    check(feed(s7, sizeof s7) == 0 && nev == 2 && evs[0].sysex && evs[0].n == 3 && ev_msg(1, 0x05, 0x90, 0x3C, 0x7F, 3), "a whole SysEx, then a message after it");

    dec_new();
    check(feed(NULL, 0) == 1, "an empty packet is dropped");
    check(feed((const uint8_t[]){ 0x00, 0xA0, 0x90, 0x3C, 0x7F }, 5) == 1 && nev == 0, "a packet whose first byte is not a header is dropped");
    check(feed((const uint8_t[]){ 0x80 }, 1) == 0 && nev == 0, "a header alone is fine");
    check(feed((const uint8_t[]){ 0x80, 0xA0 }, 2) == 0 && nev == 0, "a header and a timestamp byte alone are fine");
    check(feed((const uint8_t[]){ 0x80, 0xA0, 0xF7 }, 3) == 1, "a stray 0xF7");
    check(feed((const uint8_t[]){ 0x80, 0xA0, 0xF4 }, 3) == 1 && feed((const uint8_t[]){ 0x80, 0xA0, 0xF5 }, 3) == 1, "undefined statuses");
    check(feed((const uint8_t[]){ 0x80, 0xA0, 0x90, 0x3C, 0xA1, 0x80, 0x3C, 0x00 }, 8) == 1 && nev == 1 && ev_msg(0, 0x21, 0x80, 0x3C, 0, 3),
          "a message cut short by the next timestamp is dropped, the one after it kept");
    check(D.packets == 8, "packets counted");

    /* a decoder with no sinks does not crash */
    memset(&D, 0, sizeof D);
    check(feed(d4, sizeof d4) == 0 && feed(s1, sizeof s1) == 0 && feed(s4, sizeof s4) == 0, "no callbacks set");
}

/* ---- round trips ---- */
typedef struct { int sysex; uint32_t ts, n; uint8_t b[100]; } want;
static want W[MAXEV];

static void roundtrip(uint32_t cap, uint32_t seed, uint32_t count)
{
    uint32_t i, ts, k, pn, badpk = 0, bytes = 0;
    int ok = 1;
    static const uint8_t common[] = { 0xF1, 0xF2, 0xF3, 0xF6 }, rts[] = { 0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF };
    char what[100];

    rnd_s = seed;
    enc_new(cap);
    ts = rnd() & 0x1FFF;
    for (i = 0; i < count; i++) {
        uint32_t kind = rnd() % 10, r = rnd();
        ts = (ts + (r % 4 == 0 ? rnd() % 400 : rnd() % 5)) & 0x1FFF;
        W[i].ts = ts;
        W[i].sysex = 0;
        if (kind < 6) {                                    /* channel messages, often the same status */
            static uint8_t last = 0x90;
            if (rnd() % 3 == 0) last = (uint8_t)(0x80 | (rnd() % 0x70));
            W[i].n = ble_midi_msg_len(last);
            W[i].b[0] = last; W[i].b[1] = rnd() & 0x7F; W[i].b[2] = rnd() & 0x7F;
        } else if (kind < 7) {
            W[i].b[0] = common[rnd() % 4]; W[i].n = ble_midi_msg_len(W[i].b[0]);
            W[i].b[1] = rnd() & 0x7F; W[i].b[2] = rnd() & 0x7F;
        } else if (kind < 9) {
            W[i].b[0] = rts[rnd() % 6]; W[i].n = 1;
        } else {
            uint32_t len = rnd() % 90;
            W[i].sysex = 1; W[i].n = len + 2;
            W[i].b[0] = 0xF0; W[i].b[len + 1] = 0xF7;
            for (k = 1; k <= len; k++) W[i].b[k] = rnd() & 0x7F;
        }
        if (W[i].sysex)
            ok &= ble_midi_enc_sysex(&E, ts, W[i].b, W[i].n) == 0;
        else
            ok &= ble_midi_enc_msg(&E, ts, W[i].b[0], W[i].b[1], W[i].b[2]) == 0;
        if (rnd() % 17 == 0) ok &= ble_midi_enc_flush(&E) == 0;
    }
    ble_midi_enc_flush(&E);
    snprintf(what, sizeof what, "round trip, packets of %u, seed %u: the encoder took every message", cap, seed);
    check(ok && npk < MAXPK, what);

    dec_new();
    for (pn = 0; pn < npk && pn < MAXPK; pn++) {
        ok = pk[pn].n <= cap && pk[pn].n >= 1 && (pk[pn].b[0] & 0xC0) == 0x80;
        if (!ok) badpk++;
        bytes += pk[pn].n;
        if (feed(pk[pn].b, pk[pn].n)) badpk++;
    }
    snprintf(what, sizeof what, "round trip, packets of %u, seed %u: every packet is a well-formed packet no longer than the size", cap, seed);
    check(!badpk, what);
    snprintf(what, sizeof what, "round trip, packets of %u, seed %u: as many messages came out as went in", cap, seed);
    check(nev == count, what);
    ok = nev == count;
    for (i = 0; ok && i < count; i++) {
        if (W[i].sysex) {
            ok = evs[i].sysex && evs[i].n == W[i].n && eq(evs[i].b, W[i].b, W[i].n) && evs[i].ts == W[i].ts && evs[i].ts_end == W[i].ts;
        } else {
            ok = !evs[i].sysex && evs[i].n == W[i].n && eq(evs[i].b, W[i].b, W[i].n) && evs[i].ts == W[i].ts;
        }
        if (!ok) fprintf(stderr, "  message %u of %u: want %s ts %u n %u, got %s ts %u n %u\n", i, count, W[i].sysex ? "sysex" : "msg", W[i].ts, W[i].n,
                         evs[i].sysex ? "sysex" : "msg", evs[i].ts, evs[i].n);
    }
    snprintf(what, sizeof what, "round trip, packets of %u, seed %u: every message back, bytes and 13-bit time", cap, seed);
    check(ok, what);
    (void)bytes;
}

static void running_status_saves_bytes(void)
{
    uint32_t i, with = 0, without = 0;
    enc_new(100);
    for (i = 0; i < 8; i++) ble_midi_enc_msg(&E, 10, 0x90, (uint8_t)(0x30 + i), 0x40);
    ble_midi_enc_flush(&E);
    with = pk[0].n;
    enc_new(100);
    for (i = 0; i < 8; i++) ble_midi_enc_msg(&E, 10, (uint8_t)(0x90 + (i & 1)), (uint8_t)(0x30 + i), 0x40);
    ble_midi_enc_flush(&E);
    without = pk[0].n;
    check(with == 19 && without == 33,
          "eight notes of one status at one time take 19 bytes; alternating statuses take 33");
}

/* ---- the bridge to the app's input ---- */
static uint8_t sink_buf[400];
static uint32_t sink_n, sink_calls;
static void sink(void *ctx, const uint8_t *m, uint32_t n)
{
    (void)ctx;
    sink_calls++;
    sink_n = n;
    if (n <= sizeof sink_buf) memcpy(sink_buf, m, n);
}
static int to_in(void *ctx, const uint8_t *p, uint32_t n) { return ble_midi_in_write(ctx, p, n) ? -1 : 0; }

static int get3(ble_midi_in *in, uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t st = 0xEE, d1 = 0xEE, d2 = 0xEE;
    return ble_midi_get(in, &st, &d1, &d2) == 1 && st == a && d1 == b && d2 == c;
}

static void bridge(void)
{
    static ble_midi_in in;
    static uint8_t sx[16];
    static const uint8_t notes[] = { 0x80, 0xA0, 0x90, 0x3C, 0x7F, 0xA1, 0xF8, 0xA2, 0xC1, 0x05, 0xA3, 0xD5, 0x0A };
    static const uint8_t s1[] = { 0x80, 0x85, 0xF0, 0x01, 0x02 }, s2[] = { 0x80, 0x03, 0x86, 0xF8, 0x04, 0x87, 0xF7 };
    static const uint8_t full[] = { 0xF0, 1, 2, 3, 4, 0xF7 };
    uint8_t st, d1, d2, pkt[5] = { 0x80, 0xA0, 0x90, 0, 0x7F }, big[40];
    uint32_t i;

    ble_midi_in_init(&in, sx, sizeof sx, sink, NULL);
    check(!ble_midi_get(&in, &st, &d1, &d2), "nothing queued, nothing to get");
    check(ble_midi_in_write(&in, notes, sizeof notes) == 0, "a packet of four messages");
    check(get3(&in, 0x90, 0x3C, 0x7F) && get3(&in, 0xF8, 0, 0) && get3(&in, 0xC1, 0x05, 0) && get3(&in, 0xD5, 0x0A, 0) && !ble_midi_get(&in, &st, &d1, &d2),
          "midi_get's contract: (status, data 1, data 2) in order, the data bytes a message lacks read 0");

    ble_midi_in_write(&in, s1, sizeof s1);
    check(sink_calls == 0, "half a SysEx goes nowhere yet");
    ble_midi_in_write(&in, s2, sizeof s2);
    check(sink_calls == 1 && sink_n == 6 && eq(sink_buf, full, 6), "a SysEx over two packets reaches the sink whole, 0xF0 to 0xF7");
    check(get3(&in, 0xF8, 0, 0) && !ble_midi_get(&in, &st, &d1, &d2), "the real-time byte inside it went to the message queue");

    /* too long for the buffer: dropped, the next one still works */
    big[0] = 0xF0; for (i = 1; i < 21; i++) big[i] = (uint8_t)i; big[21] = 0xF7;
    {
        ble_midi_enc E2;
        uint8_t eb[20];
        ble_midi_enc_init(&E2, eb, 8, to_in, &in);
        ble_midi_enc_sysex(&E2, 9, big, 22);
        ble_midi_enc_flush(&E2);
        check(sink_calls == 1 && in.sysex_dropped == 1, "a SysEx longer than the buffer is dropped and counted");
        ble_midi_enc_sysex(&E2, 9, full, 6);
        ble_midi_enc_flush(&E2);
        check(sink_calls == 2 && sink_n == 6 && eq(sink_buf, full, 6), "and the next one goes through");
    }
    /* a start with no end, then a new start */
    in.sysex_dropped = 0;
    ble_midi_in_write(&in, s1, sizeof s1);
    ble_midi_in_write(&in, s1, sizeof s1);
    check(in.sysex_dropped == 1, "a SysEx that never ended is counted when the next one starts");
    ble_midi_in_reset(&in);
    check(!in.dec.in_sysex && in.sx_len == 0 && !ble_midi_get(&in, &st, &d1, &d2), "a reset leaves nothing half-received or queued");
    sink_calls = 0;
    ble_midi_in_write(&in, (const uint8_t[]){ 0x80, 0x05, 0x86, 0xF7 }, 4);
    check(sink_calls == 0, "after a reset a continuation with no start goes nowhere");

    /* the queue: 64 messages, the rest dropped and counted, oldest first */
    ble_midi_in_reset(&in);
    in.dropped = 0;
    for (i = 0; i < 70; i++) { pkt[3] = (uint8_t)i; ble_midi_in_write(&in, pkt, sizeof pkt); }
    check(in.dropped == 6, "a full queue drops the newest and counts it");
    for (i = 0; i < 64 && get3(&in, 0x90, (uint8_t)i, 0x7F); i++)
        ;
    check(i == 64 && !ble_midi_get(&in, &st, &d1, &d2), "the 64 oldest came out in order");
    for (i = 0; i < 100; i++) {         /* the counters run past the ring size */
        pkt[3] = (uint8_t)i;
        ble_midi_in_write(&in, pkt, sizeof pkt);
        if (!get3(&in, 0x90, (uint8_t)i, 0x7F)) break;
    }
    check(i == 100, "put and get across the ring's wrap");

    /* no sink, no buffer */
    ble_midi_in_init(&in, NULL, 0, NULL, NULL);
    sink_calls = 0;
    ble_midi_in_write(&in, s1, sizeof s1);
    ble_midi_in_write(&in, (const uint8_t[]){ 0x80, 0x03, 0x86, 0xF7 }, 4);
    check(sink_calls == 0 && in.sysex_dropped == 1, "with no buffer a SysEx is counted and dropped");
    ble_midi_in_init(&in, sx, sizeof sx, NULL, NULL);
    ble_midi_in_write(&in, s1, sizeof s1);
    ble_midi_in_write(&in, (const uint8_t[]){ 0x80, 0x03, 0x86, 0xF7 }, 4);
    check(in.sysex_dropped == 1, "with no sink a SysEx is counted and dropped");

    /* the encoder into the bridge: what goes in comes out */
    {
        ble_midi_enc E2;
        uint8_t eb[23], want_st[40], want_d1[40], want_d2[40];
        uint32_t round, k, n, ok = 1;
        ble_midi_in_init(&in, sx, sizeof sx, sink, NULL);
        ble_midi_enc_init(&E2, eb, 23, to_in, &in);
        rnd_s = 777;
        for (round = 0; round < 20; round++) {
            n = 1 + rnd() % 30;
            for (k = 0; k < n; k++) {
                want_st[k] = (uint8_t)(0x80 | (rnd() % 0x70));
                want_d1[k] = rnd() & 0x7F;
                want_d2[k] = ble_midi_msg_len(want_st[k]) > 2 ? rnd() & 0x7F : 0;
                if (ble_midi_msg_len(want_st[k]) < 2) want_d1[k] = 0;
                ble_midi_enc_msg(&E2, round * 40 + k, want_st[k], want_d1[k], want_d2[k]);
            }
            ble_midi_enc_flush(&E2);
            for (k = 0; k < n; k++) ok &= get3(&in, want_st[k], want_d1[k], want_d2[k]);
            ok &= !ble_midi_get(&in, &st, &d1, &d2);
        }
        check(ok && E2.dropped == 0, "messages through the encoder, the decoder and the queue come out as they went in");
    }
}

/* ---- garbage in ---- */
static void fuzz(void)
{
    static ble_midi_in in;
    static uint8_t sx[64];
    uint8_t p[48], st, d1, d2;
    uint32_t i, k, n, ok = 1, total = 0;

    ble_midi_in_init(&in, sx, sizeof sx, sink, NULL);
    rnd_s = 99;
    for (i = 0; i < 40000; i++) {
        n = rnd() % 48;
        for (k = 0; k < n; k++) p[k] = (uint8_t)(rnd() % 3 ? (rnd() & 0x7F) | (rnd() % 4 == 0 ? 0x80 : 0) : 0xF0 + rnd() % 16);
        if (n && rnd() % 8) p[0] = (uint8_t)(0x80 | (rnd() & 0x3F));
        ble_midi_in_write(&in, p, n);
        ok &= in.sx_len <= in.sx_cap && in.w - in.r <= BLE_MIDI_RING;
        if (i % 7 == 0)
            while (ble_midi_get(&in, &st, &d1, &d2)) {
                ok &= st >= 0x80 && d1 < 0x80 && d2 < 0x80 && ble_midi_msg_len(st) > 0;
                total++;
            }
    }
    check(ok, "random packets: no queue or buffer overrun, every message out is well formed");
    check(total > 1000 && in.dec.bad > 1000, "random packets: messages came out and bad bytes were counted");
}

int main(void)
{
    static const uint32_t caps[] = { 5, 6, 7, 8, 9, 12, 20, 23, 64, 244 };
    uint32_t i, s;
    gatt();
    lengths();
    encoder();
    decoder();
    running_status_saves_bytes();
    for (i = 0; i < sizeof caps / sizeof caps[0]; i++)
        for (s = 1; s <= 3; s++)
            roundtrip(caps[i], 1000u * s + caps[i], 250);
    bridge();
    fuzz();
    if (bad) {
        fprintf(stderr, "ble_midi: FAILED\n");
        return 1;
    }
    printf("ble_midi: %d checks ok\n", checks);
    return 0;
}
