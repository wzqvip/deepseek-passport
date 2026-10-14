// main/passport_audio.c
#include "passport_audio.h"

#include <string.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "passport_config_store.h"
#include "passport_metrics_math.h"

static const char *TAG = "passport_audio";

#define SAMPLE_RATE_HZ 16000
#define CHUNK_SAMPLES  256   // staging granularity; bounds the time inside a read/write
#define VOLUME_PERCENT 80
// 100 ms. The ES8311's first samples after opening are a settling transient, not
// audio; measuring them reports a full-scale input that never happened.
#define WARMUP_SAMPLES 1600
// 500 ms, used once at application start. See passport_audio.h for the measurement
// that made this longer than the per-open guard.
#define WARMUP_BOOT_SAMPLES 8000

static passport_audio_owner_t s_owner = PASSPORT_AUDIO_FREE;
static bool s_opened;
static bool s_needs_warmup;

// One cycle of a 1 kHz sine at 16 kHz, at half scale so a tone check is
// comfortable rather than startling. A table keeps the generator free of libm and
// of any per-sample arithmetic in the audio path.
static const int16_t TONE_1KHZ[16] = {
    0, 6270, 11585, 15137, 16384, 15137, 11585, 6270,
    0, -6270, -11585, -15137, -16384, -15137, -11585, -6270,
};

esp_err_t passport_audio_open(void)
{
    if (s_opened) {
        return ESP_OK;
    }
    esp_err_t err = bsp_audio_set_format(SAMPLE_RATE_HZ, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "采样格式设置失败: %s", esp_err_to_name(err));
        return err;
    }
    // The stored preference wins over the compiled-in default, so a volume set from
    // the console actually takes effect on the next codec open.
    bsp_audio_set_volume(passport_config_current()->volume);
    s_opened = true;
    s_needs_warmup = true;
    return ESP_OK;
}

// Read and throw away the codec's settling transient. Called once per open, from
// the capture path while it holds the path.
static void discard_warmup(void)
{
    int16_t scratch[CHUNK_SAMPLES];
    size_t left = WARMUP_SAMPLES;
    while (left > 0u) {
        const size_t n = (left > CHUNK_SAMPLES) ? CHUNK_SAMPLES : left;
        if (bsp_audio_read(scratch, n * sizeof(int16_t)) != ESP_OK) {
            break;
        }
        left -= n;
    }
    s_needs_warmup = false;
}

passport_audio_owner_t passport_audio_owner(void)
{
    return s_owner;
}

void passport_audio_fill_tone(int16_t *buffer, size_t samples)
{
    if (buffer == NULL) {
        return;
    }
    for (size_t i = 0u; i < samples; i++) {
        buffer[i] = TONE_1KHZ[i % 16u];
    }
}

esp_err_t passport_audio_capture(int16_t *buffer, size_t samples, passport_audio_level_t *level)
{
    if (buffer == NULL || samples == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!passport_audio_arbiter_acquire(&s_owner, PASSPORT_AUDIO_CAPTURE)) {
        // Refusing here is the point: playback holds the path and the microphone
        // must stay closed, so the caller gets an error instead of a mixed signal.
        return ESP_ERR_INVALID_STATE;
    }

    if (level != NULL) {
        level->rms = 0u;
        level->peak = 0u;
        level->samples = 0u;
    }

    esp_err_t err = passport_audio_open();
    if (err == ESP_OK && s_needs_warmup) {
        discard_warmup();
    }
    size_t got = 0u;
    while (err == ESP_OK && got < samples) {
        size_t n = samples - got;
        if (n > CHUNK_SAMPLES) {
            n = CHUNK_SAMPLES;
        }
        err = bsp_audio_read(buffer + got, n * sizeof(int16_t));
        if (err != ESP_OK) {
            break;
        }
        if (level != NULL) {
            const unsigned chunk_rms = passport_pcm_rms(buffer + got, n);
            const unsigned chunk_peak = passport_pcm_peak(buffer + got, n);
            if (chunk_rms > level->rms) {
                level->rms = chunk_rms;
            }
            if (chunk_peak > level->peak) {
                level->peak = chunk_peak;
            }
            level->samples += n;
        }
        got += n;
    }

    passport_audio_arbiter_release(&s_owner, PASSPORT_AUDIO_CAPTURE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "录音失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t passport_audio_play(const int16_t *buffer, size_t samples)
{
    if (buffer == NULL || samples == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!passport_audio_arbiter_acquire(&s_owner, PASSPORT_AUDIO_PLAYBACK)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = passport_audio_open();
    size_t sent = 0u;
    while (err == ESP_OK && sent < samples) {
        size_t n = samples - sent;
        if (n > CHUNK_SAMPLES) {
            n = CHUNK_SAMPLES;
        }
        err = bsp_audio_write(buffer + sent, n * sizeof(int16_t));
        if (err != ESP_OK) {
            break;
        }
        sent += n;
    }

    passport_audio_arbiter_release(&s_owner, PASSPORT_AUDIO_PLAYBACK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "播放失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t passport_audio_play_begin(void)
{
    if (!passport_audio_arbiter_acquire(&s_owner, PASSPORT_AUDIO_PLAYBACK)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = passport_audio_open();
    if (err != ESP_OK) {
        passport_audio_arbiter_release(&s_owner, PASSPORT_AUDIO_PLAYBACK);
    }
    return err;
}

esp_err_t passport_audio_play_chunk(const int16_t *buffer, size_t samples)
{
    if (buffer == NULL || samples == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    // No arbiter work here: the caller took the path with _begin and owns it until _end. The same
    // chunking as a one-shot play, so a long write cannot overflow the codec's own buffer.
    esp_err_t err = ESP_OK;
    size_t sent = 0u;
    while (err == ESP_OK && sent < samples) {
        size_t n = samples - sent;
        if (n > CHUNK_SAMPLES) {
            n = CHUNK_SAMPLES;
        }
        err = bsp_audio_write(buffer + sent, n * sizeof(int16_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "播放失败: %s", esp_err_to_name(err));
            break;
        }
        sent += n;
    }
    return err;
}

void passport_audio_play_end(void)
{
    passport_audio_arbiter_release(&s_owner, PASSPORT_AUDIO_PLAYBACK);
}

esp_err_t passport_audio_warmup(void)
{
    if (!passport_audio_arbiter_acquire(&s_owner, PASSPORT_AUDIO_CAPTURE)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = passport_audio_open();
    if (err == ESP_OK) {
        int16_t scratch[CHUNK_SAMPLES];
        size_t left = WARMUP_BOOT_SAMPLES;
        while (left > 0u) {
            const size_t n = (left > CHUNK_SAMPLES) ? CHUNK_SAMPLES : left;
            if (bsp_audio_read(scratch, n * sizeof(int16_t)) != ESP_OK) {
                break;
            }
            left -= n;
        }
        // The per-open guard is satisfied by this longer window.
        s_needs_warmup = false;
    }

    passport_audio_arbiter_release(&s_owner, PASSPORT_AUDIO_CAPTURE);
    return err;
}

esp_err_t passport_audio_verify_mute(bool *refused)
{
    if (refused == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *refused = false;

    if (!passport_audio_arbiter_acquire(&s_owner, PASSPORT_AUDIO_PLAYBACK)) {
        return ESP_ERR_INVALID_STATE;
    }

    // While playback owns the path, a capture attempt must be refused without
    // disturbing the owner. This is the on-device counterpart of the host test.
    int16_t scratch[8];
    passport_audio_level_t level = { 0u, 0u, 0u };
    const esp_err_t err = passport_audio_capture(scratch, 8u, &level);
    *refused = (err == ESP_ERR_INVALID_STATE) && (s_owner == PASSPORT_AUDIO_PLAYBACK);

    passport_audio_arbiter_release(&s_owner, PASSPORT_AUDIO_PLAYBACK);
    return ESP_OK;
}
