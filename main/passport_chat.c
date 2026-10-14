// main/passport_chat.c
#include "passport_chat.h"

#include <stdlib.h>
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
static volatile bool s_busy;
static char *s_response;
static char *s_answer;

const char *passport_chat_last_reply(void)
{
    if (s_answer == NULL) {
        return "";
    }
    return s_answer;
}

bool passport_chat_busy(void)
{
    return s_busy;
}

static void build_endpoint(const char *base, char *out, size_t out_size)
{
    if (strstr(base, "/chat/completions") != NULL) {
        snprintf(out, out_size, "%s", base);
    } else {
        snprintf(out, out_size, "%s/chat/completions", base);
    }
}

static void json_escape(const char *in, char *out, size_t out_size)
{
    size_t j = 0u;
    for (size_t i = 0u; in[i] != '\0' && j + 2u < out_size; i++) {
        const char c = in[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = c;
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if ((unsigned char)c < 0x20u) {
            out[j++] = ' ';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

static void run_turn(const char *prompt)
{
    const passport_config_t *cfg = passport_config_current();
    if (cfg->llm_base_url[0] == '\0' || cfg->llm_model[0] == '\0') {
        passport_app_show_error("No model configured");
        return;
    }
    if (passport_wifi_state() != PASSPORT_WIFI_CONNECTED) {
        // Saying so immediately beats a fifteen-second DNS timeout behind a spinning
        // pet.
        passport_app_show_error("No network");
        return;
    }

    passport_app_begin_turn(prompt);

    char url[192];
    build_endpoint(cfg->llm_base_url, url, sizeof(url));

    char escaped[320];
    json_escape(prompt, escaped, sizeof(escaped));

    char body[640];
    snprintf(body, sizeof(body),
             "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
             "\"max_tokens\":256,\"stream\":false}",
             cfg->llm_model, escaped);

    const size_t largest_before =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    passport_http_result_t res;
    const passport_http_request_t req = {
        .url = url,
        .api_key = cfg->llm_api_key,
        .body = body,
        .timeout_ms = 30000u,
    };
    const esp_err_t err = passport_http_request(&req, &res, s_response, RESPONSE_MAX);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "传输失败: %s", esp_err_to_name(err));
        passport_app_show_error("Network error");
        return;
    }
    if (res.status != 200) {
        ESP_LOGW(TAG, "HTTP %d", res.status);
        passport_app_show_error("Request failed");
        return;
    }

    size_t decoded = 0u;
    const passport_json_status_t js =
        passport_json_string(s_response, strlen(s_response), "content", s_answer, ANSWER_MAX, &decoded);
    if (js != PASSPORT_JSON_OK && js != PASSPORT_JSON_TRUNCATED) {
        ESP_LOGW(TAG, "无法解析回复(状态 %d)", (int)js);
        passport_app_show_error("Unreadable reply");
        return;
    }

    ESP_LOGI(TAG, "一轮完成: %lld ms, 回复 %u 字节, 会话中最大连续空闲块 %u B (此前 %u B)",
             (long long)res.elapsed_ms, (unsigned)strlen(s_answer),
             (unsigned)res.largest_during, (unsigned)largest_before);

    passport_app_show_reply(s_answer);
}

static void chat_task(void *arg)
{
    (void)arg;
    chat_request_t request;

    for (;;) {
        if (xQueueReceive(s_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        run_turn(request.prompt);
        s_busy = false;
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

    s_queue = xQueueCreate(CHAT_QUEUE_DEPTH, sizeof(chat_request_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(chat_task, "passport_chat", CHAT_TASK_STACK, NULL, CHAT_TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "对话线程已就绪(栈 %u 字节)", (unsigned)CHAT_TASK_STACK);
    return ESP_OK;
}

esp_err_t passport_chat_ask(const char *prompt)
{
    if (prompt == NULL || prompt[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }

    chat_request_t request;
    snprintf(request.prompt, sizeof(request.prompt), "%s", prompt);

    if (xQueueSend(s_queue, &request, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    return ESP_OK;
}
