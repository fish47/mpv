#include "emulator.h"
#include "ui_driver.h"
#include "ui_device.h"
#include "ui_panel.h"

struct key_map_item {
    int glfw_key_code;
    enum ui_key_code ui_key_code;
};

static const struct key_map_item platform_key_map[] = {
    { GLFW_KEY_S, UI_KEY_CODE_VITA_DPAD_LEFT },
    { GLFW_KEY_F, UI_KEY_CODE_VITA_DPAD_RIGHT },
    { GLFW_KEY_E, UI_KEY_CODE_VITA_DPAD_UP },
    { GLFW_KEY_D, UI_KEY_CODE_VITA_DPAD_DOWN },
    { GLFW_KEY_J, UI_KEY_CODE_VITA_ACTION_SQUARE },
    { GLFW_KEY_L, UI_KEY_CODE_VITA_ACTION_CIRCLE },
    { GLFW_KEY_I, UI_KEY_CODE_VITA_ACTION_TRIANGLE },
    { GLFW_KEY_K, UI_KEY_CODE_VITA_ACTION_CROSS },
    { GLFW_KEY_W, UI_KEY_CODE_VITA_L1 },
    { GLFW_KEY_O, UI_KEY_CODE_VITA_R1 },
    { GLFW_KEY_N, UI_KEY_CODE_VITA_START },
    { GLFW_KEY_M, UI_KEY_CODE_VITA_SELECT },
};

struct priv_platform {
    GLFWwindow *window;
};

static struct priv_platform *get_priv_platform(struct ui_context *ctx)
{
    return (struct priv_platform*) ctx->priv_platform;
}

static void get_glfw_centered_window_pos(GLFWwindow *win, int *out_x, int *out_y)
{
    int monitor_count = 0;
    GLFWmonitor **monitors = glfwGetMonitors(&monitor_count);
    if (monitor_count <= 0)
        return;

    int monitor_x = 0;
    int monitor_y = 0;
    glfwGetMonitorPos(monitors[0], &monitor_x, &monitor_y);

    int win_w = 0;
    int win_h = 0;
    glfwGetWindowSize(win, &win_w, &win_h);

    const GLFWvidmode *mode = glfwGetVideoMode(monitors[0]);
    *out_x = monitor_x + (mode->width - win_w) / 2;
    *out_y = monitor_y + (mode->height - win_h) / 2;
}

static void on_window_close(GLFWwindow *window)
{
    void *ctx = glfwGetWindowUserPointer(window);
    ui_panel_common_pop_all(ctx);
}

static bool platform_init(struct ui_context *ctx)
{
    if (!glfwInit())
        return false;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    GLFWwindow *window = glfwCreateWindow(VITA_SCREEN_W, VITA_SCREEN_H, "Vita", NULL, NULL);
    if (!window)
        return false;

    int win_pos_x = 0;
    int win_pos_y = 0;
    get_glfw_centered_window_pos(window, &win_pos_x, &win_pos_y);

    glfwDefaultWindowHints();
    glfwSetWindowPos(window, win_pos_x, win_pos_y);
    glfwShowWindow(window);
    glfwSwapInterval(0);
    glfwMakeContextCurrent(window);
    glfwSetWindowUserPointer(window, ctx);
    glfwSetWindowCloseCallback(window, on_window_close);

    struct priv_platform *priv = get_priv_platform(ctx);
    priv->window = window;
    return true;
}

static void platform_uninit(struct ui_context *ctx)
{
    struct priv_platform *priv = get_priv_platform(ctx);
    if (priv->window)
        glfwDestroyWindow(priv->window);
    glfwTerminate();
}

static void platform_poll_events(struct ui_context *ctx)
{
    glfwPollEvents();
}

static uint32_t platform_poll_keys(struct ui_context *ctx)
{
    uint32_t bits = 0;
    struct priv_platform *priv = get_priv_platform(ctx);
    for (int i = 0; i < MP_ARRAY_SIZE(platform_key_map); ++i) {
        const struct key_map_item *item = &platform_key_map[i];
        int state = glfwGetKey(priv->window, item->glfw_key_code);
        if (state == GLFW_PRESS)
            bits |= (1 << item->ui_key_code);
    }
    return bits;
}

const struct ui_platform_driver ui_platform_driver_vita = {
    .priv_size = sizeof(struct priv_platform),
    .init = platform_init,
    .uninit = platform_uninit,
    .exit = NULL,
    .poll_events = platform_poll_events,
    .poll_keys = platform_poll_keys,
};

GLFWwindow *emulator_get_window(struct ui_context *ctx)
{
    return get_priv_platform(ctx)->window;
}

