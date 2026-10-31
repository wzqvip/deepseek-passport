// main/passport_http.c
#include "passport_http.h"

#include <string.h>
#include <stdio.h>
#include <limits.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "passport_wifi.h"

static const char *TAG = "passport_http";

#define AUTH_HEADER_MAX 160

esp_err_t passport_http_request(const passport_http_request_t *request,
                                passport_http_result_t *result,
                                char *body_out, size_t body_out_size)
{
    if (request == NULL || result == NULL || request->url == NULL ||
        body_out == NULL || body_out_size == 0u || body_out_size > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    body_out[0] = '\0';

    // A socket cannot exist before the network stack does. Without this, lwIP
    // asserts on an invalid mailbox rather than returning an error.
    const esp_err_t net_err = passport_net_init();
    if (net_err != ESP_OK) {
        return net_err;
    }

    const esp_http_client_config_t cfg = {
        .url = request->url,
        .timeout_ms = (request->timeout_ms > 0u) ? (int)request->timeout_ms : 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP 客户端创建失败");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    size_t body_len = 0u;

    do {
        err = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (err != ESP_OK) {
            break;
        }
        if (request->api_key != NULL && request->api_key[0] != '\0') {
            char auth[AUTH_HEADER_MAX];
            const int written = snprintf(auth, sizeof(auth), "Bearer %s", request->api_key);
            if (written < 0 || (size_t)written >= sizeof(auth)) {
                // Refuse rather than send a truncated credential, which would look
                // like an authentication failure on the server side.
                ESP_LOGE(TAG, "API Key 过长,无法放入请求头");
                err = ESP_ERR_INVALID_ARG;
                break;
            }
            err = esp_http_client_set_header(client, "Authorization", auth);
            if (err != ESP_OK) {
                break;
            }
        }

        if (request->body != NULL) {
            body_len = strlen(request->body);
            err = esp_http_client_set_post_field(client, request->body, (int)body_len);
            if (err != ESP_OK) {
                break;
            }
            err = esp_http_client_set_method(client, HTTP_METHOD_POST);
        } else {
            err = esp_http_client_set_method(client, HTTP_METHOD_GET);
        }
        if (err != ESP_OK) {
            break;
        }

        const int64_t started_us = esp_timer_get_time();

        // open() performs the TCP connect and the TLS handshake, and sends nothing
        // until the first write. Measuring here is measuring with the session live.
        err = esp_http_client_open(client, (int)body_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "连接/握手失败: %s", esp_err_to_name(err));
            break;
        }

        result->heap_during = esp_get_free_heap_size();
        result->largest_during =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

        size_t sent = 0;
        while (sent < body_len) {
            const int written = esp_http_client_write(client, request->body + sent,
                                                       (int)(body_len - sent));
            if (written <= 0) {
                err = ESP_FAIL;
                break;
            }
            sent += (size_t)written;
        }
        if (err != ESP_OK) break;

        const int content_length = esp_http_client_fetch_headers(client);
        result->status = esp_http_client_get_status_code(client);
        if (content_length < 0 || result->status <= 0) {
            err = ESP_FAIL;
            break;
        }

        int total = 0;
        const int capacity = (int)body_out_size - 1;
        while (total < capacity) {
            const int read = esp_http_client_read(client, body_out + total, capacity - total);
            if (read < 0) {
                err = ESP_FAIL;
                break;
            }
            if (read == 0) break;
            total += read;
        }
        body_out[total] = '\0';
        result->body_len = total;
        result->truncated = (content_length > total);
        if (err == ESP_OK && total == capacity && !esp_http_client_is_complete_data_received(client)) {
            char extra;
            const int more = esp_http_client_read(client, &extra, 1);
            if (more > 0) result->truncated = true;
            if (more < 0) err = ESP_FAIL;
        }
        // A declared length that was not delivered is a truncated response, and a caller that parsed it
        // would silently accept half a JSON document. That is what this final check is for.
        //
        // It is deliberately narrower than "the client says it did not finish". A response delimited by
        // the connection closing (HTTP/1.0 style, no Content-Length and no chunking) never reports
        // itself complete, and failing those would turn a working provider into "transport error".
        // Only a shortfall against a length the server itself announced is treated as a failure.
        if (err == ESP_OK && !result->truncated && content_length > 0 && total < content_length &&
            !esp_http_client_is_complete_data_received(client)) {
            err = ESP_FAIL;
        }
        result->elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    } while (false);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}
