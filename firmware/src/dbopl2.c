/* SPDX-License-Identifier: GPL-2.0-or-later */
/* dbopl2: DOSBox's OPL emulator (dbopl, DOSBox r3635 as carried by Chocolate Doom 2.2.1,
 * Copyright (C) 2002-2010 The DOSBox Team) reduced to one OPL2 for the FM-1: 9 channels,
 * 18 operators, no OPL3 modes, integers only. Every table the original computes with floating
 * point is generated for 49716 Hz by host/dbopl_gen.c (dbopl2_tables.h). The arithmetic is
 * dbopl's, sample for sample: host/dbcmp.c checks the output against the unmodified original.
 *
 * Costs about a third of Nuked-OPL3 per sample: envelopes advance by 24-bit accumulators
 * instead of the chip's rate tables, the waveform is a linear table times a volume table, and
 * a block of samples runs operator by operator without the chip's 36-slot pipeline. Not
 * cycle-exact, but it is the sound of DOSBox, which is where most people heard these songs. */
#include <stdint.h>
#include "dbopl2.h"

/* Its own object in the firmware (tools/build.py): this file is compiled at -O2, the app and the platform at
 * -Os. The USB stack misbehaves at -O2 (USB-MIDI IN babbles on the device, dinghy tools/build.py), so the two
 * are not built together any more (0.4.2). DBOPL2_ON_DEVICE puts the tables and the sample loop in RAM. */
#ifdef DBOPL2_ON_DEVICE
#define DBOPL2_RAMDATA __attribute__((section(".data.dbopl2")))      /* wave and volume tables in RAM, not flash */
#define DBOPL2_RAMCODE __attribute__((section(".ram_code.dbopl2")))  /* the sample loop in RAM (app.ld, build.py) */
#else
#define DBOPL2_RAMDATA
#define DBOPL2_RAMCODE
#endif
#include "dbopl2_tables.h"

/* taps for an oscilloscope view: a channel's own samples (index 0..8; the rhythm block taps 6 = bass drum,
 * 7 = snare, 8 = hi-hat + tom + cymbal) before they are summed, kept here with the generator
 * (DBOPL2_SCOPE_N samples per channel, dbopl2.h). No-ops without DBOPL2_SCOPE_N. */
#ifdef DBOPL2_SCOPE_N
volatile uint32_t dbopl2_scope_on;
int16_t dbopl2_scope[9][DBOPL2_SCOPE_N];
uint32_t dbopl2_scope_w;
#define DBOPL2_TAP(ch, i, s) do { if (dbopl2_scope_on) dbopl2_scope[ch][(dbopl2_scope_w + (i)) & (DBOPL2_SCOPE_N - 1u)] = (int16_t)(s); } while (0)
#define DBOPL2_TAP_ZERO(ch, n) do { if (dbopl2_scope_on) { uint32_t k_; for (k_ = 0; k_ < (n); k_++) dbopl2_scope[ch][(dbopl2_scope_w + k_) & (DBOPL2_SCOPE_N - 1u)] = 0; } } while (0)
#define DBOPL2_TAP_ADVANCE(n) (dbopl2_scope_w += (n))
#else
#define DBOPL2_TAP(ch, i, s) ((void)0)       /* sample i of this block, channel ch */
#define DBOPL2_TAP_ZERO(ch, n) ((void)0)     /* the channel was skipped as silent for n samples */
#define DBOPL2_TAP_ADVANCE(n) ((void)0)      /* a block of n samples is done */
#endif

#define WAVE_BITS   10
#define WAVE_SH     (32 - WAVE_BITS)         /* 10.22 wave index */
#define WAVE_MASK   ((1u << WAVE_SH) - 1u)
#define LFO_SH      (WAVE_SH - 10)
#define LFO_MAX     (256u << LFO_SH)
#define ENV_MIN     0
#define ENV_MAX     511
#define ENV_LIMIT   ((12 * 256) >> 3)        /* 384: silent from here */
#define ENV_SILENT(x) ((x) >= ENV_LIMIT)
#define RATE_SH     24
#define RATE_MASK   ((1u << RATE_SH) - 1u)
#define MUL_SH      16
#define TREMOLO_TABLE 52
#define SHIFT_KSLBASE 16
#define SHIFT_KEYCODE 24

enum { ST_OFF, ST_RELEASE, ST_SUSTAIN, ST_DECAY, ST_ATTACK };
enum { MASK_KSR = 0x10, MASK_SUSTAIN = 0x20, MASK_VIBRATO = 0x40, MASK_TREMOLO = 0x80 };
enum { SM_FM, SM_AM, SM_PERC };

static const uint16_t WaveBaseTable[8]  = { 0, 512, 512, 2048, 2560, 3072, 256, 1024 };
static const uint16_t WaveMaskTable[8]  = { 1023, 1023, 511, 511, 1023, 1023, 512, 1023 };
static const uint16_t WaveStartTable[8] = { 512, 0, 0, 0, 0, 512, 512, 256 };
static const int8_t VibratoTable[8] = { 1 - 0x00, 0 - 0x00, 1 - 0x00, 30 - 0x00, 1 - 0x80, 0 - 0x80, 1 - 0x80, 30 - 0x80 };
static const uint8_t KslShiftTable[4] = { 31, 1, 2, 0 };
/* register slot (reg & 0x1f) -> byte offset of the operator in the chip, 0 = none. Filled at reset, as dbopl
 * does: a const table here would be folded into a bit mask of the invalid slots, 0xffc0c0c0, which the
 * build's ROM-address lint takes for a reference into the chip's boot ROM. */
static uint16_t OpOffset[32];

#define OP(ch, i) (&((ch) + ((i) >> 1))->op[(i) & 1])   /* operator i of a channel, i >= 2 in the next ones */

/* ---- operator: register-derived state (write time) */
static inline __attribute__((always_inline)) void op_set_state(dbop *o, uint8_t s) { o->state = s; }   /* inlined everywhere: RAM code may not call flash */

static void op_update_attack(dbop *o)
{
    uint8_t rate = o->reg60 >> 4;
    if (rate) { o->attackAdd = AttackRates[(rate << 2) + o->ksr]; o->rateZero &= (uint8_t)~(1u << ST_ATTACK); }
    else { o->attackAdd = 0; o->rateZero |= 1u << ST_ATTACK; }
}
static void op_update_decay(dbop *o)
{
    uint8_t rate = o->reg60 & 0xf;
    if (rate) { o->decayAdd = LinearRates[(rate << 2) + o->ksr]; o->rateZero &= (uint8_t)~(1u << ST_DECAY); }
    else { o->decayAdd = 0; o->rateZero |= 1u << ST_DECAY; }
}
static void op_update_release(dbop *o)
{
    uint8_t rate = o->reg80 & 0xf;
    if (rate) {
        o->releaseAdd = LinearRates[(rate << 2) + o->ksr];
        o->rateZero &= (uint8_t)~(1u << ST_RELEASE);
        if (!(o->reg20 & MASK_SUSTAIN)) o->rateZero &= (uint8_t)~(1u << ST_SUSTAIN);
    } else {
        o->rateZero |= 1u << ST_RELEASE;
        o->releaseAdd = 0;
        if (!(o->reg20 & MASK_SUSTAIN)) o->rateZero |= 1u << ST_SUSTAIN;
    }
}
static void op_update_attenuation(dbop *o)
{
    uint8_t kslBase = (uint8_t)((o->chanData >> SHIFT_KSLBASE) & 0xff);
    uint32_t tl = o->reg40 & 0x3f;
    uint8_t kslShift = KslShiftTable[o->reg40 >> 6];
    o->totalLevel = (int32_t)(tl << 2);
    o->totalLevel += (int32_t)((uint32_t)kslBase >> kslShift);
}
static void op_update_frequency(dbop *o)
{
    uint32_t freq = o->chanData & ((1u << 10) - 1u);
    uint32_t block = (o->chanData >> 10) & 0xff;
    o->waveAdd = (freq << block) * o->freqMul;
    if (o->reg20 & MASK_VIBRATO) {
        o->vibStrength = (uint8_t)(freq >> 7);
        o->vibrato = ((uint32_t)o->vibStrength << block) * o->freqMul;
    } else {
        o->vibStrength = 0;
        o->vibrato = 0;
    }
}
static void op_update_rates(dbop *o)
{
    uint8_t newKsr = (uint8_t)((o->chanData >> SHIFT_KEYCODE) & 0xff);
    if (!(o->reg20 & MASK_KSR)) newKsr >>= 2;
    if (o->ksr == newKsr) return;
    o->ksr = newKsr;
    op_update_attack(o); op_update_decay(o); op_update_release(o);
}
static void op_key_on(dbop *o, uint8_t mask)
{
    if (!o->keyOn) {
        o->waveIndex = o->waveStart;
        o->rateIndex = 0;
        op_set_state(o, ST_ATTACK);
    }
    o->keyOn |= mask;
}
static void op_key_off(dbop *o, uint8_t mask)
{
    o->keyOn &= (uint8_t)~mask;
    if (!o->keyOn && o->state != ST_OFF) op_set_state(o, ST_RELEASE);
}
static void op_write20(dbop *o, uint8_t val)
{
    uint8_t change = o->reg20 ^ val;
    if (!change) return;
    o->reg20 = val;
    o->tremoloMask = (uint8_t)((int8_t)val >> 7);
    if (change & MASK_KSR) op_update_rates(o);
    if ((o->reg20 & MASK_SUSTAIN) || !o->releaseAdd) o->rateZero |= 1u << ST_SUSTAIN;
    else o->rateZero &= (uint8_t)~(1u << ST_SUSTAIN);
    if (change & (0xf | MASK_VIBRATO)) {
        o->freqMul = FreqMul[val & 0xf];
        op_update_frequency(o);
    }
}
static void op_write40(dbop *o, uint8_t val)
{
    if (!(o->reg40 ^ val)) return;
    o->reg40 = val;
    op_update_attenuation(o);
}
static void op_write60(dbop *o, uint8_t val)
{
    uint8_t change = o->reg60 ^ val;
    o->reg60 = val;
    if (change & 0x0f) op_update_decay(o);
    if (change & 0xf0) op_update_attack(o);
}
static void op_write80(dbop *o, uint8_t val)
{
    uint8_t change = o->reg80 ^ val, sustain;
    if (!change) return;
    o->reg80 = val;
    sustain = val >> 4;
    sustain |= (sustain + 1) & 0x10;         /* 0xf is 0x1f */
    o->sustainLevel = (int32_t)sustain << 4;
    if (change & 0x0f) op_update_release(o);
}
static void op_writeE0(dbop *o, const dbopl2 *chip, uint8_t val)
{
    uint8_t waveForm;
    if (!(o->regE0 ^ val)) return;
    waveForm = val & (0x3 & chip->waveFormMask);
    o->regE0 = val;
    o->waveBase = WaveTable + WaveBaseTable[waveForm];
    o->waveStart = (uint32_t)WaveStartTable[waveForm] << WAVE_SH;
    o->waveMask = WaveMaskTable[waveForm];
}
static void op_init(dbop *o)
{
    o->chanData = 0; o->freqMul = 0; o->waveIndex = 0; o->waveAdd = 0; o->waveCurrent = 0;
    o->keyOn = 0; o->ksr = 0; o->reg20 = o->reg40 = o->reg60 = o->reg80 = o->regE0 = 0;
    op_set_state(o, ST_OFF);
    o->rateZero = 1u << ST_OFF;
    o->sustainLevel = ENV_MAX; o->currentLevel = ENV_MAX; o->totalLevel = ENV_MAX; o->volume = ENV_MAX;
    o->releaseAdd = 0;
    o->waveBase = WaveTable; o->waveMask = 1023; o->waveStart = 512u << WAVE_SH;
    o->attackAdd = o->decayAdd = 0; o->rateIndex = 0; o->vibrato = 0; o->vibStrength = 0; o->tremoloMask = 0;
}

/* ---- channel: register-derived state (write time) */
static void ch_set_chandata(dbchan *c, uint32_t data)
{
    uint32_t change = c->chanData ^ data;
    c->chanData = data;
    c->op[0].chanData = data; c->op[1].chanData = data;
    op_update_frequency(&c->op[0]); op_update_frequency(&c->op[1]);
    if (change & (0xffu << SHIFT_KSLBASE)) { op_update_attenuation(&c->op[0]); op_update_attenuation(&c->op[1]); }
    if (change & (0xffu << SHIFT_KEYCODE)) { op_update_rates(&c->op[0]); op_update_rates(&c->op[1]); }
}
static void ch_update_frequency(dbchan *c, const dbopl2 *chip)
{
    uint32_t data = c->chanData & 0xffff;
    uint32_t kslBase = KslTable[data >> 6];
    uint32_t keyCode = (data & 0x1c00) >> 9;
    if (chip->reg08 & 0x40) keyCode |= (data & 0x100) >> 8;     /* notesel 1 */
    else keyCode |= (data & 0x200) >> 9;                        /* notesel 0 */
    data |= (keyCode << SHIFT_KEYCODE) | (kslBase << SHIFT_KSLBASE);
    ch_set_chandata(c, data);
}
static void ch_writeA0(dbchan *c, const dbopl2 *chip, uint8_t val)
{
    uint32_t change = (c->chanData ^ val) & 0xff;
    if (change) { c->chanData ^= change; ch_update_frequency(c, chip); }
}
static void ch_writeB0(dbchan *c, const dbopl2 *chip, uint8_t val)
{
    uint32_t change = (c->chanData ^ ((uint32_t)val << 8)) & 0x1f00;
    if (change) { c->chanData ^= change; ch_update_frequency(c, chip); }
    if (!((val ^ c->regB0) & 0x20)) return;
    c->regB0 = val;
    if (val & 0x20) { op_key_on(&c->op[0], 1); op_key_on(&c->op[1], 1); }
    else { op_key_off(&c->op[0], 1); op_key_off(&c->op[1], 1); }
}
static void ch_writeC0(dbchan *c, const dbopl2 *chip, uint8_t val)
{
    uint8_t change = val ^ c->regC0;
    if (!change) return;
    c->regC0 = val;
    c->feedback = (val >> 1) & 7;
    c->feedback = c->feedback ? (uint8_t)(9 - c->feedback) : 31;   /* shift to the 10-bit wave index */
    if (c->perc && (chip->regBD & 0x20)) { /* a rhythm channel while rhythm is on: keep the rhythm synth */ }
    else c->mode = (val & 1) ? SM_AM : SM_FM;
}
static void ch_resetC0(dbchan *c, const dbopl2 *chip)
{
    uint8_t val = c->regC0;
    c->regC0 ^= 0xff;
    ch_writeC0(c, chip, val);
}
static void ch_init(dbchan *c, int perc)
{
    op_init(&c->op[0]); op_init(&c->op[1]);
    c->old[0] = c->old[1] = 0; c->chanData = 0; c->regB0 = c->regC0 = 0;
    c->feedback = 31; c->mode = SM_FM; c->perc = (uint8_t)perc;
}

/* ---- chip: writes */
static void chip_writeBD(dbopl2 *chip, uint8_t val)
{
    uint8_t change = chip->regBD ^ val;
    if (!change) return;
    chip->regBD = val;
    chip->vibratoStrength = (val & 0x40) ? 0 : 1;
    chip->tremoloStrength = (val & 0x80) ? 0 : 2;
    if (val & 0x20) {
        if (change & 0x20) chip->chan[6].mode = SM_PERC;
        if (val & 0x10) { op_key_on(&chip->chan[6].op[0], 2); op_key_on(&chip->chan[6].op[1], 2); }
        else { op_key_off(&chip->chan[6].op[0], 2); op_key_off(&chip->chan[6].op[1], 2); }
        if (val & 0x1) op_key_on(&chip->chan[7].op[0], 2); else op_key_off(&chip->chan[7].op[0], 2);   /* hi-hat */
        if (val & 0x8) op_key_on(&chip->chan[7].op[1], 2); else op_key_off(&chip->chan[7].op[1], 2);   /* snare */
        if (val & 0x4) op_key_on(&chip->chan[8].op[0], 2); else op_key_off(&chip->chan[8].op[0], 2);   /* tom */
        if (val & 0x2) op_key_on(&chip->chan[8].op[1], 2); else op_key_off(&chip->chan[8].op[1], 2);   /* cymbal */
    } else if (change & 0x20) {
        ch_resetC0(&chip->chan[6], chip);
        op_key_off(&chip->chan[6].op[0], 2); op_key_off(&chip->chan[6].op[1], 2);
        op_key_off(&chip->chan[7].op[0], 2); op_key_off(&chip->chan[7].op[1], 2);
        op_key_off(&chip->chan[8].op[0], 2); op_key_off(&chip->chan[8].op[1], 2);
    }
}

void dbopl2_write(dbopl2 *chip, uint32_t reg, uint8_t val)
{
    uint32_t hi = reg & 0xf0, k;
    dbop *o;
    if (hi == 0x00) {
        if (reg == 0x01) chip->waveFormMask = (val & 0x20) ? 0x7 : 0x0;
        else if (reg == 0x08) chip->reg08 = val;
    } else if ((hi >= 0x20 && hi < 0xa0) || hi >= 0xe0) {   /* operator registers */
        k = OpOffset[reg & 0x1f];
        if (!k) return;
        o = (dbop *)((char *)chip + k);
        if (hi < 0x40) op_write20(o, val);
        else if (hi < 0x60) op_write40(o, val);
        else if (hi < 0x80) op_write60(o, val);
        else if (hi < 0xa0) op_write80(o, val);
        else op_writeE0(o, chip, val);
    } else if (hi == 0xa0) {
        if ((reg & 0xf) < 9) ch_writeA0(&chip->chan[reg & 0xf], chip, val);
    } else if (hi == 0xb0) {
        if (reg == 0xbd) chip_writeBD(chip, val);
        else if ((reg & 0xf) < 9) ch_writeB0(&chip->chan[reg & 0xf], chip, val);
    } else if (hi == 0xc0) {
        if ((reg & 0xf) < 9) ch_writeC0(&chip->chan[reg & 0xf], chip, val);
    }
}

void dbopl2_reset(dbopl2 *chip)
{
    static const uint8_t LO_CH[6] = { 0, 1, 2, 0, 1, 2 }, LO_OP[6] = { 0, 0, 0, 1, 1, 1 };
    uint32_t i;
    for (i = 0; i < 32u; i++) {              /* slot r: channel (r / 8) * 3 + r % 8 % 3, operator r % 8 / 3 */
        uint32_t lo = i & 7u;
        OpOffset[i] = (lo < 6u && i < 24u)
            ? (uint16_t)((char *)&chip->chan[(i >> 3) * 3u + LO_CH[lo]].op[LO_OP[lo]] - (char *)chip) : 0;
    }
    for (i = 0; i < 9u; i++) ch_init(&chip->chan[i], i >= 6u);
    chip->lfoCounter = 0; chip->noiseCounter = 0; chip->noiseValue = 1;
    chip->vibratoIndex = chip->tremoloIndex = 0;
    chip->reg08 = chip->regBD = 0; chip->waveFormMask = 0;
    chip->vibratoSign = 0; chip->vibratoShift = 0; chip->tremoloValue = 0;
    chip->vibratoStrength = 0; chip->tremoloStrength = 0;
    chip->mute = 0;
    for (i = 0; i < 255u; i++) {             /* as dbopl's Setup: every register 0xff then 0 */
        dbopl2_write(chip, i, 0xff);
        dbopl2_write(chip, i, 0);
    }
}

/* ---- the sample loop */
static DBOPL2_RAMCODE int op_silent(const dbop *o)
{
    if (!ENV_SILENT(o->totalLevel + o->volume)) return 0;
    if (!(o->rateZero & (1u << o->state))) return 0;
    return 1;
}
static DBOPL2_RAMCODE void op_prepare(dbop *o, const dbopl2 *chip)
{
    o->currentLevel = (uint32_t)o->totalLevel + (chip->tremoloValue & o->tremoloMask);
    o->waveCurrent = o->waveAdd;
    if (o->vibStrength >> chip->vibratoShift) {
        int32_t add = (int32_t)(o->vibrato >> chip->vibratoShift);
        int32_t neg = chip->vibratoSign;
        add = (add ^ neg) - neg;
        o->waveCurrent += (uint32_t)add;
    }
}
static DBOPL2_RAMCODE __attribute__((always_inline)) inline int32_t op_rate_forward(dbop *o, uint32_t add)
{
    int32_t ret;
    o->rateIndex += add;
    ret = (int32_t)(o->rateIndex >> RATE_SH);
    o->rateIndex &= RATE_MASK;
    return ret;
}
static DBOPL2_RAMCODE __attribute__((always_inline)) inline int32_t op_volume(dbop *o)   /* dbopl's TemplateVolume */
{
    int32_t vol = o->volume, change;
    switch (o->state) {
    case ST_OFF:
        return ENV_MAX;
    case ST_ATTACK:
        change = op_rate_forward(o, o->attackAdd);
        if (!change) return vol;
        vol += ((~vol) * change) >> 3;
        if (vol < ENV_MIN) {
            o->volume = ENV_MIN; o->rateIndex = 0;
            op_set_state(o, ST_DECAY);
            return ENV_MIN;
        }
        break;
    case ST_DECAY:
        vol += op_rate_forward(o, o->decayAdd);
        if (vol >= o->sustainLevel) {
            if (vol >= ENV_MAX) { o->volume = ENV_MAX; op_set_state(o, ST_OFF); return ENV_MAX; }
            o->rateIndex = 0;
            op_set_state(o, ST_SUSTAIN);
        }
        break;
    case ST_SUSTAIN:
        if (o->reg20 & MASK_SUSTAIN) return vol;
        /* fall through: sustain without the sustain bit releases */
    default:
        vol += op_rate_forward(o, o->releaseAdd);
        if (vol >= ENV_MAX) { o->volume = ENV_MAX; op_set_state(o, ST_OFF); return ENV_MAX; }
        break;
    }
    o->volume = vol;
    return vol;
}
static DBOPL2_RAMCODE __attribute__((always_inline)) inline uint32_t op_forward_volume(dbop *o)
{
    return o->currentLevel + (uint32_t)op_volume(o);
}
static DBOPL2_RAMCODE __attribute__((always_inline)) inline uint32_t op_forward_wave(dbop *o)
{
    o->waveIndex += o->waveCurrent;
    return o->waveIndex >> WAVE_SH;
}
static DBOPL2_RAMCODE __attribute__((always_inline)) inline int32_t op_get_wave(const dbop *o, uint32_t index, uint32_t vol)
{
    return (o->waveBase[index & o->waveMask] * MulTable[vol]) >> MUL_SH;
}
static DBOPL2_RAMCODE __attribute__((always_inline)) inline int32_t op_get_sample(dbop *o, int32_t modulation)
{
    uint32_t vol = op_forward_volume(o);
    if (ENV_SILENT(vol)) {
        o->waveIndex += o->waveCurrent;      /* just move the wave on */
        return 0;
    } else {
        uint32_t index = op_forward_wave(o) + (uint32_t)modulation;
        return op_get_wave(o, index, vol);
    }
}

static DBOPL2_RAMCODE __attribute__((always_inline)) inline uint32_t chip_forward_noise(dbopl2 *chip)
{
    uint32_t count;
    chip->noiseCounter += DBOPL2_NOISE_ADD;
    count = chip->noiseCounter >> LFO_SH;
    chip->noiseCounter &= (1u << LFO_SH) - 1u;   /* r3635 masked with WAVE_MASK: the LFSR ran away, hundreds of steps a sample; fixed in later DOSBox */
    for (; count > 0; --count) {              /* MAME's 23-bit LFSR */
        chip->noiseValue ^= 0x800302u & (0u - (chip->noiseValue & 1u));
        chip->noiseValue >>= 1;
    }
    return chip->noiseValue;
}

static DBOPL2_RAMCODE uint32_t chip_forward_lfo(dbopl2 *chip, uint32_t samples)
{
    uint32_t todo, count;
    chip->vibratoSign = (int8_t)(VibratoTable[chip->vibratoIndex >> 2] >> 7);
    chip->vibratoShift = (uint8_t)((VibratoTable[chip->vibratoIndex >> 2] & 7) + chip->vibratoStrength);
    chip->tremoloValue = (uint8_t)(TremoloTable[chip->tremoloIndex] >> chip->tremoloStrength);
    todo = LFO_MAX - chip->lfoCounter;
    count = (todo + DBOPL2_LFO_ADD - 1u) / DBOPL2_LFO_ADD;   /* a power of two: a shift */
    if (count > samples) {
        count = samples;
        chip->lfoCounter += count * DBOPL2_LFO_ADD;
    } else {
        chip->lfoCounter += count * DBOPL2_LFO_ADD;
        chip->lfoCounter &= LFO_MAX - 1u;
        chip->vibratoIndex = (uint8_t)((chip->vibratoIndex + 1u) & 31u);
        if (chip->tremoloIndex + 1u < TREMOLO_TABLE) ++chip->tremoloIndex;
        else chip->tremoloIndex = 0;
    }
    return count;
}

static DBOPL2_RAMCODE __attribute__((always_inline)) inline void ch_percussion(dbchan *c, dbopl2 *chip, int32_t *output, uint32_t i, const int32_t keep[3])
{
    int32_t mod = (int32_t)((uint32_t)(c->old[0] + c->old[1]) >> c->feedback);
    int32_t sample, bd, hh = 0, sd = 0, rest;
    uint32_t noiseBit, c2, c5, phaseBit, hhVol, sdVol, tcVol;
    c->old[0] = c->old[1];                   /* bass drum: op 0 with feedback, op 1 */
    c->old[1] = op_get_sample(&c->op[0], mod);
    mod = (c->regC0 & 1) ? 0 : c->old[0];
    sample = op_get_sample(&c->op[1], mod);
    DBOPL2_TAP(6, i, sample << 1);
    bd = sample;
    noiseBit = chip_forward_noise(chip) & 1u;
    c2 = op_forward_wave(OP(c, 2));
    c5 = op_forward_wave(OP(c, 5));
    phaseBit = (((c2 & 0x88) ^ ((c2 << 5) & 0x80)) | ((c5 ^ (c5 << 2)) & 0x20)) ? 0x02 : 0x00;
    hhVol = op_forward_volume(OP(c, 2));     /* hi-hat */
    if (!ENV_SILENT(hhVol)) {
        uint32_t hhIndex = (phaseBit << 8) | (0x34u << (phaseBit ^ (noiseBit << 1)));
        hh = op_get_wave(OP(c, 2), hhIndex, hhVol);
    }
    sdVol = op_forward_volume(OP(c, 3));     /* snare */
    if (!ENV_SILENT(sdVol)) {
        uint32_t sdIndex = (0x100u + (c2 & 0x100u)) ^ (noiseBit << 8);
        sd = op_get_wave(OP(c, 3), sdIndex, sdVol);
    }
    DBOPL2_TAP(7, i, sd << 1);
    rest = hh + op_get_sample(OP(c, 4), 0);  /* tom */
    tcVol = op_forward_volume(OP(c, 5));     /* cymbal */
    if (!ENV_SILENT(tcVol)) {
        uint32_t tcIndex = (1u + phaseBit) << 8;
        rest += op_get_wave(OP(c, 5), tcIndex, tcVol);
    }
    DBOPL2_TAP(8, i, rest << 1);
    output[0] += ((bd & keep[0]) + (sd & keep[1]) + (rest & keep[2])) << 1;
}

/* one channel (three in rhythm mode) over a block; returns the next channel */
static DBOPL2_RAMCODE __attribute__((always_inline)) inline dbchan *ch_block(dbchan *c, dbopl2 *chip, uint32_t samples, int32_t *output, const int mode)
{
    uint32_t i, ch = (uint32_t)(c - chip->chan);
    const int32_t keep = (chip->mute >> ch) & 1u ? 0 : -1;   /* muted: computed, tapped, not mixed */
    if (mode == SM_AM) {
        if (op_silent(&c->op[0]) && op_silent(&c->op[1])) { c->old[0] = c->old[1] = 0; DBOPL2_TAP_ZERO(ch, samples); return c + 1; }
    } else if (mode == SM_FM) {
        if (op_silent(&c->op[1])) { c->old[0] = c->old[1] = 0; DBOPL2_TAP_ZERO(ch, samples); return c + 1; }
    }
    op_prepare(&c->op[0], chip); op_prepare(&c->op[1], chip);
    if (mode == SM_PERC) {
        op_prepare(OP(c, 2), chip); op_prepare(OP(c, 3), chip);
        op_prepare(OP(c, 4), chip); op_prepare(OP(c, 5), chip);
        {
            const int32_t keep3[3] = { keep, (chip->mute >> 7) & 1u ? 0 : -1, (chip->mute >> 8) & 1u ? 0 : -1 };
            for (i = 0; i < samples; i++) ch_percussion(c, chip, output + i, i, keep3);
        }
        return c + 3;
    }
    for (i = 0; i < samples; i++) {
        int32_t mod = (int32_t)((uint32_t)(c->old[0] + c->old[1]) >> c->feedback), out0, s;
        c->old[0] = c->old[1];
        c->old[1] = op_get_sample(&c->op[0], mod);
        out0 = c->old[0];
        if (mode == SM_AM) s = out0 + op_get_sample(&c->op[1], 0);
        else s = op_get_sample(&c->op[1], out0);
        DBOPL2_TAP(ch, i, s);
        output[i] += s & keep;
    }
    return c + 1;
}
static DBOPL2_RAMCODE __attribute__((noinline)) dbchan *ch_block_fm(dbchan *c, dbopl2 *chip, uint32_t n, int32_t *out) { return ch_block(c, chip, n, out, SM_FM); }
static DBOPL2_RAMCODE __attribute__((noinline)) dbchan *ch_block_am(dbchan *c, dbopl2 *chip, uint32_t n, int32_t *out) { return ch_block(c, chip, n, out, SM_AM); }
static DBOPL2_RAMCODE __attribute__((noinline)) dbchan *ch_block_perc(dbchan *c, dbopl2 *chip, uint32_t n, int32_t *out) { return ch_block(c, chip, n, out, SM_PERC); }

/* total mono samples ADDED into output (the caller zeroes it: this code lives in RAM on the FM-1 and may not
 * call flash, which a zeroing loop can turn into); dbopl's GenerateBlock2 otherwise */
DBOPL2_RAMCODE void dbopl2_generate(dbopl2 *chip, uint32_t total, int32_t *output)
{
    while (total > 0) {
        uint32_t samples = chip_forward_lfo(chip, total);
        dbchan *c;
        for (c = chip->chan; c < chip->chan + 9;) {
            if (c->mode == SM_FM) c = ch_block_fm(c, chip, samples, output);
            else if (c->mode == SM_AM) c = ch_block_am(c, chip, samples, output);
            else c = ch_block_perc(c, chip, samples, output);
        }
        DBOPL2_TAP_ADVANCE(samples);
        total -= samples;
        output += samples;
    }
}
