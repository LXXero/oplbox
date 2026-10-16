/* SPDX-License-Identifier: GPL-3.0-only */
/* OPLBOX: the M-VAVE FM-1 as an AdLib. Plays the SoundFX Macs Opera songs of LINEL's games and of
 * Stone Age (Eclipse, 1992) through an OPL2 (Nuked-OPL3), the way FXDRIVER V1.06 played them on a
 * Sound Blaster: register for register (verified against DOSBox captures of the original driver).
 *
 *   PLAY           play / pause          HOME     back to the start of the song
 *   OCT- OCT+      previous / next song  SELECT   turn: pick a song (plays it if playing)
 *   REC            loop one song <-> play them all in turn (the LED says which)
 *   MASTER         volume (platform.c)
 *
 * The song that was playing and the loop mode survive a power-off (settings sector).
 * Built on Dinghy (zvenson) / Felucca (Leo Kuroshita, Hügelton Instruments) / SLOOP (isod89), GPL-3.0.
 * Nuked-OPL3 (Alexey Khokholov), LGPL-2.1. Player after AdPlug's cmfmcsop.cpp (Sebastian Kienzl), LGPL-2.1. */
#define APP_NAME "OPLBOX"
#include "platform.c"
#ifdef DINGHY_HOST                            /* the fake FM-1 has no tick counter and no busy wait */
#define FM1_TICKS_PER_US 24u
static uint32_t fm1_ticks(void) { return 0; }
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#endif
#include "opl3.c"
#include "macsop.c"
#include "songs.h"

/* ---- the chip and the player (the audio ISR owns them while playing) */
static opl3_chip chip __attribute__((section(".pool")));       /* 21 KB: too big for .bss, lives in the pool */
static macsop player;

static volatile uint32_t playing;             /* 1: the ISR runs the chip and the player   (main writes, ISR reads) */
static volatile uint32_t song_over;           /* the ISR says the song ended; main picks what is next */
static volatile uint32_t audio_us;            /* the last ISR's render time, for the screen */
static volatile uint32_t pos_order, pos_row;  /* where the player is, for the screen */
static volatile uint32_t rows_done;           /* rows played since the song started */

static void opl_write(void *ctx, uint16_t reg, uint8_t val)
{
    (void)ctx;
    OPL3_WriteRegBuffered(&chip, reg, val);   /* as the ISA bus did: one write per chip sample at most */
}


#include "opl2fast.c"

/* OPL2 runs at 49716 Hz; the FM-1 at 44100. Native samples are generated as needed and interpolated:
 * no division, and the row clock ticks on native samples so the timing is the driver's (18.2 / 9.1 / 4.55 rows/s). */
#define RS_STEP 73875u                        /* 49716 / 44100 in Q16 */
static const uint32_t SPR_Q16[4] = { 0, 179021084u, 358042167u, 716084334u };   /* samples per row, Q16, speed 1..3 */
static uint32_t rs_phase, row_acc;            /* resampler phase (Q16); samples until the next row (Q16) */
static int32_t s_prev, s_cur;                 /* the last two native samples */

static int32_t next_native(void)
{
    if (row_acc < (1u << 16)) {
        if (playing && !song_over) {
            if (!macsop_tick(&player))
                song_over = 1;
            pos_order = player.order_idx; pos_row = player.row; rows_done++;
        }
        row_acc += SPR_Q16[player.speed];
    }
    row_acc -= 1u << 16;
    return opl2_generate(&chip);
}

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
        s = s_prev + (((s_cur - s_prev) * (int32_t)rs_phase) >> 16);
        s = (s * 3) >> 1;                     /* the OPL's peak sits well under full scale: +3.5 dB */
        out[2u * i] += s;
        out[2u * i + 1u] += s;
    }
    audio_us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
}

/* ---- songs (main loop) */
static uint32_t cur;                          /* the song in the player */

static void chip_silence(void)                /* every key off, so a paused song does not hang a note */
{
    uint32_t ch;
    for (ch = 0; ch < 9u; ch++)
        OPL3_WriteReg(&chip, (uint16_t)(0xB0 + ch), (uint8_t)(player.bx[ch] & ~0x20u));
    OPL3_WriteReg(&chip, 0xBD, (uint8_t)(player.bd & 0x20u));
}

static void load_song(uint32_t i)             /* with the ISR off the chip (playing == 0) */
{
    uint32_t k;
    cur = i;
    OPL3_Reset(&chip, 49716);
    OPL3_WriteReg(&chip, 0x105, 0);           /* an OPL2: no OPL3 features */
    macsop_load(&player, SONG_DATA + SONGS[i].off, SONGS[i].len, opl_write, 0);
    for (k = 0; k < 4096u; k++)               /* drain the write buffer: the rewind's register writes */
        opl2_generate(&chip);
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
    u2s(s + k, (audio_us * 100u) / 5805u, 2); k += 2; s[k++] = '%'; s[k] = 0;
    cv_text(170, 20, &FONT_S, s, C_MUTE);
    cv_blit(0, 190);
}

static void app_main(void)
{
    struct { uint8_t song, loop_all; } set = {0, 0};
    uint32_t shown_order = ~0u, shown_row = ~0u, t_status = 0, t_change = 0, dirty = 0, play = 0, next = 0;
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
        platform_poll();

        b = fm1_input_edges(&rel);
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
            load_song(i);
            draw_title(cur);
            draw_instruments();
            playing = play ? 1u : 0u;
            set.song = (uint8_t)cur; dirty = 1; t_change = fm1_ms;
            shown_order = ~0u;
        }
        if (b & (1u << BTN_PLAY)) {
            play ^= 1u;
            if (play) playing = 1; else stop();
            shown_order = ~0u;
        }
        if (b & (1u << BTN_HOME)) {
            stop(); load_song(cur); playing = play ? 1u : 0u; shown_order = ~0u;
        }
        if (b & (1u << BTN_REC)) {
            set.loop_all ^= 1u; dirty = 1; t_change = fm1_ms; shown_order = ~0u;
        }

        fm1_led_key(BTN_PLAY, play);
        fm1_led_key(BTN_REC, set.loop_all);
        if (pos_order != shown_order || pos_row != shown_row || fm1_ms - t_status > 250u) {
            shown_order = pos_order; shown_row = pos_row; t_status = fm1_ms;
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
