#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define DINGHY_HOST 1
#include "opl3.c"
#include "opl2fast.c"
#include "macsop.h"
static opl3_chip A, B; static macsop pa, pb;
static void wa(void *c, uint16_t r, uint8_t v) { (void)c; OPL3_WriteRegBuffered(&A, r, v); }
static void wb(void *c, uint16_t r, uint8_t v) { (void)c; OPL3_WriteRegBuffered(&B, r, v); }
static int16_t gen(opl3_chip *chip, int skip) { (void)skip; return opl2_generate(chip); }
#define F(name) if (sa->name != sb->name) printf("  slot %u field %s: %d vs %d\n", i, #name, (int)sa->name, (int)sb->name)
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc(n); if (fread(blob, 1, n, f) != (size_t)n) return 1; fclose(f);
    OPL3_Reset(&A, 49716); OPL3_Reset(&B, 49716);
    macsop_load(&pa, blob, n, wa, 0); macsop_load(&pb, blob, n, wb, 0);
    uint32_t acc = 0, spr = 358042167u, t; int diffs = 0;
    for (t = 0; t < 49716u * 30u; t++) {
        if (acc < 65536u) { macsop_tick(&pa); macsop_tick(&pb); acc += spr; } acc -= 65536u;
        static int16_t yprev; int16_t bufA[2]; OPL3_Generate(&A, bufA); int16_t x = bufA[1], ynow = gen(&B, 1), y = yprev; yprev = ynow;
        if (x != y) { printf("t=%u (%.3f s) sample %d vs %d\n", t, t / 49716.0, x, y);
            for (unsigned i = 0; i < 18; i++) { opl3_slot *sa = &A.slot[i], *sb = &B.slot[i];
                F(out); F(prout); F(fbmod); F(eg_rout); F(eg_out); F(eg_gen); F(key); F(pg_phase); F(pg_phase_out); F(pg_reset); F(eg_ksl); }
            if (++diffs > 3) break; }
    }
    printf("done, %d diffs\n", diffs); return 0;
}
