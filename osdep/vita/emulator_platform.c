#include "ui_driver.h"
#include "ui_device.h"
#include "ui_panel.h"
#include "emulator.h"
#include "ta/ta.h"

#include <stdint.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

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

typedef struct key_map_item key_map_item_ext[2];

static const key_map_item_ext platform_key_map_virtual_asia = {
    { GLFW_KEY_L, UI_KEY_CODE_VITA_VIRTUAL_OK },
    { GLFW_KEY_K, UI_KEY_CODE_VITA_VIRTUAL_CANCEL },
};

static const key_map_item_ext platform_key_map_virtual_swap = {
    { GLFW_KEY_K, UI_KEY_CODE_VITA_VIRTUAL_OK },
    { GLFW_KEY_L, UI_KEY_CODE_VITA_VIRTUAL_CANCEL },
};

struct priv_platform {
    GLFWwindow *window;
    char *files_dir;
    char *font_path;
    const key_map_item_ext *key_map_ext;
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

static uint64_t get_path_stat_type(const char *path)
{
    struct stat path_stat;
    if (stat(path, &path_stat) != 0)
        return 0;
    return path_stat.st_mode;
}

static bool platform_init(struct ui_context *ctx, int argc, char *argv[])
{
    if (!glfwInit())
        return false;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    GLFWwindow *window = glfwCreateWindow(VITA_SCREEN_W, VITA_SCREEN_H,
                                          "Vita", NULL, NULL);
    if (!window)
        return false;

    int win_pos_x = 0;
    int win_pos_y = 0;
    get_glfw_centered_window_pos(window, &win_pos_x, &win_pos_y);

    glfwDefaultWindowHints();
    glfwSetWindowPos(window, win_pos_x, win_pos_y);
    glfwShowWindow(window);
    glfwMakeContextCurrent(window);
    glfwSetWindowUserPointer(window, ctx);
    glfwSetWindowCloseCallback(window, on_window_close);
    glfwSwapInterval(0);

    struct priv_platform *priv = get_priv_platform(ctx);
    priv->window = window;
    priv->key_map_ext = &platform_key_map_virtual_asia;

    int opt = 0;
    char buf[PATH_MAX] = {0};
    char *normalized = NULL;
    while ((opt = getopt(argc, argv, "sf:d:")) != -1) {
        switch (opt) {
        case 'f':
            normalized = realpath(optarg, buf);
            if (S_ISREG(get_path_stat_type(normalized)))
                priv->font_path = ta_strdup(priv, normalized);
            break;
        case 'd':
            normalized = realpath(optarg, buf);
            if (S_ISDIR(get_path_stat_type(normalized)))
                priv->files_dir = ta_strdup(priv, normalized);
            break;
        case 's':
            priv->key_map_ext = &platform_key_map_virtual_swap;
            break;
        }
    }

    if (priv->font_path && priv->files_dir)
        return true;

    printf("Usage: [-s] -f FONT_PATH -d OPEN_DIR\n");
    return false;
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

static void do_poll_keys(GLFWwindow *win, uint32_t *bits,
                         const struct key_map_item *map, int count)
{
    for (int i = 0; i < count; ++i) {
        int state = glfwGetKey(win, map[i].glfw_key_code);
        if (state == GLFW_PRESS)
            *bits |= (1 << map[i].ui_key_code);
    }
}

static uint32_t platform_poll_keys(struct ui_context *ctx)
{
    uint32_t bits = 0;
    struct priv_platform *priv = get_priv_platform(ctx);
    do_poll_keys(priv->window, &bits,
                 platform_key_map, MP_ARRAY_SIZE(platform_key_map));
    do_poll_keys(priv->window, &bits,
                 *priv->key_map_ext, MP_ARRAY_SIZE(*priv->key_map_ext));
    return bits;
}

static const char *platform_get_font_path(struct ui_context *ctx)
{
    return get_priv_platform(ctx)->font_path;
}

static const char *platform_get_files_dir(struct ui_context *ctx)
{
    return get_priv_platform(ctx)->files_dir;
}

const struct ui_platform_driver ui_platform_driver_vita = {
    .priv_size = sizeof(struct priv_platform),
    .init = platform_init,
    .uninit = platform_uninit,
    .exit = NULL,
    .poll_events = platform_poll_events,
    .poll_keys = platform_poll_keys,
    .get_font_path = platform_get_font_path,
    .get_files_dir = platform_get_files_dir,
};

GLFWwindow *emulator_get_window(struct ui_context *ctx)
{
    return get_priv_platform(ctx)->window;
}

