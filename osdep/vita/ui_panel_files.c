#include "ui_context.h"
#include "ui_device.h"
#include "ui_driver.h"
#include "ui_panel.h"
#include "ta/ta_talloc.h"

#include <stdlib.h>
#include <dirent.h>
#include <sys/types.h>

#define PATH_SEP "/"
#define PATH_DIR_PARENT ".."
#define PATH_DIR_CURRENT "."
#define PATH_ESCAPED_SPACE ' '

#define LAYOUT_ITEM_COUNT 10
#define LAYOUT_ITEM_HEIGHT 30
#define LAYOUT_ITEM_FONT_SIZE 30
#define LAYOUT_ITEM_FONT_COLOR 0xFFFFFFFF

#define DPAD_ACT_TRIGGER_DELAY_US   (600 * 1000)
#define DPAD_ACT_REPEAT_DELAY_US    (40 * 1000)

static const char escape_space_chars[] = "\t\n\r\f\v";

struct dpad_act_sepc {
    enum ui_key_code key;
    int cursor_offset;
    int page_offset;
};

static const struct dpad_act_sepc dpad_act_spec_list[] = {
    { .key = UI_KEY_CODE_VITA_DPAD_UP, .cursor_offset = -1, .page_offset = 0 },
    { .key = UI_KEY_CODE_VITA_DPAD_DOWN, .cursor_offset = 1, .page_offset = 0 },
    { .key = UI_KEY_CODE_VITA_DPAD_LEFT, .cursor_offset = 0, .page_offset = -1 },
    { .key = UI_KEY_CODE_VITA_DPAD_RIGHT, .cursor_offset = 0, .page_offset = 1, },
};

enum path_item_flag {
    PATH_ITEM_FLAG_TYPE_DIR = 1,
    PATH_ITEM_FLAG_SANTIZIE_NAME = 1 << 1,
};

struct path_item {
    char *name;
    int flags;
};

struct cache_data {
    struct ui_font *font;
    struct path_item *path_items;
    int path_item_count;
    char *sanitized_name_cache;
    char *path_name_pool;
};

struct cursor_data {
    int top;
    int current;
};

struct priv_panel {
    char *full_path;
    struct cursor_data cursor_pos;
    struct cache_data cache_data;

    struct cursor_data *cursor_pos_stack;
    int cursor_pos_count;

    const struct dpad_act_sepc *pressed_dpad_act;
    int64_t presssed_dpad_start_time;
    int pressed_dpad_handled_count;
};

static char *sanitize_path_name(char *name, struct priv_panel *priv, bool *out_changed)
{
    int capacity = 0;
    char *out = NULL;
    if (priv) {
        out = priv->cache_data.sanitized_name_cache;
        if (out) {
            capacity = ta_get_size(out);
        } else {
            capacity = 100;
            out = ta_alloc_size(priv, capacity);
        }
    }

    int i = 0;
    bool stop = false;
    bool changed = false;
    while (!stop) {
        char ch = name[i];
        if (!ch) {
            stop = true;
        } else if (strchr(escape_space_chars, ch)) {
            changed = true;
            ch = PATH_ESCAPED_SPACE;
        }

        if (priv) {
            if (capacity <= i) {
                capacity <<= 1;
                out = ta_realloc_size(priv, out, capacity);
            }
            out[i] = ch;
        } else if (changed) {
            // stop if special character check failed
            break;
        }

        ++i;
    }

    if (out_changed)
        *out_changed = changed;

    if (priv)
        priv->cache_data.sanitized_name_cache = out;

    return (changed && out) ? out : name;
}

static int resolve_path_item_flags(struct dirent *d)
{
    bool need_sanitize = false;
    sanitize_path_name(d->d_name, NULL, &need_sanitize);

    int flags = 0;
    if (d->d_type & DT_DIR)
        flags |= PATH_ITEM_FLAG_TYPE_DIR;
    if (need_sanitize)
        flags |= PATH_ITEM_FLAG_SANTIZIE_NAME;
    return flags;
}

static int compare_path_item(const void *l, const void *r)
{
    const struct path_item* lhs = l;
    const struct path_item* rhs = r;

    // show directories first
    int l_dir = lhs->flags & PATH_ITEM_FLAG_TYPE_DIR;
    int r_dir = rhs->flags & PATH_ITEM_FLAG_TYPE_DIR;
    if (l_dir != r_dir)
        return r_dir - l_dir;

    // stdlib should be fine to handle utf8 string comparation
    return strcmp(lhs->name, rhs->name);
}

static void do_fill_path_items(struct priv_panel *priv, DIR *dir)
{
    // a hack to suppress 'casting int to char*' warning
    char *base = NULL;

    // pack all path names into a continuous memory block
    struct cache_data *cache = &priv->cache_data;
    int offset = 0;
    int capacity = 0;
    char *name_pool = cache->path_name_pool;
    if (name_pool) {
        capacity = ta_get_size(name_pool);
    } else {
        capacity = 1024;
        name_pool = ta_alloc_size(priv, capacity);
    }

    while (true) {
        struct dirent *d = readdir(dir);
        if (!d)
            break;

        if (strcmp(d->d_name, PATH_DIR_PARENT) == 0)
            continue;
        if (strcmp(d->d_name, PATH_DIR_CURRENT) == 0)
            continue;

        // ensure capacity and copy path name with terminator
        int remain = capacity - offset;
        int request = strlen(d->d_name) + 1;
        if (remain < request) {
            capacity <<= 1;
            name_pool = ta_realloc_size(priv, name_pool, capacity);
        }
        memcpy(name_pool + offset, d->d_name, request);

        // memory block may expand, and its address may change
        // so offsets are used before iteration done
        MP_TARRAY_APPEND(priv, cache->path_items, cache->path_item_count, (struct path_item) {
            .name = base + offset,
            .flags = resolve_path_item_flags(d),
        });
        offset += request;
    }

    // translate offsets to addresses
    for (int i = 0; i < cache->path_item_count; ++i) {
        char **pp_name = &cache->path_items[i].name;
        int offset = *pp_name - base;
        *pp_name = name_pool + offset;
    }
    qsort(cache->path_items, cache->path_item_count, sizeof(struct path_item), compare_path_item);
}

static void fill_path_items(struct priv_panel *priv)
{
    struct cache_data *cache = &priv->cache_data;
    cache->path_item_count = 0;

    if (!priv->full_path)
        return;

    DIR *dir = opendir(priv->full_path);
    if (dir) {
        do_fill_path_items(priv, dir);
        closedir(dir);
    }
}

static bool cursor_pos_move(struct cursor_data *pos, int cur_offset, int page_offset, int count)
{
    if (cur_offset != 0) {
        int new_cur = MPCLAMP(pos->current + cur_offset, 0, count - 1);
        if (new_cur == pos->current)
            return false;

        // adjust viewport if cursor is out of bound
        pos->current = new_cur;
        if (pos->top > pos->current)
            pos->top = pos->current;
        else if (pos->top < pos->current - (LAYOUT_ITEM_COUNT - 1))
            pos->top = MPMAX(pos->current - (LAYOUT_ITEM_COUNT - 1), 0);
        return true;
    } else if (page_offset != 0) {
        // stop flipping to next page if we are on the last page
        int move_count = page_offset * LAYOUT_ITEM_COUNT;
        if (pos->top + move_count >= count)
            return false;

        int new_top = MPCLAMP(pos->top + move_count, 0, count - 1);
        if (new_top == pos->top)
            return false;

        // keep cursor top offset when page is flipped
        int delta = pos->current - pos->top;
        pos->top = new_top;
        pos->current = MPCLAMP(new_top + delta, 0, count - 1);
        return true;
    }
    return false;
}

static void cursor_pos_relocate(struct ui_context *ctx, char *name)
{
    struct priv_panel *priv = ctx->priv_panel;
    int cursor = priv->cursor_pos.current;
    if (cursor >= priv->cache_data.path_item_count)
        goto relocate;

    char *cursor_name = priv->cache_data.path_items[cursor].name;
    if (strcmp(cursor_name, name) != 0)
        goto relocate;

    return;

relocate:
    for (int i = 0; i < priv->cache_data.path_item_count; ++i) {
        if (strcmp(name, priv->cache_data.path_items[i].name) == 0) {
            priv->cursor_pos = (struct cursor_data) { .current = i, .top = i };
            return;
        }
    }

    priv->cursor_pos = (struct cursor_data) { .current = 0, .top = 0 };
}

static void push_path(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct path_item *item = &priv->cache_data.path_items[priv->cursor_pos.current];
    if (item->flags & PATH_ITEM_FLAG_TYPE_DIR) {
        ta_strdup_append(&priv->full_path, PATH_SEP);
        ta_strdup_append(&priv->full_path, item->name);
        MP_TARRAY_APPEND(priv, priv->cursor_pos_stack, priv->cursor_pos_count, priv->cursor_pos);
        priv->cursor_pos = (struct cursor_data) { .current = 0, .top = 0 };
        fill_path_items(priv);
        ui_panel_common_invalidate(ctx);
    }
}

static void pop_path(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    if (priv->cursor_pos_count <= 0) {
        ui_panel_common_pop(ctx);
        return;
    }

    char *sep = strrchr(priv->full_path, PATH_SEP[0]);
    if (!sep)
        return;

    *sep = 0;
    fill_path_items(priv);
    MP_TARRAY_POP(priv->cursor_pos_stack, priv->cursor_pos_count, &priv->cursor_pos);
    ui_panel_common_invalidate(ctx);

    // in case path items are changed
    char *name = sep + 1;
    cursor_pos_relocate(ctx, name);
}

static bool files_init(struct ui_context *ctx, void *p)
{
    struct priv_panel *priv = ctx->priv_panel;
    priv->full_path = ta_strdup(priv, "/home/fish47");
    fill_path_items(priv);
    return true;
}

static void files_uninit(struct ui_context *ctx)
{}

static void files_on_show(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    ui_render_driver_vita.font_init(ctx, &priv->cache_data.font, "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc");
}

static void files_on_hide(struct ui_context *ctx)
{}

static void files_on_draw(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct cache_data *cache = &priv->cache_data;
    if (!cache->font)
        return;

    int draw_top = 60;
    for (int i = 0; i < LAYOUT_ITEM_COUNT; ++i) {
        int idx = priv->cursor_pos.top + i;
        if (idx >= cache->path_item_count)
            break;

        struct path_item *item = &cache->path_items[idx];
        bool need_sanitize = (item->flags & PATH_ITEM_FLAG_SANTIZIE_NAME);
        char *text = need_sanitize ? sanitize_path_name(item->name, priv, NULL) : item->name;
        if (priv->cursor_pos.current == idx) {
            struct mp_rect cursor_rect = {
                .x0 = 0,
                .y0 = draw_top - 40,
                .x1 = 700,
                .y1 = draw_top,
            };
            struct ui_triangle_draw_args rect_args = {
                .rects = &cursor_rect,
                .color = 0x00ff00ff,
                .count = 1,
            };
            ui_render_driver_vita.draw_rectangle(ctx, &rect_args);
        }

        struct ui_font_draw_args args = {
            .text = text,
            .size = LAYOUT_ITEM_FONT_SIZE,
            .x = 40,
            .y = draw_top,
            .color = 0xffff00ff,
        };
        ui_render_driver_vita.draw_font(ctx, cache->font, &args);

        draw_top += 40;
    }
}

static void do_move_cursor(struct ui_context *ctx, const struct dpad_act_sepc *spec, int count)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct cache_data *cache = &priv->cache_data;
    if (cache->path_item_count <= 0)
        return;

    int cur_offset = spec->cursor_offset * count;
    int page_offset = spec->page_offset * count;
    if (!cursor_pos_move(&priv->cursor_pos, cur_offset, page_offset, cache->path_item_count))
        return;

    ui_panel_common_invalidate(ctx);
}

static bool do_handle_dpad_trigger(struct ui_context *ctx, enum ui_key_code code, enum ui_key_state state)
{
    const struct dpad_act_sepc *spec = NULL;
    for (size_t i = 0; i < MP_ARRAY_SIZE(dpad_act_spec_list); ++i) {
        if (dpad_act_spec_list[i].key == code) {
            spec = &dpad_act_spec_list[i];
            break;
        }
    }

    if (!spec)
        return false;

    struct priv_panel *priv = ctx->priv_panel;
    switch (state) {
    case UI_KEY_STATE_DOWN:
        priv->pressed_dpad_act = spec;
        priv->presssed_dpad_start_time = ui_panel_common_get_frame_time(ctx);
        priv->pressed_dpad_handled_count = 1;
        do_move_cursor(ctx, spec, 1);
        break;
    case UI_KEY_STATE_UP:
        priv->pressed_dpad_act = NULL;
        priv->presssed_dpad_start_time = 0;
        priv->pressed_dpad_handled_count = 0;
        break;
    }
    return true;
}

static void do_handle_dpad_pressed(struct ui_context *ctx) {
    struct priv_panel *priv = ctx->priv_panel;
    if (!priv->pressed_dpad_act)
        return;

    int delta = ui_panel_common_get_frame_time(ctx)
            - priv->presssed_dpad_start_time
            - DPAD_ACT_TRIGGER_DELAY_US;
    int count = delta / DPAD_ACT_REPEAT_DELAY_US - priv->pressed_dpad_handled_count;
    if (count <= 0)
        return;

    priv->pressed_dpad_handled_count += count;
    do_move_cursor(ctx, priv->pressed_dpad_act, count);
}

static void files_on_key(struct ui_context *ctx, enum ui_key_code code, enum ui_key_state state)
{
    bool done_dpad = do_handle_dpad_trigger(ctx, code, state);
    if (done_dpad)
        return;

    if (state != UI_KEY_STATE_DOWN)
        return;

    switch (code) {
    case UI_KEY_CODE_VITA_VIRTUAL_OK:
        push_path(ctx);
        break;
    case UI_KEY_CODE_VITA_VIRTUAL_CANCEL:
        pop_path(ctx);
        break;
    default:
        break;
    }
}

static void files_on_poll(struct ui_context *ctx)
{
    do_handle_dpad_pressed(ctx);
}

const struct ui_panel ui_panel_files = {
    .priv_size = sizeof(struct priv_panel),
    .init = files_init,
    .uninit = files_uninit,
    .on_show = files_on_show,
    .on_hide = files_on_hide,
    .on_draw = files_on_draw,
    .on_poll = files_on_poll,
    .on_key = files_on_key,
};
