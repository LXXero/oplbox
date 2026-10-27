# OPLBOX API

Based on Dinghy by zvenson, https://github.com/zvenson/dinghy: the platform and this API are Dinghy's.

Everything [platform.c](../firmware/src/platform.c) gives your app. One compilation unit: `firmware/src/oplbox.c` includes
`platform.c` first, so all of this is just there. Nothing here is a library you link against; read the source
when in doubt, it is short.

- [The contract](#the-contract)
- [Time](#time)
- [Keys, buttons, encoders](#keys-buttons-encoders)
- [LEDs](#leds)
- [Screen](#screen)
- [Sound](#sound)
- [MIDI](#midi)
- [Settings](#settings)
- [Memory and flash](#memory-and-flash)
- [Build options](#build-options)

## The contract

Your app supplies two functions:

```c
static void app_audio(int32_t *out, uint32_t n);   /* the audio interrupt: fill n stereo frames */
static void app_main(void);                        /* your main loop; never returns */
```

`app_main` must call **`platform_poll()`** often: every few milliseconds, and never less than once every
couple of seconds. It feeds the watchdog (8 s, then the FM-1 resets), runs USB (MIDI, the updater, a computer
asking for the update mode) and reads the MASTER knob. A long job (drawing a lot, saving) is fine; a loop that
waits for something must poll.

```c
static void app_main(void)
{
    /* set up */
    for (;;) {
        platform_poll();
        /* read the panel, MIDI; draw what changed */
    }
}
```

Names the platform defines that you can change from the build: `APP_NAME` (the rescue and crash screens,
default `"DINGHY"` in `platform.c`; `oplbox.c` sets `"OPLBOX"`), `APP_VER` (set by `./build.sh --release`), `FELUCCA_ID` (the package identity; see
[MAKE-IT-YOURS.md](MAKE-IT-YOURS.md)).

## Time

| | |
| --- | --- |
| `fm1_ms` | milliseconds since power-on (`volatile uint32_t`, counted by the 10 kHz timer) |
| `fm1_ticks()` | a free-running 24 MHz counter: `FM1_TICKS_PER_US` (24) per microsecond; wraps in ~179 s, so subtract: `fm1_ticks() - t0` |
| `fm1_micros()` | microseconds, from the same counter |
| `fm1_delay_ms(ms)`, `fm1_delay_us(us)` | busy waits; keep them short (they do not poll) |

To time your audio code: `t0 = fm1_ticks()` before, `(fm1_ticks() - t0) / FM1_TICKS_PER_US` after.

## Keys, buttons, encoders

The panel is scanned at 10 kHz in the background and debounced.

| | |
| --- | --- |
| `fm1_in.notes` | the 27 keys held now: bit 0 = F3 ... bit 26 = G5 |
| `fm1_in.buttons` | the 14 buttons held now: bit = `BTN_...` |
| `fm1_input_edges(&released)` | buttons **pressed** since the last call (bits), and those released into `released` |
| `fm1_input_note_edges()` | keys pressed since the last call (bits): a press shorter than a loop pass still counts |
| `fm1_enc_take(ENC_...)` | detents turned since the last call, + = clockwise |
| `fm1_adc_read(FM1_ADC_MASTER)` | the MASTER knob, 0..1023 (-1 = not ready). platform.c already makes it the volume |
| `fm1_adc_read(FM1_ADC_BATT)` | the battery divider, raw 0..1023 (-1 = not ready). No conversion to volts is known; OPLBOX's icon (`batt_poll` / `batt_draw` in oplbox.c) uses Felucca's and SLOOP's raw thresholds 531 / 561 / 591 |

Buttons (ids 0..13):

| id | | id | | id | |
| --- | --- | --- | --- | --- | --- |
| 0 | `BTN_OCT_DOWN` | 5 | `BTN_LFO` | 10 | `BTN_ARP` |
| 1 | `BTN_OCT_UP` | 6 | `BTN_EDIT` | 11 | `BTN_SEQ` |
| 2 | `BTN_FX` | 7 | `BTN_GLO` | 12 | `BTN_PLAY` |
| 3 | `BTN_SEL` | 8 | `BTN_HOME` | 13 | `BTN_REC` |
| 4 | `BTN_ENV` | 9 | `BTN_SAVE` | | |

`BTN_OCT_DOWN` held at power-on is the USB rescue; your app sees it like any button otherwise.

Encoders: `ENC_SELECT` (0), `ENC_ALGORITHM` (1), `ENC_KNOB1` .. `ENC_KNOB4` (2..5), `ENC_PRESETS` (6).

A key down and up, the usual way:

```c
uint32_t keys = fm1_in.notes, down = keys & ~prev, up = prev & ~keys;
prev = keys;
```

## LEDs

| | |
| --- | --- |
| `fm1_led_key(id, on)` | a button (`BTN_...`) or a key (`KEY_LED(k)`, k = 0..26) lit or not |
| `fm1_led[col]`, `fm1_led_dim[col]` | the raw layers, for many LEDs at once (see `bsp/boards/fm1/fm1_input.h`); `_dim` glows at ~1/24 |

## Screen

240 x 240 pixels, 16-bit colour (`RGB(r, g, b)` with 0..255 each; `C_BLACK`, `C_WHITE`). The screen keeps
what you draw: draw only what changed.

Straight to the screen:

| | |
| --- | --- |
| `lcd_fill(x, y, w, h, colour)` | a rectangle |
| `draw_text_box(x, y, w, font, "text", colour, align)` | text in a box of width `w`, cleared to black first; align 0 left, 1 centre, 2 right |

Fonts: `FONT_S` (16 px, ASCII 32..126) and `FONT_L` (32 px, ASCII 32..95: capitals, digits, signs; small
letters show as `?`).

For anything more, a canvas: draw into a buffer, then send it in one go (no flicker):

```c
cv_begin(240, 56, C_BLACK);                 /* w x h, cleared; w * h <= CV_MAX (240 x 64) */
cv_rect(x, y, w, h, colour);
cv_line(x0, y0, x1, y1, colour);
cv_pset(x, y, colour);
cv_text(x, y, &FONT_S, "text", colour);     /* returns x after the text; text_w(&FONT_S, "text") measures */
cv_blit(0, 140);                            /* to the screen at (0, 140) */
```

A bigger canvas: raise `CV_MAX` in platform.c (2 bytes a pixel, in the pool: 240 x 240 is 113 KB of 336).

## Sound

```c
static void app_audio(int32_t *out, uint32_t n)
```

- Runs in the audio interrupt, every 5.8 ms, with `n` = 256 frames at **44.1 kHz** (`FS`).
- `out` is `L R L R ...`, **already zero**: add your sound to it. Full scale is **±32767** (Q15); more is clipped.
- After you: the MASTER knob, the clip, the USB audio stream (the computer records exactly this), the DAC.
  The DAC gets it 6 dB under its full scale, so the FM-1's output stage does not distort.
- Keep it fast and simple: no flash, no screen, no waiting. Talk to the main loop through `volatile` variables
  (one writer each). If a render runs late the sound breaks up; see [RULES.md](RULES.md).

Pitch: a 32-bit phase that wraps is an oscillator with no division at run time: `inc = f * 2^32 / 44100`,
`ph += inc` each frame. `oplbox.c` keeps a table for one octave and shifts it for the others.

## MIDI

USB-MIDI, class compliant, cable 0.

| | |
| --- | --- |
| `midi_get(&status, &d1, &d2)` | 1 and the next message received, or 0. Call it from the main loop until 0 |
| `midi_send(status, d1, d2)` | send one message (from the main loop only) |

`status` includes the channel: `0x90` note on channel 1, `0x93` channel 4; a note on with velocity 0 is a
note off. SysEx does not reach `midi_get`: the platform takes it (the updater's keys and frames). A frame in the
updater's framing whose command is not the updater's goes to the app's `app_usb_cmd(d, n)` in the main loop
(`d` decoded, the command `d[2]`, the body from `d + 6`); OPLBOX's are the song slots and the WiFi test page below.

## Song slots over SysEx

`tools/fm1_slot.py pull SLOT FILE` and `push SLOT FILE` copy a slot's `.CMF` to and from a computer
(`firmware/src/slot_sysex.c`). The messages use the updater's framing (`ota.c`), 7-bit packed:
`F0 pack7(00 59 cmd len24 body ~sum(body)) F7`, numbers little-endian.

| cmd | from the computer | from the FM-1 |
| --- | --- | --- |
| `0x60` pull | `slot off24` | `slot status total24 off24 data`: up to 512 bytes of the file from `off` |
| `0x61` push | `slot len24 crc32` (CRC-32 as zlib) | first the updater's read requests (`0x30`, the file at address 0, 512 bytes a request; answer them as an update is answered), then `slot status` |

`slot` is 0..8 for the jukebox's SLOT 1..9; a reply to a bad slot number says `0x7F`. The file is the song's
bytes; the slot's 16-byte header stays on the FM-1, which writes it. A push holds the main loop (the screen) until
it is done; the sound goes on until the slot is written, which stalls it as SAVE does.

| status | |
| --- | --- |
| 0 | done |
| 1 | bad request: slot, length or offset |
| 2 | pull: the slot is empty |
| 3 | push: bigger than a slot (32 KiB − 16 = 32752 bytes) |
| 4 | push: the slot's song is in the player (playing or paused, its edits are in RAM): pick another song first |
| 5 | push: the transfer failed (no answer within 3 × 5 s for a request, 30 s for the whole file, or a CRC mismatch) |
| 6 | push: not a `.CMF` the player takes (`macsop_check`, the same rules as `macsop_parse`) |
| 7 | push: the flash write failed |

The command bytes are none of the stock updater's (`0x11` handshake, `0x30` read) or fm1-base's (`0x41`-`0x43`
backup and restore, `0x50`-`0x54` the screen mirror). The other SysEx in the family has its own header and never
reaches here: the stock keys `F0 22 24 35 7D/7F F7` and Felucca's editor `F0 7D 46 4C`.

## WiFi test page over SysEx

`tools/fm1_wifi_report.py` reads the WiFi test page's results (docs/WIFI_STAGE4.md, docs/WIFI_STAGE8.md,
docs/WIFI_STAGE9.md), can start a run and can set the WPA2 network a run joins, so nobody has to photograph 14 pages (`firmware/src/wifi_sysex.c`).
Same framing as the song slots.

| cmd | from the computer | from the FM-1 |
| --- | --- | --- |
| `0x62` report | `off24` | `status total24 off24 serial16 data`: up to 512 bytes of the report from `off` |
| `0x63` run | (nothing) | `status run16`, at once |
| `0x64` network | `ssid_len8 SSID pass_len8 passphrase`, or a single `00` to forget it | `status ssid_len8` |
| `0x65` live | (nothing) | `status step8 addr4 left16 answered16`, at once, also while a run goes on |

The network (stage 8) is the WPA2-PSK network the runs join: an SSID of 1-32 bytes and a passphrase of 8-63
printable ASCII characters (IEEE 802.11 J.4.1); anything else is status 1. The FM-1 keeps both in RAM only, until it
restarts or the command comes again, refuses a change while a run is going on (status 3), and never sends the
passphrase back, draws it or puts it in the report. With no network set a run joins the open network OPLBOX.

The live command (stage 9) says what a run is doing while it goes on: `step8` 0 no run, 1 running, 2 DHCP, 3 ARP,
4 pinging the gateway, 5 listening for echo requests; `addr4` the address DHCP gave (0.0.0.0 before it);
`left16` the listening window's milliseconds left (0 outside it); `answered16` the echo requests answered so far
(little-endian). `fm1_wifi_report.py --run --ping` uses it to ping the FM-1 from the computer while it listens.

`serial16` changes when the FM-1 rebuilds the report (it does while the page is open and its RAM check changes); a
reader that sees it change between chunks starts again. A run asked for starts in the main loop as SAVE then PLAY
would: the sound stops, the page opens and runs (about 15 s), and stays open on page 1 afterwards (SAVE leaves). Asked
for with the page open on a finished run, the page leaves as SAVE does and runs again. `run16` is the number the run
will have (1 for the first since the FM-1 started); the report's header carries the number of the run it is from.

| status | |
| --- | --- |
| 0 | report: done, the chunk follows. run: the run will start |
| 1 | bad request (a body of the wrong length, an offset past the end, a network out of range) |
| 2 | report: no run since the FM-1 started |
| 3 | a run is asked for or going on (run: `run16` is its number); the report is not there until it ends; network: not changed |
| 4 | run: EDIT is open (SAVE writes the song there, so the page cannot open); nothing started |

The report, in RAM until the next run (numbers little-endian):

| Bytes | What |
| --- | --- |
| 4 | `OPLW` |
| 1 | version, 1 |
| 1 | flags: bit 0 the text was cut short, bit 1 counters or frames were (the buffer holds 6 KiB of text and every frame) |
| 2 | the run's number |
| 2 | T, then T bytes of text: every line pages 1-11 show, in order (not the `PLAY: PAGE n` footers), a blank line between pages, then `ALL NETWORKS: ...` and one line per network the scan kept (SSID whole, BSSID, channel, RSSI, security, `P` probed), `n MORE NOT KEPT` when the list was full; after a WPA2 join, the key tables as read back (each key's words only as written or not), `TX_SEC_CNT0` / `RX_SEC_CNT0` before the join and at the end, and two reads of the random number register |
| 1 | K, then K 32-bit counters: 0x30F04 before and after, `TXRXQ_PCNT` before and after, frames read, those that did not parse, those to our address, frames sent, sends that failed, `RX_STA_CNT0` CRC and PHY errors, `RX_STA_CNT2` duplicates and overflows, `TX_STA_CNT` OK, failed, retries, underflows, beacons, probe responses; then (stage 8, K is 58) the WPA2 station's: PMK ms, PMK cached, state, why, RSN refusal, EAPOL refusal, messages seen, keys loaded, 4-way ms, EAPOL in, refused, protected frames in, decrypted, not, group in, decrypted, group key handshakes, key table error; the pairwise entries read back (WCID entry 2 words, attribute, IV, EIV, key words as written as a bit mask), the group's (mode, attribute, IV, EIV, mask), the random register twice, `TX_STA_CNT` OK, failed, retries and CRC errors before the join, `TX_SEC_CNT0` before and after, `RX_SEC_CNT0` before and after (`tools/fm1_wifi_report.py` names them all) |
| 1 | F, then F frames: step (1 passive scan, 2 active scan, 3 join, 4 / 5 an EAPOL frame from / to the AP in the join, 6 / 7 in the rejoin), channel (an EAPOL frame: the message, 1-4, 5 group msg 1, 6 group msg 2), the bytes it had (16 bits: N + 8; EAPOL: the PDU's length), the bytes kept (16 bits, at most 512; EAPOL 192), then those bytes as the FIFO gave them (length word, RXWI, the frame, the trailer; EAPOL: the 802.1X header onward) |

Up to four frames are kept per step. The tool prints the text and the counters; `--out DIR` writes `report.txt`,
`report.bin`, `counters.txt` and `frame-NN-<step>-chNN.bin` (EAPOL frames `frame-NN-<step>-msgN.bin`). EAPOL
msg 1 and 2 allow offline passphrase guesses: keep a WPA2 run's report private.

## Bluetooth test page over SysEx

`tools/fm1_ble_report.py` reads the Bluetooth test page's results (docs/BLUETOOTH_PAGE.md) and can start a run
(`firmware/src/ble_sysex.c`). Same framing as the song slots and the WiFi page; it reuses the WiFi report's status codes,
chunking and `serial16`.

| cmd | from the computer | from the FM-1 |
| --- | --- | --- |
| `0x66` report | `off24` | `status total24 off24 serial16 data`: up to 512 bytes of the report from `off` |
| `0x67` run | (nothing), or `wait8` | `status run16`, at once |

`wait8` is the seconds a central has to connect once advertising is on, 0 to 120 (0 skips the connect and MIDI steps);
with no body it is 20. A longer wait or a longer body is status 1. The statuses are the WiFi page's (0 done or taken, 1 bad
request, 2 no run since the FM-1 started, 3 a run is asked for or going on, 4 EDIT is open). A run asked for starts in
the main loop as SAVE, HOME, PLAY would on the pages: the sound stops, the page opens and runs, and stays open on page 1
afterwards (SAVE leaves, back on the WiFi page's first screen if it was opened from there). The report is in RAM until the
next run (numbers little-endian):

| Bytes | What |
| --- | --- |
| 4 | `OPLB` |
| 1 | version, 1 |
| 1 | flags: bit 0 the text was cut short, bit 1 counters or steps were |
| 2 | the run's number |
| 2 | T, then T bytes of text: every line pages 1-12 show (not the `PLAY: PAGE n` footers), a blank line between pages, then `STEPS IN FULL: NAME, RESULT, MS, NOTE` and one line per step with its whole note, then `ADDRESS` (the controller's, `00:00:00:00:00:00` before the HCI step) |
| 1 | K, then K 32-bit counters (24): `tools/fm1_ble_report.py` and `fm1ble_ctr_name` name them |
| 1 | S, then S steps (8): the step (0 radio ... 7 midi_out), its result (0 not run, 1 pass, 2 fail, 3 skip), the evidence bytes it had (16 bits) and kept (16 bits, the same, at most 48), then those bytes |

The tool prints the text and the counters; `--out DIR` writes `report.txt`, `report.bin`, `counters.txt` and
`step-N-<name>.bin` for each step that has evidence.

## Settings

```c
static int settings_load(void *p, uint32_t n);         /* 0 = loaded; else *p is left alone */
static int settings_save(const void *p, uint32_t n);   /* 0 = saved */
```

Up to 256 bytes, kept with their length and a CRC32 in one 4 KiB flash sector (`SETTINGS_AT`, 0xFC000). A
change of size or a broken save reads as "nothing saved": start from your defaults then.

**Save when nothing sounds.** Erasing the sector holds the interrupts off for tens of milliseconds: the sound
drops out. `oplbox.c` saves 2 s after the last change, once all voices are silent. Flash wears out after about
100,000 erases of a sector: never save in a loop.

## Memory and flash

| | | |
| --- | --- | --- |
| Code + constants | 568 KB | runs straight from flash (`const` tables cost no RAM) |
| RAM `.data` + `.bss` | 96 KB | your globals and statics |
| Pool | 336 KB | big buffers, zeroed at boot: `static int16_t delay[65536] __attribute__((section(".pool")));` |
| Stack | 24 KB | keep big arrays off it: `static` |

The build prints what you use and fails above the limits.

Flash (1 MiB) as Dinghy 0.3 lays it out (OPLBOX keeps its 9 song slots at 0x98000, `SLOT_BASE` in `oplbox.c`): the app, the update staging area (0xE0000, the platform's) and the settings
sector (0xFC000). The other firmwares of the family keep their data in 0x97000..0xFBFFF; a new firmware simply
finds its own magic missing and starts fresh. To store more (samples, patterns), widen `FL_RANGE_OK` in
platform.c to the range you pick: it is the guard that keeps a bug from erasing anything else.

## Build options

In platform.c, before `#include "usb.c"`:

| | | |
| --- | --- | --- |
| `FELUCCA_UAC` | 1 | USB audio input (the output to the computer) |
| `FELUCCA_UAC_DEEP` | 1 | a USB audio ring a half deeper (+5.8 ms latency), so a render may take its whole half without a crackle in the recording |
| `FELUCCA_CDC` | 0 | a USB serial console for debugging (some hosts then hide the audio; see usb.c) |
| `FELUCCA_OTA` | 1 | the updater. **Leave it on**: without it nothing can install over your firmware but the rescue |
