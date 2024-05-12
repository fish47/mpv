#include "emulator.h"

void emulator_fontconfig_init(void *parent, void **fc)
{
    *fc = NULL;
}

bool emulator_fontconfig_select(void *fc, int codepoint, char **best_path, int *best_idx)
{
    return false;
}
