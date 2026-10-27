/* SPDX-License-Identifier: GPL-3.0-only
 * Cordio's LE controller as one object (cordio_ctrl.h, docs/BLUETOOTH_CORDIO.md): the start-up Cordio's own
 * controller/build/ble4-ctr/main.c does (read here for the order of the calls, not copied: it runs on an RTOS-less Nordic
 * board with a UART), the controller-to-host and host-to-controller halves of Cordio's CHCI transport as a function pair
 * (CHCI_TR_CUSTOM: chci_tr.c asks for ChciTrInit and ChciTrWrite and takes packets through chciTrRecv), and the main-loop
 * step. Own code. */
#include <string.h>
#include "wsf_types.h"
#include "wsf_os.h"
#include "wsf_buf.h"
#include "wsf_msg.h"
#include "wsf_timer.h"
#include "wsf_heap.h"
#include "hci_defs.h"
#include "ll_init_api.h"
#include "ll_api.h"
#include "lhci_api.h"
#include "chci_api.h"
#include "chci_tr.h"
#include "bb_api.h"
#include "pal_bb.h"
#include "cordio_ctrl.h"

#ifndef CORDIO_HEAP_BYTES
#define CORDIO_HEAP_BYTES 3072u         /* LlInit, the scheduler and the buffer pools (cordio_ctrl_state()->heap_used) */
#endif
#define CORDIO_DISPATCH_MAX 64          /* dispatcher rounds per poll: a handler that keeps queuing work must not hold the loop */

static uint32_t g_heap[CORDIO_HEAP_BYTES / 4];
static BbRtCfg_t g_bb_cfg;
static LlRtCfg_t g_ll_cfg;
static cordio_ctrl_cfg g_cfg;
static cordio_ctrl_stats g_st;
static uint16_t g_max_acl;

/* the packet the host's transport has not taken yet (from ChciTrWrite) */
static uint8_t g_hold_type;
static uint8_t *g_hold;
static uint16_t g_hold_n;

/* ---- the transport, controller to host ---- */

static int to_host(uint8_t type, const uint8_t *p, uint16_t n)
{
    return g_cfg.to_host.send(g_cfg.to_host.ctx, type, p, n);
}

void ChciTrInit(uint16_t maxAclLen, uint16_t maxIsoSduLen)
{
    (void)maxIsoSduLen;
    g_max_acl = maxAclLen;
}

/* One packet from the controller (an event or ACL data: the HCI packet without the type byte). Cordio keeps the buffer
 * until chciTrSendComplete, so a packet the host refuses is held and offered again by the next poll. */
void ChciTrWrite(uint8_t prot, uint8_t type, uint16_t len, uint8_t *pData)
{
    uint8_t h4 = type == CHCI_TR_TYPE_ACL ? HCI_ACL : HCI_EVT;
    (void)prot;
    if (type == CHCI_TR_TYPE_ACL)
        g_st.acl_out++;
    else
        g_st.events++;
    if (to_host(h4, pData, len) == 0) {
        chciTrSendComplete();
        return;
    }
    g_st.host_refused++;
    g_hold_type = h4;
    g_hold = pData;
    g_hold_n = len;
}

static void hold_retry(void)
{
    if (g_hold && to_host(g_hold_type, g_hold, g_hold_n) == 0) {
        g_hold = NULL;
        chciTrSendComplete();
    }
}

/* ---- the transport, host to controller ---- */

int cordio_ctrl_from_host(uint8_t type, const uint8_t *p, uint16_t n)
{
    uint8_t hdr, ctype;
    uint16_t len, alloc;
    uint8_t *b;

    if (type == HCI_CMD) {
        hdr = HCI_CMD_HDR_LEN;
        ctype = CHCI_TR_TYPE_CMD;
        if (n < hdr || p[2] != n - hdr)
            goto drop;
        len = p[2];
        alloc = len;
        g_st.cmds++;
    } else if (type == HCI_ACL) {
        hdr = HCI_ACL_HDR_LEN;
        ctype = CHCI_TR_TYPE_ACL;
        if (n < hdr)
            goto drop;
        len = (uint16_t)(p[2] | p[3] << 8);
        if (len != n - hdr || len > g_max_acl)
            goto drop;
        alloc = (uint16_t)(g_max_acl + CHCI_BUF_TAILROOM);
        g_st.acl_in++;
    } else {
        goto drop;
    }
    b = WsfMsgAlloc((uint16_t)(hdr + alloc));
    if (!b)
        goto drop;
    memcpy(b, p, n);
    chciTrRecv(CHCI_TR_PROT_BLE, ctype, b);
    return 0;
drop:
    g_st.rx_dropped++;
    return -1;
}

/* ---- start-up and the loop ---- */

int cordio_ctrl_init(const cordio_ctrl_cfg *cfg)
{
    uint16_t acl_buf;
    uint32_t used;
    wsfBufPoolDesc_t pools[5];
    uint8_t acl = cfg->max_acl_len ? cfg->max_acl_len : 27;

    if (acl < 27)
        return -1;
    memset(&g_st, 0, sizeof g_st);
    g_cfg = *cfg;
    g_hold = NULL;
    memset(g_heap, 0, sizeof g_heap);
    cordio_wsf_heap(g_heap, sizeof g_heap);
    cordio_wsf_clock(cfg->now_ms, cfg->ctx);

    PalBbLoadCfg((PalBbCfg_t *)&g_bb_cfg);
    LlGetDefaultRunTimeCfg(&g_ll_cfg);
    g_ll_cfg.btVer = LL_VER_BT_CORE_SPEC_4_2;
    g_ll_cfg.maxConn = 1;
    g_ll_cfg.numTxBufs = 3;
    g_ll_cfg.numRxBufs = 3;
    g_ll_cfg.maxAclLen = acl;
    g_ll_cfg.maxAdvReports = 1;                 /* no scanning: nothing is reported */

    /* +12 for the message headroom, +4 for the header (main.c's sizes) */
    acl_buf = (uint16_t)(12 + g_ll_cfg.maxAclLen + 4 + BB_DATA_PDU_TAILROOM);
    /* The controller's messages go through these pools too: LE Set Advertising Parameters is a 40-byte message and a
     * connection's set-up 96 bytes (sizes on the host), so the 64- and 128-byte pools hold several. With one 128-byte
     * buffer (the advertising reports' count) the parameter message found no buffer and was dropped without an error:
     * advertising ran at the default 1.28 s interval (found by tests/ble_pal.c). */
    pools[0].len = 16;      pools[0].num = 8;
    pools[1].len = 32;      pools[1].num = 6;
    pools[2].len = 64;      pools[2].num = 4;
    pools[3].len = 128;     pools[3].num = (uint8_t)(g_ll_cfg.maxAdvReports + 3);
    pools[4].len = acl_buf; pools[4].num = (uint8_t)(g_ll_cfg.numTxBufs + g_ll_cfg.numRxBufs);
    used = WsfBufInit(5, pools);
    if (!used)
        return -1;
    WsfHeapAlloc(used);
    WsfOsInit();
    WsfTimerInit();

    LlInitRtCfg_t ll = {
        .pBbRtCfg = &g_bb_cfg,
        .wlSizeCfg = 4,
        .rlSizeCfg = 4,
        .plSizeCfg = 4,
        .pLlRtCfg = &g_ll_cfg,
        .pFreeMem = WsfHeapGetFreeStartAddress(),
        .freeMemAvail = WsfHeapCountAvailable()
    };
    used = LlInitControllerInit(&ll);
    if (!used || used > WsfHeapCountAvailable())
        return -1;
    WsfHeapAlloc(used);
    g_st.heap_size = sizeof g_heap;
    g_st.heap_used = WsfHeapCountUsed();

    LlSetBdAddr(g_cfg.bdaddr);
    {
        uint32_t seed[2];
        memcpy(seed, g_cfg.bdaddr, 6);
        LlMathSetSeed(seed);
    }
    return 0;
}

void cordio_ctrl_poll(void)
{
    int i;
    hold_retry();
    for (i = 0; i < CORDIO_DISPATCH_MAX; i++) {
        WsfTimerSleepUpdate();
        wsfOsDispatcher();
        if (wsfOsReadyToSleep())
            break;
    }
    hold_retry();
}

const cordio_ctrl_stats *cordio_ctrl_state(void) { return &g_st; }
