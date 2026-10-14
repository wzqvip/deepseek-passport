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
#include "passport_notify.h"
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

esp_err_t passport_work_client_answer(const char *answer)
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

    char body[256];
    snprintf(body, sizeof(body), "{\"answer\":\"%s\"}", answer);

    char response[512];
    passport_http_result_t result;
    const passport_http_request_t request = {
        .url = url,
        .api_key = NULL,
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
    build_url(PASSPORT_WORK_PATH_STATUS, url, sizeof(url));

    static char document[STATUS_MAX];
    passport_http_result_t result;
    const passport_http_request_t request = {
        .url = url,
        .api_key = NULL,
        .body = NULL,   // a GET
        .timeout_ms = 8000u,
    };
    const esp_err_t err = passport_http_request(&request, &result, document, sizeof(document));
    s_polls++;
    if (err != ESP_OK) {
        s_failures++;
        snprintf(s_last, sizeof(s_last), "fetch failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "取状态失败: %s", esp_err_to_name(err));
        return;
    }
    if (result.status != 200) {
        s_failures++;
        snprintf(s_last, sizeof(s_last), "fetch returned HTTP %d", result.status);
        ESP_LOGW(TAG, "取状态返回 HTTP %d", result.status);
        return;
    }

    passport_work_view_t parsed;
    if (!passport_work_parse(document, &parsed)) {
        s_failures++;
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
        return;
    }
    if (state_changed) {
        ESP_LOGI(TAG, "工作状态变为 %s: %s", passport_work_state_name(parsed.state), parsed.task);
    }

    const uint8_t mode = passport_config_current()->notify_mode;
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
        // Sleep in short steps so a request to poll now is honoured quickly.
        for (int waited = 0; s_running && waited < POLL_INTERVAL_MS; waited += 100) {
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
