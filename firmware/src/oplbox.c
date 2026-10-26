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
 *   OCT- OCT+      the keys' octave, OCT0..OCT6 (the arp's too, as it plays; the status line shows it a moment)
 *   SELECT         turn: pick a song (18 built in, then 9 flash slots)
 *   REC            loop one song <-> play them all in turn (the LED says which)
 *   GLO            the battery's raw ADC count in the status line, for a moment (the icon at the top right)
 *   SAVE           the WiFi test page (docs/WIFI.md stages 2-3): PLAY there brings the radio up and reads it, SAVE leaves
 *   scope: the first nine white keys (F3..G4) mute / unmute channels 1..9 (key LED = muted)
 *   ARP            the arpeggiator on (its settings open) / off  LFO   the arp's settings: open, or the next page
 *   PRESETS        the instrument the keys / the arp play (one of the song's; the editor's I too)
 *   ALGO           its connection and feedback, FM 0-7 then additive 0-7, on a copy the song never hears (see below)
 * Arpeggiator (ARP on, outside EDIT): held keys play an arpeggio instead of nothing, on a channel the song leaves
 * free (see "the arpeggiator"); not from the scope screen, whose keys stay the mutes; EDIT gives the keys back
 * to the editor. Its settings screen (LFO turns the page; SEQ / FX leave it, the arp plays on; its last line names
 * the keys' instrument and octave, so PRESETS / ALGO / OCT say nothing there, and the status line shows the song's title):
 *   page 1:        K1 mode (up, down, inclusive, exclusive, random, order, repeat)  K2 octaves 1-4
 *                  K3 rate (1/4 1/8 1/8T 1/16 1/32)  K4 tempo 30-300 BPM, or SYNC (left of 30): the song's rows,
 *                  one a 1/16, the steps on its row clock, playing or stopped (see "the arpeggiator")
 *   page 2:        K1 gate 20-100 %  K2 swing 50-75 %  K3 latch (right: on, left: off)
 *                  K4 length ARP writes into the tracker, 1-8 patterns (not kept)
 * Editing (EDIT, LED lit; the tracker screen follows the cursor, not the playback):
 *   K1 / K2        cursor column / row          SELECT   order position
 *   keys           enter the note (and audition it), then step; OCT- OCT+ keyboard octave
 *   K3             note +- semitone             K4       instrument    ALGO  volume    PRESETS  step
 *   FX             key off (==)                 SCL      clear the cell        GLO   end of pattern here
 *   PLAY           play from the cursor / pause SAVE     save the song to its flash slot (or the first free one)
 *   ENV            undo one edit (32 steps, no redo)    REC   arm recording: keys during playback land on the playing row
 *   ARP            write the arp's last chord into the column, as notes, from the cursor down (see "the arp into the tracker";
 *                  with SYNC, on the rows the synced arp plays it)
 *   song screen:   SELECT position, K1 pattern number, GLO insert, SCL delete, ARP new pattern, K2 speed, K3 rhythm
 *   instrument:    PRESETS instrument, SELECT parameter, K1 value (name: K1 position, K2 character), ARP new, keys audition
 *
 * The song that was playing, the loop mode, the arp's settings and the keys' octave survive a power-off (settings sector).
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
#include "fm1wifi.h"                         /* the WiFi driver (firmware/wifi/), for the WiFi test page */
#ifdef DINGHY_HOST
static uint32_t host_supply;                  /* platform_supply_full: 1 called with the WiFi core down, 2 with it up */
static void platform_supply_full(void) { host_supply = fm1wifi_is_up() ? 2u : 1u; }
#endif

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
static uint32_t undo_cnt, undo_head;          /* the editor's undo log (see "undo" below): steps held, next slot */
#define ISR_BUDGET_US 5805u                   /* 256 frames at 44.1 kHz */

/* OPL2 runs at 49716 Hz; the FM-1 at 44100. Native samples are generated as needed and interpolated:
 * no division, and the row clock ticks on native samples so the timing is the driver's (18.2 / 9.1 / 4.55 rows/s). */
#define RS_STEP 73875u                        /* 49716 / 44100 in Q16 */
static const uint32_t SPR_Q16[4] = { 0, 179021084u, 358042167u, 716084334u };   /* samples per row, Q16, speed 1..3 */
static uint32_t rs_phase, row_acc;            /* resampler phase (Q16); samples until the next row (Q16) */
static int32_t s_prev, s_cur;                 /* the last two native samples */
static uint32_t arp_acc;                      /* the arpeggiator's clock: native samples until its next event, Q8 */
static void tick_arp(void);                   /* (the arpeggiator, below) */
static void arp_follow(void);
static void arp_drop(void);
static void arp_lock(void);
static uint32_t arp_sync, arp_lmask;          /* SYNC (main's, the ISR held off): the arp on the row clock; its lock rows' mask */
static uint32_t arp_free, arp_gr;             /* the ISR's: rows counted (no song playing); rows since the arp's last lock */
#define LIVE_OWN 0xFFu
static volatile uint32_t live_alg = LIVE_OWN; /* ALGO's copy (the keys' instrument, below): its step 0-15, or none */
#ifdef DINGHY_HOST
static uint32_t host_nat;                     /* native samples generated, for tests/host.c's arp checks */
static uint32_t host_row_at, host_row_g;      /* the last row tick: its native sample, its grid row */
#endif

/* register writes queue here (the player's from the ISR, the editor's from main) and land on the chip between
 * blocks, so main never touches the chip while the ISR is inside it */
#define WQ_N 512u
static volatile uint16_t wq_reg[WQ_N];
static volatile uint8_t wq_val[WQ_N];
static volatile uint32_t wq_w, wq_r;

#ifdef DINGHY_HOST
/* tests/host.c's view of the notes keyed, whoever keys them (the player, the arp, an audition): per channel, the pitch
 * (A0 | B0's block and F-number bits << 8) and the time of the last key-on, the gap before it, and how many */
static uint8_t host_a0[9], host_b0[9];
static uint32_t host_kon_n[9], host_kon_at[9], host_kon_gap[9], host_kon_pitch[9];
static uint8_t host_c0[9];                    /* C0 + channel as last written: feedback << 1 | C0 bit 0 (1: additive) */
#endif
static void opl_write(void *ctx, uint16_t reg, uint8_t val)
{
    uint32_t f = irq_save(), i = wq_w;
    (void)ctx;
    if (i - wq_r < WQ_N) {
        wq_reg[i & (WQ_N - 1u)] = reg; wq_val[i & (WQ_N - 1u)] = val;
        wq_w = i + 1u;
    }
    irq_restore(f);
#ifdef DINGHY_HOST
    if (reg >= 0xA0u && reg <= 0xA8u) host_a0[reg - 0xA0u] = val;
    if (reg >= 0xC0u && reg <= 0xC8u) host_c0[reg - 0xC0u] = val;
    if (reg >= 0xB0u && reg <= 0xB8u) {
        i = reg - 0xB0u;
        if ((val & 0x20u) && !(host_b0[i] & 0x20u)) {
            host_kon_gap[i] = host_nat - host_kon_at[i]; host_kon_at[i] = host_nat; host_kon_n[i]++;
            host_kon_pitch[i] = host_a0[i] | (uint32_t)(val & 0x1Fu) << 8;
        }
        host_b0[i] = val;
    }
#endif
}

static void tick_row(void)                    /* the row clock: before a native sample, when it is due */
{
    uint32_t g = arp_free++;                  /* the arp's grid row (SYNC): the pattern's row while a song plays, else a count */
    if (playing && !song_over) {
        arp_follow();                         /* a new pattern: the arp may need another channel, before the row plays */
        g = player.row;
        if (!macsop_tick(&player))
            song_over = 1;
        pos_order = player.order_idx; pos_row = player.row; rows_done++;
    }
    arp_gr = g & arp_lmask;
#ifdef DINGHY_HOST
    host_row_at = host_nat; host_row_g = g;
#endif
    if (arp_sync && !arp_gr) arp_lock();      /* a row the grid starts a step on: the arp's step, now */
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
static int chan_quiet(uint32_t ch) { return !(player.bx[ch] & 0x20u); }   /* no envelope state here: the key bit */
static int32_t next_native(void)
{
    uint32_t t = 0;
    if (row_acc < (1u << 16)) { tick_row(); t = 1; }
    if (arp_acc < (1u << 8)) { tick_arp(); t = 1; }
    if (t) chip_drain();
    row_acc -= 1u << 16; arp_acc -= 1u << 8;
    return opl2_generate(&chip);
}
#else
/* the core is its own object (tools/build.py, tests/run_host.sh: dbopl2.c at -O2 with DBOPL2_SCOPE_N; the app,
 * the USB stack included, at -Os). The scope screen reads the taps the generator keeps (dbopl2.h). */
#include "dbopl2.h"
#define SCOPE_N DBOPL2_SCOPE_N
#define scope_on dbopl2_scope_on
#define scope dbopl2_scope
#define scope_w dbopl2_scope_w
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
static int chan_quiet(uint32_t ch)            /* the channel's envelope has ended (ST_OFF, dbopl2.c): the carrier, and the
                                               * other operator too when the two are added (C0 bit 0) */
{
    const dbchan *c = &chip.chan[ch];
    return c->op[1].state == 0 && (!(c->regC0 & 1u) || c->op[0].state == 0);
}
static void refill(void)                      /* the next block: up to the row (or arp event) due next, at most NAT_CHUNK */
{
    uint32_t k, i;
    if (row_acc < (1u << 16))
        tick_row();
    if (arp_acc < (1u << 8))
        tick_arp();
    chip_drain();
    k = row_acc >> 16;
    if (k > arp_acc >> 8) k = arp_acc >> 8;   /* >= 1: an arp event is at least a sample away */
    if (k > NAT_CHUNK) k = NAT_CHUNK;
    row_acc -= k << 16; arp_acc -= k << 8;
#ifdef DINGHY_HOST
    host_nat += k;
#endif
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
    undo_cnt = 0;                             /* a new song: the log is about the old one */
    live_alg = LIVE_OWN;                      /* and ALGO's copy is of the old one's instrument */
    chip_reset();
    arp_drop();                               /* the reset took the arp's note: it picks a channel again */
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
    arp_drop();
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

/* ---- the battery: ADC ch3 (PB1, the battery divider), read once a second, smoothed, drawn as a 3-bar icon at
 * the top right of every screen (x 225..238, y 5..14). The top lines are centred and at most 23 characters
 * (184 px), so they end by x 212; the two longer ones, the jukebox's (25 with a version like 0.4.2) and the
 * editor's cursor line (26), are centred in 220 px and end by x 214.
 * The thresholds are raw ADC counts, not volts: 531 / 561 / 591 for 1 / 2 / 3 bars, the values Felucca and
 * SLOOP use (Felucca firmware/src/ui_draw.c batt_level, which calls them the stock thresholds; SLOOP
 * firmware/src/ui_draw.c, the same numbers). Neither gives the divider ratio or the ADC reference, so the count
 * is not converted to a voltage, and the levels are not checked against a meter on OPLBOX. To tune them: GLO
 * (outside the editor) shows the smoothed count in the status line; BATT_LEVEL is the table to change. */
static const uint16_t BATT_LEVEL[3] = { 531, 561, 591 };   /* raw count at which 1, 2, 3 bars show */
#define BATT_HYST 4                           /* counts past a threshold before the bars change: no flicker */
static int32_t batt_avg;                      /* the smoothed count x 16; 0 = not read yet */
static uint32_t batt_lvl;                     /* bars shown, 0..3 */

static void batt_draw(void)                   /* the icon; after anything that redraws the top line */
{
    uint32_t i;
    uint16_t edge = batt_avg && !batt_lvl ? C_RED : C_MUTE;
    cv_begin(14, 10, C_BLACK);
    cv_rect(0, 0, 12, 1, edge); cv_rect(0, 9, 12, 1, edge);   /* the body, 12 x 10 */
    cv_rect(0, 0, 1, 10, edge); cv_rect(11, 0, 1, 10, edge);
    cv_rect(12, 3, 2, 4, edge);               /* the terminal */
    for (i = 0; i < batt_lvl; i++)
        cv_rect((int32_t)(2u + 3u * i), 2, 2, 6, C_TXT);
    cv_blit(225, 5);
}

static void batt_poll(void)                   /* one reading; the icon again when the bars change */
{
    int32_t a = fm1_adc_read(FM1_ADC_BATT), raw;
    uint32_t lvl, first = !batt_avg;
    if (a <= 0)                               /* -1: the ADC timed out */
        return;
    batt_avg = first ? a * 16 : batt_avg + (a * 16 - batt_avg) / 4;   /* about 4 s to settle */
    raw = batt_avg / 16;
    lvl = first ? 0u : batt_lvl;
    while (lvl < 3u && raw >= BATT_LEVEL[lvl] + (first ? 0 : BATT_HYST)) lvl++;
    while (lvl > 0u && raw < BATT_LEVEL[lvl - 1u] - (first ? 0 : BATT_HYST)) lvl--;
    if (first || lvl != batt_lvl) { batt_lvl = lvl; batt_draw(); }
}

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
static uint32_t rec_arm;                      /* REC in EDIT: keys played during playback are written where the song is */
/* Where the song is, for recording. The ISR's pos_order/pos_row are the row that plays NEXT (macsop_tick plays a
 * row, then moves on). Main keeps the row that played last, (rec_o, rec_r), and when it began (rec_ms, the
 * first loop pass that saw rows_done change): a key later than half a row after that goes into the next row. */
static uint32_t rec_o, rec_r, rec_ms, rec_ok;
static uint32_t rec_no, rec_nr, rec_seen;     /* the next row as of the last change, and rows_done then */
static void rec_resync(void) { rec_no = player.order_idx; rec_nr = player.row; rec_seen = rows_done; rec_ok = 0; }
static int32_t kb_oct;                        /* keyboard octave shift, -3..3 */
static uint32_t ins_sel;                      /* the instrument editor's instrument, 0.. */
static uint32_t ins_param, ins_namepos;       /* its cursor: parameter row; character in the name */
static uint32_t song_pos;                     /* the song editor's cursor: order position */
enum { V_JUKE, V_TRACK, V_SCOPE, V_SONG, V_INS, V_ARP };
static uint32_t view;
static const char *msg; static uint32_t msg_until;   /* a line in the status area, for a moment */

static void say(const char *m) { msg = m; msg_until = fm1_ms + 2000u; }

/* ---- the instrument the keys and the arp play outside EDIT (TODO.md 5). PRESETS picks one of the song's: ed_instr,
 * the editor's too, so what was played is what the editor writes. ALGO steps through connection and feedback as one
 * list, FM with feedback 0-7 (steps 0-7), then additive with feedback 0-7 (8-15), on a copy only the keys and the arp
 * play: the song keeps its own (its channels, SAVE). Loading a song drops the copy, and so does another instrument
 * (PRESETS, or K4 in the editor); edits to the instrument in the editor reach the copy when EDIT ends. */
static uint32_t live_src;                     /* the instrument (1..) the copy is of */
static macsop_instr live_patch;               /* the copy: the ISR plays it, main writes it with the ISR held off */

static uint32_t live_step_of(const macsop_instr *in)   /* the list's step for an instrument: the file's 1 is FM */
{
    return ((in->connection & 1u) ? 0u : 8u) + (in->feedback & 7u);
}

static void live_set(uint32_t step)           /* the copy of ed_instr (1..n_instr) at this step, or none (LIVE_OWN) */
{
    uint32_t f = irq_save();
    if (step != LIVE_OWN) {
        live_patch = song.instr[ed_instr - 1u];
        live_patch.connection = step < 8u ? 1u : 0u; live_patch.feedback = (uint8_t)(step & 7u);
    }
    live_alg = step; live_src = ed_instr;
    irq_restore(f);
}

static void live_note(uint32_t ch, uint32_t note, uint32_t vol)   /* (ISR) a note in the keys' instrument */
{
    if (live_alg != LIVE_OWN) macsop_play_patch(&player, (int)ch, (int)note, &live_patch, (int)vol);
    else macsop_play_note(&player, (int)ch, (int)note, (int)(ed_instr <= song.n_instr ? ed_instr : 1u), (int)vol);
}

static char *live_text(char *m)               /* "I03 BRASS", or with a copy "I03 FM FB 5" / "I03 ADDITIVE FB 5" */
{
    const char *nm = song.instr[ed_instr - 1u].name;
    uint32_t i, n;
    char *p = cat(m, "I");
    p = catn(p, ed_instr, 2); *p++ = ' ';
    if (live_alg == LIVE_OWN) {
        for (n = 13u; n && (nm[n - 1u] == ' ' || !nm[n - 1u]); n--) { }
        for (i = 0; i < n; i++) *p++ = nm[i] >= ' ' && nm[i] <= '~' ? nm[i] : '?';
        *p = 0;
    } else {
        p = cat(p, live_alg < 8u ? "FM FB " : "ADDITIVE FB ");
        p = catn(p, live_alg & 7u, 1);
    }
    return p;
}

static char *oct_text(char *m)                /* "OCT3": the octave of the lowest key (F3 with kb_oct 0), as EDIT shows it */
{
    char *p = cat(m, "OCT");
    *p++ = (char)('3' + kb_oct); *p = 0;
    return p;
}

static void live_say(void)
{
    static char m[24];
    live_text(m);
    say(m);
}

static int status_loop_all;                   /* the loop mode draw_status last showed (tests/host.c reads the line) */
#define STATUS_COLS 18u                       /* the second line's text: 8 px characters from x 4, left of the CPU readout at 160 */
static uint16_t status_text(char *s, int loop_all)   /* the status area's second line into s (STATUS_COLS + 1); its colour */
{
    const char *t;
    char *p;
    uint32_t n;
    if (msg && (int32_t)(msg_until - fm1_ms) > 0) t = msg;
    else if (edit) {
        p = oct_text(s); *p++ = ' '; p = cat(p, "STEP"); p = catn(p, ed_step, 1);
        *p++ = ' '; p = cat(p, "I"); p = catn(p, ed_instr, 2);
        if (rec_arm) p = cat(p, " REC");
        return C_MUTE;
    } else t = view == V_ARP ? song_title(cur) : loop_all ? "ALL IN TURN" : "LOOP SONG";
    for (n = 0; t[n] && n < STATUS_COLS; n++) s[n] = t[n];
    s[n] = 0;
    return t == msg ? C_ACC : C_MUTE;
}

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
    /* second line: a message, the mode (on the arp screen the song's title), the audio load */
    {   uint16_t c = status_text(s, status_loop_all = loop_all);
        cv_text(4, 20, &FONT_S, s, c);
    }
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
        draw_text_box(0, 2, 220, &FONT_S, s, C_ACC, 1);   /* 26 characters: left of the battery (batt_draw) */
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
    batt_draw();
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

/* ---- the scope screen: nine boxes, one oscilloscope per channel (rhythm mode: 6 = bass drum, 7 = snare,
 * 8 = hi-hat + tom + cymbal), each labelled with the instrument the channel plays (rhythm boxes: the drums). The trace is the channel's
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
    batt_draw();
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
            if (song.rhythm && ch >= 6u) {    /* the rhythm block: the drums that box mixes, not an instrument */
                const char *d = ch == 6u ? "BD" : ch == 7u ? "SD" : "HH TM CY";
                for (k = 0; (name[k] = d[k]) != 0; k++) {}
            } else {   /* "03 produc": the instrument's number first (what the tracker shows), then its name */
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
    batt_draw();
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
    batt_draw();
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
                if (r == 26) p = cat(s, v ? "FM" : "ADD");   /* the file's 1 is FM: the chip gets 1 - connection (macsop.c set_instr) */
                else if (r <= 24 && (r - 1) % 12 == 11) p = cat(s, v == 0 ? "SINE" : v == 1 ? "HALF" : v == 2 ? "ABS" : "PULSE");
                else p = catn(s, v, 2);
                cv_text(170, y, &FONT_S, s, cur_line ? C_ACC : C_MUTE);
            }
        }
        cv_blit(0, TR_TOP + r0 * 16u);
    }
}

/* ---- views */
static void draw_arp(void);                   /* the arp's settings (after the arpeggiator) */

static void draw_view_frame(void)
{
    lcd_fill(0, 0, 240, 190, C_BLACK);
    scope_on = view == V_SCOPE;
    switch (view) {
    case V_TRACK: draw_tracker_head(); draw_tracker(); break;
    case V_SCOPE: draw_scope_frame(); break;
    case V_SONG:  draw_song_frame(); draw_song(); break;
    case V_INS:   draw_ins_frame(); draw_ins(); break;
    case V_ARP:   draw_arp(); break;
    default:
        draw_text_box(0, 6, 220, &FONT_S, "OPLBOX " APP_VER " - MACS OPERA", C_MUTE, 1);   /* left of the battery */
        draw_title(cur); draw_instruments(); batt_draw();
    }
}

/* ---- editing helpers */
static macsop_cell *cursor_cell(void)
{
    uint16_t pat = macsop_order_pattern(&player, (uint16_t)cur_order);
    return pat == 0xFFFF ? 0 : &song.pat[pat].cell[cur_row][cur_col];
}
/* ---- undo: a log of what the song held before each edit (ENV in EDIT undoes one step, no redo). Not a copy of
 * the song (190 KB; the pool has no room for a second): each step keeps up to 3 scalars as (offset into `song`,
 * size, old value), and, for an insert or delete in the order list, the whole list (198 bytes). 32 steps in a
 * ring, the oldest dropped. A knob turned on and on is one step: the same control on the same value within
 * UNDO_MERGE_MS of its last click adds nothing. Cleared when a song loads (load_song), not saved. */
#define UNDO_N 32u
#define UNDO_MERGE_MS 500u
typedef struct {
    uint32_t off[3], old[3];                  /* the scalars, restored last to first */
    uint8_t size[3], n;
    uint8_t merge;                            /* which control made it (0: none, never merged into) */
    uint8_t list;                             /* 1: the order list was kept too, in undo_list[this slot]; 2: the
                                               * cursor's column of its pattern was (an arp written, arp_write) */
    uint8_t view, a, b, c;                    /* where the cursors were, to put them back */
} undo_step;
static undo_step undo_log[UNDO_N];
static union { uint16_t order[MACSOP_MAX_ORDERS]; macsop_cell col[MACSOP_ROWS]; } undo_list[UNDO_N] __attribute__((section(".pool")));
static uint32_t undo_t;                       /* ms of the last change logged */

static undo_step *undo_last(void) { return undo_cnt ? &undo_log[(undo_head + UNDO_N - 1u) % UNDO_N] : 0; }

static undo_step *undo_push(void)             /* a new step at the cursors as they are now */
{
    undo_step *u = &undo_log[undo_head];
    memset(u, 0, sizeof *u);
    u->view = (uint8_t)view;
    if (view == V_INS) { u->a = (uint8_t)ins_sel; u->b = (uint8_t)ins_param; u->c = (uint8_t)ins_namepos; }
    else if (view == V_SONG) u->a = (uint8_t)song_pos;
    else { u->a = (uint8_t)cur_order; u->b = (uint8_t)cur_row; u->c = (uint8_t)cur_col; }
    undo_head = (undo_head + 1u) % UNDO_N;
    if (undo_cnt < UNDO_N) undo_cnt++;
    return u;
}

static void undo_add(undo_step *u, const void *p, uint32_t size)   /* one more scalar of the song, before it changes */
{
    uint32_t i = u->n++;
    u->off[i] = (uint32_t)((const uint8_t *)p - (const uint8_t *)&song);
    u->size[i] = (uint8_t)size; u->old[i] = 0;
    memcpy(&u->old[i], p, size);              /* little-endian: the low bytes are the value */
}

/* Call before changing the scalar at p (1, 2 or 4 bytes). merge: the control's id, to fold a run of clicks
 * into one step, or 0 for a step of its own. Returns the step (for undo_add, or to move its cursors). */
static undo_step *undo_save(const void *p, uint32_t size, uint32_t merge)
{
    undo_step *u = undo_last();
    uint32_t off = (uint32_t)((const uint8_t *)p - (const uint8_t *)&song);
    if (merge && u && u->merge == merge && u->n == 1u && u->off[0] == off && fm1_ms - undo_t < UNDO_MERGE_MS) {
        undo_t = fm1_ms;
        return u;
    }
    u = undo_push();
    u->merge = (uint8_t)merge;
    undo_add(u, p, size);
    undo_t = fm1_ms;
    return u;
}

static void undo_save_orders(void)            /* before an insert or delete moves the order list */
{
    undo_step *u = undo_push();
    memcpy(undo_list[(undo_head + UNDO_N - 1u) % UNDO_N].order, song.order, sizeof song.order);
    u->list = 1;
    undo_add(u, &song.n_orders, 2);
    undo_t = fm1_ms;
}

static undo_step *undo_pop(void)              /* put the last step back; the step (valid until the next edit), or 0 */
{
    undo_step *u;
    uint32_t i;
    if (!undo_cnt) return 0;
    undo_head = (undo_head + UNDO_N - 1u) % UNDO_N; undo_cnt--;
    u = &undo_log[undo_head];
    if (u->list == 1u) memcpy(song.order, undo_list[undo_head].order, sizeof song.order);
    if (u->list == 2u) {                      /* an arp written: its new order positions out, the column back */
        uint32_t k = song.n_orders - (u->old[0] & 0xFFFFu), pat;
        uint32_t f = irq_save();
        for (i = u->a + 1u; i + k < song.n_orders; i++) song.order[i] = song.order[i + k];
        song.n_orders = (uint16_t)(song.n_orders - k);
        if (player.order_idx != 0xFFFFu && player.order_idx > u->a)   /* the player stays on its pattern; inside the removed range it drops to the position before */
            player.order_idx = player.order_idx > u->a + k ? (uint16_t)(player.order_idx - k) : (uint16_t)u->a;
        irq_restore(f);
        pat = macsop_order_pattern(&player, u->a);
        if (pat != 0xFFFFu)
            for (i = 0; i < MACSOP_ROWS; i++) song.pat[pat].cell[i][u->c].u = undo_list[undo_head].col[i].u;
    }
    for (i = u->n; i-- > 0;)
        memcpy((uint8_t *)&song + u->off[i], &u->old[i], u->size[i]);   /* a cell is one aligned 32-bit store: whole */
    return u;
}

static void cursor_step(void) { cur_row = (cur_row + ed_step) % MACSOP_ROWS; }
static uint32_t key_note(uint32_t key) { int32_t n = 65 + (int32_t)key + 12 * kb_oct; return n < 24 ? 24u : n > 119 ? 119u : (uint32_t)n; }
static int32_t wrap(int32_t v, int32_t n) { return ((v % n) + n) % n; }

static void audition(int ch, uint32_t note, uint32_t instr)   /* a key held: that note on that channel, now */
{
    if (!playing) macsop_play_note(&player, ch, (int)note, (int)instr, (int)ed_vol);
}

/* ---- the arpeggiator. Main keeps the held notes and the settings; the audio ISR plays them, on a clock of its own
 * beside the row clock (arp_acc, native samples in Q8, in refill / next_native), so the steps keep the audio
 * stream's time, not the main loop's. Each event is one note on or one gate's key off through macsop_play_note()
 * (the path audition() takes), a handful of queued register writes, less than a row; swing only changes the
 * length of every other step, so it costs the ISR nothing more.
 * The channel: while a song plays, the highest melodic channel whose column has no note in the playing pattern and
 * whose envelope has ended (its own arp note aside); else the highest channel muted on the scope; else the last
 * melodic one (6 in rhythm mode, 9 otherwise; the rhythm-mode drums, 7-9, never). With no song playing, the highest
 * one not muted. While the arp has a channel the player leaves that column alone (player.skip), and a muted
 * channel is let back into the mix for the arp. Picked again when the pattern changes and when playback starts
 * (tick_row -> arp_follow), given back when the notes stop. The instrument is the keys' (live_note: ed_instr, or
 * ALGO's copy of it), the volume ed_vol. Settings in the settings sector; whether the arp is on is not kept. */
enum { ARP_UP, ARP_DOWN, ARP_INCL, ARP_EXCL, ARP_RAND, ARP_ORDER, ARP_REPEAT, ARP_MODES };
#define ARP_NONE 0xFFu
#define ARP_MAXN 27u                          /* every key at once */
#define ARP_IDLE (128u << 8)                  /* nothing to play: look again in 128 native samples */
static const char *const ARP_MODE_NAME[ARP_MODES] = { "UP", "DOWN", "INCLUSIVE", "EXCLUSIVE", "RANDOM", "ORDER", "REPEAT" };
static const char *const ARP_RATE_NAME[5] = { "1/4", "1/8", "1/8T", "1/16", "1/32" };
static const uint8_t ARP_RATE_SPB[5] = { 1, 2, 3, 4, 8 };   /* steps a beat */
static volatile uint32_t arp_on;
static uint32_t arp_mode, arp_oct = 1, arp_rate = 3, arp_bpm = 120, arp_gate = 50, arp_swing = 50, arp_latch;
static uint32_t arp_step_q8;                  /* native samples a step at the tempo and rate, Q8 */
/* SYNC (arp_bpm 0, K4 left of 30 BPM): the steps come from the song's row clock, one row a 1/16, so 1/4 is 4 rows,
 * 1/8 2, 1/8T 4/3, 1/32 half a row. Every lock row (a row the grid starts a step on: every NUM rows, the pattern's
 * rows 0, NUM, 2 NUM.. while a song plays; twice that with swing, so a swing pair stays whole) the row clock starts
 * the step itself (arp_lock, tick_row), on the row's first sample; the steps between are timed from it, and the
 * last of them waits for the next lock (ARP_WAIT) instead of timing it, so nothing drifts. A new chord's first step
 * waits for the grid's next step (arp_grid_wait). The row clock runs while the song is stopped too, so SYNC then
 * free-runs at the song's speed. Per row the ISR adds a mask and a compare; a division only when a chord starts. */
#define ARP_SYNC 0u
#define ARP_WAIT 0xF0000000u                  /* (arp_acc) the next step is the row clock's: far beyond any lock */
static const uint8_t ARP_SYNC_NUM[5] = { 4, 2, 4, 1, 1 }, ARP_SYNC_DEN[5] = { 1, 1, 3, 1, 2 };   /* rows a step: NUM / DEN */
static uint32_t arp_ls = 1, arp_gk, arp_fresh;   /* steps a lock period (main's, the ISR held off); the ISR's: the step's
                                               * index in the period; a new chord (or new settings) looks for the grid */
static uint32_t arp_spd;                      /* main's: the song speed arp_retime last timed SYNC for */
static uint32_t arp_n;                        /* notes held (or latched); main changes the lists with the ISR held off */
static uint8_t arp_key[ARP_MAXN], arp_ord[ARP_MAXN], arp_sort[ARP_MAXN];   /* keys and notes in press order; notes sorted */
static volatile uint32_t arp_ch = ARP_NONE, arp_unmute;   /* the ISR's (main reads them): the channel held; a muted one let back in */
static uint32_t arp_pos, arp_rest, arp_off_due, arp_rng = 0x2545F491u;            /* the ISR's */
static uint16_t arp_pat = 0xFFFF;             /* the pattern the channel was picked for (0xFFFF: no song playing) */
static uint32_t arpw_n;                       /* main's: the chord as the last key pressed left it, for arp_write */
static uint8_t arpw_ord[ARP_MAXN], arpw_sort[ARP_MAXN];
#ifdef DINGHY_HOST
static uint32_t host_arp_steps, host_arp_at, host_arp_gap, host_arp_last[8];   /* note | channel << 8 */
/* since the settings last changed (arp_retime): steps, those not on a row's first sample, those on a row the grid
 * has no step on (its row not a multiple of NUM), the shortest and longest gap between two */
static uint32_t host_sy_steps, host_sy_off, host_sy_offgrid, host_sy_gmin = ~0u, host_sy_gmax;
#endif

static uint32_t arp_pick(void)                /* (ISR) the channel, as above */
{
    uint32_t n = song.rhythm ? 6u : 9u, used = 0, ch, r;
    uint16_t pat = macsop_order_pattern(&player, player.order_idx);
    if (!playing) {
        for (ch = n; ch-- > 0;) if (!((mute_mask >> ch) & 1u)) return ch;
        return n - 1u;
    }
    if (pat != 0xFFFF)
        for (r = 0; r < MACSOP_ROWS; r++)
            for (ch = 0; ch < n; ch++)
                if (song.pat[pat].cell[r][ch].f.note >= 24u) used |= 1u << ch;
    for (ch = n; ch-- > 0;)
        if (!((used | mute_mask) >> ch & 1u) && (ch == arp_ch || chan_quiet(ch))) return ch;
    for (ch = n; ch-- > 0;) if ((mute_mask >> ch) & 1u) return ch;
    return n - 1u;
}

static void arp_move(void)                    /* (ISR) take the channel arp_pick() names */
{
    uint32_t ch = arp_pick();
    if (arp_ch != ARP_NONE && ch != arp_ch) macsop_play_note(&player, (int)arp_ch, 0, 0, 0);
    arp_ch = ch;
    player.skip = (uint16_t)(1u << ch);
    arp_unmute = (mute_mask >> ch) & 1u ? 1u << ch : 0u;
    chip_mute(mute_mask & ~arp_unmute);
    arp_pat = playing ? macsop_order_pattern(&player, player.order_idx) : 0xFFFFu;
}

static void arp_release(void)                 /* (ISR) the notes stopped: the channel back to the song */
{
    if (arp_ch == ARP_NONE) return;
    macsop_play_note(&player, (int)arp_ch, 0, 0, 0);
    arp_ch = ARP_NONE; arp_off_due = 0; arp_pat = 0xFFFF;
    player.skip = 0;
    if (arp_unmute) { arp_unmute = 0; chip_mute(mute_mask); }
}

static void arp_follow(void)                  /* (ISR, the row clock) a row of another pattern, or the first: pick again */
{
    if (arp_ch != ARP_NONE && macsop_order_pattern(&player, player.order_idx) != arp_pat) arp_move();
}

static void arp_drop(void)                    /* (main, the ISR off the chip) the chip was reset: no channel held */
{
    arp_ch = ARP_NONE; arp_off_due = 0; arp_unmute = 0; arp_pat = 0xFFFF; arp_acc = 0; arp_fresh = 1;
    player.skip = 0;
}

static uint32_t arp_note_of(uint32_t pos, uint32_t n, const uint8_t *ord, const uint8_t *sort, uint32_t *rng)
{                                             /* step pos of the arpeggio of n notes (in press order, sorted) */
    uint32_t len = n * arp_oct, i, v;
    const uint8_t *src = arp_mode == ARP_ORDER ? ord : sort;
    switch (arp_mode) {
    case ARP_DOWN:   i = len - 1u - pos % len; break;
    case ARP_INCL:   i = pos % (2u * len); if (i >= len) i = 2u * len - 1u - i; break;   /* the ends twice */
    case ARP_EXCL:   if (len < 2u) { i = 0; break; }
                     i = pos % (2u * len - 2u); if (i >= len) i = 2u * len - 2u - i; break;   /* the ends once */
    case ARP_RAND:   *rng ^= *rng << 13; *rng ^= *rng >> 17; *rng ^= *rng << 5; i = *rng % len; break;
    case ARP_REPEAT: i = (pos / 2u) % len; break;   /* each note twice */
    default:         i = pos % len;           /* up; order: up through the notes as pressed */
    }
    v = src[i % n] + 12u * (i / n);           /* octave by octave */
    while (v > 119u) v -= 12u;
    return v;
}
static uint32_t arp_note(uint32_t pos) { return arp_note_of(pos, arp_n, arp_ord, arp_sort, &arp_rng); }   /* (ISR) */

static void arp_lock(void)                    /* (ISR, SYNC, a lock row's tick) the step starts on this row's first sample */
{
    if (!arp_on || !arp_n) return;
    arp_acc = row_acc >> 8;                   /* due now, at the row's own fraction of a sample (the steps after keep it) */
    arp_off_due = 0; arp_fresh = 0; arp_gk = 0;   /* a gate still open: the step's key-on retriggers the channel */
}

static uint32_t arp_grid_wait(void)           /* (ISR, SYNC) Q8 samples to the grid's next step (arp_gk its index), or
                                               * ARP_WAIT when that is the next lock's */
{
    uint32_t spr = SPR_Q16[song.speed], in = row_acc < spr ? spr - row_acc : 0u;   /* Q16 into the row */
    uint32_t e = arp_gr * (spr >> 8) + (in >> 8), l0 = arp_step_q8, l1 = arp_step_q8, pr, k, r;
    if (arp_swing > 50u) { l0 = arp_step_q8 / 50u * arp_swing; l1 = arp_step_q8 / 50u * (100u - arp_swing); }
    pr = l0 + l1; k = e / pr * 2u; r = e % pr;
    if (r < 256u) r = 0;                      /* on a step, to the sample: now */
    else if (r <= l0) { k++; r = l0 - r; }
    else { k += 2u; r = pr - r; }
    if (k >= arp_ls) return ARP_WAIT;
    arp_gk = k;
    return r;
}

static void tick_arp(void)                    /* (ISR) the arp's next event is due: a step, or its gate's end */
{
    uint32_t len, on, s, note;
    if (!arp_on || !arp_n) { arp_release(); arp_acc += ARP_IDLE; return; }
    if (arp_off_due) {
        arp_off_due = 0;
        macsop_play_note(&player, (int)arp_ch, 0, 0, 0);
        arp_acc += arp_rest;
        return;
    }
    if (arp_sync && arp_fresh) {              /* a new chord, or new settings: the grid's next step, not now */
        arp_fresh = 0;
        if ((len = arp_grid_wait()) >= 256u) { arp_acc += len; return; }
    }
    if (arp_ch == ARP_NONE) arp_move();
    s = arp_pos++;
    note = arp_note(s);
    live_note(arp_ch, note, ed_vol);
#ifdef DINGHY_HOST
    host_arp_gap = host_nat - host_arp_at; host_arp_at = host_nat;
    host_arp_last[host_arp_steps++ & 7u] = note | arp_ch << 8;
    host_sy_steps++;
    if (host_nat != host_row_at) host_sy_off++;
    else if (host_row_g % ARP_SYNC_NUM[arp_rate]) host_sy_offgrid++;
    if (host_sy_steps > 1u) {
        if (host_arp_gap < host_sy_gmin) host_sy_gmin = host_arp_gap;
        if (host_arp_gap > host_sy_gmax) host_sy_gmax = host_arp_gap;
    }
#endif
    len = arp_step_q8;                        /* swing: a long step, then a short one (SYNC: by the step's place in the grid) */
    if (arp_swing > 50u) len = len / 50u * ((arp_sync ? arp_gk : s) & 1u ? 100u - arp_swing : arp_swing);
    on = arp_gate >= 100u ? len : len / 100u * arp_gate;
    if (on < 256u) on = 256u;                 /* an event at least a sample apart (refill relies on it) */
    if (len < on + 256u) on = len;            /* no room for a key off: legato */
    arp_rest = len - on; arp_off_due = arp_rest != 0;
    if (arp_sync && ++arp_gk >= arp_ls) {     /* the period's last step: the next is the lock row's (arp_lock) */
        if (arp_off_due) arp_rest = ARP_WAIT; else on = ARP_WAIT;
    }
    arp_acc += on;
}

/* main's side: the notes the keys hold, the settings */
static void arp_retime(void)                  /* the step length from the tempo (SYNC: the song's rows) and the rate */
{
    uint32_t q8, lp = ARP_SYNC_NUM[arp_rate], ls = ARP_SYNC_DEN[arp_rate], f, was = arp_sync;
    if (arp_bpm == ARP_SYNC) {                /* rows a step x samples a row (Q16), in Q8 */
        q8 = (uint32_t)((uint64_t)SPR_Q16[song.speed] * lp / (ls * 256u));
        if (arp_swing > 50u && (ls & 1u)) { lp *= 2u; ls *= 2u; }   /* a lock on an even step: swing's pairs stay whole */
        arp_spd = song.speed;
    } else {
        uint32_t d = arp_bpm * ARP_RATE_SPB[arp_rate], q = 2982960u / d, r = 2982960u % d;   /* 49716 x 60 samples a minute */
        q8 = (q << 8) + (r << 8) / d;
    }
    f = irq_save();
    arp_step_q8 = q8;
    arp_sync = arp_bpm == ARP_SYNC; arp_lmask = lp - 1u; arp_ls = ls;
    if (arp_sync || was) { arp_fresh = 1; if (arp_n) arp_acc = 0; }   /* onto the grid (or off it, out of ARP_WAIT) now */
#ifdef DINGHY_HOST
    host_sy_steps = host_sy_off = host_sy_offgrid = host_sy_gmax = 0; host_sy_gmin = ~0u;
#endif
    irq_restore(f);
}

static void arp_sorted(void)                  /* arp_sort: arp_ord ascending (27 at most: insertion) */
{
    uint32_t i, j;
    for (i = 0; i < arp_n; i++) {
        uint8_t v = arp_ord[i];
        for (j = i; j > 0 && arp_sort[j - 1u] > v; j--) arp_sort[j] = arp_sort[j - 1u];
        arp_sort[j] = v;
    }
}

static void arp_press(uint32_t k)             /* key k down: its note joins; the first note starts the clock */
{
    uint32_t i, f;
    for (i = 0; i < arp_n; i++) if (arp_key[i] == k) return;
    if (arp_n >= ARP_MAXN) return;
    f = irq_save();
    arp_key[arp_n] = (uint8_t)k; arp_ord[arp_n] = (uint8_t)key_note(k);
    if (!arp_n++) { arp_acc = 0; arp_pos = 0; arp_fresh = 1; }
    arp_sorted();
    irq_restore(f);
    arpw_n = arp_n; memcpy(arpw_ord, arp_ord, arp_n); memcpy(arpw_sort, arp_sort, arp_n);
}

static void arp_lift(uint32_t k)              /* key k up (not latched): its note leaves; the last one stops the arp now */
{
    uint32_t i, f;
    for (i = 0; i < arp_n && arp_key[i] != k; i++) { }
    if (i == arp_n) return;
    f = irq_save();
    for (; i + 1u < arp_n; i++) { arp_key[i] = arp_key[i + 1u]; arp_ord[i] = arp_ord[i + 1u]; }
    if (!--arp_n) arp_acc = 0;
    arp_sorted();
    irq_restore(f);
}

static void arp_octave(void)                 /* kb_oct changed: the held (or latched) keys' notes again, from the next step */
{
    uint32_t i, f;
    if (!arp_n) return;
    f = irq_save();
    for (i = 0; i < arp_n; i++) arp_ord[i] = (uint8_t)key_note(arp_key[i]);   /* from the key: clamping never drifts */
    arp_sorted();
    irq_restore(f);
    arpw_n = arp_n; memcpy(arpw_ord, arp_ord, arp_n); memcpy(arpw_sort, arp_sort, arp_n);
}

static void arp_clear(void)
{
    uint32_t f = irq_save();
    if (arp_n) { arp_n = 0; arp_acc = 0; }
    irq_restore(f);
}

/* ---- the arp into the tracker (ARP in the pattern editor, TODO.md 4). Offline, from the chord the arp last played
 * (the keys pressed last, held or latched) and its settings now: ordinary cells in the cursor's column, from the cursor
 * down, arp_len patterns of rows. A note each step (ed_instr, ed_vol, pitch 32), a key off where the gate ends, every
 * other row of the range emptied, so the column plays back as the arp's steps and the song stays a plain .CMF. Past
 * row 63, or at the cursor's pattern's end of pattern, it goes on in new empty patterns put into the order list right
 * after the cursor's position. A step is a whole number of rows, the nearest to the arp's rate at the song's speed;
 * the status line says when that is not the arp's tempo. Swing (a long step, then a short one) and the gate round
 * to rows too; a step of one row cannot swing. With SYNC the steps are the synced arp's own rows (arp_write_sync).
 * One undo step takes all of it back (undo_list keeps the column). */
#define ARP_LEN_MAX 8u
static uint32_t arp_len = 1;                  /* patterns of rows ARP writes (the settings' page 2, K4); not kept */
static uint32_t arpw_rng = 0x2545F491u;       /* RANDOM's state for writing, apart from the ISR's */
static const uint16_t ROWS_PER_MIN[4] = { 0, 1092, 546, 273 };   /* speed 1..3: 18.2 / 9.1 / 4.55 rows a second */

static int row_ends(const macsop_pattern *p, uint32_t r, uint32_t skip)   /* macsop.c row_is_break, column skip empty */
{
    uint32_t c;
    for (c = 0; c < MACSOP_CHANNELS; c++)
        if (c != skip && p->cell[r][c].f.note) return p->cell[r][c].f.note == 1u;
    return 0;
}

static macsop_cell *arpw_cell(uint32_t r, uint32_t first)   /* row r of the range: in the cursor's pattern, then the new ones */
{
    uint16_t pat;
    if (r < first) { pat = macsop_order_pattern(&player, (uint16_t)cur_order); r += cur_row; }
    else { r -= first; pat = song.order[cur_order + 1u + r / MACSOP_ROWS]; r %= MACSOP_ROWS; }
    return &song.pat[pat].cell[r][cur_col];
}

static void arpw_put(uint32_t r, uint32_t first, uint32_t note, uint32_t instr)   /* row r of the range: a note, or a key off (4) */
{
    macsop_cell t;
    t.u = 0; t.f.note = (uint8_t)note;
    if (note != 4u) { t.f.instr = (uint8_t)instr; t.f.vol = (uint8_t)ed_vol; t.f.pitch = 32; }
    arpw_cell(r, first)->u = t.u;             /* each cell one aligned store: the player sees it whole */
}

/* SYNC: what the synced arp plays, from the cursor: step s on the row it starts in, s x NUM / DEN rows on (swing
 * as the arp times it), its key off on the row its gate ends in when that is a later row than the note's and before
 * the next step's; a gate that ends inside the note's own row cannot be a row, so the note runs on. 1/8T lands on
 * rows 0 1 2, 4 5 6 (4/3 apart); 1/32, two steps a row, cannot be cells: a row each (as 1/16), and the status says so. */
static void arp_write_sync(uint32_t total, uint32_t first, uint32_t instr)
{
    uint32_t n = ARP_SYNC_NUM[arp_rate], dn = ARP_SYNC_DEN[arp_rate], u, pr, l0, a0, a1, at, b, s;
    if (dn == 2u) dn = 1u;                    /* 1/32 */
    u = 50u * dn;                             /* positions in 1/(50 DEN) rows: a step 50 NUM, swing's long one NUM x swing */
    pr = 100u * n; l0 = arp_swing > 50u ? n * arp_swing : 50u * n;
    for (s = 0;; s++) {
        a0 = s / 2u * pr + (s & 1u ? l0 : 0u);
        a1 = (s + 1u) / 2u * pr + ((s + 1u) & 1u ? l0 : 0u);
        if ((at = a0 / u) >= total) break;
        arpw_put(at, first, arp_note_of(s, arpw_n, arpw_ord, arpw_sort, &arpw_rng), instr);
        b = (a0 + (a1 - a0) * arp_gate / 100u) / u;
        if (arp_gate < 100u && b > at && b < a1 / u && b < total) arpw_put(b, first, 4u, instr);
    }
    say(ARP_SYNC_DEN[arp_rate] == 2u ? "ROUNDED: 1/32 AS 1/16" : ARP_SYNC_DEN[arp_rate] == 3u ? "ARP: SYNC, 1/8T ON ROWS" : "ARP: SYNC");
}

static void arp_write(void)
{
    static char m[24];
    uint32_t spb = ARP_RATE_SPB[arp_rate], d = (arp_bpm == ARP_SYNC ? 1u : arp_bpm) * spb, rpm = ROWS_PER_MIN[song.speed];
    uint32_t total = arp_len * MACSOP_ROWS, first, add, len, lng, sht, step, on, at, s, i, r, f;
    uint32_t instr = ed_instr <= song.n_instr ? ed_instr : 1u;
    uint16_t pat = macsop_order_pattern(&player, (uint16_t)cur_order);
    undo_step *u;
    macsop_cell t;
    char *p;
    if (!arpw_n) { say("ARP: NO CHORD YET"); return; }
    if (song.rhythm && cur_col >= 6u) { say("ARP: NOT ON DRUMS"); return; }
    if (pat == 0xFFFFu) return;
    /* the rows the cursor's pattern plays from the cursor: up to an end of pattern (or a row emptying this
     * column would make one) */
    for (r = cur_row; r < MACSOP_ROWS && !row_ends(&song.pat[pat], r, MACSOP_CHANNELS) && !row_ends(&song.pat[pat], r, cur_col); r++) { }
    first = r - cur_row;
    add = (total - first + MACSOP_ROWS - 1u) / MACSOP_ROWS;
    if (song.n_patterns + add > MACSOP_MAX_PATTERNS || song.n_orders + add > MACSOP_MAX_ORDERS) { say("ARP: SONG IS FULL"); return; }

    u = undo_push();                          /* one step: the column as it was, the order list's length, the patterns' count */
    u->list = 2;
    for (r = 0; r < MACSOP_ROWS; r++) undo_list[(undo_head + UNDO_N - 1u) % UNDO_N].col[r].u = song.pat[pat].cell[r][cur_col].u;
    undo_add(u, &song.n_orders, 2);
    undo_add(u, &song.n_patterns, 2);
    undo_t = fm1_ms;

    for (i = 0; i < add; i++)                 /* the new patterns, empty, then into the order list after the cursor's position */
        for (r = 0; r <= MACSOP_ROWS; r++) for (s = 0; s < MACSOP_CHANNELS; s++) song.pat[song.n_patterns + i].cell[r][s].u = 0;
    f = irq_save();                           /* the ISR may be playing the order list */
    for (i = song.n_orders; i-- > cur_order + 1u;) song.order[i + add] = song.order[i];
    for (i = 0; i < add; i++) song.order[cur_order + 1u + i] = (uint16_t)(song.n_patterns + i);
    song.n_patterns = (uint16_t)(song.n_patterns + add); song.n_orders = (uint16_t)(song.n_orders + add);
    if (player.order_idx != 0xFFFFu && player.order_idx > cur_order) player.order_idx = (uint16_t)(player.order_idx + add);   /* the player stays on its pattern */
    irq_restore(f);

    len = (2u * rpm + d) / (2u * d);          /* rows a step: the nearest whole number */
    if (!len) len = 1;
    lng = (len * arp_swing + 25u) / 50u;      /* swing: two steps of 2 x len rows, the first swing % of them */
    if (lng > 2u * len - 1u) lng = 2u * len - 1u;
    if (lng < len) lng = len;
    sht = 2u * len - lng;
    for (r = 0; r < total; r++) arpw_cell(r, first)->u = 0;   /* each cell one aligned store: the player sees it whole */
    if (arp_bpm == ARP_SYNC) { arp_write_sync(total, first, instr); return; }
    for (s = 0, at = 0; at < total; s++, at += step) {
        step = s & 1u ? sht : lng;
        t.u = 0; t.f.note = (uint8_t)arp_note_of(s, arpw_n, arpw_ord, arpw_sort, &arpw_rng);
        t.f.instr = (uint8_t)instr; t.f.vol = (uint8_t)ed_vol; t.f.pitch = 32;
        arpw_cell(at, first)->u = t.u;
        on = (step * arp_gate + 50u) / 100u;  /* the gate: a key off that many rows on, if the step has room */
        if (!on) on = 1;
        if (on < step && at + on < total) { t.u = 0; t.f.note = 4; arpw_cell(at + on, first)->u = t.u; }
    }

    if (len * d != rpm) {                     /* the tempo the rows give, to a tenth */
        uint32_t v = (20u * rpm / (len * spb) + 1u) / 2u, w = v / 10u;
        p = cat(m, "ROUNDED: ");
        p = catn(p, w, w >= 1000u ? 4u : w >= 100u ? 3u : w >= 10u ? 2u : 1u);
        *p++ = '.'; *p++ = (char)('0' + v % 10u);
        cat(p, " BPM");
    } else if (lng * 50u != len * arp_swing)
        cat(m, "ROUNDED: SWING");
    else {
        p = cat(m, "ARP: "); p = catn(p, len, len >= 10u ? 2u : 1u); cat(p, " ROWS A STEP");
    }
    say(m);
}

/* ---- the arp's settings screen: two pages of four knobs, the channel it plays on */
static uint32_t arp_page, arp_back = V_JUKE;  /* the page shown; the view ARP off goes back to */
static uint32_t arp_shown_ch = ARP_NONE, arp_shown_n;
static void arp_foot(char *a, char *b)        /* the arp screen's last two lines: channel, notes, latch; the keys' instrument (PRESETS, ALGO) and octave */
{
    char *p = cat(a, "CH ");
    if (arp_ch == ARP_NONE) *p++ = '-'; else *p++ = (char)('1' + arp_ch);
    p = cat(p, "  NOTES "); p = catn(p, arp_n, 2);
    cat(p, arp_latch ? "  LATCH ON" : "  LATCH OFF");
    p = b;
    if (song.n_instr && ed_instr >= 1u && ed_instr <= song.n_instr) { p = live_text(b); p = cat(p, "  "); }
    oct_text(p);
}

static void draw_arp(void)
{
    static const char *const LBL[2][4] = { { "K1 MODE", "K2 OCTAVES", "K3 RATE", "K4 TEMPO" }, { "K1 GATE", "K2 SWING", "K3 LATCH", "K4 LENGTH" } };
    char s[32], t[32], *p;
    uint32_t i;
    lcd_fill(0, 0, 240, TR_TOP, C_BLACK);
    p = cat(s, arp_on ? "ARP ON" : "ARP OFF"); p = cat(p, "  PAGE "); *p++ = (char)('1' + arp_page); p = cat(p, "/2");
    draw_text_box(0, 2, 220, &FONT_S, s, C_ACC, 1);
    draw_text_box(0, 20, 240, &FONT_S, "LFO PAGE  ARP ON/OFF", C_MUTE, 1);
    for (i = 0; i < 4u; i++) {
        cv_begin(240, 24, C_BLACK);
        if (*LBL[arp_page][i]) {
            uint32_t v = 0;
            p = s;
            if (!arp_page) switch (i) {
                case 0: p = cat(s, ARP_MODE_NAME[arp_mode]); break;
                case 1: v = arp_oct; break;
                case 2: p = cat(s, ARP_RATE_NAME[arp_rate]); break;
                default: if (arp_bpm == ARP_SYNC) p = cat(s, "SYNC"); else v = arp_bpm;
            } else switch (i) {
                case 0: v = arp_gate; break;
                case 1: v = arp_swing; break;
                case 2: p = cat(s, arp_latch ? "ON" : "OFF"); break;
                default: p = catn(s, arp_len, 1); p = cat(p, arp_len > 1u ? " PATTERNS" : " PATTERN");
            }
            if (p == s) { p = catn(s, v, v >= 100u ? 3u : v >= 10u ? 2u : 1u); if (arp_page) p = cat(p, " %"); else if (i == 3u) p = cat(p, " BPM"); }
            cv_text(20, 4, &FONT_S, LBL[arp_page][i], C_TXT);
            cv_text(140, 4, &FONT_S, s, C_ACC);
        }
        cv_blit(0, TR_TOP + 8u + i * 24u);
    }
    cv_begin(240, 40, C_BLACK);
    arp_foot(s, t);
    cv_text(20, 0, &FONT_S, s, C_MUTE);
    cv_text(20, 18, &FONT_S, t, C_ACC);
    cv_blit(0, TR_TOP + 112u);
    arp_shown_ch = arp_ch; arp_shown_n = arp_n;
    batt_draw();
}

/* ---- the WiFi diagnostic page (docs/WIFI.md stages 2 and 3; the driver is firmware/wifi/, the fm1wifi library).
 * SAVE outside EDIT opens it; nothing touches the radio until PLAY there. PLAY stops the sound (and waits until the
 * output buffers hold silence), fills the free pool (past the last .pool buffer, up to the stacks) with a pattern,
 * powers the WiFi core up and reads MAC_CSR0, MAC_SYS_CTRL and BBP 0 (stage 2), then brings the radio up (stage 3:
 * MAC/PHY reset, PLL bank scan, the trim from RAM with interrupts masked, RF RAM table, channel 6), surveys channels
 * 1-13 as JieLi's channel select does (fm1wifi_channel_survey: 200 ms each, the false CCA count and busy time), goes
 * back to 6 and reads the receive state there (fm1wifi_rx_probe). Page 1 shows the bring-up, page 2 the trim and the
 * false CCA counts, page 3 the VGA searches and the receive state, page 4 both counts per channel; PLAY again goes to
 * the next page. The pool is checked again every half second while the page is open, for RAM the
 * hardware writes. SAVE leaves: the core powered down (its registers back as they were), the sound back. The page
 * runs its own loop, so the rest of the app is held meanwhile; platform_poll() keeps USB (the updater) alive. */
#ifdef DINGHY_HOST
#include "sdio_model.h"                       /* the host's WiFi core: tests/wifi/sdio_model.c */
static uint32_t wifi_pool_host[4096];
#define WIFI_FREE_LO ((uint8_t *)wifi_pool_host)
#define WIFI_FREE_HI ((uint8_t *)wifi_pool_host + sizeof wifi_pool_host)
#else
extern uint32_t _pool_end[], _pool_top[];     /* app.ld: the pool's buffers end; the pool region's end */
#define WIFI_FREE_LO ((uint8_t *)_pool_end)
#define WIFI_FREE_HI ((uint8_t *)_pool_top)
#endif

static char *hexn(char *d, uint32_t v, uint32_t digits)
{
    while (digits--) { uint32_t x = (v >> (4u * digits)) & 15u; *d++ = (char)(x < 10u ? '0' + x : 'A' + x - 10u); }
    *d = 0;
    return d;
}

static char *cati(char *d, int32_t v, uint32_t w, int plus)   /* v right-aligned in w places, its sign in front */
{
    char t[12];
    uint32_t n = 0, a = v < 0 ? (uint32_t)-v : (uint32_t)v;
    do { t[n++] = (char)('0' + a % 10u); a /= 10u; } while (a);
    if (v < 0) t[n++] = '-';
    else if (plus) t[n++] = '+';
    while (w > n) { *d++ = ' '; w--; }
    while (n) *d++ = t[--n];
    *d = 0;
    return d;
}

static void wifi_line(uint32_t i, const char *s, uint16_t c)   /* line i of the page */
{
    draw_text_box(0, 2u + 22u * i, 240, &FONT_S, s, c, 0);
#ifdef DINGHY_HOST
    printf("%6u ms  wifi page %u: %s\n", fm1_ms, i, s);
#endif
}

static void wifi_ram_line(fm1wifi_ramcheck rc)                 /* lines 7 and 8 of page 1 */
{
    char s[40], *p;
    if (!rc.changed) { wifi_line(7, "RAM CHECK: 0 CHANGED", C_ACC); wifi_line(8, "", C_MUTE); return; }
    p = cat(s, "RAM CHECK: "); p = catn(p, rc.changed, rc.changed >= 10000u ? 5u : rc.changed >= 1000u ? 4u : rc.changed >= 100u ? 3u : rc.changed >= 10u ? 2u : 1u);
    cat(p, " CHANGED");
    wifi_line(7, s, C_ACC);
    p = cat(s, "FIRST AT "); hexn(p, (uint32_t)(uintptr_t)rc.first, 8);
    wifi_line(8, s, C_ACC);
}

static char *wifi_val(char *d, uint32_t got, uint32_t bit, int32_t v, uint32_t w, int plus)   /* as cati, or "--" if never read */
{
    if (got & bit) return cati(d, v, w, plus);
    while (w > 2u) { *d++ = ' '; w--; }
    return cat(d, "--");
}

static char *wifi_count(char *d, uint32_t got, uint32_t bit, uint32_t v)   /* 4 places: 0-999, then thousands "64K" */
{
    if (!(got & bit) || v < 1000u) return wifi_val(d, got, bit, (int32_t)v, 4, 0);
    d = cati(d, (int32_t)(v / 1000u), 3, 0);
    *d++ = 'K';
    *d = 0;
    return d;
}

static int32_t wifi_dc(uint16_t code) { return code & 0x200u ? (int32_t)(code & 0x1FFu) : -(int32_t)code; }

static uint32_t wifi_reached(const fm1wifi_radio *r)   /* 0 not started .. 6 done: how far the bring-up got */
{
    static const char *const ST[] = { "PHY", "PLL", "TRIM", "RF", "TABLE", "DONE" };
    uint32_t i, k;
    for (i = 0; i < 6u && r->stage; i++) {                 /* no strcmp on the chip */
        for (k = 0; ST[i][k] && r->stage[k] == ST[i][k]; k++)
            ;
        if (!ST[i][k] && !r->stage[k]) return i + 1u;
    }
    return 0;
}

/* ---- stages 4-6 on the page (docs/WIFI_STAGE4.md): after the survey, the link up with the FM-1's address, a passive
 * scan (120 ms per channel), an active one (40 ms per channel, probe requests for any network and for OPLBOX), then
 * joining the open network OPLBOX (a phone's hotspot), 2 s joined, leaving and joining again. What each step found
 * is kept here for pages 5-10. */
enum { WL_LINK = 1, WL_PASSIVE = 2, WL_ACTIVE = 4, WL_JOIN = 8, WL_STAY = 16, WL_REJOIN = 32 };
enum { WL_NO_OPLBOX = 1, WL_NOT_OPEN = 2 };
static struct {
    uint32_t reached;             /* WL_*: the steps that ran */
    int link, ep, ea, ej;         /* fm1wifi_link_up's, the passive and active scans', fm1wifi_join's results */
    uint32_t no_join;             /* WL_NO_OPLBOX, WL_NOT_OPEN: why the join did not run */
    uint8_t mac[6], mac_uid;      /* the FM-1's address; 1: made from the flash's unique ID */
    fm1wifi_scan_list list;
    uint32_t n_passive;           /* networks the passive scan found */
    fm1wifi_frames fp, fa, fj;    /* what the passive scan, the active scan and the join saw */
    fm1wifi_counters c;           /* the core's counters from the link up to the end */
    uint32_t q0, w0;              /* TXRXQ_PCNT and the WL30 host block's 0x04 before the scans */
    fm1wifi_bss target;
    fm1wifi_sta sta;
    uint8_t j_state, j_why, s_state, s_why, r_state, r_why;   /* the join, after 2 s, the rejoin */
    uint16_t j_status, j_aid, s_reason;
    uint32_t s_beacons;
} wl;

/* The FM-1's WiFi address, the same on every start: the flash part's factory unique ID hashed (FNV-1a) into a
 * locally administered unicast address. No flash write is needed to keep it. Without an ID, 02:4F:50:4C:42:58. */
static uint32_t wifi_mac(uint8_t *mac)
{
    uint8_t id[16];
    uint32_t h = 2166136261u, g = 0x811C9DC5u ^ 0x5A5A5A5Au, i;
    if (platform_uid(id)) {
        static const uint8_t FIXED[6] = { 0x02, 0x4F, 0x50, 0x4C, 0x42, 0x58 };   /* "OPLBX" */
        for (i = 0; i < 6u; i++) mac[i] = FIXED[i];
        return 0;
    }
    for (i = 0; i < 16u; i++) { h = (h ^ id[i]) * 16777619u; g = (g ^ id[15u - i]) * 16777619u; }
    mac[0] = (uint8_t)((h & 0xFCu) | 0x02u);
    mac[1] = (uint8_t)(h >> 8); mac[2] = (uint8_t)(h >> 16); mac[3] = (uint8_t)(h >> 24);
    mac[4] = (uint8_t)g; mac[5] = (uint8_t)(g >> 8);
    return 1;
}

static void wifi_frames_run(void)
{
    const fm1wifi_bss *b;
    uint32_t ch, i;
    for (i = 0; i < sizeof wl; i++) ((uint8_t *)&wl)[i] = 0;
    wl.mac_uid = (uint8_t)wifi_mac(wl.mac);
    wl.link = fm1wifi_link_up(wl.mac, 1);
    wl.reached = WL_LINK;
    if (wl.link != FM1WIFI_OK) return;
    fm1wifi_counters_clear(&wl.c);
    wl.q0 = wl.c.txrxq; wl.w0 = wl.c.host04;
    wl.reached |= WL_PASSIVE;
    for (ch = 1; ch <= 13u && wl.ep == FM1WIFI_OK; ch++) { wl.ep = fm1wifi_scan_channel(ch, 120, 0, 0, &wl.list, &wl.fp); platform_poll(); }
    wl.n_passive = wl.list.n;
    if (wl.ep == FM1WIFI_OK) {
        wl.reached |= WL_ACTIVE;
        for (ch = 1; ch <= 13u && wl.ea == FM1WIFI_OK; ch++) { wl.ea = fm1wifi_scan_channel(ch, 40, 1, "OPLBOX", &wl.list, &wl.fa); platform_poll(); }
    }
    fm1wifi_counters_add(&wl.c);
    b = fm1wifi_scan_find(&wl.list, "OPLBOX");
    if (wl.ep != FM1WIFI_OK || wl.ea != FM1WIFI_OK) return;
    if (!b) { wl.no_join = WL_NO_OPLBOX; return; }
    wl.target = *b;
    if (b->sec != FM1WIFI_SEC_OPEN) { wl.no_join = WL_NOT_OPEN; return; }
    wl.reached |= WL_JOIN;
    wl.ej = fm1wifi_join(&wl.sta, &wl.target, 2000, &wl.fj);
    platform_poll();
    wl.j_state = wl.sta.state; wl.j_why = wl.sta.why; wl.j_status = wl.sta.status; wl.j_aid = wl.sta.aid;
    if (wl.ej == FM1WIFI_OK && wl.sta.state == FM1WIFI_STA_UP) {
        wl.reached |= WL_STAY;
        for (i = 0; i < 4u && wl.sta.state == FM1WIFI_STA_UP; i++) { fm1wifi_listen(&wl.sta, 500, &wl.fj); platform_poll(); }
        wl.s_state = wl.sta.state; wl.s_why = wl.sta.why; wl.s_reason = wl.sta.reason; wl.s_beacons = wl.sta.beacons;
        if (wl.s_state == FM1WIFI_STA_UP) {
            wl.reached |= WL_REJOIN;
            fm1wifi_leave(&wl.sta, &wl.fj);
            fm1wifi_join(&wl.sta, &wl.target, 2000, &wl.fj);
            wl.r_state = wl.sta.state; wl.r_why = wl.sta.why;
            fm1wifi_leave(&wl.sta, &wl.fj);
            platform_poll();
        }
    }
    fm1wifi_counters_add(&wl.c);
}

static char *catu(char *d, uint32_t v) { return cati(d, (int32_t)v, 1, 0); }

static char *cat_mac(char *d, const uint8_t *m)
{
    for (uint32_t i = 0; i < 6u; i++) { if (i) *d++ = ':'; d = hexn(d, m[i], 2); }
    return d;
}

static char *cat_err(char *d, const char *what, int err)   /* "FAIL <what> <error>" */
{
    d = cat(d, "FAIL ");
    if (what) d = cat(cat(d, what), " ");
    return cat(d, fm1wifi_strerror(err));
}

static void wifi_step(uint32_t i, const char *name, char *s)   /* a STEPS line: OK green, the rest plain */
{
    char t[40], *p = cat(t, name);
    while (p < t + 8) *p++ = ' ';
    cat(p, s);
    wifi_line(i, t, s[0] == 'O' && s[1] == 'K' ? C_ACC : C_TXT);
}

static const fm1wifi_frames *wl_raw(uint32_t k, uint32_t *idx)   /* the k-th raw frame kept: passive first */
{
    const fm1wifi_frames *f[3] = { &wl.fp, &wl.fa, &wl.fj };
    for (uint32_t i = 0; i < 3u; i++) {
        if (k < f[i]->n_raw) { *idx = k; return f[i]; }
        k -= f[i]->n_raw;
    }
    return 0;
}

static void wifi_more(uint32_t page, const fm1wifi_diag *d, const fm1wifi_radio *r, uint32_t sv_got)
{
    static const char *const FOOT[6] = { "PLAY: PAGE 6   SAVE: LEAVE", "PLAY: PAGE 7   SAVE: LEAVE", "PLAY: PAGE 8   SAVE: LEAVE",
                                         "PLAY: PAGE 9   SAVE: LEAVE", "PLAY: PAGE 10  SAVE: LEAVE", "PLAY: PAGE 1   SAVE: LEAVE" };
    char s[40], *p;
    uint32_t i, k, n;
    wifi_line(9, FOOT[page - 4u], C_MUTE);
    if (page == 4u) {                                 /* each step's result: where the run stopped */
        wifi_line(0, "STEPS                 5/10", C_ACC);
        if (d->err != FM1WIFI_OK) cat_err(s, "POWER UP", d->err);
        else if (r->err != FM1WIFI_OK) { p = cat_err(s, 0, r->err); cat(cat(p, " AT "), r->stage); }
        else cat(s, "OK");
        wifi_step(1, "RADIO", s);
        for (i = 0, n = 0; i < 13u; i++) n += sv_got >> i & 1u;
        if (r->err != FM1WIFI_OK || d->err != FM1WIFI_OK) cat(s, "NOT REACHED");
        else { p = cat(s, n == 13u ? "OK " : "FAIL "); cat(catu(p, n), " OF 13 CH"); }
        wifi_step(2, "SURVEY", s);
        if (!(wl.reached & WL_LINK)) cat(s, "NOT REACHED");
        else if (wl.link != FM1WIFI_OK) cat_err(s, 0, wl.link);
        else cat(s, "OK");
        wifi_step(3, "LINK", s);
        if (!(wl.reached & WL_PASSIVE)) cat(s, "NOT REACHED");
        else if (wl.ep != FM1WIFI_OK) cat_err(s, "RX", wl.ep);
        else if (!wl.fp.frames) cat(s, "FAIL NO FRAMES");
        else if (!wl.n_passive) cat(s, "FAIL NO NETWORKS");
        else cat(catu(cat(s, "OK "), wl.n_passive), wl.n_passive == 1u ? " NETWORK" : " NETWORKS");
        wifi_step(4, "PASSIVE", s);
        if (!(wl.reached & WL_ACTIVE)) cat(s, "NOT REACHED");
        else if (wl.ea != FM1WIFI_OK) cat_err(s, "RX", wl.ea);
        else if (wl.fa.tx_errno != FM1WIFI_OK) cat_err(s, "TX", wl.fa.tx_errno);
        else if (!wl.fa.probe_resps) cat(s, "FAIL NO ANSWER");
        else cat(catu(cat(s, "OK "), wl.fa.probe_resps), wl.fa.probe_resps == 1u ? " ANSWER" : " ANSWERS");
        wifi_step(5, "ACTIVE", s);
        if (wl.no_join == WL_NO_OPLBOX) cat(s, "FAIL NO OPLBOX");
        else if (wl.no_join == WL_NOT_OPEN) cat(cat(s, "FAIL OPLBOX IS "), fm1wifi_sec_name(wl.target.sec));
        else if (!(wl.reached & WL_JOIN)) cat(s, "NOT REACHED");
        else if (wl.ej != FM1WIFI_OK) cat_err(s, 0, wl.ej);
        else if (wl.j_state == FM1WIFI_STA_UP) catu(cat(s, "OK AID "), wl.j_aid);
        else if (wl.j_state == FM1WIFI_STA_DOWN) cat(cat(s, "FAIL "), fm1wifi_why_name(wl.j_why));
        else cat(cat(s, "FAIL STILL "), fm1wifi_sta_state_name(wl.j_state));
        wifi_step(6, "JOIN", s);
        if (!(wl.reached & WL_STAY)) cat(s, "NOT REACHED");
        else if (wl.s_state == FM1WIFI_STA_UP) cat(catu(cat(s, "OK "), wl.s_beacons), " BEACONS");
        else cat(cat(s, "FAIL "), fm1wifi_why_name(wl.s_why));
        wifi_step(7, "STAY 2S", s);
        if (!(wl.reached & WL_REJOIN)) cat(s, "NOT REACHED");
        else if (wl.r_state == FM1WIFI_STA_UP) cat(s, "OK");
        else if (wl.r_state == FM1WIFI_STA_DOWN) cat(cat(s, "FAIL "), fm1wifi_why_name(wl.r_why));
        else cat(cat(s, "FAIL STILL "), fm1wifi_sta_state_name(wl.r_state));
        wifi_step(8, "REJOIN", s);
        return;
    }
    if (page == 5u) {                                 /* the networks: SSID, channel, RSSI, security, P: probed */
        p = cat(s, "SCAN LIST "); p = cati(p, wl.list.n, 3, 0); cat(p, " NETS   6/10");
        wifi_line(0, s, C_ACC);
        if (!(wl.reached & WL_PASSIVE)) { wifi_line(1, "NOT REACHED: SEE PAGE 5", C_TXT); return; }
        if (!wl.list.n) { wifi_line(1, "NO NETWORK HEARD", C_TXT); return; }
        n = wl.list.n > 8u ? 7u : wl.list.n;
        for (i = 0; i < n; i++) {
            const fm1wifi_bss *b = &wl.list.e[i];
            const char *id = b->ssid_len ? b->ssid : "(HIDDEN)";
            for (k = 0, p = s; k < 10u && id[k]; k++) *p++ = (char)(id[k] >= 'a' && id[k] <= 'z' ? id[k] - 32 : id[k] < ' ' || id[k] > '~' ? '?' : id[k]);
            while (p < s + 10) *p++ = ' ';
            p = cati(p, b->channel, 3, 0); p = cati(p, b->rssi, 5, 0); *p++ = ' ';
            p = cat(p, fm1wifi_sec_name(b->sec));
            cat(p, b->probed ? " P" : "");
            wifi_line(1u + i, s, C_ACC);
        }
        if (n < wl.list.n || wl.list.full) {
            p = cat(catu(cat(s, "+"), wl.list.n - n), " MORE");
            if (wl.list.full) cat(catu(cat(p, ", "), wl.list.full), " NOT KEPT");
            wifi_line(8, s, C_TXT);
        }
        return;
    }
    if (page == 6u || page == 7u) {                   /* the first four frames as the FIFO gave them, 36 bytes each */
        uint32_t base = page == 6u ? 0u : 2u, idx;
        cat(cat(cat(cat(s, "RAW FRAMES "), page == 6u ? "1-2" : "3-4"), "       "), page == 6u ? "7/10" : "8/10");
        wifi_line(0, s, C_ACC);
        for (k = 0; k < 2u; k++) {
            const fm1wifi_frames *f = wl_raw(base + k, &idx);
            if (!f) { if (!k) wifi_line(1, (wl.reached & WL_PASSIVE) ? "NO FRAME RECEIVED" : "NOT REACHED: SEE PAGE 5", C_TXT); break; }
            p = cat(catu(s, base + k + 1u), " CH "); p = cati(p, f->raw_ch[idx], 2, 0);
            p = cat(p, "  BYTES "); catu(p, f->raw_n[idx]);
            wifi_line(1u + 4u * k, s, C_TXT);
            for (i = 0; i < 3u; i++) {                /* 3 lines of 3 groups of 4 bytes, in memory order */
                p = s;
                for (n = 0; n < 3u; n++) {
                    const uint8_t *g = &f->raw[idx][12u * i + 4u * n];
                    if (n) *p++ = ' ';
                    for (uint32_t b = 0; b < 4u; b++) p = hexn(p, g[b], 2);
                }
                wifi_line(2u + 4u * k + i, s, C_ACC);
            }
        }
        return;
    }
    if (page == 8u) {                                 /* the core's side: FIFO state, queue counts, counters */
        const fm1wifi_frames *f[3] = { &wl.fp, &wl.fa, &wl.fj };
        uint32_t fr = 0, bad = 0, me = 0, tx = 0, txe = 0;
        int e = FM1WIFI_OK;
        for (i = 0; i < 3u; i++) {
            fr += f[i]->frames; bad += f[i]->bad; me += f[i]->to_us; tx += f[i]->tx; txe += f[i]->tx_err;
            if (e == FM1WIFI_OK) e = f[i]->tx_errno;
        }
        wifi_line(0, "RX/TX COUNTS          9/10", C_ACC);
        if (!(wl.reached & WL_PASSIVE)) { wifi_line(1, "NOT REACHED: SEE PAGE 5", C_TXT); return; }
        p = cat(s, "WL30 04 "); p = hexn(p, wl.w0, 8); *p++ = '>'; hexn(p, wl.c.host04, 8);
        wifi_line(1, s, C_ACC);
        p = cat(s, "TXRXQ "); p = hexn(p, wl.q0, 8); *p++ = '>'; hexn(p, wl.c.txrxq, 8);
        wifi_line(2, s, C_ACC);
        p = cat(catu(cat(s, "FRAMES "), fr), " BAD "); p = catu(p, bad); catu(cat(p, " TO ME "), me);
        wifi_line(3, s, C_ACC);
        p = cat(catu(cat(s, "CRC ERR "), wl.c.crc_err), "  PHY ERR "); catu(p, wl.c.phy_err);
        wifi_line(4, s, C_ACC);
        p = cat(catu(cat(s, "DUP "), wl.c.rx_dup), "  OVERFLOW "); catu(p, wl.c.rx_overflow);
        wifi_line(5, s, C_ACC);
        p = cat(catu(cat(s, "SENT "), tx), " ERR "); p = catu(p, txe);
        if (e != FM1WIFI_OK) cat(cat(p, " "), fm1wifi_strerror(e));
        wifi_line(6, s, C_ACC);
        p = cat(catu(cat(s, "TX OK "), wl.c.tx_ok), " FAIL "); p = catu(p, wl.c.tx_fail); catu(cat(p, " RETRY "), wl.c.tx_retry);
        wifi_line(7, s, C_ACC);
        cat(cat_mac(cat(s, "MAC "), wl.mac), wl.mac_uid ? " UID" : " FIX");
        wifi_line(8, s, C_ACC);
        return;
    }
    wifi_line(0, "JOIN OPLBOX          10/10", C_ACC);   /* page 10: the join in detail */
    if (!(wl.reached & WL_ACTIVE) || wl.ep != FM1WIFI_OK || wl.ea != FM1WIFI_OK) { wifi_line(1, "NOT REACHED: SEE PAGE 5", C_TXT); return; }
    if (wl.no_join == WL_NO_OPLBOX) { wifi_line(1, "NO OPLBOX IN THE SCAN", C_TXT); wifi_line(2, "(AN OPEN HOTSPOT NAMED", C_MUTE); wifi_line(3, "OPLBOX, 2.4 GHZ)", C_MUTE); return; }
    cat_mac(cat(s, "BSSID "), wl.target.bssid);
    wifi_line(1, s, C_ACC);
    p = cat(s, "CH "); p = cati(p, wl.target.channel, 2, 0); p = cat(p, "  RSSI "); p = cati(p, wl.target.rssi, 4, 0);
    cat(cat(p, "  "), fm1wifi_sec_name(wl.target.sec));
    wifi_line(2, s, C_ACC);
    if (wl.no_join == WL_NOT_OPEN) { wifi_line(3, "NOT OPEN: NOT JOINED", C_TXT); return; }
    p = cat(s, "AUTH  ");
    if (wl.j_why == FM1WIFI_WHY_AUTH_TIMEOUT) cat(p, "NO ANSWER (3 TRIES)");
    else if (wl.j_why == FM1WIFI_WHY_AUTH_REFUSED) catu(cat(p, "REFUSED, STATUS "), wl.j_status);
    else if (wl.j_state == FM1WIFI_STA_AUTH) cat(p, "STILL WAITING");
    else cat(p, "OK");
    wifi_line(3, s, C_ACC);
    p = cat(s, "ASSOC ");
    if (wl.j_why == FM1WIFI_WHY_ASSOC_TIMEOUT) cat(p, "NO ANSWER (3 TRIES)");
    else if (wl.j_why == FM1WIFI_WHY_ASSOC_REFUSED) catu(cat(p, "REFUSED, STATUS "), wl.j_status);
    else if (wl.j_state == FM1WIFI_STA_UP) catu(cat(p, "OK  AID "), wl.j_aid);
    else if (wl.j_state == FM1WIFI_STA_DOWN && wl.j_why >= FM1WIFI_WHY_DEAUTH) cat(p, fm1wifi_why_name(wl.j_why));
    else cat(p, "--");
    wifi_line(4, s, C_ACC);
    p = cat(s, "2 S: ");
    if (!(wl.reached & WL_STAY)) cat(p, "--");
    else if (wl.s_state == FM1WIFI_STA_UP) cat(catu(cat(p, "UP, "), wl.s_beacons), " BEACONS");
    else { p = cat(p, fm1wifi_why_name(wl.s_why)); if (wl.s_why == FM1WIFI_WHY_DEAUTH || wl.s_why == FM1WIFI_WHY_DISASSOC) catu(cat(p, " "), wl.s_reason); }
    wifi_line(5, s, C_ACC);
    p = cat(s, "LEFT, REJOIN ");
    if (!(wl.reached & WL_REJOIN)) cat(p, "--");
    else if (wl.r_state == FM1WIFI_STA_UP) cat(p, "UP");
    else cat(p, fm1wifi_why_name(wl.r_why));
    wifi_line(6, s, C_ACC);
    p = cat(catu(cat(s, "SENT "), wl.fj.tx), " ERR "); catu(p, wl.fj.tx_err);
    wifi_line(7, s, C_ACC);
    p = cat(catu(cat(s, "FRAMES "), wl.fj.frames), " TO ME "); catu(p, wl.fj.to_us);
    wifi_line(8, s, C_ACC);
}

static void wifi_show(uint32_t page, const fm1wifi_diag *d, const fm1wifi_radio *r, const fm1wifi_survey *sv, uint32_t sv_got,
                      const fm1wifi_rxprobe *rx, int rx_ok, fm1wifi_ramcheck rc)
{
    char s[40], *p;
    uint32_t i, k;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    if (page >= 4u) { wifi_more(page, d, r, sv_got); return; }
    if (page == 0u) {
        int ok = d->err == FM1WIFI_OK && r->err == FM1WIFI_OK;
        wifi_line(0, "WIFI TEST (STAGE 6)   1/10", C_ACC);
        p = cat(s, "RESULT: ");
        if (d->err != FM1WIFI_OK) cat(p, fm1wifi_strerror(d->err));
        else if (r->err != FM1WIFI_OK) { p = cat(p, fm1wifi_strerror(r->err)); p = cat(p, " AT "); cat(p, r->stage); }
        else cat(p, "OK");
        wifi_line(1, s, ok ? C_ACC : C_TXT);
        p = cat(s, "MAC_CSR0     "); hexn(p, d->mac_csr0, 8);
        wifi_line(2, s, C_ACC);
        p = cat(s, "MAC_SYS_CTRL "); hexn(p, d->err == FM1WIFI_OK ? r->mac_sys_ctrl : d->mac_sys_ctrl, 8);
        wifi_line(3, s, C_ACC);
        p = cat(s, "BBP 0        ");
        if (d->err == FM1WIFI_OK) hexn(p, r->bbp0, 2);
        else if (d->bbp_err == FM1WIFI_OK) hexn(p, d->bbp0, 2);
        else cat(p, fm1wifi_strerror(d->bbp_err));
        wifi_line(4, s, C_ACC);
        if (wifi_reached(r) > 2u) { p = cat(s, "PLL BANK "); p = cati(p, r->pll_bank, 3, 0); p = cat(p, "  MID "); cati(p, r->pll_mid, 3, 0); }
        else cat(s, "PLL BANK  -");
        wifi_line(5, s, C_ACC);
        p = cat(s, "FILTER ");
        if (wifi_reached(r) > 2u) for (i = 0; i < 3u; i++) p = wifi_val(p, r->got, FM1WIFI_GOT_BW(i), r->bw[i], 4, 0);   /* also when the trim itself failed: what it had read */
        else cat(p, " -");
        wifi_line(6, s, C_ACC);
        wifi_ram_line(rc);
        wifi_line(9, "PLAY: PAGE 2   SAVE: LEAVE", C_MUTE);
        return;
    }
    if (wifi_reached(r) < 3u) {                       /* stopped before the trim began: nothing to show */
        static const char *const HEAD[3] = { "TRIM                  2/10", "RX CHECK              3/10",
                                             "SURVEY                4/10" };
        static const char *const FOOT[3] = { "PLAY: PAGE 3   SAVE: LEAVE", "PLAY: PAGE 4   SAVE: LEAVE",
                                             "PLAY: PAGE 5   SAVE: LEAVE" };
        wifi_line(0, HEAD[page - 1u], C_ACC);
        wifi_line(1, "NOT REACHED", C_TXT);
        wifi_line(9, FOOT[page - 1u], C_MUTE);
        return;
    }
    if (page == 3u) {                                 /* the survey: channels 1-7 on the left, 8-13 on the right */
        wifi_line(0, "SURVEY  200 MS EACH   4/10", C_ACC);
        wifi_line(1, "    CCA   BUSY     CCA   BUSY", C_MUTE);
        for (k = 0; k < 7u; k++) {                    /* 30 places: "1 65535 200000 13 65535 200000" at most */
            p = s;
            for (i = k; i < 13u; i += 7u) {
                p = cati(p, (int32_t)(i + 1u), i < 7u ? 1u : 3u, 0);
                p = wifi_val(p, sv_got, 1u << i, (int32_t)sv[i].false_cca, 6, 0);
                p = wifi_val(p, sv_got, 1u << i, (int32_t)sv[i].busy, 7, 0);
            }
            wifi_line(2u + k, s, C_ACC);
        }
        wifi_line(9, "PLAY: PAGE 5   SAVE: LEAVE", C_MUTE);
        return;
    }
    if (page == 2u) {                                 /* the VGA searches, and the receive state on channel 6 */
        static const char *const RXL[3] = { "RXFE 0  ", "  19 ", "  20 " };
        const uint8_t rxv[3] = { rx->rxfe0, rx->rxfe19, rx->rxfe20 };
        wifi_line(0, "RX CHECK  CH  6       3/10", C_ACC);
        p = cat(s, "VGA GAIN ");
        for (i = 0; i < 3u; i++) p = wifi_val(p, r->got, FM1WIFI_GOT_VGA(i), r->vga[i], 4, 0);
        wifi_line(1, s, C_ACC);
        p = cat(s, "VGA PEAK ");
        for (i = 0; i < 3u; i++) p = wifi_val(p, r->got, FM1WIFI_GOT_VGA(i), r->vga_peak[i], 4, 0);
        wifi_line(2, s, C_ACC);
        p = cat(s, "PEAK AT VGA  0 "); wifi_val(p, rx_ok, 1u, rx->peak, 4, 0);
        wifi_line(3, s, C_ACC);
        p = cat(s, "PEAK AT VGA 41 "); wifi_val(p, rx_ok, 1u, rx->peak_vga41, 4, 0);
        wifi_line(4, s, C_ACC);
        p = cat(s, "RXFE 80 ");
        if (rx_ok) { p = hexn(p, rx->idle80, 2); p = cat(p, "  23 "); hexn(p, rx->rxfe23, 2); } else cat(p, "--");
        wifi_line(5, s, C_ACC);
        p = s;
        for (i = 0; i < 3u; i++) { p = cat(p, RXL[i]); p = rx_ok ? hexn(p, rxv[i], 2) : cat(p, "--"); }
        wifi_line(6, s, C_ACC);
        p = cat(s, "BBP 25  "); if (rx_ok) hexn(p, rx->bbp25, 2); else cat(p, "--");
        wifi_line(7, s, C_ACC);
        wifi_line(9, "PLAY: PAGE 4   SAVE: LEAVE", C_MUTE);
        return;
    }
    p = cat(s, wifi_reached(r) == 3u ? "TRIM FAILED  CH " : "TRIM  CH "); p = wifi_val(p, r->got, FM1WIFI_GOT_CHANNEL, r->channel, 2, 0);
    p = cat(p, "  PWR "); p = wifi_val(p, r->got, FM1WIFI_GOT_CHANNEL, r->power, 1, 0); cat(p, "   2/10");
    wifi_line(0, s, C_ACC);
    p = cat(s, "TX LO "); p = wifi_val(p, r->got, FM1WIFI_GOT_TXLO_I, r->txlo[0], 3, 0);
    p = wifi_val(p, r->got, FM1WIFI_GOT_TXLO_Q, r->txlo[1], 4, 0);
    p = cat(p, "  DC "); p = wifi_val(p, r->got, FM1WIFI_GOT_TXDC_I, r->tx_dc[0], 4, 0);
    wifi_val(p, r->got, FM1WIFI_GOT_TXDC_Q, r->tx_dc[1], 4, 0);
    wifi_line(1, s, C_ACC);
    p = cat(s, "TX GAIN "); p = wifi_val(p, r->got, FM1WIFI_GOT_TX_GAIN, r->tx_gain, 4, 1);
    p = cat(p, "  PHASE "); p = wifi_val(p, r->got, FM1WIFI_GOT_TX_PHASE, r->tx_phase, 4, 1);
    if (r->got & FM1WIFI_GOT_TX_PHASE) cat(p, r->tx_gain >= 0 ? " Q" : " I");
    wifi_line(2, s, C_ACC);
    p = cat(s, "RX GAIN "); p = wifi_val(p, r->got, FM1WIFI_GOT_RX_GAIN, r->rx_gain, 4, 1);
    p = cat(p, "  PHASE "); p = wifi_val(p, r->got, FM1WIFI_GOT_RX_PHASE, r->rx_phase, 4, 1);
    if (r->got & FM1WIFI_GOT_RX_PHASE) cat(p, r->rx_gain >= 0 ? " Q" : " I");
    wifi_line(3, s, C_ACC);
    for (k = 0; k < 2u; k++) {
        p = cat(s, k ? "RX DC Q" : "RX DC I");
        for (i = 0; i < 4u; i++) p = wifi_val(p, r->got, FM1WIFI_GOT_RXDC(i, k), wifi_dc(r->rx_dc[i][k]), 5, 1);
        wifi_line(4u + k, s, C_ACC);
    }
    for (k = 0; k < 3u; k++) {                         /* the false CCA count per channel: 1-5, 6-10, 11-13 */
        uint32_t lo = 1u + 5u * k, hi = k == 2u ? 13u : lo + 4u;
        p = cat(s, "CCA ");
        p = cati(p, (int32_t)lo, 2, 0); *p++ = '-'; p = cati(p, (int32_t)hi, 2, 0); *p++ = ' ';
        for (i = lo; i <= hi; i++) p = wifi_count(p, sv_got, 1u << (i - 1u), sv[i - 1u].false_cca);
        *p = 0;
        wifi_line(6u + k, s, C_ACC);
    }
    wifi_line(9, "PLAY: PAGE 3   SAVE: LEAVE", C_MUTE);
}

/* The crash screen's note (platform.c): the WiFi step and the last register write, when the crash came inside the
 * WiFi library (fm1wifi_where). Called from the fault handler: reads memory only. */
static const char *app_crash_note(uint32_t i)
{
    static char s[2][32];
    char *p;
    if (fm1wifi_where.magic != FM1WIFI_WHERE_MAGIC || i > 1u) return 0;
    if (i == 0u) { cat(cat(s[0], "WIFI AT "), fm1wifi_step_name(fm1wifi_where.step)); return s[0]; }
    p = hexn(cat(s[1], "WR "), fm1wifi_where.addr, 8);
    hexn(cat(p, " = "), fm1wifi_where.val, 8);
    return s[1];
}

static void wifi_page(void)
{
    uint32_t was_run = chip_run, was_play = playing, started = 0, page = 0, t_check = 0, rel, i, sv_got = 0;   /* bit i: sv[i] read */
    fm1wifi_ramcheck rc = { 0, 0 };
    fm1wifi_diag d;
    fm1wifi_radio r;
    fm1wifi_rxprobe rx = { 0 };
    int rx_ok = 0;
    fm1wifi_survey sv[13] = { { 0 } };
#ifdef DINGHY_HOST
    static sdio_model *core;
    if (!core) {                              /* the core the host simulator talks to: HOST_WIFI_MODEL (scripts, ':' between
                                               * them, loaded in order), or the made-up one with its access points */
        const char *path = getenv("HOST_WIFI_MODEL");
        char list[1024], *one, *rest;
        snprintf(list, sizeof list, "%s", path ? path : "tests/wifi/core_model.txt:tests/wifi/aps.txt");
        core = sdio_model_new();
        for (one = list; one; one = rest) {
            if ((rest = strchr(one, ':'))) *rest++ = 0;
            if (sdio_model_load(core, one, stderr)) exit(1);
        }
        sdio_model_raw_log(core, 0);
        sdio_model_use(core);
    }
#endif
    lcd_fill(0, 0, 240, 240, C_BLACK);
    wifi_line(0, "WIFI TEST (STAGE 6)", C_ACC);
    wifi_line(1, "PLAY: POWER UP, TRIM, READ", C_TXT);
    wifi_line(2, "SAVE: LEAVE", C_TXT);
    wifi_line(3, "THE SOUND STOPS MEANWHILE", C_MUTE);
    for (;;) {
        uint32_t b;
        platform_poll();
        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_SAVE)) break;
        if ((b & (1u << BTN_PLAY)) && started) { page = (page + 1u) % 10u; wifi_show(page, &d, &r, sv, sv_got, &rx, rx_ok, rc); }
        if ((b & (1u << BTN_PLAY)) && !started) {
            started = 1;
            chip_off();                       /* no sound while the core comes up: */
            fm1_delay_ms(15);                 /* both output halves rendered silent, so a masked trim stays quiet */
            platform_supply_full();           /* the rails at M-VAVE's levels before the radio's load (platform.c) */
            wifi_line(1, "WORKING (ABOUT 15 S)", C_TXT);
            fm1wifi_pattern_fill(WIFI_FREE_LO, (uint32_t)(WIFI_FREE_HI - WIFI_FREE_LO));
            fm1wifi_diag_read(&d);
            for (i = 0; i < sizeof r; i++) ((uint8_t *)&r)[i] = 0;
            r.stage = "-";
            if (d.err == FM1WIFI_OK && fm1wifi_radio_up(&fm1wifi_cal_default, &r) == FM1WIFI_OK) {
                for (i = 0; i < 13u; i++) {   /* the receive level per channel, then back to 6 */
                    if (fm1wifi_channel_survey(i + 1u, FM1WIFI_SURVEY_MS, &sv[i]) == FM1WIFI_OK) sv_got |= 1u << i;
                    platform_poll();          /* USB (the updater) between the 200 ms waits */
                }
                fm1wifi_set_channel(6);
                rx_ok = fm1wifi_rx_probe(&rx) == FM1WIFI_OK;   /* the ADC peak outside the trim (page 3) */
                wifi_frames_run();            /* stages 4-6: scans and the join (pages 5-10) */
            } else wl.reached = 0;
            rc = fm1wifi_pattern_check(WIFI_FREE_LO, (uint32_t)(WIFI_FREE_HI - WIFI_FREE_LO));
            t_check = fm1_ms;
            wifi_show(page, &d, &r, sv, sv_got, &rx, rx_ok, rc);
        }
        if (started && fm1_ms - t_check >= 500u) {   /* the pool again: a later write by the hardware shows too */
            fm1wifi_ramcheck now = fm1wifi_pattern_check(WIFI_FREE_LO, (uint32_t)(WIFI_FREE_HI - WIFI_FREE_LO));
            t_check = fm1_ms;
            if (now.changed != rc.changed || now.first != rc.first) { rc = now; if (page == 0u) wifi_ram_line(rc); }
        }
    }
    fm1wifi_power_down();                     /* nothing if it never came up */
    chip_run = was_run; playing = was_play;
}

#ifdef DINGHY_HOST
/* tests/host.c checks: the cell at (order position, row, column), whole; row 99 asks for the number of order positions,
 * row 98 for a checksum of the whole order list */
static uint32_t host_cell(uint32_t order, uint32_t row, uint32_t col)
{
    if (row == 99u) return song.n_orders;
    if (row == 97u)                           /* the arpeggiator: col 0 steps played, 1 the last step's channel, 2 native
                                               * samples between the last two steps, 3 the last note, 4 the chip's key-on bit
                                               * on the last step's channel, 5 player.skip, 6 the channel held (255 none),
                                               * 7 the chip's mute mask, 8 the order position playing next */
        switch (col) {
        case 8: return pos_order;
        case 0: return host_arp_steps;
        case 1: return host_arp_last[(host_arp_steps - 1u) & 7u] >> 8;
        case 2: return host_arp_gap;
        case 3: return host_arp_last[(host_arp_steps - 1u) & 7u] & 0xFFu;
#if OPL_CORE == OPL_NUKED
        case 4: return (player.bx[(host_arp_last[(host_arp_steps - 1u) & 7u] >> 8) % 9u] >> 5) & 1u;
        case 5: return player.skip;
        case 6: return arp_ch;
        default: return 0;
#else
        case 4: return (chip.chan[(host_arp_last[(host_arp_steps - 1u) & 7u] >> 8) % 9u].regB0 >> 5) & 1u;
        case 5: return player.skip;
        case 6: return arp_ch;
        default: return chip.mute;
#endif
        }
    if (row == 88u)                           /* the WiFi page: col 0 the core powered up, 1 the song's rows ticking, 2 the chip
                                               * sounding, 3 the radio up (stage 3), 4 the supply rails raised (1 before the
                                               * core came up, 2 after), 5 the crash screen's note (1: it would show one) */
        return col == 0u ? (uint32_t)fm1wifi_is_up() : col == 1u ? playing : col == 2u ? chip_run : col == 3u ? (uint32_t)fm1wifi_radio_is_up()
             : col == 4u ? host_supply : (uint32_t)(app_crash_note(0) != 0);
    if (row == 89u)                           /* SYNC, since the settings last changed: col 0 steps, 1 those not on a row's first
                                               * sample, 2 those on a row the grid has no step on, 3 / 4 the shortest / longest
                                               * gap between two (native samples); 5 arp_bpm (0 SYNC), 6 the song's speed */
        switch (col) {
        case 0: return host_sy_steps;
        case 1: return host_sy_off;
        case 2: return host_sy_offgrid;
        case 3: return host_sy_gmin;
        case 4: return host_sy_gmax;
        case 5: return arp_bpm;
        default: return song.speed;
        }
    if (row == 92u) return col < 9u ? host_c0[col] : 0;   /* the keys' instrument (TODO 5): channel col's C0 */
    if (row == 90u) return (uint32_t)macsop_channel_instr_no(&player, (int)col);   /* its instrument 1.. (0: not the song's) */
    if (row == 91u) {                         /* col 0 ed_instr, 1 ALGO's step (255: no copy), 2 the song's instrument
                                               * ed_instr as a step, 3 the song (cur), 8 kb_oct + 3 (0..6), 4 a hash of the status message (h * 31 + c; 0: none), 5 and 6 the same of the arp screen's
                                               * two last lines (channel, notes, latch; the keys' instrument), 7 the same of the
                                               * status area's second line as drawn (a message, the mode, on the arp screen the song's title) */
        uint32_t h = 0; const char *q;
        switch (col) {
        case 0: return ed_instr;
        case 1: return live_alg;
        case 3: return cur;
        case 8: return (uint32_t)(kb_oct + 3);
        case 2: return ed_instr >= 1u && ed_instr <= song.n_instr ? live_step_of(&song.instr[ed_instr - 1u]) : 255u;
        case 5: case 6: {                     /* the arp screen's two last lines (arp_foot), hashed like the message */
            char a[32], b[32];
            arp_foot(a, b);
            for (q = col == 5u ? a : b; *q; q++) h = h * 31u + (uint8_t)*q;
            return h;
        }
        case 7: {
            char a[40];
            status_text(a, status_loop_all);
            for (q = a; *q; q++) h = h * 31u + (uint8_t)*q;
            return h;
        }
        default:
            if (msg && (int32_t)(msg_until - fm1_ms) > 0) for (q = msg; *q; q++) h = h * 31u + (uint8_t)*q;
            return h;
        }
    }
    if (row == 95u) return col < 9u ? host_kon_pitch[col] | (uint32_t)(host_b0[col] & 0x20u) << 11 : 0;   /* channel col: */
    if (row == 94u) return col < 9u ? host_kon_gap[col] : 0;   /* the last key-on's pitch (| 0x10000 while keyed), */
    if (row == 93u) return col < 9u ? host_kon_n[col] : 0;     /* native samples since the one before, key-ons so far */
    if (row == 96u) return host_arp_last[(host_arp_steps - 1u - col) & 7u] & 0xFFu;   /* the note col steps back */
    if (row == 98u) { uint32_t i, h = 0; for (i = 0; i < MACSOP_MAX_ORDERS; i++) h = h * 31u + song.order[i]; return h; }
    uint16_t pat = macsop_order_pattern(&player, (uint16_t)order);
    return pat == 0xFFFF ? ~0u : song.pat[pat].cell[row][col].u;
}
#endif

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

/* ---- the slots' .CMF files over USB SysEx (slot_sysex.c, docs/API.md): pull any slot, push any slot but the
 * song in the player */
static int slot_in_player(uint32_t i) { return cur == N_SONGS + i; }
static int slot_store(uint32_t i, uint32_t n)
{
    int rc;
    uint32_t was_playing = playing;           /* chip_off() clears it: a push must not stop the song */
    chip_off();                               /* the erase stalls the sound, as SAVE does */
    rc = slot_program(i, slot_buf, n + SLOT_HDR);
    chip_run = 1; playing = was_playing;
    return rc;
}
#ifndef DINGHY_HOST                           /* tests/slot_sysex.c runs it on a computer */
#include "slot_sysex.c"
static void app_usb_cmd(const uint8_t *d, uint32_t n) { slot_sysex(d, n); }
#endif

static void app_main(void)
{
    fm1wifi_where_clear();                    /* .noinit: what the last run left (fm1wifi.h) */
    /* the first two as before the arp, the first ten as before kb_oct: a sector an older OPLBOX wrote still loads (its two
     * or ten bytes; the rest at the defaults). arp_bpm 0 is SYNC: no older sector holds a 0 (they kept 30-300, the
     * defaults say 120), and an older OPLBOX reading this one takes 0 for out of range, so 120 BPM */
    struct { uint8_t song, loop_all, arp_mode, arp_oct, arp_rate, arp_gate, arp_swing, arp_latch; uint16_t arp_bpm; int8_t kb_oct, pad; }
        set = {0, 0, ARP_UP, 1, 3, 50, 50, 0, 120, 0, 0};
    uint32_t shown_order = ~0u, shown_row = ~0u, t_status = 0, t_change = 0, dirty = 0, play = 0, next = 0;
    uint32_t t_scope = 0, keys_prev = 0, redraw = 0, head_dirty = 0, held_ch = 0, t_batt = 0;
    int32_t spin, k1, k2, k3, k4, alg, pre;
    if (settings_load(&set, sizeof set) && settings_load(&set, 10)) settings_load(&set, 2);
    if (set.song >= N_ALL) set.song = 0;
    set.loop_all &= 1u;
    arp_mode = set.arp_mode < ARP_MODES ? set.arp_mode : ARP_UP;
    arp_oct = set.arp_oct >= 1u && set.arp_oct <= 4u ? set.arp_oct : 1u;
    arp_rate = set.arp_rate < 5u ? set.arp_rate : 3u;
    arp_gate = set.arp_gate >= 20u && set.arp_gate <= 100u ? set.arp_gate : 50u;
    arp_swing = set.arp_swing >= 50u && set.arp_swing <= 75u ? set.arp_swing : 50u;
    arp_latch = set.arp_latch & 1u;
    arp_bpm = set.arp_bpm == ARP_SYNC || (set.arp_bpm >= 30u && set.arp_bpm <= 300u) ? set.arp_bpm : 120u;   /* 0: SYNC */
    arp_retime();
    kb_oct = set.kb_oct >= -3 && set.kb_oct <= 3 ? set.kb_oct : 0;

    load_song(set.song);
    batt_poll();
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
            batt_draw();
        }
        if (song_over) {                      /* the ISR stopped at the end */
            next = set.loop_all && !edit ? 1 : 0;
            if (!next) { chip_off(); reload_song(); chip_run = 1; playing = play ? 1u : 0u; }
        }

        /* ---- the views and the modes */
        if (b & (1u << BTN_EDIT)) {
            edit ^= 1u;
            if (!edit) {
                rec_arm = 0;
                if (live_alg != LIVE_OWN)     /* the copy again from the instrument as edited; none if K4 picked another */
                    live_set(live_src == ed_instr && ed_instr <= song.n_instr ? live_alg : LIVE_OWN);
            }
            else arp_clear();                 /* the keys are the editor's now */
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

        /* ---- the keys' octave, everywhere: the editor's notes, the keys outside EDIT, the arp as it plays */
        if (b & ((1u << BTN_OCT_UP) | (1u << BTN_OCT_DOWN))) {
            static char om[8];
            kb_oct += (int32_t)((b >> BTN_OCT_UP) & 1u) - (int32_t)((b >> BTN_OCT_DOWN) & 1u);
            if (kb_oct > 3) kb_oct = 3;
            if (kb_oct < -3) kb_oct = -3;
            arp_octave();
            set.kb_oct = (int8_t)kb_oct; dirty = 1; t_change = fm1_ms;
            if (view == V_ARP && !edit) draw_arp();   /* its footer shows it */
            else if (!edit) { oct_text(om); say(om); }   /* EDIT's status line shows it anyway */
            shown_order = ~0u;
        }
        /* ---- song choice: SELECT (not while editing: the knobs are the editor's there) */
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
                rec_resync();
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
        if (b & (1u << BTN_REC)) {
            if (edit) { rec_arm ^= 1u; say(rec_arm ? "REC ARMED" : "REC OFF"); shown_order = ~0u; }
            else { set.loop_all ^= 1u; dirty = 1; t_change = fm1_ms; shown_order = ~0u; }
        }
        if (edit && (b & (1u << BTN_ENV))) {      /* undo one edit */
            undo_step *u = undo_pop();
            if (u) {
                view = u->view;
                if (view == V_INS) { ins_sel = u->a < song.n_instr ? u->a : song.n_instr - 1u; ins_param = u->b; ins_namepos = u->c; }
                else if (view == V_SONG) { song_pos = u->a < song.n_orders ? u->a : song.n_orders - 1u; cur_order = song_pos; }
                else { cur_order = u->a < song.n_orders ? u->a : song.n_orders - 1u; cur_row = u->b; cur_col = u->c < tr_ncol() ? u->c : 0u; }
                if (view == V_INS) macsop_refresh_instr(&player, (int)ins_sel);   /* channels playing it hear it */
                say("UNDONE");
            } else
                say("NOTHING TO UNDO");
            redraw = 1;
        }
        if ((b & (1u << BTN_SAVE)) && edit) { save_song(); redraw = 1; }
        if ((b & (1u << BTN_SAVE)) && !edit) { wifi_page(); redraw = 1; shown_order = ~0u; }   /* the WiFi page */
        if ((b & (1u << BTN_GLO)) && !edit) {   /* the battery's smoothed ADC count, to tune BATT_LEVEL by */
            static char bm[24];
            char *p = cat(bm, "BATTERY ");
            p = catn(p, (uint32_t)(batt_avg / 16), 4); *p++ = ' '; *p++ = '(';
            p = catn(p, batt_lvl, 1); p = cat(p, "/3)");
            say(bm); shown_order = ~0u;
        }

        /* ---- the scope's mutes */
        if (view == V_SCOPE && down)
            for (k = 0; k < 9u; k++)
                if (down & (1u << WHITE_KEY[k])) { mute_mask ^= 1u << k; chip_mute(mute_mask & ~arp_unmute); }

        /* ---- the arpeggiator (outside EDIT) */
        if (!edit && (b & (1u << BTN_ARP))) {
            arp_on ^= 1u;
            if (arp_on) { if (view != V_ARP) arp_back = view; view = V_ARP; arp_page = 0; }
            else { arp_clear(); if (view == V_ARP) view = arp_back; }
            redraw = 1;
        }
        if (!edit && (b & (1u << BTN_LFO))) {     /* the settings: open them, or turn the page */
            if (view == V_ARP) arp_page ^= 1u; else { arp_back = view; view = V_ARP; arp_page = 0; }
            redraw = 1;
        }
        if (!edit && view == V_ARP && (k1 || k2 || k3 || k4)) {
            int32_t v;
            if (!arp_page) {
                if (k1) arp_mode = (uint32_t)wrap((int32_t)arp_mode + k1, ARP_MODES);
                if (k2) { v = (int32_t)arp_oct + k2; arp_oct = (uint32_t)(v < 1 ? 1 : v > 4 ? 4 : v); }
                if (k3) { v = (int32_t)arp_rate + k3; arp_rate = (uint32_t)(v < 0 ? 0 : v > 4 ? 4 : v); }
                if (k4) { v = arp_bpm == ARP_SYNC ? (k4 > 0 ? 29 + k4 : 0) : (int32_t)arp_bpm + k4;   /* left of 30: SYNC */
                          arp_bpm = (uint32_t)(v < 30 ? (int32_t)ARP_SYNC : v > 300 ? 300 : v); }
            } else {
                if (k1) { v = (int32_t)arp_gate + 5 * k1; arp_gate = (uint32_t)(v < 20 ? 20 : v > 100 ? 100 : v); }
                if (k2) { v = (int32_t)arp_swing + k2; arp_swing = (uint32_t)(v < 50 ? 50 : v > 75 ? 75 : v); }
                if (k4) { v = (int32_t)arp_len + k4; arp_len = (uint32_t)(v < 1 ? 1 : v > (int32_t)ARP_LEN_MAX ? (int32_t)ARP_LEN_MAX : v); }
                if (k3) {
                    arp_latch = k3 > 0;
                    if (!arp_latch)                   /* latch off: only the keys still down stay */
                        for (k = 0; k < 27u; k++) if (!((keys >> k) & 1u)) arp_lift(k);
                }
            }
            arp_retime();
            set.arp_mode = (uint8_t)arp_mode; set.arp_oct = (uint8_t)arp_oct; set.arp_rate = (uint8_t)arp_rate;
            set.arp_gate = (uint8_t)arp_gate; set.arp_swing = (uint8_t)arp_swing; set.arp_latch = (uint8_t)arp_latch;
            set.arp_bpm = (uint16_t)arp_bpm;
            dirty = 1; t_change = fm1_ms;
            draw_arp();
        }
        /* ---- the keys' instrument (outside EDIT; in EDIT the two knobs keep their editor meanings) */
        if (!edit && (pre || alg) && song.n_instr) {
            int32_t v;
            if (ed_instr < 1u || ed_instr > song.n_instr) ed_instr = 1;   /* from a song with more instruments */
            if (pre) { ed_instr = (uint32_t)wrap((int32_t)ed_instr - 1 + pre, (int32_t)song.n_instr) + 1u; live_set(LIVE_OWN); }
            if (alg) {
                v = (int32_t)(live_alg == LIVE_OWN ? live_step_of(&song.instr[ed_instr - 1u]) : live_alg) + alg;
                live_set((uint32_t)(v < 0 ? 0 : v > 15 ? 15 : v));
            }
            if (view == V_ARP) draw_arp();          /* its footer names the instrument; the status line keeps the song's title */
            else live_say();
            shown_order = ~0u;
        }
        if (arp_on && !edit) {                    /* the keys feed it, except on the scope (the mutes); key ups always count */
            if (up && !arp_latch)
                for (k = 0; k < 27u; k++) if ((up >> k) & 1u) arp_lift(k);
            if (down && view != V_SCOPE) {
                if (arp_latch && !(keys & ~down)) arp_clear();   /* latched: a chord after every key was up replaces it */
                for (k = 0; k < 27u; k++) if ((down >> k) & 1u) arp_press(k);
            }
        }
        if (view == V_ARP && (arp_ch != arp_shown_ch || arp_n != arp_shown_n)) draw_arp();

        /* ---- which row just played, and when (recording) */
        if (!playing || rows_done < rec_seen) rec_resync();
        else if (rows_done != rec_seen) {
            rec_o = rec_no; rec_r = rec_nr; rec_ok = 1; rec_ms = fm1_ms;
            rec_no = pos_order; rec_nr = pos_row; rec_seen = rows_done;
        }

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
                    t.f.note = (uint8_t)(n < 24 ? 24 : n > 119 ? 119 : n);
                    if (t.u != c->u) undo_save(&c->u, 4, 1);
                    c->u = t.u; moved = 1;
                    audition((int)cur_col, t.f.note, t.f.instr);
                }
                if (k4) { int32_t v = (int32_t)ed_instr + k4; ed_instr = (uint32_t)(v < 1 ? 1 : v > (int32_t)song.n_instr ? (int32_t)song.n_instr : v);
                          if (c->f.note >= 24u && c->f.instr != (uint8_t)ed_instr) { macsop_cell t = *c; t.f.instr = (uint8_t)ed_instr; undo_save(&c->u, 4, 2); c->u = t.u; } moved = 1; }
                if (alg) { int32_t v = (int32_t)ed_vol + alg * 4; ed_vol = (uint32_t)(v < 0 ? 0 : v > 127 ? 127 : v);
                           if (c->f.note >= 24u && c->f.vol != (uint8_t)ed_vol) { macsop_cell t = *c; t.f.vol = (uint8_t)ed_vol; undo_save(&c->u, 4, 3); c->u = t.u; } moved = 1; }
                if (down) {
                    for (k = 0; k < 27u; k++)
                        if (down & (1u << k)) {
                            macsop_cell t;
                            if (rec_arm && playing && rec_ok) {
                                /* recording: the note goes where the song is, not at the cursor, in the cursor's column.
                                 * The row that played last, or the next one when the key is later than half a row. */
                                uint32_t len_ms = (SPR_Q16[song.speed] >> 16) * 1000u / 49716u;
                                uint32_t to = rec_o, tr = rec_r, f;
                                uint16_t pat;
                                if ((fm1_ms - rec_ms) * 2u > len_ms) { to = pos_order; tr = pos_row; }
                                pat = macsop_order_pattern(&player, (uint16_t)to);
                                if (pat != 0xFFFF && tr < MACSOP_ROWS) {
                                    macsop_cell *rc = &song.pat[pat].cell[tr][cur_col];
                                    undo_step *u = undo_save(&rc->u, 4, 0);
                                    u->view = V_TRACK; u->a = (uint8_t)to; u->b = (uint8_t)tr; u->c = (uint8_t)cur_col;
                                    t = *rc;
                                    t.f.note = (uint8_t)key_note(k); t.f.instr = (uint8_t)ed_instr;
                                    if (!rc->f.note) { t.f.vol = (uint8_t)ed_vol; t.f.pitch = 32; }
                                    /* main writes a cell the ISR may read: one aligned 32-bit store of the whole
                                     * macsop_cell, so the player sees the old cell or the new one, never half of each */
                                    rc->u = t.u;
                                    f = irq_save();   /* the ISR ticks the player: not in the middle of this note */
                                    macsop_play_note(&player, (int)cur_col, t.f.note, t.f.instr, (int)ed_vol);   /* its register writes go through the write queue */
                                    irq_restore(f);
                                    if (to == cur_order) moved = 1;
                                }
                                break;
                            }
                            t = *c;               /* a key: the note into the cell, hear it, step on */
                            t.f.note = (uint8_t)key_note(k); t.f.instr = (uint8_t)ed_instr;
                            if (!c->f.note) { t.f.vol = (uint8_t)ed_vol; t.f.pitch = 32; }
                            undo_save(&c->u, 4, 0);
                            c->u = t.u;
                            audition((int)cur_col, t.f.note, t.f.instr); held_ch = cur_col + 1u;
                            cursor_step(); moved = 1;
                            break;
                        }
                }
                if (b & (1u << BTN_FX))  { macsop_cell t; t.u = 0; t.f.note = 4; undo_save(&c->u, 4, 0); c->u = t.u; cursor_step(); moved = 1; }
                if (b & (1u << BTN_SEL)) { undo_save(&c->u, 4, 0); c->u = 0; cursor_step(); moved = 1; }
                if (b & (1u << BTN_GLO)) { macsop_cell t; t.u = 0; t.f.note = c->f.note == 1 ? 0 : 1; t.f.vol = 127; t.f.pitch = 32; undo_save(&c->u, 4, 0); c->u = t.u; moved = 1; }
                if (b & (1u << BTN_ARP)) { arp_write(); moved = 1; }
            }
            if (up && held_ch) { audition((int)(held_ch - 1u), 0, 0); held_ch = 0; }
            if (moved) { draw_tracker_head(); draw_tracker(); shown_order = ~0u; }
        }

        /* ---- the song editor */
        if (edit && view == V_SONG) {
            uint32_t moved = 0;
            if (spin) { song_pos = (uint32_t)wrap((int32_t)song_pos + spin, (int32_t)song.n_orders); moved = 1; }
            if (k1 && song_pos < song.n_orders) { undo_save(&song.order[song_pos], 2, 4); song.order[song_pos] = (uint16_t)wrap((int32_t)song.order[song_pos] + k1, (int32_t)song.n_patterns); moved = 1; }
            if (k2) { int32_t v = (int32_t)song.speed + k2; v = v < 1 ? 1 : v > 3 ? 3 : v;
                      if (v != (int32_t)song.speed) { undo_save(&song.speed, 1, 7); song.speed = (uint8_t)v; } moved = 1; }
            if (k3) { undo_save(&song.rhythm, 1, 0); song.rhythm ^= 1u; moved = 1; }
            if ((b & (1u << BTN_GLO)) && song.n_orders < MACSOP_MAX_ORDERS) {   /* insert: a copy of this position after it */
                uint32_t i;
                undo_save_orders();
                for (i = song.n_orders; i > song_pos + 1u; i--) song.order[i] = song.order[i - 1u];
                song.order[song_pos + 1u] = song.order[song_pos]; song.n_orders++; song_pos++; moved = 1;
            }
            if ((b & (1u << BTN_SEL)) && song.n_orders > 1u) {                   /* delete this position */
                uint32_t i;
                undo_save_orders();
                for (i = song_pos; i + 1u < song.n_orders; i++) song.order[i] = song.order[i + 1u];
                song.n_orders--; if (song_pos >= song.n_orders) song_pos = song.n_orders - 1u; moved = 1;
            }
            if ((b & (1u << BTN_ARP)) && song.n_patterns < MACSOP_MAX_PATTERNS) {   /* a new empty pattern, here */
                uint32_t r, cc;
                undo_add(undo_save(&song.order[song_pos], 2, 0), &song.n_patterns, 2);   /* the order entry and the count */
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
                if (k2) { int32_t ch = in->name[ins_namepos] ? in->name[ins_namepos] : ' '; ch += k2; if (ch < 32) ch = 32; if (ch > 126) ch = 126;
                          undo_save(&in->name[ins_namepos], 1, 6); in->name[ins_namepos] = (char)ch; moved = 1; }
            } else if (k1) {
                uint8_t *f = ins_field(in, ins_param);
                int32_t v = (int32_t)*f + k1, mx = (int32_t)ins_max(ins_param);
                v = v < 0 ? 0 : v > mx ? mx : v;
                if (v != (int32_t)*f) undo_save(f, 1, 5);
                *f = (uint8_t)v;
                macsop_refresh_instr(&player, (int)ins_sel);   /* channels playing it hear the change */
                moved = 1;
            }
            if ((b & (1u << BTN_ARP)) && song.n_instr < MACSOP_MAX_INSTR) {   /* a new instrument: a copy of this one */
                undo_save(&song.n_instr, 2, 0);
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
        fm1_led_key(BTN_REC, edit ? (int)rec_arm : (int)set.loop_all);   /* EDIT: armed to record; else loop mode */
        fm1_led_key(BTN_EDIT, (int)edit);
        fm1_led_key(BTN_SEQ, view == V_TRACK || view == V_SONG || view == V_INS);
        fm1_led_key(BTN_FX, view == V_SCOPE);
        fm1_led_key(BTN_ARP, (int)arp_on);
        for (k = 0; k < 9u; k++) fm1_led_key(14u + WHITE_KEY[k], view == V_SCOPE && (int)((mute_mask >> k) & 1u));
        if (redraw) { draw_view_frame(); redraw = 0; shown_order = ~0u; }
        if (arp_bpm == ARP_SYNC && song.speed != arp_spd) arp_retime();   /* SYNC: a new speed (K2, ENV, a new song) at once */
        if (fm1_ms - t_batt >= 1000u) { t_batt = fm1_ms; batt_poll(); }   /* the battery: once a second */
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
        if (dirty && !playing && !arp_n && fm1_ms - t_change > 2000u) {   /* an erase stalls the sound: only while paused */
            settings_save(&set, sizeof set); dirty = 0;
        }
        if (dirty && fm1_ms - t_change > 30000u) {               /* ... or after a while anyway, between two rows */
            settings_save(&set, sizeof set); dirty = 0;
        }
    }
}
