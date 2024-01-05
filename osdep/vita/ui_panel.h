#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "ui_device.h"

struct ui_context;

struct ui_key {
    enum ui_key_code code;
    enum ui_key_state state;
};

struct ui_panel {
    int priv_size;
    bool (*init)(struct ui_context *ctx, void *params);
    void (*uninit)(struct ui_context *ctx);
    void (*on_show)(struct ui_context *ctx);
    void (*on_hide)(struct ui_context *ctx);
    void (*on_draw)(struct ui_context *ctx);
    void (*on_poll)(struct ui_context *ctx);
    void (*on_key)(struct ui_context *ctx, struct ui_key *key);
};

struct ui_panel_player_init_params {
    char *path;
};

void ui_panel_common_wakeup(struct ui_context *ctx);
void ui_panel_common_invalidate(struct ui_context *ctx);
void *ui_panel_common_get_priv(struct ui_context *ctx, const struct ui_panel *panel);
void ui_panel_common_push(struct ui_context *ctx, const struct ui_panel *panel, void *data);
void ui_panel_common_pop(struct ui_context *ctx);
void ui_panel_common_pop_all(struct ui_context *ctx);
int64_t ui_panel_common_get_frame_time(struct ui_context *ctx);

void *ui_panel_player_get_vo_draw_data(struct ui_context *ctx);
void ui_panel_player_set_vo_draw_data(struct ui_context *ctx, void *data);
void ui_panel_player_set_vo_draw_fn(struct ui_context *ctx, void (*fn)(void *data));

extern const struct ui_panel ui_panel_player;
extern const struct ui_panel ui_panel_files;
