#pragma once

#include "ui_device.h"

#include <stdint.h>
#include <stdbool.h>

struct ui_key;

typedef void (*key_helper_cb)(void *p, const void *data, int repeat);

struct key_helper_spec {
    enum ui_key_code key;
    key_helper_cb callback;
    const void *data;
    bool repeatable;
};

struct key_helper_ctx {
    int repeat_handled_count;
    int64_t repeat_pressed_time;
    const struct key_helper_spec *repeat_pressed_spec;
};

void key_helper_dispatch(struct key_helper_ctx *c, struct ui_key *key, int64_t time,
                         const struct key_helper_spec *list, int n, void *p);

void key_helper_poll(struct key_helper_ctx *c, int64_t time, void *p);
