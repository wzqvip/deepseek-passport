// main/passport_work_client.c
#include "passport_work_client.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "passport_app.h"
#include "passport_config_store.h"
#include "passport_http.h"
#include "passport_json.h"
#include "passport_notify.h"
#include "passport_speech.h"
#include "passport_power.h"
#include "passport_wifi.h"
#include "passport_work.h"

static const char *TAG = "passport_work_cl";

#define POLL_INTERVAL_MS 5000
// Enough for a status document with four options; the bridge is ours, so the size is a contract
// rather than a guess.
#define STATUS_MAX 1024
// The HTTP client needs a few kilobytes of stack for its own work even without TLS.
#define CLIENT_STACK 5120
#define URL_MAX 192

static TaskHandle_t s_task;
static volatile bool s_poll_now;
static volatile bool s_running;
static char s_last[96] = "no attempt yet";
static char s_last_task[PASSPORT_WORK_TASK_MAX];
static char s_last_question[PASSPORT_WORK_QUESTION_MAX];
static passport_work_state_t s_last_state = PASSPORT_WORK_IDLE;
static bool s_have_document;
static int s_polls;
static int s_failures;
// Failures since the last successful poll. Two is enough to stop blaming a single lost packet, and small
// enough that a wrong address shows on the screen within about ten seconds instead of never.
#define WORK_LINK_FAILURES_SHOWN 2
static int s_consecutive_failures;
// The bridge's status version this device has already displayed; sent as since so a long-poll returns
// only when something changed.
static int s_status_version;

static void build_url(const char *path, char *out, size_t out_size)
{
    const passport_config_t *cfg = passport_config_current();
    snprintf(out, out_size, "%s%s", cfg->bridge_url, path);
}

void passport_work_client_poll_now(void)
{
    s_poll_now = true;
}

void passport_work_client_report(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return;
    }
    const passport_config_t *cfg = passport_config_current();
    snprintf(out, out_size,
             "bridge=%s polls=%d failures=%d last=\"%s\"",
             (cfg->bridge_url[0] != '\0') ? cfg->bridge_url : "(not configured)", s_polls,
             s_failures, s_last);
}

bool passport_work_client_link_ok(void)
{
    const passport_config_t *cfg = passport_config_current();
    if (cfg->bridge_url[0] == '\0') {
        return true;   // nothing is configured, so nothing is broken
    }
    // Consecutive rather than cumulative: one success means the address and the token are right, and a
    // later single failure is not worth a permanent warning.
    return s_consecutive_failures < WORK_LINK_FAILURES_SHOWN;
}

esp_err_t passport_work_client_answer(const char *answer)
{
    return passport_work_client_answer_for(NULL, answer);
}

esp_err_t passport_work_client_answer_for(const char *question_id, const char *answer)
{
    if (answer == NULL || answer[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const passport_config_t *cfg = passport_config_current();
    if (cfg->bridge_url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (passport_wifi_state() != PASSPORT_WIFI_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[URL_MAX];
    build_url(PASSPORT_WORK_PATH_ANSWER, url, sizeof(url));

    char body[768];
    if (!passport_work_answer_json(question_id, answer, body, sizeof(body))) {
        return ESP_ERR_INVALID_SIZE;
    }

    char response[512];
    passport_http_result_t result;
    const passport_http_request_t request = {
        .url = url,
        .api_key = cfg->bridge_token,
        .body = body,
        .timeout_ms = 8000u,
    };
    const esp_err_t err = passport_http_request(&request, &result, response, sizeof(response));
    if (err != ESP_OK) {
        snprintf(s_last, sizeof(s_last), "answer failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "回答发送失败: %s", esp_err_to_name(err));
        return err;
    }
    if (result.status != 200) {
        snprintf(s_last, sizeof(s_last), "answer rejected: HTTP %d", result.status);
        ESP_LOGW(TAG, "桥接服务拒绝了回答: HTTP %d", result.status);
        return ESP_FAIL;
    }
    snprintf(s_last, sizeof(s_last), "answer accepted");
    ESP_LOGI(TAG, "回答已送达(%d 字节响应,%lld ms)", result.body_len, (long long)result.elapsed_ms);
    return ESP_OK;
}

static void poll_once(void)
{
    const passport_config_t *cfg = passport_config_current();
    if (cfg->bridge_url[0] == '\0') {
        snprintf(s_last, sizeof(s_last), "no bridge configured");
        return;
    }
    if (passport_wifi_state() != PASSPORT_WIFI_CONNECTED) {
        snprintf(s_last, sizeof(s_last), "not associated");
        return;
    }

    char url[URL_MAX];
    // Long-poll: the bridge holds this request until there is something new or the wait expires, so the
    // screen reacts in milliseconds instead of waiting for the next interval. "since" is the version this
    // device has already seen; a timeout answers with {"unchanged":true}, which is success, not a failure.
    snprintf(url, sizeof(url), "%s%s?wait=15&since=%d", cfg->bridge_url, PASSPORT_WORK_PATH_STATUS,
             s_status_version);

    static char document[STATUS_MAX];
    passport_http_result_t result;
    const passport_http_request_t request = {
        .url = url,
        .api_key = cfg->bridge_token,
        .body = NULL,   // a GET
        // Longer than the bridge's maximum hold (15 s) so a held request is answered rather than cut off.
        .timeout_ms = 20000u,
    };
    const esp_err_t err = passport_http_request(&request, &result, document, sizeof(document));
    s_polls++;
    if (err != ESP_OK) {
        s_failures++;
        s_consecutive_failures++;
        snprintf(s_last, sizeof(s_last), "fetch failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "取状态失败: %s", esp_err_to_name(err));
        return;
    }
    if (result.status == 404) {
        s_consecutive_failures = 0;   // reachable and authenticated: the link is fine
        snprintf(s_last, sizeof(s_last), "no work to report");
        return;
    }
    // A long-poll that nothing disturbed. The bridge answered, the link is fine, and there is nothing to
    // put on the screen — three different things from a failed fetch.
    bool found = false;
    const int version = passport_json_int(document, passport_json_len(document), "version", &found);
    if (found) {
        s_status_version = version;
    }
    bool unchanged = false;
    const int flag = passport_json_int(document, passport_json_len(document), "unchanged", &unchanged);
    if (unchanged) {   // presence is enough; the bridge sends 1, and a JSON true must not become a parse failure
        s_consecutive_failures = 0;
        snprintf(s_last, sizeof(s_last), "bridge quiet (version %d)", s_status_version);
        return;
    }
    if (result.status != 200 || result.truncated) {
        s_failures++;
        s_consecutive_failures++;
        snprintf(s_last, sizeof(s_last), "fetch returned HTTP %d", result.status);
        ESP_LOGW(TAG, "取状态返回 HTTP %d", result.status);
        return;
    }

    s_consecutive_failures = 0;   // the bridge answered with a document

    passport_work_view_t parsed;
    if (!passport_work_parse(document, &parsed)) {
        s_failures++;
        s_consecutive_failures++;
        snprintf(s_last, sizeof(s_last), "document had nothing usable");
        ESP_LOGW(TAG, "状态文档没有可用内容");
        return;
    }

    // Worth announcing only when something changed: a poll every few seconds must not chatter.
    // A new question counts as a change even while the state stays "waiting", because that is
    // exactly when the owner is needed again.
    const bool state_changed = (parsed.state != s_last_state) ||
                               (strcmp(parsed.task, s_last_task) != 0);
    const bool question_changed = (parsed.question[0] != '\0') &&
                                  (strcmp(parsed.question, s_last_question) != 0);

    // The first document after boot is not announced. The question may have been waiting for
    // hours, and chiming at power-on would teach the owner to ignore the sound.
    const bool announce = s_have_document && (state_changed || question_changed);

    s_last_state = parsed.state;
    snprintf(s_last_task, sizeof(s_last_task), "%s", parsed.task);
    snprintf(s_last_question, sizeof(s_last_question), "%s", parsed.question);
    s_have_document = true;
    snprintf(s_last, sizeof(s_last), "fetched, state=%s", passport_work_state_name(parsed.state));

    if (!passport_app_work_feed(document)) {
        s_failures++;
        s_consecutive_failures++;
        return;
    }
    if (state_changed) {
        ESP_LOGI(TAG, "工作状态变为 %s: %s", passport_work_state_name(parsed.state), parsed.task);
    }

    if (announce && parsed.state == PASSPORT_WORK_WAITING) {
        passport_power_activity(); // Connected standby must surface a new question.
    }
    const uint8_t mode = passport_config_current()->notify_mode;
    if (announce && mode >= 2u && (parsed.state == PASSPORT_WORK_WAITING ||
        parsed.state == PASSPORT_WORK_DONE || parsed.state == PASSPORT_WORK_FAILED)) {
        const char *text = parsed.question[0] ? parsed.question : parsed.task;
        const esp_err_t queued = passport_speech_request_speak(text);
        if (queued != ESP_OK) ESP_LOGI(TAG, "Speech notification skipped: audio busy");
        return; // Never substitute a chime for the selected speech policy.
    }
    const passport_work_announcement_t announcement =
        passport_work_announce(announce, parsed.state, mode);
    if (announcement != PASSPORT_WORK_ANNOUNCE_NONE) {
        // Blocking, and this is the right task for it: the microphone is closed by the audio
        // arbiter for the duration, so a poll that arrives during a chime cannot open it.
        passport_notify_play(announcement);
    }
}

static void client_task(void *arg)
{
    (void)arg;
    while (s_running) {
        poll_once();
        // The poll itself now waits for news, so the gap between requests is short: a long-poll that has
        // just been answered should be renewed promptly, or the device is briefly deaf between requests.
        // After a failure the back-off is longer, which keeps a wrong address from becoming a busy loop.
        const int gap = (s_consecutive_failures > 0) ? POLL_INTERVAL_MS : 200;
        for (int waited = 0; s_running && waited < gap; waited += 100) {
            if (s_poll_now) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_poll_now = false;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t passport_work_client_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_running = true;
    if (xTaskCreate(client_task, "passport_work", CLIENT_STACK, NULL, 4, &s_task) != pdPASS) {
        s_running = false;
        s_task = NULL;
        ESP_LOGE(TAG, "工作模式轮询线程创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "工作模式轮询已启动(每 %d ms)", POLL_INTERVAL_MS);
    return ESP_OK;
}
