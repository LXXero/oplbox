/* the firmware's dbopl2 */
#include <string.h>
#include "dbopl2.c"
static dbopl2 chip;
void new_init(void) { dbopl2_reset(&chip); }
void new_write(unsigned reg, unsigned val) { dbopl2_write(&chip, reg, (uint8_t)val); }
void new_gen(unsigned n, int32_t *out) { memset(out, 0, n * sizeof *out); dbopl2_generate(&chip, n, out); }
