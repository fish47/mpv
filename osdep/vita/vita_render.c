#include "config.h"
#include "ui_driver.h"
#include "ui_context.h"
#include "common/common.h"
#include "options/path.h"

#include <vita2d.h>
#include <libavutil/macros.h>
#include <libavutil/imgutils.h>

struct font_impl_data_pgf {
    void *reserved;
};

struct font_impl_data_freetype {
    const char *font_path;
};

union font_impl_data_pack {
    struct font_impl_data_pgf pgf;
    struct font_impl_data_freetype freetype;
};

struct font_impl {
    void *(*init)(union font_impl_data_pack *data);
    void (*uninit)(void *font);
    void (*draw)(void *font, struct ui_font_draw_args *args);
};

struct priv_render {
    const struct font_impl *font_impl;
    union font_impl_data_pack font_impl_data;
};

static const struct font_impl font_impl_pgf;
static const struct font_impl font_impl_freetype;

static const struct font_impl *font_impl_list[] = {
    &font_impl_freetype,
    &font_impl_pgf,
};

static const char *freetype_font_paths[] = {
    "ux0:app/"MPV_VITA_TITLE_ID"/font.ttf",
    "ux0:app/"MPV_VITA_TITLE_ID"/font.ttc",
};

static struct priv_render *get_priv_render(struct ui_context *ctx)
{
    return (struct priv_render*) ctx->priv_render;
}

static void *font_impl_pgf_init(union font_impl_data_pack *data)
{
    return vita2d_load_default_pgf();
}

static void font_impl_pgf_uninit(void *font)
{
    vita2d_pgf *pgf = font;
    vita2d_free_pgf(pgf);
}

static void font_impl_pgf_draw(void *font, struct ui_font_draw_args *args)
{
    vita2d_pgf *pgf = font;
    float scale = MPMIN(args->size / 24.0, 1);
    vita2d_pgf_draw_text(pgf, args->x, args->y, args->color, scale, args->text);
}

static void *font_impl_ft_init(union font_impl_data_pack *data)
{
    const char *path = data->freetype.font_path;
    if (path && mp_path_exists(path))
        return vita2d_load_font_file(path);

    for (int i = 0; i < MP_ARRAY_SIZE(freetype_font_paths); ++i) {
        path = freetype_font_paths[i];
        if (mp_path_exists(path)) {
            data->freetype.font_path = path;
            return vita2d_load_font_file(path);
        }
    }

    return NULL;
}

static void font_impl_ft_uninit(void *font)
{
    vita2d_font *ft = font;
    vita2d_free_font(ft);
}

static void font_impl_ft_draw(void *font, struct ui_font_draw_args *args)
{
    vita2d_font *ft = font;
    vita2d_font_draw_text(ft, args->x, args->y, args->color, args->size, args->text);
}

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

static bool do_init_yuv420p3(struct ui_texture **tex, int w, int h)
{
    // https://github.com/xerpi/libvita2d/issues/86
    // vita2d doesn't handle 3-byte-per-pixel formats

    // firstly, create a texture with the same amount of memory
    int rounded_w = FFALIGN(w, 16);
    int rounded_h = FFALIGN(h, 2);
    int total_bytes = rounded_w * rounded_h * 3 / 2;
    int extra_w = rounded_w;
    int extra_h = total_bytes / extra_w;
    if (!do_init_texture(tex, extra_w, extra_h, SCE_GXM_TEXTURE_FORMAT_S8_000R))
        return false;

    // secondly, correct its size and the format
    vita2d_texture *impl = (vita2d_texture*) *tex;
    void *data = vita2d_texture_get_datap(impl);
    sceGxmTextureInitLinear(&impl->gxm_tex, data,
                            SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC0,
                            rounded_w, rounded_h, 0);
    return true;
}

static bool render_texture_init(struct ui_context *ctx, struct ui_texture **tex,
                                enum ui_texure_fmt fmt, int w, int h)
{
    *tex = NULL;
    switch (fmt) {
    case TEX_FMT_RGBA:
        return do_init_texture(tex, FFALIGN(w, 8), h, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_RGBA);
    case TEX_FMT_YUV420:
        return do_init_yuv420p3(tex, w, h);
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

static bool render_font_init(struct ui_context *ctx, struct ui_font **font)
{
    void *impl = NULL;
    struct priv_render *priv = get_priv_render(ctx);
    if (priv->font_impl) {
        impl = priv->font_impl->init(&priv->font_impl_data);
        if (impl)
            goto found;
    }

    for (int i = 0; i < MP_ARRAY_SIZE(font_impl_list); ++i) {
        const struct font_impl *policy = font_impl_list[i];
        impl = policy->init(&priv->font_impl_data);
        if (impl) {
            priv->font_impl = policy;
            goto found;
        }
    }

found:
    if (!impl)
        return false;

    *font = impl;
    return true;
}

static void render_font_uninit(struct ui_context *ctx, struct ui_font **font)
{
    get_priv_render(ctx)->font_impl->uninit(*font);
    *font = NULL;
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

static void render_draw_font(struct ui_context *ctx, struct ui_font *font,
                             struct ui_font_draw_args *args)
{
    get_priv_render(ctx)->font_impl->draw(font, args);
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

static const struct font_impl font_impl_pgf = {
    .init = font_impl_pgf_init,
    .uninit = font_impl_pgf_uninit,
    .draw = font_impl_pgf_draw,
};

static const struct font_impl font_impl_freetype = {
    .init = font_impl_ft_init,
    .uninit = font_impl_ft_uninit,
    .draw = font_impl_ft_draw,
};

const struct ui_render_driver ui_render_driver_vita = {
    .priv_size = sizeof(struct priv_render),

    .init = render_init,
    .uninit = render_uninit,

    .render_start = render_render_start,
    .render_end = render_render_end,

    .texture_init = render_texture_init,
    .texture_uninit = render_texture_uninit,
    .texture_upload = render_texture_upload,

    .font_init = render_font_init,
    .font_uninit = render_font_uninit,

    .clip_start = render_clip_start,
    .clip_end = render_clip_end,

    .draw_font = render_draw_font,
    .draw_texture = render_draw_texture,
    .draw_rectangle = render_draw_rectangle,
};
