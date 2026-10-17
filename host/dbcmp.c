/* Lockstep check: the firmware's dbopl2 against the original dbopl on a song's register stream,
 * generated in random-sized blocks (the same for both). usage: dbcmp song.cmf [seconds] [out.wav] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "macsop.h"
void orig_init(void); void orig_write(unsigned, unsigned); void orig_gen(unsigned, int32_t *);
void new_init(void); void new_write(unsigned, unsigned); void new_gen(unsigned, int32_t *);
static void w(void *ctx, uint16_t reg, uint8_t val) { (void)ctx; orig_write(reg, val); new_write(reg, val); }
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    double secs = argc > 2 ? atof(argv[2]) : 30;
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc(n); fread(blob, 1, n, f); fclose(f);
    orig_init(); new_init();
    macsop m; static macsop_song song;
    if (macsop_load(&m, &song, blob, (uint32_t)n, w, NULL)) { fprintf(stderr, "load failed\n"); return 1; }
    uint64_t spr = ((uint64_t)49716 << 32) / macsop_rows_per_sec_q16(&m), acc = 0;
    static int32_t a[512], b[512];
    uint64_t total = (uint64_t)(secs * 49716), done = 0, bad = 0, first = ~0ull; int playing = 1;
    int32_t peak = 0; double sq = 0; double ta = 0, tb = 0;
    FILE *wav = argc > 3 ? fopen(argv[3], "wb") : NULL;
    if (wav) { uint8_t h[44] = {0}; fwrite(h, 1, 44, wav); }
    srand(1);
    while (done < total) {
        if (acc < (1ull << 16)) { if (playing) playing = macsop_tick(&m); acc += spr; }
        acc -= 1ull << 16;
        /* generate until the next row in random blocks */
        uint32_t k = (uint32_t)(acc >> 16) + 1, left = k;   /* samples before the next tick check */
        acc -= (uint64_t)(k - 1) << 16;
        while (left) {
            uint32_t blk = 1 + rand() % 300; if (blk > left) blk = left; if (blk > 512) blk = 512;
            clock_t t0 = clock(); orig_gen(blk, a); clock_t t1 = clock(); new_gen(blk, b); clock_t t2 = clock();
            ta += t1 - t0; tb += t2 - t1;
            for (uint32_t i = 0; i < blk; i++) {
                if (a[i] != b[i]) { if (bad < 5) fprintf(stderr, "mismatch @%llu: orig %d new %d\n", (unsigned long long)(done + i), a[i], b[i]); bad++; if (first == ~0ull) first = done + i; }
                int32_t v = b[i] < 0 ? -b[i] : b[i]; if (v > peak) peak = v; sq += (double)b[i] * b[i];
                if (wav) { int32_t s = b[i]; if (s > 32767) s = 32767; if (s < -32768) s = -32768; int16_t o = (int16_t)s; fwrite(&o, 2, 1, wav); }
            }
            done += blk; left -= blk;
        }
    }
    if (wav) {
        uint32_t data = (uint32_t)done * 2, riff = 36 + data, v; uint16_t s;
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
        v = 16; fwrite(&v, 4, 1, wav); s = 1; fwrite(&s, 2, 1, wav); fwrite(&s, 2, 1, wav);
        v = 49716; fwrite(&v, 4, 1, wav); v = 49716 * 2; fwrite(&v, 4, 1, wav); s = 2; fwrite(&s, 2, 1, wav); s = 16; fwrite(&s, 2, 1, wav);
        fwrite("data", 1, 4, wav); fwrite(&data, 4, 1, wav); fclose(wav);
    }
    printf("%s: %llu samples, %llu mismatches%s; peak %d, rms %.0f; orig %.3fs new %.3fs (%.1fx / %.1fx realtime)\n",
           argv[1], (unsigned long long)done, (unsigned long long)bad, bad ? " (FAIL)" : "", peak, __builtin_sqrt(sq / done),
           ta / CLOCKS_PER_SEC, tb / CLOCKS_PER_SEC, done / 49716.0 / (ta / CLOCKS_PER_SEC), done / 49716.0 / (tb / CLOCKS_PER_SEC));
    return bad ? 1 : 0;
}
