#include "ui_driver.h"

#include <al.h>
#include <alc.h>
#include <unistd.h>

#define BUFFER_COUNT            3

#define S16_BYTES_PER_FRAME     2

struct audio_context {
    ALuint source;
    ALsizei buffer_size;
    ALsizei frequency;
    int channels;
    int buffer_samples;
    useconds_t buffer_delay;

    ALuint all_buffers[BUFFER_COUNT];
    ALuint free_buffers[BUFFER_COUNT];
    int free_count;
};

static bool emulator_audio_init(void **ctx, int samples, int freq, int channels)
{
    struct audio_context *out = ta_new_ptrtype(NULL, out);
    ALCdevice *device = alcOpenDevice(NULL);
    ALCcontext *context = alcCreateContext(device, NULL);
    alcMakeContextCurrent(context);

    alGenBuffers(BUFFER_COUNT, out->all_buffers);
    alGenSources(1, &out->source);
    alSource3i(out->source, AL_POSITION, 0, 0, -1);
    alSourceRewind(out->source);
    alSourcei(out->source, AL_BUFFER, 0);

    out->free_count = BUFFER_COUNT;
    memcpy(out->free_buffers, out->all_buffers, BUFFER_COUNT * sizeof(ALuint));

    out->channels = channels;
    out->frequency = freq;
    out->buffer_size = samples * channels * S16_BYTES_PER_FRAME;
    out->buffer_delay = 1000000L * samples / (freq * channels);
    out->buffer_samples = samples;

    *ctx = out;
    return true;
}

static void reclaim_buffers(struct audio_context *ctx)
{
    ALint available = 0;
    alGetSourcei(ctx->source, AL_BUFFERS_PROCESSED, &available);
    if (available <= 0)
        return;

    alSourceUnqueueBuffers(ctx->source, available, (ctx->free_buffers + ctx->free_count));
    ctx->free_count += available;
}

static void wait_buffers(struct audio_context *ctx, int count)
{
    while (true) {
        reclaim_buffers(ctx);
        if (ctx->free_count >= count)
            break;

        // wait until new buffer available
        usleep(ctx->buffer_delay);
    }
}

static void emulator_audio_uninit(void **ctx)
{
    struct audio_context *context = *ctx;
    alSourceRewind(context->source);
    alDeleteSources(1, &context->source);
    alDeleteBuffers(BUFFER_COUNT, context->all_buffers);

    ALCcontext *c = alcGetCurrentContext();
    ALCdevice *dev = alcGetContextsDevice(c);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(c);
    alcCloseDevice(dev);

    TA_FREEP(ctx);
}

static int emulator_audio_output(void *ctx, void *buff)
{
    struct audio_context *context = ctx;
    if (!buff) {
        // drain all audio buffers
        wait_buffers(context, BUFFER_COUNT);
        return 0;
    }

    // ensure available buffer
    wait_buffers(context, 1);

    // enqueue new buffer
    ALuint new_buf = context->free_buffers[--context->free_count];
    ALenum format = (context->channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
    alBufferData(new_buf, format, buff, context->buffer_size, context->frequency);
    alSourceQueueBuffers(context->source, 1, &new_buf);

    // start playing if not triggered
    ALint state;
    alGetSourcei(context->source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING)
        alSourcePlay(context->source);

    ALint sample_offset = 0;
    ALint queued_count = 0;
    alGetSourcei(context->source, AL_SAMPLE_OFFSET, &sample_offset);
    alGetSourcei(context->source, AL_BUFFERS_QUEUED, &queued_count);

    // the result does not count of the input buffer
    int remaining_samples = queued_count * context->buffer_samples - sample_offset;
    return MPMAX(remaining_samples - context->buffer_samples, 0);
}

const struct ui_audio_driver ui_audio_driver_vita = {
    .buffer_count = BUFFER_COUNT,
    .init = emulator_audio_init,
    .uninit = emulator_audio_uninit,
    .output = emulator_audio_output,
};
