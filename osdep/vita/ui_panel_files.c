#include "ui_context.h"
#include "ui_device.h"
#include "ui_driver.h"
#include "ui_panel.h"
#include "key_helper.h"
#include "shape_draw.h"
#include "misc/bstr.h"
#include "ta/ta_talloc.h"

#include <time.h>
#include <dirent.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PATH_SEP "/"
#define PATH_DIR_PARENT ".."
#define PATH_DIR_CURRENT "."
#define PATH_UNKNOWN_SIZE "--"
#define PATH_ESCAPED_SPACE ' '

#define LAYOUT_COMMON_TEXT_FONT_SIZE 26
#define LAYOUT_COMMON_ITEM_TEXT_P 26
#define LAYOUT_COMMON_ITEM_ROW_H 32
#define LAYOUT_COMMON_ITEM_COUNT 14

#define UI_COLOR_TEXT       0xffffffff
#define UI_COLOR_MOVABLE    0xff722B72
#define UI_COLOR_BLOCK      0xff343434

#define LAYOUT_MAIN_W VITA_SCREEN_W
#define LAYOUT_MAIN_H VITA_SCREEN_H

#define LAYOUT_FRAME_MAIN_PADDING_X 28
#define LAYOUT_FRAME_MAIN_PADDING_Y 28

#define LAYOUT_FRAME_ITEMS_PADDING_X 20
#define LAYOUT_FRAME_ITEMS_H (LAYOUT_COMMON_ITEM_COUNT * LAYOUT_COMMON_ITEM_ROW_H)
#define LAYOUT_FRAME_ITEMS_L LAYOUT_FRAME_MAIN_PADDING_X
#define LAYOUT_FRAME_ITEMS_R (LAYOUT_MAIN_W - LAYOUT_FRAME_MAIN_PADDING_X)
#define LAYOUT_FRAME_ITEMS_B (LAYOUT_MAIN_H - LAYOUT_FRAME_MAIN_PADDING_Y)
#define LAYOUT_FRAME_ITEMS_T (LAYOUT_FRAME_ITEMS_B - LAYOUT_FRAME_ITEMS_H)

#define LAYOUT_FRAME_SCROLL_BAR_MARGIN_L 6
#define LAYOUT_FRAME_SCROLL_BAR_W 8
#define LAYOUT_FRAME_SCROLL_BAR_H LAYOUT_FRAME_ITEMS_H
#define LAYOUT_FRAME_SCROLL_BAR_T LAYOUT_FRAME_ITEMS_T
#define LAYOUT_FRAME_SCROLL_BAR_L (LAYOUT_FRAME_ITEMS_R - LAYOUT_FRAME_SCROLL_BAR_W)
#define LAYOUT_FRAME_SCROLL_BAR_R LAYOUT_FRAME_ITEMS_R
#define LAYOUT_FRAME_SCROLL_BAR_B LAYOUT_FRAME_ITEMS_B

#define LAYOUT_FRAME_TITLE_T (LAYOUT_FRAME_MAIN_PADDING_Y)

#define LAYOUT_ITEM_SIZE_W 130
#define LAYOUT_ITEM_DATE_W 260
#define LAYOUT_ITEM_NAME_W (LAYOUT_MAIN_W \
                            - LAYOUT_FRAME_MAIN_PADDING_X * 2 \
                            - LAYOUT_FRAME_ITEMS_PADDING_X * 2 \
                            - LAYOUT_ITEM_SIZE_W \
                            - LAYOUT_ITEM_DATE_W)

#define LAYOUT_ITEM_NAME_L (LAYOUT_FRAME_MAIN_PADDING_X + LAYOUT_FRAME_ITEMS_PADDING_X)
#define LAYOUT_ITEM_SIZE_L (LAYOUT_ITEM_NAME_L + LAYOUT_ITEM_NAME_W)
#define LAYOUT_ITEM_DATE_L (LAYOUT_ITEM_SIZE_L + LAYOUT_ITEM_SIZE_W)

#define LAYOUT_ITEM_NAME_CLIP_L LAYOUT_ITEM_NAME_L
#define LAYOUT_ITEM_NAME_CLIP_R (LAYOUT_ITEM_NAME_L + LAYOUT_ITEM_NAME_W - 20)

#define LAYOUT_CURSOR_L (LAYOUT_FRAME_MAIN_PADDING_X)
#define LAYOUT_CURSOR_R (LAYOUT_MAIN_W - LAYOUT_CURSOR_L)
#define LAYOUT_CURSOR_H LAYOUT_COMMON_ITEM_ROW_H

#define UI_STRING_TITLE_NAME "Name"
#define UI_STRING_TITLE_SIZE "Size"
#define UI_STRING_TITLE_DATE "Date"
#define UI_STRING_TITLE_SORT_ASC "\xe2\x96\xb2" // U+25b2
#define UI_STRING_TITLE_SORT_DESC "\xe2\x96\xbc" // U+25bc

#define DPAD_ACT_TRIGGER_DELAY_US   (600 * 1000)
#define DPAD_ACT_REPEAT_DELAY_US    (40 * 1000)

struct sort_act {
    int field_offset;
    int order_flip;
};

struct dpad_act {
    int offset_cursor;
    int offset_page;
};

static void on_key_dpad(void *p, const void *data, int repeat);
static void on_key_sort(void *p, const void *data, int repeat);
static void on_key_ok(void *p, const void *data, int repeat);
static void on_key_cancel(void *p, const void *data, int repeat);

static const struct key_helper_spec key_helper_spec_list[] = {
    { .key = UI_KEY_CODE_VITA_DPAD_UP, .callback = on_key_dpad, .repeatable = true, .data = &(struct dpad_act) { .offset_cursor = -1 } },
    { .key = UI_KEY_CODE_VITA_DPAD_DOWN, .callback = on_key_dpad, .repeatable = true, .data = &(struct dpad_act) { .offset_cursor = 1 } },
    { .key = UI_KEY_CODE_VITA_DPAD_LEFT, .callback = on_key_dpad, .repeatable = true, .data = &(struct dpad_act) { .offset_page = -1 } },
    { .key = UI_KEY_CODE_VITA_DPAD_RIGHT, .callback = on_key_dpad, .repeatable = true, .data = &(struct dpad_act) { .offset_page = 1 } },
    { .key = UI_KEY_CODE_VITA_VIRTUAL_OK, .callback = on_key_ok },
    { .key = UI_KEY_CODE_VITA_VIRTUAL_CANCEL, .callback = on_key_cancel },
    { .key = UI_KEY_CODE_VITA_ACTION_TRIANGLE, .callback = on_key_sort, .data = &(struct sort_act) { .order_flip = 1 } },
    { .key = UI_KEY_CODE_VITA_TRIGGER_L, .callback = on_key_sort, .data = &(struct sort_act) { .field_offset = -1 } },
    { .key = UI_KEY_CODE_VITA_TRIGGER_R, .callback = on_key_sort, .data = &(struct sort_act) { .field_offset = 1 } },
};

struct size_spec {
    uint64_t size;
    char *name;
};

static const struct size_spec size_spec_list[] = {
    { .size = 1, .name = "B" },
    { .size = 1 << 10, .name = "KB" },
    { .size = 1 << 20, .name = "MB" },
    { .size = 1 << 30, .name = "GB" },
};


static int cmp_name_asc(const void *l, const void *r);
static int cmp_name_desc(const void *l, const void *r);
static int cmp_size_asc(const void *l, const void *r);
static int cmp_size_desc(const void *l, const void *r);
static int cmp_date_asc(const void *l, const void *r);
static int cmp_date_desc(const void *l, const void *r);

struct field_title_spec {
    int (*cmp_func[2])(const void *pa, const void *pb);
    char *draw_name;
    int draw_x;
};

static const struct field_title_spec field_title_spec_list[] = {
    {
        .cmp_func = { cmp_name_asc, cmp_name_desc },
        .draw_name = UI_STRING_TITLE_NAME,
        .draw_x = LAYOUT_ITEM_NAME_L,
    },
    {
        .cmp_func = { cmp_size_asc, cmp_size_desc },
        .draw_name = UI_STRING_TITLE_SIZE,
        .draw_x = LAYOUT_ITEM_SIZE_L,
    },
    {
        .cmp_func = { cmp_date_asc, cmp_date_desc },
        .draw_name = UI_STRING_TITLE_DATE,
        .draw_x = LAYOUT_ITEM_DATE_L,
    },
};

enum path_item_field {
    PATH_ITEM_FIELD_NAME = 1,
    PATH_ITEM_FIELD_SIZE = 1 << 1,
    PATH_ITEM_FIELD_DATE = 1 << 2,
};

enum path_item_flag {
    PATH_ITEM_FLAG_SANTIZIE_NAME = 1,
    PATH_ITEM_FLAG_TYPE_DIR = 1 << 1,
    PATH_ITEM_FLAG_TYPE_FILE = 1 << 2,
};

struct path_item {
    int flags;
    const char *str_name;
    const char *str_date;
    const char *str_size;
    int name_len;
    time_t path_date;
    size_t file_size;
};

struct cache_data {
    struct path_item *path_items;
    int path_item_count;
    char *path_str_pool;
    void *tmp_buffer;
};

struct cursor_data {
    int top;
    int current;
};

struct priv_panel {
    bstr work_dir;
    struct cursor_data cursor_pos;
    struct cache_data cache_data;
    struct key_helper_ctx key_ctx;

    int sort_field_idx;
    int sort_field_order;

    struct cursor_data *cursor_pos_stack;
    int cursor_pos_count;
};

static bool is_special_white_space(char c)
{
    return c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool sanitize_path_name(const char *name, char *out)
{
    int i = 0;
    bool stop = false;
    bool changed = false;
    while (!stop) {
        char ch = name[i];
        if (!ch) {
            // make sure to write the last terminator
            stop = true;
        } else if (is_special_white_space(ch)) {
            changed = true;
            ch = PATH_ESCAPED_SPACE;
        }

        if (out) {
            out[i] = ch;
        } else if (changed) {
            // stop if special character check failed
            break;
        }

        ++i;
    }

    return changed;
}

static const char *format_name_text(struct priv_panel *priv, struct path_item *item)
{
    // it is uncommon to have special characters
    if (item->flags & PATH_ITEM_FLAG_SANTIZIE_NAME) {
        char *buf = priv->cache_data.tmp_buffer;
        MP_TARRAY_GROW(priv, buf, (item->name_len + 1));
        sanitize_path_name(item->str_name, buf);
        priv->cache_data.tmp_buffer = buf;
        return buf;
    }

    return item->str_name;
}

static int format_size_text(uint64_t bytes, char *buf, size_t limit)
{
    for (size_t i = 0; i < MP_ARRAY_SIZE(size_spec_list); ++i) {
        const struct size_spec *spec = &size_spec_list[i];
        if (bytes < spec->size) {
            spec = &size_spec_list[MPMAX(i, 1) - 1];
            uint64_t num = bytes / spec->size;
            return snprintf(buf, limit, "%"PRIu64"%s", num, spec->name);
        }
    }
    return 0;
}

static int format_date_text(struct tm *tm, char *buf, size_t limit)
{
    return snprintf(buf, limit, "%04u-%02u-%02u %02u:%02u",
                    (tm->tm_year + 1900), (tm->tm_mon + 1), tm->tm_mday,
                    tm->tm_hour, tm->tm_min);
}

static int resolve_path_item_flags(const char *name, struct stat *s)
{
    int flags = 0;
    if (S_ISDIR(s->st_mode))
        flags |= PATH_ITEM_FLAG_TYPE_DIR;
    if (S_ISREG(s->st_mode))
        flags |= PATH_ITEM_FLAG_TYPE_FILE;
    if (sanitize_path_name(name, NULL))
        flags |= PATH_ITEM_FLAG_SANTIZIE_NAME;
    return flags;
}

static int do_cmp_path_item(const void *l, const void *r,
                            enum path_item_field type, bool reverse)
{
    const struct path_item* lhs = l;
    const struct path_item* rhs = r;

    // show directories first
    int l_dir = lhs->flags & PATH_ITEM_FLAG_TYPE_DIR;
    int r_dir = rhs->flags & PATH_ITEM_FLAG_TYPE_DIR;
    if (l_dir != r_dir)
        return r_dir - l_dir;

    int tmp = 0;
    int result = 0;
    switch (type) {
    case PATH_ITEM_FIELD_NAME:
        // stdlib should be fine to handle utf8 string comparison
        result = strcmp(lhs->str_name, rhs->str_name);
        break;
    case PATH_ITEM_FIELD_DATE:
        result = (lhs->path_date == rhs->path_date) ? 0
                : (lhs->path_date < rhs->path_date) ? -1 : 1;
        break;
    case PATH_ITEM_FIELD_SIZE:
        result = (lhs->file_size == rhs->file_size) ? 0
                : (lhs->file_size < rhs->file_size) ? -1 : 1;
        break;
    }

    return reverse ? -result : result;
}

static int cmp_name_asc(const void *l, const void *r)
{
    return do_cmp_path_item(l, r, PATH_ITEM_FIELD_NAME, false);
}

static int cmp_name_desc(const void *l, const void *r)
{
    return do_cmp_path_item(l, r, PATH_ITEM_FIELD_NAME, true);
}

static int cmp_size_asc(const void *l, const void *r)
{
    return do_cmp_path_item(l, r, PATH_ITEM_FIELD_SIZE, false);
}

static int cmp_size_desc(const void *l, const void *r)
{
    return do_cmp_path_item(l, r, PATH_ITEM_FIELD_SIZE, true);
}

static int cmp_date_asc(const void *l, const void *r)
{
    return do_cmp_path_item(l, r, PATH_ITEM_FIELD_DATE, false);
}

static int cmp_date_desc(const void *l, const void *r)
{
    return do_cmp_path_item(l, r, PATH_ITEM_FIELD_DATE, true);
}

static void cursor_pos_relocate(struct priv_panel *priv, char *name)
{
    int cursor = priv->cursor_pos.current;
    if (cursor >= priv->cache_data.path_item_count)
        goto relocate;

    const char *cursor_name = priv->cache_data.path_items[cursor].str_name;
    if (strcmp(cursor_name, name) != 0)
        goto relocate;

    return;

relocate:
    for (int i = 0; i < priv->cache_data.path_item_count; ++i) {
        if (strcmp(name, priv->cache_data.path_items[i].str_name) == 0) {
            priv->cursor_pos = (struct cursor_data) { .current = i, .top = i };
            return;
        }
    }

    priv->cursor_pos = (struct cursor_data) { .current = 0, .top = 0 };
}

static void do_pack_str(void *p, char **pool, void *s, int len, int *io_offset)
{
    if (len <= 0)
        return;

    // copy string with terminator
    int request = len + 1;
    int ensure = *io_offset + request;
    MP_TARRAY_GROW(p, *pool, ensure);
    memcpy(*pool + *io_offset, s, request);
    *io_offset += request;
}

static void do_sort_path_items(struct priv_panel *priv)
{
    struct cache_data *cache = &priv->cache_data;
    const struct field_title_spec *spec = &field_title_spec_list[priv->sort_field_idx];
    qsort(cache->path_items, cache->path_item_count,
          sizeof(struct path_item),
          spec->cmp_func[priv->sort_field_order]);
}

static void do_fill_path_items(struct priv_panel *priv, DIR *dir)
{
    bstr item_path = bstrdup(priv, priv->work_dir);
    int item_cut_idx = item_path.len + 1;
    bstr_xappend(priv, &item_path, bstr0(PATH_SEP));

    // a hack to suppress 'casting int to char*' warning
    const char *base_zero = NULL;

    char buf[40];
    int offset = 0;
    struct cache_data *cache = &priv->cache_data;
    while (true) {
        struct dirent *d = readdir(dir);
        if (!d)
            break;

        if (strcmp(d->d_name, PATH_DIR_PARENT) == 0)
            continue;
        if (strcmp(d->d_name, PATH_DIR_CURRENT) == 0)
            continue;

        // build absolute path
        bstr name = bstr0(d->d_name);
        item_path.len = item_cut_idx;
        item_path.start[item_cut_idx] = 0;
        bstr_xappend(priv, &item_path, name);

        struct stat file_stat;
        if (stat(BSTR_CAST(item_path), &file_stat) != 0)
            continue;

        // name
        int offset_name = offset;
        do_pack_str(priv, &cache->path_str_pool, name.start, name.len, &offset);

        // date
        time_t date = file_stat.st_mtime;
        struct tm file_tm = *localtime(&date);
        int offset_date = offset;
        int len_date = format_date_text(&file_tm, buf, sizeof(buf));
        do_pack_str(priv, &cache->path_str_pool, buf, len_date, &offset);

        // size
        int offset_size = offset;
        int flags = resolve_path_item_flags(d->d_name, &file_stat);
        if (flags & PATH_ITEM_FLAG_TYPE_FILE) {
            int len_size = format_size_text(file_stat.st_size, buf, sizeof(buf));
            do_pack_str(priv, &cache->path_str_pool, buf, len_size, &offset);
        }

        // remember offsets before iteration is done
        MP_TARRAY_APPEND(priv, cache->path_items, cache->path_item_count,
                         (struct path_item) {
            .flags = flags,
            .str_name = base_zero + offset_name,
            .str_date = base_zero + offset_date,
            .str_size = base_zero + offset_size,
            .name_len = name.len,
            .path_date = date,
            .file_size = file_stat.st_size,
        });
    }

    // translate offsets to addresses
    long translate_offset = cache->path_str_pool - base_zero;
    for (int i = 0; i < cache->path_item_count; ++i) {
        struct path_item *item = &cache->path_items[i];
        item->str_name += translate_offset;
        item->str_date += translate_offset;
        item->str_size = (item->flags & PATH_ITEM_FLAG_TYPE_FILE)
                ? item->str_size + translate_offset
                : PATH_UNKNOWN_SIZE;
    }

    do_sort_path_items(priv);

    item_path.len = 0;
    TA_FREEP(&item_path.start);
}

static void fill_path_items(struct priv_panel *priv, char *match_name, bool reset)
{
    struct cache_data *cache = &priv->cache_data;
    cache->path_item_count = 0;

    if (!priv->work_dir.len)
        return;

    DIR *dir = opendir(BSTR_CAST(priv->work_dir));
    if (dir) {
        do_fill_path_items(priv, dir);
        closedir(dir);
    }

    if (match_name) {
        cursor_pos_relocate(priv, match_name);
    } else if (reset) {
        priv->cursor_pos = (struct cursor_data) { .current = 0, .top = 0 };
    } else {
        int max_pos = MPMAX(cache->path_item_count - 1, 0);
        struct cursor_data *cursor = &priv->cursor_pos;
        cursor->current = MPCLAMP(cursor->current, 0, max_pos);
        cursor->top = MPCLAMP(cursor->top, 0, max_pos);
    }
}

static bool cursor_pos_move(struct cursor_data *pos,
                            int cur_offset, int page_offset, int count)
{
    if (cur_offset != 0) {
        int new_cur = MPCLAMP(pos->current + cur_offset, 0, count - 1);
        if (new_cur == pos->current)
            return false;

        // adjust viewport if cursor is out of bound
        pos->current = new_cur;
        if (pos->top > pos->current)
            pos->top = pos->current;
        else if (pos->top < pos->current - (LAYOUT_COMMON_ITEM_COUNT - 1))
            pos->top = MPMAX(pos->current - (LAYOUT_COMMON_ITEM_COUNT - 1), 0);
        return true;
    } else if (page_offset != 0) {
        // stop flipping to next page if we are on the last page
        int move_count = page_offset * LAYOUT_COMMON_ITEM_COUNT;
        if (pos->top + move_count >= count)
            return false;

        int max_top = MPMAX(0, count - LAYOUT_COMMON_ITEM_COUNT);
        int new_top = MPCLAMP(pos->top + move_count, 0, max_top);
        if (new_top == pos->top)
            return false;

        // keep cursor top offset when page is flipped
        int delta = pos->current - pos->top;
        pos->top = new_top;
        pos->current = MPCLAMP(new_top + delta, 0, max_top);
        return true;
    }
    return false;
}

static void join_path(void *p, bstr *path, struct path_item *item)
{
    bstr_xappend(p, path, bstr0(PATH_SEP));
    bstr_xappend(p, path, bstr0(item->str_name));
}

static void push_path(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct cursor_data *pos = &priv->cursor_pos;
    struct path_item *item = &priv->cache_data.path_items[pos->current];
    if (item->flags & PATH_ITEM_FLAG_TYPE_DIR) {
        // remember cursor position in case of backward navigation
        MP_TARRAY_APPEND(priv, priv->cursor_pos_stack, priv->cursor_pos_count, *pos);
        join_path(priv, &priv->work_dir, item);
        fill_path_items(priv, NULL, true);
        ui_panel_common_invalidate(ctx);
    } else if (item->flags & PATH_ITEM_FLAG_TYPE_FILE) {
        struct ui_panel_player_init_params *p = talloc_ptrtype(priv, p);
        uint32_t combo = UI_KEY_CODE_VITA_TRIGGER_L | UI_KEY_CODE_VITA_TRIGGER_R;
        bstr file_path = bstrdup(priv, priv->work_dir);
        join_path(priv, &file_path, item);
        p->file_path = ta_steal(p, BSTR_CAST(file_path));
        p->enable_perf = ui_panel_common_check_pressed_keys(ctx, combo);
        ui_panel_common_push(ctx, &ui_panel_player, p);
    }
}

static void pop_path(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    if (priv->cursor_pos_count <= 0) {
        ui_panel_common_pop(ctx);
        return;
    }

    int sep = bstrrchr(priv->work_dir, PATH_SEP[0]);
    if (sep < 0)
        return;

    // pop last path segment
    priv->work_dir.start[sep] = 0;
    priv->work_dir.len = sep;

    // try to relocate popped position data if it is not matched
    char *match_name = BSTR_CAST(priv->work_dir) + sep + 1;
    MP_TARRAY_POP(priv->cursor_pos_stack, priv->cursor_pos_count, &priv->cursor_pos);
    fill_path_items(priv, match_name, false);
    ui_panel_common_invalidate(ctx);
}

static bool files_init(struct ui_context *ctx, void *p)
{
    struct priv_panel *priv = ctx->priv_panel;
    const char *init_dir = ui_platform_driver_vita.get_files_dir(ctx);
    priv->work_dir.len = 0;
    bstr_xappend(priv, &priv->work_dir, bstr0(init_dir));
    priv->sort_field_idx = 0;
    priv->sort_field_order = 0;
    return true;
}

static void files_on_show(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    fill_path_items(priv, NULL, false);
}

static void files_on_hide(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct cache_data *cache = &priv->cache_data;
    TA_FREEP(&cache->tmp_buffer);
    TA_FREEP(&cache->path_str_pool);
    TA_FREEP(&cache->path_items);
    cache->path_item_count = 0;
}

static bool has_scroll_bar(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct cache_data *cache = &priv->cache_data;
    return (cache->path_item_count > LAYOUT_COMMON_ITEM_COUNT);
}

static void do_draw_titles(struct ui_context *ctx)
{
    struct ui_font *font = ui_panel_common_get_font(ctx);
    if (!font)
        return;

    struct priv_panel *priv = ctx->priv_panel;
    struct cache_data *cache = &priv->cache_data;
    struct ui_font_draw_args args = {
        .size = LAYOUT_COMMON_TEXT_FONT_SIZE,
        .color = UI_COLOR_TEXT,
        .y = LAYOUT_FRAME_TITLE_T + LAYOUT_COMMON_ITEM_TEXT_P,
    };

    char buf[30];
    for (size_t i = 0; i < MP_ARRAY_SIZE(field_title_spec_list); ++i) {
        const struct field_title_spec *spec = &field_title_spec_list[i];
        if (priv->sort_field_idx == i) {
            const char *sign = priv->sort_field_order
                ? UI_STRING_TITLE_SORT_ASC
                : UI_STRING_TITLE_SORT_DESC;
            buf[0] = 0;
            strcat(strcat(buf, spec->draw_name), sign);
            args.text = buf;
        } else {
            args.text = spec->draw_name;
        }
        args.x = spec->draw_x;
        ui_render_driver_vita.draw_font(ctx, font, &args);
    }
}

static void do_draw_content(struct ui_context *ctx,
                            struct shape_draw_item *shape_item,
                            int *shape_count, int fields)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct cache_data *cache = &priv->cache_data;
    struct ui_font *font = ui_panel_common_get_font(ctx);
    if (!font)
        return;

    struct ui_font_draw_args args;
    args.size = LAYOUT_COMMON_TEXT_FONT_SIZE;
    args.color = UI_COLOR_TEXT;

    int draw_top = LAYOUT_FRAME_ITEMS_T;
    bool clip_name = (fields & PATH_ITEM_FIELD_NAME);
    if (clip_name) {
        struct mp_rect rect = {
            .x0 = LAYOUT_ITEM_NAME_CLIP_L,
            .y0 = draw_top,
            .x1 = LAYOUT_ITEM_NAME_CLIP_R,
            .y1 = LAYOUT_MAIN_H,
        };
        ui_render_driver_vita.clip_start(ctx, &rect);
    }

    for (int i = 0; i < LAYOUT_COMMON_ITEM_COUNT; ++i) {
        int idx = priv->cursor_pos.top + i;
        if (idx >= cache->path_item_count)
            break;

        // cursor
        if (shape_item && priv->cursor_pos.current == idx) {
            int cursor_right = LAYOUT_CURSOR_R;
            if (has_scroll_bar(ctx))
                cursor_right -= LAYOUT_FRAME_SCROLL_BAR_W + LAYOUT_FRAME_SCROLL_BAR_MARGIN_L;

            ++(*shape_count);
            (*shape_item) = (struct shape_draw_item) {
                .type = SHAPE_DRAW_TYPE_RECT_FILL,
                .color = UI_COLOR_MOVABLE,
                .shape.rect = (struct shape_draw_rect) {
                    .x0 = LAYOUT_CURSOR_L,
                    .y0 = draw_top,
                    .x1 = cursor_right,
                    .y1 = draw_top + LAYOUT_CURSOR_H,
                }
            };
            break;
        }

        struct path_item *item = &cache->path_items[idx];
        args.y = draw_top + LAYOUT_COMMON_ITEM_TEXT_P;

        if (fields & PATH_ITEM_FIELD_NAME) {
            args.x = LAYOUT_ITEM_NAME_L;
            args.text = format_name_text(priv, item);
            ui_render_driver_vita.draw_font(ctx, font, &args);
        }

        if (fields & PATH_ITEM_FIELD_SIZE) {
            args.x = LAYOUT_ITEM_SIZE_L;
            args.text = item->str_size;
            ui_render_driver_vita.draw_font(ctx, font, &args);
        }

        if (fields & PATH_ITEM_FIELD_DATE) {
            args.x = LAYOUT_ITEM_DATE_L;
            args.text = item->str_date;
            ui_render_driver_vita.draw_font(ctx, font, &args);
        }

        draw_top += LAYOUT_COMMON_ITEM_ROW_H;
    }

    if (clip_name)
        ui_render_driver_vita.clip_end(ctx);
}


static void do_draw_shapes(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct shape_draw_item shapes[4];
    int count = 0;

    shapes[count++] = (struct shape_draw_item) {
        .type = SHAPE_DRAW_TYPE_RECT_FILL,
        .color = UI_COLOR_BLOCK,
        .shape.rect = (struct shape_draw_rect) {
            .x0 = LAYOUT_FRAME_ITEMS_L,
            .y0 = LAYOUT_FRAME_TITLE_T,
            .x1 = LAYOUT_FRAME_ITEMS_R,
            .y1 = LAYOUT_FRAME_TITLE_T + LAYOUT_COMMON_ITEM_ROW_H,
        }
    };

    if (has_scroll_bar(ctx)) {
        struct cache_data *cache = &priv->cache_data;
        int n = cache->path_item_count;
        int height = LAYOUT_FRAME_SCROLL_BAR_H * LAYOUT_COMMON_ITEM_COUNT / n;
        int offset = LAYOUT_FRAME_SCROLL_BAR_H * priv->cursor_pos.top / n;
        offset = MPMIN(offset, LAYOUT_FRAME_SCROLL_BAR_H - height);

        shapes[count++] = (struct shape_draw_item) {
            .type = SHAPE_DRAW_TYPE_RECT_FILL,
            .color = UI_COLOR_BLOCK,
            .shape.rect = (struct shape_draw_rect) {
                .x0 = LAYOUT_FRAME_SCROLL_BAR_L,
                .y0 = LAYOUT_FRAME_SCROLL_BAR_T,
                .x1 = LAYOUT_FRAME_SCROLL_BAR_R,
                .y1 = LAYOUT_FRAME_SCROLL_BAR_B,
            }
        };

        shapes[count++] = (struct shape_draw_item) {
            .type = SHAPE_DRAW_TYPE_RECT_FILL,
            .color = UI_COLOR_MOVABLE,
            .shape.rect = (struct shape_draw_rect) {
                .x0 = LAYOUT_FRAME_SCROLL_BAR_L,
                .y0 = LAYOUT_FRAME_SCROLL_BAR_T + offset,
                .x1 = LAYOUT_FRAME_SCROLL_BAR_R,
                .y1 = LAYOUT_FRAME_SCROLL_BAR_T + offset + height,
            }
        };
    }

    do_draw_content(ctx, &shapes[count], &count, 0);

    shape_draw_commit(ctx, shapes, count);
}


static void files_on_draw(struct ui_context *ctx)
{
    do_draw_shapes(ctx);
    do_draw_titles(ctx);
    do_draw_content(ctx, NULL, NULL, PATH_ITEM_FIELD_NAME);
    do_draw_content(ctx, NULL, NULL, PATH_ITEM_FIELD_DATE | PATH_ITEM_FIELD_SIZE);
}

static void on_key_ok(void *p, const void *data, int repeat)
{
    push_path(p);
}

static void on_key_cancel(void *p, const void *data, int repeat)
{
    pop_path(p);
}

static void on_key_dpad(void *p, const void *data, int repeat)
{
    const struct dpad_act *act = data;
    struct ui_context *ctx = p;
    struct priv_panel *priv = ctx->priv_panel;
    struct cache_data *cache = &priv->cache_data;
    if (cache->path_item_count <= 0)
        return;

    int count = MPMAX(repeat, 1);
    bool changed = cursor_pos_move(&priv->cursor_pos,
                                   act->offset_cursor * count,
                                   act->offset_page * count,
                                   cache->path_item_count);
    if (changed)
        ui_panel_common_invalidate(ctx);
}

static void on_key_sort(void *p, const void *data, int repeat)
{
    const struct sort_act *act = data;
    struct ui_context *ctx = p;
    struct priv_panel *priv = ctx->priv_panel;
    size_t field_count = MP_ARRAY_SIZE(field_title_spec_list);
    int new_idx = MPCLAMP(priv->sort_field_idx + act->field_offset, 0, field_count - 1);
    int new_order = (priv->sort_field_order ^ act->order_flip);
    if (new_idx == priv->sort_field_idx && new_order == priv->sort_field_order)
        return;

    priv->sort_field_idx = new_idx;
    priv->sort_field_order = new_order;
    do_sort_path_items(priv);
    ui_panel_common_invalidate(ctx);
}

static void files_on_key(struct ui_context *ctx, struct ui_key *key)
{
    struct priv_panel *priv = ctx->priv_panel;
    key_helper_dispatch(&priv->key_ctx, key,
                        ui_panel_common_get_frame_time(ctx),
                        key_helper_spec_list,
                        MP_ARRAY_SIZE(key_helper_spec_list),
                        ctx);
}

static void files_on_poll(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    int64_t time = ui_panel_common_get_frame_time(ctx);
    key_helper_poll(&priv->key_ctx, time, ctx);
}

const struct ui_panel ui_panel_files = {
    .priv_size = sizeof(struct priv_panel),
    .init = files_init,
    .uninit = NULL,
    .on_show = files_on_show,
    .on_hide = files_on_hide,
    .on_draw = files_on_draw,
    .on_poll = files_on_poll,
    .on_key = files_on_key,
};
