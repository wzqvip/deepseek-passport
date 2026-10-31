// main/passport_speech.c
#include "passport_speech.h"

#include <stdio.h>
#include <stdlib.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "passport_provider.h"
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "passport_app.h"
#include "passport_audio.h"
#include "passport_chat.h"
#include "passport_config_store.h"
#include "passport_http.h"
#include "passport_json.h"
#include "passport_voice.h"
#include "passport_wifi.h"
#include "passport_utf8.h"

// fetch_transcript() is defined after stream_turn() uses it.
static int fetch_transcript(const char *turn_id, char *out, size_t out_size);

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
#define SPEECH_STACK 12288

// How long to keep asking for a transcript before giving up on it.
//
// Three numbers that have to agree with each other and with the bridge. A transcription is a provider
// round trip, and a well-behaved bridge holds this request open until it has an answer rather than
// replying "not ready": so a single request may legitimately take many seconds, and a per-request timeout
// of eight seconds — which is what this used to be — aborts a request that is about to succeed. The
// owner saw exactly that, after seeing the device give up on a bridge that was still working.
//
//   per request  15 s   the longest a bridge may hold one request (its own hold is shorter)
//   between them  0.5 s the gap after a bridge says "not ready" and is polled again
//   in total     30 s   after which the turn fails with the number of seconds it waited
#define TRANSCRIPT_TIMEOUT_MS 15000u
#define TRANSCRIPT_GAP_MS     500
#define TRANSCRIPT_BUDGET_MS  30000

static TaskHandle_t s_task;
static portMUX_TYPE s_intent_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_turn_active;
static volatile bool s_cancelled;
static bool s_speak_pending;
static char s_speak_request[192];
static volatile bool s_running;
static volatile bool s_held;        // the button is down right now
static volatile bool s_started;     // a turn has been asked for and not yet run
static volatile bool s_release_early;   // the button came up before the worker got to it
static volatile int64_t s_press_us;     // when the button went down, for the latency measurement
static volatile int64_t s_woke_us;      // when the worker started the turn
static volatile int64_t s_ui_us;        // when the screen had been updated
static passport_voice_t s_voice;
static char s_last[128] = "no turn yet";
static char s_last_transcript[PASSPORT_WORK_QUESTION_MAX];
static size_t s_last_bytes;
static unsigned s_last_chunks;
static unsigned s_turns;
// The peak and the threshold of the last recording, kept so a discarded turn can say *why* instead of
// only that nothing was heard. A room too loud for the gate is the difference between "the device is
// broken" and "this threshold does not suit this room", and only the owner can tell those apart.
static volatile unsigned s_last_peak;
static volatile int s_last_gate;
// The peak below which a recording is treated as an empty room. -1 means "derive it from the room",
// which is the default: a fixed number is a guess about a room nobody has measured. The console can
// pin a value, including 0 for "never treat anything as silence".
static volatile int s_silence_peak = -1;

// Below this, the codec's own noise would trip the gate; above it, a quiet speaker would be dropped.
#define GATE_MIN 300
#define GATE_MAX 4000

// The threshold to use right now, and the reason for it.
static int effective_gate(void)
{
    if (s_silence_peak >= 0) {
        return s_silence_peak;
    }
    passport_audio_level_t floor;
    if (!passport_audio_noise_floor(&floor)) {
        // No measurement: fall back to the fixed number this used to be, and say so where the console
        // can see it rather than pretending the value is informed.
        return 400;
    }
    // Twice the room's own peak. This is a heuristic and is described as one: the room was measured
    // by the boot warm-up, but the level of a person speaking into this microphone is something only
    // a person can check, so the margin is deliberately generous rather than tuned.
    int gate = (int)(floor.peak * 2u);
    if (gate < GATE_MIN) {
        gate = GATE_MIN;
    } else if (gate > GATE_MAX) {
        gate = GATE_MAX;
    }
    return gate;
}

static void wake(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void passport_speech_talk_down(void)
{
    taskENTER_CRITICAL(&s_intent_lock);
    if (s_turn_active || s_started || s_speak_pending) {
        taskEXIT_CRITICAL(&s_intent_lock);
        // A press while an answer is being spoken is refused by the turn rules, not queued; saying
        // so once is more useful than silently ignoring it.
        ESP_LOGI(TAG, "回答中,忽略这次按下(半双工:要打断应先取消)");
        return;
    }
    s_cancelled = false;
    s_press_us = esp_timer_get_time();
    s_held = true;
    s_started = true;
    s_release_early = false;
    taskEXIT_CRITICAL(&s_intent_lock);
    wake();
}

void passport_speech_talk_up(void)
{
    ESP_LOGI(TAG, "talk_up(按键抬起): held %d -> 0", (int)s_held);
    taskENTER_CRITICAL(&s_intent_lock);
    s_held = false;
    if (s_started) {
        // Released before the worker began: remember it, or the turn would never end.
        s_release_early = true;
    }
    taskEXIT_CRITICAL(&s_intent_lock);
    wake();
}

void passport_speech_talk_cancel(void)
{
    taskENTER_CRITICAL(&s_intent_lock);
    s_cancelled = true;
    s_held = false;
    s_started = false;
    s_speak_pending = false;
    s_release_early = false;
    taskEXIT_CRITICAL(&s_intent_lock);
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
    passport_audio_level_t floor;
    const bool measured = passport_audio_noise_floor(&floor);
    snprintf(out, out_size,
             "state=%s mic=%s turns=%u gate=%d%s last=\"%s\" transcript=\"%s\"",
             passport_voice_state_name(s_voice.state), s_voice.mic_open ? "open" : "closed",
             s_turns, effective_gate(),
             (s_silence_peak >= 0) ? "" : (measured ? " (twice the measured room)" : " (fallback)"),
             s_last, s_last_transcript);
}

// A short write is legal. Never advance to another frame until all bytes are sent.
static bool write_all(esp_http_client_handle_t client, const void *data, size_t len)
{
    const char *p = data;
    const int64_t deadline = esp_timer_get_time() + 30000000;
    while (len) {
        if (s_cancelled) return false;
        if (esp_timer_get_time() >= deadline) return false;
        int n = esp_http_client_write(client, p, (int)len);
        if (n <= 0) return false;
        p += n; len -= (size_t)n;
    }
    return true;
}

// Direct upload uses known Content-Length and a valid WAV, never a 30-second heap allocation.
static int direct_turn(void)
{
    const passport_config_t *cfg = passport_config_current();
    char url[192], prefix[384];
    if (passport_wifi_state() != PASSPORT_WIFI_CONNECTED ||
        !passport_provider_endpoint(cfg->stt_base_url, "/audio/transcriptions", url, sizeof(url)) ||
        !passport_provider_stt_prefix(cfg->stt_model, prefix, sizeof(prefix))) {
        snprintf(s_last, sizeof(s_last), "invalid direct STT configuration/network");
        return -1;
    }
    size_t cap = passport_provider_record_capacity(
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    int16_t *pcm = cap ? malloc(cap) : NULL;
    if (!pcm) { snprintf(s_last,sizeof(s_last),"STT low RAM; use bridge or retry"); return -1; }
    ESP_LOGI(TAG, "Direct STT recording limit: %u ms", (unsigned)(cap / 32));
    char limit_text[48];
    snprintf(limit_text, sizeof(limit_text), "REC max %u.%u s", (unsigned)(cap/32000),
             (unsigned)((cap%32000)/3200));
    passport_app_show_status(limit_text, PASSPORT_PET_LISTENING);
    size_t bytes = 0; unsigned peak = 0;
    const int64_t capture_deadline = esp_timer_get_time() + 3500000;
    while (s_held && bytes + 640 <= cap && esp_timer_get_time() < capture_deadline) {
        passport_audio_level_t part = {0};
        if (passport_audio_capture(pcm + bytes/2, CHUNK_SAMPLES, &part) != ESP_OK) {
            free(pcm); snprintf(s_last,sizeof(s_last),"capture failed"); return -1;
        }
        if (part.peak > peak) peak = part.peak;
        bytes += 640;
    }
    s_last_bytes = bytes; s_last_chunks = bytes / 640;
    if (s_cancelled) { free(pcm); return -1; }
    s_last_peak = peak;
    s_last_gate = effective_gate();
    if (!bytes || peak < (unsigned)s_last_gate) { free(pcm); return -2; }
    passport_app_show_status("正在转写…", PASSPORT_PET_THINKING);
    uint8_t wav[44]; passport_provider_wav(wav, bytes);
    const char *tail = passport_provider_stt_suffix();
    const esp_http_client_config_t config = {
        .url=url, .method=HTTP_METHOD_POST, .timeout_ms=15000,
        .crt_bundle_attach=esp_crt_bundle_attach, .disable_auto_redirect=true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&config);
    if (!c) { free(pcm); snprintf(s_last,sizeof(s_last),"STT client allocation failed"); return -1; }
    char auth[128]; snprintf(auth,sizeof(auth),"Bearer %s",cfg->stt_api_key);
    esp_http_client_set_header(c,"Authorization",auth);
    esp_http_client_set_header(c,"Content-Type","multipart/form-data; boundary=" PASSPORT_PROVIDER_BOUNDARY);
    esp_err_t err=esp_http_client_open(c,(int)(strlen(prefix)+44+bytes+strlen(tail)));
    bool sent=err==ESP_OK && write_all(c,prefix,strlen(prefix)) && write_all(c,wav,44) &&
        write_all(c,pcm,bytes) && write_all(c,tail,strlen(tail));
    free(pcm); // Release recording before parsing or starting another TLS request.
    int status=0; char response[RESPONSE_MAX]; size_t used=0;
    bool complete=false;
    const int64_t deadline=esp_timer_get_time()+30000000;
    if (sent && esp_http_client_fetch_headers(c)>=0) {
        status=esp_http_client_get_status_code(c);
        while(status==200 && used<sizeof(response)-1 && esp_timer_get_time()<deadline) {
            int n=esp_http_client_read(c,response+used,(int)(sizeof(response)-1-used));
            if(n<=0) { complete=n==0 && esp_http_client_is_complete_data_received(c); break; }
            used+=(size_t)n;
            if(esp_http_client_is_complete_data_received(c)) { complete=true; break; }
        }
    }
    response[used]=0;
    esp_http_client_close(c); esp_http_client_cleanup(c);
    size_t decoded=0;
    if(status!=200 || !complete || passport_json_string(response,used,"text",s_last_transcript,
            sizeof(s_last_transcript),&decoded)!=PASSPORT_JSON_OK || !s_last_transcript[0]) {
        s_last_transcript[0]=0;
        snprintf(s_last,sizeof(s_last),"STT failed (HTTP %d/incomplete or empty response)",status);
        return -1;
    }
    snprintf(s_last,sizeof(s_last),"direct STT %u bytes",(unsigned)bytes);
    return 200;
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
    if (header_len <= 0 || !write_all(client, header, (size_t)header_len)) {
        return false;
    }
    if (len > 0u && !write_all(client, data, len)) {
        return false;
    }
    return write_all(client, "\r\n", 2);
}

static bool write_body_end(esp_http_client_handle_t client)
{
    return write_all(client, "0\r\n\r\n", 5);
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
    // Where the time between the press and the first sample goes. Measured rather than guessed: this
    // instrumentation was added to chase a reported 1.4-second delay that turned out not to exist —
    // press to first sample measures 26 ms — and it is kept because the number is worth having when
    // something later does slow down.
    const int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "延迟: 按下->唤醒 %lld ms, 唤醒->界面 %lld ms, 界面->连接就绪 %lld ms",
             (long long)((s_woke_us - s_press_us) / 1000),
             (long long)((s_ui_us - s_woke_us) / 1000),
             (long long)((now - s_ui_us) / 1000));
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
    const int gate = effective_gate();
    s_last_peak = level.peak;
    s_last_gate = gate;
    if (gate > 0 && level.peak < (unsigned)gate) {
        snprintf(s_last, sizeof(s_last), "%u KB, peak %u below gate %d: discarded",
                 (unsigned)(bytes / 1024u), level.peak, gate);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -2;
    }

    if (s_cancelled) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    // Terminate the chunked body before waiting for the answer: without this the server keeps
    // waiting for more chunks and the request never completes.
    if (!write_body_end(client)) {
        snprintf(s_last, sizeof(s_last), "could not finish the request body");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    // Validate acceptance before polling this upload's explicit request ID.
    if (esp_http_client_fetch_headers(client) < 0 ||
        esp_http_client_get_status_code(client) != 202) {
        snprintf(s_last, sizeof(s_last), "voice upload rejected (HTTP %d)",
                 esp_http_client_get_status_code(client));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
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
    const bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    char turn_id[40];
    size_t id_len = 0;
    if (!complete || passport_json_string(response, (size_t)total, "turn_id", turn_id,
            sizeof(turn_id), &id_len) != PASSPORT_JSON_OK || id_len != 32 ||
        strspn(turn_id, "0123456789abcdef") != 32) {
        snprintf(s_last, sizeof(s_last), "voice upload missing valid turn ID");
        return -1;
    }

    ESP_LOGI(TAG, "音频已送达(%u KB),去取转写", (unsigned)(bytes / 1024u));

    // Say what is happening while it happens. Transcribing is a request to a service now, not a local
    // computation, and the gap between "the audio is delivered" and "here is the text" is seconds long:
    // leaving the screen on the previous line makes a working device look like a hung one.
    passport_app_show_status("正在转写…", PASSPORT_PET_THINKING);

    static char text[PASSPORT_WORK_QUESTION_MAX];
    const int transcribed = fetch_transcript(turn_id, text, sizeof(text));
    if (transcribed != 200) {
        return (transcribed < 0) ? transcribed : -5;
    }
    snprintf(s_last_transcript, sizeof(s_last_transcript), "%s", text);
    snprintf(s_last, sizeof(s_last), "transcript %u bytes from %u KB of audio",
             (unsigned)strlen(text), (unsigned)(bytes / 1024u));
    ESP_LOGI(TAG, "转写:%s", text);
    return 200;
}

// Ask the bridge for the transcript of the turn that was just uploaded.
//
// A separate request, so that a bridge which takes seconds to transcribe does not have to hold the
// upload's socket open. The explicit turn ID prevents a stale transcript from another upload.
static int fetch_transcript(const char *turn_id, char *out, size_t out_size)
{
    const passport_config_t *cfg = passport_config_current();
    char url[192];
    snprintf(url, sizeof(url), "%s/voice/transcript?turn_id=%s", cfg->bridge_url, turn_id);

    // Transcription takes a moment on a real bridge, and "a moment" was once six attempts with a 400 ms
    // gap — about three seconds, which a real service disproved. A recognition request is a network
    // round trip through a provider and several seconds is normal rather than exceptional, so the wait
    // is now a budget of thirty seconds, and the caller puts a thinking face on the screen for the
    // duration, because a wait nobody can see is indistinguishable from a failure.
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)TRANSCRIPT_BUDGET_MS * 1000;
    while (!s_cancelled && esp_timer_get_time() < deadline_us) {
        static char body[512];
        passport_http_result_t result;
        const passport_http_request_t request = {
            .url = url,
            .api_key = (cfg->bridge_token[0] != '\0') ? cfg->bridge_token : NULL,
            .body = NULL,
            .timeout_ms = TRANSCRIPT_TIMEOUT_MS,
        };
        const esp_err_t err = passport_http_request(&request, &result, body, sizeof(body));
        if (err != ESP_OK) {
            snprintf(s_last, sizeof(s_last), "transcript fetch failed: %s", esp_err_to_name(err));
            return -1;
        }
        if (result.status != 200) {
            snprintf(s_last, sizeof(s_last), "transcript fetch returned HTTP %d", result.status);
            return result.status;
        }

        char returned_id[40];
        size_t returned_len = 0;
        if (passport_json_string(body, passport_json_len(body), "turn_id", returned_id,
                sizeof(returned_id), &returned_len) != PASSPORT_JSON_OK || strcmp(returned_id, turn_id)) {
            snprintf(s_last, sizeof(s_last), "transcript turn ID mismatch");
            return -1;
        }
        bool present = false;
        const int ready_value = passport_json_int(body, passport_json_len(body), "ready", &present);
        // A missing "ready" counts as ready. The reader is an integer reader, so a bridge that
        // answers `"ready": true` — a JSON boolean, which is not an integer — would otherwise be
        // polled until the attempts ran out while it was in fact answering every time. Being
        // tolerant here costs nothing and cannot mis-read a bridge that says `"ready": 0` while it
        // is still working, which is the case that matters.
        const bool ready = !present || ready_value != 0;
        if (!ready) {
            vTaskDelay(pdMS_TO_TICKS(TRANSCRIPT_GAP_MS));
            continue;
        }
        {
            size_t decoded = 0u;
            const passport_json_status_t parsed = passport_json_string(
                body, passport_json_len(body), "text", out, out_size, &decoded);
            if (parsed == PASSPORT_JSON_OK && out[0] != '\0') {
                if (parsed == PASSPORT_JSON_TRUNCATED) {
                    ESP_LOGW(TAG, "转写被截断:完整长度 %u 字节", (unsigned)decoded);
                }
                return 200;
            }
            // Ready but empty: the bridge heard nothing. Reported as such rather than retried.
            out[0] = '\0';
            snprintf(s_last, sizeof(s_last), "bridge returned an empty transcript");
            return -3;
        }
    }
    // Say how long it waited, in the terms the owner experiences: the number is the point of the
    // message, and the old one ("after six attempts") described the loop instead of the wait.
    snprintf(s_last, sizeof(s_last), "still transcribing after %u seconds",
             (unsigned)(TRANSCRIPT_BUDGET_MS / 1000u));
    return -4;
}

// Ask the bridge to synthesize the answer and play it as it arrives.
//
// The audio path is taken once for the whole utterance (passport_audio_play_begin/_end), so the
// microphone stays closed from the first sample to the last. Playing chunk by chunk with the one-shot
// call would release the path hundreds of times inside one answer, which is the hole the invariant
// exists to close.
//
// A bridge without synthesis is not a failure: the reply is already on the screen, so a 404 or an
// empty body just means the turn finishes quietly.
typedef struct { bool seen; bool allowed; } speech_headers_t;
static esp_err_t speech_header_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->user_data && event->header_key) {
        const char *key = event->header_key;
        const char *expected = "content-type";
        while (*key && *expected) {
            char c = *key++;
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != *expected++) return ESP_OK;
        }
        if (!*key && !*expected) {
            speech_headers_t *headers = event->user_data;
            bool allowed = passport_provider_pcm_content_type(event->header_value);
            headers->allowed = headers->seen ? headers->allowed && allowed : allowed;
            headers->seen = true;
        }
    }
    return ESP_OK;
}

static esp_err_t speak_text_impl(const char *reply)
{
    const passport_config_t *cfg = passport_config_current();
    if (s_cancelled) return ESP_ERR_INVALID_STATE;
    if (reply == NULL || reply[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const bool direct = cfg->tts_base_url[0] && cfg->tts_model[0];
    if ((!direct && cfg->bridge_url[0] == '\0') || passport_wifi_state() != PASSPORT_WIFI_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    // A reply that starts with a control byte is not text, whatever produced it.
    //
    // Observed on hardware: the console showed the model's answer correctly — a Chinese sentence, twenty
    // bytes long — and the request that reached the bridge carried `text=%14`, a single byte of value
    // twenty. That is the answer's *length*, where the answer should have been, so something handed this
    // function a number instead of a string. Rather than build a URL out of it and make the provider
    // synthesize a click, say so with the evidence: the first bytes and the length.
    if ((unsigned char)reply[0] < 0x20u) {
        ESP_LOGW(TAG, "回复不像文本(首字节 0x%02X, 长度 %u);不朗读",
                 (unsigned char)reply[0], (unsigned)strlen(reply));
        return ESP_ERR_INVALID_ARG;
    }

    // A long answer is spoken as a bounded prefix. Speaking a kilobyte of Chinese would be half a
    // minute of audio, and the encoding for a query string triples whatever it carries, so an
    // unbounded answer would need a buffer this board would rather spend elsewhere. The full text is
    // on the screen; what is said is the beginning of it, and the log says when it was shortened.
    char spoken[192];
    const size_t keep = passport_utf8_truncate(reply, sizeof(spoken) - 1u);
    memcpy(spoken, reply, keep);
    spoken[keep] = '\0';
    if (keep < strlen(reply)) {
        ESP_LOGI(TAG, "回答较长(%u 字节),朗读前 %u 字节", (unsigned)strlen(reply), (unsigned)keep);
    }

    // The answer is Chinese, so it has to be escaped before it can sit in a query string.
    static char encoded[768];
    const size_t needed = passport_url_encode(spoken, encoded, sizeof(encoded));
    if (needed >= sizeof(encoded)) {
        ESP_LOGW(TAG, "朗读文本无法编码(%u 字节)", (unsigned)needed);
        return ESP_ERR_INVALID_SIZE;
    }

    char url[1024];
    snprintf(url, sizeof(url), "%s/speak?text=%s", cfg->bridge_url, encoded);

    char body[1024];
    if (direct && (!passport_provider_endpoint(cfg->tts_base_url, "/audio/speech", url, sizeof(url)) ||
        !passport_provider_tts_body(cfg->tts_model,cfg->tts_voice,spoken,body,sizeof(body))))
        return ESP_ERR_INVALID_ARG;
    static speech_headers_t headers;
    headers.seen = false;
    headers.allowed = true;
    const esp_http_client_config_t config = {
        .url = url,
        .method = direct ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .event_handler = speech_header_event,
        .user_data = &headers,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const char *key = direct ? cfg->tts_api_key : cfg->bridge_token;
    if (key[0] != '\0') {
        char auth[160];
        snprintf(auth, sizeof(auth), "Bearer %s", key);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    if (direct) esp_http_client_set_header(client,"Content-Type","application/json");
    esp_err_t err = esp_http_client_open(client, direct ? (int)strlen(body) : 0);
    if (err == ESP_OK && direct && !write_all(client,body,strlen(body))) err=ESP_FAIL;
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    // This returns a *length*, not a status: a positive content length, 0 for a chunked or
    // length-less body, or -1 on error. Treating it as an esp_err_t is what made an earlier version
    // of this file report a transport failure on every response that had a body — the same mistake,
    // made twice, in the upload path and here. See the note beside the upload.
    const int64_t length = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (length < 0 || status != 200) {
        ESP_LOGI(TAG, "桥接服务没有朗读音频(HTTP %d,长度 %lld),回答已上屏", status,
                 (long long)length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return (status == 404) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    if (s_cancelled) {
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_STATE;
    }
    // A 200 is not by itself audio. A JSON body, an MP3 or a WAV served by mistake would be played as
    // PCM and come out of the speaker as noise, so the declared type is checked and an undeclared one
    // is refused: only the raw little-endian formats this device can actually play are accepted.
    if (!headers.seen || !headers.allowed) {
        ESP_LOGW(TAG, "朗读响应不是原始 PCM(Content-Type %s),不播放",
                 headers.seen ? "unsupported" : "missing");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_ARG;
    }
    err = passport_audio_play_begin();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "音频通路被占用,无法朗读: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    // Bytes are read into a byte buffer and only whole samples are played. A read can end on an odd
    // byte, and dropping it would shift every later sample by one byte — which is not a click, it is
    // the rest of the sentence turned into noise. The leftover byte is carried into the next read.
    //
    // No byte swapping: the bridge sends little-endian PCM and so is this chip.
    static uint8_t raw[CHUNK_SAMPLES * sizeof(int16_t)];
    static int16_t samples_buf[CHUNK_SAMPLES];
    size_t pending = 0u;
    size_t played = 0u;
    const int64_t deadline = esp_timer_get_time() + 60000000;
    for (;;) {
        if (s_cancelled) { err=ESP_ERR_INVALID_STATE; break; }
        if (esp_timer_get_time() >= deadline || played >= 16000u * 60u) { err=ESP_ERR_TIMEOUT; break; }
        const int read = esp_http_client_read(client, (char *)(raw + pending),
                                             (int)(sizeof(raw) - pending));
        if (read <= 0) {
            if (read < 0 || !esp_http_client_is_complete_data_received(client) || pending) err=ESP_FAIL;
            break;
        }
        if (s_cancelled) { err=ESP_ERR_INVALID_STATE; break; }
        const size_t total = pending + (size_t)read;
        const size_t whole = total / sizeof(int16_t);
        if (whole > 0u) {
            memcpy(samples_buf, raw, whole * sizeof(int16_t));
        }
        pending = total - whole * sizeof(int16_t);
        if (pending > 0u) {
            memmove(raw, raw + whole * sizeof(int16_t), pending);
        }
        if (whole == 0u) {
            continue;
        }
        const esp_err_t write = passport_audio_play_chunk(samples_buf, whole);
        if (write != ESP_OK) {
            err = write;
            break;
        }
        played += whole;
    }
    passport_audio_play_end();
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    snprintf(s_last, sizeof(s_last), "spoke %u KB (%u samples)",
             (unsigned)((played * sizeof(int16_t)) / 1024u), (unsigned)played);
    ESP_LOGI(TAG, "朗读完成:%u 个采样", (unsigned)played);
    return err != ESP_OK ? err : ((played > 0u) ? ESP_OK : ESP_FAIL);
}

// Serialize the static PCM buffers across CLI, notification, and speech callers.
static portMUX_TYPE s_speak_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_speaking;
static esp_err_t speak_text(const char *reply)
{
    taskENTER_CRITICAL(&s_speak_lock);
    if (s_speaking) { taskEXIT_CRITICAL(&s_speak_lock); return ESP_ERR_INVALID_STATE; }
    s_speaking=true;
    taskEXIT_CRITICAL(&s_speak_lock);
    esp_err_t result=speak_text_impl(reply);
    taskENTER_CRITICAL(&s_speak_lock);
    s_speaking=false;
    taskEXIT_CRITICAL(&s_speak_lock);
    return result;
}

// The public entry point, which the console uses to check playback without a spoken turn. `text` NULL
// means the model's last answer.
esp_err_t passport_speech_speak(const char *text)
{
    // Legacy entry point now accepts work rather than running TLS on caller stacks.
    return passport_speech_request_speak(text);
}

esp_err_t passport_speech_request_speak(const char *text)
{
    char copy[192];
    if (!s_task) return ESP_ERR_INVALID_STATE;
    if (!text) {
        esp_err_t err = passport_chat_copy_last_reply(copy, sizeof(copy));
        if (err != ESP_OK) return err;
    } else {
        size_t n = passport_utf8_truncate(text, sizeof(copy)-1);
        memcpy(copy, text, n); copy[n] = 0;
    }
    if (!copy[0]) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_intent_lock);
    if (s_turn_active || s_started || s_speak_pending) {
        taskEXIT_CRITICAL(&s_intent_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_cancelled = false;
    memcpy(s_speak_request, copy, sizeof(copy));
    s_speak_pending = true;
    taskEXIT_CRITICAL(&s_intent_lock);
    wake();
    return ESP_OK;
}

// Drive one whole turn. Runs on the speech task, so it may block.
static void run_turn(void)
{
    s_started = false;
    s_turns++;
    s_woke_us = esp_timer_get_time();

    passport_voice_action_t action = passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_TALK_PRESS);
    if (action != PASSPORT_VOICE_ACT_OPEN_MIC) {
        // Say so. A press that is accepted and then quietly does nothing is what the owner reported as
        // "holding OK does nothing", and the console's record of it was the only place it was visible.
        snprintf(s_last, sizeof(s_last), "press refused in state %s",
                 passport_voice_state_name(s_voice.state));
        passport_app_show_error("还在处理上一条");
        return;
    }
    passport_app_show_status("听你说…", PASSPORT_PET_LISTENING);
    s_ui_us = esp_timer_get_time();

    s_last_transcript[0] = 0;
    const passport_config_t *cfg = passport_config_current();
    const int result = (cfg->stt_base_url[0] && cfg->stt_model[0]) ? direct_turn() : stream_turn();

    // The microphone is closed by now in every path, which is what the turn rules require before
    // the audio is interpreted.
    action = passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_TALK_RELEASE);
    if (s_cancelled) {
        passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_FAILED);
        passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_RESET);
        return;
    }
    if (result == -1) {
        passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_FAILED);
        // Distinguish "this device has no network" from everything else, and say the first one in words
        // the owner can act on: the console's record ("not associated") is for the console.
        if (passport_wifi_state() != PASSPORT_WIFI_CONNECTED) {
            passport_app_show_error("没连上网络,先配网吧");
        } else {
            passport_app_show_error(s_last);
        }
        passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_RESET);
        return;
    }
    if (result == -2) {
        // Silence is not a failure: it is a room with nobody talking in it. But the numbers matter,
        // because a threshold set above a quiet voice looks exactly like a broken device, and the
        // owner is the only one who can say whether that is what happened. The console command is
        // named here because there is no other way to change it today.
        char message[128];
        snprintf(message, sizeof(message), "太安静了(峰值%u/阈值%d)\n串口 voice gate 600 可调",
                 (unsigned)s_last_peak, s_last_gate);
        passport_app_show_status(message, PASSPORT_PET_CURIOUS);
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
    if (action == PASSPORT_VOICE_ACT_ASK_MODEL && !s_cancelled) {
        // The rest of the conversation is the path that already works, unchanged.
        uint32_t request_id = 0;
        if (passport_chat_submit(s_last_transcript, &request_id) != ESP_OK) {
            passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_FAILED);
            passport_app_show_error("模型正忙");
            passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_RESET);
            return;
        }

        char reply[1024];
        esp_err_t completion = ESP_ERR_NOT_FINISHED;
        const int64_t deadline = esp_timer_get_time() + 35000000;
        while (!s_cancelled && completion == ESP_ERR_NOT_FINISHED && esp_timer_get_time() < deadline) {
            completion = passport_chat_result(request_id, reply, sizeof(reply));
            if (completion == ESP_ERR_NOT_FINISHED) vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (s_cancelled || completion != ESP_OK) {
            passport_chat_abandon(request_id);
            passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_FAILED);
            passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_RESET);
            if (!s_cancelled) passport_app_show_error("Model request failed or timed out");
            return; // Never synthesize a previous request's reply after failure/timeout.
        }
        passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_REPLY);
        // The reply is on the screen by now; asking the bridge to say it is what closes the turn.
        // The state machine insists the microphone is shut before this, and the arbiter enforces it.
        esp_err_t spoken = speak_text(reply);
        if (spoken != ESP_OK) {
            ESP_LOGI(TAG, "这一回合只上屏不发声");
            if (!s_cancelled) passport_app_show_error("Speech failed; reply retained");
        }
        passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_SPEECH_DONE);
        snprintf(s_last, sizeof(s_last), "%s", s_cancelled ? "turn cancelled" :
                 (spoken == ESP_OK ? "turn completed with speech" : "reply ready; speech failed"));
    }
    // Cancellation between TRANSCRIPT and model admission must not strand THINKING.
    if (s_cancelled) {
        passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_FAILED);
        passport_voice_handle(&s_voice, PASSPORT_VOICE_EV_RESET);
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
        char spoken[192];
        taskENTER_CRITICAL(&s_intent_lock);
        bool do_speak = s_speak_pending;
        bool do_turn = s_started;
        if (do_speak) {
            memcpy(spoken, s_speak_request, sizeof(spoken));
            s_speak_pending = false;
        }
        s_turn_active = do_speak || do_turn;
        taskEXIT_CRITICAL(&s_intent_lock);
        if (do_speak) {
            esp_err_t err = speak_text(spoken);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Queued speech failed: %s", esp_err_to_name(err));
                snprintf(s_last, sizeof(s_last), "%s", s_cancelled ? "speech cancelled" : "speech failed");
                if (!s_cancelled) passport_app_show_error("Speech failed");
            }
        } else if (do_turn) {
            run_turn();
        }
        taskENTER_CRITICAL(&s_intent_lock);
        s_turn_active = false;
        s_release_early = false;
        taskEXIT_CRITICAL(&s_intent_lock);
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
