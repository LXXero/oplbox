/* SPDX-License-Identifier: GPL-3.0-only
 * The WSF pieces Cordio's controller needs from an operating system, for OPLBOX's polled main loop (docs/BLUETOOTH_CORDIO.md).
 * Own code, written to the function list in wsf/include (wsf_timer.h, wsf_heap.h, wsf_cs.h, wsf_assert.h): nothing is copied
 * from Cordio's baremetal targets, whose timer sleeps on a 32 kHz RTC and whose heap comes from linker symbols. Here
 * the timers count milliseconds from a clock the caller gives (cordio_wsf_clock), the heap is one static array, and a
 * critical section is a counter: the controller runs from the main loop and from the baseband interrupt, and the port
 * layer masks that interrupt in pal_enter_cs (the chip's PalEnterCs), which WsfCsEnter calls. */
#include "wsf_types.h"
#include "wsf_os.h"
#include "wsf_queue.h"
#include "wsf_timer.h"
#include "wsf_heap.h"
#include "wsf_cs.h"
#include "wsf_assert.h"
#include "pal_sys.h"
#include "cordio_ctrl.h"

/* ---- heap: the controller's own memory (LlInit takes it from here), set by cordio_ctrl_init ---- */

static uint8_t *g_heap;
static uint32_t g_heap_size, g_heap_left;

void cordio_wsf_heap(void *mem, uint32_t size)
{
    g_heap = (uint8_t *)mem;
    g_heap_size = g_heap_left = size;
}

void WsfHeapAlloc(uint32_t size)
{
    size = (size + 3u) & ~3u;
    if (size > g_heap_left)
        size = g_heap_left;
    g_heap += size;
    g_heap_left -= size;
}

void *WsfHeapGetFreeStartAddress(void) { return g_heap; }
uint32_t WsfHeapCountAvailable(void) { return g_heap_left; }
uint32_t WsfHeapCountUsed(void) { return g_heap_size - g_heap_left; }

/* ---- critical sections ---- */

static uint8_t g_cs_depth;

void WsfCsEnter(void)
{
    if (g_cs_depth++ == 0)
        PalEnterCs();
}

void WsfCsExit(void)
{
    if (g_cs_depth && --g_cs_depth == 0)
        PalExitCs();
}

/* ---- assert: a counter and the chip's trap ---- */

static uint16_t g_asserts;

void WsfAssert(const char *pFile, uint16_t line)
{
    (void)pFile; (void)line;
    g_asserts++;
    PalSysAssertTrap();
}

uint16_t WsfAssertNum(void) { return g_asserts; }
void WsfAssertTrapEnable(bool_t enable) { (void)enable; }

/* ---- timers: a list ordered by ticks left, one tick a millisecond (build with WSF_MS_PER_TICK=1) ---- */

static wsfQueue_t g_timers;
static uint32_t (*g_now)(void *);
static void *g_now_ctx;
static uint32_t g_last;

void cordio_wsf_clock(uint32_t (*now_ms)(void *ctx), void *ctx)
{
    g_now = now_ms;
    g_now_ctx = ctx;
    g_last = now_ms ? now_ms(ctx) : 0;
}

static void timer_remove(wsfTimer_t *t)
{
    wsfTimer_t *p = (wsfTimer_t *)g_timers.pHead, *prev = NULL;
    while (p && p != t) {
        prev = p;
        p = p->pNext;
    }
    if (p) {
        WsfQueueRemove(&g_timers, t, prev);
        t->isStarted = FALSE;
    }
}

static void timer_insert(wsfTimer_t *t, wsfTimerTicks_t ticks)
{
    wsfTimer_t *p, *prev = NULL;
    WsfTaskLock();
    if (t->isStarted)
        timer_remove(t);
    t->isStarted = TRUE;
    t->ticks = ticks;
    p = (wsfTimer_t *)g_timers.pHead;
    while (p && t->ticks >= p->ticks) {
        prev = p;
        p = p->pNext;
    }
    WsfQueueInsert(&g_timers, t, prev);
    WsfTaskUnlock();
}

void WsfTimerInit(void) { WSF_QUEUE_INIT(&g_timers); }

/* one extra tick, so a timer never fires early (the way Cordio's own timer rounds) */
void WsfTimerStartSec(wsfTimer_t *t, wsfTimerTicks_t sec) { timer_insert(t, 1000u * sec + 1u); }
void WsfTimerStartMs(wsfTimer_t *t, wsfTimerTicks_t ms) { timer_insert(t, ms + 1u); }

void WsfTimerStop(wsfTimer_t *t)
{
    WsfTaskLock();
    timer_remove(t);
    WsfTaskUnlock();
}

void WsfTimerUpdate(wsfTimerTicks_t ticks)
{
    wsfTimer_t *p;
    WsfTaskLock();
    for (p = (wsfTimer_t *)g_timers.pHead; p; p = p->pNext) {
        if (p->ticks > ticks) {
            p->ticks -= ticks;
        } else {
            p->ticks = 0;
            WsfTaskSetReady(p->handlerId, WSF_TIMER_EVENT);
        }
    }
    WsfTaskUnlock();
}

wsfTimer_t *WsfTimerServiceExpired(wsfTaskId_t taskId)
{
    wsfTimer_t *p;
    (void)taskId;
    WsfTaskLock();
    p = (wsfTimer_t *)g_timers.pHead;
    if (p && p->ticks == 0) {
        WsfQueueRemove(&g_timers, p, NULL);
        p->isStarted = FALSE;
        WsfTaskUnlock();
        return p;
    }
    WsfTaskUnlock();
    return NULL;
}

/* The dispatcher's two calls: the first brings the timers up to the clock, the second is the sleep, which the main
 * loop does not do. */
void WsfTimerSleepUpdate(void)
{
    uint32_t now = g_now ? g_now(g_now_ctx) : g_last, d = now - g_last;
    g_last = now;
    if (d)
        WsfTimerUpdate(d);
}

void WsfTimerSleep(void) { }
