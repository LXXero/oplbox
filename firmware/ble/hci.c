/* SPDX-License-Identifier: GPL-3.0-only
 * The host side of HCI for a BLE peripheral (docs/BLUETOOTH.md stage 1). Own code, from the Bluetooth Core
 * Specification; see hci.h. Its own copy loops instead of string.h, so the directory compiles with no C library
 * headers (a struct copy still becomes a memcpy call, which the firmware provides).
 */
#include "hci.h"

static void cpy(uint8_t *d, const uint8_t *s, uint32_t n) { while (n--) *d++ = *s++; }
static void zero(uint8_t *d, uint32_t n) { while (n--) *d++ = 0; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* ---- packets ---- */

uint16_t hci_cmd_pack(uint8_t *out, uint16_t op, const uint8_t *par, uint16_t n)
{
    if (n > 255)
        return 0;
    wr16(out, op);
    out[2] = (uint8_t)n;
    cpy(out + 3, par, n);
    return (uint16_t)(3 + n);
}

uint16_t hci_acl_pack(uint8_t *out, uint16_t handle, uint8_t pb, const uint8_t *data, uint16_t n)
{
    if (n > HCI_ACL_DATA_MAX)
        return 0;
    wr16(out, (uint16_t)((handle & 0x0FFF) | (pb & 3) << 12));    /* broadcast flag 00: point to point */
    wr16(out + 2, n);
    cpy(out + 4, data, n);
    return (uint16_t)(4 + n);
}

int hci_acl_parse(const uint8_t *p, uint16_t n, uint16_t *handle, uint8_t *pb, const uint8_t **data, uint16_t *len)
{
    if (n < 4 || rd16(p + 2) != n - 4)
        return HCI_EINVAL;
    *handle = rd16(p) & 0x0FFF;
    *pb = (uint8_t)(p[1] >> 4 & 3);
    *data = p + 4;
    *len = (uint16_t)(n - 4);
    return HCI_OK;
}

int hci_evt_parse(const uint8_t *p, uint16_t n, hci_evt *e)
{
    const uint8_t *q = p + 2;
    uint8_t m;

    zero((uint8_t *)e, sizeof *e);
    if (n < 2 || p[1] != n - 2)
        return HCI_EINVAL;
    e->code = p[0];
    m = p[1];
    switch (e->code) {
    case HCI_EV_CMD_COMPLETE:                       /* ncmd, opcode, return parameters (status first) */
        if (m < 3)
            return HCI_EINVAL;
        e->ncmd = q[0];
        e->opcode = rd16(q + 1);
        if (m > 3) {
            e->status = q[3];
            e->ret = q + 4;
            e->nret = (uint8_t)(m - 4);
        }
        break;
    case HCI_EV_CMD_STATUS:                         /* status, ncmd, opcode */
        if (m != 4)
            return HCI_EINVAL;
        e->status = q[0];
        e->ncmd = q[1];
        e->opcode = rd16(q + 2);
        break;
    case HCI_EV_DISCONN_COMPLETE:                   /* status, handle, reason */
        if (m != 4)
            return HCI_EINVAL;
        e->status = q[0];
        e->handle = rd16(q + 1) & 0x0FFF;
        e->reason = q[3];
        break;
    case HCI_EV_HW_ERROR:
        if (m != 1)
            return HCI_EINVAL;
        e->status = q[0];
        break;
    case HCI_EV_NUM_COMPLETED:                      /* count, then (handle, completed) pairs */
        if (m < 1 || m != 1 + 4 * q[0])
            return HCI_EINVAL;
        e->npairs = q[0];
        e->pairs = q + 1;
        break;
    case HCI_EV_LE_META:
        if (m < 1)
            return HCI_EINVAL;
        e->sub = q[0];
        if (e->sub == HCI_LE_CONN_COMPLETE) {       /* status, handle, role, peer type, peer, interval, latency,
                                                       timeout, clock accuracy */
            if (m != 19)
                return HCI_EINVAL;
            e->status = q[1];
            e->handle = rd16(q + 2) & 0x0FFF;
            e->role = q[4];
            e->peer_type = q[5];
            cpy(e->peer, q + 6, 6);
            e->interval = rd16(q + 12);
            e->latency = rd16(q + 14);
            e->timeout = rd16(q + 16);
        } else if (e->sub == HCI_LE_CONN_UPDATE_COMPLETE) {   /* status, handle, interval, latency, timeout */
            if (m != 10)
                return HCI_EINVAL;
            e->status = q[1];
            e->handle = rd16(q + 2) & 0x0FFF;
            e->interval = rd16(q + 4);
            e->latency = rd16(q + 6);
            e->timeout = rd16(q + 8);
        }
        break;
    default:
        break;
    }
    return HCI_OK;
}

const char *hci_op_name(uint16_t op)
{
    switch (op) {
    case HCI_OP_DISCONNECT:            return "DISCONNECT";
    case HCI_OP_SET_EVENT_MASK:        return "SET EVENT MASK";
    case HCI_OP_RESET:                 return "RESET";
    case HCI_OP_READ_BUFFER_SIZE:      return "READ BUFFER SIZE";
    case HCI_OP_READ_BD_ADDR:          return "READ BD ADDR";
    case HCI_OP_LE_SET_EVENT_MASK:     return "LE SET EVENT MASK";
    case HCI_OP_LE_READ_BUFFER_SIZE:   return "LE READ BUFFER SIZE";
    case HCI_OP_LE_SET_ADV_PARAMS:     return "LE SET ADV PARAMS";
    case HCI_OP_LE_SET_ADV_DATA:       return "LE SET ADV DATA";
    case HCI_OP_LE_SET_SCAN_RSP_DATA:  return "LE SET SCAN RSP DATA";
    case HCI_OP_LE_SET_ADV_ENABLE:     return "LE SET ADV ENABLE";
    default:                           return "?";
    }
}

/* ---- H4 framing ---- */

uint32_t hci_h4_pack(uint8_t *out, uint8_t type, const uint8_t *p, uint16_t n)
{
    out[0] = type;
    cpy(out + 1, p, n);
    return 1u + n;
}

void hci_h4_init(hci_h4 *r) { zero((uint8_t *)r, sizeof *r); }

/* The packet's whole size (indicator included) once its header is in, 0 before; header sizes from 5.4. */
static uint32_t h4_size(const uint8_t *b, uint16_t have)
{
    switch (b[0]) {
    case HCI_CMD: return have >= 4 ? 4u + b[3] : 0;
    case HCI_ACL: return have >= 5 ? 5u + rd16(b + 3) : 0;
    case HCI_SCO: return have >= 4 ? 4u + b[3] : 0;
    case HCI_EVT: return have >= 3 ? 3u + b[2] : 0;
    case HCI_ISO: return have >= 5 ? 5u + (rd16(b + 3) & 0x3FFF) : 0;
    default:      return 0;
    }
}

void hci_h4_feed(hci_h4 *r, const uint8_t *b, uint32_t n,
                 void (*emit)(void *ctx, uint8_t type, const uint8_t *p, uint16_t n), void *ctx)
{
    while (n) {
        uint8_t c = *b++;
        uint32_t need;

        n--;
        if (r->skip) {
            r->skip--;
            r->dropped++;
            continue;
        }
        if (r->have == 0 && (c < HCI_CMD || c > HCI_ISO)) {
            r->dropped++;
            continue;
        }
        r->buf[r->have++] = c;
        need = r->need ? r->need : h4_size(r->buf, r->have);
        if (!need)
            continue;
        if (need > HCI_H4_MAX) {                     /* longer than any packet the host takes: throw it away */
            r->dropped += r->have;
            r->skip = (uint16_t)(need - r->have);
            r->have = r->need = 0;
            continue;
        }
        r->need = (uint16_t)need;
        if (r->have == r->need) {
            emit(ctx, r->buf[0], r->buf + 1, (uint16_t)(r->have - 1));
            r->have = r->need = 0;
        }
    }
}

/* ---- the host ---- */

int hci_ad_add(uint8_t *buf, uint8_t *n, uint8_t type, const void *data, uint8_t len)
{
    if (*n + 2u + len > 31u)
        return HCI_EINVAL;
    buf[*n] = (uint8_t)(len + 1);
    buf[*n + 1] = type;
    cpy(buf + *n + 2, data, len);
    *n = (uint8_t)(*n + 2 + len);
    return HCI_OK;
}

void hci_host_init(hci_host *h, const hci_transport *t, hci_l2cap_fn on_l2cap, void *ctx)
{
    zero((uint8_t *)h, sizeof *h);
    h->t = t;
    h->on_l2cap = on_l2cap;
    h->ctx = ctx;
    h->conn = HCI_NO_CONN;
    h->ncmd = 1;                                     /* the controller takes one command until it says otherwise */
}

static void fail(hci_host *h, uint8_t why, uint16_t op, uint8_t status)
{
    h->state = HCI_ST_FAILED;
    h->fail = why;
    h->fail_op = op;
    h->fail_status = status;
    h->wait_op = HCI_OP_NOP;
    h->ncmd_bytes = 0;
}

/* Packs a command; hci_host_poll sends it when the controller has a credit and the transport takes it. */
static void command(hci_host *h, uint16_t op, const uint8_t *par, uint16_t n)
{
    h->ncmd_bytes = hci_cmd_pack(h->cmd, op, par, n);
    h->wait_op = op;
    h->wait_ms = 0;
    hci_host_poll(h, 0);
}

static void mask(uint8_t *p, uint32_t lo, uint32_t hi)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(lo >> 8 * i);
        p[4 + i] = (uint8_t)(hi >> 8 * i);
    }
}

static void step(hci_host *h)
{
    uint8_t p[32];

    zero(p, sizeof p);
    switch (h->step) {
    case HCI_STEP_RESET:
        command(h, HCI_OP_RESET, p, 0);
        break;
    case HCI_STEP_EVENT_MASK:                        /* 32-bit halves: no 64-bit arithmetic on the chip */
        mask(p, (uint32_t)HCI_EVENT_MASK, (uint32_t)(HCI_EVENT_MASK >> 32));
        command(h, HCI_OP_SET_EVENT_MASK, p, 8);
        break;
    case HCI_STEP_LE_EVENT_MASK:
        mask(p, (uint32_t)HCI_LE_EVENT_MASK, (uint32_t)(HCI_LE_EVENT_MASK >> 32));
        command(h, HCI_OP_LE_SET_EVENT_MASK, p, 8);
        break;
    case HCI_STEP_LE_BUFFER_SIZE:
        command(h, HCI_OP_LE_READ_BUFFER_SIZE, p, 0);
        break;
    case HCI_STEP_BUFFER_SIZE:
        command(h, HCI_OP_READ_BUFFER_SIZE, p, 0);
        break;
    case HCI_STEP_BD_ADDR:
        command(h, HCI_OP_READ_BD_ADDR, p, 0);
        break;
    case HCI_STEP_ADV_PARAMS:                        /* intervals, ADV_IND, public address, no peer, channels
                                                        37-39, no filter */
        wr16(p, h->adv.int_min);
        wr16(p + 2, h->adv.int_max);
        p[13] = 0x07;
        command(h, HCI_OP_LE_SET_ADV_PARAMS, p, 15);
        break;
    case HCI_STEP_ADV_DATA:                          /* the length, then 31 bytes whatever the length */
        p[0] = h->adv.nadv;
        cpy(p + 1, h->adv.adv, h->adv.nadv);
        command(h, HCI_OP_LE_SET_ADV_DATA, p, 32);
        break;
    case HCI_STEP_SCAN_RSP:
        p[0] = h->adv.nrsp;
        cpy(p + 1, h->adv.rsp, h->adv.nrsp);
        command(h, HCI_OP_LE_SET_SCAN_RSP_DATA, p, 32);
        break;
    case HCI_STEP_ADV_ENABLE:
        p[0] = 1;
        command(h, HCI_OP_LE_SET_ADV_ENABLE, p, 1);
        break;
    }
}

int hci_host_start(hci_host *h, const hci_adv_cfg *adv)
{
    if (h->state != HCI_ST_OFF && h->state != HCI_ST_FAILED)
        return HCI_EBUSY;
    if (adv->nadv > 31 || adv->nrsp > 31)
        return HCI_EINVAL;
    h->adv = *adv;
    h->state = HCI_ST_STARTING;
    h->fail = HCI_FAIL_NONE;
    h->fail_op = HCI_OP_NOP;
    h->fail_status = 0;
    h->conn = HCI_NO_CONN;
    h->ntx = h->tx_off = h->nrx = h->rx_on = 0;
    h->ncmd = 1;                                     /* Reset goes out whatever the last credit count was */
    h->step = HCI_STEP_RESET;
    step(h);
    return HCI_OK;
}

/* The answer to the command that waits: the next step, or the start fails. */
static void answered(hci_host *h, const hci_evt *e)
{
    if (e->opcode != h->wait_op) {
        h->bad_events++;
        return;
    }
    h->wait_op = HCI_OP_NOP;
    if (e->opcode == HCI_OP_DISCONNECT)              /* a Command Status; the Disconnection Complete follows */
        return;
    if (e->status) {
        fail(h, HCI_FAIL_STATUS, e->opcode, e->status);
        return;
    }
    switch (h->step) {
    case HCI_STEP_LE_BUFFER_SIZE:                    /* LE_ACL_Data_Packet_Length, Total_Num_LE_ACL_Data_Packets */
        if (e->nret < 3) {
            fail(h, HCI_FAIL_BAD_EVENT, e->opcode, 0);
            return;
        }
        h->acl_len = rd16(e->ret);
        h->acl_total = e->ret[2];
        h->step = h->acl_len && h->acl_total ? HCI_STEP_BD_ADDR : HCI_STEP_BUFFER_SIZE;   /* 0: shared buffers */
        break;
    case HCI_STEP_BUFFER_SIZE:                       /* ACL length (2), SCO length (1), ACL count (2), SCO count */
        if (e->nret < 7 || !rd16(e->ret) || !rd16(e->ret + 3)) {
            fail(h, HCI_FAIL_BAD_EVENT, e->opcode, 0);
            return;
        }
        h->acl_len = rd16(e->ret);
        h->acl_total = (uint8_t)(rd16(e->ret + 3) > 255 ? 255 : rd16(e->ret + 3));
        h->step = HCI_STEP_BD_ADDR;
        break;
    case HCI_STEP_BD_ADDR:
        if (e->nret < 6) {
            fail(h, HCI_FAIL_BAD_EVENT, e->opcode, 0);
            return;
        }
        cpy(h->bdaddr, e->ret, 6);
        h->step++;
        break;
    case HCI_STEP_ADV_ENABLE:
        h->step = HCI_STEP_DONE;
        h->state = HCI_ST_ADVERTISING;
        return;
    default:
        h->step++;
        break;
    }
    if (h->acl_len > HCI_ACL_DATA_MAX)               /* the host sends at most what its own buffers hold */
        h->acl_len = HCI_ACL_DATA_MAX;
    h->acl_free = h->acl_total;
    step(h);
}

static void completed(hci_host *h, const hci_evt *e)
{
    for (uint8_t i = 0; i < e->npairs; i++) {
        uint16_t handle = rd16(e->pairs + 4 * i) & 0x0FFF, k = rd16(e->pairs + 4 * i + 2);
        if (handle != h->conn || h->acl_free + k > h->acl_total) {
            h->bad_events++;                         /* more than the host sent: keep the count it can trust */
            continue;
        }
        h->acl_free = (uint8_t)(h->acl_free + k);
    }
}

static void event(hci_host *h, const uint8_t *p, uint16_t n)
{
    hci_evt e;

    h->events++;
    if (hci_evt_parse(p, n, &e) != HCI_OK) {
        h->bad_events++;
        return;
    }
    switch (e.code) {
    case HCI_EV_CMD_COMPLETE:
    case HCI_EV_CMD_STATUS:
        h->ncmd = e.ncmd;
        if (e.opcode != HCI_OP_NOP)
            answered(h, &e);
        break;
    case HCI_EV_HW_ERROR:
        fail(h, HCI_FAIL_HW_ERROR, HCI_OP_NOP, e.status);
        h->conn = HCI_NO_CONN;
        break;
    case HCI_EV_NUM_COMPLETED:
        completed(h, &e);
        break;
    case HCI_EV_DISCONN_COMPLETE:
        if (e.status || e.handle != h->conn) {
            h->bad_events++;
            break;
        }
        h->disconnects++;
        h->disc_reason = e.reason;
        h->conn = HCI_NO_CONN;
        h->acl_free = h->acl_total;                  /* the controller drops what it held for the link (4.3) */
        h->ntx = h->tx_off = h->nrx = h->rx_on = 0;
        if (h->state == HCI_ST_CONNECTED) {          /* a peripheral advertises again */
            h->state = HCI_ST_STARTING;
            h->step = HCI_STEP_ADV_ENABLE;
            step(h);
        }
        break;
    case HCI_EV_LE_META:
        if (e.sub == HCI_LE_CONN_COMPLETE) {
            if (e.status) {                          /* no connection made; advertising carries on */
                h->bad_events++;
                break;
            }
            if (h->state != HCI_ST_ADVERTISING) {
                h->bad_events++;
                break;
            }
            h->state = HCI_ST_CONNECTED;             /* connectable advertising stops when a connection starts */
            h->conn = e.handle;
            h->peer_type = e.peer_type;
            cpy(h->peer, e.peer, 6);
            h->interval = e.interval;
            h->latency = e.latency;
            h->timeout = e.timeout;
            h->connects++;
        } else if (e.sub == HCI_LE_CONN_UPDATE_COMPLETE) {
            if (e.status || e.handle != h->conn) {
                h->bad_events++;
                break;
            }
            h->interval = e.interval;
            h->latency = e.latency;
            h->timeout = e.timeout;
        }
        break;
    default:
        h->bad_events++;                             /* masked off: the controller should not have sent it */
        break;
    }
}

/* One ACL fragment from the controller: the start of an L2CAP PDU (its basic header gives the length) or more of
 * the one coming in. A whole PDU goes to on_l2cap. */
static void acl(hci_host *h, const uint8_t *p, uint16_t n)
{
    uint16_t handle, len, want;
    uint8_t pb;
    const uint8_t *d;

    h->acl_in++;
    if (hci_acl_parse(p, n, &handle, &pb, &d, &len) != HCI_OK || handle != h->conn) {
        h->rx_dropped++;
        return;
    }
    if (pb == HCI_PB_FIRST_FLUSH || pb == HCI_PB_FIRST_NOFLUSH) {
        if (h->rx_on)
            h->rx_dropped++;                         /* the previous PDU never finished */
        h->nrx = 0;
        h->rx_on = 1;
    } else if (pb != HCI_PB_CONT || !h->rx_on) {
        h->rx_dropped++;
        return;
    }
    if (h->nrx + len > HCI_L2CAP_MAX) {
        h->rx_dropped++;
        h->rx_on = 0;
        return;
    }
    cpy(h->rx + h->nrx, d, len);
    h->nrx = (uint16_t)(h->nrx + len);
    if (h->nrx < 4)
        return;
    want = (uint16_t)(4 + rd16(h->rx));
    if (want > HCI_L2CAP_MAX) {
        h->rx_dropped++;
        h->rx_on = 0;
        return;
    }
    if (h->nrx < want)
        return;
    h->rx_on = 0;
    if (h->nrx > want) {                             /* more than its header says */
        h->rx_dropped++;
        return;
    }
    h->l2cap_in++;
    if (h->on_l2cap)
        h->on_l2cap(h->ctx, h->conn, rd16(h->rx + 2), h->rx + 4, (uint16_t)(want - 4));
}

void hci_host_receive(hci_host *h, uint8_t type, const uint8_t *p, uint16_t n)
{
    if (type == HCI_EVT)
        event(h, p, n);
    else if (type == HCI_ACL)
        acl(h, p, n);
    else
        h->bad_events++;
}

void hci_host_poll(hci_host *h, uint32_t elapsed_ms)
{
    /* The state is moved on before each send, so a controller that answers inside the send call (whether
     * JieLi's does is not known) finds the host as it is after the send; it is put back if the transport refuses. */
    if (h->wait_op != HCI_OP_NOP) {
        if (h->ncmd_bytes && h->ncmd) {
            uint8_t pkt[HCI_CMD_MAX];
            uint16_t nb = h->ncmd_bytes;
            cpy(pkt, h->cmd, nb);
            h->ncmd_bytes = 0;
            h->ncmd--;
            h->cmds++;
            h->wait_ms = 0;
            if (h->t->send(h->t->ctx, HCI_CMD, pkt, nb) != 0) {
                cpy(h->cmd, pkt, nb);
                h->ncmd_bytes = nb;
                h->ncmd++;
                h->cmds--;
            }
        } else {                                     /* no answer, or no credit to send it: both time out */
            h->wait_ms += elapsed_ms;
            if (h->wait_ms >= HCI_CMD_TIMEOUT_MS)
                fail(h, HCI_FAIL_TIMEOUT, h->wait_op, 0);
        }
    }
    while (h->conn != HCI_NO_CONN && h->tx_off < h->ntx && h->acl_free) {
        uint8_t pkt[HCI_ACL_MAX];
        uint16_t k = (uint16_t)(h->ntx - h->tx_off), m;
        if (k > h->acl_len)
            k = h->acl_len;
        uint16_t off = h->tx_off, ntx = h->ntx;
        m = hci_acl_pack(pkt, h->conn, off ? HCI_PB_CONT : HCI_PB_FIRST_NOFLUSH, h->tx + off, k);
        h->acl_free--;
        h->acl_out++;
        h->tx_off = (uint16_t)(off + k);
        if (h->tx_off == ntx) {
            h->l2cap_out++;
            h->ntx = h->tx_off = 0;
        }
        if (h->t->send(h->t->ctx, HCI_ACL, pkt, m) != 0) {
            h->acl_free++;
            h->acl_out--;
            if (!h->ntx && off + k == ntx)
                h->l2cap_out--;
            h->ntx = ntx;
            h->tx_off = off;
            break;
        }
    }
}

int hci_host_send_l2cap(hci_host *h, uint16_t cid, const uint8_t *p, uint16_t n)
{
    if (n > HCI_L2CAP_MAX - 4)
        return HCI_EINVAL;
    if (h->state != HCI_ST_CONNECTED || h->ntx || !h->acl_len)
        return HCI_EBUSY;
    wr16(h->tx, n);
    wr16(h->tx + 2, cid);
    cpy(h->tx + 4, p, n);
    h->ntx = (uint16_t)(4 + n);
    h->tx_off = 0;
    hci_host_poll(h, 0);
    return HCI_OK;
}

int hci_host_disconnect(hci_host *h, uint8_t reason)
{
    uint8_t p[3];

    if (h->state != HCI_ST_CONNECTED || h->wait_op != HCI_OP_NOP)
        return HCI_EBUSY;
    wr16(p, h->conn);
    p[2] = reason;
    command(h, HCI_OP_DISCONNECT, p, 3);
    return HCI_OK;
}
