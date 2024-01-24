#include "ui_context.h"
#include "ui_panel.h"
#include "player_perf.h"

#include "ta/ta.h"
#include "player/core.h"
#include "input/input.h"
#include "input/keycodes.h"

#include <pthread.h>
#include <stdatomic.h>

enum key_act {
    KEY_ACT_DROP,
    KEY_ACT_INPUT,
    KEY_ACT_SEND_QUIT,
    KEY_ACT_SEND_TOGGLE,
};

struct priv_panel {
    mpv_handle *mpv_handle;
    struct MPContext *mpv_ctx;
    struct player_perf_ctx *perf_ctx;

    void *vo_data;
    ui_panel_player_vo_draw_fn vo_draw_fn;

    bool destroy_signaled;
    atomic_bool destroy_done;
};

mpv_handle *mpv_create_vita(struct MPContext **p_mpctx);

void *ui_panel_player_get_vo_draw_data(struct ui_context *ctx)
{
    struct priv_panel *priv = ui_panel_common_get_priv(ctx, &ui_panel_player);
    return priv ? priv->vo_data : NULL;
}

void ui_panel_player_set_vo_draw_data(struct ui_context *ctx, void *data)
{
    struct priv_panel *priv = ui_panel_common_get_priv(ctx, &ui_panel_player);
    if (priv)
        priv->vo_data = ta_steal(priv, data);
}

void ui_panel_player_set_vo_draw_fn(struct ui_context *ctx, ui_panel_player_vo_draw_fn fn)
{
    struct priv_panel *priv = ui_panel_common_get_priv(ctx, &ui_panel_player);
    if (priv)
        priv->vo_draw_fn = fn;
}

static void on_mpv_wakeup(void *p)
{
    ui_panel_common_wakeup(p);
}

static bool player_init(struct ui_context *ctx, void *p)
{
    struct priv_panel *priv = ctx->priv_panel;
    priv->destroy_done = ATOMIC_VAR_INIT(false);
    priv->mpv_handle = mpv_create_vita(&priv->mpv_ctx);
    if (!priv->mpv_handle)
        return false;

    mpv_set_option(priv->mpv_handle, "wid", MPV_FORMAT_INT64, &ctx);
    mpv_set_option_string(priv->mpv_handle, "idle", "yes");
    mpv_set_option_string(priv->mpv_handle, "keep-open", "yes");
    mpv_set_wakeup_callback(priv->mpv_handle, on_mpv_wakeup, ctx);
    if (mpv_initialize(priv->mpv_handle) != 0)
        return false;

    struct ui_panel_player_init_params *params = p;
    if (params) {
        if (params->enable_perf)
            priv->perf_ctx = player_perf_create_ctx(priv);
        mpv_command(priv->mpv_handle, (const char*[]) {
            "loadfile", params->file_path, NULL
        });
    }
    return true;
}

static void player_uninit(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    if (priv->perf_ctx)
        player_perf_stop(priv->perf_ctx, ctx);
}

static void player_on_draw(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    if (priv->vo_draw_fn && priv->vo_data)
        priv->vo_draw_fn(ctx, priv->vo_data);
    if (priv->perf_ctx)
        player_perf_draw(priv->perf_ctx, ctx);
}

static void *do_destroy_mpv(void *args)
{
    void **pp = args;
    mpv_handle *mpv = pp[0];
    atomic_bool *done = pp[1];
    struct ui_context *ctx = pp[2];
    mpv_terminate_destroy(mpv);
    atomic_store(done, true);
    ui_panel_common_wakeup(ctx);
    return NULL;
}

static void wait_mpv_destruction_async(struct ui_context *ctx)
{
    // prevent mpv destruction from blocking current thread
    // because some async-posted uninitializations have to be done later
    pthread_t thread;
    struct priv_panel *priv = ctx->priv_panel;
    void **args = ta_new_array(priv, void*, 2);
    args[0] = priv->mpv_handle;
    args[1] = &priv->destroy_done;
    args[2] = ctx;
    pthread_create(&thread, NULL, do_destroy_mpv, args);
    priv->mpv_ctx = NULL;
    priv->mpv_handle = NULL;
    priv->destroy_signaled = true;
}

static void player_on_poll(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    if (priv->destroy_signaled) {
        if (atomic_load_explicit(&priv->destroy_done, memory_order_relaxed))
            ui_panel_common_pop(ctx);
        return;
    }

    if (!priv->mpv_handle)
        return;

    if (priv->perf_ctx)
        player_perf_poll(priv->perf_ctx, ctx, priv->mpv_ctx);

    while (true) {
        mpv_event *event = mpv_wait_event(priv->mpv_handle, 0);
        if (event->event_id == MPV_EVENT_NONE) {
            break;
        } else if (event->event_id == MPV_EVENT_SHUTDOWN) {
            wait_mpv_destruction_async(ctx);
            break;
        }
    }
}

static int resolve_mp_key_code(enum ui_key_code key, enum key_act *out_act)
{
    switch (key) {
    case UI_KEY_CODE_VITA_DPAD_LEFT:
        return MP_KEY_GAMEPAD_DPAD_LEFT;
    case UI_KEY_CODE_VITA_DPAD_RIGHT:
        return MP_KEY_GAMEPAD_DPAD_RIGHT;
    case UI_KEY_CODE_VITA_DPAD_UP:
        return MP_KEY_GAMEPAD_DPAD_UP;
    case UI_KEY_CODE_VITA_DPAD_DOWN:
        return MP_KEY_GAMEPAD_DPAD_DOWN;
    case UI_KEY_CODE_VITA_ACTION_SQUARE:
        return MP_KEY_GAMEPAD_ACTION_LEFT;
    case UI_KEY_CODE_VITA_ACTION_TRIANGLE:
        return MP_KEY_GAMEPAD_ACTION_UP;
    case UI_KEY_CODE_VITA_TRIGGER_L:
        return MP_KEY_GAMEPAD_LEFT_SHOULDER;
    case UI_KEY_CODE_VITA_TRIGGER_R:
        return MP_KEY_GAMEPAD_RIGHT_SHOULDER;
    case UI_KEY_CODE_VITA_START:
        return MP_KEY_GAMEPAD_START;
    case UI_KEY_CODE_VITA_SELECT:
        return MP_KEY_GAMEPAD_MENU;

    case UI_KEY_CODE_VITA_ACTION_CIRCLE:
    case UI_KEY_CODE_VITA_ACTION_CROSS:
        // ignore associated key bindings
        break;

    case UI_KEY_CODE_VITA_VIRTUAL_OK:
        *out_act = KEY_ACT_SEND_TOGGLE;
        return 0;
    case UI_KEY_CODE_VITA_VIRTUAL_CANCEL:
        *out_act = KEY_ACT_SEND_QUIT;
        return 0;
    }

    *out_act = KEY_ACT_DROP;
    return 0;
}

static int resolve_mp_input_key(uint32_t code, enum ui_key_state state)
{
    switch (state) {
    case UI_KEY_STATE_DOWN:
        return code | MP_KEY_STATE_DOWN;
    case UI_KEY_STATE_UP:
        return code | MP_KEY_STATE_UP;
    }

    // it is unlikely to reach here
    return MP_KEY_UNMAPPED;
}

static void player_on_key(struct ui_context *ctx, struct ui_key *key)
{
    struct priv_panel *priv = ctx->priv_panel;
    if (!priv->mpv_handle)
        return;

    enum key_act act = KEY_ACT_INPUT;
    int code = resolve_mp_key_code(key->code, &act);
    switch (act) {
    case KEY_ACT_DROP:
        break;
    case KEY_ACT_INPUT:
        mp_input_put_key(priv->mpv_ctx->input, resolve_mp_input_key(code, key->state));
        break;
    case KEY_ACT_SEND_QUIT:
        if (key->state == UI_KEY_STATE_DOWN)
            mpv_command_async(priv->mpv_handle, 0, (const char*[]) { "quit", NULL });
        break;
    case KEY_ACT_SEND_TOGGLE:
        if (key->state == UI_KEY_STATE_DOWN)
            mpv_command_async(priv->mpv_handle, 0, (const char*[]) { "cycle", "pause", NULL });
        break;
    }
}

const struct ui_panel ui_panel_player = {
    .priv_size = sizeof(struct priv_panel),
    .init = player_init,
    .uninit = player_uninit,
    .on_show = NULL,
    .on_hide = NULL,
    .on_draw = player_on_draw,
    .on_poll = player_on_poll,
    .on_key = player_on_key,
};
