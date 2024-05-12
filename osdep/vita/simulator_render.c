#include "emulator.h"
#include "ui_device.h"
#include "ui_driver.h"
#include "shape_draw.h"
#include "misc/linked_list.h"
#include "common/common.h"
#include "video/img_format.h"

#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
#include <libavcodec/codec.h>
#include <libavcodec/avcodec.h>

#include <freetype2/ft2build.h>
#include FT_CACHE_H
#include FT_FREETYPE_H

#define PRIV_BUFFER_SIZE        (8 * 1024 * 1024)
#define DECODE_BUFFER_SIZE      (4 * 1024)

#define TEX_FMT_INTERNAL_A8     (1000)

struct gl_attr_spec {
    const char *name;
    int pos;
};

struct gl_uniform_spec {
    const char *name;
    int *output;
};

static const char *const shader_source_vert_texture =
    "uniform vec2 u_offset;"
    "uniform mat4 u_transform;"
    "attribute vec4 a_draw_pos;"
    "attribute vec2 a_texture_pos;"
    "varying vec2 v_texture_pos;"
    "void main() {"
    "    gl_Position = (a_draw_pos + vec4(u_offset, 0, 0)) * u_transform;"
    "    v_texture_pos = a_texture_pos;"
    "}";

static const struct gl_attr_spec attr_draw_tex_pos_draw = { .name = "a_draw_pos", .pos = 0, };
static const struct gl_attr_spec attr_draw_tex_pos_tex = { .name = "a_texture_pos", .pos = 1, };

static const char *uniform_draw_tex_tint = "u_tint";
static const char *uniform_draw_tex_offset = "u_offset";
static const char *uniform_draw_tex_transform = "u_transform";

static const char *const shader_source_vert_triangle =
    "uniform mat4 u_transform;"
    "attribute vec4 a_draw_pos;"
    "attribute vec4 a_draw_color;"
    "varying vec4 v_color;"
    "void main() {"
    "    gl_Position = a_draw_pos * u_transform;"
    "    v_color = a_draw_color;"
    "}";

static const struct gl_attr_spec attr_draw_triangle_pos = { .name = "a_draw_pos", .pos = 0 };
static const struct gl_attr_spec attr_draw_triangle_color = { .name = "a_draw_color", .pos = 1 };

static const char *uniform_draw_triangle_transform = "u_transform";

static const char *const shader_source_frag_triangle =
    "precision mediump float;"
    "varying vec4 v_color;"
    "void main() {"
    "    gl_FragColor = v_color;"
    "}";

struct gl_tex_plane_spec {
    int bpp;
    int div;
    GLenum fmt;
    GLenum type;
    const char *name;
};

struct gl_tex_impl_spec {
    enum AVPixelFormat ff_format;
    int pixel_bits;
    int align_w;
    int align_h;
    int num_planes;
    const struct gl_tex_plane_spec *plane_specs;
    const char *shader_source_frag;
};

static const struct gl_tex_impl_spec tex_spec_unknown = {
    .ff_format = AV_PIX_FMT_NONE,
    .pixel_bits = 0,
    .align_w = 0,
    .align_h = 0,
    .num_planes = 0,
    .plane_specs = NULL,
    .shader_source_frag = NULL,
};

static const struct gl_tex_impl_spec tex_spec_a8 = {
    .ff_format = AV_PIX_FMT_GRAY8,
    .pixel_bits = 8,
    .align_w = 1,
    .align_h = 1,
    .num_planes = 1,
    .plane_specs = (const struct gl_tex_plane_spec[]) {
        { 1, 1, GL_ALPHA, GL_UNSIGNED_BYTE, "u_texture" },
    },
    .shader_source_frag =
        "precision mediump float;"
        "varying vec2 v_texture_pos;"
        "uniform sampler2D u_texture;"
        "uniform vec4 u_tint;"
        "void main() {"
        "    gl_FragColor = texture2D(u_texture, v_texture_pos).a * u_tint;"
        "}",
};

static const struct gl_tex_impl_spec tex_spec_rgba = {
    .ff_format = AV_PIX_FMT_RGBA,
    .pixel_bits = 32,
    .align_w = 1,
    .align_h = 1,
    .num_planes = 1,
    .plane_specs = (const struct gl_tex_plane_spec[]) {
        { 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, "u_texture" },
    },
    .shader_source_frag =
        "precision mediump float;"
        "varying vec2 v_texture_pos;"
        "uniform sampler2D u_texture;"
        "uniform vec4 u_tint;"
        "void main() {"
        "    gl_FragColor = texture2D(u_texture, v_texture_pos) * u_tint;"
        "}",
};

static const struct gl_tex_impl_spec tex_spec_yuv420 = {
    .ff_format = AV_PIX_FMT_YUV420P,
    .pixel_bits = 12,
    .align_w = 2,
    .align_h = 2,
    .num_planes = 3,
    .plane_specs = (const struct gl_tex_plane_spec[]) {
        { 1, 1, GL_ALPHA, GL_UNSIGNED_BYTE, "u_texture_y" },
        { 1, 2, GL_ALPHA, GL_UNSIGNED_BYTE, "u_texture_u" },
        { 1, 2, GL_ALPHA, GL_UNSIGNED_BYTE, "u_texture_v" },
    },
    .shader_source_frag =
        "precision mediump float;"
        "varying vec2 v_texture_pos;"
        "uniform sampler2D u_texture_y;"
        "uniform sampler2D u_texture_u;"
        "uniform sampler2D u_texture_v;"
        "uniform vec4 u_tint;"
        "const vec3 c_yuv_offset = vec3(-0.0627451017, -0.501960814, -0.501960814);"
        "const mat3 c_yuv_matrix = mat3("
        "    1.1644,  1.1644,   1.1644,"
        "    0,      -0.2132,   2.1124,"
        "    1.7927, -0.5329,   0"
        ");"
        "void main() {"
        "    mediump vec3 yuv = vec3("
        "        texture2D(u_texture_y, v_texture_pos).a,"
        "        texture2D(u_texture_u, v_texture_pos).a,"
        "        texture2D(u_texture_v, v_texture_pos).a"
        "    );"
        "    lowp vec3 rgb = c_yuv_matrix * (yuv + c_yuv_offset);"
        "    gl_FragColor = vec4(rgb, 1) * u_tint;"
        "}",
};

struct gl_uv_vertex {
    float vx;
    float vy;
    float ux;
    float uy;
};

struct gl_program_data {
    GLuint program;
    GLuint shader_vert;
    GLuint shader_frag;
};

struct gl_program_draw_tex {
    struct gl_program_data program_data;
    GLint uniform_textures[MP_MAX_PLANES];
    GLint uniform_tint;
    GLint uniform_offset;
    GLint uniform_transform;
};

struct gl_program_draw_triangle {
    struct gl_program_data program_data;
    GLint uniform_transform;
};

struct vram_header {
    bool locked;
};

union vram_header_aligned {
    struct vram_header header;
    void *align_ptr;
    char align_bytes[FFALIGN(sizeof(struct vram_header), 64)];
};

struct freetype_lib {
    FT_Library lib;
    FTC_Manager manager;
};

struct draw_font_cache_entry {
    int font_id;
    int font_size;
    const char *text;
};

struct draw_font_cache {
    GLuint tex;
    int tex_w;
    int tex_h;
    int draw_w;
    int draw_count;
    struct gl_uv_vertex *draw_buffer;
    struct draw_font_cache_entry entry;
    struct draw_font_cache *next;
};

struct priv_render {
    void *buffer;
    float normalize_matrix[16];
    struct gl_program_draw_tex program_draw_tex_a8;
    struct gl_program_draw_tex program_draw_tex_rgba;
    struct gl_program_draw_tex program_draw_tex_yuv420;
    struct gl_program_draw_triangle program_draw_triangle;

    int font_id;
    struct freetype_lib *ft_lib;
    struct draw_font_cache *font_cache_reused;
    struct draw_font_cache *font_cache_old;
};

struct ui_texture {
    GLuint ids[MP_MAX_PLANES];
    int w;
    int h;
    enum ui_texure_fmt fmt;
    bool dr;
    bool attached;
};

struct freetype_font_data {
    const char *file_path;
    int face_idx;
    FTC_Manager ftc_manager;
    FTC_CMapCache cmap_cache;
    FTC_ImageCache image_cache;
    struct {
        struct freetype_font_data *prev;
        struct freetype_font_data *next;
    } list;
};

struct ui_font {
    int font_id;
    struct {
        struct freetype_font_data *head;
        struct freetype_font_data *tail;
    } font_data;
};

struct ui_color_vertex {
    float x;
    float y;
    ui_color color;
};

struct uv_rect_vert_build_ctx {
    void *parent;
    struct gl_uv_vertex **buffer;
    struct mp_rect *verts;
    struct mp_rect *uvs;
    int tex_w;
    int tex_h;
    int rect_n;
};

static void font_cache_free_all(struct draw_font_cache **head);

static struct priv_render *get_priv_render(struct ui_context *ctx)
{
    return (struct priv_render*) ctx->priv_render;
}

static const struct gl_tex_impl_spec *get_gl_tex_impl_spec(enum ui_texure_fmt fmt)
{
    if (fmt == TEX_FMT_INTERNAL_A8)
        return &tex_spec_a8;

    switch (fmt) {
    case TEX_FMT_RGBA:
        return &tex_spec_rgba;
    case TEX_FMT_YUV420:
        return &tex_spec_yuv420;
    case TEX_FMT_UNKNOWN:
        return &tex_spec_unknown;
    }
    return &tex_spec_unknown;
}

static struct gl_program_draw_tex *get_gl_program_draw_tex(struct priv_render *priv, enum ui_texure_fmt fmt)
{
    if (fmt == TEX_FMT_INTERNAL_A8)
        return &priv->program_draw_tex_a8;

    switch (fmt) {
    case TEX_FMT_RGBA:
        return &priv->program_draw_tex_rgba;
    case TEX_FMT_YUV420:
        return &priv->program_draw_tex_yuv420;
    case TEX_FMT_UNKNOWN:
        return NULL;
    }
    return NULL;
}

static void delete_program(struct gl_program_data *program)
{
    if (program->program) {
        glDeleteProgram(program->program);
        program->program = 0;
    }
    if (program->shader_vert) {
        glDeleteShader(program->shader_vert);
        program->shader_vert = 0;
    }
    if (program->shader_frag) {
        glDeleteShader(program->shader_frag);
        program->shader_frag = 0;
    }
}

static bool load_shader(const char *source, GLenum type, GLuint *out_shader)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        glDeleteShader(shader);
        return false;
    }

    *out_shader = shader;
    return true;
}

static bool init_program(struct gl_program_data *program,
                         const char *vert_shader, const char *frag_shader,
                         const struct gl_attr_spec **attrs,
                         const struct gl_uniform_spec *uniforms)
{
    bool succeed = true;
    succeed &= load_shader(vert_shader, GL_VERTEX_SHADER, &program->shader_vert);
    succeed &= load_shader(frag_shader, GL_FRAGMENT_SHADER, &program->shader_frag);
    if (!succeed)
        goto error;

    program->program = glCreateProgram();
    glAttachShader(program->program, program->shader_vert);
    glAttachShader(program->program, program->shader_frag);
    for (int i = 0; attrs[i]; ++i)
        glBindAttribLocation(program->program, attrs[i]->pos, attrs[i]->name);
    glLinkProgram(program->program);

    GLint linked = 0;
    glGetProgramiv(program->program, GL_LINK_STATUS, &linked);
    if (!linked)
        goto error;

    for (int i = 0; uniforms[i].name; ++i) {
        GLint uniform = glGetUniformLocation(program->program, uniforms[i].name);
        if (uniform == -1)
            goto error;
        *(uniforms[i].output) = uniform;
    }
    return true;

error:
    delete_program(program);
    return false;
}

static bool init_program_tex(struct gl_program_draw_tex *program, const struct gl_tex_impl_spec *spec)
{
    struct gl_uniform_spec uniforms[3 + MP_MAX_PLANES + 1] = {
        { .name = uniform_draw_tex_tint, .output = &program->uniform_tint },
        { .name = uniform_draw_tex_offset, .output = &program->uniform_offset },
        { .name = uniform_draw_tex_transform, .output = &program->uniform_transform },
    };

    for (int i = 0; i < spec->num_planes; ++i) {
        uniforms[3 + i] = (struct gl_uniform_spec) {
            .name = spec->plane_specs[i].name,
            .output = &program->uniform_textures[i]
        };
    }

    const struct gl_attr_spec *attrs[] = {
        &attr_draw_tex_pos_draw,
        &attr_draw_tex_pos_tex,
        NULL,
    };
    return init_program(&program->program_data,
                        shader_source_vert_texture, spec->shader_source_frag,
                        attrs, uniforms);
}

static bool init_program_triangle(struct gl_program_draw_triangle *program)
{
    const struct gl_attr_spec *attrs[] = {
        &attr_draw_triangle_pos,
        &attr_draw_triangle_color,
        NULL,
    };
    struct gl_uniform_spec uniforms[2] = {
        { .name = uniform_draw_triangle_transform, .output = &program->uniform_transform },
    };
    return init_program(&program->program_data,
                        shader_source_vert_triangle, shader_source_frag_triangle,
                        attrs, uniforms);
}

static void make_normalize_matrix(float *matrix)
{
    float a = 2.0 / VITA_SCREEN_W;
    float b = -2.0 / VITA_SCREEN_H;
    float c = -1.0;
    float d = 1.0;
    float result[] = {
        a, 0, 0, c,
        0, b, 0, d,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    memcpy(matrix, result, sizeof(result));
}

static FT_Error ftc_request_cb(FTC_FaceID face_id, FT_Library lib,
                               FT_Pointer data, FT_Face *face)
{
    struct priv_render *priv = data;
    struct freetype_font_data *font_data = face_id;
    return FT_New_Face(lib, font_data->file_path, font_data->face_idx, face);
}

static void uninit_freetype(void *p)
{
    struct freetype_lib *lib = p;
    FTC_Manager_Done(lib->manager);
    FT_Done_FreeType(lib->lib);
}

static bool init_freetype(struct priv_render *priv)
{
    FT_Library lib = NULL;
    FTC_Manager ft_manager = NULL;
    if (FT_Init_FreeType(&lib) != 0)
        goto fail;
    if (FTC_Manager_New(lib, 0, 0, 0, &ftc_request_cb, priv, &ft_manager) != 0)
        goto fail;

    priv->ft_lib = ta_new_ptrtype(priv, priv->ft_lib);
    *priv->ft_lib = (struct freetype_lib) {
        .lib = lib,
        .manager = ft_manager,
    };
    ta_set_destructor(priv->ft_lib, uninit_freetype);
    return true;

fail:
    if (ft_manager)
        FTC_Manager_Done(ft_manager);
    if (lib)
        FT_Done_FreeType(lib);
    return false;
}

static bool render_init(struct ui_context *ctx)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    struct priv_render *priv = ctx->priv_render;
    memset(priv, 0, sizeof(struct priv_render));
    make_normalize_matrix(priv->normalize_matrix);
    return init_program_tex(&priv->program_draw_tex_a8, &tex_spec_a8)
        && init_program_tex(&priv->program_draw_tex_rgba, &tex_spec_rgba)
        && init_program_tex(&priv->program_draw_tex_yuv420, &tex_spec_yuv420)
        && init_program_triangle(&priv->program_draw_triangle)
        && (priv->buffer = ta_alloc_size(priv, PRIV_BUFFER_SIZE))
        && init_freetype(priv);
}

static void render_uninit(struct ui_context *ctx)
{
    struct priv_render *priv = ctx->priv_render;
    delete_program(&priv->program_draw_tex_a8.program_data);
    delete_program(&priv->program_draw_tex_rgba.program_data);
    delete_program(&priv->program_draw_tex_yuv420.program_data);
    delete_program(&priv->program_draw_triangle.program_data);
}

static void render_render_start(struct ui_context *ctx)
{
    struct priv_render *priv = ctx->priv_render;
    font_cache_free_all(&priv->font_cache_old);
    priv->font_cache_old = priv->font_cache_reused;
    priv->font_cache_reused = NULL;

    glViewport(0, 0, VITA_SCREEN_W, VITA_SCREEN_H);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void render_render_end(struct ui_context *ctx)
{
    struct priv_render *priv = ctx->priv_render;
    font_cache_free_all(&priv->font_cache_old);

    glfwSwapBuffers(emulator_get_platform_data(ctx)->window);
}

static int render_dr_align(enum ui_texure_fmt fmt, int *w, int *h)
{
    // just make sure the buffer is big enough to hold padded pixel data
    const struct gl_tex_impl_spec *spec = get_gl_tex_impl_spec(fmt);
    *w = FFALIGN(*w + 1, 32);
    *h = FFALIGN(*h + 1, 32);
    return (*w) * (*h) * spec->pixel_bits / 8;
}

static bool render_dr_prepare(struct ui_context *ctx,
                              const AVCodec *codec, AVDictionary **opts)
{
    return emulator_get_platform_data(ctx)->enable_dr;
}

static struct vram_header *get_vram_header(void *vram)
{
    union vram_header_aligned *p = vram;
    return &(p - 1)->header;
}

static void do_toggle_vram_lock_state(void *vram, bool locked)
{
    struct vram_header *header = get_vram_header(vram);
    assert(header->locked != locked);
    header->locked = locked;
}

static void uninit_vram(void *vram)
{
    struct vram_header *header = vram;
    assert(!header->locked);
}

static bool render_dr_vram_init(struct ui_context *ctx, int size, void **vram)
{
    size_t total_size = size + sizeof(union vram_header_aligned);
    union vram_header_aligned *header = ta_alloc_size(ctx->priv_render, total_size);
    if (!header)
        return false;

    ta_set_destructor(header, uninit_vram);
    header->header.locked = false;
    *vram = header + 1;
    return true;
}

static void render_dr_vram_uninit(struct ui_context *ctx, void **vram)
{
    struct vram_header *header = get_vram_header(*vram);
    assert(!header->locked);
    ta_free(header);
    *vram = NULL;
}

static void render_dr_vram_lock(struct ui_context *ctx, void *vram)
{
    do_toggle_vram_lock_state(vram, true);
}

static void render_dr_vram_unlock(struct ui_context *ctx, void *vram)
{
    do_toggle_vram_lock_state(vram, false);
}

static GLuint create_texture(GLsizei w, GLsizei h, GLenum fmt, GLenum type)
{
    GLuint tex_id;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, type, NULL);
    return tex_id;
}

static void uninit_texture(void *p)
{
    struct ui_texture *new_tex = p;
    if (new_tex->dr)
        assert(!new_tex->attached);
}

static bool render_texture_init(struct ui_context *ctx, struct ui_texture **tex,
                                enum ui_texure_fmt fmt, int w, int h, bool dr)
{
    struct ui_texture *new_tex = ta_new_ptrtype(ctx, new_tex);
    const struct gl_tex_impl_spec *spec = get_gl_tex_impl_spec(fmt);
    int rounded_w = FFALIGN(w, spec->align_w);
    int rounded_h = FFALIGN(h, spec->align_h);
    ta_set_destructor(new_tex, uninit_texture);
    new_tex->w = rounded_w;
    new_tex->h = rounded_h;
    new_tex->fmt = fmt;
    new_tex->dr = dr;
    new_tex->attached = false;

    for (int i = 0; i < spec->num_planes; ++i) {
        const struct gl_tex_plane_spec *plane = spec->plane_specs + i;
        int tex_w = rounded_w / plane->div;
        int tex_h = rounded_h / plane->div;
        new_tex->ids[i] = create_texture(tex_w, tex_h, plane->fmt, plane->type);
    }

    *tex = new_tex;
    return true;
}

static void render_texture_uninit(struct ui_context *ctx, struct ui_texture **tex)
{
    const struct gl_tex_impl_spec *spec = get_gl_tex_impl_spec((*tex)->fmt);
    if (spec->num_planes > 0)
        glDeleteTextures(spec->num_planes, (*tex)->ids);
    TA_FREEP(tex);
}

static void upload_texture_buffered(GLuint id, const void *data,
                                    int x, int y, int w, int h,
                                    int stride, int bpp,
                                    GLenum fmt, GLenum type,
                                    void *buffer, int capacity)
{
    glBindTexture(GL_TEXTURE_2D, id);

    int row = 0;
    int col = 0;
    int row_bytes = w * bpp;
    const uint8_t *cur = data;
    const uint8_t *next = cur + stride;
    while (row < h) {
        // caculate readable bytes in current row
        int available_bytes = capacity;
        int read_bytes = MPMIN((w - col) * bpp, available_bytes);
        int read_pixels = read_bytes / bpp;
        if (read_bytes == 0) {
            // unless the buffer is too small
            return;
        }

        // texture upload area
        int dst_x = col + x;
        int dst_y = row + y;
        int dst_w = read_pixels;
        int dst_h = 1;

        uint8_t *dst_p = buffer;
        memcpy(dst_p, cur, read_bytes);
        cur += read_bytes;
        col += read_pixels;
        dst_p += read_bytes;
        available_bytes -= read_bytes;

        if (col == w) {
            // swith to next row
            ++row;
            col = 0;
            cur = next;
            next += stride;

            // copy as many rows as we can
            if (dst_x == x) {
                int row_count = MPMIN((available_bytes / row_bytes), (h - row));
                for (int i = 0; i < row_count; ++i) {
                    memcpy(dst_p, cur, row_bytes);
                    ++row;
                    ++dst_h;
                    cur = next;
                    next += stride;
                    dst_p += row_bytes;
                }
            }
        }

        // finish current batch
        glTexSubImage2D(GL_TEXTURE_2D, 0, dst_x, dst_y, dst_w, dst_h, fmt, type, buffer);
    }
}

static void do_upload_tex_data(struct ui_context *ctx, struct ui_texture *tex,
                               struct ui_texture_data_args *args)
{
    const struct gl_tex_impl_spec *spec = get_gl_tex_impl_spec(tex->fmt);
    if (spec->num_planes != args->planes)
        return;

    struct priv_render *priv_render = ctx->priv_render;
    for (int i = 0; i < args->planes; ++i) {
        const struct gl_tex_plane_spec *plane = spec->plane_specs + i;
        int data_w = MPMIN(tex->w, args->width) / plane->div;
        int data_h = MPMIN(tex->h, args->height) / plane->div;
        upload_texture_buffered(tex->ids[i], args->data[i], 0, 0, data_w, data_h,
                                args->strides[i], plane->bpp, plane->fmt, plane->type,
                                priv_render->buffer, PRIV_BUFFER_SIZE);
    }
}

static bool do_drain_packet(AVCodecContext *avctx, AVFrame *frame, AVPacket *packet)
{
    int ret = avcodec_send_packet(avctx, packet);
    if (ret < 0)
        return false;

    while (true) {
        int ret = avcodec_receive_frame(avctx, frame);
        if (ret == 0)
            return true;
        else
            break;
    }
    return false;
}

static bool do_decode_image(struct ui_context *ctx,
                            const uint8_t *data,
                            int size, AVFrame *frame)
{
    AVPacket *packet = NULL;
    AVIOContext *avio = NULL;
    AVCodecContext *avctx = NULL;
    AVCodecParserContext *parser = NULL;

    // padding zero bytes are required by FFMPEG
    struct priv_render *priv = ctx->priv_render;
    uint8_t *buffer = priv->buffer;
    memset(buffer + DECODE_BUFFER_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    if (!codec)
        goto done;
    packet = av_packet_alloc();
    if (!packet)
        goto done;
    parser = av_parser_init(codec->id);
    if (!parser)
        goto done;
    avctx = avcodec_alloc_context3(codec);
    if (!avctx)
        goto done;
    int ret_open = avcodec_open2(avctx, codec, NULL);
    if (ret_open != 0)
        goto done;

    bool eof = false;
    bool found = false;
    int copied = 0;
    while (!eof && !found) {
        uint8_t *chunk_data = buffer;
        int chunk_size = MPCLAMP(size - copied, 0, DECODE_BUFFER_SIZE);
        if (chunk_size)
            memcpy(chunk_data, data + copied, chunk_size);
        else
            eof = true;
        copied += chunk_size;

        while (chunk_size || eof) {
            // 'zero-size' is a nice-to-have EOF signal
            int ret = av_parser_parse2(parser, avctx,
                                       &packet->data, &packet->size,
                                       chunk_data, chunk_size,
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0)
                goto done;

            chunk_data += ret;
            chunk_size -= ret;
            if (packet->size)
                found = do_drain_packet(avctx, frame, packet);
            else if (eof)
                break;

            if (found)
                goto done;
        }
    }
    // flush pending data
    found = do_drain_packet(avctx, frame, packet);

done:
    if (packet)
        av_packet_free(&packet);
    if (parser)
        av_parser_close(parser);
    if (ctx)
        avcodec_free_context(&avctx);
    return found;
}

static bool do_convert_image(AVFrame *frame, enum AVPixelFormat fmt)
{
    int ret = 0;
    bool succeed = false;
    AVFrame *result = NULL;
    struct SwsContext *sws = NULL;

    if (frame->format == fmt) {
        succeed = true;
        goto done;
    }

    sws = sws_alloc_context();
    av_opt_set_int(sws, "srcw", frame->width, 0);
    av_opt_set_int(sws, "srch", frame->height, 0);
    av_opt_set_int(sws, "src_format", frame->format, 0);
    av_opt_set_int(sws, "dstw", frame->width, 0);
    av_opt_set_int(sws, "dsth", frame->height, 0);
    av_opt_set_int(sws, "dst_format", fmt, 0);

    ret = sws_init_context(sws, NULL, NULL);
    if (ret < 0)
        goto done;

    result = av_frame_alloc();
    ret = sws_scale_frame(sws, result, frame);
    if (ret < 0)
        goto done;

    succeed = true;
    av_frame_unref(frame);
    av_frame_move_ref(frame, result);

done:
    if (sws)
        sws_freeContext(sws);
    if (result)
        av_frame_free(&result);
    return succeed;
}

static bool render_texture_decode(struct ui_context *ctx, struct ui_texture **tex,
                                  const void *data, int size, int *w, int *h)
{
    bool succeed = false;
    AVFrame frame = {0};
    av_frame_unref(&frame);

    bool decoded = do_decode_image(ctx, data, size, &frame);
    if (!decoded)
        goto done;

    bool converted = do_convert_image(&frame, AV_PIX_FMT_RGBA);
    if (!converted)
        goto done;

    bool created = render_texture_init(ctx, tex, TEX_FMT_RGBA,
                                       frame.width, frame.height, false);
    if (!created)
        goto done;

    succeed = true;
    struct ui_texture_data_args args = {
        .data = (const uint8_t**) frame.data,
        .strides = frame.linesize,
        .width = frame.width,
        .height = frame.height,
        .planes = 1,
    };
    do_upload_tex_data(ctx, *tex, &args);
    *w = frame.width;
    *h = frame.height;

done:
    av_frame_unref(&frame);
    return succeed;
}

static bool render_texture_attach(struct ui_context *ctx, struct ui_texture *tex,
                                  struct ui_texture_data_args *args)
{
    assert(tex->dr);
    tex->attached = true;
    do_upload_tex_data(ctx, tex, args);
    return true;
}

static void render_texture_detach(struct ui_context *ctx, struct ui_texture *tex)
{
    assert(tex->dr);
    tex->attached = false;
}

static void render_texture_upload(struct ui_context *ctx, struct ui_texture *tex,
                                  struct ui_texture_data_args *args)
{
    do_upload_tex_data(ctx, tex, args);
}

static void *normalize_to_vec4_color(float *base, ui_color color)
{
    // follow the color definition of vita2d
    float a = (float) ((color >> 24) & 0xff) / 0xff;
    float b = (float) ((color >> 16) & 0xff) / 0xff;
    float g = (float) ((color >> 8) & 0xff) / 0xff;
    float r = (float) ((color >> 0) & 0xff) / 0xff;
    base[0] = r;
    base[1] = g;
    base[2] = b;
    base[3] = a;
    return base;
}

static void do_render_draw_texture_ext(struct ui_context *ctx, struct ui_texture *tex,
                                       struct gl_uv_vertex *buffer, ui_color tint,
                                       int offset_x, int offset_y, int count)
{
    struct priv_render *priv = get_priv_render(ctx);
    struct gl_program_draw_tex *program = get_gl_program_draw_tex(priv, tex->fmt);
    if (!program)
        return;

    const struct gl_tex_impl_spec *spec = get_gl_tex_impl_spec(tex->fmt);
    if (!spec)
        return;

    int stride = sizeof(struct gl_uv_vertex);
    glUseProgram(program->program_data.program);
    glVertexAttribPointer(attr_draw_tex_pos_draw.pos, 2, GL_FLOAT, GL_FALSE, stride, &buffer->vx);
    glEnableVertexAttribArray(attr_draw_tex_pos_draw.pos);
    glVertexAttribPointer(attr_draw_tex_pos_tex.pos, 2, GL_FLOAT, GL_FALSE, stride, &buffer->ux);
    glEnableVertexAttribArray(attr_draw_tex_pos_tex.pos);

    for (int i = 0; i < spec->num_planes; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, tex->ids[i]);
        glUniform1i(program->uniform_textures[i], i);
    }

    float color_vec4[4];
    glUniform4fv(program->uniform_tint, 1, normalize_to_vec4_color(color_vec4, tint));
    glUniform2f(program->uniform_offset, offset_x, offset_y);
    glUniformMatrix4fv(program->uniform_transform, 1, GL_FALSE, priv->normalize_matrix);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, count);
}

static void vp_rect_spec_fn_write_textured(void *dst, void *src, bool lr, bool tb)
{
    void **args = src;
    struct mp_rect *rect = args[0];
    struct mp_rect *uv = args[1];
    int *tex_w = args[2];
    int *tex_h = args[3];

    struct gl_uv_vertex *result = dst;
    result->vx = lr ? rect->x0 : rect->x1;
    result->vy = tb ? rect->y0 : rect->y1;
    result->ux = (float) (lr ? uv->x0 : uv->x1) / *tex_w;
    result->uy = (float) (tb ? uv->y0 : uv->y1) / *tex_h;
}

static void do_build_uv_rect_buffer_dup(void *data)
{
    void **args = data;
    int *draw_n = args[0];
    struct uv_rect_vert_build_ctx *ctx = args[1];
    int last = *draw_n - 1;
    MP_TARRAY_APPEND(ctx->parent, *ctx->buffer, *draw_n, (*ctx->buffer)[last]);
}

static void do_build_uv_rect_buffer_write(void *data, int i, bool lr, bool tb)
{
    void **args = data;
    int *draw_n = args[0];
    struct uv_rect_vert_build_ctx *ctx = args[1];
    struct mp_rect *rect = &ctx->verts[i];
    struct mp_rect *uv = &ctx->uvs[i];
    MP_TARRAY_APPEND(ctx->parent, *ctx->buffer, *draw_n, (struct gl_uv_vertex) {
        .vx = lr ? rect->x0 : rect->x1,
        .vy = tb ? rect->y0 : rect->y1,
        .ux = (float) (lr ? uv->x0 : uv->x1) / ctx->tex_w,
        .uy = (float) (tb ? uv->y0 : uv->y1) / ctx->tex_h,
    });
}

static int build_uv_rect_buffer(struct uv_rect_vert_build_ctx ctx)
{
    int draw_n = 0;
    void *args[] = { &draw_n, &ctx };
    shape_draw_do_build_rect_verts(args, 0, ctx.rect_n,
                                   do_build_uv_rect_buffer_dup,
                                   do_build_uv_rect_buffer_write);
    return draw_n;
}

static void render_draw_texture(struct ui_context *ctx,
                                struct ui_texture *tex,
                                struct ui_texture_draw_args *args)
{
    struct priv_render *priv = get_priv_render(ctx);
    struct mp_rect uv_default = { .x0 = 0, .y0 = 0, .x1 = tex->w, .y1 = tex->h };
    int count = build_uv_rect_buffer((struct uv_rect_vert_build_ctx) {
        .parent = priv,
        .buffer = (struct gl_uv_vertex**) &priv->buffer,
        .verts = args->dst,
        .uvs = (args->src ? args->src : &uv_default),
        .tex_w = tex->w,
        .tex_h = tex->h,
        .rect_n = 1,
    });

    ui_color tint = args->tint ? *args->tint : -1;
    if (count)
        do_render_draw_texture_ext(ctx, tex, priv->buffer, tint, 0, 0, count);
}

static void font_cache_free_all(struct draw_font_cache **head)
{
    struct draw_font_cache *iter = *head;
    while (iter) {
        struct draw_font_cache *next = iter->next;
        ta_free(iter);
        iter = next;
    }
    *head = NULL;
}

static bool font_cache_entry_is_match(struct draw_font_cache_entry *lhs,
                                      struct draw_font_cache_entry *rhs)
{
    return lhs->font_id == rhs->font_id
        && lhs->font_size == rhs->font_size
        && strcmp(lhs->text, rhs->text) == 0;
}

static struct draw_font_cache *font_cache_find(struct draw_font_cache **head,
                                               struct draw_font_cache_entry *entry,
                                               bool remove)
{
    struct draw_font_cache *iter = *head;
    struct draw_font_cache *prev = NULL;
    while (iter) {
        if (font_cache_entry_is_match(&iter->entry, entry)) {
            if (remove) {
                struct draw_font_cache **pp_joint = prev ? &prev->next : head;
                *pp_joint = iter->next;
                iter->next = NULL;
            }
            return iter;
        }
        prev = iter;
        iter = iter->next;
    }
    return NULL;
}

static FT_Glyph do_find_glyph(FTC_Manager mgr,
                              struct freetype_font_data *data,
                              int size, int codepoint)
{
    FT_Face face = NULL;
    FTC_FaceID face_id = (FTC_FaceID) data;
    if (FTC_Manager_LookupFace(mgr, face_id, &face) != 0)
        return NULL;

    FT_Int cmap_idx = FT_Get_Charmap_Index(face->charmap);
    if (cmap_idx < 0)
        return NULL;

    // respect the forced 'no glyph' request
    FT_UInt glyph_idx = FTC_CMapCache_Lookup(data->cmap_cache, face_id, cmap_idx, codepoint);
    if (codepoint != 0 && glyph_idx == 0)
        return NULL;

    FTC_ScalerRec scaler = {
        .face_id = face_id,
        .width = size,
        .height = size,
        .pixel = 1,
        .x_res = 0,
        .y_res = 0,
    };
    FT_Glyph glyph = NULL;
    FT_Error ret = FTC_ImageCache_LookupScaler(data->image_cache, &scaler,
                                               (FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL),
                                               glyph_idx, &glyph, NULL);
    return (ret == 0) ? glyph : NULL;
}

static void font_data_destroy(void *p)
{
    struct freetype_font_data *data = p;
    FTC_Manager_RemoveFaceID(data->ftc_manager, data);
}

static void do_append_ft_font_data(struct priv_render *priv,
                                   struct ui_font *font,
                                   char *path, int idx,
                                   struct freetype_font_data **out)
{
    // each cache should be associated to only one typeface
    FTC_CMapCache cmap_cache = NULL;
    FTC_ImageCache image_cache = NULL;
    FTC_CMapCache_New(priv->ft_lib->manager, &cmap_cache);
    FTC_ImageCache_New(priv->ft_lib->manager, &image_cache);

    struct freetype_font_data *data = ta_new_ptrtype(font, data);
    ta_set_destructor(data, font_data_destroy);
    *data = (struct freetype_font_data) {
        .file_path = path,
        .face_idx = idx,
        .ftc_manager = priv->ft_lib->manager,
        .cmap_cache = cmap_cache,
        .image_cache = image_cache,
    };
    LL_APPEND(list, &font->font_data, data);
    *out = data;
}

static FT_Glyph find_glyph(struct ui_context *ctx,
                           struct ui_font *font,
                           int size, int codepoint)
{
    // find from cached typefaces
    struct priv_render *priv = ctx->priv_render;
    struct freetype_font_data *font_data = font->font_data.head;
    while (font_data) {
        FT_Glyph glyph = do_find_glyph(priv->ft_lib->manager, font_data, size, codepoint);
        if (glyph)
            return glyph;
        font_data = font_data->list.next;
    }

    int best_idx = 0;
    char *best_font = NULL;
    struct emulator_platform_data *data = emulator_get_platform_data(ctx);
    emulator_fontconfig_select(data->fontconfig, codepoint, &best_font, &best_idx);
    if (best_font) {
        // take the ownership of path string
        do_append_ft_font_data(priv, font, best_font, best_idx, &font_data);
        ta_steal(font_data, best_font);
    } else if (!font->font_data.head) {
        // try the fallback typeface if we don't have any
        do_append_ft_font_data(priv, font, data->fallback_font, 0, &font_data);
    }

    // try new typeface
    if (font_data) {
        FT_Glyph glyph = do_find_glyph(priv->ft_lib->manager, font_data, size, codepoint);
        if (glyph)
            return glyph;
    }

    return NULL;
}

static void font_cache_iterate(struct ui_context *ctx,
                               struct ui_font *font,
                               int size, const char *text,
                               void (*func)(FT_BitmapGlyph, void*),
                               void *data)
{
    bstr utf8_text = bstr0(text);
    while (true) {
        int codepoint = bstr_decode_utf8(utf8_text, &utf8_text);
        if (codepoint < 0)
            break;

        FT_Glyph glyph = find_glyph(ctx, font, size, codepoint);
        if (!glyph)
            glyph = find_glyph(ctx, font, size, 0);

        if (!glyph || glyph->format != FT_GLYPH_FORMAT_BITMAP)
            continue;

        FT_BitmapGlyph casted = (FT_BitmapGlyph) glyph;
        if (casted->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
            continue;

        func(casted, data);
    }
}

static void font_cache_cb_get_size(FT_BitmapGlyph glyph, void *p)
{
    void **pack = p;
    int *p_tex_w = pack[0];
    int *p_tex_h = pack[1];
    int *p_draw_w = pack[2];
    int *p_count = pack[3];
    (*p_count)++;
    *p_tex_w += glyph->bitmap.width;
    *p_draw_w += glyph->root.advance.x >> 16;
    *p_tex_h = MPMAX(*p_tex_h, (int) glyph->bitmap.rows);
}

static void font_cache_cb_build_text(FT_BitmapGlyph glyph, void *p)
{
    void **pack = p;
    struct priv_render *priv = pack[0];
    struct mp_rect *p_verts = pack[1];
    struct mp_rect *p_uvs = pack[2];
    struct ui_texture *tex = pack[3];
    int *p_idx = pack[4];
    int *p_tex_offset = pack[5];
    int *p_vert_offset = pack[6];

    int glyph_w = glyph->bitmap.width;
    int glyph_h = glyph->bitmap.rows;
    upload_texture_buffered(tex->ids[0], glyph->bitmap.buffer,
                            *p_tex_offset, 0, glyph_w, glyph_h, glyph->bitmap.pitch, 1,
                            GL_ALPHA, GL_UNSIGNED_BYTE, priv->buffer, PRIV_BUFFER_SIZE);

    struct mp_rect *uv = &p_uvs[*p_idx];
    uv->x0 = *p_tex_offset;
    uv->y0 = 0;
    uv->x1 = uv->x0 + glyph_w;
    uv->y1 = uv->y0 + glyph_h;

    struct mp_rect *vert = &p_verts[*p_idx];
    vert->x0 = *p_vert_offset + glyph->left;
    vert->y0 = -glyph->top;
    vert->x1 = vert->x0 + glyph_w;
    vert->y1 = vert->y0 + glyph_h;

    (*p_idx)++;
    *p_tex_offset += glyph_w;
    *p_vert_offset += (glyph->root.advance.x >> 16);
}

static void font_cache_destroy(void *p)
{
    struct draw_font_cache *cache = p;
    glDeleteTextures(1, &cache->tex);
}

static struct draw_font_cache *font_cache_ensure(struct ui_context *ctx,
                                                 struct ui_font *font,
                                                 int size, const char *text)
{
    struct draw_font_cache_entry entry = {
        .font_id = font->font_id,
        .font_size = size,
        .text = text,
    };

    // reuse from old cache
    struct priv_render *priv = ctx->priv_render;
    struct draw_font_cache *cache1 = font_cache_find(&priv->font_cache_old, &entry, true);
    if (cache1) {
        cache1->next = priv->font_cache_reused;
        priv->font_cache_reused = cache1;
        return cache1;
    }

    // maybe a duplicated draw text call
    struct draw_font_cache *cache2 = font_cache_find(&priv->font_cache_reused, &entry, false);
    if (cache2)
        return cache2;

    // calculate text texture size
    int tex_w = 0;
    int tex_h = 0;
    int draw_w = 0;
    int glyph_n = 0;
    void *pack_get_size[] = { &tex_w, &tex_h, &draw_w, &glyph_n };
    font_cache_iterate(ctx, font, size, text, font_cache_cb_get_size, pack_get_size);
    if (!tex_w || !tex_h)
        return NULL;

    // calculate glyph draw position and update texture
    GLuint tex_id = create_texture(tex_w, tex_h, GL_ALPHA, GL_UNSIGNED_BYTE);
    struct mp_rect *vert_rects = ta_new_array(NULL, struct mp_rect, glyph_n);
    struct mp_rect *uv_rects = ta_new_array(NULL, struct mp_rect, glyph_n);
    struct ui_texture tex = {
        .fmt = TEX_FMT_INTERNAL_A8,
        .w = tex_w,
        .h = tex_h,
        .ids = { tex_id },
    };
    int build_idx = 0;
    int build_off_tex = 0;
    int build_off_vert = 0;
    void *pack_build_text[] = {
        priv, vert_rects, uv_rects, &tex,
        &build_idx, &build_off_tex, &build_off_vert
    };
    font_cache_iterate(ctx, font, size, text, font_cache_cb_build_text, pack_build_text);

    // calculate vertices and texture coordinates
    struct gl_uv_vertex *buffer = NULL;
    int draw_n = build_uv_rect_buffer((struct uv_rect_vert_build_ctx) {
        .parent = NULL,
        .buffer = &buffer,
        .verts = vert_rects,
        .uvs = uv_rects,
        .tex_w = tex_w,
        .tex_h = tex_h,
        .rect_n = glyph_n,
    });
    buffer = ta_realloc_size(NULL, buffer, draw_n * sizeof(struct gl_uv_vertex));
    TA_FREEP(&vert_rects);
    TA_FREEP(&uv_rects);

    struct draw_font_cache *cache3 = ta_new_ptrtype(priv, cache3);
    ta_set_destructor(cache3, font_cache_destroy);
    *cache3 = (struct draw_font_cache) {
        .tex = tex_id,
        .tex_w = tex_w,
        .tex_h = tex_h,
        .draw_w = draw_w,
        .draw_buffer = ta_steal(cache3, buffer),
        .draw_count = draw_n,
        .entry = {
            .font_id = font->font_id,
            .font_size = size,
            .text = ta_strdup(cache3, text),
        },
        .next = priv->font_cache_reused,
    };
    priv->font_cache_reused = cache3;
    return cache3;
}

static bool render_font_init(struct ui_context *ctx, struct ui_font **font)
{
    struct priv_render *priv = ctx->priv_render;
    struct ui_font *result = ta_new_ptrtype(ctx, result);
    *result = (struct ui_font) {
        .font_id = ++priv->font_id,
        .font_data = NULL,
    };
    *font = result;
    return true;
}

static void render_font_uninit(struct ui_context *ctx, struct ui_font **font)
{
    TA_FREEP(font);
}

static void render_font_measure(struct ui_context *ctx, struct ui_font *font,
                                const char *text, int size, int *w, int *h)
{
    struct draw_font_cache *cache = font_cache_ensure(ctx, font, size, text);
    if (!cache)
        return;

    if (w)
        *w = cache->draw_w;
    if (h)
        *h = cache->tex_h;
}

static void render_clip_start(struct ui_context *ctx, struct mp_rect *rect)
{
    int inverted_y = VITA_SCREEN_H - rect->y1;
    glEnable(GL_SCISSOR_TEST);
    glScissor(rect->x0, inverted_y, mp_rect_w(*rect), mp_rect_h(*rect));
}

static void render_clip_end(struct ui_context *ctx)
{
    glDisable(GL_SCISSOR_TEST);
}

static void render_draw_font(struct ui_context *ctx, struct ui_font *font,
                             struct ui_font_draw_args *args)
{
    struct draw_font_cache *cache = font_cache_ensure(ctx, font, args->size, args->text);
    if (!cache)
        return;

    struct ui_texture tex = {
        .fmt = TEX_FMT_INTERNAL_A8,
        .w = cache->tex_w,
        .h = cache->tex_h,
        .ids = { cache->tex },
    };
    do_render_draw_texture_ext(ctx, &tex, cache->draw_buffer, args->color,
                               args->x, args->y, cache->draw_count);
}

static bool render_draw_vertices_prepare(struct ui_context *ctx,
                                         struct ui_color_vertex **verts, int n)
{
    struct priv_render *priv = get_priv_render(ctx);
    int req_size = sizeof(struct ui_color_vertex) * n;
    if (req_size > ta_get_size(priv->buffer))
        return false;

    *verts = priv->buffer;
    return true;
}

static void render_draw_vertices_compose(struct ui_context *ctx,
                                         struct ui_color_vertex *verts,
                                         int i, float x, float y, ui_color color)
{
    verts[i] = (struct ui_color_vertex) {
        .x = x,
        .y = y,
        .color = color,
    };
}

static void render_draw_vertices_copy(struct ui_context *ctx,
                                      struct ui_color_vertex *verts,
                                      int dst, int src)
{
    memcpy(&verts[dst], &verts[src], sizeof(struct ui_color_vertex));
}

static void render_draw_vertices_commit(struct ui_context *ctx,
                                        struct ui_color_vertex *verts, int n)
{
    int stride = sizeof(struct ui_color_vertex);
    struct priv_render *priv = get_priv_render(ctx);
    struct gl_program_draw_triangle *program = &priv->program_draw_triangle;
    glUseProgram(program->program_data.program);
    glVertexAttribPointer(attr_draw_triangle_pos.pos, 2,
                          GL_FLOAT, GL_FALSE, stride, &verts->x);
    glEnableVertexAttribArray(attr_draw_triangle_pos.pos);
    glVertexAttribPointer(attr_draw_triangle_color.pos, 4,
                          GL_UNSIGNED_BYTE, GL_TRUE, stride, &verts->color);
    glEnableVertexAttribArray(attr_draw_triangle_color.pos);
    glUniformMatrix4fv(program->uniform_transform, 1, GL_FALSE, priv->normalize_matrix);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, n);
}

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
    .texture_decode = render_texture_decode,
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
    .draw_vertices_compose = render_draw_vertices_compose,
    .draw_vertices_copy = render_draw_vertices_copy,
    .draw_vertices_commit = render_draw_vertices_commit,
};
