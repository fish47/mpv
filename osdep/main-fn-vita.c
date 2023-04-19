#include <pthread.h>

#include "common/common.h"
#include "osdep/timer.h"
#include "osdep/vita/ui_context.h"
#include "osdep/vita/ui_device.h"
#include "osdep/vita/ui_driver.h"
#include "osdep/vita/ui_panel.h"
#include "ta/ta_talloc.h"

#define FRAME_INTERVAL_US (1 * 1000 * 1000 / 60)

struct ui_panel_item {
    void *data;
    const struct ui_panel *panel;
};

struct ui_context_internal {
    bool need_wakeup;
    pthread_mutex_t lock;
    pthread_cond_t wakeup;

    int panel_count;
    struct ui_panel_item *panel_stack;
    const struct ui_panel *top_panel;

    bool want_redraw;
    int64_t frame_start;
    uint32_t key_bits;
};

static void do_wait_next_frame(struct ui_context_internal *priv)
{
    // handle pending wakeup request
    if (priv->need_wakeup)
        return;

    int64_t frame_next = priv->frame_start + FRAME_INTERVAL_US;
    int64_t wait_time = MPMAX(frame_next - mp_time_us(), 0);
    struct timespec ts = mp_time_us_to_timespec(wait_time);
    pthread_cond_timedwait(&priv->wakeup, &priv->lock, &ts);
}


static void wait_next_frame(struct ui_context *ctx)
{
    struct ui_context_internal *priv = ctx->priv_context;
    pthread_mutex_lock(&priv->lock);
    do_wait_next_frame(priv);
    priv->need_wakeup = false;
    pthread_mutex_unlock(&priv->lock);
}

static bool advnace_frame_time(struct ui_context *ctx)
{
    struct ui_context_internal *priv = ctx->priv_context;
    int frame_count = (mp_time_us() - priv->frame_start) / FRAME_INTERVAL_US;
    if (frame_count > 0) {
        priv->frame_start += frame_count * FRAME_INTERVAL_US;
        return true;
    }
    return false;
}

static const struct ui_panel *get_top_panel(struct ui_context *ctx)
{
    struct ui_context_internal *priv = ctx->priv_context;
    return priv->top_panel;
}

static void handle_platform_keys(struct ui_context *ctx)
{
    struct ui_context_internal *priv = ctx->priv_context;
    uint32_t new_bits = ui_platform_driver_vita.poll_keys(ctx);
    uint32_t changed_mask = new_bits ^ priv->key_bits;
    if (!changed_mask)
        return;

    for (int i = 0; i < UI_KEY_CODE_VITA_END; ++i) {
        uint32_t key_bit = 1 << i;
        if (key_bit & changed_mask) {
            bool pressed = key_bit & new_bits;
            enum ui_key_state state = pressed ? UI_KEY_STATE_DOWN : UI_KEY_STATE_UP;
            const struct ui_panel *panel = get_top_panel(ctx);
            if (panel) {
                struct ui_key key = { .code = i, .state = state };
                panel->on_key(ctx, &key);
            }
        }
    }

    priv->key_bits = new_bits;
}

static void handle_platform_events(struct ui_context *ctx)
{
    if (ui_platform_driver_vita.poll_events)
        ui_platform_driver_vita.poll_events(ctx);
}

static void on_dispatch_wakeup(void *p)
{
    ui_panel_common_wakeup(p);
}

static void ui_context_destroy(void *p)
{
    struct ui_context *ctx = p;
    struct ui_context_internal *priv = ctx->priv_context;
    pthread_mutex_destroy(&priv->lock);
    pthread_cond_destroy(&priv->wakeup);
    if (ctx->priv_render) {
        ui_render_driver_vita.uninit(ctx);
        TA_FREEP(&ctx->priv_render);
    }
    if (ctx->priv_platform) {
        ui_platform_driver_vita.uninit(ctx);
        TA_FREEP(&ctx->priv_platform);
    }
}

static struct ui_context *ui_context_new(int argc, char *argv[])
{
    struct ui_context *ctx = talloc_zero_size(NULL, sizeof(struct ui_context));
    talloc_set_destructor(ctx, ui_context_destroy);
    ctx->dispatch = mp_dispatch_create(ctx);
    mp_dispatch_set_wakeup_fn(ctx->dispatch, on_dispatch_wakeup, ctx);

    struct ui_context_internal *priv = talloc_zero_size(ctx, sizeof(struct ui_context_internal));
    ctx->priv_context = priv;
    pthread_mutex_init(&priv->lock, NULL);
    pthread_cond_init(&priv->wakeup, NULL);

    ctx->priv_platform = talloc_zero_size(ctx, ui_platform_driver_vita.priv_size);
    if (!ui_platform_driver_vita.init(ctx, argc, argv))
        goto error;

    ctx->priv_render = talloc_zero_size(ctx, ui_render_driver_vita.priv_size);
    if (!ui_render_driver_vita.init(ctx))
        goto error;

    return ctx;

error:
    talloc_free(ctx);
    return NULL;
}

static void handle_panel_events(struct ui_context *ctx)
{
    const struct ui_panel *panel = get_top_panel(ctx);
    if (!panel)
        return;

    if (panel->on_poll)
        panel->on_poll(ctx);
}

static bool has_panel(struct ui_context *ctx, const struct ui_panel *panel)
{
    if (get_top_panel(ctx) == panel)
        return true;

    struct ui_context_internal *priv = ctx->priv_context;
    for (int i = 0; i < priv->panel_count; ++i)
        if (priv->panel_stack[i].panel == panel)
            return true;
    return false;
}

static void do_push_panel(struct ui_context *ctx, const struct ui_panel *panel, void *data)
{
    // ignore duplicated panel
    if (has_panel(ctx, panel))
        return;

    // hide current panel
    struct ui_context_internal *priv = ctx->priv_context;
    if (priv->top_panel) {
        struct ui_panel_item save_item = {
            .data = ctx->priv_panel,
            .panel = priv->top_panel,
        };
        MP_TARRAY_APPEND(ctx, priv->panel_stack, priv->panel_count, save_item);
        if (priv->top_panel->on_hide)
            priv->top_panel->on_hide(ctx);
    }

    // show new panel
    priv->top_panel = panel;
    ctx->priv_panel = talloc_zero_size(ctx, panel->priv_size);
    priv->top_panel->init(ctx, data);
    if (priv->top_panel->on_show)
        priv->top_panel->on_show(ctx);
}

static void do_pop_panel(struct ui_context *ctx)
{
    struct ui_context_internal *priv = ctx->priv_context;
    if (!priv->top_panel)
        return;

    if (priv->top_panel->uninit)
        priv->top_panel->uninit(ctx);
    priv->top_panel = NULL;
    TA_FREEP(&ctx->priv_panel);

    struct ui_panel_item item;
    bool has_panel = MP_TARRAY_POP(priv->panel_stack, priv->panel_count, &item);
    if (has_panel) {
        ctx->priv_panel = item.data;
        priv->top_panel = item.panel;
        if (priv->top_panel->on_show)
            priv->top_panel->on_show(ctx);
    }
}

static void handle_redraw(struct ui_context *ctx)
{
    struct ui_context_internal *priv = ctx->priv_context;
    if (!priv->want_redraw)
        return;

    priv->want_redraw = false;
    ui_render_driver_vita.render_start(ctx);
    const struct ui_panel *panel = get_top_panel(ctx);
    if (panel)
        panel->on_draw(ctx);
    ui_render_driver_vita.render_end(ctx);
}

static void main_loop(struct ui_context *ctx)
{
    if (!ctx)
        return;

    // message loop relys on timestamp service
    mp_time_init();

    ui_panel_common_push(ctx, &ui_panel_files, NULL);
    while (true) {
        // poll and run pending async jobs
        handle_panel_events(ctx);
        mp_dispatch_queue_process(ctx->dispatch, 0);

        if (advnace_frame_time(ctx)) {
            handle_platform_keys(ctx);
            handle_platform_events(ctx);
            handle_redraw(ctx);
        }

        if (!get_top_panel(ctx))
            break;

        // sleep until next frame or interrupt to avoid CPU stress
        wait_next_frame(ctx);
    }
}

int main(int argc, char *argv[])
{
    struct ui_context *ctx = ui_context_new(argc, argv);
    main_loop(ctx);
    TA_FREEP(&ctx);
    if (ui_platform_driver_vita.exit)
        ui_platform_driver_vita.exit();
    return 0;
}

void *ui_panel_common_get_priv(struct ui_context *ctx, const struct ui_panel *panel)
{
    const struct ui_panel *top = get_top_panel(ctx);
    return (top && top == panel) ? ctx->priv_panel : NULL;
}

void ui_panel_common_wakeup(struct ui_context *ctx)
{
    struct ui_context_internal *priv = ctx->priv_context;
    pthread_mutex_lock(&priv->lock);
    if (!priv->need_wakeup) {
        priv->need_wakeup = true;
        pthread_cond_signal(&priv->wakeup);
    }
    pthread_mutex_unlock(&priv->lock);
}

void ui_panel_common_invalidate(struct ui_context *ctx)
{
    struct ui_context_internal *priv = ctx->priv_context;
    priv->want_redraw = true;
}

void ui_panel_common_push(struct ui_context *ctx, const struct ui_panel *panel, void *data)
{
    ui_panel_common_invalidate(ctx);
    do_push_panel(ctx, panel, data);
    ta_free(data);
}

void ui_panel_common_pop(struct ui_context *ctx)
{
    ui_panel_common_invalidate(ctx);
    do_pop_panel(ctx);
}

void ui_panel_common_pop_all(struct ui_context *ctx)
{
    ui_panel_common_invalidate(ctx);
    while (get_top_panel(ctx))
        do_pop_panel(ctx);
}

int64_t ui_panel_common_get_frame_time(struct ui_context *ctx)
{
    struct ui_context_internal *priv = ctx->priv_context;
    return priv->frame_start;
}
