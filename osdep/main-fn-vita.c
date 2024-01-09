#include <pthread.h>

#include "ta/ta_talloc.h"
#include "common/common.h"
#include "misc/dispatch.h"
#include "osdep/timer.h"
#include "osdep/vita/ui_context.h"
#include "osdep/vita/ui_device.h"
#include "osdep/vita/ui_driver.h"
#include "osdep/vita/ui_panel.h"

#define FRAME_INTERVAL_US (1 * 1000 * 1000 / 60)

struct ui_panel_item {
    void *data;
    const struct ui_panel *panel;
};

struct ui_context_internal {
    bool need_wakeup;
    pthread_mutex_t lock;
    pthread_cond_t wakeup;
    struct mp_dispatch_queue *dispatch;

    int panel_count;
    struct ui_panel_item *panel_stack;
    const struct ui_panel *panel_top;

    bool font_init;
    struct ui_font *font_impl;

    bool want_redraw;
    int64_t frame_start;
    uint32_t key_bits;
};

static void do_wait_next_frame(struct ui_context_internal *in)
{
    // handle pending wakeup request
    if (in->need_wakeup)
        return;

    int64_t frame_next = in->frame_start + FRAME_INTERVAL_US;
    int64_t wait_time = MPMAX(frame_next - mp_time_us(), 0);
    struct timespec ts = mp_time_us_to_timespec(wait_time);
    pthread_cond_timedwait(&in->wakeup, &in->lock, &ts);
}


static void wait_next_frame(struct ui_context *ctx)
{
    struct ui_context_internal *in = ctx->priv_context;
    pthread_mutex_lock(&in->lock);
    do_wait_next_frame(in);
    in->need_wakeup = false;
    pthread_mutex_unlock(&in->lock);
}

static bool advnace_frame_time(struct ui_context *ctx)
{
    struct ui_context_internal *in = ctx->priv_context;
    int frame_count = (mp_time_us() - in->frame_start) / FRAME_INTERVAL_US;
    if (frame_count > 0) {
        in->frame_start += frame_count * FRAME_INTERVAL_US;
        return true;
    }
    return false;
}

static const struct ui_panel *get_top_panel(struct ui_context *ctx)
{
    struct ui_context_internal *in = ctx->priv_context;
    return in->panel_top;
}

static void handle_platform_keys(struct ui_context *ctx)
{
    struct ui_context_internal *in = ctx->priv_context;
    uint32_t new_bits = ui_platform_driver_vita.poll_keys(ctx);
    uint32_t changed_mask = new_bits ^ in->key_bits;
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

    in->key_bits = new_bits;
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

static void destroy_ui_context(void *p)
{
    struct ui_context *ctx = p;
    struct ui_context_internal *in = ctx->priv_context;
    pthread_mutex_destroy(&in->lock);
    pthread_cond_destroy(&in->wakeup);

    if (in->font_impl)
        ui_render_driver_vita.font_uninit(ctx, &in->font_impl);
    if (ctx->priv_render)
        ui_render_driver_vita.uninit(ctx);
    if (ctx->priv_platform)
        ui_platform_driver_vita.uninit(ctx);
}

static void *do_new_context_internal(void *ctx)
{
    struct ui_context_internal *in = talloc_zero_size(ctx, sizeof(struct ui_context_internal));
    in->dispatch = mp_dispatch_create(in);
    mp_dispatch_set_wakeup_fn(in->dispatch, on_dispatch_wakeup, ctx);
    pthread_mutex_init(&in->lock, NULL);
    pthread_cond_init(&in->wakeup, NULL);
    return in;
}

static struct ui_context *ui_context_new(int argc, char *argv[])
{
    struct ui_context *ctx = talloc_zero_size(NULL, sizeof(struct ui_context));
    talloc_set_destructor(ctx, destroy_ui_context);

    ctx->priv_context = do_new_context_internal(ctx);

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

    struct ui_context_internal *in = ctx->priv_context;
    for (int i = 0; i < in->panel_count; ++i)
        if (in->panel_stack[i].panel == panel)
            return true;
    return false;
}

static void do_push_panel(struct ui_context *ctx, const struct ui_panel *panel, void *data)
{
    // ignore duplicated panel
    if (has_panel(ctx, panel))
        return;

    // hide current panel
    struct ui_context_internal *in = ctx->priv_context;
    if (in->panel_top) {
        struct ui_panel_item save_item = {
            .data = ctx->priv_panel,
            .panel = in->panel_top,
        };
        MP_TARRAY_APPEND(ctx, in->panel_stack, in->panel_count, save_item);
        if (in->panel_top->on_hide)
            in->panel_top->on_hide(ctx);
    }

    // show new panel
    in->panel_top = panel;
    ctx->priv_panel = talloc_zero_size(ctx, panel->priv_size);
    in->panel_top->init(ctx, data);
    if (in->panel_top->on_show)
        in->panel_top->on_show(ctx);
}

static void do_pop_panel(struct ui_context *ctx)
{
    struct ui_context_internal *in = ctx->priv_context;
    if (!in->panel_top)
        return;

    if (in->panel_top->uninit)
        in->panel_top->uninit(ctx);
    in->panel_top = NULL;
    TA_FREEP(&ctx->priv_panel);

    struct ui_panel_item item;
    bool has_panel = MP_TARRAY_POP(in->panel_stack, in->panel_count, &item);
    if (has_panel) {
        ctx->priv_panel = item.data;
        in->panel_top = item.panel;
        if (in->panel_top->on_show)
            in->panel_top->on_show(ctx);
    }
}

static void handle_redraw(struct ui_context *ctx)
{
    struct ui_context_internal *in = ctx->priv_context;
    if (!in->want_redraw)
        return;

    in->want_redraw = false;
    ui_render_driver_vita.render_start(ctx);
    const struct ui_panel *panel = get_top_panel(ctx);
    if (panel)
        panel->on_draw(ctx);
    ui_render_driver_vita.render_end(ctx);
}

static struct mp_dispatch_queue *get_dispatch(struct ui_context *ctx)
{
    struct ui_context_internal *in = ctx->priv_context;
    return in->dispatch;
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
        mp_dispatch_queue_process(get_dispatch(ctx), 0);

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
    struct ui_context_internal *in = ctx->priv_context;
    pthread_mutex_lock(&in->lock);
    if (!in->need_wakeup) {
        in->need_wakeup = true;
        pthread_cond_signal(&in->wakeup);
    }
    pthread_mutex_unlock(&in->lock);
}

void ui_panel_common_invalidate(struct ui_context *ctx)
{
    struct ui_context_internal *in = ctx->priv_context;
    in->want_redraw = true;
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
    struct ui_context_internal *in = ctx->priv_context;
    return in->frame_start;
}

struct ui_font *ui_panel_common_get_font(struct ui_context *ctx)
{
    struct ui_context_internal *in = ctx->priv_context;
    if (!in->font_init) {
        in->font_init = true;
        ui_render_driver_vita.font_init(ctx, &in->font_impl);
    }
    return in->font_impl;
}

void ui_panel_common_run_sync(struct ui_context *ctx, ui_panel_run_fn fn, void *data)
{
    mp_dispatch_run(get_dispatch(ctx), fn, data);
}

void ui_panel_common_run_post(struct ui_context *ctx, ui_panel_run_fn fn, void *data)
{
    mp_dispatch_enqueue(get_dispatch(ctx), fn, data);
}

void ui_panel_common_run_post_steal(struct ui_context *ctx, ui_panel_run_fn fn, void *data)
{
    mp_dispatch_enqueue_autofree(get_dispatch(ctx), fn, data);
}

void ui_panel_common_run_cancel(struct ui_context *ctx, ui_panel_run_fn fn, void *data)
{
    mp_dispatch_cancel_fn(get_dispatch(ctx), fn, data);
}
