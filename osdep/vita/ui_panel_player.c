#include "ui_context.h"
#include "ui_panel.h"
#include "ta/ta.h"
#include "libmpv/client.h"

struct priv_panel {
    mpv_handle *mpv;
    void *vo_data;
    struct ui_panel_player_vo_fns vo_fns;
};

void *ui_panel_player_get_vo_data(struct ui_context *ctx)
{
    struct priv_panel *priv = ui_panel_common_get_priv(ctx, &ui_panel_player);
    return priv ? priv->vo_data : NULL;
}

void ui_panel_player_set_vo_data(struct ui_context *ctx, void *data)
{
    struct priv_panel *priv = ui_panel_common_get_priv(ctx, &ui_panel_player);
    if (priv)
        priv->vo_data = ta_steal(priv, data);
}

void ui_panel_player_set_vo_fns(struct ui_context *ctx,
                                const struct ui_panel_player_vo_fns *fns)
{
    struct priv_panel *priv = ui_panel_common_get_priv(ctx, &ui_panel_player);
    if (priv)
        priv->vo_fns = *fns;
}

void ui_panel_player_send_quit(struct ui_context *ctx)
{
    const char *args[] = { "quit", NULL };
    struct priv_panel *priv = ctx->priv_panel;
    mpv_command(priv->mpv, args);
}

void ui_panel_player_send_toggle(struct ui_context *ctx)
{
    const char *args[] = { "cycle", "pause", NULL };
    struct priv_panel *priv = ctx->priv_panel;
    mpv_command(priv->mpv, args);
}

static void on_mpv_wakeup(void *p)
{
    ui_panel_common_wakeup(p);
}

static bool player_init(struct ui_context *ctx, void *p)
{
    struct priv_panel *priv = ctx->priv_panel;
    priv->mpv = mpv_create();
    if (!priv->mpv)
        return false;

    mpv_set_option(priv->mpv, "wid", MPV_FORMAT_INT64, &ctx);
    mpv_set_option_string(priv->mpv, "idle", "yes");
    mpv_set_option_string(priv->mpv, "keep-open", "yes");
    mpv_set_wakeup_callback(priv->mpv, on_mpv_wakeup, ctx);
    if (mpv_initialize(priv->mpv) != 0)
        return false;

    struct ui_panel_player_init_params *params = p;
    if (params) {
        const char *args[] = { "loadfile", params->path, NULL };
        mpv_command(priv->mpv, args);
    }
    return true;
}

static void player_uninit(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    if (priv->vo_fns.uninit)
        priv->vo_fns.uninit(ctx);
    if (priv->mpv)
        mpv_destroy(priv->mpv);
}

static void player_on_draw(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    if (priv->vo_fns.draw)
        priv->vo_fns.draw(ctx);
}

static void player_on_poll(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    if (!priv->mpv)
        return;

    while (true) {
        mpv_event *event = mpv_wait_event(priv->mpv, 0);
        if (event->event_id == MPV_EVENT_NONE) {
            break;
        } else if (event->event_id == MPV_EVENT_SHUTDOWN) {
            mpv_terminate_destroy(priv->mpv);
            priv->mpv = NULL;
            ui_panel_common_pop(ctx);
            break;
        }
    }
}

static void player_on_key(struct ui_context *ctx, struct ui_key *key)
{
    struct priv_panel *priv = ctx->priv_panel;
    if (priv->vo_fns.send_key)
        priv->vo_fns.send_key(ctx, key);
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
