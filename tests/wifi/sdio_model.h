/* SPDX-License-Identifier: GPL-3.0-only
 * Host register model of the FM-1's WiFi core as the CPU sees it, the fixture format and a diff helper
 * (docs/WIFI.md "Order": what stages 2 to 5 test against). Own code, written from docs/WIFI.md. Host only:
 * never built into the firmware. (The name is the bus's: JieLi calls the link between CPU and core SDIO.)
 *
 * The model is fm1wifi's port for the host (firmware/wifi/fm1wifi_port.h): it supplies fm1wifi_port_rd32,
 * _wr32, _clk, _delay_us and _trim, so the library runs unchanged against it. It keeps a register file of CPU
 * addresses (the JL_ANA and JL_WL blocks, the core's window at 0x30000 with the WL30 host block in it), plays
 * the baseband behind BBP_CSR_CFG (a write with BUSY set reads or writes the model's BBP register and clears
 * BUSY), and logs every call. An access to any other address is counted as bad (sdio_model_bad): the library
 * must never make one. Reads are answered from a script.
 *
 * Stage 3 (docs/WIFI_STAGE3.md, "The register model's answers"), the same model as tools/wifi_ir.py's (keep the
 * two in step): the four BBP windows behind BBP registers 0xC8-0xD7 (a strobe of 1 writes the window, 2 reads it
 * into the read register), and made-up answers for what the radio's bring-up reads back: the status read
 * (WLA_CON30, selector 1: the filter tune done at once with code (WLA_CON26 bits 9-1) * 5 / 9, the VCO result 2
 * below 250 - 2 * cap, 0 for the next five dividers, 1 above), the measurement (RX front end 0x81-0x8C, a linear
 * function of the trim settings in force, ready at once), the ADC peak (RX front end 0x80) and the RF digital
 * block, never busy, and the channel survey's counters (RX_STA_CNT1, CH_BUSY_STA: made-up access points on
 * channels 1, 6 and 11, counted per millisecond of delay, cleared when read; host only, the trace never reads them).
 * None of it is measured. Besides the full log, the model keeps a write log at window level
 * (sdio_model_wlog): CPU and MAC register writes (not BBP_CSR_CFG), BBP writes other than the windows' own
 * registers, window writes and delays; reads left out. tools/wifi_ir.py --trace writes the same records for
 * JieLi's code, and tests/wifi_model.c diffs the two.
 *
 * Fixture format (plain text, one record per line; "#" starts a comment; blank lines are ignored; numbers are
 * hex, with or without 0x, except a delay's microseconds; a line that starts with a space or tab continues
 * the previous frame's data):
 *
 *   wr32 0x14040 0x00000100           a register write: CPU address, value
 *   rd32 0x31000 -> 0x35920200        a register read and what it returned
 *   clk 0x0000 -> 0xc000              fm1wifi_port_clk: the WiFi bits of CLK_CON1 asked for, and what they were
 *   delay 50                          fm1wifi_port_delay_us, in microseconds (decimal)
 *   bbp 0x16 0x00                     a BBP register write (write log): register, value
 *   win txbe 0x5c 0x10                a BBP window write (write log): cca, rx11n, txbe or rxfe, address, value
 *   frame rx ch=6 rssi=-42 80 00 ...  a recorded raw 802.11 frame, rx or tx, optional key=value attributes
 *
 * In an expected fixture "-> ?" matches any value read and "??" any frame byte. A model's log is written in
 * the same format, so a run can be kept as a fixture.
 *
 * Model script (same lexical rules), what the core answers:
 *
 *   reg 0x31000 0x35920200            a register's value until the library writes it
 *   next 0x3101c 0x00020000 0x0       queued answers: reads at this address take these first (a polled busy
 *                                     bit), then the register
 *   stuck 0x3101c 0x00020000          every read at this address answers this, whatever was written
 *   bbp 0x00 0x21                     a BBP register (read through BBP_CSR_CFG)
 *   clk 0xc000                        CLK_CON1's WiFi bits before the library changes them
 *   hang tune|measure|anl             that wait never ends: the filter tune is never done, the measurement
 *                                     never ready, the RF digital block always busy
 *   hang measure after N              the first N measurements answer, then none is ever ready
 *   hang rxdma|txdma                  that frame FIFO's DMA never finishes (stages 4-5)
 *   rxbad                             the first received frame's length word reads 0
 *   ap ch=6 bssid=02:00:00:00:00:06 ssid=OPLBOX [sec=open|wep|wpa|wpa2] [rssi=-40] [hidden]
 *      [auth=accept|refuse|silent] [assoc=accept|refuse|silent] [kick=MS] [gone=MS]
 *                                     an access point (8 at most): a beacon every 100 TU on its channel while the
 *                                     library listens there, a probe response to a probe request for any network
 *                                     (not when hidden) or for its SSID, open-system authentication and association
 *                                     (AID 1) answered as given, and kick / gone: a deauthentication, or no more
 *                                     beacons, that many ms after the association
 *   ap ... sec=wpa2 psk=PASSPHRASE [eapol=normal|silent|badmic] [rekey=MS] [bcast=MS] [rsn=HEX]
 *                                     WPA2-PSK (stage 8, docs/WIFI_STAGE8.md): after the association the AP runs the
 *                                     4-way handshake (tests/wifi/wpa_ap.c, the PMK from PASSPHRASE and the SSID) as
 *                                     EAPOL data frames, msg 1 at once, msg 1 or 3 again after 1 s, 4 tries, then a
 *                                     deauthentication (reason 15); silent: no msg 1; badmic: msg 3 with a bad MIC.
 *                                     rekey: a group key handshake (a new GTK, key ID 1 <-> 2) that many ms after it;
 *                                     bcast: a protected broadcast frame (an ARP request) every MS. rsn: the RSN
 *                                     element in its beacons, whole (default CCMP / CCMP / PSK).
 *                                     The MAC's key tables are played as the hardware would use them: a protected
 *                                     frame from the AP reaches the library decrypted (RXD bit 16, IV and MIC off,
 *                                     the RXWI's WCID the AP's entry) only when MAC_WCID_ENTRY holds the AP's address
 *                                     with its attribute pairwise AES and the TK in its PAIRWISE_KEY_ENTRY (a group
 *                                     frame: the GTK in SHARED_KEY_ENTRY(key ID), AES in SHARED_KEY_MODE_ENTRY(0)'s
 *                                     bits, WCID 0's attribute shared AES); else as it came over the air with a key
 *                                     error (RXD bits 10-9 = 3). A protected frame the library sends is taken only
 *                                     when JieLi's WIV is 0 and the TXWI's WCID holds the TK as above (with Ext IV in
 *                                     its IV/EIV entry); after the 4-way handshake nothing is taken in the clear.
 *
 * Stages 4-6 (docs/WIFI_STAGE4.md): the WL30 host block's DMA moves frames between the model's RX FIFO or its
 * access points and the library's buffers (fm1wifi_port_bus hands out the addresses), 0x04's bits 17, 20 and 21
 * answer as the core's (frame waiting, TX done, RX done; bits 10 and 11 clear the done bits), and frames reach the
 * library only once the link is up (MAC_SYS_CTRL's RX on, RX_FILTER_CFG written) and pass its filter (not to our
 * address: dropped). The model's clock is the library's delays added up. PBF_DBG reads what the script gives it.
 *
 * A poll that gets the same busy answer SDIO_MODEL_POLL_MAX times in a row ends the program (exit 3, "unbounded
 * wait"): a test of a wait that lost its bound fails instead of hanging.
 */
#define SDIO_MODEL_POLL_MAX 100000
#pragma once
#include <stdint.h>
#include <stdio.h>

enum { WIFI_REC_RD, WIFI_REC_WR, WIFI_REC_CLK, WIFI_REC_DELAY, WIFI_REC_FRAME, WIFI_REC_BBP, WIFI_REC_WIN };

typedef struct {
    int kind;
    int line;             /* line in its file, 0 for a record a model logged */
    uint32_t addr, val;   /* RD / WR: address and value; CLK: bits asked for and (val) the old ones; DELAY: us;
                             BBP: register, value; WIN: window << 8 | address, value */
    int any;              /* expected only: "-> ?" */
    int write;            /* frame: 1 = tx */
    unsigned n;           /* frame data bytes */
    uint8_t *data, *bany; /* bany[i]: "??" in an expected fixture */
    char attr[64];        /* frame: its key=value attributes, space-separated */
} wifi_rec;

typedef struct {
    wifi_rec *r;
    int n, cap;
} wifi_fix;

/* 0, or -1 after printing "name:line: what" to err. */
int wifi_fix_parse(wifi_fix *f, const char *text, const char *name, FILE *err);
int wifi_fix_load(wifi_fix *f, const char *path, FILE *err);
void wifi_fix_free(wifi_fix *f);
void wifi_fix_write(FILE *out, const wifi_fix *f);
void wifi_rec_write(FILE *out, const wifi_rec *r, int one_line);
int wifi_rec_match(const wifi_rec *want, const wifi_rec *got);
/* Compare a run with an expected fixture, report each difference (missing from the run, extra in it, or
 * different) with its line, and return how many there were (0: the run matches). */
int wifi_fix_diff(FILE *out, const wifi_fix *want, const wifi_fix *got);

typedef struct sdio_model sdio_model;
sdio_model *sdio_model_new(void);       /* every register 0, CLK_CON1's WiFi bits 0, no BBP values */
void sdio_model_free(sdio_model *m);
int sdio_model_script(sdio_model *m, const char *text, const char *name, FILE *err);
int sdio_model_load(sdio_model *m, const char *path, FILE *err);
void sdio_model_use(sdio_model *m);     /* the model the port functions go to */
const wifi_fix *sdio_model_log(const sdio_model *m);
uint32_t sdio_model_peek(const sdio_model *m, uint32_t addr);   /* the register file, unlogged */
uint32_t sdio_model_clk(const sdio_model *m);                   /* CLK_CON1's WiFi bits now */
uint8_t sdio_model_bbp(const sdio_model *m, uint8_t reg);
int sdio_model_bad(const sdio_model *m);   /* accesses outside the WiFi blocks, or not word-aligned */
const wifi_fix *sdio_model_wlog(const sdio_model *m);   /* the write log (stage 3) */
void sdio_model_wlog_clear(sdio_model *m);
void sdio_model_zero(sdio_model *m);   /* every register 0 (the trace starts from there), the windows too */
void sdio_model_raw_log(sdio_model *m, int on);          /* the full log on (default) or off (long runs) */
uint8_t sdio_model_win(const sdio_model *m, unsigned w, uint8_t a);   /* a window register, unlogged */
int sdio_model_trims(const sdio_model *m);   /* fm1wifi_port_trim(1) calls so far; -1 if 0 came without a 1 */
int sdio_model_in_trim(const sdio_model *m);
unsigned sdio_model_polls(const sdio_model *m);   /* the longest run of busy answers to one wait */
/* fn(ctx, addr, v) at every fm1wifi_port_wr32, before the model takes the write (NULL: none) */
void sdio_model_on_wr(sdio_model *m, void (*fn)(void *ctx, uint32_t addr, uint32_t v), void *ctx);
const wifi_fix *sdio_model_frames(const sdio_model *m);   /* frames sent (tx) and handed to the library (rx) */
uint64_t sdio_model_now_us(const sdio_model *m);          /* the model's clock */
/* access point i (in script order), WPA2: the 4-way handshake done and the station still joined, group key handshakes
 * finished, station frames refused, the PTK it agreed (or NULL) */
void sdio_model_ap_wpa(const sdio_model *m, int i, int *done, int *group_done, uint32_t *refused, uint8_t ptk[48]);
