#include "ui_device.h"
#include "ui_driver.h"
#include "common/common.h"
#include "audio/out/internal.h"

#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>

extern const struct ao_driver audio_out_null;

// special hack to increase available heap memory
// https://github.com/bvschaik/julius/blob/master/src/platform/vita/vita.c#L21
unsigned int _newlib_heap_size_user = 300 * 1024 * 1024;
unsigned int _pthread_stack_default_user = 1 * 1024 * 1024;

struct priv_platform {};

static bool platform_init(struct ui_context *ctx)
{
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
    keys |= (ctrl.buttons & SCE_CTRL_L1) ? (1 << UI_KEY_CODE_VITA_L1) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_R1) ? (1 << UI_KEY_CODE_VITA_R1) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_START) ? (1 << UI_KEY_CODE_VITA_START) : 0;
    keys |= (ctrl.buttons & SCE_CTRL_SELECT) ? (1 << UI_KEY_CODE_VITA_SELECT) : 0;
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
