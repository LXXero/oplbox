/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of firmware/ble/hci.c (docs/BLUETOOTH.md stage 1): the packets byte for byte, the event decoder and what
 * it refuses, the H4 reader, then the host against the fake controller (tests/ble/hci_model.c): a whole session
 * diffed against tests/ble/session_expect.txt (reset, both event masks, the buffer size, the advertising parameters,
 * payloads and enable, LE Connection Complete, ACL in and out with flow control, a connection update, a
 * disconnection either way and advertising again), the same with the controller answering inside the send call,
 * and the ways a start fails. Exits 1 when a check fails.
 *
 * Usage: ble_hci <dir with session.txt and the other scripts>
 */
#include <stdio.h>
#include <string.h>
#include "hci.h"
#include "hci_model.h"

static int bad, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fprintf(stderr, "FAIL: %s\n", what); bad = 1; } }
static const char *dir;

static int eq(const uint8_t *a, const uint8_t *b, uint32_t n) { return memcmp(a, b, n) == 0; }

/* ---- packets ---- */

static void packets(void)
{
    uint8_t p[300], d[300];
    hci_evt e;
    uint16_t handle, len;
    uint8_t pb;
    const uint8_t *data;

    static const uint8_t reset[] = { 0x03, 0x0C, 0x00 };
    check(hci_cmd_pack(p, HCI_OP_RESET, NULL, 0) == 3 && eq(p, reset, 3), "Reset packs as 03 0c 00");
    static const uint8_t disc[] = { 0x06, 0x04, 0x03, 0x40, 0x00, 0x13 }, dpar[] = { 0x40, 0x00, 0x13 };
    check(hci_cmd_pack(p, HCI_OP_DISCONNECT, dpar, 3) == 6 && eq(p, disc, 6), "Disconnect packs opcode, length, parameters");
    check(HCI_OP(0x08, 0x000A) == HCI_OP_LE_SET_ADV_ENABLE && HCI_OP(0x03, 0x0003) == HCI_OP_RESET, "OGF << 10 | OCF");
    memset(d, 0, sizeof d);
    check(hci_cmd_pack(p, HCI_OP_RESET, d, 256) == 0, "a command with 256 parameter bytes is refused");

    static const uint8_t ab[] = { 0xAA, 0xBB };
    static const uint8_t acl0[] = { 0x40, 0x00, 0x02, 0x00, 0xAA, 0xBB }, acl1[] = { 0x41, 0x10, 0x02, 0x00, 0xAA, 0xBB };
    check(hci_acl_pack(p, 0x0040, HCI_PB_FIRST_NOFLUSH, ab, 2) == 6 && eq(p, acl0, 6), "ACL: handle, PB 00, length, data");
    check(hci_acl_pack(p, 0xF041, HCI_PB_CONT, ab, 2) == 6 && eq(p, acl1, 6), "ACL: PB 01 in bits 13-12, handle to 12 bits");
    check(hci_acl_pack(p, 0x0040, 0, d, HCI_ACL_DATA_MAX + 1) == 0, "ACL data over 251 bytes is refused");
    static const uint8_t acl2[] = { 0x40, 0xE0, 0x02, 0x00, 0xAA, 0xBB };
    check(hci_acl_parse(acl2, 6, &handle, &pb, &data, &len) == HCI_OK && handle == 0x040 && pb == HCI_PB_FIRST_FLUSH &&
          len == 2 && data == acl2 + 4, "an ACL packet's handle, PB 10 (broadcast bits ignored) and data");
    check(hci_acl_parse(acl2, 5, &handle, &pb, &data, &len) == HCI_EINVAL, "an ACL packet shorter than its length field");
    check(hci_acl_parse(acl2, 3, &handle, &pb, &data, &len) == HCI_EINVAL, "an ACL packet shorter than its header");

    static const uint8_t cc[] = { 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00 };
    check(hci_evt_parse(cc, 6, &e) == HCI_OK && e.code == HCI_EV_CMD_COMPLETE && e.ncmd == 1 && e.opcode == HCI_OP_RESET &&
          e.status == 0 && e.nret == 0, "Command Complete for Reset");
    static const uint8_t cc_buf[] = { 0x0E, 0x07, 0x01, 0x02, 0x20, 0x00, 0x1B, 0x00, 0x02 };
    check(hci_evt_parse(cc_buf, 9, &e) == HCI_OK && e.nret == 3 && e.ret == cc_buf + 6, "Command Complete's return parameters");
    static const uint8_t cc_nop[] = { 0x0E, 0x03, 0x05, 0x00, 0x00 };
    check(hci_evt_parse(cc_nop, 5, &e) == HCI_OK && e.ncmd == 5 && e.opcode == 0 && e.ret == NULL, "Command Complete, no opcode");
    static const uint8_t cs[] = { 0x0F, 0x04, 0x0C, 0x01, 0x06, 0x04 };
    check(hci_evt_parse(cs, 6, &e) == HCI_OK && e.status == 0x0C && e.ncmd == 1 && e.opcode == HCI_OP_DISCONNECT, "Command Status");
    static const uint8_t dc[] = { 0x05, 0x04, 0x00, 0x40, 0x00, 0x13 };
    check(hci_evt_parse(dc, 6, &e) == HCI_OK && e.handle == 0x40 && e.reason == 0x13, "Disconnection Complete");
    static const uint8_t ncp[] = { 0x13, 0x09, 0x02, 0x40, 0x00, 0x01, 0x00, 0x41, 0x00, 0x03, 0x00 };
    check(hci_evt_parse(ncp, 11, &e) == HCI_OK && e.npairs == 2 && e.pairs == ncp + 3, "Number Of Completed Packets, two handles");
    static const uint8_t lcc[] = { 0x3E, 0x13, 0x01, 0x00, 0x40, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0xEE, 0xFF, 0xC0,
                                   0x18, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00 };
    static const uint8_t peer[] = { 0x01, 0x00, 0x00, 0xEE, 0xFF, 0xC0 };
    check(hci_evt_parse(lcc, sizeof lcc, &e) == HCI_OK && e.sub == HCI_LE_CONN_COMPLETE && e.handle == 0x40 && e.role == 1 &&
          e.peer_type == 1 && eq(e.peer, peer, 6) && e.interval == 24 && e.latency == 0 && e.timeout == 72,
          "LE Connection Complete's fields");
    static const uint8_t lcu[] = { 0x3E, 0x0A, 0x03, 0x00, 0x40, 0x00, 0x0C, 0x00, 0x01, 0x00, 0x48, 0x00 };
    check(hci_evt_parse(lcu, sizeof lcu, &e) == HCI_OK && e.sub == HCI_LE_CONN_UPDATE_COMPLETE && e.interval == 12 &&
          e.latency == 1 && e.timeout == 72, "LE Connection Update Complete's fields");
    static const uint8_t hw[] = { 0x10, 0x01, 0x07 };
    check(hci_evt_parse(hw, 3, &e) == HCI_OK && e.status == 7, "Hardware Error's code");
    static const uint8_t other[] = { 0xFF, 0x02, 0x01, 0x02 };
    check(hci_evt_parse(other, 4, &e) == HCI_OK && e.code == 0xFF, "an event the host does not use decodes to its code");

    check(hci_evt_parse(cc, 5, &e) == HCI_EINVAL, "an event shorter than its length byte");
    check(hci_evt_parse(cc, 1, &e) == HCI_EINVAL, "an event shorter than its header");
    static const uint8_t cc_short[] = { 0x0E, 0x02, 0x01, 0x03 };
    check(hci_evt_parse(cc_short, 4, &e) == HCI_EINVAL, "a Command Complete without its opcode");
    static const uint8_t cs_short[] = { 0x0F, 0x03, 0x00, 0x01, 0x06 };
    check(hci_evt_parse(cs_short, 5, &e) == HCI_EINVAL, "a Command Status of 3 bytes");
    check(hci_evt_parse(lcc, sizeof lcc - 1, &e) == HCI_EINVAL, "an LE Connection Complete one byte short");
    static const uint8_t ncp_bad[] = { 0x13, 0x05, 0x02, 0x40, 0x00, 0x01, 0x00 };
    check(hci_evt_parse(ncp_bad, 7, &e) == HCI_EINVAL, "a Number Of Completed Packets whose count disagrees with its length");
    static const uint8_t meta_empty[] = { 0x3E, 0x00 };
    check(hci_evt_parse(meta_empty, 2, &e) == HCI_EINVAL, "an LE Meta event without a subevent");

    uint8_t ad[31], n = 0;
    static const uint8_t flags = 0x06, ad_want[] = { 2, 1, 6, 7, 9, 'O', 'P', 'L', 'B', 'O', 'X' };
    check(hci_ad_add(ad, &n, 0x01, &flags, 1) == HCI_OK && hci_ad_add(ad, &n, 0x09, "OPLBOX", 6) == HCI_OK && n == 11 &&
          eq(ad, ad_want, 11), "AD structures: length, type, data");
    check(hci_ad_add(ad, &n, 0xFF, d, 19) == HCI_EINVAL && n == 11, "an AD structure past 31 bytes is refused, nothing written");
    check(hci_ad_add(ad, &n, 0xFF, d, 18) == HCI_OK && n == 31, "an AD structure up to exactly 31 bytes");

    check(!strcmp(hci_op_name(HCI_OP_LE_SET_ADV_PARAMS), "LE SET ADV PARAMS") && !strcmp(hci_op_name(0x1234), "?"),
          "command names");
}

/* ---- H4 ---- */

static uint8_t got[8][HCI_H4_MAX], got_t[8];
static uint16_t got_n[8];
static int ngot;
static void emit(void *ctx, uint8_t type, const uint8_t *p, uint16_t n)
{
    (void)ctx;
    if (ngot < 8) {
        got_t[ngot] = type;
        memcpy(got[ngot], p, n);
        got_n[ngot++] = n;
    }
}

static void h4(void)
{
    static const uint8_t cmd[] = { 0x03, 0x0C, 0x00 }, ev[] = { 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00 },
                         acl[] = { 0x40, 0x20, 0x03, 0x00, 1, 2, 3 }, iso[] = { 0x40, 0x00, 0x02, 0x40, 9, 9 };
    uint8_t s[2000];
    uint32_t n = 0;
    hci_h4 r;

    n += hci_h4_pack(s + n, HCI_CMD, cmd, sizeof cmd);
    s[n++] = 0x77;                                     /* not a packet indicator */
    n += hci_h4_pack(s + n, HCI_EVT, ev, sizeof ev);
    n += hci_h4_pack(s + n, HCI_ACL, acl, sizeof acl);
    for (int way = 0; way < 2; way++) {               /* all at once, then a byte at a time */
        hci_h4_init(&r);
        ngot = 0;
        if (way == 0)
            hci_h4_feed(&r, s, n, emit, NULL);
        else
            for (uint32_t i = 0; i < n; i++)
                hci_h4_feed(&r, s + i, 1, emit, NULL);
        check(ngot == 3 && got_t[0] == HCI_CMD && got_n[0] == 3 && eq(got[0], cmd, 3) && got_t[1] == HCI_EVT &&
              got_n[1] == 6 && eq(got[1], ev, 6) && got_t[2] == HCI_ACL && got_n[2] == 7 && eq(got[2], acl, 7),
              way ? "H4: three packets a byte at a time" : "H4: three packets in one piece");
        check(r.dropped == 1, "H4: the stray byte between them dropped");
    }
    /* an ACL packet of 300 bytes is longer than the host takes: thrown away whole, the next packet still read; the
     * ISO length is 14 bits (0x4002 -> 2) */
    hci_h4_init(&r);
    ngot = 0;
    n = 0;
    s[n++] = HCI_ACL; s[n++] = 0x40; s[n++] = 0x20; s[n++] = 0x2C; s[n++] = 0x01;
    memset(s + n, 0x55, 300);
    n += 300;
    n += hci_h4_pack(s + n, HCI_ISO, iso, sizeof iso);
    hci_h4_feed(&r, s, n, emit, NULL);
    check(ngot == 1 && got_t[0] == HCI_ISO && got_n[0] == 6, "H4: an over-long packet skipped, the next one read");
    check(r.dropped == 305, "H4: the over-long packet's 305 bytes counted");
}

/* ---- the host against the fake controller ---- */

typedef struct {
    hci_model m;
    hci_host h;
    hci_transport t;
    int l2cap, disc_at;                 /* PDUs the test saw; hci_host_disconnect on the connects == disc_at one */
    uint16_t last_cid, nlast;
    uint8_t last[HCI_L2CAP_MAX];
} rig;

static rig R;

static void to_host(void *ctx, uint8_t type, const uint8_t *p, uint16_t n) { hci_host_receive(&((rig *)ctx)->h, type, p, n); }

/* What a GATT server would do: answer an MTU request with 23, a write with a 70-byte notification. */
static void on_l2cap(void *ctx, uint16_t handle, uint16_t cid, const uint8_t *p, uint16_t n)
{
    rig *r = ctx;
    (void)handle;
    r->l2cap++;
    r->last_cid = cid;
    r->nlast = n;
    memcpy(r->last, p, n);
    if (cid == L2CAP_CID_ATT && n == 3 && p[0] == 0x02) {
        static const uint8_t rsp[] = { 0x03, 0x17, 0x00 };
        check(hci_host_send_l2cap(&r->h, L2CAP_CID_ATT, rsp, 3) == HCI_OK, "the MTU response is queued");
    } else if (cid == L2CAP_CID_ATT && p[0] == 0x52) {
        uint8_t ntf[70] = { 0x1B, 0x12, 0x00 };
        for (int i = 0; i < 67; i++)
            ntf[3 + i] = (uint8_t)i;
        check(hci_host_send_l2cap(&r->h, L2CAP_CID_ATT, ntf, 70) == HCI_OK, "the notification is queued");
        check(hci_host_send_l2cap(&r->h, L2CAP_CID_ATT, ntf, 3) == HCI_EBUSY, "a second PDU waits for the first");
    }
}

static void rig_init(rig *r, const char *script, int sync)
{
    char path[512];
    memset(r, 0, sizeof *r);
    hci_model_init(&r->m, to_host, r);
    snprintf(path, sizeof path, "%s/%s", dir, script);
    if (hci_model_load(&r->m, path)) {
        fprintf(stderr, "FAIL: %s\n", r->m.why);
        bad = 1;
    }
    if (sync)
        r->m.sync = 1;
    r->t.send = hci_model_send;
    r->t.ctx = &r->m;
    hci_host_init(&r->h, &r->t, on_l2cap, r);
}

static hci_adv_cfg adv_cfg(void)
{
    hci_adv_cfg a;
    static const uint8_t flags = 0x06;
    memset(&a, 0, sizeof a);
    a.int_min = a.int_max = 0x0030;
    hci_ad_add(a.adv, &a.nadv, 0x01, &flags, 1);
    hci_ad_add(a.adv, &a.nadv, 0x09, "OPLBOX", 6);
    return a;
}

/* Runs both sides a millisecond at a time until the script is done (and the host idle) or ms have passed. */
static void run(rig *r, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t++) {
        hci_model_tick(&r->m, 1);
        hci_host_poll(&r->h, 1);
        if (r->disc_at && r->h.connects == (uint32_t)r->disc_at && r->h.state == HCI_ST_CONNECTED) {
            check(hci_host_disconnect(&r->h, HCI_ERR_REMOTE_USER_TERM) == HCI_OK, "hci_host_disconnect sends Disconnect");
            check(hci_host_disconnect(&r->h, HCI_ERR_REMOTE_USER_TERM) == HCI_EBUSY, "a second Disconnect waits");
            r->disc_at = 0;
        }
        if (hci_model_done(&r->m) && r->h.state != HCI_ST_STARTING && !r->m.nq)
            break;
    }
}

static void session(int sync)
{
    char why[400], path[512], what[120];
    rig *r = &R;
    hci_adv_cfg a = adv_cfg();
    const char *how = sync ? " (answers inside send)" : "";

    rig_init(r, "session.txt", sync);
    r->disc_at = 2;
    check(hci_host_send_l2cap(&r->h, L2CAP_CID_ATT, (const uint8_t *)"x", 1) == HCI_EBUSY, "no PDU before a connection");
    check(hci_host_start(&r->h, &a) == HCI_OK, "the start begins");
    check(hci_host_start(&r->h, &a) == HCI_EBUSY, "a second start while starting is refused");
    run(r, 5000);
    snprintf(path, sizeof path, "%s/session_expect.txt", dir);
#define CHK(c, s) do { snprintf(what, sizeof what, "%s%s", s, how); check(c, what); } while (0)
    CHK(hci_model_done(&r->m), "session: the script ran to its end");
    if (hci_model_diff(&r->m, path, why, sizeof why)) {
        fprintf(stderr, "FAIL: session%s: %s\n", how, why);
        bad = 1;
    }
    checks++;
    if (r->m.bad) {
        fprintf(stderr, "FAIL: session%s: the controller saw %d bad packets, the first: %s\n", how, r->m.bad, r->m.why);
        bad = 1;
    }
    checks++;
    CHK(r->h.state == HCI_ST_ADVERTISING && r->m.adv_on, "session: advertising again at the end");
    CHK(r->h.acl_len == 27 && r->h.acl_total == 2 && r->h.acl_free == 2, "session: the buffer size read, all buffers free");
    CHK(r->h.bdaddr[5] == 0x02 && r->h.bdaddr[0] == 0x01, "session: the address read");
    CHK(r->h.connects == 2 && r->h.disconnects == 2 && r->h.conn == HCI_NO_CONN, "session: two connections, both ended");
    CHK(r->h.disc_reason == HCI_ERR_LOCAL_HOST_TERM, "session: the last one ended by the host");
    CHK(r->h.interval == 12 && r->h.timeout == 72, "session: the connection parameters followed");
    CHK(r->l2cap == 2 && r->h.l2cap_in == 2 && r->last_cid == L2CAP_CID_ATT && r->nlast == 60 && r->last[0] == 0x52 &&
        r->last[59] == 56, "session: the three-fragment write arrived whole");
    CHK(r->m.l2cap_from_host == 2 && r->m.last_cid == L2CAP_CID_ATT && r->m.nlast == 70 && r->m.last[0] == 0x1B &&
        r->m.last[69] == 66, "session: the notification arrived whole at the central");
    CHK(r->h.acl_out == 4 && r->h.l2cap_out == 2 && r->h.acl_in == 4, "session: ACL packet counts");
    CHK(r->h.bad_events == 0 && r->h.rx_dropped == 0, "session: no bad events, nothing dropped");
    CHK(r->h.cmds == 12, "session: 12 commands");
#undef CHK
    if (!sync) {
        snprintf(path, sizeof path, "%s/../../build/host/ble_session.txt", dir);
        hci_model_write(&r->m, path);
    }
}

/* A start that fails, or ends somewhere other than advertising: the script, what the host must say. */
static void start_case(const char *script, uint32_t ms, uint8_t state, uint8_t why, uint16_t op, uint8_t status,
                       const char *what)
{
    rig *r = &R;
    hci_adv_cfg a = adv_cfg();
    char s[400];

    rig_init(r, script, 0);
    if (!strcmp(script, "start_bad_interval.txt"))
        a.int_min = 0x0010;                          /* under 20 ms: the controller refuses it */
    hci_host_start(&r->h, &a);
    run(r, ms);
    snprintf(s, sizeof s, "%s: state %u fail %u op %04x status %02x", what, r->h.state, r->h.fail, r->h.fail_op,
             r->h.fail_status);
    check(r->h.state == state && r->h.fail == why && r->h.fail_op == op && r->h.fail_status == status, s);
    snprintf(s, sizeof s, "%s: the controller saw nothing wrong (%s)", what, r->m.why);
    check(r->m.bad == 0, s);
}

static void failures(void)
{
    rig *r = &R;
    hci_adv_cfg a = adv_cfg();
    char s[200];

    start_case("start_refused.txt", 3000, HCI_ST_FAILED, HCI_FAIL_STATUS, HCI_OP_LE_SET_ADV_PARAMS, HCI_ERR_INVALID_PARAMS,
               "a refused LE Set Advertising Parameters fails the start there");
    check(r->m.nlog == 12 && !strcmp(r->m.log[11], "evt 0e 01 06 20 12"), "nothing is sent after the refusal");
    start_case("start_bad_interval.txt", 3000, HCI_ST_FAILED, HCI_FAIL_STATUS, HCI_OP_LE_SET_ADV_PARAMS,
               HCI_ERR_INVALID_PARAMS, "an advertising interval under 20 ms: the controller's own refusal fails the start");
    start_case("start_silent.txt", HCI_CMD_TIMEOUT_MS - 2, HCI_ST_STARTING, HCI_FAIL_NONE, HCI_OP_NOP, 0,
               "a silent controller: still waiting just before the timeout");
    run(r, 10);
    check(r->h.state == HCI_ST_FAILED && r->h.fail == HCI_FAIL_TIMEOUT && r->h.fail_op == HCI_OP_RESET,
          "a silent controller: the start fails at Reset after 2 s");
    check(hci_host_start(&r->h, &a) == HCI_OK && r->h.state == HCI_ST_STARTING, "a failed start can be started again");
    start_case("start_shared.txt", 3000, HCI_ST_ADVERTISING, HCI_FAIL_NONE, HCI_OP_NOP, 0,
               "no LE buffers of its own: the start asks Read Buffer Size and advertises");
    check(r->h.acl_len == 64 && r->h.acl_total == 5 && r->m.nlog == 20 && !strncmp(r->m.log[8], "cmd 1005", 8),
          "no LE buffers of its own: the shared size used");
    start_case("start_hwerror.txt", 3000, HCI_ST_FAILED, HCI_FAIL_HW_ERROR, HCI_OP_NOP, 0x07,
               "a Hardware Error while advertising fails the host");
    start_case("start_credits.txt", 3000, HCI_ST_ADVERTISING, HCI_FAIL_NONE, HCI_OP_NOP, 0,
               "a controller with no command credit between answers: each command waits for its credit");
    snprintf(s, sizeof s, "no command credit: the last command after 800 ms (%u ms)", r->m.now);
    check(r->m.cmds == 9 && r->m.now >= 800, s);

    /* the link drops while the controller holds both buffers: the next connection has them back */
    rig_init(r, "disc_held.txt", 0);
    hci_host_start(&r->h, &a);
    run(r, 3000);
    snprintf(s, sizeof s, "a link lost with buffers held: the next connection sends (script %s, %u PDUs out, %u free)",
             hci_model_done(&r->m) ? "done" : "stuck", r->m.l2cap_from_host, r->h.acl_free);
    check(hci_model_done(&r->m) && r->m.l2cap_from_host == 1 && r->h.l2cap_out == 1 && r->h.acl_free == 2 &&
          r->h.disc_reason == 0x08 && r->m.bad == 0, s);

    /* the drop rules for ACL in: a continuing fragment with no start, another handle, a PDU longer than the host's
     * buffer, a start cutting off an unfinished PDU; a good PDU after all of them still arrives */
    rig_init(r, "acl_drops.txt", 0);
    hci_host_start(&r->h, &a);
    run(r, 3000);
    snprintf(s, sizeof s, "ACL in: 4 dropped (%u), the good PDU still arrives (%d)", r->h.rx_dropped, r->l2cap);
    check(r->h.rx_dropped == 4 && r->l2cap == 1 && r->last_cid == L2CAP_CID_LE_SIG && r->nlast == 2, s);
    check(r->m.bad == 0 && r->h.state == HCI_ST_CONNECTED, "ACL in: the connection stays up");
    check(hci_host_send_l2cap(&r->h, L2CAP_CID_ATT, r->last, HCI_L2CAP_MAX - 3) == HCI_EINVAL, "an L2CAP PDU over 256 refused");
    check(hci_host_start(&r->h, &a) == HCI_EBUSY, "a start while connected is refused");
    a.nadv = 32;
    rig_init(r, "start_shared.txt", 0);
    check(hci_host_start(&r->h, &a) == HCI_EINVAL && r->h.state == HCI_ST_OFF, "advertising data over 31 bytes is refused");
}

int main(int argc, char **argv)
{
    dir = argc > 1 ? argv[1] : "tests/ble";
    packets();
    h4();
    session(0);
    session(1);
    failures();
    if (bad)
        return 1;
    printf("ble_hci: %d checks passed (packets, H4, a session against tests/ble/session_expect.txt both ways, failures)\n",
           checks);
    return 0;
}
