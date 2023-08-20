#include "ui_driver.h"
#include "common/common.h"

#include <vita2d.h>
#include <libavutil/macros.h>
#include <libavutil/imgutils.h>

struct priv_render {};

static bool render_init(struct ui_context *ctx)
{
    vita2d_init();
    vita2d_set_clear_color(RGBA8(0x00, 0x00, 0x00, 0xFF));
    return true;
}

static void render_uninit(struct ui_context *ctx)
{
    vita2d_fini();
}

static void render_render_start(struct ui_context *ctx)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
}

static void render_render_end(struct ui_context *ctx)
{
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static bool do_init_texture(struct ui_texture **tex,
                            int w, int h,
                            SceGxmTextureFormat fmt)
{
    vita2d_texture **cast = (vita2d_texture**) tex;
    vita2d_texture *impl = vita2d_create_empty_texture_format(w, h, fmt);
    if (!impl)
        return false;
    *cast = impl;
    return true;
}

static bool render_texture_init(struct ui_context *ctx, struct ui_texture **tex,
                                enum ui_texure_fmt fmt, int w, int h)
{
    *tex = NULL;
    switch (fmt) {
    case TEX_FMT_RGBA:
        return do_init_texture(tex, FFALIGN(w, 8), h,
                               SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_RGBA);
    case TEX_FMT_YUV420:
        return do_init_texture(tex, FFALIGN(w, 16), FFALIGN(h, 2),
                               SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC0);
    case TEX_FMT_UNKNOWN:
        return false;
    }
    return false;
}

static void render_texture_uninit(struct ui_context *ctx, struct ui_texture **tex)
{
    vita2d_texture *impl = (vita2d_texture*) *tex;
    if (impl)
        vita2d_free_texture(impl);
    *tex = NULL;
}

static void render_clip_start(struct ui_context *ctx, struct mp_rect *rect)
{
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(rect->x0, rect->y0, rect->x1, rect->y1);
}

static void render_clip_end(struct ui_context *ctx)
{
    vita2d_disable_clipping();
}

static enum AVPixelFormat get_ff_format(SceGxmTextureFormat fmt, int planes)
{
    if (fmt == SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_RGBA)
        return AV_PIX_FMT_RGBA;
    else if (fmt == SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC0 && planes == 3)
        return AV_PIX_FMT_YUV420P;
    else
        return AV_PIX_FMT_NONE;
}

static void render_texture_upload(struct ui_context *ctx,
                                  struct ui_texture *tex, int w, int h,
                                  const uint8_t **data, const int *strides, int planes)
{
    vita2d_texture *impl = (vita2d_texture*) tex;
    SceGxmTextureFormat vita_fmt = vita2d_texture_get_format(impl);
    enum AVPixelFormat ff_fmt = get_ff_format(vita_fmt, planes);
    if (ff_fmt == AV_PIX_FMT_NONE)
        return;

    int dst_strides[4];
    uint8_t *dst_data[4];
    void *tex_data = vita2d_texture_get_datap(impl);
    int tex_w = vita2d_texture_get_width(impl);
    int tex_h = vita2d_texture_get_height(impl);
    av_image_fill_arrays(dst_data, dst_strides, tex_data, ff_fmt, tex_w, tex_h, 1);
    av_image_copy(dst_data, dst_strides, data, strides, ff_fmt, w, h);
}

static void render_draw_texture(struct ui_context *ctx, struct ui_texture *tex,
                                struct ui_texture_draw_args *args)
{
    int tex_w = mp_rect_w(*args->src);
    int tex_h = mp_rect_h(*args->src);
    if (!tex_w || !tex_h)
        return;

    int dst_w = mp_rect_w(*args->dst);
    int dst_h = mp_rect_h(*args->dst);
    if (!dst_w || !dst_h)
        return;

    vita2d_texture *impl = (vita2d_texture*) tex;
    float sx = (float) dst_w / tex_w;
    float sy = (float) dst_h / tex_h;
    vita2d_draw_texture_part_scale(impl,
                                   args->dst->x0, args->dst->y0,
                                   args->src->x0, args->src->y0,
                                   tex_w, tex_h, sx, sy);
}

static vita2d_color_vertex* pool_alloc_color_vertex(int count)
{
    size_t vert_size = sizeof(vita2d_color_vertex);
    void *mem = vita2d_pool_memalign(count * vert_size, vert_size);
    return (vita2d_color_vertex*) mem;
}

static void render_draw_rectangle(struct ui_context *ctx,
                                  struct ui_rectangle_draw_args *args)
{
    int count = args->count * 4 + (args->count - 1) * 2;
    vita2d_color_vertex *base = pool_alloc_color_vertex(count);
    if (!base)
        return;

    vita2d_color_vertex *p = base;
    for (int i = 0; i < args->count; ++i) {
        int c = args->colors[i];
        struct mp_rect *r = &args->rects[i];

        *p++ = (vita2d_color_vertex) { .x = r->x0, .y = r->y0, .z = 0.5, .color = c };
        if (i > 0) {
            vita2d_color_vertex *first = p - 1;
            *p++ = *first;
        }

        *p++ = (vita2d_color_vertex) { .x = r->x1, .y = r->y0, .z = 0.5, .color = c };
        *p++ = (vita2d_color_vertex) { .x = r->x0, .y = r->y1, .z = 0.5, .color = c };
        *p++ = (vita2d_color_vertex) { .x = r->x1, .y = r->y1, .z = 0.5, .color = c };

        if (i + 1 < args->count) {
            vita2d_color_vertex *last = p - 1;
            *p++ = *last;
        }
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, base, count);
}

const struct ui_render_driver ui_render_driver_vita = {
    .priv_size = sizeof(struct priv_render),

    .init = render_init,
    .uninit = render_uninit,

    .render_start = render_render_start,
    .render_end = render_render_end,

    .texture_init = render_texture_init,
    .texture_uninit = render_texture_uninit,
    .texture_upload = render_texture_upload,

    .clip_start = render_clip_start,
    .clip_end = render_clip_end,

    .draw_texture = render_draw_texture,
    .draw_rectangle = render_draw_rectangle,
};
