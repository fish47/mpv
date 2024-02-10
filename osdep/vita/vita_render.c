#include "config.h"
#include "ui_driver.h"
#include "ui_context.h"
#include "common/common.h"
#include "options/path.h"

#include <vita2d.h>
#include <libavutil/macros.h>
#include <libavutil/imgutils.h>

#define DR_ALIGN_MIN_W      64
#define DR_ALIGN_MIN_H      64

#define VRAM_MEM_BLOCK_NAME         "mpv_vram"
#define VRAM_MEM_BLOCK_TYPE         SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW
#define VRAM_MEM_BLOCK_ALIGN        (256*1024)
#define VRAM_MEM_BLOCK_NONE_ID      (-1)

struct ui_color_vertex {
    vita2d_color_vertex impl;
};

struct texture_fmt_spec {
    enum ui_texure_fmt tex_fmt;
    SceGxmTextureFormat sce_fmt;
    int init_align_w;
    int init_align_h;
    int dr_align_w;
    int dr_align_h;
};

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
    void (*measure)(void *font, const char *text, int size, int *w, int *h);
};

struct vram_entry {
    SceUID id;
    void *addr;
    int size;
};

struct dr_texture {
    vita2d_texture tex;
    const struct texture_fmt_spec *spec;
    int w;
    int h;
};

struct priv_render {
    const struct font_impl *font_impl;
    union font_impl_data_pack font_impl_data;

    int vram_count;
    struct vram_entry *vram_list;
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

static const struct texture_fmt_spec texture_format_list[] = {
    [TEX_FMT_UNKNOWN] = { TEX_FMT_UNKNOWN, 0, 1, 1 },
    [TEX_FMT_RGBA] = { TEX_FMT_RGBA, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_RGBA, 8, 1, 16, 16 },
    [TEX_FMT_YUV420] = { TEX_FMT_YUV420, SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC0, 16, 2, 32, 16 },
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

static void font_impl_pgf_measure(void *font, const char *text, int size, int *w, int *h)
{
    vita2d_pgf *pgf = font;
    float scale = MPMIN(size / 24.0, 1);
    vita2d_pgf_text_dimensions(pgf, scale, text, w, h);
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

static void font_impl_ft_measure(void *font, const char *text, int size, int *w, int *h)
{
    vita2d_font *ft = font;
    vita2d_font_text_dimensions(font, size, text, w, h);
}

static void do_dr_align_size(enum ui_texure_fmt fmt, int *w, int *h)
{
    const struct texture_fmt_spec *spec = &texture_format_list[fmt];
    *w = FFMAX(FFALIGN(*w, spec->dr_align_w), DR_ALIGN_MIN_W);
    *h = FFMAX(FFALIGN(*h, spec->dr_align_h), DR_ALIGN_MIN_H);
}

static int render_dr_align(enum ui_texure_fmt fmt, int *w, int *h)
{
    if (fmt == TEX_FMT_YUV420) {
        do_dr_align_size(fmt, w, h);
        return (*w) * (*h) * 3 / 2;
    }
    return 0;
}

static bool render_dr_prepare(struct ui_context *ctx, const AVCodec *codec, AVDictionary **opts)
{
    if (codec->id == AV_CODEC_ID_H264 && strncmp(codec->name, "h264_vita", 9) == 0) {
        av_dict_set(opts, "vita_h264_dr", "1", 0);
        return true;
    }
    return false;
}

static struct vram_entry* find_vram_entry(struct priv_render *priv, const void *addr, int *idx)
{
    for (int i = 0; i < priv->vram_count; ++i) {
        struct vram_entry *ve = &priv->vram_list[i];
        if (addr == ve->addr) {
            if (idx)
                *idx = i;
            return ve;
        }
    }
    return NULL;
}

static void remove_vram_entry(struct priv_render *priv, int idx)
{
    // the order does not matter
    int last = priv->vram_count - 1;
    if (last && idx < last)
        memcpy(&priv->vram_list[idx], &priv->vram_list[last], sizeof(struct vram_entry));
    --priv->vram_count;
}

static bool render_dr_vram_init(struct ui_context *ctx, int size, void **vram)
{
    void *addr = NULL;
    int rounded_size = FFALIGN(size, VRAM_MEM_BLOCK_ALIGN);
    SceUID id = sceKernelAllocMemBlock(VRAM_MEM_BLOCK_NAME, VRAM_MEM_BLOCK_TYPE,
                                       rounded_size, NULL);
    if (id < 0)
        goto fail_alloc;
    if (sceKernelGetMemBlockBase(id, &addr) != SCE_OK)
        goto fail_addr;

    struct priv_render *priv = get_priv_render(ctx);
    MP_TARRAY_APPEND(priv, priv->vram_list, priv->vram_count, (struct vram_entry) {
        .id = id,
        .addr = addr,
        .size = rounded_size,
    });
    *vram = addr;
    return true;

fail_addr:
    sceKernelFreeMemBlock(id);
fail_alloc:
    return false;
}

static void render_dr_vram_uninit(struct ui_context *ctx, void **vram)
{
    int idx = 0;
    struct priv_render *priv = get_priv_render(ctx);
    struct vram_entry *ve = find_vram_entry(priv, *vram, &idx);
    if (ve) {
        sceKernelFreeMemBlock(ve->id);
        remove_vram_entry(priv, idx);
    }
    *vram = NULL;
}

static void render_dr_vram_lock(struct ui_context *ctx, void *vram)
{
    struct priv_render *priv = get_priv_render(ctx);
    struct vram_entry *ve = find_vram_entry(priv, vram, NULL);
    if (ve)
        sceGxmMapMemory(ve->addr, ve->size, SCE_GXM_MEMORY_ATTRIB_READ);
}

static void render_dr_vram_unlock(struct ui_context *ctx, void *vram)
{
    sceGxmUnmapMemory(vram);
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

static void do_set_texture_filters(vita2d_texture *tex)
{
    vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
}

static bool do_init_texture(vita2d_texture **p_tex,
                            const struct texture_fmt_spec *spec,
                            SceGxmTextureFormat fmt, int w, int h, bool dr)
{
    vita2d_texture *impl = NULL;
    if (dr) {
        struct dr_texture *tex = malloc(sizeof(struct dr_texture));
        if (!tex)
            return false;

        memset(&tex->tex, 0, sizeof(vita2d_texture));
        tex->w = w;
        tex->h = h;
        tex->spec = spec;
        tex->tex.data_UID = VRAM_MEM_BLOCK_NONE_ID;
        do_dr_align_size(spec->tex_fmt, &tex->w, &tex->h);
        impl = &tex->tex;
    } else {
        SceGxmTextureFormat resolve_fmt = spec ? spec->sce_fmt : fmt;
        impl = vita2d_create_empty_texture_format(w, h, resolve_fmt);
        if (!impl)
            return false;

        do_set_texture_filters(impl);
    }
    *p_tex = impl;
    return true;
}

static bool do_init_yuv420p3(vita2d_texture **p_tex, int rounded_w, int rounded_h)
{
    // https://github.com/xerpi/libvita2d/issues/86
    // vita2d doesn't handle 3-byte-per-pixel formats

    // firstly, create a texture with the same amount of memory
    int total_bytes = rounded_w * rounded_h * 3 / 2;
    int extra_w = rounded_w;
    int extra_h = total_bytes / extra_w;
    if (!do_init_texture(p_tex, NULL, SCE_GXM_TEXTURE_FORMAT_S8_000R, extra_w, extra_h, false))
        return false;

    // secondly, correct its size and the format
    void *data = vita2d_texture_get_datap(*p_tex);
    sceGxmTextureInitLinear(&(*p_tex)->gxm_tex, data,
                            SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC0,
                            rounded_w, rounded_h, 0);
    do_set_texture_filters(*p_tex);
    return true;
}

static bool render_texture_init(struct ui_context *ctx, struct ui_texture **tex,
                                enum ui_texure_fmt fmt, int w, int h, bool dr)
{
    *tex = NULL;
    vita2d_texture **p_tex = (vita2d_texture**) tex;
    const struct texture_fmt_spec *spec = &texture_format_list[fmt];
    int rounded_w = FFALIGN(w, spec->init_align_w);
    int rounded_h = FFALIGN(h, spec->init_align_h);
    switch (fmt) {
    case TEX_FMT_RGBA:
        return do_init_texture(p_tex, spec, 0, rounded_w, rounded_h, dr);
    case TEX_FMT_YUV420:
        return dr
            ? do_init_texture(p_tex, spec, 0, rounded_w, rounded_h, true)
            : do_init_yuv420p3(p_tex, rounded_w, rounded_h);
    case TEX_FMT_UNKNOWN:
        return false;
    }
    return false;
}

static void render_texture_uninit(struct ui_context *ctx, struct ui_texture **tex)
{
    vita2d_texture **p_tex = (vita2d_texture**) tex;
    vita2d_free_texture(*p_tex);
    *p_tex = NULL;
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

static void render_font_measure(struct ui_context *ctx, struct ui_font *font,
                                const char* text, int size, int *w, int *h)
{
    get_priv_render(ctx)->font_impl->measure(font, text, size, w, h);
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

static void render_texture_upload(struct ui_context *ctx, struct ui_texture *tex,
                                  struct ui_texture_data_args *args)
{
    vita2d_texture *impl = (vita2d_texture*) tex;
    SceGxmTextureFormat vita_fmt = vita2d_texture_get_format(impl);
    enum AVPixelFormat ff_fmt = get_ff_format(vita_fmt, args->planes);
    if (ff_fmt == AV_PIX_FMT_NONE)
        return;

    int dst_strides[4];
    uint8_t *dst_data[4];
    void *tex_data = vita2d_texture_get_datap(impl);
    int tex_w = vita2d_texture_get_width(impl);
    int tex_h = vita2d_texture_get_height(impl);
    av_image_fill_arrays(dst_data, dst_strides, tex_data, ff_fmt, tex_w, tex_h, 1);
    av_image_copy(dst_data, dst_strides, args->data, args->strides,
                  ff_fmt, args->width, args->height);
}

static bool render_texture_attach(struct ui_context *ctx, struct ui_texture *tex,
                                  struct ui_texture_data_args *args)
{
    const void *vram = args->data[0];
    struct dr_texture *impl = (struct dr_texture*) tex;
    struct priv_render *priv = get_priv_render(ctx);
    struct vram_entry *ve = find_vram_entry(priv, vram, NULL);
    if (!ve)
        return false;

    int ret = sceGxmTextureInitLinear(&impl->tex.gxm_tex, vram,
                                      impl->spec->sce_fmt, impl->w, impl->h, 0);
    if (ret != SCE_OK)
        return false;

    do_set_texture_filters(&impl->tex);
    return true;
}

static void render_texture_detach(struct ui_context *ctx, struct ui_texture *tex)
{}

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

static bool render_draw_vertices_prepare(struct ui_context *ctx,
                                         struct ui_color_vertex **verts, int n)
{
    size_t elem_size = sizeof(struct ui_color_vertex);
    struct ui_color_vertex *base = vita2d_pool_memalign(elem_size * n, elem_size);
    if (!base)
        return false;

    *verts = base;
    return true;
}

static void render_draw_vertices_copy(struct ui_context *ctx,
                                      struct ui_color_vertex *verts,
                                      int dst, int src)
{
    memcpy(&verts[dst], &verts[src], sizeof(struct ui_color_vertex));
}

static void render_draw_vertices_compose(struct ui_context *ctx,
                                         struct ui_color_vertex *verts,
                                         int i, float x, float y, ui_color color)
{
    verts[i].impl = (vita2d_color_vertex) {
        .x = x,
        .y = y,
        .z = 0.5,
        .color = color,
    };
}

static void render_draw_vertices_commit(struct ui_context *ctx,
                                        struct ui_color_vertex *verts, int n)
{
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, &verts->impl, n);
}

static const struct font_impl font_impl_pgf = {
    .init = font_impl_pgf_init,
    .uninit = font_impl_pgf_uninit,
    .draw = font_impl_pgf_draw,
    .measure = font_impl_pgf_measure,
};

static const struct font_impl font_impl_freetype = {
    .init = font_impl_ft_init,
    .uninit = font_impl_ft_uninit,
    .draw = font_impl_ft_draw,
    .measure = font_impl_ft_measure,
};

const struct ui_render_driver ui_render_driver_vita = {
    .priv_size = sizeof(struct priv_render),

    .init = render_init,
    .uninit = render_uninit,

    .render_start = render_render_start,
    .render_end = render_render_end,

    .dr_align = render_dr_align,
    .dr_prepare = render_dr_prepare,
    .dr_vram_init = render_dr_vram_init,
    .dr_vram_uninit = render_dr_vram_uninit,
    .dr_vram_lock = render_dr_vram_lock,
    .dr_vram_unlock = render_dr_vram_unlock,

    .texture_init = render_texture_init,
    .texture_uninit = render_texture_uninit,
    .texture_upload = render_texture_upload,
    .texture_attach = render_texture_attach,
    .texture_detach = render_texture_detach,

    .font_init = render_font_init,
    .font_uninit = render_font_uninit,
    .font_measure = render_font_measure,

    .clip_start = render_clip_start,
    .clip_end = render_clip_end,

    .draw_font = render_draw_font,
    .draw_texture = render_draw_texture,
    .draw_vertices_prepare = render_draw_vertices_prepare,
    .draw_vertices_copy = render_draw_vertices_copy,
    .draw_vertices_compose = render_draw_vertices_compose,
    .draw_vertices_commit = render_draw_vertices_commit,
};
