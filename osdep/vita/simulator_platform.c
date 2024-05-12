#include "ui_driver.h"
#include "ui_device.h"
#include "ui_panel.h"
#include "simulator.h"
#include "ta/ta.h"

#include <stdlib.h>
#include <getopt.h>
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
    { GLFW_KEY_W, UI_KEY_CODE_VITA_TRIGGER_L },
    { GLFW_KEY_O, UI_KEY_CODE_VITA_TRIGGER_R },
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

enum cmd_option_type {
    CMD_OPTION_TYPE_BOOL,
    CMD_OPTION_TYPE_FILE,
    CMD_OPTION_TYPE_DIR,
};

struct cmd_option {
    const char *name;
    enum cmd_option_type type;
    bool required;
    void *dest;
};

struct priv_platform {
    struct simulator_platform_data platform_data;
    const char *files_dir;

    const key_map_item_ext *key_map_ext;
    uint32_t key_pressed_bits;
};

static struct priv_platform *get_priv_platform(struct ui_context *ctx)
{
    return (struct priv_platform*) ctx->priv_platform;
}

static void resolve_changed_key(uint32_t *bits, int key, int act,
                                const struct key_map_item *map, int n)
{
    for (int i = 0; i < n; ++i) {
        const struct key_map_item *item = &map[i];
        if (item->glfw_key_code == key) {
            if (act == GLFW_PRESS)
                *bits |= item->ui_key_code;
            else if (act == GLFW_RELEASE)
                *bits &= ~item->ui_key_code;
            break;
        }
    }
}

static void on_key_changed(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    struct ui_context *ctx = glfwGetWindowUserPointer(window);
    struct priv_platform *priv = get_priv_platform(ctx);
    resolve_changed_key(&priv->key_pressed_bits, key, action,
                        platform_key_map, MP_ARRAY_SIZE(platform_key_map));
    resolve_changed_key(&priv->key_pressed_bits, key, action,
                        *priv->key_map_ext, MP_ARRAY_SIZE(*priv->key_map_ext));
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

static bool do_set_param_path(void *parent, char **dst, const char *src, mode_t type)
{
    bool result = false;
    char *normalized = realpath(src, NULL);
    if (!normalized)
        goto done;

    struct stat path_stat;
    if (stat(normalized, &path_stat) != 0)
        goto done;

    if ((path_stat.st_mode & S_IFMT) != type)
        goto done;

    result = true;
    TA_FREEP(dst);
    *dst = ta_strdup(parent, src);

done:
    if (normalized)
        free(normalized);
    return result;
}

static void print_usage(const struct cmd_option *options, int n)
{
    const char *line_fmt = "%-16s%-10s%s\n";
    printf(line_fmt, "[Parameter]", "[Type]", "[Required]");

    for (int i = 0; i < n; ++i) {
        const struct cmd_option *cmd = &options[i];
        const char *required = cmd->required ? "yes" : "no";
        const char *type = "?";
        switch (cmd->type) {
        case CMD_OPTION_TYPE_BOOL:
            type = "bool";
            break;
        case CMD_OPTION_TYPE_FILE:
            type = "file";
            break;
        case CMD_OPTION_TYPE_DIR:
            type = "dir";
            break;
        }

        char cmd_name[50];
        sprintf(cmd_name, "--%s", cmd->name);
        printf(line_fmt, cmd_name, type, required);
    }
}

static bool parse_options(struct priv_platform *priv, int argc, char *argv[])
{
    bool swap_ok = false;
    int cmd_count = 3;
    struct cmd_option cmd_options[4] = {
        { "swap-ok", CMD_OPTION_TYPE_BOOL, false, &swap_ok },
        { "enable-dr", CMD_OPTION_TYPE_BOOL, false, &priv->platform_data.enable_dr },
        { "files-dir", CMD_OPTION_TYPE_DIR, true, &priv->files_dir },
    };

    if (!simulator_fontconfig_select(priv->platform_data.fontconfig, 0, NULL, NULL)) {
        cmd_options[cmd_count++] = (struct cmd_option) {
            "font-path", CMD_OPTION_TYPE_FILE, true, &priv->platform_data.fallback_font
        };
    }

    int missed_opts = 0;
    struct option opt_options[5] = {0};
    for (int i = 0; i < cmd_count; ++i) {
        struct option *opt = &opt_options[i];
        const struct cmd_option *cmd = &cmd_options[i];
        opt->name = cmd->name;
        opt->has_arg = (cmd->type != CMD_OPTION_TYPE_BOOL);
        if (cmd->required)
            missed_opts |= (1 << i);
    }

    int opterr_bak = opterr;
    opterr = 0;
    while (true) {
        int opt = -1;
        if (getopt_long(argc, argv, "", opt_options, &opt) == -1)
            break;

        // ignore any unknown parameters
        if (opt < 0)
            continue;

        bool valid = false;
        const struct cmd_option *cmd = &cmd_options[opt];
        switch (cmd->type) {
        case CMD_OPTION_TYPE_BOOL:
            valid = true;
            *((bool*) cmd->dest) = true;
            break;
        case CMD_OPTION_TYPE_DIR:
            valid = do_set_param_path(priv, cmd->dest, optarg, S_IFDIR);
            break;
        case CMD_OPTION_TYPE_FILE:
            valid = do_set_param_path(priv, cmd->dest, optarg, S_IFREG);
            break;
        }
        if (valid && cmd->required)
            missed_opts &= ~(1 << opt);
    }
    opterr = opterr_bak;

    if (missed_opts)
        goto fail;

    priv->key_map_ext = &platform_key_map_virtual_asia;
    if (swap_ok)
        priv->key_map_ext = &platform_key_map_virtual_swap;
    return true;

fail:
    print_usage(cmd_options, MP_ARRAY_SIZE(cmd_options));
    return false;
}

static bool init_glfw_window(struct ui_context *ctx, GLFWwindow **result)
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
    glfwSetKeyCallback(window, on_key_changed);
    glfwSetWindowCloseCallback(window, on_window_close);
    glfwSwapInterval(0);

    *result = window;
    return true;
}

static bool platform_init(struct ui_context *ctx, int argc, char *argv[])
{
    struct priv_platform *priv = get_priv_platform(ctx);
    priv->key_map_ext = &platform_key_map_virtual_asia;

    if (!init_glfw_window(ctx, &priv->platform_data.window))
        return false;

    bool need_fallback = false;
    simulator_fontconfig_init(priv, &priv->platform_data.fontconfig);
    if (!parse_options(priv, argc, argv))
        return false;

    return true;
}

static void platform_uninit(struct ui_context *ctx)
{
    struct priv_platform *priv = get_priv_platform(ctx);
    if (priv->platform_data.window)
        glfwDestroyWindow(priv->platform_data.window);
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
            *bits |= map[i].ui_key_code;
    }
}

static uint32_t platform_poll_keys(struct ui_context *ctx)
{
    struct priv_platform *priv = get_priv_platform(ctx);
    return priv->key_pressed_bits;
}

static const char *platform_get_files_dir(struct ui_context *ctx)
{
    return get_priv_platform(ctx)->files_dir;
}

static int platform_get_battery_level(struct ui_context *ctx)
{
    return 80;
}

const struct ui_platform_driver ui_platform_driver_vita = {
    .priv_size = sizeof(struct priv_platform),
    .init = platform_init,
    .uninit = platform_uninit,
    .exit = NULL,
    .poll_events = platform_poll_events,
    .poll_keys = platform_poll_keys,
    .get_files_dir = platform_get_files_dir,
    .get_battery_level = platform_get_battery_level,
};

struct simulator_platform_data *simulator_get_platform_data(struct ui_context *ctx)
{
    return &get_priv_platform(ctx)->platform_data;
}


