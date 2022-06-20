#include "ui_driver.h"
#include "common/common.h"

#include <vita2d.h>

struct priv_render {};

// wrap the implementation to keep structure consistency,
// so wrapper and implementation pointer can be casted to each other.
struct ui_texture {
    vita2d_texture impl;
};

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

static bool do_init_texture(struct ui_texture **tex, int w, int h, SceGxmTextureFormat fmt)
{
    vita2d_texture *impl = vita2d_create_empty_texture_format(w, h, fmt);
    if (!impl)
        return false;

    *tex = impl;
    return true;
}

static int get_aligned_size(int size, int align)
{
    // alignment should be power of 2
    return (size + align - 1) & ~(align - 1);
}

static bool render_texture_init(struct ui_context *ctx, struct ui_texture **tex,
                                enum ui_texure_fmt fmt, int w, int h)
{
    *tex = NULL;
    switch (fmt) {
    case TEX_FMT_RGBA:
        return do_init_texture(tex, get_aligned_size(w, 8), h, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_RGBA);
    case TEX_FMT_YUV420:
        return do_init_texture(tex, get_aligned_size(w, 16), h, SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC0);
    case TEX_FMT_UNKNOWN:
        return false;
    }
    return false;
}

static void render_texture_uninit(struct ui_context *ctx, struct ui_texture **tex)
{
    vita2d_texture *impl = *tex;
    if (impl)
        vita2d_free_texture(impl);
    *tex = NULL;
}

static void do_copy_plane(void *dst, void *src, int w, int h, int bpp,
                          int dst_stride, int src_stride)
{
    if (dst_stride == src_stride) {
        memcpy(dst, src, h * dst_stride);
    } else {
        uint8_t *row_dst = dst;
        uint8_t *row_src = src;
        int row_bytes = w * bpp;
        for (int i = 0; i < h; ++i) {
            memcpy(row_dst, row_src, row_bytes);
            row_dst += dst_stride;
            row_src += src_stride;
        }
    }
}

static void render_texture_upload(struct ui_context *ctx, struct ui_texture *tex,
                                  void **data, int *strides, int planes)
{
    vita2d_texture *impl = tex;
    void *tex_data = vita2d_texture_get_datap(impl);
    int tex_w = vita2d_texture_get_width(impl);
    int tex_h = vita2d_texture_get_height(impl);
    int tex_stride = vita2d_texture_get_stride(impl);
    SceGxmTextureFormat fmt = vita2d_texture_get_format(impl);
    if (fmt == SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_RGBA) {
        do_copy_plane(tex_data, data[0], tex_w, tex_h, 4, tex_stride, strides[0]);
    } else if (fmt == SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC0) {
        if (planes != 3)
            return;

        int tex_w_uv = tex_w / 2;
        int tex_h_uv = tex_h / 2;
        int stride_y = tex_stride / 4;
        int stride_uv = tex_stride / 8;
        uint8_t *dst_y = tex_data;
        uint8_t *dst_u = dst_y + stride_y * tex_h;
        uint8_t *dst_v = dst_u + stride_uv * tex_h_uv;
        do_copy_plane(dst_y, data[0], tex_w, tex_h, 1, stride_y, strides[0]);
        do_copy_plane(dst_u, data[1], tex_w_uv, tex_h_uv, 1, stride_uv, strides[1]);
        do_copy_plane(dst_v, data[2], tex_w_uv, tex_h_uv, 1, stride_uv, strides[2]);
    }
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

    vita2d_texture *impl = tex;
    float sx = (float) dst_w / tex_w;
    float sy = (float) dst_h / tex_h;
    vita2d_draw_texture_part_scale(impl,
                                   args->dst->x0, args->dst->y0,
                                   args->src->x0, args->src->y0,
                                   tex_w, tex_h, sx, sy);
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

    .draw_texture = render_draw_texture,
};
