/* SPDX-License-Identifier: GPL-3.0-only
 * NimBLE's log calls compile to nothing: no console in the firmware, and the host test reads the HCI log instead. */
#ifndef OPLBOX_NIMBLE_NPL_OS_LOG_H
#define OPLBOX_NIMBLE_NPL_OS_LOG_H

#define BLE_NPL_LOG_IMPL(lvl) \
    static inline void _BLE_NPL_LOG_CAT(BLE_NPL_LOG_MODULE, _BLE_NPL_LOG_CAT(_, lvl))(const char *fmt, ...) { (void)fmt; }

#endif
