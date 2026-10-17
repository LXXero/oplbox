/* SPDX-License-Identifier: GPL-3.0-only */
/* OPLBOX: the M-VAVE FM-1 as an AdLib, and as a SoundFX Macs Opera tracker. Plays the Macs Opera songs of
 * LINEL's games and of Stone Age (Eclipse, 1992) through an OPL2, the way FXDRIVER V1.06 played them on a
 * Sound Blaster (register for register, verified against DOSBox captures), edits them on the panel, and keeps
 * them as .CMF files the DOS tracker reads.
 *
 * Everywhere:
 *   PLAY           play / pause                 HOME     back to the start (editing: cursor to row 0)
 *   SEQ            jukebox <-> tracker screen   FX       jukebox <-> scope screen (nine oscilloscopes)
 *   EDIT           edit mode on / off (in the tracker screen; SEQ then cycles tracker / song / instrument)
 *   MASTER         volume (platform.c)
 * Jukebox / tracker / scope:
 *   OCT- OCT+      previous / next song         SELECT   turn: pick a song (18 built in, then 9 flash slots)
 *   REC            loop one song <-> play them all in turn (the LED says which)
 *   scope: the first nine white keys (F3..G4) mute / unmute channels 1..9 (key LED = muted)
 * Editing (EDIT, LED lit; the tracker screen follows the cursor, not the playback):
 *   K1 / K2        cursor column / row          SELECT   order position
 *   keys           enter the note (and audition it), then step; OCT- OCT+ keyboard octave
 *   K3             note +- semitone             K4       instrument    ALGO  volume    PRESETS  step
 *   FX             key off (==)                 SCL      clear the cell        GLO   end of pattern here
 *   PLAY           play from the cursor / pause SAVE     save the song to its flash slot (or the first free one)
 *   song screen:   SELECT position, K1 pattern number, GLO insert, SCL delete, ARP new pattern, K2 speed, K3 rhythm
 *   instrument:    PRESETS instrument, SELECT parameter, K1 value (name: K1 position, K2 character), ARP new, keys audition
 *
 * The song that was playing and the loop mode survive a power-off (settings sector).
 * Built on Dinghy (zvenson) / Felucca (Leo Kuroshita, Hügelton Instruments) / SLOOP (isod89), GPL-3.0.
 * OPL cores: DOSBox's dbopl (GPL-2+, the default) or Nuked-OPL3 (LGPL-2.1, -DOPL_CORE=OPL_NUKED).
 * Player after AdPlug's cmfmcsop.cpp (Sebastian Kienzl), LGPL-2.1. */
#define APP_NAME "OPLBOX"
#include "platform.c"
#ifdef DINGHY_HOST                            /* the fake FM-1 has no tick counter and no busy wait */
#define FM1_TICKS_PER_US 24u
static uint32_t fm1_ticks(void) { return 0; }
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
static inline uint32_t irq_save(void) { return 0; }
static inline void irq_restore(uint32_t f) { (void)f; }
#endif
#define OPL_DBOPL 1
#define OPL_NUKED 2
#ifndef OPL_CORE
#define OPL_CORE OPL_DBOPL
#endif
#include "macsop.h"
#include "songs.h"

static macsop_song song __attribute__((section(".pool")));    /* the song being played / edited (190 KB) */
static macsop player;                         /* the audio ISR owns the chip and the player while playing */
static volatile uint32_t chip_run;            /* 1: the ISR generates sound (main writes, ISR reads) */
static volatile uint32_t playing;             /* 1: the ISR also ticks the song's rows */
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

/* register writes queue here (the player's from the ISR, the editor's from main) and land on the chip between
 * blocks, so main never touches the chip while the ISR is inside it */
#define WQ_N 512u
static volatile uint16_t wq_reg[WQ_N];
static volatile uint8_t wq_val[WQ_N];
static volatile uint32_t wq_w, wq_r;

static void opl_write(void *ctx, uint16_t reg, uint8_t val)
{
    uint32_t f = irq_save(), i = wq_w;
    (void)ctx;
    if (i - wq_r < WQ_N) {
        wq_reg[i & (WQ_N - 1u)] = reg; wq_val[i & (WQ_N - 1u)] = val;
        wq_w = i + 1u;
    }
    irq_restore(f);
}

static void tick_row(void)                    /* the row clock: before a native sample, when it is due */
{
    if (playing && !song_over) {
        if (!macsop_tick(&player))
            song_over = 1;
        pos_order = player.order_idx; pos_row = player.row; rows_done++;
    }
    row_acc += SPR_Q16[song.speed];
}

#if OPL_CORE == OPL_NUKED
#include "opl3.c"
static opl3_chip chip __attribute__((section(".pool")));       /* 21 KB: too big for .bss, lives in the pool */
#include "opl2fast.c"
#define SCOPE_N 512u                          /* no taps in the Nuked path: the scope screen shows flat lines */
static volatile uint32_t scope_on;
static int16_t scope[9][SCOPE_N];
static uint32_t scope_w;
static void chip_drain(void)                  /* queued writes -> the chip */
{
    while (wq_r != wq_w) { uint32_t i = wq_r & (WQ_N - 1u); OPL3_WriteReg(&chip, wq_reg[i], wq_val[i]); wq_r++; }
}
static void chip_reset(void)
{
    uint32_t k;
    wq_r = wq_w;
    OPL3_Reset(&chip, 49716);
    OPL3_WriteReg(&chip, 0x105, 0);           /* an OPL2: no OPL3 features */
    for (k = 0; k < 4096u; k++)               /* drain the write buffer */
        opl2_generate(&chip);
}
static void chip_mute(uint32_t mask) { (void)mask; }        /* no mute in the Nuked path */
static int32_t next_native(void)
{
    if (row_acc < (1u << 16)) {
        tick_row();
        chip_drain();
    }
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
static void chip_drain(void)                  /* queued writes -> the chip */
{
    while (wq_r != wq_w) { uint32_t i = wq_r & (WQ_N - 1u); dbopl2_write(&chip, wq_reg[i], wq_val[i]); wq_r++; }
}
static void chip_reset(void) { wq_r = wq_w; dbopl2_reset(&chip); nat_rd = nat_n = 0; }
static void chip_mute(uint32_t mask) { chip.mute = mask; }   /* bit n: channel n out of the mix */
static void refill(void)                      /* the next block: up to the row due next, at most NAT_CHUNK */
{
    uint32_t k, i;
    if (row_acc < (1u << 16))
        tick_row();
    chip_drain();
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
    if (!chip_run)
        return;                               /* silence; main is loading a song into the chip */
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
        if (++over_n >= 43u) { playing = 0; chip_run = 0; overload = 1; over_n = 0; }
    } else
        over_n = 0;
}

/* ---- songs: the 18 built in (songs.h), then the flash slots */
#define N_SLOTS 9u
#define SLOT_BASE 0x98000u                    /* Felucca-family data area; 9 x 32 KiB, below the OTA staging at 0xE0000 */
#define SLOT_SIZE 0x8000u
#define SLOT_MAGIC 0x534C504Fu                /* "OPLS" */
#define SLOT_HDR 16u
#define N_ALL (N_SONGS + N_SLOTS)
static uint32_t cur;                          /* the song in the player: 0.. N_SONGS-1 built in, then slots */
static uint8_t slot_buf[SLOT_SIZE] __attribute__((section(".pool")));   /* a song's bytes on the way to flash */
#ifdef DINGHY_HOST
static uint8_t host_flash[N_SLOTS][SLOT_SIZE];
static const uint8_t *slot_ptr(uint32_t i) { return host_flash[i]; }
static int slot_program(uint32_t i, const uint8_t *src, uint32_t n)
{
    uint32_t k;
    char name[32] = "build/host/slot0.cmf";
    FILE *f;
    for (k = 0; k < SLOT_SIZE; k++) host_flash[i][k] = k < n ? src[k] : 0xFF;
    name[15] = (char)('1' + i);
    if ((f = fopen(name, "wb"))) { fwrite(src + SLOT_HDR, 1, n - SLOT_HDR, f); fclose(f); printf("%6u ms  slot %u -> %s (%u B)\n", fm1_ms, i + 1u, name, n - SLOT_HDR); }
    return 0;
}
#else
static const uint8_t *slot_ptr(uint32_t i) { return fm1_xip_ptr(SLOT_BASE + i * SLOT_SIZE); }
static int slot_program(uint32_t i, const uint8_t *src, uint32_t n)
{
    uint32_t off = SLOT_BASE + i * SLOT_SIZE, s, took;
    int rc;
    if (!flash_ok) return -1;
    for (s = 0; s < SLOT_SIZE; s += 0x1000u)
        if ((rc = fl_erase4k(off + s, &took)) != 0) return rc;
    if ((rc = fl_write(off + SLOT_HDR, src + SLOT_HDR, n - SLOT_HDR)) != 0) return rc;
    if ((rc = fl_write(off, src, SLOT_HDR)) != 0) return rc;             /* the header last: a cut save stays empty */
    fl_inval(off, SLOT_SIZE);                                            /* the XIP cache: read what was written */
    return 0;
}
#endif

#ifdef DINGHY_HOST
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) { c ^= *p++; for (k = 0; k < 8u; k++) c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1; }
    return ~c;
}
#endif
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static uint32_t slot_len(uint32_t i)          /* the song's bytes in slot i, 0 = empty / bad */
{
    const uint8_t *p = slot_ptr(i);
    uint32_t n;
    if (rd32(p) != SLOT_MAGIC) return 0;
    n = rd32(p + 4);
    if (n < 5 + 198 + 8 || n > SLOT_SIZE - SLOT_HDR || crc32(p + SLOT_HDR, n) != rd32(p + 8)) return 0;
    return n;
}

static int slot_save(uint32_t i)              /* the song -> slot i; 0 = ok */
{
    uint32_t n = macsop_write(&song, slot_buf + SLOT_HDR, SLOT_SIZE - SLOT_HDR);
    if (!n) return -2;
    wr32(slot_buf, SLOT_MAGIC); wr32(slot_buf + 4, n); wr32(slot_buf + 8, crc32(slot_buf + SLOT_HDR, n)); wr32(slot_buf + 12, 0);
    return slot_program(i, slot_buf, n + SLOT_HDR);
}

static const char *song_title(uint32_t i)
{
    static char t[12];
    if (i < N_SONGS) return SONGS[i].title;
    t[0] = 'S'; t[1] = 'L'; t[2] = 'O'; t[3] = 'T'; t[4] = ' '; t[5] = (char)('1' + (i - N_SONGS)); t[6] = 0;
    return t;
}
static const char *song_by(uint32_t i)
{
    if (i < N_SONGS) return SONGS[i].by;
    return slot_len(i - N_SONGS) ? "YOUR SONG, IN FLASH" : "EMPTY: A NEW SONG";
}

static uint32_t mute_mask;                    /* bit n: channel n muted (the scope's white keys) */

static void chip_silence(void)                /* every key off, so a paused song does not hang a note */
{
    uint32_t ch;
    for (ch = 0; ch < 9u; ch++)
        opl_write(0, (uint16_t)(0xB0 + ch), (uint8_t)(player.bx[ch] & ~0x20u));
    opl_write(0, 0xBD, (uint8_t)(player.bd & 0x20u));
}

static void load_song(uint32_t i)             /* with the ISR off the chip (chip_run == 0) */
{
    cur = i;
    chip_reset();
    chip_mute(mute_mask);                     /* the reset cleared the chip's copy: a restart keeps the mutes */
    if (i < N_SONGS)
        macsop_parse(&song, SONG_DATA + SONGS[i].off, SONGS[i].len);
    else if (slot_len(i - N_SONGS))
        macsop_parse(&song, slot_ptr(i - N_SONGS) + SLOT_HDR, slot_len(i - N_SONGS));
    else
        macsop_song_init(&song);
    macsop_attach(&player, &song, opl_write, 0);
    chip_drain();                             /* the rewind's register writes land */
    rs_phase = 0; row_acc = 0; s_prev = s_cur = 0;
    song_over = 0; pos_order = 0; pos_row = 0; rows_done = 0;
}

static void reload_song(void)                 /* the chip from scratch, the song as it is in RAM (edits kept) */
{
    chip_reset();
    chip_mute(mute_mask);
    macsop_rewind(&player);
    chip_drain();
    rs_phase = 0; row_acc = 0; s_prev = s_cur = 0;
    song_over = 0; pos_order = 0; pos_row = 0; rows_done = 0;
}

static void chip_off(void)                    /* main takes the chip: the ISR that may be running finishes */
{
    chip_run = 0; playing = 0;
    fm1_delay_ms(7);
}

static void pause_song(void)                  /* rows stop, the chip keeps running (releases, auditions) */
{
    playing = 0;
    fm1_delay_ms(7);
    chip_silence();
}

/* ---- the screen */
#define C_ACC RGB(255, 170, 60)               /* amber, like a banker's lamp */
#define C_MUTE RGB(110, 110, 110)
#define C_TXT RGB(220, 220, 220)
#define C_RED RGB(255, 60, 60)
#define C_CURBG RGB(24, 40, 80)               /* the editor's cursor row */
#define C_ROWBG RGB(52, 36, 10)               /* the playing row's bar */

static void u2s(char *s, uint32_t v, uint32_t digits)   /* decimal, fixed width, no printf on the chip */
{
    uint32_t i;
    for (i = digits; i-- > 0;) { s[i] = (char)('0' + v % 10u); v /= 10u; }
    s[digits] = 0;
}
static char *cat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *catn(char *d, uint32_t v, uint32_t digits) { u2s(d, v, digits); return d + digits; }

static void draw_title(uint32_t i)
{
    char s[24];
    uint32_t k = 0;
    const char *t = song_title(i);
    lcd_fill(0, 36, 240, 40, C_BLACK);
    if (text_w(&FONT_L, t) <= 240)
        draw_text_box(0, 42, 240, &FONT_L, t, C_ACC, 1);
    else                                      /* a long title: the small font, same place */
        draw_text_box(0, 50, 240, &FONT_S, t, C_ACC, 1);
    draw_text_box(0, 78, 240, &FONT_S, song_by(i), C_MUTE, 1);
    s[k++] = (char)('0' + (i + 1u) / 10u); s[k++] = (char)('0' + (i + 1u) % 10u); s[k++] = '/';
    s[k++] = (char)('0' + N_ALL / 10u); s[k++] = (char)('0' + N_ALL % 10u); s[k++] = ' '; s[k++] = ' ';
    if (i < N_SONGS) { const char *p = SONGS[i].file; while (*p && k < 22u) s[k++] = *p++; }
    else { s[k++] = 'S'; s[k++] = 'L'; s[k++] = 'O'; s[k++] = 'T'; s[k++] = (char)('1' + (i - N_SONGS)); s[k++] = '.'; s[k++] = 'C'; s[k++] = 'M'; s[k++] = 'F'; }
    s[k] = 0;
    draw_text_box(0, 98, 240, &FONT_S, s, C_MUTE, 1);
}

static void draw_instruments(void)          /* the first six instrument names, two columns */
{
    uint32_t i;
    cv_begin(240, 56, C_BLACK);
    for (i = 0; i < 6u && i < song.n_instr; i++)
        cv_text((i & 1u) ? 124 : 4, (int32_t)((i >> 1) * 18u), &FONT_S, macsop_instr_name(&player, (int)i), C_MUTE);
    cv_blit(0, 124);
}

static uint32_t pct_colour(uint32_t us)      /* the load figure: amber past 85 %, red when it does not fit */
{
    return us > 5805u ? C_RED : us > 4934u ? C_ACC : C_MUTE;
}

/* ---- the editor's state */
static uint32_t edit;                         /* EDIT: the tracker screen follows the cursor; keys write notes */
static uint32_t cur_order, cur_row, cur_col;  /* the cursor */
static uint32_t ed_instr = 1, ed_vol = 127, ed_step = 1;
static int32_t kb_oct;                        /* keyboard octave shift, -3..3 */
static uint32_t ins_sel;                      /* the instrument editor's instrument, 0.. */
static uint32_t ins_param, ins_namepos;       /* its cursor: parameter row; character in the name */
static uint32_t song_pos;                     /* the song editor's cursor: order position */
static const char *msg; static uint32_t msg_until;   /* a line in the status area, for a moment */

static void say(const char *m) { msg = m; msg_until = fm1_ms + 2000u; }

static void draw_status(int play, int loop_all)
{
    char s[40], *p;
    cv_begin(240, 40, C_BLACK);
    cv_text(4, 0, &FONT_S, play ? "PLAY " : "PAUSE", play ? C_ACC : C_MUTE);
    p = catn(s, pos_order + 1u, 2); *p++ = '/'; p = catn(p, song.n_orders, 2); *p++ = ' '; *p++ = ' ';
    p = catn(p, pos_row, 2);
    cv_text(70, 0, &FONT_S, s, C_TXT);
    cv_text(150, 0, &FONT_S, song.speed == 1 ? "18/S" : song.speed == 2 ? "9/S" : "4.5/S", C_MUTE);
    cv_text(200, 0, &FONT_S, song.rhythm ? "RHY" : "MEL", C_MUTE);
    /* second line: a message, the mode, the audio load */
    if (msg && (int32_t)(msg_until - fm1_ms) > 0)
        cv_text(4, 20, &FONT_S, msg, C_ACC);
    else if (edit) {
        p = cat(s, "OCT"); *p++ = (char)('3' + kb_oct); *p++ = ' '; p = cat(p, "STEP"); p = catn(p, ed_step, 1);
        *p++ = ' '; p = cat(p, "I"); p = catn(p, ed_instr, 2);
        cv_text(4, 20, &FONT_S, s, C_MUTE);
    } else
        cv_text(4, 20, &FONT_S, loop_all ? "ALL IN TURN" : "LOOP SONG", C_MUTE);
    {   uint32_t pct = (audio_us * 100u) / 5805u;         /* of the 5.8 ms an interrupt has */
        if (pct > 999u) pct = 999u;
        p = cat(s, "CPU ");
        if (pct >= 100u) p = catn(p, pct, 3); else if (pct >= 10u) p = catn(p, pct, 2); else p = catn(p, pct, 1);
        *p++ = '%'; *p = 0;
    }
    cv_text(160, 20, &FONT_S, s, pct_colour(audio_us));
    cv_blit(0, 190);
}

/* ---- the tracker screen: the current pattern as a tracker shows it, the playing row (or, editing, the
 * cursor) in the middle. Notes as "C4" / "c4" (lowercase = sharp), 9 columns 24 px apart for a melodic song,
 * 11 (6 + BD SD TM TC HH) 20 px apart for a rhythm song: 240 px is 30 characters. */
#define TR_TOP  38u                            /* first pattern row on screen */
#define TR_ROWS 9u                             /* rows shown: 4 before, the one in focus, 4 after */
#define TR_CUR  4u

static void note_text(char *s, uint8_t n)     /* "c4" / ".." / "==" / "EN" / "??" */
{
    static const char NAMES[12] = "CCDDEFFGGAAB", SHARP[12] = " # #  # # # ";
    const char *fixed = n == 0 ? ".." : n == 4 ? "==" : n == 1 ? "EN" : (n < 24 || n > 119) ? "??" : 0;
    if (fixed) { s[0] = fixed[0]; s[1] = fixed[1]; }
    else {
        uint32_t i = n % 12u, oct = n / 12u - 2u;
        s[0] = SHARP[i] == ' ' ? NAMES[i] : (char)(NAMES[i] + 32); s[1] = (char)('0' + oct);
    }
    s[2] = 0;
}

static uint32_t tr_ncol(void) { return song.rhythm ? MACSOP_CHANNELS : 9u; }
static uint32_t tr_cw(void) { return tr_ncol() <= 9u ? 24u : 20u; }

static void draw_tracker_head(void)           /* the title (or, editing, the cursor cell) and the column labels */
{
    static const char *const DRUMS[5] = { "BD", "SD", "TM", "TC", "HH" };
    uint32_t ncol = tr_ncol(), cw = tr_cw(), c;
    char s[40], *p;
    lcd_fill(0, 0, 240, TR_TOP, C_BLACK);
    if (edit) {                               /* "03/14 P07 R12  c4 I05 V127 P32" */
        uint16_t pat = macsop_order_pattern(&player, (uint16_t)cur_order);
        const macsop_cell *k = pat == 0xFFFF ? 0 : &song.pat[pat].cell[cur_row][cur_col];
        p = catn(s, cur_order + 1u, 2); *p++ = '/'; p = catn(p, song.n_orders, 2); *p++ = ' ';
        *p++ = 'P'; p = catn(p, pat == 0xFFFF ? 0u : pat, 2); *p++ = ' '; *p++ = ' ';
        if (k && k->f.note) {
            note_text(p, k->f.note); p += 2; *p++ = ' ';
            *p++ = 'I'; p = catn(p, k->f.instr, 2); *p++ = ' ';
            *p++ = 'V'; p = catn(p, k->f.vol, 3); *p++ = ' ';
            *p++ = 'B'; p = catn(p, k->f.pitch, 2);
        } else
            p = cat(p, "..");
        draw_text_box(0, 2, 240, &FONT_S, s, C_ACC, 1);
    } else
        draw_text_box(0, 2, 240, &FONT_S, song_title(cur), C_ACC, 1);
    cv_begin(240, 16, C_BLACK);
    for (c = 0; c < ncol; c++) {
        const char *l;
        if (c < 6u || ncol <= 9u) { s[0] = (char)('1' + c); s[1] = 0; l = s; }
        else l = DRUMS[c - 6u];
        cv_text((int32_t)(20u + c * cw + (l[1] ? 0u : 4u)), 0, &FONT_S, l, edit && c == cur_col ? C_ACC : C_MUTE);
    }
    cv_blit(0, 20);
}

static void draw_tracker(void)                /* the rows around the one in focus, three rows a canvas (CV_MAX) */
{
    uint32_t ncol = tr_ncol(), cw = tr_cw();
    uint32_t focus = edit ? cur_row : pos_row, band, i, c, prow = pos_row;
    uint16_t pat = macsop_order_pattern(&player, (uint16_t)(edit ? cur_order : pos_order));
    const macsop_pattern *pp = pat == 0xFFFF ? 0 : &song.pat[pat];
    int same = !edit || cur_order == pos_order;
    char s[4];
    for (band = 0; band < TR_ROWS / 3u; band++) {
        uint32_t r0 = band * 3u;
        cv_begin(240, 48, C_BLACK);
        for (i = 0; i < 3u; i++) {
            int32_t r = (int32_t)focus - (int32_t)TR_CUR + (int32_t)(r0 + i);
            int32_t y = (int32_t)(i * 16u);
            int cur_line = r0 + i == TR_CUR, play_line = same && playing && r == (int32_t)prow;
            if (cur_line) cv_rect(0, y, 240, 16, edit ? C_CURBG : C_ROWBG);
            if (r < 0 || r >= (int32_t)MACSOP_ROWS) continue;
            u2s(s, (uint32_t)r, 2);
            cv_text(2, y, &FONT_S, s, play_line ? C_ACC : cur_line ? C_TXT : C_MUTE);
            for (c = 0; c < ncol; c++) {
                uint8_t n = pp ? pp->cell[r][c].f.note : 0;
                int at = edit && cur_line && c == cur_col;
                note_text(s, n);
                if (at) cv_rect((int32_t)(18u + c * cw), y, 20, 16, C_ACC);
                cv_text((int32_t)(20u + c * cw), y, &FONT_S, s,
                        at ? C_BLACK : n ? (cur_line && !edit ? C_ACC : C_TXT) : C_MUTE);
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
#define C_MUTED_TRACE RGB(110, 40, 40)
static const uint8_t WHITE_KEY[9] = { 0, 2, 4, 6, 7, 9, 11, 12, 14 };   /* F3 G3 A3 B3 C4 D4 E4 F4 G4 as key numbers */

static void draw_scope_frame(void)
{
    lcd_fill(0, 0, 240, SC_TOP, C_BLACK);
    draw_text_box(0, 2, 240, &FONT_S, song_title(cur), C_ACC, 1);
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
            cv_text((int32_t)x0 + 3, 0, &FONT_S, name, muted ? C_RED : lit ? C_TXT : C_MUTE);
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

/* ---- the song screen: the order list, one position a line, the cursor in the middle */
static void draw_song_frame(void)
{
    char s[40], *p;
    lcd_fill(0, 0, 240, TR_TOP, C_BLACK);
    p = cat(s, "SONG  SPD"); p = catn(p, song.speed, 1); p = cat(p, song.rhythm ? "  RHY ON" : "  RHY OFF");
    draw_text_box(0, 2, 240, &FONT_S, s, C_ACC, 1);
    p = cat(s, "POSITIONS "); p = catn(p, song.n_orders, 2); p = cat(p, "  PATTERNS "); p = catn(p, song.n_patterns, 2);
    draw_text_box(0, 20, 240, &FONT_S, s, C_MUTE, 1);
}

static void draw_song(void)
{
    uint32_t band, i;
    char s[40], *p;
    for (band = 0; band < TR_ROWS / 3u; band++) {
        uint32_t r0 = band * 3u;
        cv_begin(240, 48, C_BLACK);
        for (i = 0; i < 3u; i++) {
            int32_t r = (int32_t)song_pos - (int32_t)TR_CUR + (int32_t)(r0 + i);
            int32_t y = (int32_t)(i * 16u);
            int cur_line = r0 + i == TR_CUR;
            if (cur_line) cv_rect(0, y, 240, 16, C_CURBG);
            if (r < 0 || r >= (int32_t)song.n_orders) continue;
            p = cat(s, "POS "); p = catn(p, (uint32_t)r + 1u, 2); p = cat(p, "    PATTERN "); p = catn(p, song.order[r], 2);
            cv_text(20, y, &FONT_S, s, playing && (uint32_t)r == pos_order ? C_ACC : cur_line ? C_TXT : C_MUTE);
        }
        cv_blit(0, TR_TOP + r0 * 16u);
    }
}

/* ---- the instrument screen: every parameter of one instrument as a list, the name first */
#define INS_PARAMS 27u                         /* name + 2 x 12 operator fields + feedback + connection */
static const char *const INS_NAME[12] = { "AR", "DR", "SL", "RR", "TL", "KSL", "MULT", "AM", "VIB", "EG", "KSR", "WAVE" };
static const uint8_t INS_MAX[12] = { 15, 15, 15, 15, 63, 3, 15, 1, 1, 1, 1, 3 };

static uint8_t *ins_field(macsop_instr *in, uint32_t param)   /* params 1..24: op 1 then op 2 fields; 25 FB; 26 CON */
{
    macsop_slot *o;
    uint32_t f;
    if (param == 25u) return &in->feedback;
    if (param == 26u) return &in->connection;
    o = &in->op[(param - 1u) / 12u]; f = (param - 1u) % 12u;
    switch (f) {
    case 0: return &o->ar;  case 1: return &o->dr;  case 2: return &o->sl;  case 3: return &o->rr;
    case 4: return &o->tl;  case 5: return &o->ksl; case 6: return &o->mult; case 7: return &o->am;
    case 8: return &o->vib; case 9: return &o->eg;  case 10: return &o->ksr; default: return &o->wave;
    }
}
static uint32_t ins_max(uint32_t param) { return param == 25u ? 7u : param == 26u ? 1u : INS_MAX[(param - 1u) % 12u]; }

static void draw_ins_frame(void)
{
    char s[40], *p;
    lcd_fill(0, 0, 240, TR_TOP, C_BLACK);
    p = cat(s, "INSTRUMENT "); p = catn(p, ins_sel + 1u, 2); *p++ = '/'; p = catn(p, song.n_instr, 2);
    draw_text_box(0, 2, 240, &FONT_S, s, C_ACC, 1);
    draw_text_box(0, 20, 240, &FONT_S, "KEYS PLAY  K1 VALUE  SEL ROW", C_MUTE, 1);
}

static void draw_ins(void)
{
    uint32_t band, i;
    macsop_instr *in = &song.instr[ins_sel];
    char s[40], *p;
    for (band = 0; band < TR_ROWS / 3u; band++) {
        uint32_t r0 = band * 3u;
        cv_begin(240, 48, C_BLACK);
        for (i = 0; i < 3u; i++) {
            int32_t r = (int32_t)ins_param - (int32_t)TR_CUR + (int32_t)(r0 + i);
            int32_t y = (int32_t)(i * 16u);
            int cur_line = r0 + i == TR_CUR;
            if (cur_line) cv_rect(0, y, 240, 16, C_CURBG);
            if (r < 0 || r >= (int32_t)INS_PARAMS) continue;
            if (r == 0) {
                uint32_t k;
                p = cat(s, "NAME  ");
                cv_text(20, y, &FONT_S, s, cur_line ? C_TXT : C_MUTE);
                for (k = 0; k < 13u; k++) {
                    char ch[2] = { in->name[k] ? in->name[k] : ' ', 0 };
                    int at = cur_line && k == ins_namepos;
                    if (at) cv_rect((int32_t)(68u + k * 12u), y, 12, 16, C_ACC);
                    cv_text((int32_t)(70u + k * 12u), y, &FONT_S, ch, at ? C_BLACK : C_TXT);
                }
            } else {
                uint32_t v = *ins_field(in, (uint32_t)r);
                if (r <= 24) { p = cat(s, "OP"); *p++ = (char)('1' + (r - 1) / 12); *p++ = ' '; *p++ = ' '; p = cat(p, INS_NAME[(r - 1) % 12]); }
                else p = cat(s, r == 25 ? "FEEDBACK" : "CONNECT");
                cv_text(20, y, &FONT_S, s, cur_line ? C_TXT : C_MUTE);
                if (r == 26) p = cat(s, v ? "ADD" : "FM");
                else if (r <= 24 && (r - 1) % 12 == 11) p = cat(s, v == 0 ? "SINE" : v == 1 ? "HALF" : v == 2 ? "ABS" : "PULSE");
                else p = catn(s, v, 2);
                cv_text(170, y, &FONT_S, s, cur_line ? C_ACC : C_MUTE);
            }
        }
        cv_blit(0, TR_TOP + r0 * 16u);
    }
}

/* ---- views */
enum { V_JUKE, V_TRACK, V_SCOPE, V_SONG, V_INS };
static uint32_t view;

static void draw_view_frame(void)
{
    lcd_fill(0, 0, 240, 190, C_BLACK);
    scope_on = view == V_SCOPE;
    switch (view) {
    case V_TRACK: draw_tracker_head(); draw_tracker(); break;
    case V_SCOPE: draw_scope_frame(); break;
    case V_SONG:  draw_song_frame(); draw_song(); break;
    case V_INS:   draw_ins_frame(); draw_ins(); break;
    default:
        draw_text_box(0, 6, 240, &FONT_S, "OPLBOX " APP_VER " - MACS OPERA", C_MUTE, 1);
        draw_title(cur); draw_instruments();
    }
}

/* ---- editing helpers */
static macsop_cell *cursor_cell(void)
{
    uint16_t pat = macsop_order_pattern(&player, (uint16_t)cur_order);
    return pat == 0xFFFF ? 0 : &song.pat[pat].cell[cur_row][cur_col];
}
static void cursor_step(void) { cur_row = (cur_row + ed_step) % MACSOP_ROWS; }
static uint32_t key_note(uint32_t key) { int32_t n = 65 + (int32_t)key + 12 * kb_oct; return n < 24 ? 24u : n > 119 ? 119u : (uint32_t)n; }
static int32_t wrap(int32_t v, int32_t n) { return ((v % n) + n) % n; }

static void audition(int ch, uint32_t note, uint32_t instr)   /* a key held: that note on that channel, now */
{
    if (!playing) macsop_play_note(&player, ch, (int)note, (int)instr, (int)ed_vol);
}

static void save_song(void)
{
    uint32_t slot = cur >= N_SONGS ? cur - N_SONGS : N_SLOTS, i;
    if (slot == N_SLOTS)                      /* a built-in song: the first free slot */
        for (i = 0; i < N_SLOTS; i++) if (!slot_len(i)) { slot = i; break; }
    if (slot == N_SLOTS) { say("NO FREE SLOT"); return; }
    chip_off();
    if (slot_save(slot) == 0) { cur = N_SONGS + slot; say("SAVED TO SLOT"); }
    else say("SAVE FAILED");
    chip_run = 1;
}

static void app_main(void)
{
    struct { uint8_t song, loop_all; } set = {0, 0};
    uint32_t shown_order = ~0u, shown_row = ~0u, t_status = 0, t_change = 0, dirty = 0, play = 0, next = 0;
    uint32_t t_scope = 0, keys_prev = 0, redraw = 0, head_dirty = 0, held_ch = 0;
    int32_t spin, k1, k2, k3, k4, alg, pre;
    settings_load(&set, sizeof set);
    if (set.song >= N_ALL) set.song = 0;
    set.loop_all &= 1u;

    load_song(set.song);
    draw_view_frame();
    play = 1; playing = 1; chip_run = 1;      /* a jukebox starts playing */

    for (;;) {
        uint32_t b, rel, keys, down, up, k;
        platform_poll();
        {   uint8_t st, d1, d2;
            while (midi_get(&st, &d1, &d2)) { }   /* drain MIDI in: a full ring NAKs the USB endpoint, locking SysEx out */
        }
        b = fm1_input_edges(&rel);
        keys = fm1_in.notes; down = keys & ~keys_prev; up = keys_prev & ~keys; keys_prev = keys;
        spin = fm1_enc_take(ENC_SELECT); k1 = fm1_enc_take(ENC_KNOB1); k2 = fm1_enc_take(ENC_KNOB2);
        k3 = fm1_enc_take(ENC_KNOB3); k4 = fm1_enc_take(ENC_KNOB4); alg = fm1_enc_take(ENC_ALGORITHM); pre = fm1_enc_take(ENC_PRESETS);

        if (overload && play) {               /* the ISR stopped itself: show it as paused, PLAY tries again */
            play = 0; chip_silence(); shown_order = ~0u;
            draw_text_box(0, view == V_JUKE ? 78u : 2u, 240, &FONT_S, "AUDIO OVERLOAD - PAUSED", C_RED, 1);
        }
        if (song_over) {                      /* the ISR stopped at the end */
            next = set.loop_all && !edit ? 1 : 0;
            if (!next) { chip_off(); reload_song(); chip_run = 1; playing = play ? 1u : 0u; }
        }

        /* ---- the views and the modes */
        if (b & (1u << BTN_EDIT)) {
            edit ^= 1u;
            if (edit) { cur_order = pos_order; cur_row = pos_row; cur_col = 0; song_pos = cur_order; if (view != V_SONG && view != V_INS) view = V_TRACK; }
            else if (view == V_SONG || view == V_INS) view = V_TRACK;
            redraw = 1;
        }
        if (b & (1u << BTN_SEQ)) {
            if (edit) view = view == V_TRACK ? V_SONG : view == V_SONG ? V_INS : V_TRACK;
            else view = view == V_TRACK ? V_JUKE : V_TRACK;
            redraw = 1;
        }
        if ((b & (1u << BTN_FX)) && !edit) { view = view == V_SCOPE ? V_JUKE : V_SCOPE; redraw = 1; }

        /* ---- song choice (not while editing: the knobs are the cursor there) */
        if (!edit) {
            if (b & (1u << BTN_OCT_UP)) spin++;
            if (b & (1u << BTN_OCT_DOWN)) spin--;
        }
        if (edit && (b & ((1u << BTN_OCT_UP) | (1u << BTN_OCT_DOWN)))) {
            kb_oct += (b & (1u << BTN_OCT_UP)) ? 1 : -1;
            if (kb_oct > 3) kb_oct = 3;
            if (kb_oct < -3) kb_oct = -3;
            shown_order = ~0u;
        }
        if ((spin && !edit) || next) {
            uint32_t i = next ? (cur + 1u) % N_ALL : (uint32_t)wrap((int32_t)cur + spin, (int32_t)N_ALL);
            next = 0;
            chip_off();
            mute_mask = 0;                    /* a new song starts with every channel in */
            load_song(i);
            chip_run = 1; playing = play ? 1u : 0u;
            set.song = (uint8_t)cur; dirty = 1; t_change = fm1_ms;
            cur_order = cur_row = cur_col = 0; song_pos = 0; ins_sel = 0; ins_param = 0;
            redraw = 1;
        }
        if (b & (1u << BTN_PLAY)) {
            play ^= 1u;
            if (play) {
                overload = 0;
                if (edit && view != V_INS) {  /* from the cursor */
                    if (macsop_seek(&player, (uint16_t)(view == V_SONG ? song_pos : cur_order), (uint16_t)(view == V_SONG ? 0u : cur_row)))
                        song_over = 0;
                }
                chip_run = 1; playing = 1;
            } else
                pause_song();
            shown_order = ~0u;
        }
        if (b & (1u << BTN_HOME)) {
            if (edit) { cur_row = 0; if (view == V_SONG) song_pos = 0; }
            else { chip_off(); reload_song(); chip_run = 1; playing = play ? 1u : 0u; }
            shown_order = ~0u; redraw |= edit;
        }
        if ((b & (1u << BTN_REC)) && !edit) {
            set.loop_all ^= 1u; dirty = 1; t_change = fm1_ms; shown_order = ~0u;
        }
        if ((b & (1u << BTN_SAVE)) && edit) { save_song(); redraw = 1; }

        /* ---- the scope's mutes */
        if (view == V_SCOPE && down)
            for (k = 0; k < 9u; k++)
                if (down & (1u << WHITE_KEY[k])) { mute_mask ^= 1u << k; chip_mute(mute_mask); }

        /* ---- the pattern editor */
        if (edit && view == V_TRACK) {
            macsop_cell *c;
            uint32_t ncol = tr_ncol(), moved = 0;
            if (k1) { cur_col = (uint32_t)wrap((int32_t)cur_col + k1, (int32_t)ncol); moved = 1; }
            if (k2) { cur_row = (uint32_t)wrap((int32_t)cur_row + k2, (int32_t)MACSOP_ROWS); moved = 1; }
            if (spin) { cur_order = (uint32_t)wrap((int32_t)cur_order + spin, (int32_t)song.n_orders); moved = 1; }
            if (pre) { int32_t v = (int32_t)ed_step + pre; ed_step = (uint32_t)(v < 0 ? 0 : v > 16 ? 16 : v); moved = 1; }
            c = cursor_cell();
            if (c) {
                if (k3 && c->f.note >= 24u && c->f.note <= 119u) {
                    int32_t n = (int32_t)c->f.note + k3; macsop_cell t = *c;
                    t.f.note = (uint8_t)(n < 24 ? 24 : n > 119 ? 119 : n); c->u = t.u; moved = 1;
                    audition((int)cur_col, t.f.note, t.f.instr);
                }
                if (k4) { int32_t v = (int32_t)ed_instr + k4; ed_instr = (uint32_t)(v < 1 ? 1 : v > (int32_t)song.n_instr ? (int32_t)song.n_instr : v);
                          if (c->f.note >= 24u) { macsop_cell t = *c; t.f.instr = (uint8_t)ed_instr; c->u = t.u; } moved = 1; }
                if (alg) { int32_t v = (int32_t)ed_vol + alg * 4; ed_vol = (uint32_t)(v < 0 ? 0 : v > 127 ? 127 : v);
                           if (c->f.note >= 24u) { macsop_cell t = *c; t.f.vol = (uint8_t)ed_vol; c->u = t.u; } moved = 1; }
                if (down) {                   /* a key: the note into the cell, hear it, step on */
                    for (k = 0; k < 27u; k++)
                        if (down & (1u << k)) {
                            macsop_cell t = *c;
                            t.f.note = (uint8_t)key_note(k); t.f.instr = (uint8_t)ed_instr;
                            if (!c->f.note) { t.f.vol = (uint8_t)ed_vol; t.f.pitch = 32; }
                            c->u = t.u;
                            audition((int)cur_col, t.f.note, t.f.instr); held_ch = cur_col + 1u;
                            cursor_step(); moved = 1;
                            break;
                        }
                }
                if (b & (1u << BTN_FX))  { macsop_cell t; t.u = 0; t.f.note = 4; c->u = t.u; cursor_step(); moved = 1; }
                if (b & (1u << BTN_SEL)) { c->u = 0; cursor_step(); moved = 1; }
                if (b & (1u << BTN_GLO)) { macsop_cell t; t.u = 0; t.f.note = c->f.note == 1 ? 0 : 1; t.f.vol = 127; t.f.pitch = 32; c->u = t.u; moved = 1; }
            }
            if (up && held_ch) { audition((int)(held_ch - 1u), 0, 0); held_ch = 0; }
            if (moved) { draw_tracker_head(); draw_tracker(); shown_order = ~0u; }
        }

        /* ---- the song editor */
        if (edit && view == V_SONG) {
            uint32_t moved = 0;
            if (spin) { song_pos = (uint32_t)wrap((int32_t)song_pos + spin, (int32_t)song.n_orders); moved = 1; }
            if (k1 && song_pos < song.n_orders) { song.order[song_pos] = (uint16_t)wrap((int32_t)song.order[song_pos] + k1, (int32_t)song.n_patterns); moved = 1; }
            if (k2) { int32_t v = (int32_t)song.speed + k2; song.speed = (uint8_t)(v < 1 ? 1 : v > 3 ? 3 : v); moved = 1; }
            if (k3) { song.rhythm ^= 1u; moved = 1; }
            if ((b & (1u << BTN_GLO)) && song.n_orders < MACSOP_MAX_ORDERS) {   /* insert: a copy of this position after it */
                uint32_t i;
                for (i = song.n_orders; i > song_pos + 1u; i--) song.order[i] = song.order[i - 1u];
                song.order[song_pos + 1u] = song.order[song_pos]; song.n_orders++; song_pos++; moved = 1;
            }
            if ((b & (1u << BTN_SEL)) && song.n_orders > 1u) {                   /* delete this position */
                uint32_t i;
                for (i = song_pos; i + 1u < song.n_orders; i++) song.order[i] = song.order[i + 1u];
                song.n_orders--; if (song_pos >= song.n_orders) song_pos = song.n_orders - 1u; moved = 1;
            }
            if ((b & (1u << BTN_ARP)) && song.n_patterns < MACSOP_MAX_PATTERNS) {   /* a new empty pattern, here */
                uint32_t r, cc;
                for (r = 0; r <= MACSOP_ROWS; r++) for (cc = 0; cc < MACSOP_CHANNELS; cc++) song.pat[song.n_patterns].cell[r][cc].u = 0;
                song.order[song_pos] = song.n_patterns++; moved = 1;
            }
            if (moved) { cur_order = song_pos; draw_song_frame(); draw_song(); shown_order = ~0u; }
        }

        /* ---- the instrument editor */
        if (edit && view == V_INS) {
            macsop_instr *in = &song.instr[ins_sel];
            uint32_t moved = 0;
            if (pre) { ins_sel = (uint32_t)wrap((int32_t)ins_sel + pre, (int32_t)song.n_instr); in = &song.instr[ins_sel]; moved = 1; }
            if (spin) { ins_param = (uint32_t)wrap((int32_t)ins_param + spin, (int32_t)INS_PARAMS); moved = 1; }
            if (ins_param == 0) {
                if (k1) { ins_namepos = (uint32_t)wrap((int32_t)ins_namepos + k1, 13); moved = 1; }
                if (k2) { int32_t ch = in->name[ins_namepos] ? in->name[ins_namepos] : ' '; ch += k2; if (ch < 32) ch = 32; if (ch > 126) ch = 126; in->name[ins_namepos] = (char)ch; moved = 1; }
            } else if (k1) {
                uint8_t *f = ins_field(in, ins_param);
                int32_t v = (int32_t)*f + k1, mx = (int32_t)ins_max(ins_param);
                *f = (uint8_t)(v < 0 ? 0 : v > mx ? mx : v);
                macsop_refresh_instr(&player, (int)ins_sel);   /* channels playing it hear the change */
                moved = 1;
            }
            if ((b & (1u << BTN_ARP)) && song.n_instr < MACSOP_MAX_INSTR) {   /* a new instrument: a copy of this one */
                song.instr[song.n_instr] = *in; ins_sel = song.n_instr++; moved = 1;
            }
            if (down)                         /* audition on channel 1 */
                for (k = 0; k < 27u; k++)
                    if (down & (1u << k)) { audition(0, key_note(k), ins_sel + 1u); held_ch = 1u; break; }
            if (up && held_ch) { audition(0, 0, 0); held_ch = 0; }
            if (moved) { draw_ins_frame(); draw_ins(); }
        }

        /* ---- LEDs and the screen */
        fm1_led_key(BTN_PLAY, play);
        fm1_led_key(BTN_REC, set.loop_all && !edit);
        fm1_led_key(BTN_EDIT, (int)edit);
        fm1_led_key(BTN_SEQ, view == V_TRACK || view == V_SONG || view == V_INS);
        fm1_led_key(BTN_FX, view == V_SCOPE);
        for (k = 0; k < 9u; k++) fm1_led_key(14u + WHITE_KEY[k], view == V_SCOPE && (int)((mute_mask >> k) & 1u));
        if (redraw) { draw_view_frame(); redraw = 0; shown_order = ~0u; }
        if (view == V_SCOPE && fm1_ms - t_scope > 50u) {          /* the scope: 20 frames a second */
            t_scope = fm1_ms;
            draw_scope();
        }
        if (pos_order != shown_order || pos_row != shown_row || fm1_ms - t_status > 250u) {
            int moved = pos_order != shown_order || pos_row != shown_row;
            shown_order = pos_order; shown_row = pos_row; t_status = fm1_ms;
            if (view == V_TRACK && moved) { if (edit) head_dirty = 1; draw_tracker(); }
            if (view == V_SONG && moved) draw_song();
            if (head_dirty) { head_dirty = 0; if (edit && view == V_TRACK) draw_tracker_head(); }
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
