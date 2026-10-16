/* the unmodified DOSBox dbopl (Chocolate Doom's C port, percussion abort() -> break as in DOSBox) */
#include "../build/dbopl_orig.c"
static Chip chip;
void orig_init(void) { DBOPL_InitTables(); memset(&chip, 0, sizeof chip); Chip__Chip(&chip); Chip__Setup(&chip, 49716); }
void orig_write(unsigned reg, unsigned val) { Chip__WriteReg(&chip, reg, (Bit8u)val); }
void orig_gen(unsigned n, int32_t *out) { Chip__GenerateBlock2(&chip, n, out); }
