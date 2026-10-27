/* SPDX-License-Identifier: GPL-3.0-only
 * The few C library functions the BLE stacks use and the BSP's libc.c (memset, memcpy, memcmp) does not have: a small
 * heap (NimBLE allocates its attribute and GATT tables once, at start-up and again after a restart of the host), memmove,
 * strlen, and the failed-assert hook. Own code. The heap is a static arena with first-fit blocks that merge when freed. */
#include <stddef.h>
#include <stdint.h>

#ifndef BLE_LIBC_HEAP
#define BLE_LIBC_HEAP 3072u
#endif

typedef struct blk {
    uint32_t size;                  /* payload bytes, a multiple of 8 */
    uint32_t used;
} blk;

static uint8_t g_heap[BLE_LIBC_HEAP] __attribute__((aligned(8)));
static uint8_t g_init;
uint32_t ble_libc_assert_line;      /* the line of the last failed assert (0: none), for a crash report */
const char *ble_libc_assert_file;

static void init(void)
{
    blk *b = (blk *)g_heap;
    b->size = BLE_LIBC_HEAP - sizeof(blk);
    b->used = 0;
    g_init = 1;
}

static blk *next(blk *b)
{
    uint8_t *n = (uint8_t *)b + sizeof(blk) + b->size;
    return n < g_heap + BLE_LIBC_HEAP ? (blk *)n : 0;
}

void *malloc(size_t n)
{
    blk *b;
    if (!g_init)
        init();
    if (!n || n > BLE_LIBC_HEAP)
        return 0;
    n = (n + 7u) & ~7u;
    for (b = (blk *)g_heap; b; b = next(b)) {
        if (b->used || b->size < n)
            continue;
        if (b->size >= n + sizeof(blk) + 8u) {      /* split */
            blk *r = (blk *)((uint8_t *)b + sizeof(blk) + n);
            r->size = b->size - n - (uint32_t)sizeof(blk);
            r->used = 0;
            b->size = (uint32_t)n;
        }
        b->used = 1;
        return (uint8_t *)b + sizeof(blk);
    }
    return 0;
}

void free(void *p)
{
    blk *b, *n;
    if (!p)
        return;
    b = (blk *)((uint8_t *)p - sizeof(blk));
    b->used = 0;
    for (b = (blk *)g_heap; b; b = next(b))         /* merge each free block with the free ones after it */
        while (!b->used && (n = next(b)) && !n->used)
            b->size += (uint32_t)sizeof(blk) + n->size;
}

void *realloc(void *p, size_t n)
{
    blk *b;
    void *q;
    size_t i, keep;
    if (!p)
        return malloc(n);
    b = (blk *)((uint8_t *)p - sizeof(blk));
    if (n <= b->size)
        return p;
    q = malloc(n);
    if (!q)
        return 0;
    keep = b->size;
    for (i = 0; i < keep; i++)
        ((uint8_t *)q)[i] = ((uint8_t *)p)[i];
    free(p);
    return q;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    if (a == b || !n)
        return d;
    if (a < b) {
        while (n--)
            *a++ = *b++;
    } else {
        a += n;
        b += n;
        while (n--)
            *--a = *--b;
    }
    return d;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* a failed assert: remember where, then stop; the watchdog resets the chip (a diagnostic build) */
void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    (void)func; (void)expr;
    ble_libc_assert_file = file;
    ble_libc_assert_line = (uint32_t)line;
    for (;;)
        ;
}
