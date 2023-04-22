#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <common/common.h>

struct ui_font;
struct ui_texture;
struct ui_context;

enum ui_texure_fmt {
    TEX_FMT_UNKNOWN,
    TEX_FMT_RGBA,
    TEX_FMT_YUV420,
};

struct ui_font_draw_args {
    const char *text;
    int size;
    int x;
    int y;
    unsigned int color;
};

struct ui_texture_draw_args {
    struct mp_rect *src;
    struct mp_rect *dst;
};

struct ui_rectangle_draw_args {
    struct mp_rect *rects;
    unsigned int color;
    int count;
};

struct ui_platform_driver {
    int priv_size;
    bool (*init)(struct ui_context *ctx, int argc, char *argv[]);
    void (*uninit)(struct ui_context *ctx);
    void (*exit)();
    void (*poll_events)(struct ui_context *ctx);
    uint32_t (*poll_keys)(struct ui_context *ctx);
    const char* (*get_font_path)(struct ui_context *ctx);
    const char* (*get_files_dir)(struct ui_context *ctx);
};

struct ui_audio_driver {
    int buffer_count;
    bool (*init)(void **ctx, int samples, int freq, int channels);
    void (*uninit)(void **ctx);
    int (*output)(void *ctx, void *buff);
};

struct ui_render_driver {
    int priv_size;

    bool (*init)(struct ui_context *ctx);
    void (*uninit)(struct ui_context *ctx);

    void (*render_start)(struct ui_context *ctx);
    void (*render_end)(struct ui_context *ctx);

    bool (*texture_init)(struct ui_context *ctx, struct ui_texture **tex,
                         enum ui_texure_fmt fmt, int w, int h);
    void (*texture_uninit)(struct ui_context *ctx, struct ui_texture **tex);
    void (*texture_upload)(struct ui_context *ctx, struct ui_texture *tex,
                           void **data, int *strides, int planes);

    bool (*font_init)(struct ui_context *ctx, struct ui_font **font, const char *path);
    void (*font_uninit)(struct ui_context *ctx, struct ui_font **font);

    void (*clip_start)(struct ui_context *ctx, struct mp_rect *rect);
    void (*clip_end)(struct ui_context *ctx);

    void (*draw_font)(struct ui_context *ctx, struct ui_font *font,
                      struct ui_font_draw_args *args);
    void (*draw_texture)(struct ui_context *ctx, struct ui_texture *tex,
                         struct ui_texture_draw_args *args);
    void (*draw_rectangle)(struct ui_context *ctx, struct ui_rectangle_draw_args *args);
};

extern const struct ui_platform_driver ui_platform_driver_vita;
extern const struct ui_audio_driver ui_audio_driver_vita;
extern const struct ui_render_driver ui_render_driver_vita;
