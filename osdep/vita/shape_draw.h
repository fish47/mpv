#pragma once

#include <stdbool.h>

#include "ui_driver.h"

typedef void (*shape_draw_verts_fn_dup)(void *data);
typedef void (*shape_draw_verts_fn_write_rect)(void *data, int i, bool lr, bool tb);

struct shape_draw_rect {
    float x0;
    float y0;
    float x1;
    float y1;
};

enum shape_draw_type {
    SHAPE_DRAW_TYPE_RECT_LINE,
    SHAPE_DRAW_TYPE_RECT_FILL,
};

struct shape_draw_item {
    enum shape_draw_type type;
    ui_color color;
    float line;
    union {
        struct shape_draw_rect rect;
    } shape;
};

void shape_draw_do_build_rect_verts(void *data, int i, int n,
                                    shape_draw_verts_fn_dup fn_dup,
                                    shape_draw_verts_fn_write_rect fn_write);

void shape_draw_commit(struct ui_context *ctx, struct shape_draw_item *items, int n);
