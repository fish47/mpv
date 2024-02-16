#include "key_helper.h"
#include "ui_panel.h"

#include <string.h>

#define KEY_TRIGGER_DELAY   (300 * 1000)
#define KEY_REPEAT_DELAY    (40 * 1000)

static void invoke_cb(const struct key_helper_spec *spec, void *p, int repeat)
{
    spec->callback(p, spec->data, repeat);
}

void key_helper_dispatch(struct key_helper_ctx *c, struct ui_key *key, int64_t time,
                         const struct key_helper_spec *list, int n, void *p)
{
    const struct key_helper_spec *spec = NULL;
    for (int i = 0; i < n; ++i) {
        if (list[i].key == key->code) {
            spec = &list[i];
            break;
        }
    }

    if (!spec)
        return;

    if (spec->repeatable) {
        switch (key->state) {
        case UI_KEY_STATE_DOWN:
            c->repeat_handled_count = 0;
            c->repeat_pressed_spec = spec;
            c->repeat_pressed_time = time;
            break;
        case UI_KEY_STATE_UP:
            // degrade as a normal key stroke
            if (!c->repeat_handled_count)
                invoke_cb(spec, p, 0);
            memset(c, 0, sizeof(struct key_helper_ctx));
            break;
        }
    } else {
        if (key->state == UI_KEY_STATE_UP)
            invoke_cb(spec, p, 0);
    }
}


void key_helper_poll(struct key_helper_ctx *c, int64_t time, void *p)
{
    if (!c->repeat_pressed_spec)
        return;

    int delta = time - c->repeat_pressed_time - KEY_TRIGGER_DELAY;
    int count = delta / KEY_REPEAT_DELAY - c->repeat_handled_count;
    if (count <= 0)
        return;

    c->repeat_handled_count += count;
    invoke_cb(c->repeat_pressed_spec, p, count);
}
