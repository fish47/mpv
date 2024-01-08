#include "player_perf.h"
#include "ui_panel.h"
#include "ui_driver.h"
#include "player/core.h"
#include "demux/demux.h"
#include "misc/dispatch.h"
#include "misc/linked_list.h"

#define PERF_DRAW_START_L       30
#define PERF_DRAW_START_T       30
#define PERF_DRAW_FONT_SIZE     15
#define PERF_DRAW_TEXT_COLOR    0xff0000ff
#define PERF_UPDATE_INTERVAL    (1000000L / 2)
#define PERF_MAX_PENDING_COUNT  2

struct perf_lines {
    char **lines;
    int count;
};

struct perf_data_ref {
    struct player_perf_ctx *ppc;
    struct ui_context *ctx;
    struct MPContext *mpc;
};

struct perf_data {
    struct perf_data_ref ref;
    struct perf_lines done;
    struct perf_lines cache;
    struct {
        struct perf_data *prev;
        struct perf_data *next;
    } list;
};

struct perf_data_ll {
    struct perf_data *head;
    struct perf_data *tail;
};

struct player_perf_ctx {
    uint64_t poll_time;
    int pending_count;
    struct perf_data *current;
    struct perf_data_ll free_list;
    struct perf_data_ll pending_list;
};

struct player_perf_ctx *player_perf_create_ctx(void *parent)
{
    return ta_zalloc_size(parent, sizeof(struct player_perf_ctx));
}

void player_perf_draw(struct player_perf_ctx *c, struct ui_context *ctx)
{
    if (!c->current)
        return;

    struct ui_font *font = ui_panel_common_get_font(ctx);
    if (!font)
        return;

    struct ui_font_draw_args args = {
        .x = PERF_DRAW_START_L,
        .y = PERF_DRAW_START_T,
        .size = PERF_DRAW_FONT_SIZE,
        .color = PERF_DRAW_TEXT_COLOR,
    };
    for (int i = 0; i < c->current->done.count; ++i) {
        args.text = c->current->done.lines[i];
        ui_render_driver_vita.draw_font(ctx, font, &args);
        args.y += PERF_DRAW_FONT_SIZE;
    }
}

static void do_swap_perf_data(void *p)
{
    struct perf_data *data = p;
    struct player_perf_ctx *ppc = data->ref.ppc;
    if (ppc->current)
        LL_APPEND(list, &ppc->free_list, ppc->current);
    ppc->current = data;
    --ppc->pending_count;
    LL_REMOVE(list, &ppc->pending_list, data);
    ui_panel_common_invalidate(data->ref.ctx);
}

static void do_append_perf_line(struct perf_data *data, const char *fmt, ...)
{
    void *line = NULL;
    MP_TARRAY_POP(data->cache.lines, data->cache.count, &line);

    bstr str = { .start = line, .len = 0 };
    va_list va;
    va_start(va, fmt);
    bstr_xappend_vasprintf(data, &str, fmt, va);
    va_end(va);

    MP_TARRAY_APPEND(data, data->done.lines, data->done.count, BSTR_CAST(str));
}

static void do_update_perf_data(void *p)
{
    char buf[100];
    struct perf_data *data = p;

    int dr_count = 0;
    size_t dr_size = 0;
    void *dr_args[] = { &dr_count, &dr_size };
    struct MPContext *mpc = data->ref.mpc;
    struct vo *vo = mpc->video_out;
    if (vo)
        vo_control(mpc->vo_chain->vo, VOCTRL_GET_DR_STATS, dr_args);

    struct demux_reader_state s = {0};
    struct demuxer *demux = mpc->demuxer;
    if (demux)
        demux_get_reader_state(demux, &s);

    format_file_size_sn(buf, MP_ARRAY_SIZE(buf), dr_size);
    do_append_perf_line(data, "dr = %d x %s", dr_count, buf);

    format_file_size_sn(buf, MP_ARRAY_SIZE(buf), s.fw_bytes);
    do_append_perf_line(data, "fw_bytes = %s", buf);

    format_file_size_sn(buf, MP_ARRAY_SIZE(buf), s.total_bytes);
    do_append_perf_line(data, "total_bytes = %s", buf);

    ui_panel_common_run_post(data->ref.ctx, do_swap_perf_data, data);
}


static struct perf_data *do_obtain_free_perf_data(struct player_perf_ctx *ppc)
{
    struct perf_data *data = ppc->free_list.head;
    if (!data) {
        data = ta_zalloc_size(ppc, sizeof(struct perf_data));
        return data;
    }

    LL_REMOVE(list, &ppc->free_list, data);

    // recycle all string lines to cache
    int inc_size = data->done.count;
    int old_size = data->cache.count;
    int new_size = old_size + inc_size;
    MP_TARRAY_GROW(data, data->cache.lines, new_size);
    memcpy(data->cache.lines + old_size, data->done.lines, sizeof(char*) * inc_size);
    data->done.count = 0;
    data->cache.count = new_size;
    return data;
}

void player_perf_poll(struct player_perf_ctx *ppc,
                      struct ui_context *ctx, struct MPContext *mpc)
{
    if (ppc->pending_count >= PERF_MAX_PENDING_COUNT)
        return;

    uint64_t now = ui_panel_common_get_frame_time(ctx);
    uint64_t delta = now - ppc->poll_time;
    if (delta < PERF_UPDATE_INTERVAL)
        return;

    struct perf_data *data = do_obtain_free_perf_data(ppc);
    data->ref = (struct perf_data_ref) {
        .ppc = ppc,
        .ctx = ctx,
        .mpc = mpc,
    };
    ++ppc->pending_count;
    LL_APPEND(list, &ppc->pending_list, data);
    mp_dispatch_enqueue(mpc->dispatch, do_update_perf_data, data);
    ppc->poll_time = now;
}

void player_perf_stop(struct player_perf_ctx *ppc, struct ui_context *ctx)
{
    // mpv will drain all events before destruction
    // cancel all pending callbacks posted in core thread of mpv
    struct perf_data *data = ppc->pending_list.head;
    while (data) {
        struct perf_data *next = data->list.next;
        ui_panel_common_run_cancel(ctx, do_swap_perf_data, data);
        ta_free(data);
        data = next;
    }
    ppc->pending_count = 0;
}
