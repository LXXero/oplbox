/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SoundFX Macs Opera (LINEL, Arndt Hasch 1991/92) "CMF" song player, freestanding C99.
 * Playback logic after AdPlug's cmfmcsop.cpp (Sebastian Kienzl, 2017). No heap, no libc beyond
 * stdint: the song stays where it is (flash / XIP) and is read in place. */
#pragma once
#include <stdint.h>

#define MACSOP_MAX_PATTERNS 128
#define MACSOP_MAX_INSTR    64
#define MACSOP_CHANNELS     11

typedef struct {
    uint8_t ksl, mult, ar, sl, eg, dr, rr, tl, am, vib, ksr, wave;
} macsop_slot;

typedef struct {
    macsop_slot op[2];
    uint8_t     feedback, connection;
    char        name[14];
} macsop_instr;

typedef struct {
    const uint8_t *blob;              /* the whole .CMF */
    uint32_t       size;
    uint16_t       order[99];
    uint16_t       n_orders, n_patterns, n_instr;
    uint8_t        speed;             /* 1: 18.2 rows/s, 2: 9.1, 3: 4.55 */
    uint8_t        rhythm;
    uint32_t       pat_off[MACSOP_MAX_PATTERNS];   /* offset of each pattern's first event */
    macsop_instr   instr[MACSOP_MAX_INSTR];

    /* playback state */
    void         (*write)(void *ctx, uint16_t reg, uint8_t val);
    void          *ctx;
    uint16_t       order_idx, row;
    uint32_t       ev;                /* offset of the next event in the current pattern */
    uint8_t        done;
    const macsop_instr *cur_instr[MACSOP_CHANNELS];
    uint8_t        bx[9], bd;
} macsop;

/* 0 on success, else a negative error. `write` receives every OPL register write. */
int  macsop_load(macsop *m, const uint8_t *blob, uint32_t size,
                 void (*write)(void *, uint16_t, uint8_t), void *ctx);
void macsop_rewind(macsop *m);       /* resets the chip (regs 1, BD) and restarts the song */
int  macsop_tick(macsop *m);         /* play one row; 0 when the song has ended */
/* rows per second as a 16.16 fixed-point value (18.2 / 9.1 / 4.55 as AdPlug measured against DOSBox) */
uint32_t macsop_rows_per_sec_q16(const macsop *m);
const char *macsop_instr_name(const macsop *m, int i);
/* for a tracker view: the pattern an order entry plays (0xFFFF: none), and a pattern's notes as a grid
 * (0 = empty, 1 = end of pattern, 4 = key off, 24.. = note) */
uint16_t macsop_order_pattern(const macsop *m, uint16_t order_idx);
void macsop_pattern_grid(const macsop *m, uint16_t pattern, uint8_t grid[64][MACSOP_CHANNELS]);
/* the name of the instrument channel ch (0..10) last played, or 0 */
const char *macsop_channel_instr(const macsop *m, int ch);
