/* SPDX-License-Identifier: GPL-3.0-only
 * NimBLE's OS porting layer for OPLBOX's polled main loop (docs/BLUETOOTH.md "Cordio path as built"). Own code,
 * written to the function list in nimble/include/nimble/nimble_npl.h (Apache NimBLE), nothing copied from its ports.
 * One context only: the main loop runs NimBLE and the transport's receive; no function here is called from an
 * interrupt. */
#include <nimble/nimble_npl.h>
#include <nimble/nimble_port.h>
#include "ble_nimble_npl.h"

static uint32_t (*g_now)(void *);
static void (*g_pump)(void *);
static void *g_ctx;
#define BLE_NPL_RUN_MAX 32
static struct ble_npl_callout *g_callouts;      /* armed callouts, in no order */

void ble_npl_port_set(uint32_t (*now_ms)(void *ctx), void (*pump)(void *ctx), void *ctx)
{
    g_now = now_ms;
    g_pump = pump;
    g_ctx = ctx;
}

bool ble_npl_os_started(void) { return true; }
void *ble_npl_get_current_task_id(void) { return (void *)1; }

/* ---- events */

void ble_npl_event_init(struct ble_npl_event *ev, ble_npl_event_fn *fn, void *arg)
{
    ev->fn = fn;
    ev->arg = arg;
    ev->next = 0;
    ev->q = 0;
}

bool ble_npl_event_is_queued(struct ble_npl_event *ev) { return ev->q != 0; }
void *ble_npl_event_get_arg(struct ble_npl_event *ev) { return ev->arg; }
void ble_npl_event_set_arg(struct ble_npl_event *ev, void *arg) { ev->arg = arg; }
void ble_npl_event_run(struct ble_npl_event *ev) { ev->fn(ev); }

void ble_npl_eventq_init(struct ble_npl_eventq *evq)
{
    evq->head = evq->tail = 0;
}

void ble_npl_eventq_put(struct ble_npl_eventq *evq, struct ble_npl_event *ev)
{
    if (ev->q)                      /* already queued: stays where it is */
        return;
    ev->next = 0;
    ev->q = evq;
    if (evq->tail)
        evq->tail->next = ev;
    else
        evq->head = ev;
    evq->tail = ev;
}

/* The wait argument is not used: the main loop never blocks on a queue. */
struct ble_npl_event *ble_npl_eventq_get(struct ble_npl_eventq *evq, ble_npl_time_t tmo)
{
    struct ble_npl_event *ev = evq->head;
    (void)tmo;
    if (!ev)
        return 0;
    evq->head = ev->next;
    if (!evq->head)
        evq->tail = 0;
    ev->next = 0;
    ev->q = 0;
    return ev;
}

void ble_npl_eventq_remove(struct ble_npl_eventq *evq, struct ble_npl_event *ev)
{
    struct ble_npl_event *p, *prev = 0;
    if (ev->q != evq)
        return;
    for (p = evq->head; p; prev = p, p = p->next) {
        if (p != ev)
            continue;
        if (prev)
            prev->next = p->next;
        else
            evq->head = p->next;
        if (evq->tail == p)
            evq->tail = prev;
        break;
    }
    ev->next = 0;
    ev->q = 0;
}

bool ble_npl_eventq_is_empty(struct ble_npl_eventq *evq) { return evq->head == 0; }

/* ---- mutex: one context, so nothing to exclude; the counter catches an unbalanced release */

ble_npl_error_t ble_npl_mutex_init(struct ble_npl_mutex *mu)
{
    mu->held = 0;
    return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_mutex_pend(struct ble_npl_mutex *mu, ble_npl_time_t timeout)
{
    (void)timeout;
    mu->held++;
    return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_mutex_release(struct ble_npl_mutex *mu)
{
    if (!mu->held)
        return BLE_NPL_BAD_MUTEX;
    mu->held--;
    return BLE_NPL_OK;
}

/* ---- semaphore: waits by running the pump until a token arrives or the time is up */

ble_npl_error_t ble_npl_sem_init(struct ble_npl_sem *sem, uint16_t tokens)
{
    sem->tokens = tokens;
    return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_sem_pend(struct ble_npl_sem *sem, ble_npl_time_t timeout)
{
    uint32_t t0 = ble_npl_time_get();
    while (!sem->tokens) {
        if (timeout != BLE_NPL_TIME_FOREVER && (uint32_t)(ble_npl_time_get() - t0) >= timeout)
            return BLE_NPL_TIMEOUT;
        if (!g_pump)
            return BLE_NPL_TIMEOUT; /* nothing could ever give the token */
        g_pump(g_ctx);
    }
    sem->tokens--;
    return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_sem_release(struct ble_npl_sem *sem)
{
    sem->tokens++;
    return BLE_NPL_OK;
}

uint16_t ble_npl_sem_get_count(struct ble_npl_sem *sem) { return sem->tokens; }

/* ---- time: ticks are milliseconds */

ble_npl_time_t ble_npl_time_get(void) { return g_now ? g_now(g_ctx) : 0; }

ble_npl_error_t ble_npl_time_ms_to_ticks(uint32_t ms, ble_npl_time_t *out_ticks)
{
    *out_ticks = ms;
    return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_time_ticks_to_ms(ble_npl_time_t ticks, uint32_t *out_ms)
{
    *out_ms = ticks;
    return BLE_NPL_OK;
}

ble_npl_time_t ble_npl_time_ms_to_ticks32(uint32_t ms) { return ms; }
uint32_t ble_npl_time_ticks_to_ms32(ble_npl_time_t ticks) { return ticks; }

void ble_npl_time_delay(ble_npl_time_t ticks)
{
    uint32_t t0 = ble_npl_time_get();
    while ((uint32_t)(ble_npl_time_get() - t0) < ticks) {
        if (!g_pump)
            return;
        g_pump(g_ctx);
    }
}

/* ---- callouts: an event that goes onto its queue when its time comes */

void ble_npl_callout_init(struct ble_npl_callout *co, struct ble_npl_eventq *evq, ble_npl_event_fn *ev_cb, void *ev_arg)
{
    ble_npl_event_init(&co->ev, ev_cb, ev_arg);
    co->evq = evq;
    co->next = 0;
    co->due = 0;
    co->active = 0;
}

static void callout_unlink(struct ble_npl_callout *co)
{
    struct ble_npl_callout **pp;
    for (pp = &g_callouts; *pp; pp = &(*pp)->next) {
        if (*pp == co) {
            *pp = co->next;
            break;
        }
    }
    co->next = 0;
    co->active = 0;
}

ble_npl_error_t ble_npl_callout_reset(struct ble_npl_callout *co, ble_npl_time_t ticks)
{
    if (co->active)
        callout_unlink(co);
    if (co->ev.q)
        ble_npl_eventq_remove(co->ev.q, &co->ev);
    co->due = ble_npl_time_get() + ticks;
    co->active = 1;
    co->next = g_callouts;
    g_callouts = co;
    return BLE_NPL_OK;
}

void ble_npl_callout_stop(struct ble_npl_callout *co)
{
    if (co->active)
        callout_unlink(co);
    if (co->ev.q)
        ble_npl_eventq_remove(co->ev.q, &co->ev);
}

bool ble_npl_callout_is_active(struct ble_npl_callout *co) { return co->active != 0; }
ble_npl_time_t ble_npl_callout_get_ticks(struct ble_npl_callout *co) { return co->due; }

ble_npl_time_t ble_npl_callout_remaining_ticks(struct ble_npl_callout *co, ble_npl_time_t time)
{
    return (int32_t)(co->due - time) > 0 ? co->due - time : 0;
}

void ble_npl_callout_set_arg(struct ble_npl_callout *co, void *arg) { co->ev.arg = arg; }

/* ---- critical sections: one context, nothing to mask */

uint32_t ble_npl_hw_enter_critical(void) { return 0; }
void ble_npl_hw_exit_critical(uint32_t ctx) { (void)ctx; }
bool ble_npl_hw_is_in_critical(void) { return false; }

/* ---- the main loop's step */

int ble_npl_port_run(void)
{
    struct ble_npl_callout *co, *next;
    struct ble_npl_eventq *q = nimble_port_get_dflt_eventq();
    struct ble_npl_event *ev;
    uint32_t now = ble_npl_time_get();
    int n = 0;

    for (co = g_callouts; co; co = next) {
        next = co->next;
        if ((int32_t)(now - co->due) >= 0) {
            callout_unlink(co);
            ble_npl_eventq_put(co->evq, &co->ev);
        }
    }
    /* At most this many events per call. NimBLE can queue an event from the one it runs (a reset that fails queues
     * the next one), and the loop must come back to the caller. */
    while (n < BLE_NPL_RUN_MAX && (ev = ble_npl_eventq_get(q, 0)) != 0) {
        ble_npl_event_run(ev);
        n++;
    }
    return n;
}
