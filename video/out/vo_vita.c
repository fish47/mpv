#include "vo.h"
#include "sub/osd.h"
#include "osdep/vita/ui_context.h"
#include "osdep/vita/ui_driver.h"
#include "osdep/vita/ui_panel.h"
#include "video/mp_image.h"
#include "video/img_format.h"
#include "video/fmt-conversion.h"

#include <libavutil/imgutils.h>

enum render_act {
    RENDER_ACT_INIT,
    RENDER_ACT_REDRAW,
    RENDER_ACT_TEX_INIT,
    RENDER_ACT_TEX_UPDATE,
    RENDER_ACT_MAX,
};

struct init_tex_data {
    struct mp_rect src;
    struct mp_rect dst;
    struct ui_context *ctx;
};

struct update_tex_data {
    struct ui_context *ctx;
    struct mp_image *image;
};

struct priv_draw {
    struct ui_texture *video_tex;
    struct mp_rect video_src_rect;
    struct mp_rect video_dst_rect;

    bool dr_enabled;
    struct mp_image *dr_image_locked;
    struct mp_image *dr_image_new;
};

struct priv_vo {
    bool enable_dr;
    void *cb_data_slots[RENDER_ACT_MAX];
};

static ui_panel_run_fn get_render_act_fn(enum render_act act);

static struct ui_context *get_ui_context(struct vo *vo)
{
    // this pointer is passed as option value before MPV initialization
    // it should be constant and valid during vo_driver's lifetime
    uintptr_t addr = (uintptr_t) vo->opts->WinID;
    return (struct ui_context*) addr;
}

static void free_locked_dr_image(struct ui_context *ctx, struct priv_draw *priv)
{
    if (!priv->dr_image_locked)
        return;

    if (priv->video_tex)
        ui_render_driver_vita.texture_detach(ctx, priv->video_tex);
    ui_render_driver_vita.dr_vram_unlock(ctx, priv->dr_image_locked->bufs[0]->data);
    TA_FREEP(&priv->dr_image_locked);
}

static void free_texture_and_images(struct ui_context *ctx, struct priv_draw *priv)
{
    TA_FREEP(&priv->dr_image_new);
    free_locked_dr_image(ctx, priv);

    if (priv->video_tex)
        ui_render_driver_vita.texture_uninit(ctx, &priv->video_tex);
}

static void render_act_do_modify(struct vo *vo, enum render_act act,
                                 void *data, bool steal)
{
    ui_panel_run_fn func = get_render_act_fn(act);
    if (!func)
        return;

    // cancel pending action
    struct priv_vo *priv = vo->priv;
    struct ui_context *ctx = get_ui_context(vo);
    ui_panel_common_run_cancel(ctx, func, priv->cb_data_slots[act]);

    // enqueue new action
    priv->cb_data_slots[act] = data;
    if (data) {
        if (steal)
            ui_panel_common_run_post_steal(ctx, func, data);
        else
            ui_panel_common_run_post(ctx, func, data);
    }
}

static void render_act_post_ref(struct vo *vo, enum render_act act, void *data)
{
    render_act_do_modify(vo, act, data, false);
}

static void render_act_post_steal(struct vo *vo, enum render_act act, void *data)
{
    render_act_do_modify(vo, act, data, true);
}

static void render_act_remove(struct vo *vo, enum render_act act)
{
    render_act_do_modify(vo, act, NULL, false);
}

static enum ui_texure_fmt resolve_tex_fmt(int fmt)
{
    switch (fmt) {
    case IMGFMT_RGBA:
        return TEX_FMT_RGBA;
    case IMGFMT_420P:
        return TEX_FMT_YUV420;
    default:
        return TEX_FMT_UNKNOWN;
    }
}

static int query_format(struct vo *vo, int format)
{
    return resolve_tex_fmt(format) != TEX_FMT_UNKNOWN;
}

static struct ui_texture_data_args get_texture_data_args(struct mp_image *img)
{
    return (struct ui_texture_data_args) {
        .width = img->w,
        .height = img->h,
        .planes = img->num_planes,
        .data = (const uint8_t**) img->planes,
        .strides = (const int*) img->stride,
    };
}

static void swap_locked_dr_image(struct ui_context *ctx, struct priv_draw *priv)
{
    if (!priv->dr_image_new)
        return;

    void *vram = priv->dr_image_new->bufs[0]->data;
    free_locked_dr_image(ctx, priv);
    MPSWAP(struct mp_image*, priv->dr_image_locked, priv->dr_image_new);
    ui_render_driver_vita.dr_vram_lock(ctx, vram);

    struct ui_texture_data_args args = get_texture_data_args(priv->dr_image_locked);
    if (!ui_render_driver_vita.texture_attach(ctx, priv->video_tex, &args))
        free_locked_dr_image(ctx, priv);
}

static void do_panel_draw(struct ui_context *ctx, void *p)
{
    struct priv_draw *priv = p;
    if (!priv->video_tex)
        return;

    swap_locked_dr_image(ctx, priv);
    if (priv->dr_enabled && !priv->dr_image_locked)
        return;

    struct ui_texture_draw_args args = {
        .src = &priv->video_src_rect,
        .dst = &priv->video_dst_rect,
    };
    ui_render_driver_vita.draw_texture(ctx, priv->video_tex, &args);
}

static void do_render_init_vo_driver(void *p)
{
    struct ui_context *ctx = p;
    struct priv_draw *priv = talloc_zero_size(ctx, sizeof(*priv));
    ui_panel_player_set_vo_draw_data(ctx, priv);
    ui_panel_player_set_vo_draw_fn(ctx, do_panel_draw);
}

static int preinit(struct vo *vo)
{
    struct priv_vo *priv = vo->priv;
    memset(priv->cb_data_slots, 0, sizeof(priv->cb_data_slots));
    render_act_post_ref(vo, RENDER_ACT_INIT, get_ui_context(vo));
    return 0;
}

static void do_uninit_priv_draw(void *p)
{
    struct ui_context *ctx = p;
    struct priv_draw *priv = ui_panel_player_get_vo_draw_data(ctx);
    if (priv)
        free_texture_and_images(ctx, priv);
}

static void uninit(struct vo *vo)
{
    for (int i = 0; i < RENDER_ACT_MAX; ++i)
        render_act_remove(vo, i);

    struct ui_context *ctx = get_ui_context(vo);
    ui_panel_common_run_sync(ctx, do_uninit_priv_draw, ctx);
}

static void do_render_redraw(void *p)
{
    ui_panel_common_invalidate(p);
}

static void flip_page(struct vo *vo)
{
    render_act_post_ref(vo, RENDER_ACT_REDRAW, get_ui_context(vo));
}

static void do_render_init_texture(void *p)
{
    struct init_tex_data *data = p;
    struct priv_draw *priv = ui_panel_player_get_vo_draw_data(data->ctx);
    if(!priv)
        return;

    priv->video_src_rect = data->src;
    priv->video_dst_rect = data->dst;
    free_texture_and_images(data->ctx, priv);
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    // screen size will not change
    vo->dwidth = VITA_SCREEN_W;
    vo->dheight = VITA_SCREEN_H;

    // calculate video texture placement
    struct mp_rect src;
    struct mp_rect dst;
    struct mp_osd_res osd;
    vo_get_src_dst_rects(vo, &src, &dst, &osd);

    struct init_tex_data *data = ta_new_ptrtype(vo->priv, data);
    *data = (struct init_tex_data) {
        .ctx = get_ui_context(vo),
        .src = src,
        .dst = dst,
    };
    render_act_remove(vo, RENDER_ACT_TEX_UPDATE);
    render_act_post_steal(vo, RENDER_ACT_TEX_INIT, data);

    return 0;
}

static void do_render_update_texture(void *p)
{
    struct update_tex_data *data = p;
    struct priv_draw *priv = ui_panel_player_get_vo_draw_data(data->ctx);
    if (!priv)
        return;

    // destroy the texture if dr state is changed
    struct mp_image *img = data->image;
    bool is_dr_img = ((img->fields & MP_IMGFIELD_DR_FRAME) != 0);
    if (priv->dr_enabled != is_dr_img) {
        priv->dr_enabled = is_dr_img;
        free_texture_and_images(data->ctx, priv);
    }

    // create the corresponding texture if missing
    if (!priv->video_tex) {
        enum ui_texure_fmt fmt = resolve_tex_fmt(img->imgfmt);
        ui_render_driver_vita.texture_init(
            data->ctx, &priv->video_tex,
            fmt, img->w, img->h, priv->dr_enabled);
    }

    if (!priv->video_tex)
        return;

    if (priv->dr_enabled) {
        TA_FREEP(&priv->dr_image_new);
        priv->dr_image_new = ta_steal(priv, img);
    } else {
        struct ui_texture_data_args args = get_texture_data_args(img);
        ui_render_driver_vita.texture_upload(data->ctx, priv->video_tex, &args);
    }
}

static void draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct mp_image *image = mp_image_new_ref(frame->current);
    struct update_tex_data *data = ta_new_ptrtype(vo->priv, data);
    *data = (struct update_tex_data) {
        .ctx = get_ui_context(vo),
        .image = ta_steal(data, image),
    };
    render_act_post_steal(vo, RENDER_ACT_TEX_UPDATE, data);
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    if (request == VOCTRL_PREPARE_DR_DECODER) {
        void **pack = data;
        const AVCodec *codec = pack[0];
        AVDictionary **opts = pack[1];
        struct priv_vo *priv = vo->priv;
        struct ui_context *ctx = get_ui_context(vo);
        priv->enable_dr = ui_render_driver_vita.dr_prepare(ctx, codec, opts);
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

static ui_panel_run_fn get_render_act_fn(enum render_act act)
{
    switch (act) {
    case RENDER_ACT_INIT:
        return do_render_init_vo_driver;
    case RENDER_ACT_REDRAW:
        return do_render_redraw;
    case RENDER_ACT_TEX_INIT:
        return do_render_init_texture;
    case RENDER_ACT_TEX_UPDATE:
        return do_render_update_texture;
    case RENDER_ACT_MAX:
        return NULL;
    }
    return NULL;
}

static void do_free_vram(void *opaque, uint8_t *data)
{
    void *vram = data;
    struct ui_context *ctx = opaque;
    ui_render_driver_vita.dr_vram_uninit(ctx, &vram);
}

static struct mp_image *do_alloc_dr_image(struct vo *vo, int fmt, int w, int h)
{
    void *vram = NULL;
    struct mp_image *mpi = NULL;
    struct ui_context *ctx = get_ui_context(vo);

    int tex_fmt = resolve_tex_fmt(fmt);
    int rounded_w = w;
    int rounded_h = h;
    int vram_size = ui_render_driver_vita.dr_align(tex_fmt, &rounded_w, &rounded_h);
    if (!vram_size)
        goto fail;

    if (!ui_render_driver_vita.dr_vram_init(ctx, vram_size, &vram))
        goto fail;

    mpi = mp_image_new_dummy_ref(NULL);
    if (!mpi)
        goto fail;

    mpi->bufs[0] = av_buffer_create(vram, vram_size, do_free_vram, ctx, 0);
    if (!mpi->bufs[0])
        goto fail;

    mp_image_set_size(mpi, w, h);
    mp_image_setfmt(mpi, fmt);
    av_image_fill_arrays(mpi->planes, mpi->stride, vram,
                         imgfmt2pixfmt(fmt), rounded_w, rounded_h, 1);

    return mpi;

fail:
    if (vram)
        ui_render_driver_vita.dr_vram_uninit(ctx, &vram);
    if (mpi)
        ta_free(mpi);
    return NULL;
}

struct mp_image *get_image(struct vo *vo, int imgfmt, int w, int h, int stride_align)
{
    struct priv_vo *priv = vo->priv;
    if (priv->enable_dr)
        return do_alloc_dr_image(vo, imgfmt, w, h);
    else
        return NULL;
}

const struct vo_driver video_out_vita = {
    .description = "Vita video output",
    .priv_size = sizeof(struct priv_vo),
    .name = "vita",
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .get_image_ts = get_image,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .uninit = uninit,
};
