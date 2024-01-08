#pragma once

struct ui_context;
struct MPContext;
struct player_perf_ctx;

struct player_perf_ctx *player_perf_create_ctx(void *parent);

void player_perf_draw(struct player_perf_ctx *ppc, struct ui_context *ctx);

void player_perf_poll(struct player_perf_ctx *ppc,
                      struct ui_context *ctx, struct MPContext *mpc);

void player_perf_stop(struct player_perf_ctx *ppc, struct ui_context *ctx);
