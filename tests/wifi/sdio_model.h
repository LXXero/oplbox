/* SPDX-License-Identifier: GPL-3.0-only
 * Host register model of the FM-1's internal SDIO host (docs/WIFI.md "Order": what stages 2 to 5 test
 * against), the fixture format and a diff helper. Own code, written from docs/WIFI.md and the SD
 * Association's SDIO Simplified Specification. Host only: never built into the firmware.
 *
 * The model stands in for the bus below fm1_wifi_sdio_cmd / fm1_wifi_cmd52 / fm1_wifi_cmd53 (the HAL built
 * with -DFM1_WIFI_SDIO_MODEL leaves those out and this file supplies them). It keeps the card's state as the
 * specification describes it (CMD5 until ready, CMD3 gives the RCA, CMD7 selects, CMD52/53 only once
 * selected, no function above the count CMD5 reports), a register file per function (8 x 128 KiB), CCCR I/O
 * Ready following I/O Enable, and read-only CCCR registers that ignore writes. Every command is logged
 * (function, address, data, direction, result), and reads are answered from a script.
 *
 * Fixture format (plain text, one record per line; "#" starts a comment; blank lines are ignored; numbers
 * are hex, with or without 0x; a line that starts with a space or tab continues the previous record's data):
 *
 *   cmd5 0x1f000000 -> 0x90ff8000     a command without data: index (decimal), argument, response
 *   cmd7 0x00020000 ! ebus            ... or "!" and the error it ended with
 *   cmd52 w f0 0x00002 ff             CMD52: r or w, function, 17-bit address, the byte
 *   cmd53 r f1 0x01000 00 00 71 35    CMD53: the bytes, in transfer order; may be written run together
 *   cmd52 r f2 0x00000 ! ebus         a failed read has no data
 *   frame rx ch=6 rssi=-42 80 00 ...  a recorded raw 802.11 frame, rx or tx, optional key=value attributes
 *
 * Errors: ok (the default, not written), enotyet, erange, ebus, etimeout. In an expected fixture "??" matches
 * any byte and "-> ?" any response. A model's log is written in the same format, so a run can be kept as a
 * fixture.
 *
 * Model script (same lexical rules), what the card answers:
 *
 *   ocr 0x90ff8000                    the R4 response to CMD5 (C, function count, memory present, OCR)
 *   rca 0x0001                        the RCA CMD3 offers (several rca lines: one per CMD3, the last stays)
 *   reg f1 0x01000 00 00 71 35        register file contents from this address on
 *   next f1 0x0101c 00 00 02 00       queued answers: a read at this address takes its bytes from the queue
 *                                     first (a polled busy bit, a FIFO), then from the register file
 *   fail cmd52 f1 0x00010 ebus        the next matching command fails with that error (function and address
 *   fail cmd5 etimeout                optional) and has no effect
 */
#pragma once
#include <stdint.h>
#include <stdio.h>

enum { WIFI_REC_CMD, WIFI_REC_CMD52, WIFI_REC_CMD53, WIFI_REC_FRAME };

typedef struct {
    int kind;
    int line;             /* line in its file, 0 for a record a model logged */
    unsigned idx;         /* WIFI_REC_CMD: command index */
    uint32_t arg, resp;   /* WIFI_REC_CMD */
    int resp_any;         /* expected only: "-> ?" */
    int write;            /* CMD52/53: 1 = w; frame: 1 = tx */
    unsigned fn;
    uint32_t addr;
    int status;           /* FM1_WIFI_OK or the error */
    unsigned n;           /* data bytes */
    uint8_t *data, *any;  /* any[i]: "??" in an expected fixture */
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
const char *wifi_status_name(int status);

typedef struct sdio_model sdio_model;
sdio_model *sdio_model_new(void);       /* a card with one function, ready at once, RCA 1 */
void sdio_model_free(sdio_model *m);
int sdio_model_script(sdio_model *m, const char *text, const char *name, FILE *err);
int sdio_model_load(sdio_model *m, const char *path, FILE *err);
void sdio_model_use(sdio_model *m);     /* the model fm1_wifi_sdio_cmd / cmd52 / cmd53 go to */
const wifi_fix *sdio_model_log(const sdio_model *m);
uint8_t sdio_model_peek(const sdio_model *m, unsigned fn, uint32_t addr);   /* the register file, unlogged */
