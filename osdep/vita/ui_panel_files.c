#include "ui_context.h"
#include "ui_device.h"
#include "ui_driver.h"
#include "ui_panel.h"
#include "misc/bstr.h"
#include "ta/ta_talloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PATH_SEP "/"
#define PATH_DIR_PARENT ".."
#define PATH_DIR_CURRENT "."
#define PATH_UNKNOWN_SIZE "--"
#define PATH_ESCAPED_SPACE ' '

#define LAYOUT_ITEM_COUNT 10

#define LAYOUT_ITEM_NAME_L 40
#define LAYOUT_ITEM_SIZE_L 500
#define LAYOUT_ITEM_DATE_L 700
#define LAYOUT_ITEM_TEXT_T 30
#define LAYOUT_ITEM_TEXT_FONT_SIZE 30

#define LAYOUT_ITEM_CURSOR_L 0
#define LAYOUT_ITEM_CURSOR_T 0
#define LAYOUT_ITEM_CURSOR_W 800
#define LAYOUT_ITEM_CURSOR_H 40

#define LAYOUT_ITEM_TEXT_COLOR 0xffffffff
#define LAYOUT_ITEM_CURSOR_COLOR 0x722B72ff

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
    int size;
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

enum cmp_field_type {
    CMP_FIELD_TYPE_NAME,
    CMP_FIELD_TYPE_SIZE,
    CMP_FIELD_TYPE_DATE,
};

typedef int (*cmp_func)(const void *pa, const void *pb);
static const cmp_func cmp_func_list[][2] = {
    { cmp_name_asc, cmp_name_desc },
    { cmp_size_asc, cmp_size_desc },
    { cmp_date_asc, cmp_date_desc },
};

enum path_item_flag {
    PATH_ITEM_FLAG_SANTIZIE_NAME = 1,
    PATH_ITEM_FLAG_TYPE_DIR = 1 << 1,
    PATH_ITEM_FLAG_TYPE_FILE = 1 << 2,
};

struct path_item {
    char *name;
    int length;
    int flags;

    uint16_t date_year;
    uint8_t date_month;
    uint8_t date_day;
    uint8_t date_hour;
    uint8_t date_minute;
    uint8_t size_type : 3;
    uint16_t size_num : 13;
};

struct cache_data {
    struct ui_font *font;
    struct path_item *path_items;
    int path_item_count;
    char *tmp_str_buf;
    char *path_name_pool;
};

struct cursor_data {
    int top;
    int current;
};

struct priv_panel {
    bstr work_dir;
    struct cursor_data cursor_pos;
    struct cache_data cache_data;
    cmp_func cmp_func;

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

static bool sanitize_path_name(char *name, char *out)
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

static void resolve_path_size_data(uint64_t bytes, uint32_t *num, uint8_t *type)
{
    for (size_t i = 0; i < MP_ARRAY_SIZE(size_spec_list); ++i) {
        const struct size_spec *spec = &size_spec_list[i];
        if (bytes < spec->size) {
            int idx = MPMAX(i, 1) - 1;
            spec = &size_spec_list[idx];
            *num = bytes / spec->size;
            *type = idx;
            break;
        }
    }
}

static int resolve_path_item_flags(struct dirent *d)
{
    int flags = 0;
    if (d->d_type & DT_DIR)
        flags |= PATH_ITEM_FLAG_TYPE_DIR;
    if (d->d_type & DT_REG)
        flags |= PATH_ITEM_FLAG_TYPE_FILE;
    if (sanitize_path_name(d->d_name, NULL))
        flags |= PATH_ITEM_FLAG_SANTIZIE_NAME;
    return flags;
}

static int do_cmp_path_item(const void *l, const void *r, enum cmp_field_type type, bool reverse)
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
    case CMP_FIELD_TYPE_NAME:
        // stdlib should be fine to handle utf8 string comparison
        result = strcmp(lhs->name, rhs->name);
        break;
    case CMP_FIELD_TYPE_DATE:
        result = (tmp = lhs->date_year - rhs->date_year) != 0 ? tmp :
                (tmp = lhs->date_month - rhs->date_month) != 0 ? tmp :
                (tmp = lhs->date_day - rhs->date_day) != 0 ? tmp :
                (tmp = lhs->date_hour - rhs->date_hour) != 0 ? tmp :
                (tmp = lhs->date_minute - rhs->date_minute) != 0 ? tmp : 0;
        break;
    case CMP_FIELD_TYPE_SIZE:
        result = (tmp = lhs->size_type - rhs->size_type) != 0 ? tmp :
                (tmp = lhs->size_num - rhs->size_num) != 0 ? tmp : 0;
        break;
    }

    return reverse ? -result : result;
}

static int cmp_name_asc(const void *l, const void *r)
{
    return do_cmp_path_item(l, r, CMP_FIELD_TYPE_NAME, false);
}

static int cmp_name_desc(const void *l, const void *r)
{
    return do_cmp_path_item(l, r, CMP_FIELD_TYPE_NAME, true);
}

static int cmp_size_asc(const void *l, const void *r)
{
    return do_cmp_path_item(l, r, CMP_FIELD_TYPE_SIZE, false);
}

static int cmp_size_desc(const void *l, const void *r)
{
    return do_cmp_path_item(l, r, CMP_FIELD_TYPE_SIZE, true);
}

static int cmp_date_asc(const void *l, const void *r)
{
    return do_cmp_path_item(l, r, CMP_FIELD_TYPE_DATE, false);
}

static int cmp_date_desc(const void *l, const void *r)
{
    return do_cmp_path_item(l, r, CMP_FIELD_TYPE_DATE, true);
}

static void cursor_pos_relocate(struct priv_panel *priv, char *name)
{
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

static void do_fill_path_items(struct priv_panel *priv, DIR *dir)
{
    bstr item_path = bstrdup(priv, priv->work_dir);
    int item_cut_idx = item_path.len + 1;
    bstr_xappend(priv, &item_path, bstr0(PATH_SEP));

    char *base_zero = NULL; // a hack to suppress 'casting int to char*' warning
    struct cache_data *cache = &priv->cache_data;
    int offset = 0;
    while (true) {
        struct dirent *d = readdir(dir);
        if (!d)
            break;

        if (strcmp(d->d_name, PATH_DIR_PARENT) == 0)
            continue;
        if (strcmp(d->d_name, PATH_DIR_CURRENT) == 0)
            continue;

        // copy path name with terminator
        bstr name = bstr0(d->d_name);
        int request = name.len + 1;
        MP_TARRAY_GROW(priv, cache->path_name_pool, offset + request);
        memcpy(cache->path_name_pool + offset, name.start, request);

        // build absolute path
        item_path.len = item_cut_idx;
        item_path.start[item_cut_idx] = 0;
        bstr_xappend(priv, &item_path, name);

        struct tm file_tm = {0};
        struct stat file_stat;
        uint8_t size_type = 0;
        uint32_t size_num = 0;
        int flags = resolve_path_item_flags(d);
        bool succeed = (stat((char*) item_path.start, &file_stat) == 0);
        if (succeed) {
            if (flags & PATH_ITEM_FLAG_TYPE_FILE)
                resolve_path_size_data(file_stat.st_size, &size_num, &size_type);
            file_tm = *localtime(&file_stat.st_mtime);
            file_tm.tm_year += 1900;
            file_tm.tm_mon += 1;
        }

        // remember offsets before iteration is done
        MP_TARRAY_APPEND(priv, cache->path_items, cache->path_item_count, (struct path_item) {
            .name = base_zero + offset,
            .length = name.len,
            .flags = flags,
            .date_year = file_tm.tm_year,
            .date_month = file_tm.tm_mon,
            .date_day = file_tm.tm_mday,
            .date_hour = file_tm.tm_hour,
            .date_minute = file_tm.tm_min,
            .size_type = size_type,
            .size_num = size_num,
        });
        offset += request;
    }

    // translate offsets to addresses
    long translate_offset = cache->path_name_pool - base_zero;
    for (int i = 0; i < cache->path_item_count; ++i)
        cache->path_items[i].name += translate_offset;

    qsort(cache->path_items, cache->path_item_count,
          sizeof(struct path_item), priv->cmp_func);

    item_path.len = 0;
    TA_FREEP(&item_path.start);
}

static void fill_path_items(struct priv_panel *priv, char *match_name, bool reset_cursor)
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
    } else if (reset_cursor) {
        priv->cursor_pos = (struct cursor_data) { .current = 0, .top = 0 };
    } else {
        int max_pos = MPMAX(cache->path_item_count - 1, 0);
        struct cursor_data *cursor = &priv->cursor_pos;
        cursor->current = MPCLAMP(cursor->current, 0, max_pos);
        cursor->top = MPCLAMP(cursor->top, 0, max_pos);
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

static void join_path(void *p, bstr *path, struct path_item *item)
{
    bstr_xappend(p, path, bstr0(PATH_SEP));
    bstr_xappend(p, path, bstr0(item->name));
}

static void push_path(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct path_item *item = &priv->cache_data.path_items[priv->cursor_pos.current];
    if (item->flags & PATH_ITEM_FLAG_TYPE_DIR) {
        // remember cursor position in case of backward navigation
        MP_TARRAY_APPEND(priv, priv->cursor_pos_stack, priv->cursor_pos_count, priv->cursor_pos);
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
    priv->cmp_func = cmp_name_asc;
    return true;
}

static void files_on_show(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    if (!priv->cache_data.font) {
        const char *font_path = ui_platform_driver_vita.get_font_path(ctx);
        ui_render_driver_vita.font_init(ctx, &priv->cache_data.font, font_path);
    }
    fill_path_items(priv, NULL, false);
}

static void files_on_hide(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct cache_data *cache = &priv->cache_data;
    if (cache->font)
        ui_render_driver_vita.font_uninit(ctx, &cache->font);
    TA_FREEP(&cache->tmp_str_buf);
    TA_FREEP(&cache->path_name_pool);
    TA_FREEP(&cache->path_items);
    cache->path_item_count = 0;
}

static const char *format_name(void *p, char **buf, struct path_item *item)
{
    // it is uncommon to have special characters
    if (item->flags & PATH_ITEM_FLAG_SANTIZIE_NAME) {
        MP_TARRAY_GROW(p, *buf, (item->length + 1));
        sanitize_path_name(item->name, *buf);
        return *buf;
    }

    return item->name;
}

static const char *format_size(char *buf, struct path_item *item)
{
    if (item->flags & PATH_ITEM_FLAG_TYPE_FILE) {
        sprintf(buf, "%u%s", item->size_num, size_spec_list[item->size_type].name);
        return buf;
    } else {
        return PATH_UNKNOWN_SIZE;
    }
}

static const char *format_date(char *buf, struct path_item *item)
{
    sprintf(buf, "%04u-%02u-%02u %02u:%02u",
            item->date_year, item->date_month, item->date_day,
            item->date_hour, item->date_minute);
    return buf;
}

static void files_on_draw(struct ui_context *ctx)
{
    struct priv_panel *priv = ctx->priv_panel;
    struct cache_data *cache = &priv->cache_data;
    if (!cache->font)
        return;

    MP_TARRAY_GROW(priv, cache->tmp_str_buf, 100);

    struct ui_font_draw_args args;
    args.size = LAYOUT_ITEM_TEXT_FONT_SIZE;
    args.color = LAYOUT_ITEM_TEXT_COLOR;

    int draw_top = LAYOUT_ITEM_CURSOR_T;
    for (int i = 0; i < LAYOUT_ITEM_COUNT; ++i) {
        int idx = priv->cursor_pos.top + i;
        if (idx >= cache->path_item_count)
            break;

        // cursor rect
        if (priv->cursor_pos.current == idx) {
            struct mp_rect cursor_rect = {
                .x0 = LAYOUT_ITEM_CURSOR_L,
                .y0 = draw_top,
                .x1 = LAYOUT_ITEM_CURSOR_W,
                .y1 = draw_top + LAYOUT_ITEM_CURSOR_H,
            };
            struct ui_triangle_draw_args rect_args = {
                .rects = &cursor_rect,
                .color = LAYOUT_ITEM_CURSOR_COLOR,
                .count = 1,
            };
            ui_render_driver_vita.draw_rectangle(ctx, &rect_args);
        }

        struct path_item *item = &cache->path_items[idx];
        args.y = draw_top + LAYOUT_ITEM_TEXT_T;

        // name
        args.x = LAYOUT_ITEM_NAME_L;
        args.text = format_name(priv, &cache->tmp_str_buf, item);
        ui_render_driver_vita.draw_font(ctx, cache->font, &args);

        // size
        args.x = LAYOUT_ITEM_SIZE_L;
        args.text = format_size(cache->tmp_str_buf, item);
        ui_render_driver_vita.draw_font(ctx, cache->font, &args);

        // date
        args.x = LAYOUT_ITEM_DATE_L;
        args.text = format_date(cache->tmp_str_buf, item);
        ui_render_driver_vita.draw_font(ctx, cache->font, &args);

        draw_top += LAYOUT_ITEM_CURSOR_H;
    }
}

static void do_move_cursor(struct ui_context *ctx, const struct dpad_act_spec *spec, int count)
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

static void do_change_cmp_func(struct ui_context *ctx, int f_offset, int flip_order)
{
    // find compare func in the 2d array
    bool found = false;
    int idx_field = 0;
    int idx_order = 0;
    struct priv_panel *priv = ctx->priv_panel;
    for (size_t i = 0; i < MP_ARRAY_SIZE(cmp_func_list); ++i) {
        for (size_t j = 0; j < MP_ARRAY_SIZE(cmp_func_list[i]); ++j) {
            if (priv->cmp_func == cmp_func_list[i][j]) {
                found = true;
                idx_field = i;
                idx_order = j;
                break;
            }
        }
        if (found)
            break;
    }

    // compare func is not changed
    int idx_new_field = MPCLAMP(idx_field + f_offset, 0, MP_ARRAY_SIZE(cmp_func_list) - 1);
    int idx_new_order = idx_order ^ flip_order;
    if (found && idx_new_field == idx_field && idx_new_order == idx_order)
        return;

    priv->cmp_func = cmp_func_list[idx_new_field][idx_new_order];
    qsort(priv->cache_data.path_items, priv->cache_data.path_item_count,
          sizeof(struct path_item), priv->cmp_func);
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
    case UI_KEY_CODE_VITA_L1:
        do_change_cmp_func(ctx, -1, 0);
        break;
    case UI_KEY_CODE_VITA_R1:
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
