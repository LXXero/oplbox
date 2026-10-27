/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of firmware/ble/ble_libc.c: the heap's first fit, splitting, merging, exhaustion, realloc, memmove with
 * overlap both ways and strlen. The functions are renamed on the host (-Dmalloc=ble_malloc and so on) so the test does not
 * replace the host's own. Exits 1 when a check fails. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
void *ble_malloc(size_t n); void ble_free(void *p); void *ble_realloc(void *p, size_t n);
void *ble_memmove(void *d, const void *s, size_t n); size_t ble_strlen(const char *s);
static int bad, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fprintf(stderr, "FAIL: %s\n", what); bad = 1; } }
int main(void)
{
    void *a, *b, *c, *big;
    char buf[16] = "abcdefghij";
    a = ble_malloc(100); b = ble_malloc(200); c = ble_malloc(300);
    check(a && b && c && a != b && b != c && ((uintptr_t)a & 7) == 0, "three blocks, 8-byte aligned, distinct");
    memset(a, 1, 100); memset(b, 2, 200); memset(c, 3, 300);
    check(((uint8_t *)a)[99] == 1 && ((uint8_t *)b)[0] == 2, "they do not overlap");
    check(ble_malloc(0) == NULL && ble_malloc(100000) == NULL, "zero bytes and more than the heap: NULL");
    ble_free(b);
    check(ble_malloc(150) == b, "a freed block is reused by a smaller request (first fit)");
    ble_free(b); ble_free(a);
    big = ble_malloc(300);
    check(big == a, "two freed neighbours merge: one request of their sum fits where the first was");
    ble_free(big); ble_free(c);
    big = ble_malloc(3000);
    check(big != NULL, "after everything is freed the whole heap (minus a header) is one block");
    check(ble_malloc(200) == NULL, "and then nothing is left");
    ble_free(big);
    a = ble_malloc(16);
    memcpy(a, "0123456789abcdef", 16);
    b = ble_realloc(a, 64);
    check(b && !memcmp(b, "0123456789abcdef", 16), "realloc keeps the contents");
    check(ble_realloc(b, 8) == b, "a smaller realloc stays");
    check(ble_realloc(NULL, 24) != NULL, "realloc of NULL allocates");
    ble_memmove(buf + 2, buf, 8);
    check(!memcmp(buf, "ababcdefgh", 10), "memmove forward with overlap");
    ble_memmove(buf, buf + 2, 8);
    check(!memcmp(buf, "abcdefghgh", 10), "memmove backward with overlap");
    check(ble_strlen("") == 0 && ble_strlen("OPLBOX") == 6, "strlen");
    printf("ble_libc: %d checks, %s\n", checks, bad ? "FAILED" : "all passed");
    return bad;
}
