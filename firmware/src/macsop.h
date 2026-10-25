/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SoundFX Macs Opera (LINEL, Arndt Hasch 1991/92) "CMF" songs: an editable in-memory model, a lossless
 * reader / writer for the file format, and a player that drives an OPL2 the way FXDRIVER V1.06 did.
 * Freestanding C99: no heap, no libc beyond stdint. Playback logic after AdPlug's cmfmcsop.cpp
 * (Sebastian Kienzl, 2017). */
#pragma once
#include <stdint.h>

#define MACSOP_MAX_PATTERNS 64         /* the file allows more; the biggest known song has 24 */
#define MACSOP_MAX_INSTR    64
#define MACSOP_CHANNELS     11         /* 9 melodic, or 6 + BD SD TOM TC HH in rhythm mode */
#define MACSOP_ROWS         64
#define MACSOP_MAX_ORDERS   99

/* one operator, the file's own value ranges (ksl 0..3, mult 0..15, ar/dr/sl/rr 0..15, tl 0..63, flags 0/1, wave 0..3) */
typedef struct { uint8_t ksl, mult, ar, sl, eg, dr, rr, tl, am, vib, ksr, wave; } macsop_slot;
typedef struct {
    macsop_slot op[2];
    uint8_t  feedback, connection;     /* 0..7; 1 = FM (op 1 modulates op 2), 0 = additive: C0 bit 0 is 1 - connection */
    uint16_t spare15, spare25;         /* words 15 and 25 of the record: unused, kept as read */
    char     name[14];                 /* 13 bytes as in the file (NUL padded), then a NUL */
} macsop_instr;

/* one cell of a pattern: note 0 = empty, 1 = end of pattern (the pattern stops before this row), 4 = key off,
 * 24 = C-0 .. 119 = B-7; other values below 24 appear in some songs and are kept as they are. instr 1..n
 * (0 = keep the channel's), vol 0..127, pitch 0..255 bend (32 = centre). A union so a cell is written whole. */
typedef union { struct { uint8_t note, instr, vol, pitch; } f; uint32_t u; } macsop_cell;

typedef struct {
    macsop_cell cell[MACSOP_ROWS + 1][MACSOP_CHANNELS];   /* row 64 only ever holds end-of-pattern marks */
} macsop_pattern;

/* a song: exactly what a .CMF holds (macsop_parse / macsop_write are lossless, byte for byte, on every known file) */
typedef struct {
    uint16_t order[MACSOP_MAX_ORDERS];
    uint16_t n_orders;                 /* entries before the 99 end mark */
    uint16_t n_patterns, n_instr;
    uint8_t  speed, rhythm;            /* 1..3 (18.2 / 9.1 / 4.55 rows per second); rhythm mode 0/1 */
    macsop_instr   instr[MACSOP_MAX_INSTR];
    macsop_pattern pat[MACSOP_MAX_PATTERNS];
} macsop_song;

/* the player: register writes go to write(ctx, reg, val) */
typedef struct {
    macsop_song *song;
    void (*write)(void *ctx, uint16_t reg, uint8_t val);
    void *ctx;
    uint16_t order_idx, row;
    uint8_t  done;
    const macsop_instr *cur_instr[MACSOP_CHANNELS];
    uint8_t  bx[9], bd;
    uint16_t skip;                     /* bit n: column n's cells are not played (OPLBOX's arpeggiator has the channel) */
} macsop;

/* ---- the song */
int      macsop_parse(macsop_song *s, const uint8_t *blob, uint32_t size);   /* 0, or < 0: not a CMF / too big */
int      macsop_check(const uint8_t *blob, uint32_t size);                   /* macsop_parse's answer, no song needed */
uint32_t macsop_write(const macsop_song *s, uint8_t *out, uint32_t cap);     /* bytes written, 0 if cap is too small */
uint32_t macsop_size(const macsop_song *s);                                  /* bytes macsop_write needs */
void     macsop_song_init(macsop_song *s);                                   /* an empty song: one pattern, one order */

/* ---- the player */
void     macsop_attach(macsop *m, macsop_song *s, void (*write)(void *, uint16_t, uint8_t), void *ctx);
void     macsop_rewind(macsop *m);                                           /* the driver's init writes, then row 0 */
int      macsop_seek(macsop *m, uint16_t order_idx, uint16_t row);           /* play on from here; 0 if out of range */
int      macsop_tick(macsop *m);                                             /* one row; 0 when the song has ended */
uint32_t macsop_rows_per_sec_q16(const macsop *m);
/* convenience: parse into s and attach */
int      macsop_load(macsop *m, macsop_song *s, const uint8_t *blob, uint32_t size,
                     void (*write)(void *, uint16_t, uint8_t), void *ctx);
/* key a note on a channel by hand (the editor's audition): instr 1..n, vol 0..127; note 0 = off */
void     macsop_play_note(macsop *m, int ch, int note, int instr, int vol);
/* instrument i (0..) was edited: channels playing it get the new registers */
void     macsop_refresh_instr(macsop *m, int i);

/* ---- lookups for the screen */
const char *macsop_instr_name(const macsop *m, int i);
uint16_t    macsop_order_pattern(const macsop *m, uint16_t order_idx);        /* 0xFFFF: none */
const char *macsop_channel_instr(const macsop *m, int ch);                    /* the instrument channel ch plays, or 0 */
int         macsop_channel_instr_no(const macsop *m, int ch);                 /* its number 1..n, 0 = the driver's default */
