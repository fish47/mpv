#include "vo.h"
#include "sub/osd.h"
#include "input/input.h"
#include "input/keycodes.h"
#include "osdep/vita/ui_context.h"
#include "osdep/vita/ui_device.h"
#include "osdep/vita/ui_driver.h"
#include "osdep/vita/ui_panel.h"
#include "video/mp_image.h"
#include "video/img_format.h"

#define INVALID_KEY_CODE (-1)

enum render_act {
    RENDER_ACT_INIT,
    RENDER_ACT_REDRAW,
    RENDER_ACT_TEX_INIT,
    RENDER_ACT_TEX_UPDATE,
    RENDER_ACT_MAX,
};

struct init_vo_data {
    struct ui_context *ctx;
    struct input_ctx *input;
};

struct init_tex_data {
    int w;
    int h;
    struct mp_rect src;
    struct mp_rect dst;
    enum ui_texure_fmt fmt;
    struct ui_context *ctx;
};

struct update_tex_data {
    struct ui_context *ctx;
    struct mp_image *image;
};

struct priv_panel {
    struct input_ctx *input_ctx;
    struct ui_texture *video_tex;
    struct mp_rect video_src_rect;
    struct mp_rect video_dst_rect;
};

struct priv_vo {
    void *cb_data_slots[RENDER_ACT_MAX];
};

static mp_dispatch_fn get_render_act_fn(enum render_act act);

static struct ui_context *get_ui_context(struct vo *vo)
{
    // this pointer is passed as option value before MPV initialization
    // it should be valid in vo_driver's whole lifetime
    return (struct ui_context*) vo->opts->WinID;
}

static void render_act_do_modify(struct vo *vo, enum render_act act, void *data, bool steal)
{
    mp_dispatch_fn func = get_render_act_fn(act);
    if (!func)
        return;

    // cancel pending action
    struct priv_vo *priv = vo->priv;
    struct ui_context *ctx = get_ui_context(vo);
    mp_dispatch_cancel_fn(ctx->dispatch, func, priv->cb_data_slots[act]);

    // enqueue new action
    priv->cb_data_slots[act] = data;
    if (data) {
        if (steal)
            mp_dispatch_enqueue_autofree(ctx->dispatch, func, data);
        else
            mp_dispatch_enqueue(ctx->dispatch, func, data);
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

static void do_panel_draw(struct ui_context *ctx)
{
    struct priv_panel *priv = ui_panel_player_get_vo_data(ctx);
    if (!priv)
        return;

    if (priv->video_tex) {
        struct ui_texture_draw_args args = {
            .src = &priv->video_src_rect,
            .dst = &priv->video_dst_rect,
        };
        ui_render_driver_vita.draw_texture(ctx, priv->video_tex, &args);
    }
}

static void do_panel_uninit(struct ui_context *ctx)
{
    struct priv_panel *priv = ui_panel_player_get_vo_data(ctx);
    if (!priv)
        return;

    if (priv->video_tex)
        ui_render_driver_vita.texture_uninit(ctx, &priv->video_tex);
}

static int resolve_mp_key_code(enum ui_key_code key)
{
    switch (key) {
    case UI_KEY_CODE_VITA_DPAD_LEFT:
        return MP_KEY_GAMEPAD_DPAD_LEFT;
    case UI_KEY_CODE_VITA_DPAD_RIGHT:
        return MP_KEY_GAMEPAD_DPAD_RIGHT;
    case UI_KEY_CODE_VITA_DPAD_UP:
        return MP_KEY_GAMEPAD_DPAD_UP;
    case UI_KEY_CODE_VITA_DPAD_DOWN:
        return MP_KEY_GAMEPAD_DPAD_DOWN;
    case UI_KEY_CODE_VITA_ACTION_SQUARE:
        return MP_KEY_GAMEPAD_ACTION_LEFT;
    case UI_KEY_CODE_VITA_ACTION_CIRCLE:
        return MP_KEY_GAMEPAD_ACTION_RIGHT;
    case UI_KEY_CODE_VITA_ACTION_TRIANGLE:
        return MP_KEY_GAMEPAD_ACTION_UP;
    case UI_KEY_CODE_VITA_ACTION_CROSS:
        return MP_KEY_GAMEPAD_ACTION_DOWN;
    case UI_KEY_CODE_VITA_L1:
        return MP_KEY_GAMEPAD_LEFT_SHOULDER;
    case UI_KEY_CODE_VITA_R1:
        return MP_KEY_GAMEPAD_RIGHT_SHOULDER;
    case UI_KEY_CODE_VITA_START:
        return MP_KEY_GAMEPAD_START;
    case UI_KEY_CODE_VITA_SELECT:
        return MP_KEY_GAMEPAD_MENU;
    case UI_KEY_CODE_VITA_END:
        break;
    }
    return INVALID_KEY_CODE;
}

static int resolve_mp_key_state(enum ui_key_state state)
{
    switch (state) {
    case UI_KEY_STATE_DOWN:
        return MP_KEY_STATE_DOWN;
    case UI_KEY_STATE_UP:
        return MP_KEY_STATE_UP;
    }
    return 0;
}

static void do_panel_send_key(struct ui_context *ctx, enum ui_key_code key, enum ui_key_state state)
{
    struct priv_panel *priv = ui_panel_player_get_vo_data(ctx);
    if (!priv)
        return;

    int code_bits = resolve_mp_key_code(key);
    if (code_bits == INVALID_KEY_CODE)
        return;

    // input_ctx is thread-safed, it should be fine to use it duraing its lifetime
    // if mpv or vo is destroying, main thread will be blocked, this function will not be called anymore
    int state_bits = resolve_mp_key_state(state);
    mp_input_put_key(priv->input_ctx, (code_bits | state_bits));
}

static void do_render_init_vo_driver(void *p)
{
    struct init_vo_data *data = p;
    struct ui_context *ctx = data->ctx;
    struct priv_panel *priv = talloc_zero_size(ctx, sizeof(*priv));
    priv->input_ctx = data->input;
    ui_panel_player_set_vo_data(ctx, priv);

    struct ui_panel_player_vo_fns fns = {
        .draw = do_panel_draw,
        .uninit = do_panel_uninit,
        .send_key = do_panel_send_key,
    };
    ui_panel_player_set_vo_fns(ctx, &fns);
}

static int preinit(struct vo *vo)
{
    struct priv_vo *priv = vo->priv;
    memset(priv->cb_data_slots, 0, sizeof(priv->cb_data_slots));

    struct init_vo_data *data = ta_new_ptrtype(priv, data);
    *data = (struct init_vo_data) {
        .ctx = get_ui_context(vo),
        .input = vo->input_ctx,
    };
    render_act_post_ref(vo, RENDER_ACT_INIT, data);
    return 0;
}

static void uninit(struct vo *vo)
{
    for (int i = 0; i < RENDER_ACT_MAX; ++i)
        render_act_remove(vo, i);
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
    // reinit video texture
    struct init_tex_data *data = p;
    struct priv_panel *priv = ui_panel_player_get_vo_data(data->ctx);
    if(!priv)
        return;

    if (priv->video_tex)
        ui_render_driver_vita.texture_uninit(data->ctx, &priv->video_tex);
    ui_render_driver_vita.texture_init(data->ctx, &priv->video_tex, data->fmt, data->w, data->h);

    // save placement
    priv->video_src_rect = data->src;
    priv->video_dst_rect = data->dst;
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
        .w = params->w,
        .h = params->h,
        .src = src,
        .dst = dst,
        .fmt = resolve_tex_fmt(params->imgfmt),
    };
    render_act_remove(vo, RENDER_ACT_TEX_UPDATE);
    render_act_post_steal(vo, RENDER_ACT_TEX_INIT, data);

    return 0;
}

static void do_render_update_texture(void *p)
{
    struct update_tex_data *data = p;
    struct priv_panel *priv = ui_panel_player_get_vo_data(data->ctx);
    if (!priv)
        return;

    struct mp_image *image = data->image;
    void *planes[MP_MAX_PLANES];
    for (int i = 0; i < MP_MAX_PLANES; ++i)
        planes[i] = image->planes[i];
    ui_render_driver_vita.texture_upload(data->ctx, priv->video_tex, planes, image->stride, image->num_planes);
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
    return VO_NOTIMPL;
}

static mp_dispatch_fn get_render_act_fn(enum render_act act)
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

const struct vo_driver video_out_vita = {
    .description = "Vita video output",
    .priv_size = sizeof(struct priv_vo),
    .name = "vita",
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .uninit = uninit,
};
