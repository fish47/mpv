#include "player_osc.h"
#include "ui_panel.h"
#include "key_helper.h"
#include "shape_draw.h"

#include "ta/ta.h"
#include "misc/bstr.h"
#include "player/core.h"
#include "libmpv/client.h"

#include <time.h>

#define UI_COLOR_OVERLAY            0xbf000000
#define UI_COLOR_BASE_TEXT          0xffffffff
#define UI_COLOR_PROGRESS_BAR       0xff722B72
#define UI_COLOR_PROGRESS_FRAME     0xbfffffff

#define LAYOUT_OVERLAY_TOP_H            40

#define LAYOUT_TOP_BASE_T               0
#define LAYOUT_TOP_BASE_B               (LAYOUT_TOP_BASE_T + LAYOUT_OVERLAY_TOP_H)
#define LAYOUT_TOP_BASE_MARGIN_X        20
#define LAYOUT_TOP_BASE_TEXT_P          28
#define LAYOUT_TOP_BASE_FONT_SIZE       20

#define LAYOUT_TOP_TITLE_L              LAYOUT_TOP_BASE_MARGIN_X
#define LAYOUT_TOP_TITLE_R              800
#define LAYOUT_TOP_TIME_L               820
#define LAYOUT_TOP_BATTERY_L            890

#define LAYOUT_OVERLAY_BOTTOM_H         90
#define LAYOUT_OVERLAY_BOTTOM_T         (VITA_SCREEN_H - LAYOUT_OVERLAY_BOTTOM_H)
#define LAYOUT_OVERLAY_BOTTOM_B         (VITA_SCREEN_H)

#define LAYOUT_PROGRESS_FRAME_MARGIN_T  20
#define LAYOUT_PROGRESS_FRAME_MARGIN_X  20
#define LAYOUT_PROGRESS_FRAME_LINE_W    2
#define LAYOUT_PROGRESS_FRAME_H         20
#define LAYOUT_PROGRESS_FRAME_L         LAYOUT_PROGRESS_FRAME_MARGIN_X
#define LAYOUT_PROGRESS_FRAME_R         (VITA_SCREEN_W - LAYOUT_PROGRESS_FRAME_MARGIN_X)
#define LAYOUT_PROGRESS_FRAME_T         (LAYOUT_OVERLAY_BOTTOM_T + LAYOUT_PROGRESS_FRAME_MARGIN_T)
#define LAYOUT_PROGRESS_FRAME_B         (LAYOUT_PROGRESS_FRAME_T + LAYOUT_PROGRESS_FRAME_H)

#define LAYOUT_PROGRESS_BAR_MARGIN      4
#define LAYOUT_PROGRESS_BAR_L           (LAYOUT_PROGRESS_FRAME_L + LAYOUT_PROGRESS_BAR_MARGIN)
#define LAYOUT_PROGRESS_BAR_R           (LAYOUT_PROGRESS_FRAME_R - LAYOUT_PROGRESS_BAR_MARGIN)
#define LAYOUT_PROGRESS_BAR_T           (LAYOUT_PROGRESS_FRAME_T + LAYOUT_PROGRESS_BAR_MARGIN)
#define LAYOUT_PROGRESS_BAR_B           (LAYOUT_PROGRESS_FRAME_B - LAYOUT_PROGRESS_BAR_MARGIN)

static void on_key_seek(void *p, const void *data, int repeat);
static void on_key_ok(void *p, const void *data, int repeat);
static void on_key_cancel(void *p, const void *data, int repeat);

static const struct key_helper_spec key_helper_spec_list[] = {
    { .key = UI_KEY_CODE_VITA_DPAD_UP, .callback = on_key_seek, .data = (const void*) 10, .repeatable = true },
    { .key = UI_KEY_CODE_VITA_DPAD_DOWN, .callback = on_key_seek, .data = (const void*) -10, .repeatable = true },
    { .key = UI_KEY_CODE_VITA_DPAD_LEFT, .callback = on_key_seek, .data = (const void*) -5, .repeatable = true },
    { .key = UI_KEY_CODE_VITA_DPAD_RIGHT, .callback = on_key_seek, .data = (const void*) 5, .repeatable = true },
    { .key = UI_KEY_CODE_VITA_VIRTUAL_OK, .callback = on_key_ok },
    { .key = UI_KEY_CODE_VITA_VIRTUAL_CANCEL, .callback = on_key_cancel },
};

enum poller_type {
    POLLER_TYPE_TIME,
    POLLER_TYPE_BATTERY,
    POLLER_TYPE_HIDE,
    POLLER_TYPE_FADE,
    POLLER_TYPE_MAX,
};

typedef void (*poller_update_fn)(struct player_osc_ctx *c, struct ui_context *ctx);

struct poller_spec {
    bool implicit;
    int64_t delay;
    int64_t period;
    int64_t duration;
    poller_update_fn callback;
};

static void do_poll_time(struct player_osc_ctx *c, struct ui_context *ctx);
static void do_poll_battery(struct player_osc_ctx *c, struct ui_context *ctx);
static void do_poll_hide(struct player_osc_ctx *c, struct ui_context *ctx);
static void do_poll_fade(struct player_osc_ctx *c, struct ui_context *ctx);

static const struct poller_spec poller_spec_list[POLLER_TYPE_MAX] = {
    [POLLER_TYPE_TIME] = {
        .implicit = true,
        .delay = 0,
        .period = 60 * 1000000L,
        .duration = INT64_MAX,
        .callback = do_poll_time,
    },
    [POLLER_TYPE_BATTERY] = {
        .implicit = true,
        .delay = 0,
        .period = 5 * 60 * 1000000L,
        .duration = INT64_MAX,
        .callback = do_poll_battery,
    },
    [POLLER_TYPE_HIDE] = {
        .implicit = false,
        .delay = 1000 * 1000L,
        .period = 0,
        .duration = 0,
        .callback = do_poll_hide,
    },
    [POLLER_TYPE_FADE] = {
        .implicit = false,
        .delay = 0,
        .period = 0,
        .duration = 500 * 1000L,
        .callback = do_poll_fade,
    },
};

struct key_callback_args {
    struct player_osc_ctx *osc;
    struct ui_context *ctx;
    struct mpv_handle *mpv;
    struct MPContext *mpc;
};

struct player_osc_ctx {
    float osc_alpha;
    int pause_state;
    struct key_helper_ctx key_ctx;

    bstr media_title;
    int progress_bar_width;
    char time_text[8];
    char battery_text[6];
    int battery_percent;

    int64_t poller_schedule_times[POLLER_TYPE_MAX];
    int64_t poller_trigger_times[POLLER_TYPE_MAX];
    int64_t poller_min_trigger_time;
};


static void poller_schedule(struct player_osc_ctx *c, struct ui_context *ctx, enum poller_type type)
{
    const struct poller_spec *spec = &poller_spec_list[type];
    int64_t now = ui_panel_common_get_frame_time(ctx);
    int64_t trigger = now + spec->delay;
    c->poller_schedule_times[type] = now;
    c->poller_trigger_times[type] = trigger;
    c->poller_min_trigger_time = MPMIN(c->poller_min_trigger_time, trigger);
}

static void poller_stop(struct player_osc_ctx *c, enum poller_type type)
{
    c->poller_schedule_times[type] = 0;
    c->poller_trigger_times[type] = INT64_MAX;
}

static void poller_run(struct player_osc_ctx *c, struct ui_context *ctx)
{
    int64_t now = ui_panel_common_get_frame_time(ctx);
    if (c->poller_min_trigger_time > now)
        return;

    bool stop = false;
    int64_t min_time = INT64_MAX;
    for (int i = 0; i < POLLER_TYPE_MAX; ++i) {
        const struct poller_spec *spec = &poller_spec_list[i];

        // schedule implicit pollers at the first time
        if (!c->poller_schedule_times[i]) {
            if (!spec->implicit)
                continue;
            poller_schedule(c, ctx, i);
        }

        // execute pending pollers
        if (c->poller_trigger_times[i] < now) {
            spec->callback(c, ctx);

            // finish the poller exceeding its duration
            if (spec->duration != INT64_MAX) {
                int64_t end = c->poller_schedule_times[i] + spec->duration;
                if (now > end) {
                    c->poller_schedule_times[i] = 0;
                    continue;
                }
            }

            // pollers may not be guarenteed to be executed for every period
            if (spec->period) {
                int64_t delta = now - c->poller_trigger_times[i];
                c->poller_trigger_times[i] += (delta + spec->period) / spec->period * spec->period;
            }
        }
        min_time = stop ? INT64_MAX : MPMIN(min_time, c->poller_trigger_times[i]);
    }
    c->poller_min_trigger_time = min_time;
}

static void do_poll_time(struct player_osc_ctx *c, struct ui_context *ctx)
{
    struct ui_font *font = ui_panel_common_get_font(ctx);
    if (!font)
        return;

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(c->time_text, sizeof(c->time_text), "%H:%M", tm);
    ui_panel_common_invalidate(ctx);
}

static void do_poll_battery(struct player_osc_ctx *c, struct ui_context *ctx)
{
    struct ui_font *font = ui_panel_common_get_font(ctx);
    if (!font)
        return;

    int percent = ui_platform_driver_vita.get_battery_level(ctx);
    if (c->battery_percent == percent)
        return;

    c->battery_percent = percent;
    snprintf(c->battery_text, sizeof(c->battery_text), "%d%%", percent);
    ui_panel_common_invalidate(ctx);
}

static void do_poll_hide(struct player_osc_ctx *c, struct ui_context *ctx)
{
    poller_schedule(c, ctx, POLLER_TYPE_FADE);
}

static void do_poll_fade(struct player_osc_ctx *c, struct ui_context *ctx)
{
    const struct poller_spec *spec = &poller_spec_list[POLLER_TYPE_FADE];
    int64_t now = ui_panel_common_get_frame_time(ctx);
    int64_t delta = now - c->poller_schedule_times[POLLER_TYPE_FADE];
    c->osc_alpha = MPMAX(1.0 - (float) delta / spec->duration, 0);
    ui_panel_common_invalidate(ctx);
}

struct player_osc_ctx *player_osc_create_ctx(void *parent)
{
    return ta_zalloc_size(parent, sizeof(struct player_osc_ctx));
}

void player_osc_setup(struct player_osc_ctx *c, struct mpv_handle *mpv, struct MPContext *mpc)
{
    mpv_observe_property(mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "percent-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "media-title", MPV_FORMAT_STRING);
}

void do_show_osc(struct player_osc_ctx *c, struct ui_context *ctx, bool delayed_hide)
{
    // the osc may be hidden or fading
    if (c->osc_alpha < 1) {
        c->osc_alpha = 1;
        ui_panel_common_invalidate(ctx);
    }

    if (c->poller_schedule_times[POLLER_TYPE_FADE])
        poller_stop(c, POLLER_TYPE_FADE);

    if (delayed_hide)
        poller_schedule(c, ctx, POLLER_TYPE_HIDE);
}

static void do_handle_props(struct player_osc_ctx *c, struct ui_context *ctx, mpv_event_property *prop)
{
    if (prop->format == MPV_FORMAT_NONE)
        return;

    bool redraw = false;
    if (strcmp(prop->name, "pause") == 0) {
        // to ignore the first property event, which notifies clients of its initial value,
        // we keep track of whether its value is changed and show osc if it happens.
        int *pause = prop->data;
        if (c->pause_state != *pause) {
            c->pause_state = *pause;
            do_show_osc(c, ctx, true);
        }
    } else if (strcmp(prop->name, "percent-pos") == 0) {
        double *percent = prop->data;
        int full_width = (LAYOUT_PROGRESS_BAR_R - LAYOUT_PROGRESS_BAR_L);
        int new_width = full_width * (*percent) / 100;
        redraw = ((new_width != c->progress_bar_width));
        c->progress_bar_width = new_width;
    } else if (strcmp(prop->name, "media-title") == 0) {
        const char **str = prop->data;
        c->media_title.len = 0;
        bstr_xappend(c, &c->media_title, bstr0(*str));
        redraw = true;
    }

    if (redraw)
        ui_panel_common_invalidate(ctx);
}

void player_osc_on_event(struct player_osc_ctx *c, struct ui_context *ctx, struct mpv_event *e)
{
    if (e->event_id == MPV_EVENT_PROPERTY_CHANGE)
        do_handle_props(c, ctx, e->data);
    else if (e->event_id == MPV_EVENT_SEEK)
        do_show_osc(c, ctx, true);
}

static ui_color compute_translucent_color(float alpha, ui_color color)
{
    ui_color alpha_channel = ((ui_color) ((color >> 24) * alpha)) & 0xff;
    return (color & ~(0xff << 24)) | (alpha_channel << 24);
}

static void do_draw_overlay_top(struct player_osc_ctx *c, struct ui_context *ctx)
{
    struct ui_font *font = ui_panel_common_get_font(ctx);
    if (!font)
        return;

    if (c->media_title.len) {
        ui_render_driver_vita.clip_start(ctx, &(struct mp_rect) {
            .x0 = LAYOUT_TOP_TITLE_L,
            .y0 = LAYOUT_TOP_BASE_T,
            .x1 = LAYOUT_TOP_TITLE_R,
            .y1 = LAYOUT_TOP_BASE_B,
        });
        ui_render_driver_vita.draw_font(ctx, font, &(struct ui_font_draw_args) {
            .text = BSTR_CAST(c->media_title),
            .size = LAYOUT_TOP_BASE_FONT_SIZE,
            .x = LAYOUT_TOP_TITLE_L,
            .y = LAYOUT_TOP_BASE_TEXT_P,
            .color = compute_translucent_color(c->osc_alpha, UI_COLOR_BASE_TEXT),
        });
        ui_render_driver_vita.clip_end(ctx);
    }

    ui_render_driver_vita.draw_font(ctx, font, &(struct ui_font_draw_args) {
        .text = c->battery_text,
        .size = LAYOUT_TOP_BASE_FONT_SIZE,
        .x = LAYOUT_TOP_BATTERY_L,
        .y = LAYOUT_TOP_BASE_TEXT_P,
        .color = compute_translucent_color(c->osc_alpha, UI_COLOR_BASE_TEXT),
    });

    ui_render_driver_vita.draw_font(ctx, font, &(struct ui_font_draw_args) {
        .text = c->time_text,
        .size = LAYOUT_TOP_BASE_FONT_SIZE,
        .x = LAYOUT_TOP_TIME_L,
        .y = LAYOUT_TOP_BASE_TEXT_P,
        .color = compute_translucent_color(c->osc_alpha, UI_COLOR_BASE_TEXT),
    });

}

static void do_draw_shapes(struct player_osc_ctx *c, struct ui_context *ctx)
{
    int count = 0;
    struct shape_draw_item items[4];

    // top overlay
    items[count++] = (struct shape_draw_item) {
        .type = SHAPE_DRAW_TYPE_RECT_FILL,
        .color = compute_translucent_color(c->osc_alpha, UI_COLOR_OVERLAY),
        .shape.rect = {
            .x0 = 0,
            .y0 = 0,
            .x1 = VITA_SCREEN_W,
            .y1 = LAYOUT_OVERLAY_TOP_H,
        }
    };

    // bottom overlay
    items[count++] = (struct shape_draw_item) {
        .type = SHAPE_DRAW_TYPE_RECT_FILL,
        .color = compute_translucent_color(c->osc_alpha, UI_COLOR_OVERLAY),
        .shape.rect = {
            .x0 = 0,
            .y0 = LAYOUT_OVERLAY_BOTTOM_T,
            .x1 = VITA_SCREEN_W,
            .y1 = LAYOUT_OVERLAY_BOTTOM_B,
        },
    };

    // progress frame
    items[count++] = (struct shape_draw_item) {
        .type = SHAPE_DRAW_TYPE_RECT_LINE,
        .color = compute_translucent_color(c->osc_alpha, UI_COLOR_PROGRESS_FRAME),
        .line = LAYOUT_PROGRESS_FRAME_LINE_W,
        .shape.rect = {
            .x0 = LAYOUT_PROGRESS_FRAME_L,
            .y0 = LAYOUT_PROGRESS_FRAME_T,
            .x1 = LAYOUT_PROGRESS_FRAME_R,
            .y1 = LAYOUT_PROGRESS_FRAME_B,
        },
    };

    // progress bar
    items[count++] = (struct shape_draw_item) {
        .type = SHAPE_DRAW_TYPE_RECT_FILL,
        .color = compute_translucent_color(c->osc_alpha, UI_COLOR_PROGRESS_BAR),
        .shape.rect = {
            .x0 = LAYOUT_PROGRESS_BAR_L,
            .y0 = LAYOUT_PROGRESS_BAR_T,
            .x1 = LAYOUT_PROGRESS_BAR_L + c->progress_bar_width,
            .y1 = LAYOUT_PROGRESS_BAR_B,
        },
    };

    shape_draw_commit(ctx, items, count);
}

void player_osc_on_draw(struct player_osc_ctx *c, struct ui_context *ctx)
{
    if (c->osc_alpha <= 0)
        return;

    do_draw_shapes(c, ctx);
    do_draw_overlay_top(c, ctx);
}

void player_osc_on_poll(struct player_osc_ctx *c, struct ui_context *ctx,
                        struct mpv_handle *mpv, struct MPContext *mpc)
{
    int64_t time = ui_panel_common_get_frame_time(ctx);
    poller_run(c, ctx);
    key_helper_poll(&c->key_ctx, time, &(struct key_callback_args) {
        .osc = c,
        .ctx = ctx,
        .mpv = mpv,
        .mpc = mpc,
    });
}

void player_osc_on_key(struct player_osc_ctx *c, struct ui_context *ctx,
                       struct mpv_handle *mpv, struct MPContext *mpc, struct ui_key *key)
{
    int64_t time = ui_panel_common_get_frame_time(ctx);
    int n = MP_ARRAY_SIZE(key_helper_spec_list);
    const struct key_helper_spec *list = key_helper_spec_list;
    key_helper_dispatch(&c->key_ctx, key, time, list, n, &(struct key_callback_args) {
        .osc = c,
        .ctx = ctx,
        .mpv = mpv,
        .mpc = mpc,
    });
}

static void on_key_seek(void *p, const void *data, int repeat)
{
    struct key_callback_args *args = p;
    int amount = (intptr_t) data * MPMAX(repeat, 1);
    mpv_command_node_async(args->mpv, 0, &(mpv_node) {
        .format = MPV_FORMAT_NODE_ARRAY,
        .u.list = &(struct mpv_node_list) {
            .num = 2,
            .values = (mpv_node[]) {
                (mpv_node) { .format = MPV_FORMAT_STRING, .u.string = "seek" },
                (mpv_node) { .format = MPV_FORMAT_DOUBLE, .u.double_ = amount },
            },
        },
    });
}

static void on_key_ok(void *p, const void *data, int repeat)
{
    struct key_callback_args *args = p;
    mpv_command_async(args->mpv, 0, (const char*[]) { "cycle", "pause", NULL });
}

static void on_key_cancel(void *p, const void *data, int repeat)
{
    struct key_callback_args *args = p;
    mpv_command_async(args->mpv, 0, (const char*[]) { "quit", NULL });
}
