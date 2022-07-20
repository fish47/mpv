#include "emulator.h"
#include "ui_device.h"
#include "ui_driver.h"
#include "video/img_format.h"

#include <GLES2/gl2.h>

#include <freetype2/ft2build.h>
#include FT_CACHE_H
#include FT_FREETYPE_H

#define PRIV_BUFFER_SIZE        (1024 * 1024 * 4)

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
    "attribute vec4 a_draw_pos;"
    "attribute vec2 a_texture_pos;"
    "varying vec2 v_texture_pos;"
    "void main() {"
    "    gl_Position = a_draw_pos;"
    "    v_texture_pos = a_texture_pos;"
    "}";

static const struct gl_attr_spec attr_draw_tex_pos_draw = { .name = "a_draw_pos", .pos = 0, };
static const struct gl_attr_spec attr_draw_tex_pos_tex = { .name = "a_texture_pos", .pos = 1, };

static const char *const shader_source_vert_triangle =
    "attribute vec4 a_draw_pos;"
    "void main() {"
    "    gl_Position = a_draw_pos;"
    "}";

static const struct gl_attr_spec attr_draw_triangle_pos = { .name = "a_draw_pos", .pos = 0 };

static const char *const shader_source_frag_triangle =
    "precision mediump float;"
    "uniform vec4 u_color;"
    "void main() {"
    "    gl_FragColor = u_color;"
    "}";

static const char *uniform_draw_triangle_color = "u_color";

struct gl_tex_plane_spec {
    int bpp;
    int div;
    GLenum fmt;
    GLenum type;
    const char *name;
};

struct gl_tex_impl_spec {
    int num_planes;
    const struct gl_tex_plane_spec *plane_specs;
    const char *shader_source_frag;
};

static const struct gl_tex_impl_spec tex_spec_unknown = {
    .num_planes = 0,
    .plane_specs = NULL,
    .shader_source_frag = NULL,
};

static const struct gl_tex_impl_spec tex_spec_a8 = {
    .num_planes = 1,
    .plane_specs = (const struct gl_tex_plane_spec[]) {
        { 1, 1, GL_ALPHA, GL_UNSIGNED_BYTE, "u_texture" },
    },
    .shader_source_frag =
        "precision mediump float;"
        "varying vec2 v_texture_pos;"
        "uniform sampler2D u_texture;"
        "uniform vec4 u_tint_color;"
        "void main() {"
        "    gl_FragColor = texture2D(u_texture, v_texture_pos).a * u_tint_color;"
        "}",
};

static const struct gl_tex_impl_spec tex_spec_rgba = {
    .num_planes = 1,
    .plane_specs = (const struct gl_tex_plane_spec[]) {
        { 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, "u_texture" },
    },
    .shader_source_frag =
        "precision mediump float;"
        "varying vec2 v_texture_pos;"
        "uniform sampler2D u_texture;"
        "uniform vec4 u_tint_color;"
        "void main() {"
        "    gl_FragColor = texture2D(u_texture, v_texture_pos) * u_tint_color;"
        "}",
};

static const struct gl_tex_impl_spec tex_spec_yuv420 = {
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
        "uniform vec4 u_tint_color;"
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
        "    gl_FragColor = vec4(rgb, 1) * u_tint_color;"
        "}",
};

static const char *uniform_draw_tex_tint_color = "u_tint_color";

struct gl_program_data {
    GLuint program;
    GLuint shader_vert;
    GLuint shader_frag;
};

struct gl_program_draw_tex {
    struct gl_program_data program_data;
    GLint uniform_textures[MP_MAX_PLANES];
    GLint uniform_tint_color;
};

struct gl_program_draw_triangle {
    struct gl_program_data program_data;
    GLint uniform_color;
};

struct gl_float_rect {
    float x0;
    float y0;
    float x1;
    float y1;
};

struct freetype_lib {
    FT_Library lib;
    FTC_Manager manager;
    FTC_CMapCache cmap_cache;
    FTC_ImageCache image_cache;
};

struct draw_font_cache_entry {
    int font_id;
    int font_size;
    const char *text;
};

struct draw_font_cache {
    int w;
    int h;
    GLuint tex;
    int count;
    struct gl_float_rect *uvs;
    struct gl_float_rect *verts;
    struct draw_font_cache_entry entry;
    struct draw_font_cache *next;
};

struct priv_render {
    void *buffer;
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
};

struct ui_font {
    const char *font_path;
    int font_id;
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
                         const struct gl_uniform_spec *uniforms,
                         int uniform_count)
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

    for (int i = 0; i < uniform_count; ++i) {
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
    int idx = 0;
    struct gl_uniform_spec uniforms[1 + MP_MAX_PLANES];
    uniforms[idx++] = (struct gl_uniform_spec) {
        .name = uniform_draw_tex_tint_color,
        .output = &program->uniform_tint_color
    };
    for (int i = 0; i < spec->num_planes; ++i) {
        uniforms[idx++] = (struct gl_uniform_spec) {
            .name = spec->plane_specs[i].name,
            .output = &program->uniform_textures[i]
        };
    }

    const struct gl_attr_spec *attrs[] = { &attr_draw_tex_pos_draw, &attr_draw_tex_pos_tex, NULL };
    return init_program(&program->program_data,
                        shader_source_vert_texture, spec->shader_source_frag,
                        attrs, uniforms, idx);
}

static bool init_program_triangle(struct gl_program_draw_triangle *program)
{
    const struct gl_attr_spec *attrs[] = { &attr_draw_triangle_pos, NULL };
    const struct gl_uniform_spec uniforms[] = {
        { .name = uniform_draw_triangle_color, .output = &program->uniform_color },
    };
    return init_program(&program->program_data,
                        shader_source_vert_triangle, shader_source_frag_triangle,
                        attrs, uniforms, 1);
}

static FT_Error ftc_request_cb(FTC_FaceID face_id, FT_Library lib,
                               FT_Pointer data, FT_Face *face)
{
    struct ui_font *font = (void*) face_id;
    return FT_New_Face(lib, font->font_path, 0, face);
}

static void uninit_freetype(void *p)
{
    struct freetype_lib *lib = p;
    FTC_Manager_Done(lib->manager);
    FT_Done_FreeType(lib->lib);
}

static bool init_freetype(struct priv_render *priv)
{
    FT_Library lib;
    FTC_Manager ft_manager;
    FTC_CMapCache ft_cmap_cache;
    FTC_ImageCache ft_image_cache;
    if (FT_Init_FreeType(&lib) != 0)
        goto error_lib;
    if (FTC_Manager_New(lib, 0, 0, 0, &ftc_request_cb, NULL, &ft_manager) != 0)
        goto error_manager;
    if (FTC_CMapCache_New(ft_manager, &ft_cmap_cache) != 0)
        goto error_cmap;
    if (FTC_ImageCache_New(ft_manager, &ft_image_cache) != 0)
        goto error_image;

    struct freetype_lib *result = ta_new_ptrtype(priv, result);
    *result = (struct freetype_lib) {
        .lib = lib,
        .manager = ft_manager,
        .cmap_cache = ft_cmap_cache,
        .image_cache = ft_image_cache,
    };
    ta_set_destructor(result, uninit_freetype);
    priv->ft_lib = result;
    return true;

error_cmap:
error_image:
    FTC_Manager_Done(ft_manager);
error_manager:
    FT_Done_FreeType(lib);
error_lib:
    return false;
}

static bool render_init(struct ui_context *ctx)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    struct priv_render *priv = ctx->priv_render;
    memset(priv, 0, sizeof(struct priv_render));
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
    glClearColor(0, 0, 0, 0);
}

static void render_render_end(struct ui_context *ctx)
{
    struct priv_render *priv = ctx->priv_render;
    font_cache_free_all(&priv->font_cache_old);

    glfwSwapBuffers(emulator_get_window(ctx));
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

static bool render_texture_init(struct ui_context *ctx, struct ui_texture **tex,
                                enum ui_texure_fmt fmt, int w, int h)
{
    struct ui_texture *new_tex = ta_new_ptrtype(ctx, new_tex);
    new_tex->w = w;
    new_tex->h = h;
    new_tex->fmt = fmt;

    const struct gl_tex_impl_spec *spec = get_gl_tex_impl_spec(fmt);
    for (int i = 0; i < spec->num_planes; ++i) {
        const struct gl_tex_plane_spec *plane = spec->plane_specs + i;
        int tex_w = w / plane->div;
        int tex_h = h / plane->div;
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

static void upload_texture_buffered(GLuint id, void *data,
                                    int x, int y, int w, int h,
                                    int stride, int bpp,
                                    GLenum fmt, GLenum type,
                                    void *buffer, int capacity)
{
    glBindTexture(GL_TEXTURE_2D, id);

    int row = 0;
    int col = 0;
    uint8_t *cur = data;
    uint8_t *next = cur + stride;
    int row_bytes = w * bpp;
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

static void render_texture_upload(struct ui_context *ctx, struct ui_texture *tex,
                                  void **data, int *strides, int planes)
{
    const struct gl_tex_impl_spec *spec = get_gl_tex_impl_spec(tex->fmt);
    if (spec->num_planes != planes)
        return;

    struct priv_render *priv_render = ctx->priv_render;
    for (int i = 0; i < planes; ++i) {
        const struct gl_tex_plane_spec *plane = spec->plane_specs + i;
        int tex_w = tex->w / plane->div;
        int tex_h = tex->h / plane->div;
        upload_texture_buffered(tex->ids[i], data[i], 0, 0, tex_w, tex_h,
                                strides[i], plane->bpp, plane->fmt, plane->type,
                                priv_render->buffer, PRIV_BUFFER_SIZE);
    }
}

static float normalize_to_uv_xy(float xy, int wh)
{
    return xy /= wh;
}

static void normalize_to_rect_uv(struct gl_float_rect *in_out, int w, int h)
{
    in_out->x0 = normalize_to_uv_xy(in_out->x0, w);
    in_out->y0 = normalize_to_uv_xy(in_out->y0, h);
    in_out->x1 = normalize_to_uv_xy(in_out->x1, w);
    in_out->y1 = normalize_to_uv_xy(in_out->y1, h);
}

static float normalize_to_vert_x(float x)
{
    return (x - VITA_SCREEN_W * 0.5f) / VITA_SCREEN_W * 2.0f;
}

static float normalize_to_vert_y(float y)
{
    return (y - VITA_SCREEN_H * 0.5f) / VITA_SCREEN_H * -2.0f;
}

static float normalize_to_vert_offset_x(float x)
{
    return x / VITA_SCREEN_W * 2.0f;
}

static float normalize_to_vert_offset_y(float y)
{
    return y / VITA_SCREEN_H * -2.0f;
}

static void *normalize_to_vec4_color(float *base, unsigned int color)
{
    // same as vita2d's color define
    base[0] = (float) ((color >> 24) & 0xff) / 0xff;
    base[1] = (float) ((color >> 16) & 0xff) / 0xff;
    base[2] = (float) ((color >> 8) & 0xff) / 0xff;
    base[3] = (float) ((color >> 0) & 0xff) / 0xff;
    return base + 4;
}

static void normalize_to_rect_vert(struct gl_float_rect *in_out)
{
    in_out->x0 = normalize_to_vert_x(in_out->x0);
    in_out->y0 = normalize_to_vert_y(in_out->y0);
    in_out->x1 = normalize_to_vert_x(in_out->x1);
    in_out->y1 = normalize_to_vert_y(in_out->y1);
}

static void normalize_to_rect_from_mp_rect(struct gl_float_rect *out, struct mp_rect *rect, float w, float h)
{
    out->x0 = rect ? rect->x0 : 0;
    out->y0 = rect ? rect->y0 : 0;
    out->x1 = rect ? rect->x1 : w;
    out->y1 = rect ? rect->y1 : h;
}

static int normalize_to_triangle_strip(float *out, int *offset, struct gl_float_rect *rect, int i, int n)
{
    int p = *offset;
    int draw_count = 0;
    if (i > 0) {
        draw_count++;
        out[p++] = rect->x0;
        out[p++] = rect->y0;
    }

    draw_count += 4;
    out[p++] = rect->x0;
    out[p++] = rect->y0;
    out[p++] = rect->x0;
    out[p++] = rect->y1;
    out[p++] = rect->x1;
    out[p++] = rect->y0;
    out[p++] = rect->x1;
    out[p++] = rect->y1;

    if (i + 1 < n) {
        draw_count++;
        out[p++] = rect->x1;
        out[p++] = rect->y1;
    }

    *offset = p;
    return draw_count;
}

static void normalize_to_attr_buf(void *buffer, int rect_count,
                                  struct gl_float_rect *verts, struct gl_float_rect *uvs,
                                  int voffset_x, int voffset_y,
                                  float **buf_verts, float **buf_uvs, int *draw_count)
{
    int offset = 0;
    *buf_uvs = buffer;
    *draw_count = 0;
    for (int i = 0; i < rect_count; ++i)
        *draw_count += normalize_to_triangle_strip(*buf_uvs, &offset, &uvs[i], i, rect_count);

    float norm_offset_x = normalize_to_vert_offset_x(voffset_x);
    float norm_offset_y = normalize_to_vert_offset_y(voffset_y);
    *buf_verts = *buf_uvs + offset;
    offset = 0;
    for (int i = 0; i < rect_count; ++i) {
        struct gl_float_rect transformed = verts[i];
        transformed.x0 += norm_offset_x;
        transformed.y0 += norm_offset_y;
        transformed.x1 += norm_offset_x;
        transformed.y1 += norm_offset_y;
        normalize_to_triangle_strip(*buf_verts, &offset, &transformed, i, rect_count);
    }
}

static void render_draw_texture_ext(struct ui_context *ctx, struct ui_texture *tex,
                                    struct gl_float_rect *verts, struct gl_float_rect *uvs,
                                    int tint, int voffset_x, int voffset_y, int rect_count)
{
    const struct gl_tex_impl_spec *spec = get_gl_tex_impl_spec(tex->fmt);
    if (!spec)
        return;

    struct priv_render *priv = get_priv_render(ctx);
    struct gl_program_draw_tex *program = get_gl_program_draw_tex(priv, tex->fmt);
    if (!program)
        return;

    int draw_count = 0;
    float *buf_uvs = NULL;
    float *buf_verts = NULL;
    normalize_to_attr_buf(priv->buffer, rect_count, verts, uvs,
                          voffset_x, voffset_y, &buf_verts, &buf_uvs, &draw_count);
    if (!draw_count)
        return;

    glUseProgram(program->program_data.program);
    glVertexAttribPointer(attr_draw_tex_pos_draw.pos, 2, GL_FLOAT, GL_FALSE, 0, buf_verts);
    glEnableVertexAttribArray(attr_draw_tex_pos_draw.pos);
    glVertexAttribPointer(attr_draw_tex_pos_tex.pos, 2, GL_FLOAT, GL_FALSE, 0, buf_uvs);
    glEnableVertexAttribArray(attr_draw_tex_pos_tex.pos);

    for (int i = 0; i < spec->num_planes; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, tex->ids[i]);
        glUniform1i(program->uniform_textures[i], i);
    }

    float tint_color[4];
    normalize_to_vec4_color(tint_color, tint);
    glUniform4fv(program->uniform_tint_color, 1, tint_color);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, draw_count);
}

static void render_draw_texture(struct ui_context *ctx,
                                struct ui_texture *tex,
                                struct ui_texture_draw_args *args)
{
    struct gl_float_rect uvs;
    normalize_to_rect_from_mp_rect(&uvs, args->src, tex->w, tex->h);
    normalize_to_rect_uv(&uvs, tex->w, tex->h);

    struct gl_float_rect verts;
    normalize_to_rect_from_mp_rect(&verts, args->dst, VITA_SCREEN_W, VITA_SCREEN_H);
    normalize_to_rect_vert(&verts);

    render_draw_texture_ext(ctx, tex, &verts, &uvs, -1, 0, 0, 1);
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

static void font_cache_iterate(struct ui_context *ctx,
                               struct ui_font *font,
                               struct ui_font_draw_args *args,
                               void (*func)(FT_BitmapGlyph, void*),
                               void *data)
{
    FT_Face face;
    FTC_FaceID face_id = (FTC_FaceID) font;
    struct priv_render *priv = ctx->priv_render;
    if (FTC_Manager_LookupFace(priv->ft_lib->manager, face_id, &face) != 0)
        return;

    FT_Int charmap_idx = FT_Get_Charmap_Index(face->charmap);
    if (charmap_idx < 0)
        return;

    bstr utf8_text = bstr0(args->text);
    while (true) {
        int codepoint = bstr_decode_utf8(utf8_text, &utf8_text);
        if (codepoint < 0)
            break;

        FT_UInt glyph_idx = FTC_CMapCache_Lookup(priv->ft_lib->cmap_cache, face_id, charmap_idx, codepoint);
        if (glyph_idx == 0)
            continue;

        FTC_ScalerRec scaler = {
            .face_id = face_id,
            .width = args->size,
            .height = args->size,
            .pixel = 1,
            .x_res = 0,
            .y_res = 0,
        };
        FT_Glyph glyph;
        FT_ULong flags = FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL;
        if (FTC_ImageCache_LookupScaler(priv->ft_lib->image_cache, &scaler, flags, glyph_idx, &glyph, NULL) != 0)
            continue;
        if (glyph->format != FT_GLYPH_FORMAT_BITMAP)
            continue;

        FT_BitmapGlyph casted = (void*) glyph;
        if (casted->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
            continue;

        func(casted, data);
    }
}

struct cb_args_get_size {
    int w;
    int h;
    int count;
};

static void font_cache_cb_get_size(FT_BitmapGlyph glyph, void *p)
{
    struct cb_args_get_size *args = p;
    ++args->count;
    args->w += glyph->bitmap.width;
    args->h = MPMAX(args->h, (int) glyph->bitmap.rows);
}

struct cb_args_build_text {
    struct ui_context *ctx;
    struct gl_float_rect *uvs;
    struct gl_float_rect *verts;
    GLuint tex_id;
    int tex_w;
    int tex_h;

    int index;
    int tex_offset;
    int vert_offset;
};

static void font_cache_cb_build_text(FT_BitmapGlyph glyph, void *p)
{
    struct cb_args_build_text *args = p;
    struct priv_render *priv = args->ctx->priv_render;
    int glyph_w = glyph->bitmap.width;
    int glyph_h = glyph->bitmap.rows;
    upload_texture_buffered(args->tex_id, glyph->bitmap.buffer,
                            args->tex_offset, 0, glyph_w, glyph_h, glyph->bitmap.pitch, 1,
                            GL_ALPHA, GL_UNSIGNED_BYTE, priv->buffer, PRIV_BUFFER_SIZE);

    struct gl_float_rect *uv = &args->uvs[args->index];
    uv->x0 = args->tex_offset;
    uv->y0 = 0;
    uv->x1 = uv->x0 + glyph_w;
    uv->y1 = uv->y0 + glyph_h;

    struct gl_float_rect *vert = &args->verts[args->index];
    vert->x0 = args->vert_offset + glyph->left;
    vert->y0 = -glyph->top;
    vert->x1 = vert->x0 + glyph_w;
    vert->y1 = vert->y0 + glyph_h;

    args->index++;
    args->tex_offset += glyph_w;
    args->vert_offset += (glyph->root.advance.x >> 16);
    normalize_to_rect_uv(uv, args->tex_w, args->tex_h);
    normalize_to_rect_vert(vert);
}

static void font_cache_destroy(void *p)
{
    struct draw_font_cache *cache = p;
    glDeleteTextures(1, &cache->tex);
}

static struct draw_font_cache *font_cache_ensure(struct ui_context *ctx,
                                                 struct ui_font *font,
                                                 struct ui_font_draw_args *args)
{
    struct draw_font_cache_entry entry = {
        .font_id = font->font_id,
        .font_size = args->size,
        .text = args->text,
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
    struct cb_args_get_size args_get_size = {0};
    font_cache_iterate(ctx, font, args, font_cache_cb_get_size, &args_get_size);
    if (!args_get_size.w || !args_get_size.h)
        return NULL;

    // calculate glyph draw position
    struct cb_args_build_text args_build_text = {
        .ctx = ctx,
        .uvs = ta_new_array(NULL, struct gl_float_rect, args_get_size.count),
        .verts = ta_new_array(NULL, struct gl_float_rect, args_get_size.count),
        .tex_id = create_texture(args_get_size.w, args_get_size.h, GL_ALPHA, GL_UNSIGNED_BYTE),
        .tex_w = args_get_size.w,
        .tex_h = args_get_size.h,
        .index = 0,
        .tex_offset = 0,
        .vert_offset = 0,
    };
    font_cache_iterate(ctx, font, args, font_cache_cb_build_text, &args_build_text);

    struct draw_font_cache *cache3 = ta_new_ptrtype(priv, cache3);
    ta_set_destructor(cache3, font_cache_destroy);
    *cache3 = (struct draw_font_cache) {
        .w = args_get_size.w,
        .h = args_get_size.h,
        .tex = args_build_text.tex_id,
        .uvs = ta_steal(cache3, args_build_text.uvs),
        .verts = ta_steal(cache3, args_build_text.verts),
        .count = args_get_size.count,
        .entry = {
            .font_id = font->font_id,
            .font_size = args->size,
            .text = ta_strdup(cache3, args->text),
        },
        .next = priv->font_cache_reused,
    };
    priv->font_cache_reused = cache3;
    return cache3;
}

static bool render_font_init(struct ui_context *ctx, struct ui_font **font, const char *path)
{
    struct priv_render *priv = ctx->priv_render;
    struct ui_font *result = ta_new_ptrtype(ctx, result);
    *result = (struct ui_font) {
        .font_id = ++priv->font_id,
        .font_path = ta_strdup(result, path),
    };
    *font = result;
    return true;
}

static void render_font_uninit(struct ui_context *ctx, struct ui_font **font)
{
    TA_FREEP(font);
}

static void render_draw_font(struct ui_context *ctx, struct ui_font *font,
                             struct ui_font_draw_args *args)
{
    struct draw_font_cache *cache = font_cache_ensure(ctx, font, args);
    if (!cache)
        return;

    struct ui_texture tex = {
        .fmt = TEX_FMT_INTERNAL_A8,
        .w = cache->w,
        .h = cache->h,
        .ids = { cache->tex },
    };
    render_draw_texture_ext(ctx, &tex, cache->verts, cache->uvs, args->color, args->x, args->y, cache->count);
}

static void render_draw_rectangle(struct ui_context *ctx, struct ui_triangle_draw_args *args)
{
    struct priv_render *priv = get_priv_render(ctx);
    struct gl_program_draw_triangle *program = &priv->program_draw_triangle;

    int draw_count = 0;
    int buf_offset = 0;
    float *buf_verts = priv->buffer;
    for (int i = 0; i < args->count; ++i) {
        struct mp_rect *origin = &args->rects[i];
        struct gl_float_rect normalized = {
            .x0 = origin->x0,
            .y0 = origin->y0,
            .x1 = origin->x1,
            .y1 = origin->y1,
        };
        normalize_to_rect_vert(&normalized);
        draw_count += normalize_to_triangle_strip(buf_verts, &buf_offset, &normalized, i, args->count);
    }

    if (!draw_count)
        return;

    float *buf_color = buf_verts + buf_offset;
    normalize_to_vec4_color(buf_color, args->color);

    glUseProgram(program->program_data.program);
    glVertexAttribPointer(attr_draw_triangle_pos.pos, 2, GL_FLOAT, GL_FALSE, 0, buf_verts);
    glEnableVertexAttribArray(attr_draw_triangle_pos.pos);
    glUniform4fv(program->uniform_color, 1, buf_color);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, draw_count);
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

    .font_init = render_font_init,
    .font_uninit = render_font_uninit,

    .draw_font = render_draw_font,
    .draw_texture = render_draw_texture,
    .draw_rectangle = render_draw_rectangle,
};
