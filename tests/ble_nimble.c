/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of the NimBLE host on OPLBOX's port (firmware/ble/ble_nimble.c, ble_nimble_npl.c, docs/BLUETOOTH.md
 * "Cordio path as built" step 1) against the fake controller (tests/ble/hci_model.c): the start-up commands NimBLE
 * sends, the advertising it sets up (parameters, data, scan response), a central that connects and walks the GATT
 * database the way a phone does (MTU exchange, primary services, the characteristic, its descriptors), enables
 * notifications, writes BLE-MIDI packets (with and without a response), reads the empty first value, receives
 * notifications (one that needs several ACL fragments while the controller has two buffers), the connection
 * interval the host asks for, a disconnection from either side and advertising again, and the ways start-up
 * fails. The session is diffed against tests/ble/nimble_expect.txt. Exits 1 when a check fails.
 *
 * Usage: ble_nimble <dir with nimble_session.txt and nimble_expect.txt> [scratch dir]
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ble_nimble.h"
#include "ble_nimble_npl.h"
#include <nimble/nimble_port.h>
#include "ble_midi.h"
#include "hci_model.h"

static int bad, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fprintf(stderr, "FAIL: %s\n", what); bad = 1; } }
static const char *dir, *scratch = ".";

static hci_model M;
static uint8_t g_sx[512];
static ble_midi_in g_in;
static int g_refuse;                    /* refuse this many sends, then take them */

static uint32_t now(void *ctx) { (void)ctx; return M.now; }
static void pump(void *ctx) { (void)ctx; hci_model_tick(&M, 1); }
static void deliver(void *ctx, uint8_t type, const uint8_t *p, uint16_t n) { (void)ctx; ble_nimble_receive(type, p, n); }
static int xsend(void *ctx, uint8_t type, const uint8_t *p, uint16_t n)
{
    if (g_refuse > 0) {
        g_refuse--;
        return -1;
    }
    return hci_model_send(ctx, type, p, n);
}
static const hci_transport T = { xsend, &M };

static void run(uint32_t ms)
{
    while (ms--) {
        ble_nimble_poll();
        hci_model_tick(&M, 1);
    }
}

static int run_until(int (*cond)(void), uint32_t ms)
{
    while (ms--) {
        if (cond())
            return 1;
        run(1);
    }
    return cond();
}

static int is_state(int s) { return ble_nimble_state()->state == s; }
static int c_adv(void) { return is_state(BLE_NIMBLE_ADVERTISING) && M.adv_on; }
static int c_conn(void) { return is_state(BLE_NIMBLE_CONNECTED); }
static int c_notify(void) { return ble_nimble_state()->notify_on; }
static int c_failed(void) { return is_state(BLE_NIMBLE_FAILED); }

static ble_nimble_cfg cfg(void)
{
    ble_nimble_cfg c;
    memset(&c, 0, sizeof c);
    c.t = &T;
    c.now_ms = now;
    c.pump = pump;
    c.name = "OPLBOX";
    c.in = &g_in;
    return c;
}

/* A fresh model with this script, and the host started. */
static int fresh(const char *script)
{
    char path[400];
    ble_nimble_cfg c = cfg();
    hci_model_init(&M, deliver, NULL);
    snprintf(path, sizeof path, "%s/%s", dir, script);
    if (hci_model_load(&M, path) != 0) {
        fprintf(stderr, "script: %s\n", M.why);
        return -1;
    }
    ble_midi_in_init(&g_in, g_sx, sizeof g_sx, NULL, NULL);
    g_refuse = 0;
    return ble_nimble_init(&c);
}

/* Steps added to a running model: a script file written here and read with hci_model_load, which appends. */
static int add_steps(const char *text)
{
    char path[400];
    FILE *f;
    snprintf(path, sizeof path, "%s/nimble_steps.txt", scratch);
    f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(text, f);
    fclose(f);
    return hci_model_load(&M, path);
}

/* ---- the central's ATT ---- */

static void le16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static unsigned rd16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }

/* One ATT PDU from the central, as one L2CAP PDU in one ACL packet (the central's buffers are not modelled). */
static void att_send(const uint8_t *pdu, uint16_t n)
{
    uint8_t l2[HCI_L2CAP_MAX], pkt[HCI_ACL_MAX];
    le16(l2, n);
    le16(l2 + 2, L2CAP_CID_ATT);
    memcpy(l2 + 4, pdu, n);
    ble_nimble_receive(HCI_ACL, pkt, hci_acl_pack(pkt, 0x0040, HCI_PB_FIRST_FLUSH, l2, (uint16_t)(4 + n)));
}

/* Sends a request and waits for the host's one reply. Returns its length (0: none), the PDU in M.last. */
static uint16_t att_req(const uint8_t *pdu, uint16_t n)
{
    uint32_t before = M.l2cap_from_host;
    uint32_t t;
    att_send(pdu, n);
    for (t = 0; t < 100 && M.l2cap_from_host == before; t++)
        run(1);
    return M.l2cap_from_host == before ? 0 : M.nlast;
}

static int is_err(uint8_t req, uint8_t code)
{
    return M.nlast == 5 && M.last[0] == 0x01 && M.last[1] == req && M.last[4] == code;
}

static const uint8_t uuid_svc[16] = { 0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03 };
static const uint8_t uuid_chr[16] = { 0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77 };

/* ---- the sessions ---- */

static unsigned svc_start, svc_end, val_handle, cccd_handle;

static void start_and_advertise(void)
{
    const ble_nimble_status *s;
    int i, found_params = 0, found_data = 0, found_rsp = 0, found_enable = 0;
    uint8_t want_data[31], want_rsp[31];
    uint32_t nd, nr;

    check(fresh("nimble_session.txt") == 0, "ble_nimble_init takes the configuration");
    check(is_state(BLE_NIMBLE_STARTING), "starting before the first poll");
    check(run_until(c_adv, 3000), "the host starts the controller and advertises");
    s = ble_nimble_state();
    check(s->state == BLE_NIMBLE_ADVERTISING, "state advertising");
    check(M.bad == 0, "the controller found nothing wrong with the host's commands");
    check(s->bdaddr[0] == 0x02 && s->bdaddr[5] == 0x01, "the public address read from the controller, most significant first");

    nd = ble_midi_adv_data(want_data);
    nr = ble_midi_scan_rsp(want_rsp, "OPLBOX");
    for (i = 0; i < M.nlog; i++) {
        const char *l = M.log[i];
        if (!strncmp(l, "cmd 2006", 8)) found_params++;
        if (!strncmp(l, "cmd 2008", 8)) {
            char exp[200];
            int k, o = snprintf(exp, sizeof exp, "cmd 2008 %02x", (unsigned)nd);
            for (k = 0; k < (int)nd; k++)
                o += snprintf(exp + o, sizeof exp - (size_t)o, " %02x", want_data[k]);
            found_data += !strncmp(l, exp, (size_t)o);
        }
        if (!strncmp(l, "cmd 2009", 8)) {
            char exp[200];
            int k, o = snprintf(exp, sizeof exp, "cmd 2009 %02x", (unsigned)nr);
            for (k = 0; k < (int)nr; k++)
                o += snprintf(exp + o, sizeof exp - (size_t)o, " %02x", want_rsp[k]);
            found_rsp += !strncmp(l, exp, (size_t)o);
        }
        if (!strncmp(l, "cmd 200a 01", 11)) found_enable++;
    }
    check(found_params == 1, "one LE Set Advertising Parameters");
    check(found_data == 1, "the advertising data is the BLE-MIDI profile's (flags and the 128-bit service UUID)");
    check(found_rsp == 1, "the scan response carries the name OPLBOX");
    check(found_enable == 1, "advertising enabled once");
    /* the parameters: 30-60 ms (0x30 and 0x60 units of 0.625 ms), ADV_IND (connectable undirected), public own
     * address, no peer, channels 37, 38 and 39, no filter */
    for (i = 0; i < M.nlog; i++)
        if (!strncmp(M.log[i], "cmd 2006", 8))
            check(!strcmp(M.log[i], "cmd 2006 30 00 60 00 00 00 00 00 00 00 00 00 00 07 00"),
                  "advertising parameters: 30-60 ms, ADV_IND, public own address, channels 37-39");
}

static void discover(void)
{
    uint8_t q[32];
    uint16_t n;
    int saw_gap = 0, saw_gatt = 0, i;

    check(run_until(c_conn, 500), "the central connects");
    check(ble_nimble_state()->conn == 0x0040, "the connection handle");

    /* the host asks for an interval of 15 ms or less (the central connected at 30 ms); the model's central grants 12 */
    run(30);
    check(ble_nimble_state()->update_asked == 1, "the host asked once for a shorter connection interval");
    check(ble_nimble_state()->interval == BLE_MIDI_CONN_INTERVAL_MAX, "the interval the central granted is 15 ms");
    {
        int i, sig = 0;
        for (i = 0; i < M.nlog; i++)       /* L2CAP LE signalling, Connection Parameter Update Request: 8..12, latency 0, 720 ms */
            sig += !strcmp(M.log[i], "acl> 0040 0 0c 00 05 00 12 01 08 00 08 00 0c 00 00 00 48 00");
        check(sig == 1, "the request is an L2CAP Connection Parameter Update Request for 10-15 ms (a peripheral cannot ask the controller)");
    }

    /* Exchange MTU: the central offers 247, the host answers its own 185, the MTU is the smaller */
    q[0] = 0x02; le16(q + 1, 247);
    n = att_req(q, 3);
    check(n == 3 && M.last[0] == 0x03 && rd16(M.last + 1) == 185, "Exchange MTU Response carries the host's 185");
    check(ble_nimble_state()->mtu == 185, "the connection's MTU is 185");

    /* Read By Group Type, primary services, from handle 1 */
    q[0] = 0x10; le16(q + 1, 1); le16(q + 3, 0xFFFF); le16(q + 5, 0x2800);
    n = att_req(q, 7);
    check(n > 2 && M.last[0] == 0x11 && M.last[1] == 6, "primary services: 16-bit UUIDs first (length 6 entries)");
    for (i = 2; i + 6 <= n; i += 6) {
        unsigned u = rd16(M.last + i + 4);
        saw_gap += u == 0x1800;
        saw_gatt += u == 0x1801;
        svc_end = rd16(M.last + i + 2);
    }
    check(saw_gap && saw_gatt, "the GAP and GATT services are there");
    q[0] = 0x10; le16(q + 1, svc_end + 1); le16(q + 3, 0xFFFF); le16(q + 5, 0x2800);
    n = att_req(q, 7);
    check(n == 22 && M.last[0] == 0x11 && M.last[1] == 20, "the 128-bit service follows (one entry of 20 bytes)");
    check(n == 22 && !memcmp(M.last + 6, uuid_svc, 16), "its UUID is the BLE-MIDI service");
    if (n == 22) {
        svc_start = rd16(M.last + 2);
        svc_end = rd16(M.last + 4);
        check(svc_end == 0xFFFF, "the last service's end handle is 0xFFFF (the end of the database)");
    }

    /* the characteristic declaration inside the service */
    q[0] = 0x08; le16(q + 1, svc_start); le16(q + 3, 0xFFFF); le16(q + 5, 0x2803);
    n = att_req(q, 7);
    check(n == 23 && M.last[0] == 0x09 && M.last[1] == 21, "Read By Type 0x2803: one 21-byte entry");
    check(n == 23 && M.last[4] == BLE_MIDI_CHAR_PROPS, "the properties are read, write without response and notify (0x16)");
    check(n == 23 && !memcmp(M.last + 7, uuid_chr, 16), "the characteristic UUID is the BLE-MIDI one");
    if (n == 23)
        val_handle = rd16(M.last + 5);

    /* Find Information after the value: the Client Characteristic Configuration descriptor */
    q[0] = 0x04; le16(q + 1, val_handle + 1); le16(q + 3, 0xFFFF);
    n = att_req(q, 5);
    check(n >= 6 && M.last[0] == 0x05 && M.last[1] == 1 && rd16(M.last + 4) == 0x2902, "Find Information: the CCCD follows the value");
    if (n >= 6)
        cccd_handle = rd16(M.last + 2);
    check(cccd_handle == val_handle + 1, "the CCCD is the next handle");
    check(n == 6, "and it is the last attribute");
    q[0] = 0x10; le16(q + 1, cccd_handle + 1); le16(q + 3, 0xFFFF); le16(q + 5, 0x2800);
    n = att_req(q, 7);
    check(n == 5 && is_err(0x10, 0x0A), "no more services: Error Response, Attribute Not Found");
}

static void gatt_access(void)
{
    uint8_t q[64];
    uint16_t n;
    uint8_t st, d1, d2;
    uint32_t w0 = ble_nimble_state()->writes;

    /* the first read of the characteristic answers with no payload */
    q[0] = 0x0A; le16(q + 1, val_handle);
    n = att_req(q, 3);
    check(n == 1 && M.last[0] == 0x0B, "Read of the characteristic: Read Response with no value");

    /* the CCCD reads 0 until the central writes it */
    q[0] = 0x0A; le16(q + 1, cccd_handle);
    n = att_req(q, 3);
    check(n == 3 && M.last[0] == 0x0B && rd16(M.last + 1) == 0, "the CCCD reads 0");
    check(ble_nimble_notify(NULL, (const uint8_t *)"\x80\x80\x90\x3c\x7f", 5) == -1, "no notification before the central subscribes");

    /* a Write Request to the value is answered too (NimBLE grants the ATT write permission to both write properties) and
     * its packet is a message like any other: a note-on of note 0x3B */
    q[0] = 0x12; le16(q + 1, val_handle); q[3] = 0x80; q[4] = 0x80; q[5] = 0x90; q[6] = 0x3B; q[7] = 0x7F;
    n = att_req(q, 8);
    check(n == 1 && M.last[0] == 0x13, "Write Request to the value: Write Response");
    check(ble_midi_get(&g_in, &st, &d1, &d2) == 1 && st == 0x90 && d1 == 0x3B && d2 == 0x7F, "its message comes out of ble_midi_get");
    w0++;

    /* a CCCD write of the wrong length */
    q[0] = 0x12; le16(q + 1, cccd_handle); q[3] = 0x01;
    n = att_req(q, 4);
    check(n == 5 && is_err(0x12, 0x0D), "CCCD write of one byte: Invalid Attribute Value Length");

    /* subscribe to notifications */
    q[0] = 0x12; le16(q + 1, cccd_handle); le16(q + 3, 0x0001);
    n = att_req(q, 5);
    check(n == 1 && M.last[0] == 0x13, "Write Response for the CCCD");
    check(run_until(c_notify, 20), "the host knows notifications are on");
    q[0] = 0x0A; le16(q + 1, cccd_handle);
    n = att_req(q, 3);
    check(n == 3 && rd16(M.last + 1) == 0x0001, "the CCCD now reads 1");

    /* a Write Command with a BLE-MIDI packet: header, timestamp, note on channel 1, note 60, velocity 100 */
    {
        uint32_t before = M.l2cap_from_host;
        q[0] = 0x52; le16(q + 1, val_handle); q[3] = 0x81; q[4] = 0x85; q[5] = 0x90; q[6] = 0x3C; q[7] = 0x64;
        att_send(q, 8);
        run(10);
        check(M.l2cap_from_host == before, "a Write Command gets no reply");
    }
    check(ble_nimble_state()->writes == w0 + 1, "the write reached the characteristic's handler");
    check(ble_midi_get(&g_in, &st, &d1, &d2) == 1 && st == 0x90 && d1 == 0x3C && d2 == 0x64, "the note-on comes out of ble_midi_get");
    check(ble_midi_get(&g_in, &st, &d1, &d2) == 0, "and nothing else");

    /* two messages in one packet, running status */
    q[0] = 0x52; le16(q + 1, val_handle);
    q[3] = 0x81; q[4] = 0x86; q[5] = 0x90; q[6] = 0x40; q[7] = 0x70; q[8] = 0x86; q[9] = 0x41; q[10] = 0x71;
    att_send(q, 11);
    run(10);
    check(ble_midi_get(&g_in, &st, &d1, &d2) == 1 && st == 0x90 && d1 == 0x40 && d2 == 0x70 &&
          ble_midi_get(&g_in, &st, &d1, &d2) == 1 && st == 0x90 && d1 == 0x41 && d2 == 0x71, "running status inside one write");

    /* a write longer than the packet the encoder uses is a whole ATT value too: 100 bytes of messages */
    {
        int i, k;
        uint32_t got = 0;
        uint8_t w[120];
        w[0] = 0x52; le16(w + 1, val_handle);
        w[3] = 0x81;
        for (k = 4, i = 0; i < 24; i++) {
            w[k++] = (uint8_t)(0x80 | (i & 0x3F));
            w[k++] = 0x90; w[k++] = (uint8_t)(0x30 + i); w[k++] = 0x40;
        }
        att_send(w, (uint16_t)k);
        run(10);
        while (ble_midi_get(&g_in, &st, &d1, &d2))
            got += st == 0x90 && d2 == 0x40;
        check(got == 24, "a 100-byte write: all 24 messages");
    }
}

static int g_sent;
static int count_send(void *ctx, const uint8_t *p, uint32_t n) { (void)ctx; g_sent++; return ble_nimble_notify(ctx, p, n); }

static void bad_events(void)
{
    const ble_nimble_status *s = ble_nimble_state();
    static const uint8_t long_len[] = { 0x13, 0x09, 0x01, 0x40, 0x00 };          /* says 9 parameter bytes, has 3 */
    static const uint8_t short_hdr[] = { 0x05 };
    static const uint8_t extra[] = { 0x0E, 0x01, 0x00, 0x00, 0x00 };             /* says 1, has 3 */
    uint32_t c0 = s->rx_bad;

    ble_nimble_receive(HCI_EVT, long_len, sizeof long_len);
    ble_nimble_receive(HCI_EVT, short_hdr, sizeof short_hdr);
    ble_nimble_receive(HCI_EVT, extra, sizeof extra);
    ble_nimble_receive(HCI_SCO, extra, sizeof extra);
    run(20);
    check(s->rx_bad == c0 + 4, "four malformed packets (two lengths wrong, a header cut short, a SCO packet) are refused and counted");
    check(is_state(BLE_NIMBLE_CONNECTED) && M.bad == 0, "and the connection goes on");
}

static void notify(void)
{
    uint8_t buf[182];
    ble_midi_enc e;
    uint8_t sx[150];
    uint32_t before = M.l2cap_from_host, i;
    const ble_nimble_status *s = ble_nimble_state();
    uint32_t n0 = s->notified;

    check(ble_midi_enc_init(&e, buf, sizeof buf, count_send, NULL) == 0, "the encoder takes a buffer of MTU - 3");
    check(ble_midi_enc_msg(&e, 1000, 0x90, 0x3C, 0x7F) == 0 && ble_midi_enc_msg(&e, 1002, 0x80, 0x3C, 0x00) == 0 &&
          ble_midi_enc_flush(&e) == 0, "two messages in one packet, flushed");
    check(is_state(BLE_NIMBLE_CONNECTED), "still connected");
    for (i = 0; i < 40 && M.l2cap_from_host == before; i++)
        run(1);
    check(M.l2cap_from_host == before + 1, "one notification reaches the central");
    check(M.nlast == 3 + 9 && M.last[0] == 0x1B && rd16(M.last + 1) == val_handle,
          "Handle Value Notification for the characteristic's value handle");
    {
        static const uint8_t want[9] = { 0x87, 0xE8, 0x90, 0x3C, 0x7F, 0xEA, 0x80, 0x3C, 0x00 };
        check(M.nlast == 12 && !memcmp(M.last + 3, want, 9), "its value is the encoder's packet: header, timestamp, note-on, timestamp, note-off");
    }
    check(s->notified == n0 + 1, "counted");

    /* a SysEx of 150 bytes: one packet of 154 bytes, a notification of 157 bytes: six ACL fragments of 27 */
    sx[0] = 0xF0;
    for (i = 1; i < 149; i++)
        sx[i] = (uint8_t)(i & 0x7F);
    sx[149] = 0xF7;
    before = M.l2cap_from_host;
    check(ble_midi_enc_sysex(&e, 2000, sx, 150) == 0 && ble_midi_enc_flush(&e) == 0, "a 150-byte SysEx, flushed");
    for (i = 0; i < 200 && M.l2cap_from_host == before; i++)
        run(1);
    check(M.l2cap_from_host == before + 1, "it reaches the central as one PDU although the controller has two ACL buffers of 27 bytes");
    check(M.nlast > 150 && M.last[0] == 0x1B, "the notification carries the SysEx packet");
    check(M.bad == 0, "no ACL packet beyond the controller's buffers or out of order");
    check(g_sent == 2, "two packets were sent");

    /* the transport refuses the next send: the notification waits in the port and goes out on a later poll */
    before = M.l2cap_from_host;
    g_refuse = 2;
    check(ble_midi_enc_msg(&e, 3000, 0x90, 0x40, 0x7F) == 0 && ble_midi_enc_flush(&e) == 0, "a notification while the transport refuses");
    run(1);
    check(M.l2cap_from_host == before, "it has not reached the central yet");
    for (i = 0; i < 20 && M.l2cap_from_host == before; i++)
        run(1);
    check(M.l2cap_from_host == before + 1 && g_refuse == 0, "it goes out on a poll once the transport takes it");

    /* a packet longer than MTU - 3 is refused, not truncated */
    {
        uint8_t big[200] = { 0 };
        check(ble_nimble_notify(NULL, big, 183) == -1, "a packet of MTU - 2 bytes is refused");
        check(ble_nimble_notify(NULL, big, 182) == 0, "a packet of MTU - 3 bytes is taken");
        run(200);
    }
}

static void leave_and_return(void)
{
    const ble_nimble_status *s = ble_nimble_state();
    uint8_t st, d1, d2;
    uint8_t q[16];

    {   /* a message the app has not read yet when the central leaves */
        uint8_t w[8] = { 0x52, 0, 0, 0x81, 0x85, 0x90, 0x2A, 0x55 };
        le16(w + 1, val_handle);
        att_send(w, 8);
        run(5);
    }
    check(add_steps("disconnect reason=13\nwait adv\n") == 0, "the leave loads");
    check(run_until(c_adv, 500), "the central leaves and the host advertises again");
    check(s->disconnects == 1, "one disconnection counted");
    check(!s->notify_on && s->conn == 0xFFFF && s->mtu == 23, "notifications off, no connection, MTU back to 23 as soon as it left");
    check(ble_nimble_notify(NULL, (const uint8_t *)"\x80\x80\x90\x3c\x7f", 5) == -1, "nothing is sent while nobody is connected");
    check(add_steps("wait ms 20\nconnect handle=0041 peer=c0:ff:ee:00:00:02 type=1 interval=12 latency=0 timeout=72\n") == 0,
          "the second central's steps load");
    check(run_until(c_conn, 500) && s->connects == 2, "advertising started again and a second central connected");
    check(s->conn == 0x0041, "the second connection's handle");
    check(!s->notify_on, "notifications are off again on a new connection");
    check(s->mtu == 23, "the MTU is back to 23");
    check(ble_midi_get(&g_in, &st, &d1, &d2) == 0, "the message nobody read from the old connection is gone");
    check(ble_nimble_notify(NULL, (const uint8_t *)"\x80\x80\x90\x3c\x7f", 5) == -1, "no notification until the new central subscribes");

    /* the host ends the connection itself */
    q[0] = 0x0A; le16(q + 1, 1);
    att_send(q, 3);                     /* an ATT request on a handle the model's central does not use: just traffic */
    run(5);
    check(ble_nimble_disconnect() == 0, "the host ends the connection");
    check(run_until(c_adv, 500), "and advertises again");
    check(s->disconnects == 2, "two disconnections");
    check(ble_nimble_disconnect() == -1, "nothing to end when not connected");
    check(M.bad == 0, "the controller found nothing wrong along the way");
}

/* ---- the ways start-up fails: one process each, because NimBLE is a singleton that is not started twice ---- */

static void fail_silent(void)
{
    /* a controller that never answers Reset: the host gives up after its 2 s and the state is FAILED */
    check(fresh("nimble_silent.txt") == 0, "init with a silent controller");
    check(run_until(c_failed, 6000), "no answer to Reset: failed");
    check(ble_nimble_state()->fail != 0, "with a reason");
    check(M.cmds == 1, "after one command: the host does not go on without an answer");
    run(10000);
    check(M.cmds == 1 && ble_nimble_state()->state == BLE_NIMBLE_FAILED, "no more commands go out after the failure, however long it waits");
    check(ble_nimble_state()->failures == 1, "one failure counted");
}

static void fail_refused(void)
{
    /* advertising parameters refused (status 0x12): failed, with a reason */
    check(fresh("nimble_refused.txt") == 0, "init with a controller that refuses the advertising parameters");
    check(run_until(c_failed, 3000), "refused advertising parameters: failed");
    check(ble_nimble_state()->fail != 0, "with a reason");
    check(ble_nimble_state()->fail != 0x100, "that names the NimBLE or HCI error");
}

static void transport_refuses(void)
{
    /* a transport that refuses the first sends: the packet is kept and goes out on a later poll */
    check(fresh("nimble_session.txt") == 0, "init");
    g_refuse = 5;
    check(run_until(c_adv, 4000), "a transport that refuses five sends: the start still completes");
    check(ble_nimble_state()->hci_refused == 5, "five refusals counted");
    check(M.bad == 0, "and the controller saw each command once, in order");
}

static void transport_refuses_always(void)
{
    /* a transport that never takes a packet: the 2 s silence ends the start with the reason, instead of waiting forever */
    check(fresh("nimble_session.txt") == 0, "init");
    g_refuse = 1000000;
    check(run_until(c_failed, 6000), "a transport that refuses every send: failed");
    check(ble_nimble_state()->fail != 0, "with a reason");
    check(M.cmds == 0, "the controller never saw a command");
    g_refuse = 0;
    ble_nimble_retry();
    check(is_state(BLE_NIMBLE_STARTING), "ble_nimble_retry: starting again");
    check(run_until(c_adv, 8000), "and with a transport that takes packets the start completes");
    check(M.bad == 0, "in order");
}

/* ---- the port's OS layer alone: no host, no controller ---- */

static uint32_t g_clock;
static uint32_t npl_now(void *ctx) { (void)ctx; return g_clock; }
static int g_pumped;
static void npl_pump(void *ctx) { (void)ctx; g_pumped++; g_clock++; }
static int g_order[16], g_nord;
static void ev_note(struct ble_npl_event *ev) { if (g_nord < 16) g_order[g_nord++] = (int)(intptr_t)ble_npl_event_get_arg(ev); }
static struct ble_npl_eventq *g_q;
static void ev_again(struct ble_npl_event *ev) { ble_npl_eventq_put(g_q, ev); g_nord++; }

static void npl_units(void)
{
    struct ble_npl_eventq q, *dq;
    struct ble_npl_event a, b, d, *e;
    struct ble_npl_callout co, co2;
    struct ble_npl_sem sem;
    struct ble_npl_mutex mu;
    uint32_t tmo;

    ble_npl_port_set(npl_now, npl_pump, NULL);
    nimble_port_init();                         /* the default queue, with the host's start event in it: drop that */
    dq = nimble_port_get_dflt_eventq();
    while ((e = ble_npl_eventq_get(dq, 0)) != NULL)
        ;

    /* events: FIFO, put twice is once, remove, is_queued */
    ble_npl_eventq_init(&q);
    ble_npl_event_init(&a, ev_note, (void *)1);
    ble_npl_event_init(&b, ev_note, (void *)2);
    ble_npl_event_init(&d, ev_note, (void *)3);
    check(ble_npl_eventq_is_empty(&q), "a new queue is empty");
    ble_npl_eventq_put(&q, &a);
    ble_npl_eventq_put(&q, &b);
    ble_npl_eventq_put(&q, &a);                 /* already in */
    ble_npl_eventq_put(&q, &d);
    check(ble_npl_event_is_queued(&a) && !ble_npl_eventq_is_empty(&q), "queued events say so");
    ble_npl_eventq_remove(&q, &b);
    check(!ble_npl_event_is_queued(&b), "a removed event is not queued");
    ble_npl_eventq_remove(&q, &b);              /* not queued: nothing happens */
    g_nord = 0;
    while ((e = ble_npl_eventq_get(&q, 0)) != NULL)
        ble_npl_event_run(e);
    check(g_nord == 2 && g_order[0] == 1 && g_order[1] == 3, "events come out in the order they went in, once each, without the removed one");
    check(ble_npl_eventq_is_empty(&q) && q.tail == NULL, "and the queue is empty, head and tail");
    ble_npl_eventq_put(&q, &b);
    e = ble_npl_eventq_get(&q, 0);
    check(e == &b && ble_npl_eventq_get(&q, 0) == NULL, "a queue that emptied takes an event again");
    ble_npl_eventq_put(&q, &a);
    ble_npl_eventq_put(&q, &a);                 /* the same event twice in a row */
    check(ble_npl_eventq_get(&q, 0) == &a && ble_npl_eventq_get(&q, 0) == NULL, "an event put twice in a row comes out once");
    ble_npl_event_set_arg(&b, (void *)7);
    check(ble_npl_event_get_arg(&b) == (void *)7, "an event's argument");

    /* callouts fire when due, once, on their queue; stop and re-arm */
    ble_npl_eventq_init(dq);
    g_clock = 100;
    ble_npl_callout_init(&co, dq, ev_note, (void *)10);
    ble_npl_callout_init(&co2, dq, ev_note, (void *)20);
    check(!ble_npl_callout_is_active(&co), "a new callout is not active");
    ble_npl_callout_reset(&co, 50);
    check(ble_npl_callout_is_active(&co) && ble_npl_callout_get_ticks(&co) == 150, "armed for 150");
    check(ble_npl_callout_remaining_ticks(&co, 120) == 30 && ble_npl_callout_remaining_ticks(&co, 200) == 0, "ticks remaining");
    g_nord = 0;
    g_clock = 149;
    check(ble_npl_port_run() == 0 && g_nord == 0, "not before it is due");
    g_clock = 150;
    check(ble_npl_port_run() == 1 && g_nord == 1 && g_order[0] == 10, "when it is due: once");
    check(!ble_npl_callout_is_active(&co), "and no longer armed");
    g_clock = 300;
    check(ble_npl_port_run() == 0, "it does not fire again");
    ble_npl_callout_reset(&co, 100);
    ble_npl_callout_reset(&co, 10);             /* re-armed sooner: the first arming is gone */
    g_nord = 0;
    g_clock = 310;
    check(ble_npl_port_run() == 1 && g_nord == 1, "a re-armed callout fires at its new time");
    g_clock = 500;
    check(ble_npl_port_run() == 0, "and not a second time at the old one");
    ble_npl_callout_reset(&co, 10);
    ble_npl_callout_reset(&co2, 10);
    ble_npl_callout_stop(&co);
    check(!ble_npl_callout_is_active(&co), "a stopped callout is not active");
    g_nord = 0;
    g_clock = 600;
    check(ble_npl_port_run() == 1 && g_order[0] == 20, "a stopped callout does not fire, the other does");
    g_clock = 601;
    ble_npl_callout_reset(&co, 1);
    g_clock = 602;
    g_nord = 0;
    ble_npl_callout_set_arg(&co, (void *)5);
    ble_npl_port_run();
    check(g_nord == 1 && g_order[0] == 5, "set_arg changes what the callout's event gets");
    /* several callouts: re-arming one in the middle of the list keeps the list whole */
    g_clock = 700;
    ble_npl_callout_set_arg(&co, (void *)10);
    ble_npl_callout_reset(&co, 50);
    ble_npl_callout_reset(&co2, 60);
    ble_npl_callout_reset(&co, 500);
    g_nord = 0;
    g_clock = 760;
    check(ble_npl_port_run() == 1 && g_nord == 1 && g_order[0] == 20, "the callout that was not re-armed fires at its time, the re-armed one does not");
    g_clock = 1200;
    check(ble_npl_port_run() == 1 && g_nord == 2 && g_order[1] == 10, "and the re-armed one fires at its new time");
    /* a callout fired and still queued, then stopped, leaves the queue */
    g_clock = 1300;
    ble_npl_callout_reset(&co, 1);
    g_clock = 1301;
    ble_npl_callout_stop(&co);
    check(ble_npl_port_run() == 0, "a callout that was stopped does not run");

    /* the main loop's step runs a bounded number of events, and stops although an event queues itself again */
    ble_npl_eventq_init(dq);
    {
        struct ble_npl_event ev[5];
        int i, n;
        for (i = 0; i < 5; i++) {
            ble_npl_event_init(&ev[i], ev_note, (void *)(intptr_t)(i + 1));
            ble_npl_eventq_put(dq, &ev[i]);
        }
        g_nord = 0;
        n = ble_npl_port_run();
        check(n == 5 && g_nord == 5, "one step runs all the events that are queued (up to its bound)");
    }
    g_q = dq;
    ble_npl_event_init(&a, ev_again, NULL);
    ble_npl_eventq_put(dq, &a);
    g_nord = 0;
    check(ble_npl_port_run() > 0 && g_nord <= 64, "an event that queues itself again does not keep one step from ending");
    ble_npl_eventq_remove(dq, &a);

    /* the semaphore waits by running the pump; a mutex counts */
    ble_npl_sem_init(&sem, 0);
    g_pumped = 0;
    g_clock = 1000;
    check(ble_npl_sem_pend(&sem, 50) == BLE_NPL_TIMEOUT, "nothing releases the semaphore: a wait times out");
    tmo = g_clock - 1000;
    check(tmo >= 50 && tmo <= 52 && g_pumped >= 50, "after about 50 ms, running the pump the whole time");
    ble_npl_sem_release(&sem);
    check(ble_npl_sem_get_count(&sem) == 1 && ble_npl_sem_pend(&sem, 50) == 0 && ble_npl_sem_get_count(&sem) == 0, "a released token is taken at once");
    ble_npl_sem_init(&sem, 1);
    g_pumped = 0;
    check(ble_npl_sem_pend(&sem, 50) == 0 && g_pumped == 0, "with a token, no pump");
    ble_npl_mutex_init(&mu);
    check(ble_npl_mutex_release(&mu) == BLE_NPL_BAD_MUTEX, "releasing a mutex that is not held is an error");
    check(ble_npl_mutex_pend(&mu, 0) == 0 && ble_npl_mutex_release(&mu) == 0, "pend and release");
    g_clock = 2000;
    ble_npl_time_delay(5);
    check(g_clock == 2005, "delay runs the pump for the time given");
}

int main(int argc, char **argv)
{
    char exp[400], why[300];
    const char *what = "session";
    if (argc < 2) {
        fprintf(stderr, "usage: ble_nimble <dir> [scratch [session|silent|refused|refusing]]\n");
        return 2;
    }
    dir = argv[1];
    if (argc > 2)
        scratch = argv[2];
    if (argc > 3)
        what = argv[3];

    if (!strcmp(what, "npl")) {
        npl_units();
    } else if (!strcmp(what, "silent")) {
        fail_silent();
    } else if (!strcmp(what, "refused")) {
        fail_refused();
    } else if (!strcmp(what, "refusing")) {
        transport_refuses();
    } else if (!strcmp(what, "never")) {
        transport_refuses_always();
    } else {
        start_and_advertise();
        discover();
        gatt_access();
        bad_events();
        notify();
        leave_and_return();
        snprintf(exp, sizeof exp, "%s/nimble_expect.txt", dir);
        if (hci_model_diff(&M, exp, why, sizeof why) != 0) {
            check(0, why);
            snprintf(exp, sizeof exp, "%s/nimble_actual.txt", scratch);
            hci_model_write(&M, exp);
            fprintf(stderr, "(the log is in %s)\n", exp);
        } else {
            check(1, "the session matches nimble_expect.txt");
        }
    }
    printf("ble_nimble %s: %d checks, %s\n", what, checks, bad ? "FAILED" : "all passed");
    return bad;
}
