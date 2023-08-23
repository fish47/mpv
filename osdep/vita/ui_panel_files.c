#include "ui_context.h"
#include "ui_device.h"
#include "ui_driver.h"
#include "ui_panel.h"
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

struct dpad_act_spec {
    enum ui_key_code key;
    int cursor_offset;
    int page_offset;
};

static const struct dpad_act_spec dpad_act_spec_list[] = {
    { .key = UI_KEY_CODE_VITA_DPAD_UP, .cursor_offset = -1, .page_offset = 0 },
    { .key = UI_KEY_CODE_VITA_DPAD_DOWN, .cursor_offset = 1, .page_offset = 0 },
    { .key = UI_KEY_CODE_VITA_DPAD_LEFT, .cursor_offset = 0, .page_offset = -1 },
    { .key = UI_KEY_CODE_VITA_DPAD_RIGHT, .cursor_offset = 0, .page_offset = 1, },
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

enum draw_flag {
    DRAW_FLAG_CLIP_NAME = 1,
    DRAW_FLAG_ONLY_CURSOR = 1 << 1,
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
    struct ui_font *font;
    struct path_item *path_items;
    int path_item_count;
    char *tmp_str_buf;
    char *path_str_pool;
};

struct cursor_data {
    int top;
    int current;
};

struct priv_panel {
    bstr work_dir;
    struct cursor_data cursor_pos;
    struct cache_data cache_data;

    int sort_field_idx;
    int sort_field_order;

    struct cursor_data *cursor_pos_stack;
    int cursor_pos_count;

    const struct dpad_act_spec *pressed_dpad_act;
    int64_t presssed_dpad_start_time;
    int pressed_dpad_handled_count;
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

static const char *format_name_text(void *p, char **buf, struct path_item *item)
{
    // it is uncommon to have special characters
    if (item->flags & PATH_ITEM_FLAG_SANTIZIE_NAME) {
        MP_TARRAY_GROW(p, *buf, (item->name_len + 1));
        sanitize_path_name(item->str_name, *buf);
        return *buf;
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
        if (stat((char*) item_path.start, &file_stat) != 0)
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

    DIR *dir = opendir((char*) priv->work_dir.start);
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
        bstr file_path = bstrdup(priv, priv->work_dir);
        join_path(priv, &file_path, item);
        p->path = ta_steal(p, (char*) file_path.start);
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
    char *match_name = (char*) priv->work_dir.start + sep + 1;
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
    if (!priv->cache_data.font)
        ui_render_driver_vita.font_init(ctx, &priv->cache_data.font);
    fill_path_items(priv, NULL, false);
}

static void files_on_hide(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct cache_data *cache = &priv->cache_data;
    if (cache->font)
        ui_render_driver_vita.font_uninit(ctx, &cache->font);
    TA_FREEP(&cache->tmp_str_buf);
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
    struct priv_panel *priv = ctx->priv_panel;
    struct cache_data *cache = &priv->cache_data;

    struct ui_font_draw_args args;
    args.size = LAYOUT_COMMON_TEXT_FONT_SIZE;
    args.color = UI_COLOR_TEXT;
    args.y = LAYOUT_FRAME_TITLE_T + LAYOUT_COMMON_ITEM_TEXT_P;

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
        ui_render_driver_vita.draw_font(ctx, cache->font, &args);
    }
}

static void do_draw_blocks(struct ui_context *ctx)
{
    int count = 0;
    unsigned int colors[3];
    struct mp_rect rects[3];

    colors[count] = UI_COLOR_BLOCK;
    rects[count] = (struct mp_rect) {
        .x0 = LAYOUT_FRAME_ITEMS_L,
        .y0 = LAYOUT_FRAME_TITLE_T,
        .x1 = LAYOUT_FRAME_ITEMS_R,
        .y1 = LAYOUT_FRAME_TITLE_T + LAYOUT_COMMON_ITEM_ROW_H,
    };
    ++count;

    if (has_scroll_bar(ctx)) {
        struct priv_panel *priv = ctx->priv_panel;
        struct cache_data *cache = &priv->cache_data;
        int n = cache->path_item_count;
        int height = LAYOUT_FRAME_SCROLL_BAR_H * LAYOUT_COMMON_ITEM_COUNT / n;
        int offset = LAYOUT_FRAME_SCROLL_BAR_H * priv->cursor_pos.top / n;
        offset = MPMIN(offset, LAYOUT_FRAME_SCROLL_BAR_H - height);

        colors[count] = UI_COLOR_BLOCK;
        rects[count] = (struct mp_rect) {
            .x0 = LAYOUT_FRAME_SCROLL_BAR_L,
            .y0 = LAYOUT_FRAME_SCROLL_BAR_T,
            .x1 = LAYOUT_FRAME_SCROLL_BAR_R,
            .y1 = LAYOUT_FRAME_SCROLL_BAR_B,
        };
        ++count;

        colors[count] = UI_COLOR_MOVABLE;
        rects[count] = (struct mp_rect) {
            .x0 = LAYOUT_FRAME_SCROLL_BAR_L,
            .y0 = LAYOUT_FRAME_SCROLL_BAR_T + offset,
            .x1 = LAYOUT_FRAME_SCROLL_BAR_R,
            .y1 = LAYOUT_FRAME_SCROLL_BAR_T + offset + height,
        };
        ++count;
    }

    struct ui_rectangle_draw_args args = {
        .rects = rects,
        .colors = colors,
        .count = count,
    };
    ui_render_driver_vita.draw_rectangle(ctx, &args);
}

static void do_draw_content(struct ui_context *ctx, int flags, int fields)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct cache_data *cache = &priv->cache_data;
    if (!cache->font)
        return;

    struct ui_font_draw_args args;
    args.size = LAYOUT_COMMON_TEXT_FONT_SIZE;
    args.color = UI_COLOR_TEXT;

    int draw_top = LAYOUT_FRAME_ITEMS_T;
    bool clip_name = (flags & DRAW_FLAG_CLIP_NAME);
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
        if ((flags & DRAW_FLAG_ONLY_CURSOR) && priv->cursor_pos.current == idx) {
            unsigned int cursor_color = UI_COLOR_MOVABLE;
            struct mp_rect cursor_rect = {
                .x0 = LAYOUT_CURSOR_L,
                .y0 = draw_top,
                .x1 = LAYOUT_CURSOR_R,
                .y1 = draw_top + LAYOUT_CURSOR_H,
            };
            struct ui_rectangle_draw_args rect_args = {
                .rects = &cursor_rect,
                .colors = &cursor_color,
                .count = 1,
            };

            if (has_scroll_bar(ctx))
                cursor_rect.x1 -= LAYOUT_FRAME_SCROLL_BAR_W + LAYOUT_FRAME_SCROLL_BAR_MARGIN_L;

            ui_render_driver_vita.draw_rectangle(ctx, &rect_args);
            break;
        }

        struct path_item *item = &cache->path_items[idx];
        args.y = draw_top + LAYOUT_COMMON_ITEM_TEXT_P;

        if (fields & PATH_ITEM_FIELD_NAME) {
            args.x = LAYOUT_ITEM_NAME_L;
            args.text = format_name_text(priv, &cache->tmp_str_buf, item);
            ui_render_driver_vita.draw_font(ctx, cache->font, &args);
        }

        if (fields & PATH_ITEM_FIELD_SIZE) {
            args.x = LAYOUT_ITEM_SIZE_L;
            args.text = item->str_size;
            ui_render_driver_vita.draw_font(ctx, cache->font, &args);
        }

        if (fields & PATH_ITEM_FIELD_DATE) {
            args.x = LAYOUT_ITEM_DATE_L;
            args.text = item->str_date;
            ui_render_driver_vita.draw_font(ctx, cache->font, &args);
        }

        draw_top += LAYOUT_COMMON_ITEM_ROW_H;
    }

    if (clip_name)
        ui_render_driver_vita.clip_end(ctx);
}

static void files_on_draw(struct ui_context *ctx)
{
    do_draw_blocks(ctx);
    do_draw_titles(ctx);
    do_draw_content(ctx, DRAW_FLAG_ONLY_CURSOR, 0);
    do_draw_content(ctx, DRAW_FLAG_CLIP_NAME, PATH_ITEM_FIELD_NAME);
    do_draw_content(ctx, 0, PATH_ITEM_FIELD_DATE | PATH_ITEM_FIELD_SIZE);
}

static void do_move_cursor(struct ui_context *ctx,
                           const struct dpad_act_spec *spec, int count)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct cache_data *cache = &priv->cache_data;
    if (cache->path_item_count <= 0)
        return;

    int offset_c = spec->cursor_offset * count;
    int offset_p = spec->page_offset * count;
    bool changed = cursor_pos_move(&priv->cursor_pos,
                                   offset_c, offset_p, cache->path_item_count);
    if (changed)
        ui_panel_common_invalidate(ctx);
}

static bool do_handle_dpad_trigger(struct ui_context *ctx, struct ui_key *key)
{
    const struct dpad_act_spec *spec = NULL;
    for (size_t i = 0; i < MP_ARRAY_SIZE(dpad_act_spec_list); ++i) {
        if (dpad_act_spec_list[i].key == key->code) {
            spec = &dpad_act_spec_list[i];
            break;
        }
    }

    if (!spec)
        return false;

    struct priv_panel *priv = ctx->priv_panel;
    switch (key->state) {
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

static void do_handle_dpad_pressed(struct ui_context *ctx)
{
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

static void do_change_cmp_func(struct ui_context *ctx, int f_offset, int flip)
{
    struct priv_panel *priv = ctx->priv_panel;
    size_t field_count = MP_ARRAY_SIZE(field_title_spec_list);
    int new_idx = MPCLAMP(priv->sort_field_idx + f_offset, 0, field_count - 1);
    int new_order = (priv->sort_field_order ^ flip);
    if (new_idx == priv->sort_field_idx && new_order == priv->sort_field_order)
        return;

    priv->sort_field_idx = new_idx;
    priv->sort_field_order = new_order;
    do_sort_path_items(priv);
    ui_panel_common_invalidate(ctx);
}

static void files_on_key(struct ui_context *ctx, struct ui_key *key)
{
    bool done_dpad = do_handle_dpad_trigger(ctx, key);
    if (done_dpad)
        return;

    if (key->state != UI_KEY_STATE_DOWN)
        return;

    switch (key->code) {
    case UI_KEY_CODE_VITA_VIRTUAL_OK:
        push_path(ctx);
        break;
    case UI_KEY_CODE_VITA_VIRTUAL_CANCEL:
        pop_path(ctx);
        break;
    case UI_KEY_CODE_VITA_ACTION_TRIANGLE:
        do_change_cmp_func(ctx, 0, 1);
        break;
    case UI_KEY_CODE_VITA_TRIGGER_L:
        do_change_cmp_func(ctx, -1, 0);
        break;
    case UI_KEY_CODE_VITA_TRIGGER_R:
        do_change_cmp_func(ctx, 1, 0);
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
    .uninit = NULL,
    .on_show = files_on_show,
    .on_hide = files_on_hide,
    .on_draw = files_on_draw,
    .on_poll = files_on_poll,
    .on_key = files_on_key,
};
