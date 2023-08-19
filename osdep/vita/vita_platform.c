#include "ui_device.h"
#include "ui_driver.h"
#include "ui_context.h"
#include "common/common.h"
#include "audio/out/internal.h"

#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/kernel/processmgr.h>

extern const struct ao_driver audio_out_null;

// special hack to increase available heap memory
// https://github.com/bvschaik/julius/blob/master/src/platform/vita/vita.c#L21
unsigned int _newlib_heap_size_user = 300 * 1024 * 1024;
unsigned int _pthread_stack_default_user = 1 * 1024 * 1024;

struct priv_platform {
    bool use_circle_enter;
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

static void platform_exit()
{
    sceKernelExitProcess(0);
}

static uint32_t platform_poll_keys(struct ui_context *ctx)
{
    struct priv_platform *priv = get_priv_platform(ctx);
    uint32_t enter_bit = SCE_CTRL_CIRCLE;
    uint32_t cancel_bit = SCE_CTRL_CROSS;
    if (!priv->use_circle_enter)
        MPSWAP(uint32_t, enter_bit, cancel_bit);

    SceCtrlData ctrl = {0};
    sceCtrlPeekBufferPositive(0, &ctrl, 1);

    uint32_t keys = 0;
    keys |= (ctrl.buttons & SCE_CTRL_LEFT) ? (1 << UI_KEY_CODE_VITA_DPAD_LEFT) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_RIGHT) ? (1 << UI_KEY_CODE_VITA_DPAD_RIGHT) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_UP) ? (1 << UI_KEY_CODE_VITA_DPAD_UP) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_DOWN) ? (1 << UI_KEY_CODE_VITA_DPAD_DOWN) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_SQUARE) ? (1 << UI_KEY_CODE_VITA_ACTION_SQUARE) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_CIRCLE) ? (1 << UI_KEY_CODE_VITA_ACTION_CIRCLE) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_TRIANGLE) ? (1 << UI_KEY_CODE_VITA_ACTION_TRIANGLE) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_CROSS) ? (1 << UI_KEY_CODE_VITA_ACTION_CROSS) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_LTRIGGER) ? (1 << UI_KEY_CODE_VITA_TRIGGER_L) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_RTRIGGER) ? (1 << UI_KEY_CODE_VITA_TRIGGER_R) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_START) ? (1 << UI_KEY_CODE_VITA_START) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_SELECT) ? (1 << UI_KEY_CODE_VITA_SELECT) : 0;
    keys |= (ctrl.buttons & enter_bit) ? (1 << UI_KEY_CODE_VITA_VIRTUAL_OK) : 0;
    keys |= (ctrl.buttons & cancel_bit) ? (1 << UI_KEY_CODE_VITA_VIRTUAL_CANCEL) : 0;
    return keys;
}

const struct ui_platform_driver ui_platform_driver_vita = {
    .priv_size = sizeof(struct priv_platform),
    .init = platform_init,
    .uninit = platform_uninit,
    .exit = platform_exit,
    .poll_events = NULL,
    .poll_keys = platform_poll_keys,
};
