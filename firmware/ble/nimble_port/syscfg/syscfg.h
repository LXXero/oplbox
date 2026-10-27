/* SPDX-License-Identifier: GPL-3.0-only
 * OPLBOX's NimBLE configuration: a BLE peripheral with one connection, no pairing, no scanning, no GATT client
 * procedures. The values are set here, then NimBLE's own default file (porting/nimble/include/syscfg/syscfg.h,
 * unchanged) fills in the rest with #ifndef. */
#ifndef OPLBOX_NIMBLE_SYSCFG_H
#define OPLBOX_NIMBLE_SYSCFG_H

#define MYNEWT_VAL_BLE_ROLE_CENTRAL         (0)
#define MYNEWT_VAL_BLE_ROLE_OBSERVER        (0)
#define MYNEWT_VAL_BLE_ROLE_BROADCASTER     (1)
#define MYNEWT_VAL_BLE_ROLE_PERIPHERAL      (1)
#define MYNEWT_VAL_BLE_SM_LEGACY            (0)
#define MYNEWT_VAL_BLE_SM_SC                (0)
#define MYNEWT_VAL_BLE_MAX_CONNECTIONS      (1)
#define MYNEWT_VAL_BLE_HS_FLOW_CTRL         (0)
#define MYNEWT_VAL_BLE_VERSION              (50)
#define MYNEWT_VAL_BLE_ATT_PREFERRED_MTU    (185)
#define MYNEWT_VAL_BLE_GATT_MAX_PROCS       (2)
#define MYNEWT_VAL_BLE_L2CAP_COC_MAX_NUM    (0)
#define MYNEWT_VAL_BLE_EATT_CHAN_NUM        (0)
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS      (0)
#define MYNEWT_VAL_BLE_STORE_MAX_CCCDS      (0)
#define MYNEWT_VAL_BLE_MONITOR              (0)
/* the mbuf pool the host builds ATT PDUs in (os_msys) and the HCI transport's buffers */
#define MYNEWT_VAL_MSYS_1_BLOCK_COUNT       (8)
#define MYNEWT_VAL_MSYS_1_BLOCK_SIZE        (224)
#define MYNEWT_VAL_BLE_TRANSPORT_ACL_FROM_HS_COUNT  (3)
#define MYNEWT_VAL_BLE_TRANSPORT_ACL_FROM_LL_COUNT  (4)
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_COUNT  (3)
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_DISCARDABLE_COUNT (2)

#include "../../nimble/porting/nimble/include/syscfg/syscfg.h"

#endif
