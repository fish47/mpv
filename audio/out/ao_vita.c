#include "internal.h"
#include "audio/format.h"
#include "osdep/timer.h"
#include "osdep/vita/ui_driver.h"

#include <malloc.h>

#define FRAME_COUNT             2
#define FRAME_ALIGN             64
#define FRAME_WAIT_RESERVED     (50 * 1000LL)

#define BUFFER_SAMPLE_COUNT     1024

#define FLAG_PAUSED             (1)
#define FLAG_TERMINATED         (1 << 1)

#define INIT_FIELD_BASE         (1)
#define INIT_FIELD_THREAD       (1 << 2)
#define INIT_FIELD_DRIVER       (1 << 3)

struct priv {
    pthread_mutex_t lock;
    pthread_cond_t wakeup;
    pthread_t play_thread;
    void *audio_ctx;
    void *frame_base;
    int init_fields;
    int run_flags;
};

static const int supported_samplerates[] = {
    8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
};

static int choose_samplerate(int samplerate)
{
    for (int i = 0; i < MP_ARRAY_SIZE(supported_samplerates); ++i) {
        int sr = supported_samplerates[i];
        if (sr >= samplerate)
            return sr;
    }

    int max_idx = MP_ARRAY_SIZE(supported_samplerates) - 1;
    return supported_samplerates[max_idx];
}

static void set_thread_state(struct ao *ao, int flag_bit, bool set)
{
    struct priv *priv = ao->priv;
    pthread_mutex_lock(&priv->lock);
    int *flags = &priv->run_flags;
    *flags = set ? (*flags | flag_bit) : (*flags & ~flag_bit);
    pthread_cond_broadcast(&priv->wakeup);
    pthread_mutex_unlock(&priv->lock);
}

static size_t calculate_frame_size(struct ao *ao)
{
    // s16 is 2 bytes per sample
    size_t result = BUFFER_SAMPLE_COUNT;
    result *= 2;
    result *= ao->channels.num;
    return result;
}

static void *thread_run(void *arg)
{
    struct ao *ao = arg;
    struct priv *priv = ao->priv;

    bool has_data = false;
    void *done_frame = NULL;
    void *next_frame = NULL;

    pthread_mutex_lock(&priv->lock);
    while (true) {
        if (priv->run_flags & FLAG_TERMINATED) {
            break;
        } else if (priv->run_flags & FLAG_PAUSED) {
            has_data = false;
            pthread_cond_wait(&priv->wakeup, &priv->lock);
        } else {
            pthread_mutex_unlock(&priv->lock);

            // enqueue decoded frame data, it may be blocked if queue is full
            int pending_samples = 0;
            if (has_data) {
                pending_samples = ui_audio_driver_vita.output(priv->audio_ctx, done_frame);
            } else {
                done_frame = priv->frame_base;
                next_frame = ((uint8_t*) priv->frame_base) + calculate_frame_size(ao);
            }

            // when will next read frame data will be played completely
            int64_t current_time = mp_time_us();
            int64_t next_samples = BUFFER_SAMPLE_COUNT;
            int64_t output_samples = has_data ? BUFFER_SAMPLE_COUNT : 0;
            int64_t all_samples = output_samples + next_samples + pending_samples;
            int64_t samples_delay = 1000000LL * all_samples / ao->samplerate;
            int64_t end_time = current_time + samples_delay;

            // decode next frame data
            has_data = true;
            ao_read_data(ao, &next_frame, BUFFER_SAMPLE_COUNT, end_time);
            MPSWAP(void*, done_frame, next_frame);

            pthread_mutex_lock(&priv->lock);
        }
    }
    pthread_mutex_unlock(&priv->lock);
    return NULL;
}

static void uninit(struct ao *ao)
{
    struct priv *priv = ao->priv;
    if (priv->init_fields & INIT_FIELD_THREAD) {
        set_thread_state(ao, FLAG_TERMINATED, true);
        pthread_join(priv->play_thread, NULL);
    }

    if (priv->init_fields & INIT_FIELD_BASE) {
        pthread_mutex_destroy(&priv->lock);
        pthread_cond_destroy(&priv->wakeup);
    }

    if (priv->init_fields & INIT_FIELD_DRIVER)
        ui_audio_driver_vita.uninit(&priv->audio_ctx);

    if (priv->frame_base)
        free(priv->frame_base);

    memset(priv, 0, sizeof(struct priv));
}

static int init(struct ao *ao)
{
    // conform to hardware/API restrictions
    ao->format = AF_FORMAT_S16;
    ao->samplerate = choose_samplerate(ao->samplerate);
    if (ao->channels.num > 2)
        mp_chmap_from_channels(&ao->channels, 2);

    struct priv *priv = ao->priv;
    memset(priv, 0, sizeof(struct priv));

    pthread_mutex_init(&priv->lock, NULL);
    pthread_cond_init(&priv->wakeup, NULL);
    set_thread_state(ao, FLAG_PAUSED, true);
    priv->init_fields |= INIT_FIELD_BASE;

    if (!ui_audio_driver_vita.init(&priv->audio_ctx, BUFFER_SAMPLE_COUNT, ao->samplerate, ao->channels.num))
        goto error;
    priv->init_fields |= INIT_FIELD_DRIVER;

    if (pthread_create(&priv->play_thread, NULL, thread_run, ao))
        goto error;
    priv->init_fields |= INIT_FIELD_THREAD;

    size_t frame_size = calculate_frame_size(ao);
    priv->frame_base = memalign(FRAME_ALIGN, frame_size * FRAME_COUNT);
    memset(priv->frame_base, 0, frame_size * FRAME_COUNT);

    // ensure buffer capacity to avoid underrun on audio thread
    ao->device_buffer = BUFFER_SAMPLE_COUNT * ui_audio_driver_vita.buffer_count;

    return 1;

error:
    uninit(ao);
    return -1;
}

static void reset(struct ao *ao)
{
    set_thread_state(ao, FLAG_PAUSED, true);
}

static void start(struct ao *ao)
{
    set_thread_state(ao, FLAG_PAUSED, false);
}

const struct ao_driver audio_out_vita = {
    .description = "Vita Audio",
    .name = "vita",
    .init = init,
    .uninit = uninit,
    .reset = reset,
    .start = start,
    .priv_size = sizeof(struct priv),
};
