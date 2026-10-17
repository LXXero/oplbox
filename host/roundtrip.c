/* Lossless check of the song model: parse every .CMF, write it back, compare byte for byte.
 * Also plays each file through the player and prints a hash of the register stream (compare across versions). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "macsop.h"
static macsop_song song;
static uint32_t h;
static void w(void *ctx, uint16_t reg, uint8_t val) { (void)ctx; h = (h ^ ((uint32_t)reg << 8 ^ val)) * 16777619u; }
int main(int argc, char **argv)
{
    int bad = 0;
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb"); if (!f) { perror(argv[a]); return 1; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *blob = malloc(n), *out = malloc(n + 65536); fread(blob, 1, n, f); fclose(f);
        int r = macsop_parse(&song, blob, (uint32_t)n);
        uint32_t m = r ? 0 : macsop_write(&song, out, (uint32_t)n + 65536);
        int same = !r && m == (uint32_t)n && !memcmp(blob, out, n);
        /* the register stream of a full play */
        macsop p; h = 2166136261u; uint32_t rows = 0;
        if (!r) { macsop_attach(&p, &song, w, 0); while (macsop_tick(&p)) rows++; }
        printf("%-13s parse %d  write %u/%ld bytes  %s  rows %u  regs %08x\n", strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a],
               r, m, n, same ? "IDENTICAL" : "DIFFERENT", rows, h);
        if (!same) { bad++; if (!r && m == (uint32_t)n) { for (long i = 0; i < n; i++) if (blob[i] != out[i]) { printf("   first diff at %ld: %02x vs %02x\n", i, blob[i], out[i]); break; } } }
        free(blob); free(out);
    }
    return bad ? 1 : 0;
}
