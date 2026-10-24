/* SPDX-License-Identifier: GPL-2.0-or-later */
/* dbopl2: one OPL2 after DOSBox's dbopl (see dbopl2.c). The chip, its three entry points, and the
 * oscilloscope taps the generator fills when it is built with DBOPL2_SCOPE_N. */
#ifndef DBOPL2_H
#define DBOPL2_H
#include <stdint.h>

typedef struct {
    const int16_t *waveBase;                 /* waveform: a window of WaveTable */
    uint32_t waveMask, waveStart;
    uint32_t waveIndex, waveAdd, waveCurrent;
    uint32_t chanData, freqMul, vibrato;
    int32_t sustainLevel, totalLevel;
    uint32_t currentLevel;
    int32_t volume;
    uint32_t attackAdd, decayAdd, releaseAdd, rateIndex;
    uint8_t rateZero, keyOn, reg20, reg40, reg60, reg80, regE0, state, tremoloMask, vibStrength, ksr;
} dbop;

typedef struct {
    dbop op[2];
    uint32_t chanData;
    int32_t old[2];
    uint8_t mode, feedback, regB0, regC0, perc;   /* perc: channels 6-8, whose C0 is frozen in rhythm mode */
} dbchan;

typedef struct {
    uint32_t lfoCounter, noiseCounter, noiseValue;
    dbchan chan[9];
    uint8_t reg08, regBD, vibratoIndex, tremoloIndex;
    int8_t vibratoSign;
    uint8_t vibratoShift, tremoloValue, vibratoStrength, tremoloStrength, waveFormMask;
    uint32_t mute;                           /* bit n: channel n left out of the mix (6, 7, 8 in rhythm mode:
                                              * bass drum, snare, hi-hat + tom + cymbal); the chip runs as before */
} dbopl2;

void dbopl2_reset(dbopl2 *chip);
void dbopl2_write(dbopl2 *chip, uint32_t reg, uint8_t val);
void dbopl2_generate(dbopl2 *chip, uint32_t total, int32_t *output);   /* adds total samples to output[] */

#ifdef DBOPL2_SCOPE_N                        /* a power of two: each channel's last DBOPL2_SCOPE_N samples */
extern volatile uint32_t dbopl2_scope_on;    /* taps run only while this is set (the scope screen is up) */
extern int16_t dbopl2_scope[9][DBOPL2_SCOPE_N];
extern uint32_t dbopl2_scope_w;              /* write index: sample i of the current block goes to w + i */
#endif
#endif
