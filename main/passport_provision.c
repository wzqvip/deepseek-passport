// main/passport_provision.c
#include "passport_provision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "passport_config_store.h"
#include "passport_portal.h"
#include "passport_wifi.h"

static const char *TAG = "passport_prov";

#define PROV_CHANNEL      6
#define PROV_MAX_CLIENTS  2
#define PAGE_MAX          2048
#define BODY_MAX          1024
#define AP_IP             "192.168.4.1"

static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;
static bool s_active;
static char s_ssid[33];
static char s_password[16];
static char s_address[16] = AP_IP;
static char *s_page;

bool passport_provision_active(void)
{
    return s_active;
}

// Defined further down, next to the radio setup it feeds.
static void derive_identity(void);

// The access point name and password come from the MAC, so they are a function of the device
// rather than of whether the radio has been started. Deriving them on demand removes an
// ordering trap: these accessors used to return empty strings until start() had run, so a
// caller that needed the credentials early silently built a code for the wrong payload.
static void ensure_identity(void)
{
    if (s_ssid[0] == '\0') {
        derive_identity();
    }
}

const char *passport_provision_ssid(void)
{
    ensure_identity();
    return s_ssid;
}

const char *passport_provision_password(void)
{
    ensure_identity();
    return s_password;
}

const char *passport_provision_address(void)
{
    return s_address;
}

// --- pages ------------------------------------------------------------------

static int render_root(char *out, size_t out_size)
{
    const passport_config_t *cfg = passport_config_current();

    char ssid[128];
    char model[128];
    char pet[128];
    char base[192];
    passport_portal_escape(cfg->wifi_ssid, ssid, sizeof(ssid));
    passport_portal_escape(cfg->llm_model, model, sizeof(model));
    passport_portal_escape(cfg->pet_name, pet, sizeof(pet));
    passport_portal_escape(cfg->llm_base_url, base, sizeof(base));

    // Secrets never appear as values: the field says whether one is stored and submits
    // nothing unless the owner types a replacement.
    const char *key_state = (cfg->llm_api_key[0] != '\0') ? "已设置,留空则不改动" : "未设置";
    const char *pass_state = (cfg->wifi_password[0] != '\0') ? "已设置,留空则不改动" : "未设置";

    const char *connected = (passport_wifi_state() == PASSPORT_WIFI_CONNECTED) ? "已连接" : "未连接";

    return snprintf(out, out_size,
        "<!doctype html><html lang=zh><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>DeepSeek Passport</title><style>"
        "body{font-family:system-ui,sans-serif;margin:0;padding:18px;background:#101828;color:#EAF0FF}"
        "h1{font-size:20px;margin:0 0 4px}p.sub{color:#8FA3C8;margin:0 0 18px;font-size:13px}"
        "label{display:block;margin:14px 0 4px;font-size:13px;color:#8FA3C8}"
        "input{width:100%%;box-sizing:border-box;padding:10px;font-size:15px;border-radius:8px;"
        "border:1px solid #2A3A5C;background:#18233A;color:#EAF0FF}"
        "button{margin-top:22px;width:100%%;padding:13px;font-size:16px;border:0;border-radius:8px;"
        "background:#4D6BFE;color:#fff;font-weight:600}"
        ".state{background:#18233A;border-left:3px solid #4D6BFE;padding:9px 12px;border-radius:6px;"
        "font-size:13px;color:#8FA3C8}</style></head><body>"
        "<h1>DeepSeek Passport</h1><p class=sub>设备设置 · 保存后重启生效</p>"
        "<div class=state>Wi-Fi %s · 热点 %s · 地址 %s · 可用内存 %u KB</div>"
        "<form method=post action=/save>"
        "<label>Wi-Fi 名称</label><input name=wifi_ssid value=\"%s\" autocapitalize=off>"
        "<label>Wi-Fi 密码 <span style=\"color:#4D6BFE\">(%s)</span></label>"
        "<input name=wifi_password type=password autocapitalize=off>"
        "<label>DeepSeek API Key <span style=\"color:#4D6BFE\">(%s)</span></label>"
        "<input name=llm_api_key type=password autocapitalize=off>"
        "<label>接口地址</label><input name=llm_base_url value=\"%s\" autocapitalize=off>"
        "<label>模型</label><input name=llm_model value=\"%s\" autocapitalize=off>"
        "<label>宠物名字</label><input name=pet_name value=\"%s\">"
        "<label>音量 (0-100)</label><input name=volume value=\"%u\" inputmode=numeric>"
        "<label>提示方式 (0 静音 / 1 提示音 / 2 朗读)</label>"
        "<input name=notify_mode value=\"%u\" inputmode=numeric>"
        "<button type=submit>保存并重启</button></form>"
        "<p class=sub style=\"margin-top:18px\">版本 %s</p></body></html>",
        connected, s_ssid, s_address, (unsigned)(esp_get_free_heap_size() / 1024u),
        ssid, pass_state, key_state, base, model, pet,
        (unsigned)cfg->volume, (unsigned)cfg->notify_mode, "passport");
}

static int render_status(char *out, size_t out_size)
{
    // Report the real radio and memory state. A status endpoint that answers with zeros is
    // worse than no endpoint: it looks like data.
    int rssi = 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    return snprintf(out, out_size,
        "{\"wifi\":\"%s\",\"rssi\":%d,\"ap\":\"%s\",\"ap_address\":\"%s\","
        "\"uptime_ms\":%lld,\"free_heap\":%u,\"largest_block\":%u}",
        passport_wifi_state_name(passport_wifi_state()), rssi,
        s_ssid, s_address, (long long)(esp_timer_get_time() / 1000),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static int render_config(char *out, size_t out_size)
{
    const passport_config_t *cfg = passport_config_current();
    int written = snprintf(out, out_size, "{");
    for (int i = 0; i < passport_cfg_field_count() && written > 0 && (size_t)written < out_size; i++) {
        const passport_cfg_field_t *f = passport_cfg_field(i);
        char value[128];
        passport_cfg_format(cfg, f, value, sizeof(value));
        char escaped[160];
        passport_portal_escape(value, escaped, sizeof(escaped));
        written += snprintf(out + written, out_size - (size_t)written, "%s\"%s\":\"%s\"",
                            (i == 0) ? "" : ",", f->name, escaped);
    }
    if (written > 0 && (size_t)written < out_size) {
        snprintf(out + written, out_size - (size_t)written, "}");
    }
    return written;
}

int passport_provision_render(passport_portal_route_t route, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return -1;
    }
    out[0] = '\0';
    switch (route) {
    case PASSPORT_PORTAL_ROUTE_ROOT:
        return render_root(out, out_size);
    case PASSPORT_PORTAL_ROUTE_STATUS:
        return render_status(out, out_size);
    case PASSPORT_PORTAL_ROUTE_CONFIG:
        return render_config(out, out_size);
    default:
        return -1;
    }
}

// --- applying a submission --------------------------------------------------

esp_err_t passport_provision_apply(const char *form)
{
    if (form == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    passport_config_t *cfg = passport_config_mutable();
    int applied = 0;

    for (int i = 0; i < passport_portal_field_count(); i++) {
        const passport_portal_field_t *f = passport_portal_field(i);
        char value[PASSPORT_CFG_STR_MAX];
        if (!passport_portal_form_get(form, f->form_key, value, sizeof(value))) {
            continue;   // not submitted
        }
        if (value[0] == '\0') {
            continue;   // blank means "leave unchanged", including for secrets
        }
        const passport_cfg_field_t *target = passport_cfg_find(f->config_field);
        if (target == NULL) {
            continue;
        }
        const passport_cfg_status_t st = passport_cfg_apply(cfg, target, value);
        if (st == PASSPORT_CFG_OK) {
            applied++;
        } else {
            ESP_LOGW(TAG, "%s 被拒绝: %s", f->form_key, passport_cfg_status_name(st));
        }
    }

    if (applied == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return passport_config_store_save();
}

// --- HTTP handlers ----------------------------------------------------------

static esp_err_t send_page(httpd_req_t *req, const char *body, const char *type, int status)
{
    if (status != 200) {
        char status_line[16];
        snprintf(status_line, sizeof(status_line), "%d", status);
        httpd_resp_set_status(req, status_line);
    }
    httpd_resp_set_type(req, type);
    // The setup page must never be cached by the phone's browser.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_page(httpd_req_t *req)
{
    const passport_portal_route_t route = passport_portal_route(req->uri);
    const int written = passport_provision_render(route, s_page, PAGE_MAX);
    if (written < 0) {
        return send_page(req, "not found", "text/plain", 404);
    }
    const char *type = (route == PASSPORT_PORTAL_ROUTE_STATUS ||
                        route == PASSPORT_PORTAL_ROUTE_CONFIG)
                           ? "application/json"
                           : "text/html";
    return send_page(req, s_page, type, 200);
}

static esp_err_t handler_save(httpd_req_t *req)
{
    char body[BODY_MAX];
    int received = 0;
    while (received < (int)sizeof(body) - 1) {
        const int got = httpd_req_recv(req, body + received, sizeof(body) - 1u - (size_t)received);
        if (got <= 0) {
            break;
        }
        received += got;
    }
    body[received] = '\0';

    const esp_err_t err = passport_provision_apply(body);
    char reply[512];
    if (err == ESP_OK) {
        snprintf(reply, sizeof(reply),
                 "<!doctype html><html lang=zh><head><meta charset=utf-8>"
                 "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
                 "<title>已保存</title></head><body style=\"font-family:system-ui,sans-serif;"
                 "background:#101828;color:#EAF0FF;padding:24px\">"
                 "<h1 style=\"font-size:20px\">已保存</h1>"
                 "<p>配置已写入设备。设备将在重启后使用新设置。</p>"
                 "<p><a style=\"color:#4D6BFE\" href=\"/\">返回</a></p></body></html>");
    } else {
        snprintf(reply, sizeof(reply),
                 "<!doctype html><meta charset=utf-8><body style=\"font-family:system-ui,sans-serif;"
                 "background:#101828;color:#EAF0FF;padding:24px\">"
                 "<h1 style=\"font-size:20px\">没有可保存的改动</h1>"
                 "<p>所有字段都是空的。请至少填写一项。</p>"
                 "<p><a style=\"color:#4D6BFE\" href=\"/\">返回</a></p></body>");
    }
    return send_page(req, reply, "text/html", 200);
}

// The catch-all that makes the portal appear by itself: whichever probe path the phone or
// laptop requests, it is answered with a redirect to the setup page.
static esp_err_t handler_redirect(httpd_req_t *req)
{
    char location[48];
    snprintf(location, sizeof(location), "http://%s/", s_address);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t register_routes(void)
{
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = handler_page },
        { .uri = "/index.html", .method = HTTP_GET, .handler = handler_page },
        { .uri = "/api/status", .method = HTTP_GET, .handler = handler_page },
        { .uri = "/api/config", .method = HTTP_GET, .handler = handler_page },
        { .uri = "/save", .method = HTTP_POST, .handler = handler_save },
        { .uri = "/*", .method = HTTP_GET, .handler = handler_redirect },
    };
    for (size_t i = 0u; i < sizeof(routes) / sizeof(routes[0]); i++) {
        const esp_err_t err = httpd_register_uri_handler(s_server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败: %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

// --- lifecycle --------------------------------------------------------------

// The access point name and password come from the MAC, so they are stable across reboots
// and unique per device without a factory partition. The password is displayed on screen;
// the point of protecting the AP is that the setup page can write credentials.
static void derive_identity(void)
{
    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        mac[4] = 0xAA;
        mac[5] = 0xBB;
    }
    snprintf(s_ssid, sizeof(s_ssid), "Passport-%02X%02X", mac[4], mac[5]);
    // Eight characters: long enough for the WPA2 minimum, short enough to type.
    snprintf(s_password, sizeof(s_password), "p%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t passport_provision_start(void)
{
    if (s_active) {
        return ESP_OK;
    }

    esp_err_t err = passport_wifi_init();   // also brings up netif and the event loop
    if (err != ESP_OK) {
        return err;
    }

    derive_identity();

    s_page = malloc(PAGE_MAX);    if (s_page == NULL) {
        ESP_LOGE(TAG, "页面缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "热点网络接口创建失败");
        free(s_page);
        s_page = NULL;
        return ESP_FAIL;
    }

    // APSTA so an already-configured device keeps answering on the station interface.
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切换到 APSTA 失败: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t ap_cfg = { 0 };

    // The AP's SSID field is a fixed 32-byte array with no terminator, so a longer name
    // would be silently truncated — the same hazard the station path had. The derived name
    // is short, but the check is what makes that a fact rather than a hope.
    const size_t name_len = strlen(s_ssid);
    if (name_len == 0u || name_len > sizeof(ap_cfg.ap.ssid)) {
        ESP_LOGE(TAG, "热点名称长度非法: %u", (unsigned)name_len);
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t pass_len = strlen(s_password);
    if (pass_len < 8u || pass_len > sizeof(ap_cfg.ap.password) - 1u) {
        ESP_LOGE(TAG, "热点密码长度非法: %u", (unsigned)pass_len);
        return ESP_ERR_INVALID_SIZE;
    }
    // The two interfaces share one radio, so the access point must sit on the station's
    // channel. Forcing a different one drops the association: observed on hardware as the
    // station going idle the moment the access point came up.
    uint8_t channel = PROV_CHANNEL;
    wifi_ap_record_t station_ap;
    if (passport_wifi_state() == PASSPORT_WIFI_CONNECTED &&
        esp_wifi_sta_get_ap_info(&station_ap) == ESP_OK && station_ap.primary != 0) {
        channel = station_ap.primary;
        ESP_LOGI(TAG, "热点跟随 station 信道 %u", (unsigned)channel);
    }

    memcpy(ap_cfg.ap.ssid, s_ssid, name_len);
    ap_cfg.ap.ssid_len = (uint8_t)name_len;
    memcpy(ap_cfg.ap.password, s_password, pass_len);
    ap_cfg.ap.channel = channel;
    ap_cfg.ap.max_connection = PROV_MAX_CLIENTS;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "热点配置失败: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(s_address, sizeof(s_address), IPSTR, IP2STR(&ip_info.ip));
    }

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 8;
    http_cfg.lru_purge_enable = true;
    http_cfg.stack_size = 8192;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;

    err = httpd_start(&s_server, &http_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败: %s", esp_err_to_name(err));
        return err;
    }
    err = register_routes();
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    s_active = true;
    ESP_LOGI(TAG, "热点已启动: ssid=%s password=%s 地址=%s", s_ssid, s_password, s_address);
    return ESP_OK;
}

esp_err_t passport_provision_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_active) {
        // Keep the station, drop only the access point.
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_active = false;
        ESP_LOGI(TAG, "热点已关闭");
    }
    return ESP_OK;
}
