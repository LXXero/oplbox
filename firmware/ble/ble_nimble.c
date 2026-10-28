/* SPDX-License-Identifier: GPL-3.0-only
 * See ble_nimble.h. Written against NimBLE's public headers (host/ble_hs.h, host/ble_gap.h, host/ble_gatt.h,
 * nimble/transport.h) and the Bluetooth Core Specification; no NimBLE example code was copied. */
#include <string.h>
#include <host/ble_hs.h>
#include <host/ble_uuid.h>
#include <nimble/nimble_port.h>
#include <nimble/transport.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include <os/os_mbuf.h>
#include "ble_nimble.h"
#include "ble_nimble_npl.h"

static ble_nimble_cfg g_cfg;
static ble_nimble_status g_st;
static uint16_t g_val_handle;
static uint8_t g_own_addr_type;
static uint8_t g_pending[HCI_ACL_MAX + 1];   /* a packet the transport refused, type byte first */
static uint16_t g_npending;
static uint8_t g_halt;                       /* the start failed: no more commands until ble_nimble_retry */

static int adv_start(void);

/* ---- the transport: NimBLE's controller side, a function pair ---- */

/* One packet to the controller. A refused packet is kept (one) and sent again by the next poll or pump. */
static int tx(uint8_t type, const uint8_t *p, uint16_t n)
{
    if (g_npending)
        return -1;
    if (g_cfg.t->send(g_cfg.t->ctx, type, p, n) == 0)
        return 0;
    g_st.hci_refused++;
    g_pending[0] = type;
    memcpy(g_pending + 1, p, n);
    g_npending = (uint16_t)(1 + n);
    return 0;
}

static void tx_retry(void)
{
    if (!g_npending)
        return;
    if (g_cfg.t->send(g_cfg.t->ctx, g_pending[0], g_pending + 1, (uint16_t)(g_npending - 1)) == 0)
        g_npending = 0;
    else
        g_st.hci_refused++;
}

void ble_transport_ll_init(void) { }

int ble_transport_to_ll_cmd_impl(void *buf)
{
    /* the command buffer holds the HCI command packet: opcode (2), length (1), parameters */
    const uint8_t *c = buf;
    int rc = g_halt ? -1 : tx(HCI_CMD, c, (uint16_t)(3 + c[2]));
    ble_transport_free(buf);
    return rc;
}

int ble_transport_to_ll_acl_impl(struct os_mbuf *om)
{
    uint8_t pkt[HCI_ACL_MAX];
    uint16_t n = OS_MBUF_PKTLEN(om);
    int rc = -1;

    if (n <= sizeof pkt && os_mbuf_copydata(om, 0, n, pkt) == 0)
        rc = tx(HCI_ACL, pkt, n);
    os_mbuf_free_chain(om);
    return rc;
}

int ble_transport_to_ll_iso_impl(struct os_mbuf *om)
{
    os_mbuf_free_chain(om);
    return -1;
}

void ble_nimble_receive(uint8_t type, const uint8_t *p, uint16_t n)
{
    if (type == HCI_EVT) {
        uint8_t *buf;
        if (n < 2u || p[1] + 2u != n || n > MYNEWT_VAL(BLE_TRANSPORT_EVT_SIZE)) {
            g_st.rx_bad++;
            return;
        }
        buf = ble_transport_alloc_evt(0);
        if (!buf) {
            g_st.rx_bad++;
            return;
        }
        memcpy(buf, p, n);
        ble_transport_to_hs_evt(buf);
    } else if (type == HCI_ACL) {
        struct os_mbuf *om = ble_transport_alloc_acl_from_ll();
        if (!om)
            return;
        if (os_mbuf_append(om, p, n) != 0) {
            os_mbuf_free_chain(om);
            return;
        }
        ble_transport_to_hs_acl(om);
    } else {
        g_st.rx_bad++;
    }
}

/* ---- GATT: the BLE-MIDI service ---- */

static int midi_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t pkt[256];
    uint16_t n;
    (void)conn; (void)attr; (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        return 0;                       /* the first read answers with no payload (BLE-MIDI section 5) */
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        n = OS_MBUF_PKTLEN(ctxt->om);
        if (n > sizeof pkt || os_mbuf_copydata(ctxt->om, 0, n, pkt) != 0)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        g_st.writes++;
        if (g_cfg.in)
            ble_midi_in_write(g_cfg.in, pkt, n);
        return 0;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static ble_uuid128_t g_svc_uuid, g_chr_uuid;
static struct ble_gatt_chr_def g_chrs[2];
static struct ble_gatt_svc_def g_svcs[2];

static void gatt_tables(void)
{
    memcpy(g_svc_uuid.value, ble_midi_service_uuid, 16);
    g_svc_uuid.u.type = BLE_UUID_TYPE_128;
    memcpy(g_chr_uuid.value, ble_midi_char_uuid, 16);
    g_chr_uuid.u.type = BLE_UUID_TYPE_128;

    memset(g_chrs, 0, sizeof g_chrs);
    g_chrs[0].uuid = &g_chr_uuid.u;
    g_chrs[0].access_cb = midi_access;
    g_chrs[0].val_handle = &g_val_handle;
    g_chrs[0].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY;
    memset(g_svcs, 0, sizeof g_svcs);
    g_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_svcs[0].uuid = &g_svc_uuid.u;
    g_svcs[0].characteristics = g_chrs;
}

/* ---- GAP ---- */

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    struct ble_gap_conn_desc d;
    (void)arg;

    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            adv_start();
            break;
        }
        g_st.state = BLE_NIMBLE_CONNECTED;
        g_st.conn = ev->connect.conn_handle;
        g_st.mtu = BLE_ATT_MTU_DFLT;
        g_st.notify_on = 0;
        if (g_st.connects < BLE_NIMBLE_CONN_MTUS)
            g_st.pref_mtu[g_st.connects] = ble_att_preferred_mtu();
        g_st.connects++;
        if (g_cfg.in)
            ble_midi_in_reset(g_cfg.in);
        if (ble_gap_conn_find(ev->connect.conn_handle, &d) == 0) {
            g_st.interval = d.conn_itvl;
            g_st.latency = d.conn_latency;
            g_st.timeout = d.supervision_timeout;
            if (d.conn_itvl > BLE_MIDI_CONN_INTERVAL_MAX) {
                struct ble_gap_upd_params up = {0};
                up.itvl_min = 8;
                up.itvl_max = BLE_MIDI_CONN_INTERVAL_MAX;
                up.latency = 0;
                up.supervision_timeout = d.supervision_timeout > 72 ? d.supervision_timeout : 72;
                g_st.update_asked++;
                ble_gap_update_params(ev->connect.conn_handle, &up);
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        g_st.disconnects++;
        g_st.conn = 0xFFFF;
        g_st.notify_on = 0;
        g_st.mtu = BLE_ATT_MTU_DFLT;
        adv_start();
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ble_gap_conn_find(ev->conn_update.conn_handle, &d) == 0) {
            g_st.interval = d.conn_itvl;
            g_st.latency = d.conn_latency;
            g_st.timeout = d.supervision_timeout;
        }
        break;
    case BLE_GAP_EVENT_MTU:
        g_st.mtu = ev->mtu.value;
        if (g_st.connects && g_st.connects <= BLE_NIMBLE_CONN_MTUS)
            g_st.conn_mtu[g_st.connects - 1] = ev->mtu.value;
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == g_val_handle)
            g_st.notify_on = ev->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    default:
        break;
    }
    return 0;
}

static int adv_start(void)
{
    struct ble_gap_adv_params p;
    uint8_t data[31], rsp[31];
    uint32_t n, m;
    int rc;

    if (g_cfg.nmtus)                    /* the next connection's MTU (ble_nimble.h cfg mtus) */
        ble_att_set_preferred_mtu(g_cfg.mtus[g_st.connects % g_cfg.nmtus]);
    n = ble_midi_adv_data(data);
    m = ble_midi_scan_rsp(rsp, g_cfg.name);
    rc = ble_gap_adv_set_data(data, (int)n);
    if (rc == 0)
        rc = ble_gap_adv_rsp_set_data(rsp, (int)m);
    if (rc != 0) {
        g_st.state = BLE_NIMBLE_FAILED;
        g_st.fail = rc;
        return rc;
    }
    memset(&p, 0, sizeof p);
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = g_cfg.adv_itvl_min ? g_cfg.adv_itvl_min : 0x30;
    p.itvl_max = g_cfg.adv_itvl_max ? g_cfg.adv_itvl_max : 0x60;
    rc = ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) {
        g_st.state = BLE_NIMBLE_FAILED;
        g_st.fail = rc;
        return rc;
    }
    g_st.state = BLE_NIMBLE_ADVERTISING;
    return 0;
}

static void on_sync(void)
{
    uint8_t addr[6];
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc == 0 && ble_hs_id_copy_addr(g_own_addr_type, addr, NULL) == 0) {
        int i;
        for (i = 0; i < 6; i++)
            g_st.bdaddr[i] = addr[5 - i];
    }
    if (rc != 0) {
        g_st.state = BLE_NIMBLE_FAILED;
        g_st.fail = rc;
        return;
    }
    adv_start();
}

/* NimBLE calls this when the controller did not answer a command (or answered it badly) and then starts over, forever,
 * each try holding the main loop for up to 2 s. The port stops there: the state is FAILED with the reason, no command
 * goes out until ble_nimble_retry, and NimBLE's own retries fail at once, without waiting. */
static void on_reset(int reason)
{
    g_st.state = BLE_NIMBLE_FAILED;
    g_st.fail = 0x100 + reason;
    g_st.failures++;
    g_halt = 1;
    g_npending = 0;
}

/* ---- the port's loop ---- */

static void pump(void *ctx)
{
    tx_retry();
    if (g_cfg.pump)
        g_cfg.pump(ctx);
}

int ble_nimble_init(const ble_nimble_cfg *cfg)
{
    int rc;

    g_cfg = *cfg;
    memset(&g_st, 0, sizeof g_st);
    g_st.conn = 0xFFFF;
    g_st.mtu = BLE_ATT_MTU_DFLT;
    g_st.state = BLE_NIMBLE_STARTING;
    g_npending = 0;
    g_halt = 0;
    ble_npl_port_set(cfg->now_ms, pump, cfg->ctx);
    nimble_port_init();
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    rc = ble_svc_gap_device_name_set(cfg->name);
    if (rc == 0) {
        gatt_tables();
        rc = ble_gatts_count_cfg(g_svcs);
    }
    if (rc == 0)
        rc = ble_gatts_add_svcs(g_svcs);
    if (rc != 0) {
        g_st.state = BLE_NIMBLE_FAILED;
        g_st.fail = rc;
    }
    return rc;
}

void ble_nimble_poll(void)
{
    tx_retry();
    ble_npl_port_run();
}

int ble_nimble_notify(void *ctx, const uint8_t *packet, uint32_t n)
{
    struct os_mbuf *om;
    (void)ctx;

    if (g_st.state != BLE_NIMBLE_CONNECTED || !g_st.notify_on || n + 3u > g_st.mtu) {
        g_st.notify_failed++;
        return -1;
    }
    om = ble_hs_mbuf_from_flat(packet, (uint16_t)n);
    if (!om || ble_gatts_notify_custom(g_st.conn, g_val_handle, om) != 0) {
        g_st.notify_failed++;
        return -1;
    }
    g_st.notified++;
    return 0;
}

void ble_nimble_retry(void)
{
    if (g_st.state != BLE_NIMBLE_FAILED)
        return;
    g_st.state = BLE_NIMBLE_STARTING;
    g_st.fail = 0;
    g_npending = 0;
    g_halt = 0;
}

int ble_nimble_disconnect(void)
{
    if (g_st.state != BLE_NIMBLE_CONNECTED)
        return -1;
    return ble_gap_terminate(g_st.conn, BLE_ERR_REM_USER_CONN_TERM) == 0 ? 0 : -1;
}

const ble_nimble_status *ble_nimble_state(void) { return &g_st; }
