// main/passport_speech.c
#include "passport_speech.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "passport_app.h"
#include "passport_audio.h"
#include "passport_chat.h"
#include "passport_config_store.h"
#include "passport_json.h"
#include "passport_voice.h"
#include "passport_wifi.h"

static const char *TAG = "passport_speech";

// 20 ms of speech at 16 kHz: short enough that the button's release is noticed promptly, long
// enough that the per-chunk overhead does not matter.
#define CHUNK_SAMPLES 320
// A hard ceiling on one utterance. The voice machine reports RECORDING_FULL when it is reached, so
// a stuck microphone cannot fill the network or hold the audio path forever.
#define MAX_SECONDS 30u
#define MAX_CHUNKS ((MAX_SECONDS * 16000u) / CHUNK_SAMPLES)
// The response is a small JSON object with a transcript in it.
#define RESPONSE_MAX 1024
#define SPEECH_STACK 6144

static TaskHandle_t s_task;
static volatile bool s_running;
static volatile bool s_held;        // the button is down right now
static volatile bool s_started;     // a turn has been asked for and not yet run
static volatile bool s_release_early;   // the button came up before the worker got to it
static passport_voice_t s_voice;
static char s_last[128] = "no turn yet";
static char s_last_transcript[PASSPORT_WORK_QUESTION_MAX];
static size_t s_last_bytes;
static unsigned s_last_chunks;
static unsigned s_turns;
// Below this peak a recording is treated as an empty room. Overridable from the console.
static volatile int s_silence_peak = 400;

static void wake(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void passport_speech_talk_down(void)
{
    if (s_running && passport_voice_busy(&s_voice)) {
        // A press while an answer is being spoken is refused by the turn rules, not queued; saying
        // so once is more useful than silently ignoring it.
        ESP_LOGI(TAG, "回答中,忽略这次按下(半双工:要打断应先取消)");
        return;
    }
    s_held = true;
    s_started = true;
    s_release_early = false;
    wake();
}

void passport_speech_talk_up(void)
{
    ESP_LOGI(TAG, "talk_up(按键抬起): held %d -> 0", (int)s_held);
    s_held = false;
    if (!s_started) {
        // Released before the worker began: remember it, or the turn would never end.
        s_release_early = true;
    }
    wake();
}

bool passport_speech_listening(void)
{
    return s_voice.mic_open;
}

void passport_speech_set_silence_peak(int peak)
{
    s_silence_peak = peak;
}

int passport_speech_silence_peak(void)
{
    return s_silence_peak;
}

void passport_speech_report(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return;
    }
    snprintf(out, out_size,
             "state=%s mic=%s turns=%u silence_gate=%d last=\"%s\" transcript=\"%s\"",
             passport_voice_state_name(s_voice.state), s_voice.mic_open ? "open" : "closed",
             s_turns, s_silence_peak, s_last, s_last_transcript);
}

// One chunk of a chunked body, framed by hand.
//
// Why by hand: esp_http_client declares `Transfer-Encoding: chunked` when it is opened with a
// negative write length, and then writes the caller's bytes to the socket *unframed*. That is a
// protocol violation — the first bytes of the body are then read as a chunk size, and a server
// either rejects it or hangs. It was found by watching this project's own stand-in bridge log
// "not a chunk size" followed by the first samples of the recording, which is a much better
// diagnostic than a timeout would have been.
//
// The framing is therefore the caller's here. If a future version of the client starts framing as
// well, the same log line reappears with a literal size in it, which is how it would be noticed.
static bool write_chunk(esp_http_client_handle_t client, const void *data, size_t len)
{
    char header[16];
    const int header_len = snprintf(header, sizeof(header), "%x\r\n", (unsigned)len);
    if (header_len <= 0 || esp_http_client_write(client, header, header_len) <= 0) {
        return false;
    }
    if (len > 0u && esp_http_client_write(client, (const char *)data, (int)len) <= 0) {
        return false;
    }
    return esp_http_client_write(client, "\r\n", 2) > 0;
}

static bool write_body_end(esp_http_client_handle_t client)
{
    return esp_http_client_write(client, "0\r\n\r\n", 5) > 0;
}

// One streaming POST. Returns the status code, or a negative value on a transport failure.
static int stream_turn(void)
{
    const passport_config_t *cfg = passport_config_current();
    if (cfg->bridge_url[0] == '\0') {
        snprintf(s_last, sizeof(s_last), "no bridge configured");
        return -1;
    }
    if (passport_wifi_state() != PASSPORT_WIFI_CONNECTED) {
        snprintf(s_last, sizeof(s_last), "not associated");
        return -1;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/voice", cfg->bridge_url);

    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        snprintf(s_last, sizeof(s_last), "client init failed");
        return -1;
    }

    // Raw little-endian PCM, 16 kHz, mono. The bridge expects exactly this and wraps it for the
    // provider; see the bridge protocol.
    esp_http_client_set_header(client, "Content-Type", "audio/L16;rate=16000;channels=1");
    // Ask for the connection to be closed after the answer. With a chunked request body the client's
    // own bookkeeping of the response is unreliable while the connection is kept alive, and the
    // answer to a push-to-talk turn is one small object, so a fresh connection per turn costs
    // nothing worth keeping.
    esp_http_client_set_header(client, "Connection", "close");
    if (cfg->bridge_token[0] != '\0') {
        char auth[160];
        snprintf(auth, sizeof(auth), "Bearer %s", cfg->bridge_token);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    // A negative length means the body size is not known yet, so the client uses chunked transfer.
    // That is what lets the audio leave while it is still being recorded.
    esp_err_t err = esp_http_client_open(client, -1);
    if (err != ESP_OK) {
        snprintf(s_last, sizeof(s_last), "connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    static int16_t chunk[CHUNK_SAMPLES];
    passport_audio_level_t level = { 0, 0, 0 };
    unsigned chunks = 0;
    size_t bytes = 0;
    size_t longest_silence = 0;

    ESP_LOGI(TAG, "录音开始: held=%d", (int)s_held);
    while (s_held && chunks < MAX_CHUNKS) {
        passport_audio_level_t part = { 0, 0, 0 };
        const esp_err_t read = passport_audio_capture(chunk, CHUNK_SAMPLES, &part);
        if (chunks == 0u) {
            ESP_LOGI(TAG, "第一个分块: capture=%s held=%d", esp_err_to_name(read), (int)s_held);
        }
        if (read != ESP_OK) {
            snprintf(s_last, sizeof(s_last), "capture failed: %s", esp_err_to_name(read));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return -1;
        }
        if (part.peak > level.peak) {
            level.peak = part.peak;
        }
        if (part.rms > level.rms) {
            level.rms = part.rms;
        }
        level.samples += part.samples;
        if (part.peak < 400u) {
            longest_silence += CHUNK_SAMPLES;
        } else {
            longest_silence = 0u;
        }

        if (!write_chunk(client, chunk, CHUNK_SAMPLES * sizeof(int16_t))) {
            snprintf(s_last, sizeof(s_last), "upload stopped after %u KB",
                     (unsigned)(bytes / 1024u));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return -1;
        }
        if (chunks == 0u) {
            ESP_LOGI(TAG, "第一个分块已发出: 640 字节负载");
        }
        bytes += CHUNK_SAMPLES * sizeof(int16_t);
        chunks++;
    }

    if (!s_held && chunks < MAX_CHUNKS) {
        ESP_LOGI(TAG, "松手:%u 个分块, %u 字节, rms=%u peak=%u", chunks, (unsigned)bytes,
                 level.rms, level.peak);
    } else if (chunks >= MAX_CHUNKS) {
        ESP_LOGW(TAG, "达到单次录音上限 %u 秒,自动结束", (unsigned)MAX_SECONDS);
    }

    s_last_bytes = bytes;
    s_last_chunks = chunks;

    // Too quiet to be speech: the provider would answer with an empty transcript or a guess. The
    // audio has already been streamed — that is the price of not buffering it — but its transcript
    // is discarded rather than asked for, which still saves the provider call and its cost.
    //
    // The threshold is adjustable from the console because the right value depends on the room:
    // measured ambient peaks on this desk were around 237, while speech is in the thousands, so the
    // default sits between them. Zero disables the gate.
    if (s_silence_peak > 0u && level.peak < (unsigned)s_silence_peak) {
        snprintf(s_last, sizeof(s_last), "%u KB, peak %u below gate %d: discarded",
                 (unsigned)(bytes / 1024u), level.peak, s_silence_peak);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -2;
    }

    // Terminate the chunked body before waiting for the answer: without this the server keeps
    // waiting for more chunks and the request never completes.
    if (!write_body_end(client)) {
        snprintf(s_last, sizeof(s_last), "could not finish the request body");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    // Read the answer directly rather than calling fetch_headers first. With a chunked request body
    // the client's header fetch reports a transport failure here even though the upload arrived and
    // the server answered; reading the data makes the client consume the response itself, and the
    // status code is available afterwards. The upload path is unaffected either way — the bridge
    // receives the same bytes — which is how this was narrowed down.
    static char response[RESPONSE_MAX];
    int total = 0;
    while (total < (int)sizeof(response) - 1) {
        const int read = esp_http_client_read(client, response + total,
                                              (int)sizeof(response) - 1 - total);
        if (read <= 0) {
            break;
        }
        total += read;
    }
    response[total] = '\0';
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total == 0) {
        snprintf(s_last, sizeof(s_last), "no answer body after %u KB", (unsigned)(bytes / 1024u));
        return -1;
    }
    if (status != 200) {
        snprintf(s_last, sizeof(s_last), "bridge returned HTTP %d", status);
        return status;
    }

    // The transcript is read with the same JSON reader the work protocol uses.
    char text[PASSPORT_WORK_QUESTION_MAX];
    size_t decoded = 0u;
    const passport_json_status_t parsed =
        passport_json_string(response, passport_json_len(response), "text", text, sizeof(text),
                             &decoded);
    if (parsed != PASSPORT_JSON_OK || text[0] == '\0') {
        snprintf(s_last, sizeof(s_last), "no transcript in the response");
        return -3;
    }
    if (parsed == PASSPORT_JSON_TRUNCATED) {
        // A transcript longer than one utterance should produce is reported rather than quietly
        // cut, because the model would answer a different question than the one that was asked.
        ESP_LOGW(TAG, "转写被截断:完整长度 %u 字节", (unsigned)decoded);
    }
    snprintf(s_last_transcript, sizeof(s_last_transcript), "%s", text);
    snprintf(s_last, sizeof(s_last), "transcript %u bytes from %u KB of audio",
             (unsigned)strlen(text), (unsigned)(bytes / 1024u));
    ESP_LOGI(TAG, "转写:%s", text);
    return status;
}

// Drive one whole turn. Runs on the speech task, so it may block.
static void run_turn(void)
{
    s_started = false;
    s_turns++;

    passport_voice_action_t action = passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_TALK_PRESS);
    if (action != PASSPORT_VOICE_ACT_OPEN_MIC) {
        snprintf(s_last, sizeof(s_last), "press refused in state %s",
                 passport_voice_state_name(s_voice.state));
        return;
    }
    passport_app_show_status("听你说…", PASSPORT_PET_LISTENING);

    const int result = stream_turn();

    // The microphone is closed by now in every path, which is what the turn rules require before
    // the audio is interpreted.
    action = passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_TALK_RELEASE);
    if (result == -1) {
        passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_FAILED);
        passport_app_show_error(s_last);
        passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_RESET);
        return;
    }
    if (result == -2) {
        // Silence is not a failure: it is a room with nobody talking in it.
        passport_app_show_status("没听到声音", PASSPORT_PET_CURIOUS);
        passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_RESET);
        return;
    }
    if (result != 200) {
        passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_FAILED);
        passport_app_show_error(s_last);
        passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_RESET);
        return;
    }

    passport_app_show_status("转写好了…", PASSPORT_PET_CURIOUS);
    action = passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_TRANSCRIPT);
    if (action == PASSPORT_VOICE_ACT_ASK_MODEL) {
        // The rest of the conversation is the path that already works, unchanged.
        if (passport_chat_ask(s_last_transcript) != ESP_OK) {
            passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_FAILED);
            passport_app_show_error("模型正忙");
            passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_RESET);
        }
    }
}

static void speech_task(void *arg)
{
    (void)arg;
    while (s_running) {
        // Block until the button or the console says something; the timeout is a safety net so a
        // missed wake-up cannot leave the task asleep forever.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (!s_running) {
            break;
        }
        if (s_release_early && !s_started) {
            s_release_early = false;
            continue;
        }
        if (s_started && (s_held || s_release_early)) {
            run_turn();
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t passport_speech_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_running = true;
    passport_voice_init(&s_voice);
    if (xTaskCreate(speech_task, "passport_speech", SPEECH_STACK, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        s_task = NULL;
        ESP_LOGE(TAG, "语音线程创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "按住说话已就绪(分块 %u 样本, 单次上限 %u 秒)", (unsigned)CHUNK_SAMPLES,
             (unsigned)MAX_SECONDS);
    return ESP_OK;
}
