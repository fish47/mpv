#include "ui_driver.h"
#include "ui_device.h"
#include "ui_panel.h"
#include "emulator.h"
#include "ta/ta.h"

#include <getopt.h>
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
    struct emulator_platform_data platform_data;
    const key_map_item_ext *key_map_ext;
    const char *files_dir;
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

static bool do_set_param_path(void *parent, char **dst, const char *src, mode_t type)
{
    char buf[PATH_MAX];
    char *normalized = realpath(src, buf);
    if (!normalized)
        return false;

    struct stat path_stat;
    if (stat(normalized, &path_stat) != 0)
        return false;

    if ((path_stat.st_mode & S_IFMT) != type)
        return false;

    TA_FREEP(dst);
    *dst = ta_strdup(parent, src);
    return true;
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
    const struct cmd_option cmd_options[] = {
        { "swap-ok", CMD_OPTION_TYPE_BOOL, false, &swap_ok },
        { "enable-dr", CMD_OPTION_TYPE_BOOL, false, &priv->platform_data.enable_dr },
        { "font-path", CMD_OPTION_TYPE_FILE, true, &priv->platform_data.font_path },
        { "files-dir", CMD_OPTION_TYPE_DIR, true, &priv->files_dir },
    };

    int missed_opts = 0;
    struct option opt_options[MP_ARRAY_SIZE(cmd_options) + 1] = {0};
    for (int i = 0; i < MP_ARRAY_SIZE(cmd_options); ++i) {
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

static bool platform_init(struct ui_context *ctx, int argc, char *argv[])
{
    struct priv_platform *priv = get_priv_platform(ctx);
    priv->key_map_ext = &platform_key_map_virtual_asia;

    if (!parse_options(priv, argc, argv))
        return false;

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

    priv->platform_data.window = window;
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
            *bits |= (1 << map[i].ui_key_code);
    }
}

static uint32_t platform_poll_keys(struct ui_context *ctx)
{
    uint32_t bits = 0;
    struct priv_platform *priv = get_priv_platform(ctx);
    do_poll_keys(priv->platform_data.window, &bits,
                 platform_key_map, MP_ARRAY_SIZE(platform_key_map));
    do_poll_keys(priv->platform_data.window, &bits,
                 *priv->key_map_ext, MP_ARRAY_SIZE(*priv->key_map_ext));
    return bits;
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
    .get_files_dir = platform_get_files_dir,
};

struct emulator_platform_data *emulator_get_platform_data(struct ui_context *ctx)
{
    return &get_priv_platform(ctx)->platform_data;
}


