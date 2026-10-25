/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (the fake host, after Felucca tests/ldr_test.c) */
/* Host test of the update path OPLBOX takes from felucca-bsp (firmware/bsp/chip/ota.c, firmware/bsp/loader/ldr_core.c)
 * against a fake host, with a package made here (no SDK, no device build):
 *  - the handshake answers OPLBOX's identity, and "FM-1_000" in the USB rescue (OTA_IDENTITY, as platform.c sets it);
 *  - ota_service() returns the length of a frame that is not a handshake (for OPLBOX's own commands) and 0 for none;
 *  - the update loader installs the app area and clears the update records, and leaves the song slots
 *    (0x98000..0xE0000) and the settings sector (0xFC000) alone even where their bytes look like an update record
 *    (OPLBOX's old loader erased such a sector);
 *  - a damaged package and a package for another chip key are refused before anything is erased.
 *   cc -Wall -Wno-unused-function -o build/host/loader_test tests/loader_test.c && build/host/loader_test */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FELUCCA_ID "FM-1_960"
#define FELUCCA_OTA_DRYRUN 0
static uint8_t recovery_active;
#define OTA_IDENTITY (recovery_active ? "FM-1_000" : FELUCCA_ID)   /* platform.c */

#define PKG_FL 0x400u                               /* flash.bin's offset in the package */
#define APP_HI 0x93000u                             /* flash.bin's size: the FM-1's app area end */
#define SLOT_LO 0x98000u                            /* OPLBOX's 9 song slots (oplbox.c SLOT_BASE) */
#define SLOT_HI 0xE0000u
#define SETTINGS_AT 0xFC000u                        /* platform.c */

static uint8_t nor[0x100000], logical[PKG_FL + APP_HI];
static uint8_t rx[1024], id_seen[28];
static uint32_t rx_len, rx_full, now_ms, requests, erases, bad_range, record_cleared, f0_asked, id_replies;

static uint32_t pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8;
        while (nb >= 7) { out[o++] = acc & 0x7F; acc >>= 7; nb -= 7; }
    }
    if (nb) out[o++] = acc & 0x7F;
    return o;
}
static uint32_t unpack7h(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 7;
        if (nb >= 8) { out[o++] = (uint8_t)acc; acc >>= 8; nb -= 8; }
    }
    return o;
}
static void host_send(const uint8_t *m, uint32_t n)    /* host -> device: one decoded message, packed */
{
    rx_len = pack7(m, n, rx);
    rx_full = 1;
}

/* device -> host: an identity reply is kept; a read request is answered from the package */
static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    uint8_t u[64], m[600];
    uint32_t addr, len, i, s = 0;
    unpack7h(p + 1, n - 2, u);
    if (u[2] == 0x11) {
        id_replies++;
        memcpy(id_seen, u + 6, 27);
        id_seen[27] = 0;
        return 0;
    }
    addr = u[7] | u[8] << 8 | u[9] << 16 | (uint32_t)u[10] << 24;
    len = u[11] | u[12] << 8 | u[13] << 16;
    requests++;
    memcpy(m, "\x00\x59\x30", 3);
    m[3] = (uint8_t)(len + 8); m[4] = (uint8_t)((len + 8) >> 8); m[5] = 0; m[6] = 0;
    memcpy(m + 7, &addr, 4);
    m[11] = (uint8_t)len; m[12] = (uint8_t)(len >> 8); m[13] = 0;
    if (addr >= 0xE0000000u) {
        f0_asked += addr == 0xF0000000u;
        memset(m + 14, 0, len);
        memcpy(m + 14, "success", 8);
    } else {
        if (addr + len > sizeof logical) { printf("read past the package %#x\n", addr); exit(1); }
        memcpy(m + 14, logical + addr, len);
    }
    for (i = 6; i < 14 + len; i++) s += m[i];
    m[14 + len] = (uint8_t)~s;
    host_send(m, 15 + len);
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n) { if (!rx_full) return 0; *p = rx; *n = rx_len; return 1; }
static void ota_frame_done(void) { rx_full = 0; }
static uint32_t ota_now_ms(void) { return now_ms; }
static void ota_idle(void) { now_ms++; }
static int ota_erase(uint32_t off) { (void)off; return -1; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; return -1; }
static int ota_fread(uint32_t off, void *p, uint32_t n) { memcpy(p, nor + off, n); return 0; }
static void ota_show(uint32_t step, int32_t code) { (void)step; (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm; }
#include "../firmware/bsp/chip/ota.c"

/* the loader's flash: what the device's FL_RANGE_OK lets it do (bsp/loader/loader.c: the app area and update
 * records, never the head, the store [0x97000, 0xE0000) or the settings) */
static int ldr_fread(uint32_t off, void *p, uint32_t n) { memcpy(p, nor + off, n); return 0; }
static int ldr_erase(uint32_t off)
{
    if (off < 0x4000u || off >= SETTINGS_AT || (off & 0xFFFu) || (off >= 0x97000u && off < SLOT_HI)) bad_range++;
    if (off < 0x4000u) { printf("ERASE IN THE HEAD %#x\n", off); exit(1); }
    erases++;
    memset(nor + off, 0xFF, 0x1000);
    return 0;
}
static int ldr_prog(uint32_t off, const void *p, uint32_t n)
{
    const uint8_t *s = p;
    uint32_t i;
    if (off < 0x4000u || off + n > APP_HI || ((off & 0xFFu) + n) > 256u) bad_range++;
    for (i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}
static void ldr_record_clear(void) { record_cleared++; }
static void ldr_progress(uint32_t done, uint32_t total) { (void)done; (void)total; }
#include "../firmware/bsp/loader/ldr_core.c"

static uint32_t rnd = 1;
static uint8_t next8(void) { rnd = rnd * 1103515245u + 12345u; return (uint8_t)(rnd >> 16); }

/* an update record at a 4 KiB boundary - 256, as ldr_core.c finds one: "AT" at bytes 6..7, a CRC16 that matches */
static void put_record(uint32_t at)
{
    uint8_t r[112] = {0};
    r[2] = 0x0D; r[3] = 0x5A; r[4] = 0x01; r[5] = 0x5A; r[6] = 0x41; r[7] = 0x54;
    ota_wr16(r, ota_crc16(r + 2, 78, 0));
    memcpy(nor + at, r, sizeof r);
}

/* the flash head [0, 0x4000) of device and package: a JLFS entry "isd_config.ini" (ENC 0xFFFF, CRC16) at 32
 * pointing at a 34-byte blob with its CRC16, from which ldr_chip_key derives the chip key */
static void make_head(uint8_t *h)
{
    uint8_t *e = h + 32, *blob = h + 0x200;
    uint32_t i;
    memset(h, 0xFF, 0x4000);
    memset(e, 0, 32);
    ota_wr32(e + 4, 0x200);
    ota_wr16(e + 14, 0xFFFF);
    memcpy(e + 16, "isd_config.ini", 15);
    ota_wr16(e, ota_crc16(e + 2, 30, 0));
    ota_jl_enc(e, 32);
    for (i = 0; i < 32u; i++) blob[i] = next8();
    ota_wr16(blob + 32, ota_crc16(blob, 32, 0));
}

/* the package (the logical image the host serves): a UFW header with one entry, flash.bin at PKG_FL whose app
 * area decrypts with key; its CRC16 in the entry */
static void make_package(uint32_t key)
{
    uint8_t *fb = logical + PKG_FL, *hdr = logical, *ent = logical + 0x40;
    uint32_t i;
    memset(logical, 0, sizeof logical);
    memcpy(fb, nor, 0x4000);
    for (i = 0x4000u; i < APP_HI; i++) fb[i] = next8();
    ota_wr16(fb + 0x4000 + 0, ota_crc16(fb + 0x4002, 30, 0));   /* the app head: a CRC16 once decrypted */
    ldr_sfc(fb + 0x4000, 32, 0, key);
    memset(ent, 0, 0x50);
    ota_wr16(ent, 0);                                         /* type 0: flash.bin */
    ota_wr16(ent + 4, ota_crc16(fb, APP_HI, 0));
    ota_wr32(ent + 8, PKG_FL);
    ota_wr32(ent + 12, APP_HI);
    ota_jl_enc(ent, 0x50);
    ota_wr16(hdr + 8, 1);
    ota_wr16(hdr + 2, ota_crc16(ent, 0x50, 0));               /* over the entries as stored */
    ota_wr16(hdr, ota_crc16(hdr + 2, 0x3E, 0));
    ota_jl_enc(hdr, 0x40);
}

static int check(const char *what, int ok) { printf("%-72s %s\n", what, ok ? "ok" : "FAIL"); return ok ? 0 : 1; }

int main(void)
{
    static uint8_t slots[SLOT_HI - SLOT_LO], settings[0x1000], head[0x4000];
    uint32_t key, i;
    int bad = 0, rc;

    /* the handshake and the firmware's own frames (ota_service, outside a session) */
    {
        static const uint8_t HS[7] = {0x00, 0x59, 0x11, 0, 0, 0, 0xFF};
        static const uint8_t APP[10] = {0x00, 0x59, 0x50, 3, 0, 0, 1, 2, 3, (uint8_t)~6u};
        uint32_t d;
        host_send(HS, sizeof HS);
        d = ota_service();
        bad += check("handshake answers OPLBOX's identity FM-1_960", d == 7u && id_replies == 1u &&
                     !strcmp((char *)id_seen, "FM-1_960"));
        recovery_active = 1;
        host_send(HS, sizeof HS);
        (void)ota_service();
        bad += check("in the USB rescue it answers FM-1_000", id_replies == 2u && !strcmp((char *)id_seen, "FM-1_000"));
        recovery_active = 0;
        host_send(APP, sizeof APP);
        d = ota_service();
        bad += check("a frame of the firmware's own (cmd 0x50) comes back: its length, the body in ota_dec",
                     d == 10u && ota_dec[2] == 0x50 && ota_dec[6] == 1 && ota_dec[8] == 3 && id_replies == 2u);
        bad += check("no frame: ota_service returns 0", ota_service() == 0u);
    }

    /* the device: an older app installed, the staged loader's update record (0xE4F00) and the stock firmware's
     * (0xE8F00), songs in slots 1 and 9 and settings, each with a sector whose last 256 bytes look like a record */
    memset(nor, 0xFF, sizeof nor);
    make_head(nor);
    memcpy(head, nor, sizeof head);
    if ((rc = ldr_chip_key(&key)) != 0) { printf("chip key: %d\n", rc); return 1; }
    for (i = 0x4000u; i < APP_HI; i++) nor[i] = next8();
    put_record(0xE4F00);
    put_record(0xE8F00);
    for (i = 0; i < 0x8000u; i++) nor[SLOT_LO + i] = nor[SLOT_HI - 0x8000u + i] = next8();
    put_record(SLOT_LO + 0xF00u);                  /* slot 1, its first sector */
    put_record(SLOT_HI - 0x100u);                  /* slot 9, its last sector */
    for (i = 0; i < 0x100u; i++) nor[SETTINGS_AT + i] = next8();
    put_record(SETTINGS_AT + 0xF00u);
    memcpy(slots, nor + SLOT_LO, sizeof slots);
    memcpy(settings, nor + SETTINGS_AT, sizeof settings);
    make_package(key);

    /* a damaged package (one byte deep in the app area): refused before anything is erased */
    logical[PKG_FL + 0x50123u] ^= 0x01;
    erases = 0;
    rc = ldr_session();
    bad += check("damaged package (flash.bin CRC) refused, nothing erased", rc == -12 && erases == 0);
    logical[PKG_FL + 0x50123u] ^= 0x01;

    /* a package for another chip key: refused before anything is erased */
    logical[PKG_FL + 0x4000u + 5u] ^= 0x5A;
    erases = 0;
    rc = ldr_session();
    bad += check("package for another chip key refused, nothing erased", rc == -6 && erases == 0);
    logical[PKG_FL + 0x4000u + 5u] ^= 0x5A;

    erases = 0;
    requests = 0;
    rc = ldr_session();
    printf("  rc %d, %u requests, %u sector erases\n", rc, requests, erases);
    bad += check("install completes", rc == 0);
    bad += check("app area == the package's", !memcmp(nor + 0x4000, logical + PKG_FL + 0x4000, APP_HI - 0x4000));
    bad += check("flash head [0, 0x4000) untouched", !memcmp(nor, head, sizeof head));
    bad += check("finish asked (0xF0000000)", f0_asked >= 1u);
    bad += check("update records cleared (RAM, flash 0xE4F00 and the stock firmware's 0xE8F00)",
                 record_cleared && nor[0xE4F06] == 0xFF && nor[0xE8F06] == 0xFF);
    bad += check("song slots 0x98000..0xE0000 kept, records lookalikes and all", !memcmp(nor + SLOT_LO, slots, sizeof slots));
    bad += check("settings sector 0xFC000 kept", !memcmp(nor + SETTINGS_AT, settings, sizeof settings));
    bad += check("no erase or write outside the loader's windows", bad_range == 0);

    erases = 0;
    rc = ldr_session();
    bad += check("re-run with the same package erases nothing", rc == 0 && erases == 0);

    printf("%s\n", bad ? "LOADER TEST FAILED" : "loader test passed");
    return bad != 0;
}
