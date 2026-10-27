/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of the whole Bluetooth stack as the FM-1 would run it, minus the chip: the NimBLE host and the BLE-MIDI
 * service (firmware/ble/ble_nimble.c) over HCI to Cordio's controller (firmware/ble/cordio_port/), on Cordio's PAL for the
 * FM-1 (pal_fm1.c) and the radio driver (fm1ble.c), against the register model's air (tests/ble/bb_model.c) and a central
 * (tests/ble/air_central.c) that speaks the link layer, L2CAP and ATT from the specification: it scans, connects, finds the
 * BLE-MIDI characteristic, subscribes, writes MIDI that comes out of ble_midi_get, is sent MIDI the app notifies, and
 * disconnects; the peripheral advertises again. The receive interrupt runs within T_IFS here (the model calls it at
 * once): the FM-1 has to show it does. Exits 1 when a check fails. */
#include <stdio.h>
#include <string.h>
#include "cordio_ctrl.h"
#include "pal_fm1.h"
#include "fm1ble.h"
#include "bb_model.h"
#include "air_central.h"
#include "hci.h"
#include "ble_midi.h"
#include "ble_nimble.h"
#include "fm1ble_test.h"

static int bad, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fprintf(stderr, "FAIL: %s\n", what); bad = 1; } }

static const uint8_t PERIPH[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };   /* least significant first */
static const uint8_t uuid_chr[16] = { 0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77 };

static uint8_t g_sx[512];
static ble_midi_in g_in;

void air_central_irq(void *ctx) { (void)ctx; pal_fm1_service(); }

/* the two halves of HCI as function calls: NimBLE to Cordio, Cordio to NimBLE */
static int host_to_ctrl(void *ctx, uint8_t type, const uint8_t *p, uint16_t n) { (void)ctx; return cordio_ctrl_from_host(type, p, n); }
static int ctrl_to_host(void *ctx, uint8_t type, const uint8_t *p, uint16_t n) { (void)ctx; ble_nimble_receive(type, p, n); return 0; }
static const hci_transport T = { host_to_ctrl, 0 };

static uint32_t now_ms(void *ctx) { (void)ctx; return (uint32_t)(bbm.now_us / 1000u); }

static void step(void)                  /* 100 us of the radio, the PAL, the controller */
{
    bb_model_advance_us(100);
    pal_fm1_service();
    cordio_ctrl_poll();
}

static void pump(void *ctx) { (void)ctx; for (int i = 0; i < 10; i++) step(); }   /* a millisecond while NimBLE waits */

static int g_run;                       /* "run": the test run (fm1ble_test.c) drives the stack, not this file */
static fm1ble_run R;

static void run_us(uint64_t us)
{
    uint64_t end = bbm.now_us + us;
    while (bbm.now_us < end) {
        if (g_run) {
            bb_model_advance_us(100);
            fm1ble_test_poll(&R, (uint32_t)(bbm.now_us / 1000u));
        } else {
            step();
            ble_nimble_poll();
        }
    }
}

static int is_state(int s) { return ble_nimble_state()->state == s; }

static int run_until(int (*cond)(void), uint32_t ms)
{
    uint64_t end = bbm.now_us + (uint64_t)ms * 1000u;
    while (bbm.now_us < end && !cond())
        run_us(1000);
    return cond();
}

/* ---- the central's L2CAP and ATT ---- */

static int g_used;                      /* data PDUs of C.got already taken */
static uint8_t g_frame[300];
static int g_flen, g_fwant;

/* the next whole L2CAP frame from the peripheral (CID, payload), or 0 */
static int next_frame(uint16_t *cid, const uint8_t **pl, int *n)
{
    while (g_used < C.ngot) {
        const uint8_t *p = C.got[g_used];
        int len = p[1], llid = p[0] & 3;
        g_used++;
        if (llid == 2) {
            memcpy(g_frame, p + 2, (size_t)len);
            g_flen = len;
            g_fwant = 4 + (p[2] | p[3] << 8);
        } else if (llid == 1 && g_flen) {
            memcpy(g_frame + g_flen, p + 2, (size_t)len);
            g_flen += len;
        } else {
            continue;                   /* LL control */
        }
        if (g_flen >= 4 && g_flen >= g_fwant) {
            *cid = (uint16_t)(g_frame[2] | g_frame[3] << 8);
            *pl = g_frame + 4;
            *n = g_fwant - 4;
            g_flen = 0;
            return 1;
        }
    }
    return 0;
}

static uint32_t g_upd_req, g_notes;
static uint8_t g_note[32];
static int g_note_n;

/* sends an ATT PDU in one L2CAP frame (MTU 23: one data PDU) */
static void att_send(const uint8_t *att, int n)
{
    uint8_t f[40];
    f[0] = (uint8_t)n; f[1] = 0; f[2] = 4; f[3] = 0;
    memcpy(f + 4, att, (size_t)n);
    c_send(2, f, (uint8_t)(4 + n));
}

/* runs until an ATT response with this opcode (or an Error Response) arrives; answers the signalling channel and takes
 * notifications on the way. Returns its length, 0 for none within the time. */
static int att_wait(uint8_t op, uint8_t *out, uint32_t ms)
{
    uint64_t end = bbm.now_us + (uint64_t)ms * 1000u;
    while (bbm.now_us < end) {
        uint16_t cid;
        const uint8_t *pl;
        int n;
        run_us(1000);
        while (next_frame(&cid, &pl, &n)) {
            if (cid == 5 && n >= 4 && pl[0] == 0x12) {   /* Connection Parameter Update Request: rejected (Vol 3 Part A 4.21) */
                uint8_t r[] = { 6, 0, 5, 0, 0x13, pl[1], 2, 0, 1, 0 };
                g_upd_req++;
                c_send(2, r, sizeof r);
            } else if (cid == 4 && n >= 3 && pl[0] == 0x1B) {
                g_notes++;
                g_note_n = n;
                memcpy(g_note, pl, (size_t)(n < 32 ? n : 32));
            } else if (cid == 4 && n >= 1 && (pl[0] == op || pl[0] == 0x01)) {
                memcpy(out, pl, (size_t)n);
                return n;
            }
        }
    }
    return 0;
}

static int c_advertising(void) { return is_state(BLE_NIMBLE_ADVERTISING) && C.adv_seen[0] > 0; }
static int c_connected(void) { return is_state(BLE_NIMBLE_CONNECTED); }
static int c_notify_on(void) { return ble_nimble_state()->notify_on; }

/* the central's side of a session: find the characteristic, subscribe, play a Note On; returns the value handle */
static uint16_t central_session(void)
{
    uint8_t r[64];
    uint16_t val = 0, h;
    int n, i;
    for (h = 1; h && !val;) {
        uint8_t q[7] = { 0x08, (uint8_t)h, (uint8_t)(h >> 8), 0xFF, 0xFF, 0x03, 0x28 };
        att_send(q, sizeof q);
        n = att_wait(0x09, r, 2000);
        if (n < 2 || r[0] != 0x09)
            break;
        for (i = 2; i + r[1] <= n; i += r[1]) {
            if (r[1] == 21 && !memcmp(r + i + 5, uuid_chr, 16))
                val = (uint16_t)(r[i + 3] | r[i + 4] << 8);
            h = (uint16_t)((r[i] | r[i + 1] << 8) + 1);
        }
    }
    if (val) {
        uint8_t sub[5] = { 0x12, (uint8_t)(val + 1), (uint8_t)((val + 1) >> 8), 1, 0 };
        uint8_t w[8] = { 0x52, (uint8_t)val, (uint8_t)(val >> 8), 0x80, 0x80, 0x90, 0x3C, 0x64 };
        att_send(sub, sizeof sub);
        att_wait(0x13, r, 2000);
        att_send(w, sizeof w);
    }
    return val;
}

static int c_hci_done(void) { return R.step[FM1BLE_STEP_HCI].result != FM1BLE_NOT_RUN; }
static int c_conn_done(void) { return R.step[FM1BLE_STEP_CONNECT].result != FM1BLE_NOT_RUN; }
static int c_run_done(void) { return R.state == FM1BLE_DONE; }

/* The Bluetooth test run as the FM-1 runs it (firmware/ble/fm1ble_test.c), here on the model's air with the central: the
 * driver's own advertising first (adv_set, adv_air), then the stack (hci), the central connects (connect), plays a note
 * (midi_in) and is sent one (midi_out). */
static int test_run(void)
{
    uint16_t val;
    uint8_t r[64];
    int k;
    bb_model_reset();
    bbm.air = &AIR;
    g_run = 1;
    fm1ble_test_begin(&R, 0, 4000);
    check(run_until(c_hci_done, 5000) && R.step[FM1BLE_STEP_HCI].result == FM1BLE_PASS, "the run: hci PASS (the stack on the radio)");
    for (k = 0; k < FM1BLE_STEP_HCI; k++)
        check(R.step[k].result == FM1BLE_PASS, fm1ble_step_name(k));
    check(R.step[FM1BLE_STEP_ADV_SET].result == FM1BLE_PASS && R.step[FM1BLE_STEP_ADV_AIR].result == FM1BLE_PASS,
          "the driver's own advertising first: adv_set and adv_air PASS on the air (one event per arming: it re-arms)");
    C.want_conn = 1;
    check(run_until(c_conn_done, 3000) && R.step[FM1BLE_STEP_CONNECT].result == FM1BLE_PASS, "the run: connect PASS");
    val = central_session();
    check(val != 0, "the central finds the characteristic during the run");
    att_wait(0xFF, r, 300);
    check(run_until(c_run_done, 6000), "the run ends");
    check(R.step[FM1BLE_STEP_MIDI_IN].result == FM1BLE_PASS && R.step[FM1BLE_STEP_MIDI_IN].nraw >= 3 &&
          R.step[FM1BLE_STEP_MIDI_IN].raw[0] == 0x90 && R.step[FM1BLE_STEP_MIDI_IN].raw[1] == 0x3C && R.step[FM1BLE_STEP_MIDI_IN].raw[2] == 0x64,
          "the run: midi_in PASS with the central's Note On (90 3C 64) as evidence");
    check(R.step[FM1BLE_STEP_MIDI_OUT].result == FM1BLE_PASS, "the run: midi_out PASS, the notification acknowledged");
    check(g_notes == 1 && g_note_n == 8 && g_note[5] == 0x90 && g_note[6] == 0x3C, "the central got the run's Note On");
    check((R.ctr[22] & 0xFFFFu) == 0 && (R.ctr[22] >> 16) == 0, "pal counters: no response late, nothing stuck");
    check(ble_nimble_state()->disconnects == 1, "the run closed the connection before turning the radio off");
    printf("ble_stack run: %d checks, %s (steps' ms: hci %u, connect %u, midi_in %u, midi_out %u)\n", checks, bad ? "FAILED" : "all passed",
           (unsigned)R.step[FM1BLE_STEP_HCI].ms, (unsigned)R.step[FM1BLE_STEP_CONNECT].ms, (unsigned)R.step[FM1BLE_STEP_MIDI_IN].ms,
           (unsigned)R.step[FM1BLE_STEP_MIDI_OUT].ms);
    return bad;
}

int main(int argc, char **argv)
{
    static uint8_t blk[FM1BLE_BLOCK_SIZE] __attribute__((aligned(4)));
    static fm1ble B;
    cordio_ctrl_cfg c;
    pal_fm1_cfg pc;
    ble_nimble_cfg nc;
    uint8_t r[64], adv[31];
    int n, nadv, i;
    uint16_t val = 0, h;

    if (argc > 1 && !strcmp(argv[1], "run"))
        return test_run();
    bb_model_reset();
    check(fm1ble_radio_up(&B, 0, NULL) == FM1BLE_OK && fm1ble_bb_init(&B, blk) == FM1BLE_OK, "the driver comes up on the model");
    bbm.air = &AIR;
    memset(&pc, 0, sizeof pc);
    pc.b = &B;
    pal_fm1_attach(&pc);
    memset(&c, 0, sizeof c);
    c.to_host.send = ctrl_to_host;
    c.now_ms = now_ms;
    memcpy(c.bdaddr, PERIPH, 6);
    check(cordio_ctrl_init(&c) == 0, "Cordio on the PAL");

    ble_midi_in_init(&g_in, g_sx, sizeof g_sx, NULL, NULL);
    memset(&nc, 0, sizeof nc);
    nc.t = &T;
    nc.now_ms = now_ms;
    nc.pump = pump;
    nc.name = "OPLBOX";
    nc.in = &g_in;
    check(ble_nimble_init(&nc) == 0, "ble_nimble_init over Cordio");
    check(run_until(c_advertising, 3000), "NimBLE starts Cordio (HCI start-up) and advertises: ADV_IND on the air");
    check(ble_nimble_state()->failures == 0 && ble_nimble_state()->hci_refused == 0, "no start-up failure, nothing refused");
    check(!memcmp(ble_nimble_state()->bdaddr, (const uint8_t[]){ 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 }, 6), "NimBLE read Cordio's address");
    nadv = ble_midi_adv_data(adv);
    check(C.last_adv_n == 8 + nadv && !memcmp(C.last_adv + 2, PERIPH, 6) && !memcmp(C.last_adv + 8, adv, (size_t)nadv),
          "the ADV_IND carries the BLE-MIDI service UUID (the profile's advertising data)");

    C.want_scan = 1;
    run_us(200000);
    check(C.scan_rsp == 1 && C.rsp_n > 8 && memmem(C.rsp + 8, (size_t)C.rsp_n - 8, "OPLBOX", 6) != NULL, "the scan response has the name OPLBOX");

    C.want_conn = 1;
    check(run_until(c_connected, 1000), "a CONNECT_IND: NimBLE is connected");
    check(C.chan_bad == 0 && C.aa_bad == 0 && bbm.air_missed == 0, "connection events on the right channels, none missed");

    /* find the characteristic: Read By Type (0x08) of characteristic declarations (0x2803) over the whole range */
    for (h = 1; h && !val;) {
        uint8_t q[7] = { 0x08, (uint8_t)h, (uint8_t)(h >> 8), 0xFF, 0xFF, 0x03, 0x28 };
        att_send(q, sizeof q);
        n = att_wait(0x09, r, 2000);
        if (n < 2 || r[0] != 0x09)
            break;
        for (i = 2; i + r[1] <= n; i += r[1]) {
            uint16_t decl = (uint16_t)(r[i] | r[i + 1] << 8);
            if (r[1] == 21 && !memcmp(r + i + 5, uuid_chr, 16))
                val = (uint16_t)(r[i + 3] | r[i + 4] << 8);
            h = (uint16_t)(decl + 1);
        }
    }
    check(val != 0, "the central finds the BLE-MIDI characteristic over ATT");
    check(r[0] == 0x09 && r[1] == 21 && (r[2 + 2] & 0x16) == 0x16, "its properties: read, write without response, notify");

    /* subscribe: Write Request (0x12) of 0x0001 to the client characteristic configuration descriptor after the value */
    {
        uint8_t q[5] = { 0x12, (uint8_t)(val + 1), (uint8_t)((val + 1) >> 8), 1, 0 };
        att_send(q, sizeof q);
        n = att_wait(0x13, r, 2000);
        check(n == 1 && r[0] == 0x13, "Write Response to the subscription");
        check(run_until(c_notify_on, 100), "NimBLE has notifications on");
    }

    /* the central plays a note: Write Command (0x52), a BLE-MIDI packet (header, timestamp, Note On) */
    {
        uint8_t q[8] = { 0x52, (uint8_t)val, (uint8_t)(val >> 8), 0x80, 0x80, 0x90, 0x3C, 0x64 };
        uint8_t st = 0, d1 = 0, d2 = 0;
        att_send(q, sizeof q);
        att_wait(0xFF, r, 200);
        check(ble_midi_get(&g_in, &st, &d1, &d2) == 1 && st == 0x90 && d1 == 0x3C && d2 == 0x64,
              "the central's Note On comes out of ble_midi_get: 90 3C 64");
        check(ble_nimble_state()->writes == 1, "one write counted");
    }

    /* the app plays a note: a notification */
    {
        const uint8_t pkt[5] = { 0x80, 0x81, 0x80, 0x3C, 0x00 };
        check(ble_nimble_notify(NULL, pkt, sizeof pkt) == 0, "ble_nimble_notify takes a packet");
        att_wait(0xFF, r, 300);
        check(g_notes == 1 && g_note_n == 3 + (int)sizeof pkt && (g_note[1] | g_note[2] << 8) == val && !memcmp(g_note + 3, pkt, sizeof pkt),
              "the central gets a Handle Value Notification on the characteristic with the app's Note Off");
    }
    check(g_upd_req >= 1, "NimBLE asked for a shorter connection interval over L2CAP (rejected here)");
    check(pal_fm1_state()->resp_late == 0 && pal_fm1_state()->stuck == 0 && bbm.air_missed == 0 && C.chan_bad == 0,
          "every response in time, nothing stuck or missed for the whole connection");

    /* the central leaves: LL_TERMINATE_IND; NimBLE advertises again */
    c_send(3, (const uint8_t *)"\x02\x13", 2);
    run_us(300000);
    C.connected = 0;
    C.adv_seen[0] = 0;
    check(run_until(c_advertising, 2000), "after the disconnection NimBLE advertises again, on the air");
    check(ble_nimble_state()->disconnects == 1 && ble_nimble_state()->connects == 1, "one connection, one disconnection");

    printf("ble_stack: %d checks, %s (connection events %u, PAL ops %u adv + %u conn)\n", checks, bad ? "FAILED" : "all passed",
           (unsigned)C.events, (unsigned)pal_fm1_state()->adv_ops, (unsigned)pal_fm1_state()->conn_ops);
    return bad;
}
