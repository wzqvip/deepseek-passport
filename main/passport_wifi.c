// main/passport_wifi.c
#include "passport_wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "passport_config_store.h"

static const char *TAG = "passport_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1
#define SCAN_MAX_AP        20
// A device that sits on a shelf should rejoin by itself after a router blip. The cap stops
// a wrong password from becoming an endless retry loop.
#define RECONNECT_MAX      5

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static passport_wifi_state_t s_state = PASSPORT_WIFI_IDLE;
static char s_ip[16];
static bool s_started;
static bool s_user_disconnect;
static int s_reconnects;

const char *passport_wifi_state_name(passport_wifi_state_t state)
{
    switch (state) {
    case PASSPORT_WIFI_IDLE:
        return "idle";
    case PASSPORT_WIFI_CONNECTING:
        return "connecting";
    case PASSPORT_WIFI_CONNECTED:
        return "connected";
    case PASSPORT_WIFI_FAILED:
        return "failed";
    default:
        return "?";
    }
}

passport_wifi_state_t passport_wifi_state(void)
{
    return s_state;
}

void passport_wifi_ip(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return;
    }
    snprintf(out, out_size, "%s", s_ip);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == WIFI_EVENT_STA_START) {
        return;
    }
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        // A disconnect during a connect attempt is a failure, not an idle state;
        // the connect call decides which when it wakes up.
        if (s_state == PASSPORT_WIFI_CONNECTING) {
            s_state = PASSPORT_WIFI_FAILED;
            xEventGroupSetBits(s_events, WIFI_FAILED_BIT);
        } else {
            s_state = PASSPORT_WIFI_IDLE;
            // A link that was up and dropped gets retried, unless the user asked for the
            // disconnect. This is what lets the device recover from a router restart
            // without anyone touching it.
            const passport_config_t *cfg = passport_config_current();
            if (!s_user_disconnect && cfg->wifi_ssid[0] != '\0' && s_reconnects < RECONNECT_MAX) {
                s_reconnects++;
                ESP_LOGW(TAG, "连接已断开,重连 %d/%d", s_reconnects, RECONNECT_MAX);
                esp_wifi_connect();
            }
        }
        s_ip[0] = '\0';
        ESP_LOGW(TAG, "已断开连接");
        return;
    }
    if (id == WIFI_EVENT_SCAN_DONE) {
        return;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_state = PASSPORT_WIFI_CONNECTED;
    s_reconnects = 0;   // a fresh association resets the retry budget
    xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    ESP_LOGI(TAG, "已获取地址: %s", s_ip);
}

esp_err_t passport_net_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "事件循环创建失败: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t passport_wifi_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t err = passport_net_init();
    if (err != ESP_OK) {
        return err;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "station 网络接口创建失败");
        return ESP_FAIL;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "事件处理注册失败: %s", esp_err_to_name(err));
        return err;
    }

    // Do not knock down a running access point on the way to bringing the station up.
    //
    // This used to force WIFI_MODE_STA unconditionally, which stops any access point that is running.
    // The provisioning hotspot is raised with WIFI_MODE_APSTA, so the sequence "open the setup screen,
    // then something reconnects the station" — a router hiccup, the bounded auto-reconnect, or simply
    // this function being called again — switched the mode to station-only, the hotspot's beacon
    // stopped, and a phone that was halfway through associating failed. Which is what the owner saw:
    // "it will not connect", reported by the phone as a wrong password.
    wifi_mode_t current = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&current) == ESP_OK &&
        (current == WIFI_MODE_AP || current == WIFI_MODE_APSTA)) {
        ESP_LOGI(TAG, "热点在运行,保持 APSTA 而不是切成纯 station");
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    } else {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "station 启动失败: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "station 已启动");
    return ESP_OK;
}

int passport_wifi_scan(passport_wifi_ap_t *out, int max, uint32_t timeout_ms)
{
    if (out == NULL || max <= 0) {
        return -1;
    }
    const esp_err_t init_err = passport_wifi_init();
    if (init_err != ESP_OK) {
        return -1;
    }

    // Scanning while associated drops the association on this chip, so say so
    // rather than letting the caller discover it later.
    if (s_state == PASSPORT_WIFI_CONNECTED) {
        ESP_LOGW(TAG, "扫描会中断当前连接");
    }

    const wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "扫描失败: %s", esp_err_to_name(err));
        return -1;
    }
    (void)timeout_ms;   // the synchronous scan already bounds itself

    uint16_t found = 0u;
    err = esp_wifi_scan_get_ap_num(&found);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取扫描数量失败: %s", esp_err_to_name(err));
        return -1;
    }
    if (found == 0u) {
        return 0;
    }

    wifi_ap_record_t *records = calloc(SCAN_MAX_AP, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        ESP_LOGE(TAG, "扫描结果缓冲分配失败");
        return -1;
    }

    uint16_t wanted = found;
    if (wanted > SCAN_MAX_AP) {
        wanted = SCAN_MAX_AP;
    }
    err = esp_wifi_scan_get_ap_records(&wanted, records);
    if (err != ESP_OK) {
        free(records);
        ESP_LOGE(TAG, "读取扫描结果失败: %s", esp_err_to_name(err));
        return -1;
    }

    int written = 0;
    for (uint16_t i = 0u; i < wanted && written < max; i++) {
        snprintf(out[written].ssid, sizeof(out[written].ssid), "%s",
                 (const char *)records[i].ssid);
        out[written].rssi = records[i].rssi;
        out[written].channel = records[i].primary;
        out[written].secure = (records[i].authmode != WIFI_AUTH_OPEN);
        written++;
    }
    free(records);
    return written;
}

int passport_wifi_rssi(void)
{
    wifi_ap_record_t ap_info;
    memset(&ap_info, 0, sizeof(ap_info));
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return -128;
    }
    return (int)ap_info.rssi;
}

esp_err_t passport_wifi_connect(uint32_t timeout_ms)
{
    const passport_config_t *cfg = passport_config_current();
    if (cfg->wifi_ssid[0] == '\0') {
        // Refuse before touching the radio: there is nothing to connect to, and a
        // clear error beats a 15-second timeout.
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = passport_wifi_init();
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wifi_cfg = { 0 };

    // The configuration allows a 95-character string; the Wi-Fi stack does not.
    // Truncating would silently associate with a different network, so refuse and
    // say which limit was hit. This is the compiler's format-truncation warning
    // turned into an explicit check.
    const size_t ssid_len = strlen(cfg->wifi_ssid);
    const size_t pass_len = strlen(cfg->wifi_password);
    if (ssid_len >= sizeof(wifi_cfg.sta.ssid)) {
        ESP_LOGE(TAG, "SSID 过长:%u 字节,上限 %u 字节",
                 (unsigned)ssid_len, (unsigned)(sizeof(wifi_cfg.sta.ssid) - 1u));
        return ESP_ERR_INVALID_ARG;
    }
    if (pass_len >= sizeof(wifi_cfg.sta.password)) {
        ESP_LOGE(TAG, "密码过长:%u 字节,上限 %u 字节",
                 (unsigned)pass_len, (unsigned)(sizeof(wifi_cfg.sta.password) - 1u));
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(wifi_cfg.sta.ssid, cfg->wifi_ssid, ssid_len + 1u);
    memcpy(wifi_cfg.sta.password, cfg->wifi_password, pass_len + 1u);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;   // the stored password decides

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "station 配置写入失败: %s", esp_err_to_name(err));
        return err;
    }

    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    s_state = PASSPORT_WIFI_CONNECTING;
    s_user_disconnect = false;
    s_reconnects = 0;
    s_ip[0] = '\0';

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_state = PASSPORT_WIFI_FAILED;
        return err;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }
    if ((bits & WIFI_FAILED_BIT) != 0) {
        return ESP_FAIL;
    }
    s_state = PASSPORT_WIFI_FAILED;
    ESP_LOGW(TAG, "连接超时(%u ms)", (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t passport_wifi_disconnect(void)
{
    if (!s_started) {
        return ESP_OK;
    }
    // Suppress the automatic retry: this disconnect was requested.
    s_user_disconnect = true;
    const esp_err_t err = esp_wifi_disconnect();
    s_state = PASSPORT_WIFI_IDLE;
    s_ip[0] = '\0';
    return err;
}
