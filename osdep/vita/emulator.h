#pragma once

#include "ui_context.h"

#include <stdbool.h>
#include <GLFW/glfw3.h>

struct emulator_platform_data {
    GLFWwindow *window;
    const char *font_path;
    bool enable_dr;
};

struct emulator_platform_data *emulator_get_platform_data(struct ui_context* ctx);
