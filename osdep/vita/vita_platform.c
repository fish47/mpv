#include "ui_device.h"
#include "ui_driver.h"
#include "ui_context.h"

#include <strings.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/kernel/processmgr.h>

// special hack to increase available heap memory
// https://github.com/bvschaik/julius/blob/master/src/platform/vita/vita.c#L21
unsigned int _newlib_heap_size_user = 300 * 1024 * 1024;
unsigned int _pthread_stack_default_user = 1 * 1024 * 1024;

struct priv_platform {
    bool use_circle_enter;
    uint32_t keys_cache_raw;
    uint32_t keys_cache_converted;
};

static struct priv_platform *get_priv_platform(struct ui_context *ctx)
{
    return (struct priv_platform*) ctx->priv_platform;
}

static bool platform_init(struct ui_context *ctx, int argc, char *argv[])
{
    struct priv_platform *priv = get_priv_platform(ctx);
    int enter_btn = SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter_btn);
    priv->use_circle_enter = (enter_btn == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    return true;
}

static void platform_uninit(struct ui_context *ctx)
{}

static void platform_exit(void)
{
    sceKernelExitProcess(0);
}

static uint32_t do_resolve_ui_key_code(uint32_t bit)
{
    switch (bit) {
    case SCE_CTRL_LEFT:
        return UI_KEY_CODE_VITA_DPAD_LEFT;
    case SCE_CTRL_RIGHT:
        return UI_KEY_CODE_VITA_DPAD_RIGHT;
    case SCE_CTRL_UP:
        return UI_KEY_CODE_VITA_DPAD_UP;
    case SCE_CTRL_DOWN:
        return UI_KEY_CODE_VITA_DPAD_DOWN;
    case SCE_CTRL_SQUARE:
        return UI_KEY_CODE_VITA_ACTION_SQUARE;
    case SCE_CTRL_CIRCLE:
        return UI_KEY_CODE_VITA_ACTION_CIRCLE;
    case SCE_CTRL_TRIANGLE:
        return UI_KEY_CODE_VITA_ACTION_TRIANGLE;
    case SCE_CTRL_CROSS:
        return UI_KEY_CODE_VITA_ACTION_CROSS;
    case SCE_CTRL_LTRIGGER:
        return UI_KEY_CODE_VITA_TRIGGER_L;
    case SCE_CTRL_RTRIGGER:
        return UI_KEY_CODE_VITA_TRIGGER_R;
    case SCE_CTRL_START:
        return UI_KEY_CODE_VITA_START;
    case SCE_CTRL_SELECT:
        return UI_KEY_CODE_VITA_SELECT;
    }
    return 0;
}

static uint32_t platform_poll_keys(struct ui_context *ctx)
{
    SceCtrlData ctrl = {0};
    sceCtrlPeekBufferPositive(0, &ctrl, 1);

    // keys are not likely to change frequently
    struct priv_platform *priv = get_priv_platform(ctx);
    if (priv->keys_cache_raw == ctrl.buttons)
        return priv->keys_cache_converted;

    uint32_t enter_bit = SCE_CTRL_CIRCLE;
    uint32_t cancel_bit = SCE_CTRL_CROSS;
    if (!priv->use_circle_enter)
        MPSWAP(uint32_t, enter_bit, cancel_bit);

    uint32_t keys = 0;
    if (ctrl.buttons & enter_bit)
        keys |= UI_KEY_CODE_VITA_VIRTUAL_OK;
    if (ctrl.buttons & cancel_bit)
        keys |= UI_KEY_CODE_VITA_VIRTUAL_CANCEL;

    uint32_t pending = ctrl.buttons;
    while (pending) {
        uint32_t pressed = 1 << (ffs(pending) - 1);
        pending &= ~pressed;
        keys |= do_resolve_ui_key_code(pressed);
    }

    priv->keys_cache_raw = ctrl.buttons;
    priv->keys_cache_converted = keys;
    return keys;
}

const char* platform_get_files_dir(struct ui_context *ctx)
{
    return "ux0:";
}

static int platform_get_battery_level(struct ui_context *ctx)
{
    return scePowerGetBatteryLifePercent();
}

const struct ui_platform_driver ui_platform_driver_vita = {
    .priv_size = sizeof(struct priv_platform),
    .init = platform_init,
    .uninit = platform_uninit,
    .exit = platform_exit,
    .poll_events = NULL,
    .poll_keys = platform_poll_keys,
    .get_files_dir = platform_get_files_dir,
    .get_battery_level = platform_get_battery_level,
};
