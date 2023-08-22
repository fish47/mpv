#pragma once

#include "ui_context.h"

#include <GLFW/glfw3.h>

GLFWwindow *emulator_get_window(struct ui_context* ctx);
const char *emulator_get_font_path(struct ui_context* ctx);
