#include "ui_driver.h"

#include <psp2/audioout.h>

static int get_audio_port(void *port) {
    return (int) port;
}

static bool audio_init(void **ctx, int samples, int freq, int channels) {
    SceAudioOutPortType type = (freq < 48000)
        ? SCE_AUDIO_OUT_PORT_TYPE_BGM
        : SCE_AUDIO_OUT_PORT_TYPE_MAIN;
    SceAudioOutMode mode = (channels == 1)
        ? SCE_AUDIO_OUT_MODE_MONO
        : SCE_AUDIO_OUT_MODE_STEREO;

    int port = sceAudioOutOpenPort(type, samples, freq, mode);
    if (port < 0)
        return false;

    int vols[2] = {SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL};
    SceAudioOutChannelFlag flags = SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH;
    sceAudioOutSetVolume(port, flags, vols);

    *ctx = (void*) port;
    return true;
}

static void audio_uninit(void **ctx) {
    sceAudioOutReleasePort(get_audio_port(*ctx));
}

static int audio_output(void *ctx, void *buff) {
    sceAudioOutOutput(get_audio_port(ctx), buff);
    return 0;
}

const struct ui_audio_driver ui_audio_driver_vita = {
    .buffer_count = 2,
    .init = audio_init,
    .uninit = audio_uninit,
    .output = audio_output,
};