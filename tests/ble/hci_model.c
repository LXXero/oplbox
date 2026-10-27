/* SPDX-License-Identifier: GPL-3.0-only
 * A fake BLE controller for the host build (docs/BLUETOOTH.md stage 1); see hci_model.h. Own code. Host only.
 */
#include "hci_model.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { S_WAIT_ADV, S_WAIT_NOADV, S_WAIT_MS, S_WAIT_L2CAP, S_NOP, S_CONNECT, S_UPDATE, S_L2CAP, S_ACL, S_DISCONNECT,
       S_HWERROR, S_NOCP_AUTO, S_NOCP_HOLD, S_NOCP_RELEASE };

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void badf(hci_model *m, const char *fmt, ...)
{
    va_list ap;
    if (!m->bad++) {
        va_start(ap, fmt);
        vsnprintf(m->why, sizeof m->why, fmt, ap);
        va_end(ap);
    }
}

static void logp(hci_model *m, const char *head, const uint8_t *p, uint16_t n)
{
    char *l;
    size_t k;
    if (m->nlog >= HCI_MODEL_LOG) {
        badf(m, "log full");
        return;
    }
    l = m->log[m->nlog++];
    k = (size_t)snprintf(l, HCI_MODEL_LINE, "%s", head);
    for (uint16_t i = 0; i < n && k + 4 < HCI_MODEL_LINE; i++)
        k += (size_t)snprintf(l + k, HCI_MODEL_LINE - k, " %02x", p[i]);
}

/* A packet to the host: logged, then queued for the next tick, or delivered now with "sync". */
static void out(hci_model *m, uint8_t type, const uint8_t *p, uint16_t n)
{
    char head[24];
    if (type == HCI_EVT) {
        snprintf(head, sizeof head, "evt %02x", p[0]);
        logp(m, head, p + 2, (uint16_t)(n - 2));
    } else {
        snprintf(head, sizeof head, "acl< %04x %u", rd16(p) & 0x0FFF, p[1] >> 4 & 3);
        logp(m, head, p + 4, (uint16_t)(n - 4));
    }
    if (m->sync) {
        m->deliver(m->ctx, type, p, n);
        return;
    }
    if (m->nq >= HCI_MODEL_QUEUE) {
        badf(m, "answer queue full");
        return;
    }
    memcpy(m->q[m->nq], p, n);
    m->qn[m->nq] = n;
    m->qt[m->nq++] = type;
}

static void evt(hci_model *m, uint8_t code, const uint8_t *par, uint8_t n)
{
    uint8_t p[HCI_EVT_MAX];
    p[0] = code;
    p[1] = n;
    memcpy(p + 2, par, n);
    out(m, HCI_EVT, p, (uint16_t)(2 + n));
}

/* Sent only when the masks let it (Vol 4 Part E 7.3.1, 7.8.1). */
static int ev_on(const hci_model *m, int bit) { return (int)(m->evmask >> bit & 1); }
static int le_on(const hci_model *m, int bit) { return ev_on(m, 61) && (m->lemask >> bit & 1); }

static void cmd_complete(hci_model *m, uint16_t op, const uint8_t *ret, uint8_t n)
{
    uint8_t p[255];
    p[0] = m->ncmd;
    wr16(p + 1, op);
    memcpy(p + 3, ret, n);
    m->credits = m->ncmd;
    evt(m, HCI_EV_CMD_COMPLETE, p, (uint8_t)(3 + n));
}

static void cmd_status(hci_model *m, uint16_t op, uint8_t status)
{
    uint8_t p[4] = { status, m->ncmd, (uint8_t)op, (uint8_t)(op >> 8) };
    m->credits = m->ncmd;
    evt(m, HCI_EV_CMD_STATUS, p, 4);
}

static void nocp(hci_model *m, uint16_t k)
{
    uint8_t p[5] = { 1, (uint8_t)m->conn, (uint8_t)(m->conn >> 8), (uint8_t)k, (uint8_t)(k >> 8) };
    m->outstanding = (uint16_t)(m->outstanding - k);
    evt(m, HCI_EV_NUM_COMPLETED, p, 5);
}

/* The state changes before the event goes out: with "sync" the host answers it at once. */
static void disconnected(hci_model *m, uint8_t reason)
{
    uint8_t p[4] = { 0, (uint8_t)m->conn, (uint8_t)(m->conn >> 8), reason };
    m->conn = HCI_NO_CONN;
    m->outstanding = m->held = 0;
    m->l2_on = 0;
    if (ev_on(m, 4))
        evt(m, HCI_EV_DISCONN_COMPLETE, p, 4);
}

void hci_model_init(hci_model *m, void (*deliver)(void *ctx, uint8_t type, const uint8_t *p, uint16_t n), void *ctx)
{
    static const uint8_t addr[6] = { 0x01, 0x00, 0x31, 0x4D, 0x46, 0x02 };
    memset(m, 0, sizeof *m);
    m->buf_len = 27;
    m->buf_count = 2;
    m->shared_len = 27;
    m->shared_count = 4;
    memcpy(m->bdaddr, addr, 6);
    m->ncmd = 1;
    m->credits = 1;
    m->conn = HCI_NO_CONN;
    m->evmask = 0x00001FFFFFFFFFFFull;          /* the defaults after power-up and Reset */
    m->lemask = 0x1F;
    m->deliver = deliver;
    m->ctx = ctx;
}

/* ---- commands from the host ---- */

static int want_len(uint16_t op)
{
    switch (op) {
    case HCI_OP_RESET: case HCI_OP_READ_BUFFER_SIZE: case HCI_OP_READ_BD_ADDR: case HCI_OP_LE_READ_BUFFER_SIZE:
        return 0;
    case HCI_OP_SET_EVENT_MASK: case HCI_OP_LE_SET_EVENT_MASK: return 8;
    case HCI_OP_LE_SET_ADV_PARAMS: return 15;
    case HCI_OP_LE_SET_ADV_DATA: case HCI_OP_LE_SET_SCAN_RSP_DATA: return 32;
    case HCI_OP_LE_SET_ADV_ENABLE: return 1;
    case HCI_OP_DISCONNECT: return 3;
    default: return -1;
    }
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/* The commands a full host asks beside the ones a minimal host needs (the "nimble" setting). 1: answered. */
static int nimble_command(hci_model *m, uint16_t op, const uint8_t *a, uint8_t n)
{
    uint8_t r[70] = { 0 };
    switch (op) {
    case 0x1001:                                /* Read Local Version Information (7.4.1): HCI 5.0 (9), LMP 9 */
        r[1] = 9; r[4] = 9;
        wr16(r + 5, 0xFFFF);                    /* manufacturer: none assigned */
        cmd_complete(m, op, r, 9);
        return 1;
    case 0x1002:                                /* Read Local Supported Commands (7.4.2): 64 bytes, none claimed */
        cmd_complete(m, op, r, 65);
        return 1;
    case 0x1003:                                /* Read Local Supported Features (7.4.3): LE Supported, BR/EDR not */
        r[1 + 4] = 0x60;
        cmd_complete(m, op, r, 9);
        return 1;
    case 0x2003:                                /* LE Read Local Supported Features (7.8.3): connection parameters request */
        r[1] = 0x02;
        cmd_complete(m, op, r, 9);
        return 1;
    case 0x0C63:                                /* Set Event Mask Page 2 (7.3.69) */
        if (n != 8)
            badf(m, "command %04x with %u parameter bytes, the specification says 8", op, n);
        cmd_complete(m, op, r, 1);
        return 1;
    case 0x2013: {                              /* LE Connection Update (7.8.18): handle, intervals, latency, timeout, CE */
        uint8_t p[10] = { HCI_LE_CONN_UPDATE_COMPLETE };
        if (n != 14)
            badf(m, "command %04x with %u parameter bytes, the specification says 14", op, n);
        if (m->conn == HCI_NO_CONN || (rd16(a) & 0x0FFF) != m->conn) {
            cmd_status(m, op, 0x02);
            return 1;
        }
        cmd_status(m, op, 0);
        wr16(p + 2, m->conn);
        wr16(p + 4, rd16(a + 4));               /* the central grants the largest interval asked */
        wr16(p + 6, rd16(a + 6));
        wr16(p + 8, rd16(a + 8));
        if (le_on(m, 2))
            evt(m, HCI_EV_LE_META, p, 10);
        return 1;
    }
    default:
        return 0;
    }
}

static void command(hci_model *m, const uint8_t *p, uint16_t n)
{
    uint16_t op;
    const uint8_t *a = p + 3;
    uint8_t r[16] = { 0 };
    int w;
    char head[16];

    if (n < 3 || p[2] != n - 3) {
        badf(m, "command packet of %u bytes says %u parameter bytes", n, n >= 3 ? p[2] : 0);
        return;
    }
    op = rd16(p);
    snprintf(head, sizeof head, "cmd %04x", op);
    logp(m, head, a, p[2]);
    m->cmds++;
    if (!m->credits)
        badf(m, "command %04x sent without a credit", op);
    else
        m->credits--;
    w = want_len(op);
    if (w >= 0 && w != p[2])
        badf(m, "command %04x with %u parameter bytes, the specification says %d", op, p[2], w);
    if (op == m->silent_op)
        return;
    if (op == m->status_op && m->status) {
        if (op == HCI_OP_DISCONNECT)
            cmd_status(m, op, m->status);
        else {
            r[0] = m->status;
            cmd_complete(m, op, r, 1);
        }
        return;
    }
    switch (op) {
    case HCI_OP_RESET:
        if (m->conn != HCI_NO_CONN)
            m->conn = HCI_NO_CONN;
        m->adv_on = 0;
        m->evmask = 0x00001FFFFFFFFFFFull;
        m->lemask = 0x1F;
        m->outstanding = m->held = 0;
        cmd_complete(m, op, r, 1);
        break;
    case HCI_OP_SET_EVENT_MASK:
        m->evmask = rd64(a);
        cmd_complete(m, op, r, 1);
        break;
    case HCI_OP_LE_SET_EVENT_MASK:
        m->lemask = rd64(a);
        cmd_complete(m, op, r, 1);
        break;
    case HCI_OP_LE_READ_BUFFER_SIZE:
        wr16(r + 1, m->buf_len);
        r[3] = m->buf_count;
        cmd_complete(m, op, r, 4);
        break;
    case HCI_OP_READ_BUFFER_SIZE:               /* ACL length, SCO length, ACL count, SCO count */
        wr16(r + 1, m->shared_len);
        r[3] = 0;
        wr16(r + 4, m->shared_count);
        wr16(r + 6, 0);
        cmd_complete(m, op, r, 8);
        break;
    case HCI_OP_READ_BD_ADDR:
        memcpy(r + 1, m->bdaddr, 6);
        cmd_complete(m, op, r, 7);
        break;
    case HCI_OP_LE_SET_ADV_PARAMS: {            /* 7.8.5: intervals 0x0020-0x4000, min <= max, a channel */
        uint16_t lo = rd16(a), hi = rd16(a + 2);
        r[0] = (lo < 0x20 || hi > 0x4000 || lo > hi || a[4] > 4 || !(a[13] & 7) || a[13] > 7) ? HCI_ERR_INVALID_PARAMS : 0;
        cmd_complete(m, op, r, 1);
        break;
    }
    case HCI_OP_LE_SET_ADV_DATA:
    case HCI_OP_LE_SET_SCAN_RSP_DATA:
        r[0] = a[0] > 31 ? HCI_ERR_INVALID_PARAMS : 0;
        cmd_complete(m, op, r, 1);
        break;
    case HCI_OP_LE_SET_ADV_ENABLE:
        if (a[0] && m->conn != HCI_NO_CONN)
            r[0] = 0x0C;                         /* Command Disallowed: one connection, no advertising beside it */
        else if (a[0] > 1)
            r[0] = HCI_ERR_INVALID_PARAMS;
        else
            m->adv_on = a[0];
        cmd_complete(m, op, r, 1);
        break;
    case HCI_OP_DISCONNECT:
        if (m->conn == HCI_NO_CONN || (rd16(a) & 0x0FFF) != m->conn) {
            cmd_status(m, op, 0x02);             /* Unknown Connection Identifier */
            break;
        }
        cmd_status(m, op, 0);
        disconnected(m, HCI_ERR_LOCAL_HOST_TERM);
        break;
    default:
        if (m->nimble && nimble_command(m, op, a, p[2]))
            break;
        r[0] = HCI_ERR_UNKNOWN_COMMAND;
        cmd_complete(m, op, r, 1);
        break;
    }
}

/* The buffers it reported: its own LE ones, or the shared ones when it has none (7.8.2). */
static uint16_t acl_len(const hci_model *m) { return m->buf_len && m->buf_count ? m->buf_len : m->shared_len; }
static uint16_t acl_count(const hci_model *m) { return m->buf_len && m->buf_count ? m->buf_count : m->shared_count; }

/* The "nimble" setting: the central accepts an L2CAP Connection Parameter Update Request (Vol 3 Part A 4.20: code 0x12,
 * identifier, length 8, interval min and max, latency, timeout) with the Response (0x13, result 0, accepted) and
 * moves the connection to the largest interval asked, as LE Connection Update Complete. */
static void nimble_param_request(hci_model *m)
{
    uint8_t rsp[10], pkt[HCI_ACL_MAX], p[10] = { HCI_LE_CONN_UPDATE_COMPLETE };
    const uint8_t *a = m->last + 4;
    wr16(rsp, 6);
    wr16(rsp + 2, L2CAP_CID_LE_SIG);
    rsp[4] = 0x13;
    rsp[5] = m->last[1];
    wr16(rsp + 6, 2);
    wr16(rsp + 8, 0);
    out(m, HCI_ACL, pkt, hci_acl_pack(pkt, m->conn, HCI_PB_FIRST_FLUSH, rsp, 10));
    wr16(p + 2, m->conn);
    wr16(p + 4, rd16(a + 2));
    wr16(p + 6, rd16(a + 4));
    wr16(p + 8, rd16(a + 6));
    if (le_on(m, 2))
        evt(m, HCI_EV_LE_META, p, 10);
}

/* ACL data from the host: checked, then put back together as the central would. */
static void acl(hci_model *m, const uint8_t *p, uint16_t n)
{
    uint16_t handle, len;
    uint8_t pb;
    const uint8_t *d;
    char head[24];

    if (hci_acl_parse(p, n, &handle, &pb, &d, &len) != HCI_OK) {
        badf(m, "ACL packet of %u bytes with a wrong length field", n);
        return;
    }
    snprintf(head, sizeof head, "acl> %04x %u", handle, pb);
    logp(m, head, d, len);
    m->acl_from_host++;
    if (m->conn == HCI_NO_CONN || handle != m->conn) {
        badf(m, "ACL data for handle %04x with %s", handle, m->conn == HCI_NO_CONN ? "no connection" : "another one open");
        return;
    }
    if (len > acl_len(m))
        badf(m, "ACL packet of %u data bytes, the controller said %u", len, acl_len(m));
    if (m->outstanding >= acl_count(m))
        badf(m, "ACL packet beyond the %u buffers the controller has", acl_count(m));
    m->outstanding++;
    if (m->nocp_mode)
        m->held++;
    else
        nocp(m, 1);
    if (pb == HCI_PB_FIRST_NOFLUSH) {
        if (m->l2_on)
            badf(m, "a new L2CAP PDU before the last one was whole");
        m->nl2 = 0;
        m->l2_on = 1;
    } else if (pb != HCI_PB_CONT || !m->l2_on) {
        badf(m, "ACL fragment with Packet_Boundary %u %s", pb, pb == HCI_PB_CONT ? "and no PDU started" : "from a host");
        return;
    }
    if (m->nl2 + len > HCI_L2CAP_MAX) {
        badf(m, "L2CAP PDU from the host longer than %u", HCI_L2CAP_MAX);
        m->l2_on = 0;
        return;
    }
    memcpy(m->l2 + m->nl2, d, len);
    m->nl2 = (uint16_t)(m->nl2 + len);
    if (m->nl2 >= 4 && m->nl2 >= 4 + rd16(m->l2)) {
        if (m->nl2 > 4 + rd16(m->l2))
            badf(m, "L2CAP PDU from the host longer than its header says");
        m->last_cid = rd16(m->l2 + 2);
        m->nlast = (uint16_t)(m->nl2 - 4);
        memcpy(m->last, m->l2 + 4, m->nlast);
        m->l2cap_from_host++;
        m->l2_on = 0;
        if (m->nimble && m->last_cid == L2CAP_CID_LE_SIG && m->nlast == 12 && m->last[0] == 0x12)
            nimble_param_request(m);
    }
}

int hci_model_send(void *ctx, uint8_t type, const uint8_t *p, uint16_t n)
{
    hci_model *m = ctx;
    if (type == HCI_CMD)
        command(m, p, n);
    else if (type == HCI_ACL)
        acl(m, p, n);
    else
        badf(m, "packet type %u from the host", type);
    return 0;
}

/* ---- the clock and the steps ---- */

static void l2cap_in(hci_model *m, const hci_model_step *s)
{
    uint8_t pdu[HCI_L2CAP_MAX], pkt[HCI_ACL_MAX];
    uint16_t n = (uint16_t)(4 + s->n), frag = (uint16_t)(s->b ? s->b : 27);
    wr16(pdu, s->n);
    wr16(pdu + 2, (uint16_t)s->a);
    memcpy(pdu + 4, s->data, s->n);
    for (uint16_t off = 0; off < n; off = (uint16_t)(off + frag)) {
        uint16_t k = (uint16_t)(n - off < frag ? n - off : frag);
        out(m, HCI_ACL, pkt, hci_acl_pack(pkt, m->conn, off ? HCI_PB_CONT : HCI_PB_FIRST_FLUSH, pdu + off, k));
    }
}

/* Runs the current step if its time has come. Returns 1 when it ran. */
static int run_step(hci_model *m)
{
    const hci_model_step *s = &m->steps[m->cur];
    uint8_t p[32] = { 0 };

    switch (s->kind) {
    case S_WAIT_ADV:   if (!m->adv_on) return 0; break;
    case S_WAIT_NOADV: if (m->adv_on) return 0; break;
    case S_WAIT_MS:    if (m->now - m->step_t < s->a) return 0; break;
    case S_WAIT_L2CAP: if (m->l2cap_from_host < s->a) return 0; break;
    case S_NOP: {                                 /* one credit, whatever ncmd says */
        uint8_t k = m->ncmd;
        m->ncmd = 1;
        cmd_complete(m, HCI_OP_NOP, p, 0);
        m->ncmd = k;
        break;
    }
    case S_CONNECT:                               /* 7.7.65.1: status, handle, role 1 (peripheral), peer type, peer,
                                                     interval, latency, timeout, clock accuracy */
        p[0] = HCI_LE_CONN_COMPLETE;
        wr16(p + 2, (uint16_t)s->a);
        p[4] = 1;
        p[5] = (uint8_t)s->b;
        memcpy(p + 6, s->addr, 6);
        wr16(p + 12, (uint16_t)s->c);
        wr16(p + 14, (uint16_t)(s->d >> 16));
        wr16(p + 16, (uint16_t)s->d);
        p[18] = 0;
        if (!m->adv_on)
            badf(m, "script: connect while not advertising");
        m->adv_on = 0;
        m->conn = (uint16_t)s->a;
        if (le_on(m, 0))
            evt(m, HCI_EV_LE_META, p, 19);
        break;
    case S_UPDATE:                                /* 7.7.65.3 */
        p[0] = HCI_LE_CONN_UPDATE_COMPLETE;
        wr16(p + 2, m->conn);
        wr16(p + 4, (uint16_t)s->a);
        wr16(p + 6, (uint16_t)s->b);
        wr16(p + 8, (uint16_t)s->c);
        if (le_on(m, 2))
            evt(m, HCI_EV_LE_META, p, 10);
        break;
    case S_L2CAP:
        l2cap_in(m, s);
        break;
    case S_ACL: {
        uint8_t pkt[HCI_ACL_MAX];
        out(m, HCI_ACL, pkt, hci_acl_pack(pkt, s->b ? (uint16_t)s->b : m->conn, (uint8_t)s->a, s->data, s->n));
        break;
    }
    case S_DISCONNECT:
        disconnected(m, (uint8_t)s->a);
        break;
    case S_HWERROR:
        p[0] = (uint8_t)s->a;
        m->adv_on = 0;
        m->conn = HCI_NO_CONN;
        if (ev_on(m, 15))
            evt(m, HCI_EV_HW_ERROR, p, 1);
        break;
    case S_NOCP_AUTO: m->nocp_mode = 0; break;
    case S_NOCP_HOLD: m->nocp_mode = 1; break;
    case S_NOCP_RELEASE:
        m->nocp_mode = 0;
        if (m->held && m->conn != HCI_NO_CONN)
            nocp(m, m->held);
        m->held = 0;
        break;
    }
    m->cur++;
    m->step_t = m->now;
    return 1;
}

void hci_model_tick(hci_model *m, uint32_t ms)
{
    int nq = m->nq;
    uint8_t q[HCI_MODEL_QUEUE][sizeof m->q[0]];
    uint16_t qn[HCI_MODEL_QUEUE];
    uint8_t qt[HCI_MODEL_QUEUE];

    m->now += ms;
    memcpy(q, m->q, sizeof q);                    /* what the host's answers to these add waits for the next tick */
    memcpy(qn, m->qn, sizeof qn);
    memcpy(qt, m->qt, sizeof qt);
    m->nq = 0;
    for (int i = 0; i < nq; i++)
        m->deliver(m->ctx, qt[i], q[i], qn[i]);
    while (m->cur < m->nsteps && run_step(m))
        ;
}

int hci_model_done(const hci_model *m) { return m->cur >= m->nsteps; }

/* ---- the script ---- */

static int hexbytes(char *t, uint8_t *d, uint16_t max, uint16_t *n)
{
    *n = 0;
    for (char *w = strtok(t, " \t"); w; w = strtok(NULL, " \t")) {
        char *e;
        unsigned long v = strtoul(w, &e, 16);
        if (*e || v > 255 || *n >= max)
            return -1;
        d[(*n)++] = (uint8_t)v;
    }
    return 0;
}

static int addr(const char *s, uint8_t *a)                /* aa:bb:cc:dd:ee:ff, most significant first */
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++)
        a[i] = (uint8_t)v[5 - i];
    return 0;
}

/* key=value from a step's words; the rest (bytes) are left in rest. */
static int kv(char *line, const char *key, unsigned long *v, int base)
{
    char pat[24], *p;
    snprintf(pat, sizeof pat, "%s=", key);
    for (p = strstr(line, pat); p && p != line && !isspace((unsigned char)p[-1]); p = strstr(p + 1, pat))
        ;
    if (!p)
        return 0;
    *v = strtoul(p + strlen(pat), NULL, base);
    memset(p, ' ', strcspn(p, " \t"));
    return 1;
}

int hci_model_load(hci_model *m, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    int no = 0;

    if (!f) {
        snprintf(m->why, sizeof m->why, "%s: cannot open", path);
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        char *c = strchr(line, '#'), w0[16] = "", w1[32] = "", w2[32] = "";
        unsigned long v;
        hci_model_step *s;
        int err = 0;

        no++;
        if (c)
            *c = 0;
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%15s %31s %31s", w0, w1, w2) < 1)
            continue;
        if (!strcmp(w0, "bufsize") || !strcmp(w0, "shared")) {
            unsigned len, cnt;
            err = sscanf(line, "%*s %u %u", &len, &cnt) != 2;
            if (w0[0] == 'b') { m->buf_len = (uint16_t)len; m->buf_count = (uint8_t)cnt; }
            else { m->shared_len = (uint16_t)len; m->shared_count = (uint8_t)cnt; }
        } else if (!strcmp(w0, "bdaddr")) {
            err = addr(w1, m->bdaddr);
        } else if (!strcmp(w0, "ncmd")) {
            m->ncmd = (uint8_t)strtoul(w1, NULL, 10);
        } else if (!strcmp(w0, "status")) {
            m->status_op = (uint16_t)strtoul(w1, NULL, 16);
            m->status = (uint8_t)strtoul(w2, NULL, 16);
        } else if (!strcmp(w0, "silent")) {
            m->silent_op = (uint16_t)strtoul(w1, NULL, 16);
        } else if (!strcmp(w0, "sync")) {
            m->sync = 1;
        } else if (!strcmp(w0, "nimble")) {
            m->nimble = 1;
        } else {
            if (m->nsteps >= HCI_MODEL_STEPS) {
                err = 1;
                goto done;
            }
            s = &m->steps[m->nsteps];
            memset(s, 0, sizeof *s);
            if (!strcmp(w0, "wait")) {
                if (!strcmp(w1, "adv")) s->kind = S_WAIT_ADV;
                else if (!strcmp(w1, "noadv")) s->kind = S_WAIT_NOADV;
                else if (!strcmp(w1, "ms")) { s->kind = S_WAIT_MS; s->a = (uint32_t)strtoul(w2, NULL, 10); }
                else if (!strcmp(w1, "l2cap")) { s->kind = S_WAIT_L2CAP; s->a = (uint32_t)strtoul(w2, NULL, 10); }
                else err = 1;
            } else if (!strcmp(w0, "nop")) {
                s->kind = S_NOP;
            } else if (!strcmp(w0, "connect")) {
                char *p = strstr(line, "peer=");
                unsigned long iv = 24, lat = 0, to = 72, ty = 0;
                s->kind = S_CONNECT;
                err = !p || addr(p + 5, s->addr) || !kv(line, "handle", &v, 16);
                s->a = (uint32_t)v;
                kv(line, "type", &ty, 10);
                kv(line, "interval", &iv, 10);
                kv(line, "latency", &lat, 10);
                kv(line, "timeout", &to, 10);
                s->b = (uint32_t)ty;
                s->c = (uint32_t)iv;
                s->d = (uint32_t)(lat << 16 | to);
            } else if (!strcmp(w0, "update")) {
                unsigned long iv = 24, lat = 0, to = 72;
                s->kind = S_UPDATE;
                kv(line, "interval", &iv, 10);
                kv(line, "latency", &lat, 10);
                kv(line, "timeout", &to, 10);
                s->a = (uint32_t)iv;
                s->b = (uint32_t)lat;
                s->c = (uint32_t)to;
            } else if (!strcmp(w0, "l2cap") || !strcmp(w0, "acl")) {
                char *rest = line + strspn(line, " \t") + strlen(w0);
                s->kind = w0[0] == 'l' ? S_L2CAP : S_ACL;
                if (s->kind == S_L2CAP) {
                    err = !kv(rest, "cid", &v, 16);
                    s->a = (uint32_t)v;
                    if (kv(rest, "frag", &v, 10))
                        s->b = (uint32_t)v;
                } else {
                    err = !kv(rest, "pb", &v, 10);
                    s->a = (uint32_t)v;
                    if (kv(rest, "handle", &v, 16))
                        s->b = 0x10000u | (uint32_t)v;
                }
                err = err || hexbytes(rest, s->data, HCI_L2CAP_MAX - 4, &s->n);
            } else if (!strcmp(w0, "disconnect")) {
                s->kind = S_DISCONNECT;
                err = !kv(line, "reason", &v, 16);
                s->a = (uint32_t)v;
            } else if (!strcmp(w0, "hwerror")) {
                s->kind = S_HWERROR;
                s->a = (uint32_t)strtoul(w1, NULL, 16);
            } else if (!strcmp(w0, "nocp")) {
                if (!strcmp(w1, "auto")) s->kind = S_NOCP_AUTO;
                else if (!strcmp(w1, "hold")) s->kind = S_NOCP_HOLD;
                else if (!strcmp(w1, "release")) s->kind = S_NOCP_RELEASE;
                else err = 1;
            } else {
                err = 1;
            }
            if (!err)
                m->nsteps++;
        }
    done:
        if (err) {
            snprintf(m->why, sizeof m->why, "%s:%d: cannot read '%s'", path, no, w0);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

/* ---- fixtures ---- */

/* One record: comments and surplus spaces gone, lower case. Empty when nothing is left. */
static void norm(const char *in, char *outp, size_t n)
{
    size_t k = 0;
    int sp = 0;
    for (; *in && *in != '#' && *in != '\n' && *in != '\r' && k + 1 < n; in++) {
        if (isspace((unsigned char)*in)) {
            sp = k > 0;
            continue;
        }
        if (sp)
            outp[k++] = ' ';
        sp = 0;
        outp[k++] = (char)tolower((unsigned char)*in);
    }
    outp[k] = 0;
}

/* A log line against a fixture record: word by word, "??" matching any byte. */
static int same(const char *got, const char *want)
{
    char g[HCI_MODEL_LINE * 2], w[HCI_MODEL_LINE * 2], *sg, *sw;
    char *a, *b;
    snprintf(g, sizeof g, "%s", got);
    snprintf(w, sizeof w, "%s", want);
    a = strtok_r(g, " ", &sg);
    b = strtok_r(w, " ", &sw);
    while (a && b) {
        if (strcmp(b, "??") && strcmp(a, b))
            return 0;
        a = strtok_r(NULL, " ", &sg);
        b = strtok_r(NULL, " ", &sw);
    }
    return !a && !b;
}

int hci_model_diff(const hci_model *m, const char *path, char *why, size_t n)
{
    FILE *f = fopen(path, "r");
    char line[1024], want[1024];
    int i = 0, no = 0;

    if (!f) {
        snprintf(why, n, "%s: cannot open", path);
        return 1;
    }
    while (fgets(line, sizeof line, f)) {
        no++;
        norm(line, want, sizeof want);
        if (!want[0])
            continue;
        if (i >= m->nlog) {
            snprintf(why, n, "%s:%d: want '%s', the log ended after %d packets", path, no, want, m->nlog);
            fclose(f);
            return 1;
        }
        if (!same(m->log[i], want)) {
            snprintf(why, n, "%s:%d: packet %d is '%s', want '%s'", path, no, i + 1, m->log[i], want);
            fclose(f);
            return 1;
        }
        i++;
    }
    fclose(f);
    if (i < m->nlog) {
        snprintf(why, n, "%s: ends after %d packets, the log has more: '%s'", path, i, m->log[i]);
        return 1;
    }
    return 0;
}

int hci_model_write(const hci_model *m, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    for (int i = 0; i < m->nlog; i++)
        fprintf(f, "%s\n", m->log[i]);
    return fclose(f);
}
