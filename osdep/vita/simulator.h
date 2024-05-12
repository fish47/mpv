#pragma once

#include "ui_context.h"

#include <stdbool.h>
#include <GLFW/glfw3.h>

struct emulator_platform_data {
    GLFWwindow *window;
    void *fontconfig;
    char *fallback_font;
    bool enable_dr;
};

struct emulator_platform_data *emulator_get_platform_data(struct ui_context* ctx);

void emulator_fontconfig_init(void *parent, void **fc);

bool emulator_fontconfig_select(void *fc, int codepoint, char **best_path, int *best_idx);
