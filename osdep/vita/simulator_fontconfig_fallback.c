#include "simulator.h"

void simulator_fontconfig_init(void *parent, void **fc)
{
    *fc = NULL;
}

bool simulator_fontconfig_select(void *fc, int codepoint, char **best_path, int *best_idx)
{
    return false;
}
