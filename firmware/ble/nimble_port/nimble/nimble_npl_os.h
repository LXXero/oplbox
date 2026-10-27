/* SPDX-License-Identifier: GPL-3.0-only
 * NimBLE's OS porting layer (NPL) types for OPLBOX's polled main loop (docs/BLUETOOTH.md "Cordio path as built").
 * Own code. One thread: an event queue is a linked list the main loop drains, a callout is an event with a due
 * time in milliseconds, a mutex does nothing, a semaphore is a counter that waits by running the port's pump
 * function (the host's only blocking call is an HCI command waiting for its answer). Ticks are milliseconds. */
#ifndef OPLBOX_NIMBLE_NPL_OS_H
#define OPLBOX_NIMBLE_NPL_OS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_NPL_OS_ALIGNMENT    4
#define BLE_NPL_TIME_FOREVER    UINT32_MAX

typedef uint32_t ble_npl_time_t;
typedef int32_t ble_npl_stime_t;

struct ble_npl_event {
    void (*fn)(struct ble_npl_event *ev);
    void *arg;
    struct ble_npl_event *next;
    struct ble_npl_eventq *q;       /* the queue it is in, NULL when not queued */
};

struct ble_npl_eventq {
    struct ble_npl_event *head, *tail;
};

struct ble_npl_callout {
    struct ble_npl_event ev;
    struct ble_npl_eventq *evq;
    struct ble_npl_callout *next;   /* the list of armed callouts */
    uint32_t due;
    uint8_t active;
};

struct ble_npl_mutex {
    uint8_t held;
};

struct ble_npl_sem {
    uint16_t tokens;
};

#ifdef __cplusplus
}
#endif

#endif
