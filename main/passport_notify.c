// main/passport_notify.c
#include "passport_notify.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "passport_audio.h"
#include "passport_tone.h"

static const char *TAG = "passport_notify";

#define SAMPLE_RATE 16000u
// Two notes of a few hundred milliseconds, with room to spare. 4096 samples is a quarter of a
// second at 16 kHz, which is longer than any chime here.
#define CHIME_SAMPLES 4096u
#define CHIME_AMPLITUDE 9000   // about a quarter of full scale: audible, not startling
#define FADE_SAMPLES 160u      // 10 ms, enough to remove the click at each end

// One note in a chime.
typedef struct {
    uint32_t hz;
    uint32_t ms;
} note_t;

static int16_t *s_buffer;
static char s_last[64] = "nothing yet";

esp_err_t passport_notify_init(void)
{
    if (s_buffer != NULL) {
        return ESP_OK;
    }
    // From the system heap, once. Holding it for the life of the device avoids a chime failing
    // because the heap happened to be fragmented at the moment someone pressed a button.
    s_buffer = heap_caps_malloc(CHIME_SAMPLES * sizeof(int16_t),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_buffer == NULL) {
        ESP_LOGE(TAG, "提示音缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "提示音就绪(%u 样本,约 %u ms)", (unsigned)CHIME_SAMPLES,
             (unsigned)(CHIME_SAMPLES * 1000u / SAMPLE_RATE));
    return ESP_OK;
}

void passport_notify_report(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return;
    }
    snprintf(out, out_size, "buffer=%s last=\"%s\"",
             (s_buffer != NULL) ? "ready" : "not allocated", s_last);
}

// Fill the buffer with a sequence of notes. Returns the number of samples written.
static size_t render_notes(const note_t *notes, size_t count)
{
    size_t written = 0u;
    passport_tone_t tone;

    for (size_t i = 0; i < count && written < CHIME_SAMPLES; i++) {
        size_t want = (SAMPLE_RATE * notes[i].ms) / 1000u;
        if (want > CHIME_SAMPLES - written) {
            want = CHIME_SAMPLES - written;
        }
        passport_tone_start(&tone, notes[i].hz, SAMPLE_RATE, want, CHIME_AMPLITUDE, FADE_SAMPLES);
        written += passport_tone_fill(&tone, s_buffer + written, want);
    }
    return written;
}

esp_err_t passport_notify_play(passport_work_announcement_t announcement)
{
    if (announcement == PASSPORT_WORK_ANNOUNCE_NONE) {
        snprintf(s_last, sizeof(s_last), "silent");
        return ESP_OK;
    }
    if (s_buffer == NULL) {
        snprintf(s_last, sizeof(s_last), "no buffer");
        return ESP_ERR_INVALID_STATE;
    }

    // Rising notes ask for attention; falling ones say the task is over; a low note says it went
    // wrong. Three shapes, distinguishable without being told what they mean.
    static const note_t QUESTION_NOTES[] = { { 880u, 120u }, { 1318u, 160u } };
    static const note_t DONE_NOTES[] = { { 1046u, 120u }, { 784u, 170u } };
    static const note_t FAILED_NOTES[] = { { 392u, 260u } };

    const note_t *notes = QUESTION_NOTES;
    size_t count = sizeof(QUESTION_NOTES) / sizeof(QUESTION_NOTES[0]);
    const char *what = "question";

    switch (announcement) {
    case PASSPORT_WORK_ANNOUNCE_DONE:
        notes = DONE_NOTES;
        count = sizeof(DONE_NOTES) / sizeof(DONE_NOTES[0]);
        what = "done";
        break;
    case PASSPORT_WORK_ANNOUNCE_FAILED:
        notes = FAILED_NOTES;
        count = sizeof(FAILED_NOTES) / sizeof(FAILED_NOTES[0]);
        what = "failed";
        break;
    case PASSPORT_WORK_ANNOUNCE_SPEAK:
        // Speech is not wired to work mode yet. Chime, and say so once rather than silently
        // doing something the owner did not ask for.
        ESP_LOGW(TAG, "配置要求朗读,但工作模式尚未接入语音;改用提示音");
        what = "speak-fallback";
        break;
    default:
        break;
    }

    const size_t samples = render_notes(notes, count);
    if (samples == 0u) {
        snprintf(s_last, sizeof(s_last), "nothing rendered");
        return ESP_FAIL;
    }

    // play() takes the audio path, which closes the microphone for the duration: the device must
    // not be able to record its own chime.
    const esp_err_t err = passport_audio_play(s_buffer, samples);
    if (err != ESP_OK) {
        snprintf(s_last, sizeof(s_last), "%s: playback failed", what);
        ESP_LOGW(TAG, "提示音播放失败: %s", esp_err_to_name(err));
        return err;
    }
    snprintf(s_last, sizeof(s_last), "%s chime, %u samples", what, (unsigned)samples);
    ESP_LOGI(TAG, "提示音: %s(%u 样本)", what, (unsigned)samples);
    return ESP_OK;
}
