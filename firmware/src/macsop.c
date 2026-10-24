/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SoundFX Macs Opera CMF: song model, lossless file reader / writer, player. Freestanding C99.
 * Playback after AdPlug's cmfmcsop.cpp (Copyright (c) 2017 Sebastian Kienzl); the chip mapping and
 * rhythm-mode quirks are his findings; the arithmetic is FXDRIVER V1.06's own (disassembled, verified
 * against DOSBox captures of the original driver).
 *
 * File: "A.H." NUL, 99 x u16 order (99 = end, zeros after it), u16 patterns, u16 speed (1..3), u16 rhythm
 * (1 = on), u16 instruments, then instruments (28 x u16 + 13-byte name = 69 B) and patterns: 6-byte events
 * (row, col, note, instrument, volume, pitch) sorted by row then column, each pattern ended by a 0xFF byte.
 * note: 1 = end of pattern, 4 = key off, 24 = C-0 .. 119 = B-7. instrument 0 = keep. pitch: bend, 32 = centre. */
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

/* the driver's own patch, in every channel at init until the song sets one */
static const macsop_instr DEFAULT_INSTR = {
    { { 0, 1, 15, 5, 0, 1, 0, 0, 0, 0, 0, 0 }, { 0, 1, 15, 7, 0, 2, 0, 0, 0, 0, 1, 0 } },
    3, 0, 0, 0, "Default"
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* ---- the 69-byte instrument record: word i <-> a field (AdPlug's loadOffsets) */
static uint8_t *instr_word(macsop_instr *in, int i)
{
    switch (i) {
    case 0: return &in->op[0].ksl;  case 1: return &in->op[0].mult; case 2: return &in->feedback;
    case 3: return &in->op[0].ar;   case 4: return &in->op[0].sl;   case 5: return &in->op[0].eg;
    case 6: return &in->op[0].dr;   case 7: return &in->op[0].rr;   case 8: return &in->op[0].tl;
    case 9: return &in->op[0].am;   case 10: return &in->op[0].vib; case 11: return &in->op[0].ksr;
    case 12: return &in->connection;
    case 13: return &in->op[1].ksl; case 14: return &in->op[1].mult;
    case 16: return &in->op[1].ar;  case 17: return &in->op[1].sl;  case 18: return &in->op[1].eg;
    case 19: return &in->op[1].dr;  case 20: return &in->op[1].rr;  case 21: return &in->op[1].tl;
    case 22: return &in->op[1].am;  case 23: return &in->op[1].vib; case 24: return &in->op[1].ksr;
    case 26: return &in->op[0].wave; case 27: return &in->op[1].wave;
    default: return 0;                         /* 15, 25: spare */
    }
}

static void instr_read(macsop_instr *in, const uint8_t *p)
{
    int i;
    for (i = 0; i < 28; i++) {
        uint8_t *f = instr_word(in, i);
        uint16_t w = rd16(p + 2 * i);
        if (f) *f = (uint8_t)w;
        else if (i == 15) in->spare15 = w;
        else in->spare25 = w;
    }
    for (i = 0; i < 13; i++) in->name[i] = (char)p[56 + i];
    in->name[13] = 0;
}

static void instr_write(const macsop_instr *in, uint8_t *p)
{
    int i;
    for (i = 0; i < 28; i++) {
        uint8_t *f = instr_word((macsop_instr *)in, i);
        wr16(p + 2 * i, f ? *f : i == 15 ? in->spare15 : in->spare25);
    }
    for (i = 0; i < 13; i++) p[56 + i] = (uint8_t)in->name[i];
}

/* ---- the song */
void macsop_song_init(macsop_song *s)
{
    uint32_t i, r, c;
    for (i = 0; i < MACSOP_MAX_ORDERS; i++) s->order[i] = 0;
    s->n_orders = 1; s->n_patterns = 1; s->n_instr = 1;
    s->speed = 2; s->rhythm = 0;
    s->instr[0] = DEFAULT_INSTR;
    for (r = 0; r <= MACSOP_ROWS; r++)
        for (c = 0; c < MACSOP_CHANNELS; c++)
            s->pat[0].cell[r][c].u = 0;
}

int macsop_parse(macsop_song *s, const uint8_t *blob, uint32_t size)
{
    uint32_t p = 5, i, r, c;
    if (size < 5 + 198 + 8 || blob[0] != 'A' || blob[1] != '.' || blob[2] != 'H' || blob[3] != '.')
        return -1;
    s->n_orders = MACSOP_MAX_ORDERS;
    for (i = 0; i < MACSOP_MAX_ORDERS; i++, p += 2) {
        s->order[i] = rd16(blob + p);
        if (s->order[i] == 99 && s->n_orders == MACSOP_MAX_ORDERS)
            s->n_orders = (uint16_t)i;
    }
    s->n_patterns = rd16(blob + p); p += 2;
    s->speed = (uint8_t)rd16(blob + p); p += 2;
    s->rhythm = rd16(blob + p) == 1; p += 2;
    s->n_instr = rd16(blob + p); p += 2;
    if (s->speed < 1 || s->speed > 3 || s->n_patterns > MACSOP_MAX_PATTERNS || s->n_instr > MACSOP_MAX_INSTR)
        return -2;
    if (p + (uint32_t)s->n_instr * 69u > size)
        return -3;
    for (i = 0; i < s->n_instr; i++, p += 69)
        instr_read(&s->instr[i], blob + p);
    for (i = 0; i < s->n_patterns; i++) {
        macsop_pattern *pat = &s->pat[i];
        for (r = 0; r <= MACSOP_ROWS; r++)
            for (c = 0; c < MACSOP_CHANNELS; c++)
                pat->cell[r][c].u = 0;
        while (p < size && blob[p] != 0xFF) {
            const uint8_t *e = blob + p;
            if (p + 6 > size)
                return -4;
            if (e[0] > MACSOP_ROWS || e[1] >= MACSOP_CHANNELS || e[2] == 0)
                return -5;                      /* not something the grid can hold */
            pat->cell[e[0]][e[1]].f.note = e[2];
            pat->cell[e[0]][e[1]].f.instr = e[3];
            pat->cell[e[0]][e[1]].f.vol = e[4];
            pat->cell[e[0]][e[1]].f.pitch = e[5];
            p += 6;
        }
        if (p >= size)
            return -4;
        p++;                                      /* the 0xFF */
    }
    return 0;
}

uint32_t macsop_size(const macsop_song *s)
{
    uint32_t n = 5 + 198 + 8 + 69u * s->n_instr, i, r, c;
    for (i = 0; i < s->n_patterns; i++) {
        n++;
        for (r = 0; r <= MACSOP_ROWS; r++)
            for (c = 0; c < MACSOP_CHANNELS; c++)
                if (s->pat[i].cell[r][c].f.note) n += 6;
    }
    return n;
}

uint32_t macsop_write(const macsop_song *s, uint8_t *out, uint32_t cap)
{
    uint32_t p = 0, i, r, c;
    if (macsop_size(s) > cap)
        return 0;
    out[p++] = 'A'; out[p++] = '.'; out[p++] = 'H'; out[p++] = '.'; out[p++] = 0;
    for (i = 0; i < MACSOP_MAX_ORDERS; i++, p += 2)
        wr16(out + p, i < s->n_orders ? s->order[i] : i == s->n_orders ? 99 : 0);
    wr16(out + p, s->n_patterns); p += 2;
    wr16(out + p, s->speed); p += 2;
    wr16(out + p, s->rhythm ? 1 : 0); p += 2;
    wr16(out + p, s->n_instr); p += 2;
    for (i = 0; i < s->n_instr; i++, p += 69)
        instr_write(&s->instr[i], out + p);
    for (i = 0; i < s->n_patterns; i++) {
        for (r = 0; r <= MACSOP_ROWS; r++)
            for (c = 0; c < MACSOP_CHANNELS; c++) {
                const macsop_cell *k = &s->pat[i].cell[r][c];
                if (!k->f.note) continue;
                out[p++] = (uint8_t)r; out[p++] = (uint8_t)c;
                out[p++] = k->f.note; out[p++] = k->f.instr; out[p++] = k->f.vol; out[p++] = k->f.pitch;
            }
        out[p++] = 0xFF;
    }
    return p;
}

/* ---- the chip side */
static int valid_ch(const macsop *m, int ch)
{
    return ch >= 0 && (m->song->rhythm ? ch < MACSOP_CHANNELS : ch < 9);
}
static int rhythm_ch(const macsop *m, int ch) { return m->song->rhythm && ch >= 6; }

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

static void note_event(macsop *m, int ch, const macsop_cell *k)
{
    int note = k->f.note, in = k->f.instr, vol = k->f.vol, pitch = k->f.pitch;
    if (!valid_ch(m, ch))
        return;
    key_off(m, ch);
    if (note == 4)
        return;
    if (in >= 1 && in <= m->song->n_instr)
        set_instr(m, ch, &m->song->instr[in - 1]);
    set_volume(m, ch, vol);
    if (set_note(m, ch, note, pitch))
        key_on(m, ch);
}

void macsop_play_note(macsop *m, int ch, int note, int instr, int vol)
{
    macsop_cell k;
    if (!valid_ch(m, ch))
        return;
    if (note == 0) { key_off(m, ch); return; }
    k.f.note = (uint8_t)note; k.f.instr = (uint8_t)instr; k.f.vol = (uint8_t)vol; k.f.pitch = 32;
    note_event(m, ch, &k);
}

void macsop_refresh_instr(macsop *m, int i)
{
    int ch;
    const macsop_instr *in;
    if (i < 0 || i >= m->song->n_instr)
        return;
    in = &m->song->instr[i];
    for (ch = 0; ch < MACSOP_CHANNELS; ch++)
        if (m->cur_instr[ch] == in) {
            m->cur_instr[ch] = 0;             /* so set_instr sends it again */
            set_instr(m, ch, in);
            set_volume(m, ch, 127);           /* the volume column is not known here: full, as an audition */
        }
}

/* the first cell of a row (lowest column) says "end of pattern": AdPlug's reading of the driver */
static int row_is_break(const macsop_pattern *p, int row)
{
    int c;
    for (c = 0; c < MACSOP_CHANNELS; c++)
        if (p->cell[row][c].f.note)
            return p->cell[row][c].f.note == 1;
    return 0;
}

/* -> 1 while the song goes on, 0 at its end */
static int advance_row(macsop *m)
{
    const macsop_song *s = m->song;
    for (;;) {
        if (++m->row >= MACSOP_ROWS) {
            m->row = 0;
            do {
                m->order_idx++;
                if (m->order_idx >= MACSOP_MAX_ORDERS || s->order[m->order_idx] == 99)
                    return 0;
            } while (s->order[m->order_idx] >= s->n_patterns);
        }
        if (row_is_break(&s->pat[s->order[m->order_idx]], m->row))
            m->row = MACSOP_ROWS;
        else
            return 1;
    }
}

static void reset_player(macsop *m)
{
    m->row = MACSOP_ROWS;
    m->order_idx = 0xFFFF;
    m->done = !advance_row(m);
}

void macsop_attach(macsop *m, macsop_song *s, void (*write)(void *, uint16_t, uint8_t), void *ctx)
{
    m->song = s; m->write = write; m->ctx = ctx;
    macsop_rewind(m);
}

int macsop_load(macsop *m, macsop_song *s, const uint8_t *blob, uint32_t size,
                void (*write)(void *, uint16_t, uint8_t), void *ctx)
{
    int r = macsop_parse(s, blob, size);
    if (r)
        return r;
    macsop_attach(m, s, write, ctx);
    return 0;
}

void macsop_rewind(macsop *m)
{
    int i;
    wr(m, 0x01, 0x20);                            /* wave select enable */
    m->bd = m->song->rhythm ? 0x20 : 0;
    wr(m, 0xbd, m->bd);
    for (i = 0; i < 9; i++) m->bx[i] = 0;
    for (i = 0; i < MACSOP_CHANNELS; i++) m->cur_instr[i] = 0;
    for (i = 0; i < MACSOP_CHANNELS; i++) set_instr(m, i, &DEFAULT_INSTR);
    reset_player(m);
}

int macsop_seek(macsop *m, uint16_t order_idx, uint16_t row)
{
    const macsop_song *s = m->song;
    int i;
    if (order_idx >= s->n_orders || s->order[order_idx] >= s->n_patterns || row >= MACSOP_ROWS)
        return 0;
    for (i = 0; i < MACSOP_CHANNELS; i++)         /* every voice off: what sounded before is not this row's */
        if (valid_ch(m, i)) key_off(m, i);
    m->order_idx = order_idx;
    m->row = row;
    m->done = 0;
    return 1;
}

int macsop_tick(macsop *m)
{
    const macsop_song *s = m->song;
    const macsop_pattern *p;
    int c;
    if (m->done)
        return 0;
    p = &s->pat[s->order[m->order_idx]];
    for (c = 0; c < MACSOP_CHANNELS; c++)
        if (p->cell[m->row][c].f.note && !((m->skip >> c) & 1u))
            note_event(m, c, &p->cell[m->row][c]);
    if (!advance_row(m)) {
        m->done = 1;
        return 0;
    }
    return 1;
}

uint32_t macsop_rows_per_sec_q16(const macsop *m)
{
    /* 18.2 rows/s at speed 1, halved per step */
    return (uint32_t)(18.2 * 65536.0) >> (m->song->speed - 1);
}

const char *macsop_instr_name(const macsop *m, int i)
{
    return (i >= 0 && i < m->song->n_instr) ? m->song->instr[i].name : "";
}

uint16_t macsop_order_pattern(const macsop *m, uint16_t order_idx)
{
    if (order_idx >= m->song->n_orders || m->song->order[order_idx] >= m->song->n_patterns)
        return 0xFFFF;
    return m->song->order[order_idx];
}

const char *macsop_channel_instr(const macsop *m, int ch)
{
    if (ch < 0 || ch >= MACSOP_CHANNELS || !m->cur_instr[ch])
        return 0;
    return m->cur_instr[ch]->name;
}

int macsop_channel_instr_no(const macsop *m, int ch)
{
    const macsop_instr *in;
    if (ch < 0 || ch >= MACSOP_CHANNELS || !(in = m->cur_instr[ch]))
        return 0;
    if (in < m->song->instr || in >= m->song->instr + m->song->n_instr)
        return 0;                             /* the driver's own "Default" patch: not in the song */
    return (int)(in - m->song->instr) + 1;
}
