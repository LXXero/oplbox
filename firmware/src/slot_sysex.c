/* SPDX-License-Identifier: GPL-3.0-only */
/* The song slots' .CMF files over USB SysEx (TODO 2a), in the updater's framing (ota.c):
 * F0 pack7(00 59 cmd len24 body ~sum(body)) F7.
 *
 *   0x60 pull  host: slot off24          device: slot status total24 off24 data (512 bytes at most)
 *   0x61 push  host: slot len24 crc32    the device reads the file from the host with the updater's own read
 *                                        requests (cmd 0x30, the file at address 0, 512 bytes a request), checks
 *                                        it and writes the slot; then device: slot status
 *
 * slot 0..8 is the jukebox's SLOT 1..9; the file is the song's bytes, the 16-byte slot header stays on the device.
 * docs/API.md has the tables and the status codes. The command bytes are none of the stock updater's (0x11, 0x30)
 * or fm1-base's (0x41-0x43 backup and restore, 0x50-0x54 the mirror); Felucca's editor has its own header
 * (F0 7D 46 4C), so it never reaches here.
 *
 * Include it after ota.c and after the app's N_SLOTS, SLOT_SIZE, SLOT_HDR, SLOT_MAGIC, slot_buf, slot_ptr(i),
 * slot_len(i), crc32(p, n), wr32(p, v), say(text), and:
 *   slot_in_player(i)   1: slot i is the song in the player (its edits live in RAM): a push to it is refused
 *   slot_store(i, n)    program slot i from slot_buf (header, then n bytes), the sound stopped as SAVE does; 0 = ok
 */
#define SLOT_CMD_PULL 0x60u
#define SLOT_CMD_PUSH 0x61u
#define SLOT_CHUNK 512u                         /* the updater's read size: a reply is 605 wire bytes */
#define SLOT_MIN_CMF (5u + 198u + 8u)           /* the smallest file macsop_check takes */
#define SLOT_PUSH_MS 30000u                     /* a push whose host stops answering gives up after this (the request
                                                 * in flight still has its 3 x 5 s) */
enum { SLOT_OK, SLOT_E_REQ, SLOT_E_EMPTY, SLOT_E_BIG, SLOT_E_PLAYING, SLOT_E_XFER, SLOT_E_CMF, SLOT_E_FLASH };

static void slot_wr24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); }

/* one reply, built in the top 2 KiB of slot_buf: nothing else uses it then (a pull reads the slot from flash, a
 * push replies once its file is written or refused) */
static int slot_reply(uint32_t cmd, const uint8_t *head, uint32_t nh, const uint8_t *data, uint32_t nd)
{
    uint8_t *m = slot_buf + SLOT_SIZE - 2048u, *w = slot_buf + SLOT_SIZE - 1024u;
    uint32_t i, s = 0, n = nh + nd, k;
    m[0] = 0;
    m[1] = 0x59;
    m[2] = (uint8_t)cmd;
    slot_wr24(m + 3, n);
    for (i = 0; i < nh; i++) m[6 + i] = head[i];
    for (i = 0; i < nd; i++) m[6 + nh + i] = data[i];
    for (i = 0; i < n; i++) s += m[6 + i];
    m[6 + n] = (uint8_t)~s;
    w[0] = 0xF0;
    k = 1u + ota_pack7(m, 7u + n, w + 1);
    w[k++] = 0xF7;
    return ota_wire_send(w, k);
}

static void slot_pull(const uint8_t *d, uint32_t n)
{
    uint8_t h[8];
    uint32_t i = n == 11u ? d[6] : N_SLOTS, off = n == 11u ? ota_rd24(d + 7) : 0, total = 0, k = 0, st;
    if (i >= N_SLOTS) st = SLOT_E_REQ;
    else if (!(total = slot_len(i))) st = SLOT_E_EMPTY;
    else if (off > total) st = SLOT_E_REQ;
    else {
        st = SLOT_OK;
        k = total - off > SLOT_CHUNK ? SLOT_CHUNK : total - off;
    }
    h[0] = (uint8_t)(i < N_SLOTS ? i : 0x7Fu);
    h[1] = (uint8_t)st;
    slot_wr24(h + 2, total);
    slot_wr24(h + 5, off);
    slot_reply(SLOT_CMD_PULL, h, sizeof h, k ? slot_ptr(i) + SLOT_HDR + off : 0, k);
}

static void slot_push(const uint8_t *d, uint32_t n)
{
    static char said[24] = "USB: SLOT 1 NOT SAVED";
    uint8_t h[2];
    uint32_t i = n == 15u ? d[6] : N_SLOTS, len = n == 15u ? ota_rd24(d + 7) : 0, crc = n == 15u ? ota_rd32(d + 10) : 0;
    uint32_t off, k, st;
    if (i >= N_SLOTS) st = SLOT_E_REQ;
    else if (len > SLOT_SIZE - SLOT_HDR) st = SLOT_E_BIG;
    else if (len < SLOT_MIN_CMF) st = SLOT_E_CMF;
    else if (slot_in_player(i)) st = SLOT_E_PLAYING;
    else {
        st = SLOT_OK;
        ota_deadline = ota_now_ms() + SLOT_PUSH_MS;
        for (off = 0; off < len && st == SLOT_OK; off += k) {
            k = len - off > SLOT_CHUNK ? SLOT_CHUNK : len - off;
            if (ota_read(off, slot_buf + SLOT_HDR + off, k)) st = SLOT_E_XFER;
        }
        ota_deadline = 0;
        if (st == SLOT_OK && crc32(slot_buf + SLOT_HDR, len) != crc) st = SLOT_E_XFER;
        if (st == SLOT_OK && macsop_check(slot_buf + SLOT_HDR, len) < 0) st = SLOT_E_CMF;
        if (st == SLOT_OK) {
            wr32(slot_buf, SLOT_MAGIC); wr32(slot_buf + 4, len); wr32(slot_buf + 8, crc); wr32(slot_buf + 12, 0);
            if (slot_store(i, len)) st = SLOT_E_FLASH;
        }
    }
    if (i < N_SLOTS) {                          /* the screen: what the computer did to which slot */
        const char *t = st == SLOT_OK ? " SAVED" : st == SLOT_E_PLAYING ? " IS PLAYING" : " NOT SAVED";
        uint32_t j = 11u;
        said[10] = (char)('1' + i);
        while (*t) said[j++] = *t++;
        said[j] = 0;
        say(said);
    }
    h[0] = (uint8_t)(i < N_SLOTS ? i : 0x7Fu);
    h[1] = (uint8_t)st;
    slot_reply(SLOT_CMD_PUSH, h, sizeof h, 0, 0);
}

/* a frame in the updater's framing that is not the updater's (d: decoded, n: its length); 1 = it was ours */
static int slot_sysex(const uint8_t *d, uint32_t n)
{
    if (n < 7u) return 0;
    if (d[2] == SLOT_CMD_PULL) { slot_pull(d, n); return 1; }
    if (d[2] == SLOT_CMD_PUSH) { slot_push(d, n); return 1; }
    return 0;
}
