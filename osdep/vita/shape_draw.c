#include "shape_draw.h"
#include "common/common.h"

struct vertex_cursor {
    struct ui_color_vertex *base;
    int idx;
};

void shape_draw_do_build_rect_verts(void *data, int i, int n,
                                    shape_draw_verts_fn_dup fn_dup,
                                    shape_draw_verts_fn_write_rect fn_write)
{
    for (int idx = i; idx < n; ++idx) {
        if (idx) {
            fn_dup(data);
            fn_write(data, idx, false, false);
            fn_dup(data);
        } else {
            fn_write(data, idx, false, false);
        }
        fn_write(data, idx, true, false);
        fn_write(data, idx, false, true);
        fn_write(data, idx, true, true);
    }
}

static void rect_fn_dup(void *data)
{
    void **args = data;
    struct ui_context *ctx = args[0];
    struct vertex_cursor *c = args[1];
    ui_render_driver_vita.draw_vertices_duplicate(ctx, c->base, c->idx++);
}

static void rect_fn_write(void *data, int i, bool lr, bool tb)
{
    void **args = data;
    struct ui_context *ctx = args[0];
    struct vertex_cursor *c = args[1];
    struct shape_draw_item *item = args[2];
    struct shape_draw_rect *rect = &item->shape.rect;
    float x = lr ? rect->x0 : rect->x1;
    float y = tb ? rect->y0 : rect->y1;
    ui_render_driver_vita.draw_vertices_compose(ctx, c->base, c->idx++, x, y, item->color);
}

static void build_verts_rect(struct ui_context *ctx,
                             struct vertex_cursor *cursor,
                             struct shape_draw_item *items, int i)
{
    void *args[] = { ctx, cursor, &items[i] };
    shape_draw_do_build_rect_verts(args, i, i + 1, rect_fn_dup, rect_fn_write);
}

void shape_draw_commit(struct ui_context *ctx, struct shape_draw_item *items, int n)
{
    int vert_n = 0;
    for (int i = 0; i < n; ++i) {
        if (items[i].type == SHAPE_DRAW_TYPE_RECT)
            vert_n += 4;
    }
    vert_n += MPMAX(n - 1, 0) * 2;

    struct vertex_cursor c = {0};
    if (!ui_render_driver_vita.draw_vertices_prepare(ctx, &c.base, vert_n))
        return;

    for (int i = 0; i < n; ++i) {
        struct shape_draw_item *item = &items[i];
        if (items[i].type == SHAPE_DRAW_TYPE_RECT)
            build_verts_rect(ctx, &c, items, i);
    }
    ui_render_driver_vita.draw_vertices_commit(ctx, c.base, vert_n);
}
