/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SoundFX Macs Opera CMF player, freestanding C99. After AdPlug's cmfmcsop.cpp
 * (Copyright (c) 2017 Sebastian Kienzl); the chip mapping and rhythm-mode quirks are his findings.
 *
 * File: "A.H." NUL, 99 x u16 order (99 = end), u16 patterns, u16 speed (1..3), u16 rhythm (1 = on),
 * u16 instruments, then instruments (28 x u16 + 13-byte name = 69 B) and patterns: 6-byte events
 * (row, col, note, instrument, volume, pitch), each pattern ended by a 0xFF row byte.
 * note: 1 = end of pattern, 4 = key off, 24 = C-0 .. 119 = H-7. instrument 0 = keep. pitch: bend, 32 = centre. */
#include "macsop.h"

/* FXDRIVER's own table (data segment offset 0x28). AdPlug's ends 0x221/0x242/0x264/0x288 because it was read off
 * the chip at pitch 32, where the driver's bend arithmetic adds one to the top four notes (see set_note). */
static const uint16_t fnum[12] = { 0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca, 0x1e5, 0x202, 0x220, 0x241, 0x263, 0x287 };
#define BEND_RANGE 1               /* semitones; FXDRIVER init sets 1, games may change it through the driver API */
static const uint8_t slot_reg[18] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21 };
static const uint8_t ch_op1[9] = { 0, 1, 2, 6, 7, 8, 12, 13, 14 };
static const uint8_t ch_op2[9] = { 3, 4, 5, 9, 10, 11, 15, 16, 17 };
/* rhythm mode: channel 6 BD (two slots, like a melodic channel), 7 SD, 8 TOM, 9 TC, 10 HH */
static const uint8_t rhy_slot[MACSOP_CHANNELS] = { 0, 0, 0, 0, 0, 0, 12, 16, 14, 17, 13 };
enum { CH_BD = 6, CH_SN = 7, CH_TOM = 8, CH_TC = 9, CH_HH = 10 };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int valid_ch(const macsop *m, int ch)
{
    return ch >= 0 && (m->rhythm ? ch < MACSOP_CHANNELS : ch < 9);
}
static int rhythm_ch(const macsop *m, int ch) { return m->rhythm && ch >= 6; }

static void wr(macsop *m, uint16_t reg, uint8_t val) { m->write(m->ctx, reg, val); }

static void set_slot(macsop *m, int slot, const macsop_slot *s)
{
    uint8_t r = slot_reg[slot];
    wr(m, 0x20 + r, (uint8_t)((s->mult & 0xf) | ((s->ksr & 1) << 4) | ((s->eg & 1) << 5) | ((s->vib & 1) << 6) | ((s->am & 1) << 7)));
    wr(m, 0x60 + r, (uint8_t)(((s->ar & 0xf) << 4) | (s->dr & 0xf)));
    wr(m, 0x80 + r, (uint8_t)(((s->sl & 0xf) << 4) | (s->rr & 0xf)));
    wr(m, 0xe0 + r, (uint8_t)(s->wave & 3));
}

static void set_instr(macsop *m, int ch, const macsop_instr *in)
{
    if (!valid_ch(m, ch) || m->cur_instr[ch] == in)
        return;
    if (!rhythm_ch(m, ch) || ch == CH_BD) {
        wr(m, 0xc0 + ch, (uint8_t)(((in->feedback & 7) << 1) | (1 - (in->connection & 1))));
        set_slot(m, ch_op1[ch], &in->op[0]);
        set_slot(m, ch_op2[ch], &in->op[1]);
    } else {
        set_slot(m, rhy_slot[ch], &in->op[0]);
    }
    m->cur_instr[ch] = in;
}

/* FXDRIVER's own arithmetic (V1.06, routine at code offset 0x3100): rounded /128, not AdPlug's floor /127 */
static int atten(int tl, int vol)
{
    tl &= 0x3f;
    if (vol > 127) vol = 127;
    return 63 - ((vol * (63 - tl) + 64) >> 7);
}

static void set_volume(macsop *m, int ch, int vol)
{
    const macsop_instr *in = m->cur_instr[ch];
    if (!valid_ch(m, ch) || !in)
        return;
    if (!rhythm_ch(m, ch) || ch == CH_BD) {
        /* a modulator's level is not scaled by the volume column; a carrier's is */
        wr(m, 0x40 + slot_reg[ch_op1[ch]], (uint8_t)(((in->op[0].ksl & 3) << 6) |
           (in->connection == 0 ? atten(in->op[0].tl, vol) : (in->op[0].tl & 0x3f))));
        wr(m, 0x40 + slot_reg[ch_op2[ch]], (uint8_t)(((in->op[1].ksl & 3) << 6) | atten(in->op[1].tl, vol)));
    } else {
        wr(m, 0x40 + slot_reg[rhy_slot[ch]], (uint8_t)(((in->op[0].ksl & 3) << 6) | atten(in->op[0].tl, vol)));
    }
}

static void key_on(macsop *m, int ch)
{
    if (!rhythm_ch(m, ch)) {
        m->bx[ch] |= 0x20;
        wr(m, 0xb0 + ch, m->bx[ch]);
    } else {
        m->bd |= (uint8_t)(1 << (10 - ch));
        wr(m, 0xbd, m->bd);
    }
}

static void key_off(macsop *m, int ch)
{
    if (!rhythm_ch(m, ch)) {
        m->bx[ch] &= (uint8_t)~0x20;
        wr(m, 0xb0 + ch, m->bx[ch]);
    } else {
        m->bd &= (uint8_t)~(1 << (10 - ch));
        wr(m, 0xbd, m->bd);
    }
}

static void set_axbx(macsop *m, int ch, uint8_t ax, uint8_t bx)
{
    if (ch < 0 || ch >= 8)                      /* as the original: channel 8 is never retuned */
        return;
    wr(m, 0xa0 + ch, ax);
    m->bx[ch] = bx;
    wr(m, 0xb0 + ch, bx);
}

static int set_note(macsop *m, int ch, int note, int pitch)
{
    if (!valid_ch(m, ch) || note < 23 || note >= 120)
        return 0;
    /* FXDRIVER V1.06, routine at code offset 0x34AD: a 14-bit pitch bend, centre 0x2000, range BEND_RANGE
     * semitones. The player hands it (pitch_cell << 8) | 0xFF, so even the nominal centre value 32 bends up by
     * 255/8192 of a semitone (which is what makes A and B land on 0x242 / 0x288 on the chip). */
    int n = note - 12;                          /* the driver's note numbering */
    int f = fnum[n % 12], block = n / 12 - 1;
    int b = ((pitch & 0xff) << 8) | 0xff;
    if (rhythm_ch(m, ch) && ch != CH_BD)       /* set_bend (0x2C79) ignores channels above 6 in rhythm mode */
        b = 0x2000;
    if (b > 0x2000) {
        int t = fnum[(n + BEND_RANGE) % 12];
        if (t < f) t <<= 1;
        f += (int)(((uint32_t)(t - f) * (uint32_t)(b - 0x2000)) >> 13);
        while (f > 0x3ff) { f >>= 1; block++; }
    } else if (b < 0x2000) {
        int t = fnum[(n - BEND_RANGE + 12) % 12];
        if (t > f) t >>= 1;
        f -= (int)(((uint32_t)(f - t) * (uint32_t)(0x2000 - b)) >> 13);
        while (f < fnum[0]) { f <<= 1; block--; }
    }
    if (block < 0) block = 0;
    if (block > 7) block = 7;
    uint8_t ax = (uint8_t)(f & 0xff);
    uint8_t bx = (uint8_t)(((f >> 8) & 3) | (block << 2));
    if (!rhythm_ch(m, ch)) {
        set_axbx(m, ch, ax, bx);
    } else {                                     /* what the original driver does, quirks included */
        if (ch == CH_BD) set_axbx(m, 6, ax, bx);
        set_axbx(m, 7, ax, bx);
        if (ch == CH_SN || ch == CH_TOM) set_axbx(m, 8, ax, bx);
    }
    return 1;
}

static void note_event(macsop *m, const uint8_t *e)
{
    int ch = e[1], note = e[2], in = e[3], vol = e[4], pitch = e[5];
    if (!valid_ch(m, ch))
        return;
    key_off(m, ch);
    if (note == 4)
        return;
    if (in >= 1 && in <= m->n_instr)
        set_instr(m, ch, &m->instr[in - 1]);
    set_volume(m, ch, vol);
    if (set_note(m, ch, note, pitch))
        key_on(m, ch);
}

/* -> 1 while the song goes on, 0 at its end */
static int advance_row(macsop *m)
{
    for (;;) {
        if (++m->row >= 64) {
            m->row = 0;
            do {
                m->order_idx++;
                if (m->order_idx >= 99 || m->order[m->order_idx] == 99)
                    return 0;
            } while (m->order[m->order_idx] >= m->n_patterns);
            m->ev = m->pat_off[m->order[m->order_idx]];
        }
        const uint8_t *e = m->blob + m->ev;
        if (m->ev + 6 <= m->size && e[0] == m->row && e[2] == 1)   /* pattern break */
            m->row = 64;
        else
            return 1;
    }
}

static void reset_player(macsop *m)
{
    m->row = 64;
    m->order_idx = 0xFFFF;
    m->done = !advance_row(m);
}

int macsop_load(macsop *m, const uint8_t *blob, uint32_t size,
                void (*write)(void *, uint16_t, uint8_t), void *ctx)
{
    uint32_t p = 0;
    if (size < 5 + 198 + 8 || blob[0] != 'A' || blob[1] != '.' || blob[2] != 'H' || blob[3] != '.')
        return -1;
    m->blob = blob; m->size = size; m->write = write; m->ctx = ctx;
    p = 5;
    m->n_orders = 99;
    for (int i = 0; i < 99; i++, p += 2) {
        m->order[i] = rd16(blob + p);
        if (m->order[i] == 99 && m->n_orders == 99)
            m->n_orders = (uint16_t)i;
    }
    m->n_patterns = rd16(blob + p); p += 2;
    m->speed = (uint8_t)rd16(blob + p); p += 2;
    m->rhythm = rd16(blob + p) == 1; p += 2;
    m->n_instr = rd16(blob + p); p += 2;
    if (m->speed < 1 || m->speed > 3 || m->n_patterns > MACSOP_MAX_PATTERNS || m->n_instr > MACSOP_MAX_INSTR)
        return -2;
    if (p + (uint32_t)m->n_instr * 69 > size)
        return -3;
    for (int i = 0; i < m->n_instr; i++, p += 69) {
        uint16_t w[28];
        macsop_instr *in = &m->instr[i];
        for (int j = 0; j < 28; j++) w[j] = rd16(blob + p + 2 * j);
        /* field order of the 69-byte record (AdPlug's loadOffsets) */
        in->op[0].ksl = (uint8_t)w[0];  in->op[0].mult = (uint8_t)w[1];  in->feedback = (uint8_t)w[2];
        in->op[0].ar = (uint8_t)w[3];   in->op[0].sl = (uint8_t)w[4];    in->op[0].eg = (uint8_t)w[5];
        in->op[0].dr = (uint8_t)w[6];   in->op[0].rr = (uint8_t)w[7];    in->op[0].tl = (uint8_t)w[8];
        in->op[0].am = (uint8_t)w[9];   in->op[0].vib = (uint8_t)w[10];  in->op[0].ksr = (uint8_t)w[11];
        in->connection = (uint8_t)w[12];
        in->op[1].ksl = (uint8_t)w[13]; in->op[1].mult = (uint8_t)w[14]; /* w[15] unused */
        in->op[1].ar = (uint8_t)w[16];  in->op[1].sl = (uint8_t)w[17];   in->op[1].eg = (uint8_t)w[18];
        in->op[1].dr = (uint8_t)w[19];  in->op[1].rr = (uint8_t)w[20];   in->op[1].tl = (uint8_t)w[21];
        in->op[1].am = (uint8_t)w[22];  in->op[1].vib = (uint8_t)w[23];  in->op[1].ksr = (uint8_t)w[24];
        /* w[25] unused */
        in->op[0].wave = (uint8_t)w[26]; in->op[1].wave = (uint8_t)w[27];
        for (int j = 0; j < 13; j++) in->name[j] = (char)blob[p + 56 + j];
        in->name[13] = 0;
    }
    for (int i = 0; i < m->n_patterns; i++) {
        m->pat_off[i] = p;
        while (p < size && blob[p] != 0xFF)
            p += 6;
        if (p >= size)
            return -4;
        p++;                                      /* the 0xFF */
    }
    macsop_rewind(m);
    return 0;
}

void macsop_rewind(macsop *m)
{
    static const macsop_instr def = {
        { { 0, 1, 15, 5, 0, 1, 0, 0, 0, 0, 0, 0 }, { 0, 1, 15, 7, 0, 2, 0, 0, 0, 0, 1, 0 } },
        3, 0, "Default"
    };
    wr(m, 0x01, 0x20);                            /* wave select enable */
    m->bd = m->rhythm ? 0x20 : 0;
    wr(m, 0xbd, m->bd);
    for (int i = 0; i < 9; i++) m->bx[i] = 0;
    for (int i = 0; i < MACSOP_CHANNELS; i++) m->cur_instr[i] = 0;
    for (int i = 0; i < MACSOP_CHANNELS; i++) set_instr(m, i, &def);
    reset_player(m);
}

int macsop_tick(macsop *m)
{
    if (m->done)
        return 0;
    for (;;) {
        const uint8_t *e = m->blob + m->ev;
        if (m->ev + 6 > m->size || e[0] == 0xFF || e[0] != m->row)
            break;
        note_event(m, e);
        m->ev += 6;
    }
    if (!advance_row(m)) {
        m->done = 1;
        return 0;
    }
    return 1;
}

uint32_t macsop_rows_per_sec_q16(const macsop *m)
{
    /* 18.2 rows/s at speed 1, halved per step */
    return (uint32_t)(18.2 * 65536.0) >> (m->speed - 1);
}

const char *macsop_instr_name(const macsop *m, int i)
{
    return (i >= 0 && i < m->n_instr) ? m->instr[i].name : "";
}

uint16_t macsop_order_pattern(const macsop *m, uint16_t order_idx)
{
    if (order_idx >= 99u || m->order[order_idx] >= m->n_patterns)
        return 0xFFFF;
    return m->order[order_idx];
}

void macsop_pattern_grid(const macsop *m, uint16_t pattern, uint8_t grid[64][MACSOP_CHANNELS])
{
    uint32_t r, c, p;
    for (r = 0; r < 64u; r++)
        for (c = 0; c < MACSOP_CHANNELS; c++)
            grid[r][c] = 0;
    if (pattern >= m->n_patterns)
        return;
    for (p = m->pat_off[pattern]; p + 6u <= m->size && m->blob[p] != 0xFF; p += 6u) {
        const uint8_t *e = m->blob + p;
        if (e[0] < 64u && e[1] < MACSOP_CHANNELS)
            grid[e[0]][e[1]] = e[2];
    }
}

const char *macsop_channel_instr(const macsop *m, int ch)
{
    if (ch < 0 || ch >= MACSOP_CHANNELS || !m->cur_instr[ch])
        return 0;
    return m->cur_instr[ch]->name;
}

int macsop_channel_instr_no(const macsop *m, int ch)
{
    if (ch < 0 || ch >= MACSOP_CHANNELS || !m->cur_instr[ch])
        return 0;
    if (m->cur_instr[ch] < m->instr || m->cur_instr[ch] >= m->instr + m->n_instr)
        return 0;                             /* the driver's own "Default" patch: not in the song */
    return (int)(m->cur_instr[ch] - m->instr) + 1;
}
