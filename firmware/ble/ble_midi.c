/* SPDX-License-Identifier: GPL-3.0-only
 * The BLE-MIDI profile: packet encoder and decoder, advertising data and the bridge to the app's MIDI input.
 * Written from the BLE-MIDI 1.0 specification (MMA/AMEI RP-052); see ble_midi.h. Its own copy loops instead of
 * string.h, so the directory compiles with no C library headers, like hci.c. */
#include "ble_midi.h"

static void copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    while (n--)
        *dst++ = *src++;
}

static void zero(void *p, uint32_t n)
{
    uint8_t *b = p;
    while (n--)
        *b++ = 0;
}

#define TS_MASK     0x1FFFu     /* a timestamp is 13 bits of milliseconds (section 9) */
#define HIGH_MASK   0x3Fu       /* timestampHigh is 6 bits */

/* 03B80E5A-EDE8-4B33-A751-6CE34EC4C700 and 7772E5DB-3868-4112-A1A9-F2669D106BF3 (section 3), least significant first */
const uint8_t ble_midi_service_uuid[16] = {
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03
};
const uint8_t ble_midi_char_uuid[16] = {
    0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77
};

uint32_t ble_midi_read_value(uint8_t *out)
{
    (void)out;
    return 0;                   /* section 5: the first read gets a packet with no payload */
}

/* Advertising data types (Core Specification Supplement Part A) */
#define AD_FLAGS            0x01
#define AD_UUID128_COMPLETE 0x07
#define AD_NAME_SHORT       0x08
#define AD_NAME_COMPLETE    0x09
#define ADV_MAX             31

uint32_t ble_midi_adv_data(uint8_t *out)
{
    out[0] = 2;
    out[1] = AD_FLAGS;
    out[2] = 0x06;              /* LE General Discoverable, BR/EDR not supported */
    out[3] = 17;
    out[4] = AD_UUID128_COMPLETE;
    copy(out + 5, ble_midi_service_uuid, 16);
    return 21;
}

uint32_t ble_midi_scan_rsp(uint8_t *out, const char *name)
{
    uint32_t n = 0, room = ADV_MAX - 2;
    if (!name)
        return 0;
    while (name[n] && n < room)
        n++;
    out[0] = (uint8_t)(1 + n);
    out[1] = name[n] ? AD_NAME_SHORT : AD_NAME_COMPLETE;
    copy(out + 2, (const uint8_t *)name, n);
    return 2 + n;
}

uint8_t ble_midi_msg_len(uint8_t st)
{
    if (st < 0x80)
        return 0;
    if (st < 0xC0)
        return 3;               /* note off and on, key pressure, control change */
    if (st < 0xE0)
        return 2;               /* program change, channel pressure */
    if (st < 0xF0)
        return 3;               /* pitch bend */
    switch (st) {
    case 0xF1: case 0xF3: return 2;     /* time code, song select */
    case 0xF2: return 3;                /* song position */
    case 0xF6: return 1;                /* tune request */
    case 0xF0: case 0xF4: case 0xF5: case 0xF7: return 0;
    default: return 1;                  /* 0xF8..0xFF: real time */
    }
}

/* ---- the encoder ---- */

int ble_midi_enc_init(ble_midi_enc *e, uint8_t *buf, uint32_t cap, ble_midi_send_fn send, void *ctx)
{
    if (!e || !buf || !send || cap < 5)
        return BLE_MIDI_EBAD;
    zero(e, sizeof *e);
    e->buf = buf;
    e->cap = cap;
    e->send = send;
    e->ctx = ctx;
    return BLE_MIDI_OK;
}

static int emit(ble_midi_enc *e)
{
    int rc;
    if (!e->len)
        return BLE_MIDI_OK;
    rc = e->send(e->ctx, e->buf, e->len);
    e->len = 0;
    if (rc) {
        e->dropped++;
        return BLE_MIDI_ESEND;
    }
    e->packets++;
    return BLE_MIDI_OK;
}

/* a new packet: the header takes timestampHigh from ts; running status and the timestamp memory start over */
static void begin(ble_midi_enc *e, uint32_t ts)
{
    e->high = (uint8_t)((ts >> 7) & HIGH_MASK);
    e->buf[0] = (uint8_t)(0x80u | e->high);
    e->len = 1;
    e->have_prev = 0;
    e->rs = 0;
    e->force_ts = 0;
}

/* Would the receiver rebuild ts from this packet? It starts from the header's high bits and adds one whenever a
 * timestamp low is smaller than the one before it (section 9). */
static int representable(const ble_midi_enc *e, uint32_t ts)
{
    uint8_t high = e->high;
    if (e->have_prev && (ts & 0x7Fu) < e->prev_low)
        high = (uint8_t)((high + 1u) & HIGH_MASK);
    return high == ((ts >> 7) & HIGH_MASK);
}

static void put_ts(ble_midi_enc *e, uint32_t ts)
{
    uint8_t low = (uint8_t)(ts & 0x7Fu);
    if (e->have_prev && low < e->prev_low)
        e->high = (uint8_t)((e->high + 1u) & HIGH_MASK);
    e->prev_low = low;
    e->have_prev = 1;
    e->buf[e->len++] = (uint8_t)(0x80u | low);
}

/* the bytes message st adds to the packet being built */
static uint32_t cost(const ble_midi_enc *e, uint32_t ts, uint8_t st, uint8_t ml, int *running, int *with_ts)
{
    *running = st < 0xF0 && st == e->rs;
    /* a running-status message may leave its timestamp out only when it is the previous message's (section 7 rule 2),
     * and not after a System Common or Real-Time message (rule 3) */
    *with_ts = !*running || e->force_ts || !e->have_prev || (ts & 0x7Fu) != e->prev_low;
    return (uint32_t)*with_ts + (uint32_t)(*running ? ml - 1 : ml);
}

int ble_midi_enc_msg(ble_midi_enc *e, uint32_t ts, uint8_t st, uint8_t d1, uint8_t d2)
{
    uint8_t ml = ble_midi_msg_len(st);
    int rc = BLE_MIDI_OK, running, with_ts;
    uint32_t need;
    if (!e || !ml)
        return BLE_MIDI_EBAD;
    ts &= TS_MASK;
    if (e->len) {
        need = cost(e, ts, st, ml, &running, &with_ts);
        if (!representable(e, ts) || e->len + need > e->cap)
            rc = emit(e);
    }
    if (!e->len)
        begin(e, ts);
    cost(e, ts, st, ml, &running, &with_ts);
    if (with_ts)
        put_ts(e, ts);
    if (!running)
        e->buf[e->len++] = st;
    if (ml > 1)
        e->buf[e->len++] = d1 & 0x7Fu;
    if (ml > 2)
        e->buf[e->len++] = d2 & 0x7Fu;
    if (st < 0xF0) {
        e->rs = st;
        e->force_ts = 0;
    } else {
        e->force_ts = 1;
    }
    return rc;
}

int ble_midi_enc_sysex(ble_midi_enc *e, uint32_t ts, const uint8_t *data, uint32_t n)
{
    uint32_t i, rem, space, take;
    int rc = BLE_MIDI_OK, r;
    if (!e || !data || n < 2 || data[0] != 0xF0 || data[n - 1] != 0xF7)
        return BLE_MIDI_EBAD;
    for (i = 1; i + 1 < n; i++)
        if (data[i] & 0x80u)
            return BLE_MIDI_EBAD;
    ts &= TS_MASK;
    /* the start: a timestamp, 0xF0, and either one data byte or the closing timestamp and 0xF7 */
    if (e->len && (!representable(e, ts) || e->len + (n == 2 ? 4u : 3u) > e->cap))
        rc = emit(e);
    if (!e->len)
        begin(e, ts);
    put_ts(e, ts);
    e->buf[e->len++] = 0xF0;
    e->rs = 0;                  /* SysEx cancels running status */
    rem = n - 2;
    data++;
    for (;;) {
        space = e->cap - e->len;
        if (rem + 2 <= space) {                 /* the rest, the timestamp and 0xF7 fit here (section 8 rule 7) */
            copy(e->buf + e->len, data, rem);
            e->len += rem;
            put_ts(e, ts);
            e->buf[e->len++] = 0xF7;
            e->force_ts = 0;
            return rc;
        }
        take = rem < space ? rem : space;       /* fill the packet; a continuation has no timestamp byte (rule 3) */
        copy(e->buf + e->len, data, take);
        e->len += take;
        data += take;
        rem -= take;
        r = emit(e);
        if (r)
            rc = r;
        begin(e, ts);
    }
}

int ble_midi_enc_flush(ble_midi_enc *e)
{
    return e ? emit(e) : BLE_MIDI_EBAD;
}

/* ---- the decoder ---- */

void ble_midi_dec_reset(ble_midi_dec *d)
{
    d->in_sysex = 0;
}

static void dec_msg(ble_midi_dec *d, uint16_t ts, const uint8_t *m, uint8_t n)
{
    if (d->msg)
        d->msg(d->ctx, ts, m, n);
}

static void dec_sysex(ble_midi_dec *d, uint16_t ts, const uint8_t *p, uint32_t n, int first, int last)
{
    if (d->sysex)
        d->sysex(d->ctx, ts, p, n, first, last);
}

enum { ST_IDLE, ST_AFTER_TS, ST_DATA };

uint32_t ble_midi_dec_packet(ble_midi_dec *d, const uint8_t *p, uint32_t n)
{
    uint32_t i = 1, j, bad0 = d->bad;
    uint8_t high, prev_low = 0, have_prev = 0, rs = 0, rs_no_ts = 0, state = ST_IDLE, m[3] = {0, 0, 0}, ml = 0, got = 0, b;
    uint16_t ts;

    d->packets++;
    if (n < 1 || !(p[0] & 0x80u)) {
        d->bad++;
        return d->bad - bad0;
    }
    high = p[0] & HIGH_MASK;
    ts = (uint16_t)(high << 7);
    while (i < n) {
        b = p[i];
        if (state == ST_DATA) {
            if (b & 0x80u) {            /* a message cut short by the next status or timestamp */
                d->bad++;
                state = ST_IDLE;
                continue;
            }
            m[got++] = b;
            i++;
            if (got == ml) {
                dec_msg(d, ts, m, ml);
                if (m[0] < 0xF0) {
                    rs = m[0];
                    rs_no_ts = 1;
                } else {
                    rs_no_ts = 0;
                }
                state = ST_IDLE;
            }
            continue;
        }
        if (b & 0x80u) {
            i++;
            if (state == ST_IDLE) {     /* a timestamp byte */
                uint8_t low = b & 0x7Fu;
                if (have_prev && low < prev_low)
                    high = (uint8_t)((high + 1u) & HIGH_MASK);
                prev_low = low;
                have_prev = 1;
                ts = (uint16_t)(high << 7 | low);
                state = ST_AFTER_TS;
                continue;
            }
            state = ST_IDLE;            /* the byte after a timestamp is a status byte */
            if (d->in_sysex) {
                if (b == 0xF7) {
                    dec_sysex(d, ts, p + i - 1, 1, 0, 1);
                    d->in_sysex = 0;
                    rs_no_ts = 0;
                    continue;
                }
                if (b >= 0xF8) {        /* real time inside SysEx (section 8 rule 4) */
                    m[0] = b;
                    dec_msg(d, ts, m, 1);
                    continue;
                }
                d->bad++;               /* anything else ends the SysEx unfinished */
                d->in_sysex = 0;
            }
            if (b == 0xF0) {
                d->in_sysex = 1;
                rs = 0;
                rs_no_ts = 0;
                dec_sysex(d, ts, p + i - 1, 1, 1, 0);
            } else if ((ml = ble_midi_msg_len(b)) == 0) {
                d->bad++;               /* a stray 0xF7, or 0xF4 or 0xF5 */
            } else {
                m[0] = b;
                got = 1;
                if (ml == 1) {
                    dec_msg(d, ts, m, 1);
                    rs_no_ts = 0;
                } else {
                    state = ST_DATA;
                }
            }
            continue;
        }
        /* a data byte */
        if (d->in_sysex) {              /* a continuation (no timestamp byte, section 8 rule 3), or after a real-time byte */
            for (j = i; j < n && !(p[j] & 0x80u); j++)
                ;
            dec_sysex(d, ts, p + i, j - i, 0, 0);
            i = j;
            state = ST_IDLE;
            continue;
        }
        if (rs && (state == ST_AFTER_TS || rs_no_ts)) {     /* running status, with or without its own timestamp */
            ml = ble_midi_msg_len(rs);
            m[0] = rs;
            got = 1;
            state = ST_DATA;
            continue;
        }
        d->bad++;                       /* data with nothing to belong to: no status yet, or running status cancelled */
        i++;
        state = ST_IDLE;
    }
    if (state == ST_DATA)
        d->bad++;                       /* the packet ended inside a message */
    return d->bad - bad0;
}

/* ---- the bridge ---- */

static void in_msg(void *ctx, uint16_t ts, const uint8_t *m, uint8_t n)
{
    ble_midi_in *in = ctx;
    uint8_t *slot;
    (void)ts;
    if (in->w - in->r >= BLE_MIDI_RING) {
        in->dropped++;
        return;
    }
    slot = in->ring[in->w % BLE_MIDI_RING];
    slot[0] = m[0];
    slot[1] = n > 1 ? m[1] : 0;
    slot[2] = n > 2 ? m[2] : 0;
    in->w++;
}

static void in_sysex(void *ctx, uint16_t ts, const uint8_t *p, uint32_t n, int first, int last)
{
    ble_midi_in *in = ctx;
    (void)ts;
    if (first) {
        if (in->sx_len)
            in->sysex_dropped++;        /* the one before never ended */
        in->sx_len = 0;
        in->sx_over = 0;
    } else if (!in->sx_len) {
        in->sx_over = 1;                /* a continuation with no start here */
    }
    if (!in->sx_over) {
        if (!in->sx || in->sx_len + n > in->sx_cap)
            in->sx_over = 1;
        else {
            copy(in->sx + in->sx_len, p, n);
            in->sx_len += n;
        }
    }
    if (last) {
        if (in->sx_over || !in->sysex)
            in->sysex_dropped++;
        else
            in->sysex(in->sx_ctx, in->sx, in->sx_len);
        in->sx_len = 0;
        in->sx_over = 0;
    }
}

void ble_midi_in_reset(ble_midi_in *in)
{
    in->r = in->w = 0;
    in->sx_len = 0;
    in->sx_over = 0;
    ble_midi_dec_reset(&in->dec);
}

void ble_midi_in_init(ble_midi_in *in, uint8_t *sx, uint32_t sx_cap,
                      void (*sink)(void *ctx, const uint8_t *msg, uint32_t n), void *ctx)
{
    zero(in, sizeof *in);
    in->dec.msg = in_msg;
    in->dec.sysex = in_sysex;
    in->dec.ctx = in;
    in->sx = sx;
    in->sx_cap = sx ? sx_cap : 0;
    in->sysex = sink;
    in->sx_ctx = ctx;
}

uint32_t ble_midi_in_write(ble_midi_in *in, const uint8_t *packet, uint32_t n)
{
    return ble_midi_dec_packet(&in->dec, packet, n);
}

int ble_midi_get(ble_midi_in *in, uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    const uint8_t *slot;
    if (in->r == in->w)
        return 0;
    slot = in->ring[in->r % BLE_MIDI_RING];
    *st = slot[0];
    *d1 = slot[1];
    *d2 = slot[2];
    in->r++;
    return 1;
}
