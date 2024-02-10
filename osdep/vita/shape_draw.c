#include "shape_draw.h"
#include "common/common.h"

struct build_verts_params {
    struct ui_context *ctx;
    struct shape_draw_item *item;
    struct ui_color_vertex *base;
    int index;
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

static void vert_copy(struct build_verts_params *p, int idx)
{
    ui_render_driver_vita.draw_vertices_copy(p->ctx, p->base, p->index++, idx);
}

static void vert_write(struct build_verts_params *p, float x, float y)
{
    ui_render_driver_vita.draw_vertices_compose(p->ctx, p->base, p->index++,
                                                x, y, p->item->color);
}

static void rect_fn_dup(void *data)
{
    struct build_verts_params *p = data;
    vert_copy(p, p->index - 1);
}

static void rect_fn_write(void *data, int i, bool lr, bool tb)
{
    struct build_verts_params *p = data;
    struct shape_draw_rect *rect = &p->item->shape.rect;
    float x = lr ? rect->x0 : rect->x1;
    float y = tb ? rect->y0 : rect->y1;
    vert_write(p, x, y);
}

static void build_verts_rect_fill(struct build_verts_params *p, int i)
{
    shape_draw_do_build_rect_verts(p, 0, 1, rect_fn_dup, rect_fn_write);
}

static void build_verts_rect_line(struct build_verts_params *p, int i)
{
    float half = p->item->line * 0.5;
    struct shape_draw_rect *base = &p->item->shape.rect;
    struct shape_draw_rect in = {
        .x0 = base->x0 + half,
        .y0 = base->y0 + half,
        .x1 = base->x1 - half,
        .y1 = base->y1 - half,
    };
    struct shape_draw_rect out = {
        .x0 = base->x0 - half,
        .y0 = base->y0 - half,
        .x1 = base->x1 + half,
        .y1 = base->y1 + half,
    };

    vert_write(p, out.x0, out.y0);
    vert_write(p, in.x0, in.y0);
    vert_write(p, out.x1, out.y0);
    vert_write(p, in.x1, in.y0);
    vert_write(p, out.x1, out.y1);
    vert_write(p, in.x1, in.y1);
    vert_write(p, out.x0, out.y1);
    vert_write(p, in.x0, in.y1);
    vert_write(p, out.x0, out.y0);
    vert_write(p, in.x0, in.y0);
}

void shape_draw_commit(struct ui_context *ctx, struct shape_draw_item *items, int n)
{
    int vert_n = 0;
    for (int i = 0; i < n; ++i) {
        switch (items[i].type) {
        case SHAPE_DRAW_TYPE_RECT_LINE:
            vert_n += 10;
            break;
        case SHAPE_DRAW_TYPE_RECT_FILL:
            vert_n += 4;
            break;
        }
    }
    vert_n += MPMAX(n - 1, 0) * 2;

    struct build_verts_params p = { .ctx = ctx };
    if (!ui_render_driver_vita.draw_vertices_prepare(ctx, &p.base, vert_n))
        return;

    int sep_idx = 0;
    for (int i = 0; i < n; ++i) {
        if (i) {
            sep_idx = p.index;
            p.index += 2;
        }

        p.item = &items[i];
        switch (p.item->type) {
        case SHAPE_DRAW_TYPE_RECT_LINE:
            build_verts_rect_line(&p, i);
            break;
        case SHAPE_DRAW_TYPE_RECT_FILL:
            build_verts_rect_fill(&p, i);
            break;
        }

        if (i) {
            int bak = p.index;
            p.index = sep_idx;
            vert_copy(&p, p.index - 1);
            vert_copy(&p, p.index + 1);
            p.index = bak;
        }
    }
    ui_render_driver_vita.draw_vertices_commit(ctx, p.base, vert_n);
}
