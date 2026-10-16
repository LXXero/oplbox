/* Host reference: render a Macs Opera .CMF to WAV through Nuked-OPL3, time the core.
 *   render <song.cmf> <out.wav> [rate] [--regs regs.txt] [--seconds N] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "macsop.h"
#include "opl3.h"

static FILE *regf;
static opl3_chip chip;
static uint64_t sample_pos;

static void oplwrite(void *ctx, uint16_t reg, uint8_t val)
{
    (void)ctx;
    OPL3_WriteRegBuffered(&chip, reg, val);
    if (regf) fprintf(regf, "%llu %02X %02X\n", (unsigned long long)sample_pos, reg, val);
}

static void wav_header(FILE *f, uint32_t rate, uint32_t frames)
{
    uint32_t data = frames * 2, riff = 36 + data; uint16_t s;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    uint32_t v = 16; fwrite(&v, 4, 1, f); s = 1; fwrite(&s, 2, 1, f); s = 1; fwrite(&s, 2, 1, f);
    fwrite(&rate, 4, 1, f); v = rate * 2; fwrite(&v, 4, 1, f); s = 2; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: render song.cmf out.wav [rate] [--regs f] [--seconds N]\n"); return 2; }
    uint32_t rate = 49716; double max_sec = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--regs") && i + 1 < argc) regf = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) max_sec = atof(argv[++i]);
        else rate = (uint32_t)atoi(argv[i]);
    }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc(n); fread(blob, 1, n, f); fclose(f);

    OPL3_Reset(&chip, rate);
    macsop m;
    int r = macsop_load(&m, blob, (uint32_t)n, oplwrite, NULL);
    if (r) { fprintf(stderr, "load failed: %d\n", r); return 1; }
    fprintf(stderr, "%s: %u patterns, %u orders, speed %u (%.2f rows/s), rhythm %s, %u instruments\n", argv[1],
            m.n_patterns, m.n_orders, m.speed, macsop_rows_per_sec_q16(&m) / 65536.0, m.rhythm ? "on" : "off", m.n_instr);
    for (int i = 0; i < m.n_instr; i++) fprintf(stderr, "  %2d %s\n", i + 1, macsop_instr_name(&m, i));

    /* samples per row in 16.16 */
    uint64_t spr_q16 = ((uint64_t)rate << 32) / macsop_rows_per_sec_q16(&m);
    uint64_t acc = 0;
    FILE *w = fopen(argv[2], "wb"); wav_header(w, rate, 0);
    uint32_t frames = 0; int playing = 1; uint32_t tail = rate;   /* 1 s of release after the end */
    int16_t buf[2];
    clock_t t0 = clock();
    while (playing || tail) {
        if (acc < (1ull << 16)) {
            if (playing) playing = macsop_tick(&m);
            acc += spr_q16;
        }
        acc -= 1ull << 16;
        if (rate == 49716) OPL3_Generate(&chip, buf); else OPL3_GenerateResampled(&chip, buf);
        int16_t mono = (int16_t)((buf[0] + buf[1]) / 2);
        fwrite(&mono, 2, 1, w); frames++; sample_pos++;
        if (!playing) tail--;
        if (max_sec && frames > max_sec * rate) break;
    }
    double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
    fseek(w, 0, SEEK_SET); wav_header(w, rate, frames); fclose(w);
    if (regf) fclose(regf);
    fprintf(stderr, "%u frames = %.1f s audio, rendered in %.3f s (%.1fx realtime)\n", frames, (double)frames / rate, dt, frames / rate / dt);
    return 0;
}
