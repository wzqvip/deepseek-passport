// main/passport_chat.c
#include "passport_chat.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "passport_app.h"
#include "passport_config_store.h"
#include "passport_http.h"
#include "passport_json.h"
#include "passport_provider.h"
#include "passport_utf8.h"
#include "passport_wifi.h"

static const char *TAG = "passport_chat";

// A TLS handshake lives on this stack. 4096 panicked the console task when it tried
// the same work, so this task is given room rather than discovered to be short.
#define CHAT_TASK_STACK 12288
#define CHAT_TASK_PRIO  4
#define CHAT_QUEUE_DEPTH 2
#define PROMPT_MAX      200
#define RESPONSE_MAX    2048
#define ANSWER_MAX      1024

typedef struct {
    char prompt[PROMPT_MAX];
} chat_request_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static bool s_busy;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_next_id;
static uint32_t s_reserved_id;
static bool s_completed;
static esp_err_t s_result;
static char *s_response;
static char *s_answer;
static char s_previous_prompt[PROMPT_MAX];
static char s_previous_reply[256];
static char s_body[2048]; // Single chat worker; keep TLS stack headroom.

const char *passport_chat_last_reply(void)
{
    if (s_answer == NULL) {
        return "";
    }
    return s_answer;
}

esp_err_t passport_chat_copy_last_reply(char *out, size_t size)
{
    if (!out || !size) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_lock);
    out[0] = 0;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!s_busy && s_answer && s_answer[0]) {
        snprintf(out, size, "%s", s_answer);
        result = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_lock);
    return result;
}

bool passport_chat_busy(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool busy = s_busy || s_reserved_id != 0;
    taskEXIT_CRITICAL(&s_lock);
    return busy;
}

static void build_endpoint(const char *base, char *out, size_t out_size)
{
    if (strstr(base, "/chat/completions") != NULL) {
        snprintf(out, out_size, "%s", base);
    } else {
        snprintf(out, out_size, "%s/chat/completions", base);
    }
}

static esp_err_t run_turn(const char *prompt)
{
    const passport_config_t *cfg = passport_config_current();
    if (cfg->llm_base_url[0] == '\0' || cfg->llm_model[0] == '\0') {
        passport_app_show_error("No model configured");
        return ESP_FAIL;
    }
    if (passport_wifi_state() != PASSPORT_WIFI_CONNECTED) {
        // Saying so immediately beats a fifteen-second DNS timeout behind a spinning
        // pet.
        passport_app_show_error("No network");
        return ESP_FAIL;
    }

    passport_app_begin_turn(prompt);

    char url[192];
    build_endpoint(cfg->llm_base_url, url, sizeof(url));

    if (!passport_provider_chat_body(cfg->llm_model, prompt, s_previous_prompt,
            s_previous_reply, s_body, sizeof(s_body))) {
        passport_app_show_error("Request text too long");
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t largest_before =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    passport_http_result_t res;
    const passport_http_request_t req = {
        .url = url,
        .api_key = cfg->llm_api_key,
        .body = s_body,
        .timeout_ms = 30000u,
    };
    const esp_err_t err = passport_http_request(&req, &res, s_response, RESPONSE_MAX);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "传输失败: %s", esp_err_to_name(err));
        passport_app_show_error("Network error");
        return ESP_FAIL;
    }
    if (res.status != 200) {
        ESP_LOGW(TAG, "HTTP %d", res.status);
        passport_app_show_error("Request failed");
        return ESP_FAIL;
    }

    size_t decoded = 0u;
    const passport_json_status_t js =
        passport_json_string(s_response, strlen(s_response), "content", s_answer, ANSWER_MAX, &decoded);
    if (js != PASSPORT_JSON_OK && js != PASSPORT_JSON_TRUNCATED) {
        ESP_LOGW(TAG, "无法解析回复(状态 %d)", (int)js);
        passport_app_show_error("Unreadable reply");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "一轮完成: %lld ms, 回复 %u 字节, 会话中最大连续空闲块 %u B (此前 %u B)",
             (long long)res.elapsed_ms, (unsigned)strlen(s_answer),
             (unsigned)res.largest_during, (unsigned)largest_before);

    if (!s_answer[0]) return ESP_FAIL;
    snprintf(s_previous_prompt, sizeof(s_previous_prompt), "%s", prompt);
    size_t keep = passport_utf8_truncate(s_answer, sizeof(s_previous_reply)-1);
    memcpy(s_previous_reply, s_answer, keep); s_previous_reply[keep] = 0;
    passport_app_show_reply(s_answer);
    return ESP_OK;
}

static void chat_task(void *arg)
{
    (void)arg;
    chat_request_t request;

    for (;;) {
        if (xQueueReceive(s_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_answer[0] = '\0';
        esp_err_t result = run_turn(request.prompt);
        if (result != ESP_OK) s_answer[0] = '\0';
        taskENTER_CRITICAL(&s_lock);
        s_result = result;
        s_completed = true;
        s_busy = false;
        taskEXIT_CRITICAL(&s_lock);
    }
}

esp_err_t passport_chat_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    // Allocated once. Per-turn allocation is what fragments a heap this small.
    s_response = malloc(RESPONSE_MAX);
    s_answer = malloc(ANSWER_MAX);
    if (s_response == NULL || s_answer == NULL) {
        free(s_response);
        free(s_answer);
        s_response = NULL;
        s_answer = NULL;
        ESP_LOGE(TAG, "对话缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }
    // Cleared, because this buffer is read by callers whenever they like — the speaker reads it after a
    // turn — and handing out whatever malloc happened to leave there is how a device ends up speaking a
    // number. An empty string is at least honestly empty.
    s_response[0] = '\0';
    s_answer[0] = '\0';

    s_queue = xQueueCreate(CHAT_QUEUE_DEPTH, sizeof(chat_request_t));
    if (s_queue == NULL) {
        free(s_response); free(s_answer);
        s_response = NULL; s_answer = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(chat_task, "passport_chat", CHAT_TASK_STACK, NULL, CHAT_TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        vQueueDelete(s_queue); s_queue = NULL;
        free(s_response); free(s_answer);
        s_response = NULL; s_answer = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "对话线程已就绪(栈 %u 字节)", (unsigned)CHAT_TASK_STACK);
    return ESP_OK;
}

static esp_err_t submit(const char *prompt, uint32_t *id)
{
    if (prompt == NULL || prompt[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strlen(prompt) >= PROMPT_MAX) return ESP_ERR_INVALID_SIZE;
    if (s_queue == NULL) return ESP_ERR_INVALID_STATE;
    chat_request_t request;
    snprintf(request.prompt, sizeof(request.prompt), "%s", prompt);
    taskENTER_CRITICAL(&s_lock);
    if (s_busy || s_reserved_id != 0) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true; // Admission must precede publishing to the worker.
    s_completed = false;
    if (++s_next_id == 0) ++s_next_id;
    s_reserved_id = id ? s_next_id : 0;
    if (id) *id = s_next_id;
    taskEXIT_CRITICAL(&s_lock);
    if (xQueueSend(s_queue, &request, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_lock);
        s_busy = false;
        s_reserved_id = 0;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t passport_chat_ask(const char *prompt) { return submit(prompt, NULL); }

esp_err_t passport_chat_submit(const char *prompt, uint32_t *id)
{
    if (!id) return ESP_ERR_INVALID_ARG;
    *id = 0;
    return submit(prompt, id);
}

esp_err_t passport_chat_result(uint32_t id, char *out, size_t size)
{
    if (!out || !size || !id) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    taskENTER_CRITICAL(&s_lock);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_reserved_id == id) {
        result = s_completed ? s_result : ESP_ERR_NOT_FINISHED;
        if (s_completed) {
            if (result == ESP_OK) snprintf(out, size, "%s", s_answer);
            s_reserved_id = 0;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return result;
}

void passport_chat_abandon(uint32_t id)
{
    taskENTER_CRITICAL(&s_lock);
    if (id && s_reserved_id == id) s_reserved_id = 0;
    taskEXIT_CRITICAL(&s_lock);
}
