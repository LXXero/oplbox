/* SPDX-License-Identifier: GPL-3.0-only */
/* OPLBOX: the M-VAVE FM-1 as an AdLib. Plays the SoundFX Macs Opera songs of LINEL's games and of
 * Stone Age (Eclipse, 1992) through an OPL2, the way FXDRIVER V1.06 played them on a Sound Blaster:
 * register for register (verified against DOSBox captures of the original driver).
 *
 *   PLAY           play / pause          HOME     back to the start of the song
 *   OCT- OCT+      previous / next song  SELECT   turn: pick a song (plays it if playing)
 *   REC            loop one song <-> play them all in turn (the LED says which)
 *   SEQ            jukebox screen <-> tracker screen (the pattern scrolling by, one column per channel)
 *   FX             jukebox screen <-> scope screen (one oscilloscope per channel, labelled with its instrument);
 *                  there, the first nine white keys (F3..G4) mute / unmute channels 1..9 (key LED = muted)
 *   MASTER         volume (platform.c)
 *
 * The song that was playing and the loop mode survive a power-off (settings sector).
 * Built on Dinghy (zvenson) / Felucca (Leo Kuroshita, Hügelton Instruments) / SLOOP (isod89), GPL-3.0.
 * OPL cores: DOSBox's dbopl (GPL-2+, the default: a third of the CPU) or Nuked-OPL3 (LGPL-2.1, cycle-exact,
 * -DOPL_CORE=OPL_NUKED: more than the chip has, until it runs on the second core).
 * Player after AdPlug's cmfmcsop.cpp (Sebastian Kienzl), LGPL-2.1. */
#define APP_NAME "OPLBOX"
#include "platform.c"
#ifdef DINGHY_HOST                            /* the fake FM-1 has no tick counter and no busy wait */
#define FM1_TICKS_PER_US 24u
static uint32_t fm1_ticks(void) { return 0; }
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#endif
#define OPL_DBOPL 1
#define OPL_NUKED 2
#ifndef OPL_CORE
#define OPL_CORE OPL_DBOPL
#endif
#include "macsop.h"
#include "songs.h"

static macsop player;                         /* the audio ISR owns the chip and the player while playing */
static volatile uint32_t playing;             /* 1: the ISR runs the chip and the player   (main writes, ISR reads) */
static volatile uint32_t song_over;           /* the ISR says the song ended; main picks what is next */
static volatile uint32_t audio_us;            /* the last ISR's render time, for the screen */
static volatile uint32_t pos_order, pos_row;  /* where the player is, for the screen */
static volatile uint32_t rows_done;           /* rows played since the song started */
static volatile uint32_t overload;            /* the ISR could not keep up for 1/4 s and stopped itself */
static uint32_t over_n;                       /* consecutive interrupts over budget */
#define ISR_BUDGET_US 5805u                   /* 256 frames at 44.1 kHz */

/* OPL2 runs at 49716 Hz; the FM-1 at 44100. Native samples are generated as needed and interpolated:
 * no division, and the row clock ticks on native samples so the timing is the driver's (18.2 / 9.1 / 4.55 rows/s). */
#define RS_STEP 73875u                        /* 49716 / 44100 in Q16 */
static const uint32_t SPR_Q16[4] = { 0, 179021084u, 358042167u, 716084334u };   /* samples per row, Q16, speed 1..3 */
static uint32_t rs_phase, row_acc;            /* resampler phase (Q16); samples until the next row (Q16) */
static int32_t s_prev, s_cur;                 /* the last two native samples */

static void tick_row(void)                    /* the row clock: before a native sample, when it is due */
{
    if (playing && !song_over) {
        if (!macsop_tick(&player))
            song_over = 1;
        pos_order = player.order_idx; pos_row = player.row; rows_done++;
    }
    row_acc += SPR_Q16[player.speed];
}

#if OPL_CORE == OPL_NUKED
#include "opl3.c"
static opl3_chip chip __attribute__((section(".pool")));       /* 21 KB: too big for .bss, lives in the pool */
static void opl_write(void *ctx, uint16_t reg, uint8_t val)
{
    (void)ctx;
    OPL3_WriteRegBuffered(&chip, reg, val);   /* as the ISA bus did: one write per chip sample at most */
}
#include "opl2fast.c"
#define SCOPE_N 512u                          /* no taps in the Nuked path: the scope screen shows flat lines */
static volatile uint32_t scope_on;
static int16_t scope[9][SCOPE_N];
static uint32_t scope_w;
static void chip_reset(void)
{
    uint32_t k;
    OPL3_Reset(&chip, 49716);
    OPL3_WriteReg(&chip, 0x105, 0);           /* an OPL2: no OPL3 features */
    for (k = 0; k < 4096u; k++)               /* drain the write buffer */
        opl2_generate(&chip);
}
static void chip_write_now(uint16_t reg, uint8_t val) { OPL3_WriteReg(&chip, reg, val); }
static void chip_flush(void) { uint32_t k; for (k = 0; k < 4096u; k++) opl2_generate(&chip); }
static void chip_mute(uint32_t mask) { (void)mask; }        /* no mute in the Nuked path */
static int32_t next_native(void)
{
    if (row_acc < (1u << 16))
        tick_row();
    row_acc -= 1u << 16;
    return opl2_generate(&chip);
}
#else
#ifndef DINGHY_HOST
#define DBOPL2_RAMDATA __attribute__((section(".data.dbopl2")))      /* wave and volume tables in RAM, not flash */
#define DBOPL2_RAMCODE __attribute__((section(".ram_code.dbopl2")))  /* the sample loop in RAM (app.ld, build.py) */
#endif
/* the scope screen's taps: each channel's last SCOPE_N native samples, written by the ISR when the screen is up */
#define SCOPE_N 512u
static volatile uint32_t scope_on;
static int16_t scope[9][SCOPE_N];
static uint32_t scope_w;
#define DBOPL2_TAP(ch, i, s) do { if (scope_on) scope[ch][(scope_w + (i)) & (SCOPE_N - 1u)] = (int16_t)(s); } while (0)
#define DBOPL2_TAP_ZERO(ch, n) do { if (scope_on) { uint32_t k_; for (k_ = 0; k_ < (n); k_++) scope[ch][(scope_w + k_) & (SCOPE_N - 1u)] = 0; } } while (0)
#define DBOPL2_TAP_ADVANCE(n) (scope_w += (n))
#include "dbopl2.c"
static dbopl2 chip;
#define NAT_CHUNK 128u                        /* native samples generated per block (a row is >= 2731) */
static int32_t nat[NAT_CHUNK];
static uint32_t nat_rd, nat_n;
static void opl_write(void *ctx, uint16_t reg, uint8_t val)
{
    (void)ctx;
    dbopl2_write(&chip, reg, val);            /* between blocks: a write lands at the next block boundary */
}
static void chip_reset(void) { dbopl2_reset(&chip); nat_rd = nat_n = 0; }
static void chip_mute(uint32_t mask) { chip.mute = mask; }   /* bit n: channel n out of the mix */
static void chip_write_now(uint16_t reg, uint8_t val) { dbopl2_write(&chip, reg, val); }
static void chip_flush(void) { }
static void refill(void)                      /* the next block: up to the row due next, at most NAT_CHUNK */
{
    uint32_t k, i;
    if (row_acc < (1u << 16))
        tick_row();
    k = row_acc >> 16;
    if (k > NAT_CHUNK) k = NAT_CHUNK;
    row_acc -= k << 16;
    for (i = 0; i < k; i++) nat[i] = 0;
#ifdef DINGHY_HOST
    dbopl2_generate(&chip, k, nat);
#else
    FL_FAR(dbopl2_generate)(&chip, k, nat);   /* RAM code: beyond the call range of XIP, through a pointer */
#endif
    nat_rd = 0; nat_n = k;
}
static inline int32_t next_native(void)
{
    if (nat_rd == nat_n)
        refill();
    return nat[nat_rd++];
}
#endif
#include "macsop.c"

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, t0 = fm1_ticks();
    if (!playing)
        return;                               /* silence; main may be loading a song into the chip */
    for (i = 0; i < n; i++) {
        int32_t s;
        rs_phase += RS_STEP;
        while (rs_phase >= (1u << 16)) {
            rs_phase -= 1u << 16;
            s_prev = s_cur;
            s_cur = next_native();
        }
        s = s_prev + (((s_cur - s_prev) * (int32_t)rs_phase) >> 16);   /* unity: the OPL's full scale is the DAC's */
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;      /* ECHOS and KAISER touch it */
        out[2u * i] += s;
        out[2u * i + 1u] += s;
    }
    audio_us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
    /* over budget a quarter second in a row: the sound stops itself, so the main loop, USB and the
     * updater keep the device reachable (0.1 on hardware: 100 %+ and the hub could not talk to it) */
    if (audio_us > ISR_BUDGET_US) {
        if (++over_n >= 43u) { playing = 0; overload = 1; over_n = 0; }
    } else
        over_n = 0;
}

/* ---- songs (main loop) */
static uint32_t cur;                          /* the song in the player */

static void chip_silence(void)                /* every key off, so a paused song does not hang a note */
{
    uint32_t ch;
    for (ch = 0; ch < 9u; ch++)
        chip_write_now((uint16_t)(0xB0 + ch), (uint8_t)(player.bx[ch] & ~0x20u));
    chip_write_now(0xBD, (uint8_t)(player.bd & 0x20u));
}

static void load_song(uint32_t i)             /* with the ISR off the chip (playing == 0) */
{
    cur = i;
    chip_reset();
    macsop_load(&player, SONG_DATA + SONGS[i].off, SONGS[i].len, opl_write, 0);
    chip_flush();                             /* the rewind's register writes land */
    rs_phase = 0; row_acc = 0; s_prev = s_cur = 0;
    song_over = 0; pos_order = 0; pos_row = 0; rows_done = 0;
}

static void stop(void)
{
    playing = 0;
    fm1_delay_ms(7);                          /* the ISR that may be running finishes (5.8 ms) */
    chip_silence();
}

/* ---- the screen */
#define C_ACC RGB(255, 170, 60)               /* amber, like a banker's lamp */
#define C_MUTE RGB(110, 110, 110)
#define C_TXT RGB(220, 220, 220)

static void u2s(char *s, uint32_t v, uint32_t digits)   /* decimal, fixed width, no printf on the chip */
{
    uint32_t i;
    for (i = digits; i-- > 0;) { s[i] = (char)('0' + v % 10u); v /= 10u; }
    s[digits] = 0;
}

static void draw_title(uint32_t i)
{
    char s[24];
    uint32_t k = 0;
    lcd_fill(0, 36, 240, 40, C_BLACK);
    if (text_w(&FONT_L, SONGS[i].title) <= 240)
        draw_text_box(0, 42, 240, &FONT_L, SONGS[i].title, C_ACC, 1);
    else                                      /* a long title: the small font, same place */
        draw_text_box(0, 50, 240, &FONT_S, SONGS[i].title, C_ACC, 1);
    draw_text_box(0, 78, 240, &FONT_S, SONGS[i].by, C_MUTE, 1);
    s[k++] = (char)('0' + (i + 1u) / 10u); s[k++] = (char)('0' + (i + 1u) % 10u); s[k++] = '/';
    s[k++] = (char)('0' + N_SONGS / 10u); s[k++] = (char)('0' + N_SONGS % 10u); s[k++] = ' '; s[k++] = ' ';
    { const char *p = SONGS[i].file; while (*p && k < 22u) s[k++] = *p++; }
    s[k] = 0;
    draw_text_box(0, 98, 240, &FONT_S, s, C_MUTE, 1);
}

static void draw_instruments(void)          /* the first six instrument names, two columns */
{
    uint32_t i;
    cv_begin(240, 56, C_BLACK);
    for (i = 0; i < 6u && i < player.n_instr; i++)
        cv_text((i & 1u) ? 124 : 4, (int32_t)((i >> 1) * 18u), &FONT_S, macsop_instr_name(&player, (int)i), C_MUTE);
    cv_blit(0, 124);
}

static uint32_t pct_colour(uint32_t us)      /* the load figure: amber past 85 %, red when it does not fit */
{
    return us > 5805u ? RGB(255, 60, 60) : us > 4934u ? C_ACC : C_MUTE;
}

static void draw_status(int play, int loop_all)
{
    char s[40];
    uint32_t k = 0;
    const char *p;
    cv_begin(240, 40, C_BLACK);
    p = play ? "PLAY " : "PAUSE";
    cv_text(4, 0, &FONT_S, p, play ? C_ACC : C_MUTE);
    u2s(s, pos_order + 1u, 2); s[2] = '/'; u2s(s + 3, player.n_orders, 2); s[5] = ' '; s[6] = ' ';
    u2s(s + 7, pos_row, 2); s[9] = 0;
    cv_text(70, 0, &FONT_S, s, C_TXT);
    p = player.speed == 1 ? "18/S" : player.speed == 2 ? "9/S" : "4.5/S";
    cv_text(150, 0, &FONT_S, p, C_MUTE);
    cv_text(200, 0, &FONT_S, player.rhythm ? "RHY" : "MEL", C_MUTE);
    /* second line: mode and the audio load */
    cv_text(4, 20, &FONT_S, loop_all ? "ALL IN TURN" : "LOOP SONG", C_MUTE);
    k = 0; s[k++] = 'C'; s[k++] = 'P'; s[k++] = 'U'; s[k++] = ' ';
    {   uint32_t pct = (audio_us * 100u) / 5805u;         /* of the 5.8 ms an interrupt has */
        if (pct > 999u) pct = 999u;
        u2s(s + k, pct, 3); k += 3; if (s[4] == '0') { s[4] = s[5]; s[5] = s[6]; k--; } if (s[4] == '0' && s[5] != 0) { s[4] = s[5]; k--; }
    }
    s[k++] = '%'; s[k] = 0;
    cv_text(160, 20, &FONT_S, s, pct_colour(audio_us));
    cv_blit(0, 190);
}

/* ---- the tracker screen: the current pattern as a tracker shows it, the playing row in the middle.
 * Notes as "C4" / "c4" (lowercase = sharp), 9 columns 24 px apart for a melodic song, 11 (6 + BD SD TM TC HH)
 * 20 px apart for a rhythm song: 240 px is 30 characters. A pattern is read into a grid once per pattern;
 * editing it is for later (EDIT). */
#define TR_TOP  38u                            /* first pattern row on screen */
#define TR_ROWS 9u                             /* rows shown: 4 before, the playing one, 4 after */
#define TR_CUR  4u
#define C_ROWBG RGB(52, 36, 10)                /* the playing row's bar */
static uint8_t grid[64][MACSOP_CHANNELS];
static uint16_t grid_pat = 0xFFFF;

static void note_text(char *s, uint8_t n, int wide)   /* "C#4" / "..." / "===" / "END", or 2 chars: "c4" */
{
    static const char NAMES[12] = "CCDDEFFGGAAB", SHARP[12] = " # #  # # # ";
    const char *fixed = n == 0 ? "..." : n == 4 ? "===" : n == 1 ? "END" : (n < 24 || n > 119) ? "???" : 0;
    if (fixed) { s[0] = fixed[0]; s[1] = fixed[1]; s[2] = fixed[2]; }
    else {
        uint32_t i = n % 12u, oct = n / 12u - 2u;
        if (wide) { s[0] = NAMES[i]; s[1] = SHARP[i] == ' ' ? '-' : '#'; s[2] = (char)('0' + oct); }
        else      { s[0] = SHARP[i] == ' ' ? NAMES[i] : (char)(NAMES[i] + 32); s[1] = (char)('0' + oct); }
    }
    s[wide ? 3 : 2] = 0;
}

static void draw_tracker_frame(void)          /* title and column labels (once per song / view switch) */
{
    static const char *const DRUMS[5] = { "BD", "SD", "TM", "TC", "HH" };
    uint32_t ncol = player.rhythm ? MACSOP_CHANNELS : 9u, cw = ncol <= 9u ? 24u : 20u, c;
    char s[3];
    lcd_fill(0, 0, 240, TR_TOP, C_BLACK);
    draw_text_box(0, 2, 240, &FONT_S, SONGS[cur].title, C_ACC, 1);
    cv_begin(240, 16, C_BLACK);
    for (c = 0; c < ncol; c++) {
        const char *l;
        if (c < 6u || ncol <= 9u) { s[0] = (char)('1' + c); s[1] = 0; l = s; }
        else l = DRUMS[c - 6u];
        cv_text((int32_t)(20u + c * cw + (l[1] ? 0u : 4u)), 0, &FONT_S, l, C_MUTE);
    }
    cv_blit(0, 20);
    grid_pat = 0xFFFF;
}

static void draw_tracker(void)                /* the rows around the playing one, three rows a canvas (CV_MAX) */
{
    uint32_t ncol = player.rhythm ? MACSOP_CHANNELS : 9u, cw = ncol <= 9u ? 24u : 20u;
    uint32_t row = pos_row, band, i, c;
    uint16_t pat = macsop_order_pattern(&player, (uint16_t)pos_order);
    char s[4];
    if (pat != grid_pat) { macsop_pattern_grid(&player, pat, grid); grid_pat = pat; }
    for (band = 0; band < TR_ROWS / 3u; band++) {
        uint32_t r0 = band * 3u, rn = 3u;
        cv_begin(240, rn * 16u, C_BLACK);
        for (i = 0; i < rn; i++) {
            int32_t r = (int32_t)row - (int32_t)TR_CUR + (int32_t)(r0 + i);
            int32_t y = (int32_t)(i * 16u);
            int cur = r0 + i == TR_CUR;
            if (cur) cv_rect(0, y, 240, 16, C_ROWBG);
            if (r < 0 || r >= 64) continue;
            u2s(s, (uint32_t)r, 2);
            cv_text(2, y, &FONT_S, s, cur ? C_ACC : C_MUTE);
            for (c = 0; c < ncol; c++) {
                note_text(s, grid[r][c], 0);
                cv_text((int32_t)(20u + c * cw), y, &FONT_S, s, grid[r][c] ? (cur ? C_ACC : C_TXT) : C_MUTE);
            }
        }
        cv_blit(0, TR_TOP + r0 * 16u);
    }
}

/* ---- the scope screen: nine boxes, one oscilloscope per channel (rhythm mode: 6 = bass drum, 7 = hi-hat +
 * snare, 8 = tom + cymbal), each labelled with the instrument the channel plays. The trace is the channel's
 * own output before mixing, 4 native samples a pixel (6 ms across a box), triggered on a rising zero crossing. */
#define SC_TOP 20u
#define SC_BOXW 80u
#define SC_BOXH 56u
#define C_SCOPE_AXIS RGB(44, 44, 44)
#define C_MUTED RGB(255, 60, 60)
#define C_MUTED_TRACE RGB(110, 40, 40)
static uint32_t mute_mask;                    /* bit n: channel n muted (the scope's white keys) */
static const uint8_t WHITE_KEY[9] = { 0, 2, 4, 6, 7, 9, 11, 12, 14 };   /* F3 G3 A3 B3 C4 D4 E4 F4 G4 as key numbers */

static void draw_scope_frame(void)
{
    lcd_fill(0, 0, 240, SC_TOP, C_BLACK);
    draw_text_box(0, 2, 240, &FONT_S, SONGS[cur].title, C_ACC, 1);
}

static void draw_scope(void)
{
    static int16_t snap[SCOPE_N];
    uint32_t w0 = scope_w, r, c, k;
    char name[12];
    for (r = 0; r < 3u; r++) {
        cv_begin(240, SC_BOXH, C_BLACK);
        for (c = 0; c < 3u; c++) {
            uint32_t ch = r * 3u + c, x0 = c * SC_BOXW, t = 0, px, lit = 0, muted = (mute_mask >> ch) & 1u;
            int32_t ylast = 0, peak = 0;
            const char *n = macsop_channel_instr(&player, (int)ch);
            for (k = 0; k < SCOPE_N; k++) {   /* oldest .. newest; the peak sets the gain (auto-range) */
                int32_t v = snap[k] = scope[ch][(w0 + k) & (SCOPE_N - 1u)];
                if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
            lit = peak >= 48;                 /* below that it is the tail of a release: flat */
            if (peak < 48) peak = 48;
            for (k = 1; k < 200u; k++)        /* trigger: the first rising zero crossing */
                if (snap[k - 1] < 0 && snap[k] >= 0) { t = k; break; }
            {   /* "03 produc": the instrument's number first (what the tracker shows), then its name */
                int no = macsop_channel_instr_no(&player, (int)ch);
                uint32_t j = 0;
                if (no) { name[j++] = (char)('0' + no / 10); name[j++] = (char)('0' + no % 10); }
                else { name[j++] = '-'; name[j++] = '-'; }   /* the driver's default patch, before the song sets one */
                name[j++] = ' ';
                for (k = 0; j < 9u && n && n[k]; k++) name[j++] = n[k];
                name[j] = 0;
            }
            cv_text((int32_t)x0 + 3, 0, &FONT_S, name, muted ? C_MUTED : lit ? C_TXT : C_MUTE);
            cv_line((int32_t)x0 + 2, 37, (int32_t)x0 + (int32_t)SC_BOXW - 3, 37, C_SCOPE_AXIS);
            if (c) cv_line((int32_t)x0, 2, (int32_t)x0, (int32_t)SC_BOXH - 3, C_SCOPE_AXIS);
            for (px = 0; px < 76u; px++) {
                int32_t v = lit ? (snap[t + px * 4u] * 17) / peak : 0, y;
                y = 37 - v;
                if (px) cv_line((int32_t)(x0 + 1u + px), ylast, (int32_t)(x0 + 2u + px), y,
                                muted ? C_MUTED_TRACE : lit ? C_ACC : C_MUTE);
                ylast = y;
            }
        }
        cv_blit(0, SC_TOP + r * SC_BOXH);
    }
}

static void app_main(void)
{
    struct { uint8_t song, loop_all; } set = {0, 0};
    uint32_t shown_order = ~0u, shown_row = ~0u, t_status = 0, t_change = 0, dirty = 0, play = 0, next = 0;
    uint32_t view = 0, t_scope = 0;           /* 0: jukebox, 1: tracker (SEQ), 2: scope (FX) */
    uint32_t keys_prev = 0;
    int32_t spin;
    settings_load(&set, sizeof set);
    if (set.song >= N_SONGS) set.song = 0;
    set.loop_all &= 1u;

    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 6, 240, &FONT_S, "OPLBOX " APP_VER " - MACS OPERA", C_MUTE, 1);
    load_song(set.song);
    draw_title(cur);
    draw_instruments();
    play = 1; playing = 1;                    /* a jukebox starts playing */

    for (;;) {
        uint32_t b, rel;
        uint8_t st, d1, d2;
        platform_poll();
        while (midi_get(&st, &d1, &d2)) { }   /* drain MIDI in: a full ring NAKs the USB endpoint and locks SysEx
                                               * (the installer's handshake) out - 0.1 never read it (Smalley) */

        b = fm1_input_edges(&rel);
        {   /* the scope's white keys: mute / unmute a channel */
            uint32_t keys = fm1_in.notes, down = keys & ~keys_prev, k;
            keys_prev = keys;
            if (view == 2u && down)
                for (k = 0; k < 9u; k++)
                    if (down & (1u << WHITE_KEY[k])) { mute_mask ^= 1u << k; chip_mute(mute_mask); }
        }
        spin = fm1_enc_take(ENC_SELECT);
        if (b & (1u << BTN_OCT_UP)) spin++;
        if (b & (1u << BTN_OCT_DOWN)) spin--;
        if (song_over) {                      /* the ISR stopped at the end */
            next = set.loop_all ? 1 : 0;
            if (!next) { stop(); load_song(cur); playing = play ? 1u : 0u; }
        }
        if (spin || next) {
            uint32_t i = next ? (cur + 1u) % N_SONGS
                       : (uint32_t)(((int32_t)cur + spin) % (int32_t)N_SONGS + (int32_t)N_SONGS) % N_SONGS;
            next = 0;
            stop();
            mute_mask = 0;                    /* a new song starts with every channel in */
            load_song(i);
            if (view == 1u) draw_tracker_frame(); else if (view == 2u) draw_scope_frame(); else { draw_title(cur); draw_instruments(); }
            playing = play ? 1u : 0u;
            set.song = (uint8_t)cur; dirty = 1; t_change = fm1_ms;
            shown_order = ~0u;
        }
        if (overload && play) {               /* the ISR stopped itself: show it as paused, PLAY tries again */
            play = 0; stop(); shown_order = ~0u;
            draw_text_box(0, view ? 2u : 78u, 240, &FONT_S, "AUDIO OVERLOAD - PAUSED", RGB(255, 60, 60), 1);
        }
        if (b & (1u << BTN_PLAY)) {
            play ^= 1u;
            if (play) { overload = 0; if (view == 1u) draw_tracker_frame(); else if (view == 2u) draw_scope_frame(); else draw_title(cur); playing = 1; } else stop();
            shown_order = ~0u;
        }
        if (b & (1u << BTN_HOME)) {
            stop(); load_song(cur); playing = play ? 1u : 0u; shown_order = ~0u;
        }
        if (b & (1u << BTN_REC)) {
            set.loop_all ^= 1u; dirty = 1; t_change = fm1_ms; shown_order = ~0u;
        }
        if (b & ((1u << BTN_SEQ) | (1u << BTN_FX))) {   /* SEQ: tracker, FX: scope; again: the jukebox */
            uint32_t want = (b & (1u << BTN_SEQ)) ? 1u : 2u;
            view = view == want ? 0u : want;
            scope_on = view == 2u;
            lcd_fill(0, 0, 240, 190, C_BLACK);
            if (view == 1u) draw_tracker_frame();
            else if (view == 2u) draw_scope_frame();
            else {
                draw_text_box(0, 6, 240, &FONT_S, "OPLBOX " APP_VER " - MACS OPERA", C_MUTE, 1);
                draw_title(cur); draw_instruments();
            }
            shown_order = ~0u;
        }

        fm1_led_key(BTN_PLAY, play);
        fm1_led_key(BTN_REC, set.loop_all);
        fm1_led_key(BTN_SEQ, view == 1u);
        fm1_led_key(BTN_FX, view == 2u);
        {   uint32_t k;                       /* the white keys' LEDs: lit = that channel is muted */
            for (k = 0; k < 9u; k++) fm1_led_key(14u + WHITE_KEY[k], (int)((mute_mask >> k) & 1u));
        }
        if (view == 2u && fm1_ms - t_scope > 50u) {          /* the scope: 20 frames a second */
            t_scope = fm1_ms;
            draw_scope();
        }
        if (pos_order != shown_order || pos_row != shown_row || fm1_ms - t_status > 250u) {
            int moved = pos_order != shown_order || pos_row != shown_row;
            shown_order = pos_order; shown_row = pos_row; t_status = fm1_ms;
            if (view == 1u && moved) draw_tracker();
            draw_status((int)play, (int)set.loop_all);
        }
        if (dirty && !playing && fm1_ms - t_change > 2000u) {   /* an erase stalls the sound: only while paused */
            settings_save(&set, sizeof set); dirty = 0;
        }
        if (dirty && fm1_ms - t_change > 30000u) {               /* ... or after a while anyway, between two rows */
            settings_save(&set, sizeof set); dirty = 0;
        }
    }
}
