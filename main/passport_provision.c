// main/passport_provision.c
#include "passport_provision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_battery.h"
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
#include "passport_identity.h"
#include "passport_portal.h"
#include "passport_wifi.h"

static const char *TAG = "passport_prov";

#define PROV_CHANNEL      6
#define PROV_MAX_CLIENTS  2
#define PAGE_MAX          4096
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
    const char *connected = (passport_wifi_state() == PASSPORT_WIFI_CONNECTED) ? "已连接" : "未连接";
    const int soc = bsp_battery_soc();
    char battery[24];
    if (soc >= 0) {
        snprintf(battery, sizeof(battery), "电量 %d%%", soc);
    } else {
        snprintf(battery, sizeof(battery), "电量未知");
    }

    int used = snprintf(out, out_size,
        "<!doctype html><html lang=zh><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>DeepSeek Passport</title><style>"
        "body{font-family:system-ui,sans-serif;margin:0;padding:18px;background:#101828;color:#EAF0FF}"
        "h1{font-size:20px;margin:0 0 4px}h2{font-size:14px;margin:22px 0 0;color:#4D6BFE}"
        "p.sub{color:#8FA3C8;margin:0 0 18px;font-size:13px}"
        "label{display:block;margin:14px 0 4px;font-size:13px;color:#8FA3C8}"
        ".hint{color:#4D6BFE}"
        "input{width:100%%;box-sizing:border-box;padding:10px;font-size:15px;border-radius:8px;"
        "border:1px solid #2A3A5C;background:#18233A;color:#EAF0FF}"
        "button{margin-top:22px;width:100%%;padding:13px;font-size:16px;border:0;border-radius:8px;"
        "background:#4D6BFE;color:#fff;font-weight:600}"
        ".state{background:#18233A;border-left:3px solid #4D6BFE;padding:9px 12px;border-radius:6px;"
        "font-size:13px;color:#8FA3C8;line-height:1.6}</style></head><body>"
        "<h1>DeepSeek Passport</h1><p class=sub>设备设置 · 保存后立即生效</p>"
        "<div class=state>Wi-Fi %s · 热点 %s · 地址 %s<br>%s · 可用内存 %u KB</div>"
        "<form method=post action=/save>",
        connected, s_ssid, s_address, battery, (unsigned)(esp_get_free_heap_size() / 1024u));

    // The form is generated from the same table that parses the submission, so a field cannot be
    // writable from the console but invisible here. Group headings come from the table too.
    const char *group = NULL;
    for (int i = 0; i < passport_portal_field_count(); i++) {
        const passport_portal_field_t *field = passport_portal_field(i);
        const passport_cfg_field_t *target = passport_cfg_find(field->config_field);
        if (target == NULL) {
            continue;
        }
        if (group == NULL || strcmp(group, field->group) != 0) {
            group = field->group;
            used += snprintf(out + used, out_size - (size_t)used, "<h2>%s</h2>", group);
        }

        char value[128];
        char escaped[192];
        passport_cfg_format(cfg, target, value, sizeof(value));
        passport_portal_escape(value, escaped, sizeof(escaped));

        if (target->secret) {
            // A stored secret is never a value in the page. What the person sees is whether one
            // exists, and leaving it blank keeps it.
            used += snprintf(out + used, out_size - (size_t)used,
                             "<label>%s <span class=hint>(%s,留空则不改动)</span></label>"
                             "<input name=%s type=password autocomplete=off>",
                             field->label, escaped, field->form_key);
        } else {
            used += snprintf(out + used, out_size - (size_t)used,
                             "<label>%s</label><input name=%s value=\"%s\" autocapitalize=off>",
                             field->label, field->form_key, escaped);
        }
        if (used < 0 || (size_t)used >= out_size) {
            ESP_LOGW(TAG, "设置页在 %s 处被截断", field->form_key);
            break;
        }
    }

    if (used > 0 && (size_t)used < out_size) {
        used += snprintf(out + used, out_size - (size_t)used,
                         "<button type=submit>保存</button></form>"
                         "<p class=sub style=\"margin-top:18px\">"
                         "<a style=\"color:#4D6BFE\" href=/api/status>状态</a> · "
                         "<a style=\"color:#4D6BFE\" href=/api/config>全部配置</a></p>"
                         "</body></html>");
    }
    return used;
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
//
// The derivation itself lives in passport_identity.c, which is pure and host-tested: these are
// credentials a person reads off a small screen and types into a phone, so the alphabet (no i, l, o, 0
// or 1) and the length are properties worth a test rather than a code review.
static void derive_identity(void)
{
    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        mac[4] = 0xAA;
        mac[5] = 0xBB;
    }
    if (!passport_identity_from_mac(mac, s_ssid, sizeof(s_ssid), s_password, sizeof(s_password))) {
        ESP_LOGE(TAG, "热点凭据推导失败:缓冲不足");
        s_ssid[0] = '\0';
        s_password[0] = '\0';
    }
}

// --- access point events ----------------------------------------------------
//
// A small ring of what the radio has seen, so that a client's failed attempt can be read afterwards
// instead of having to be watched live. Each entry carries the Wi-Fi reason code, which is the only
// thing that separates "wrong key" from "handshake timed out" from "associated and left again".
#define EVENT_RING 8
#define EVENT_TEXT 128

static char s_events[EVENT_RING][EVENT_TEXT];
static unsigned s_event_count;

static void record_event(const char *fmt, ...)
{
    // Newest first: shift down and write at the top, each entry stamped with the uptime so the order
    // is unambiguous rather than inferred from the print order.
    for (int i = EVENT_RING - 1; i > 0; i--) {
        memcpy(s_events[i], s_events[i - 1], EVENT_TEXT);
    }
    // The body is sized to leave room for the longest possible timestamp prefix, so the prefixed
    // result provably fits and the compiler can see that it does.
    char body[EVENT_TEXT - 32];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    snprintf(s_events[0], EVENT_TEXT, "[%5lld ms] %s", (long long)(esp_timer_get_time() / 1000), body);
    if (s_event_count < EVENT_RING) {
        s_event_count++;
    }
}

void passport_provision_events(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return;
    }
    out[0] = '\0';
    if (s_event_count == 0u) {
        snprintf(out, out_size, "no access point events yet\n");
        return;
    }
    size_t used = 0u;
    const int header = snprintf(out, out_size, "access point events, newest first:\n");
    if (header < 0 || (size_t)header >= out_size) {
        return;
    }
    used = (size_t)header;
    for (unsigned i = 0u; i < s_event_count; i++) {
        const int written = snprintf(out + used, out_size - used, "  %s\n", s_events[i]);
        if (written < 0 || (size_t)written >= out_size - used) {
            break;
        }
        used += (size_t)written;
    }
}

static void ap_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_AP_START:
        record_event("ap started");
        break;
    case WIFI_EVENT_AP_STOP:
        record_event("ap stopped");
        break;
    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *e = (const wifi_event_ap_staconnected_t *)data;
        record_event("client %02x:%02x:%02x:%02x:%02x:%02x joined (aid %u)",
                     e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                     (unsigned)e->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        // The reason code, spelled out for the ones that matter in practice. This is what turns "it
        // will not connect" into something actionable.
        const wifi_event_ap_stadisconnected_t *e = (const wifi_event_ap_stadisconnected_t *)data;
        const char *why = "see the Wi-Fi reason code table";
        switch (e->reason) {
        case WIFI_REASON_AUTH_EXPIRE: why = "authentication expired: the key was probably wrong, or the attempt took too long"; break;
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: why = "4-way handshake timed out: the password did not match"; break;
        case WIFI_REASON_HANDSHAKE_TIMEOUT: why = "handshake timed out"; break;
        case WIFI_REASON_ASSOC_EXPIRE: why = "association expired: the client gave up waiting"; break;
        case WIFI_REASON_ASSOC_LEAVE: why = "the client left on its own: often no internet on this network"; break;
        case WIFI_REASON_DISASSOC_PWRCAP_BAD: why = "the client's power capability was rejected"; break;
        case WIFI_REASON_NO_AP_FOUND: why = "the client could not find the network"; break;
        case WIFI_REASON_CONNECTION_FAIL: why = "the client reported a connection failure"; break;
        default: break;
        }
        record_event("client %02x:%02x:%02x:%02x:%02x:%02x left (reason %u: %s)",
                     e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                     (unsigned)e->reason, why);
        break;
    }
    default:
        break;
    }
}

esp_err_t passport_provision_start(void)
{
    if (s_active) {
        return ESP_OK;
    }
    // A trace through the start sequence. The hotspot once failed to come up with nothing in the log
    // to say where it stopped, which left "is it running?" to be guessed at from the outside; the step
    // markers make the answer a line in the console instead.
    ESP_LOGI(TAG, "配网启动:1/6 开始");

    // Watch the access point's own events, before anything raises it: WIFI_EVENT_AP_START is emitted
    // as soon as the interface is beaconing, and a handler registered after esp_wifi_set_config()
    // misses it — which is exactly what happened the first time, leaving the event log empty and the
    // question "is the hotspot really on the air?" unanswered.
    static bool watching;
    if (!watching) {
        if (esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, &ap_event_handler, NULL) == ESP_OK &&
            esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, &ap_event_handler, NULL) == ESP_OK &&
            esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &ap_event_handler, NULL) == ESP_OK &&
            esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &ap_event_handler, NULL) == ESP_OK) {
            watching = true;
        } else {
            ESP_LOGW(TAG, "热点事件监听注册失败;连接诊断将没有记录");
        }
    }

    esp_err_t err = passport_wifi_init();   // also brings up netif and the event loop
    ESP_LOGI(TAG, "配网启动:2/6 网络栈 (%s)", esp_err_to_name(err));
    if (err != ESP_OK) {
        return err;
    }

    derive_identity();

    // The page buffer and the access point's network interface are created once and kept for the
    // lifetime of the run.
    //
    // Both were created here on every start, and the interface was never destroyed on stop. Opening
    // the setup screen a second time therefore called esp_netif_create_default_wifi_ap() on an
    // interface that already existed, which does not return an error — it aborts:
    //
    //     assert failed: esp_netif_create_default_wifi_ap (config or if_key is NULL or duplicate key)
    //
    // The device rebooted, the screen had already shown the join code, and the owner was left trying
    // to connect to a hotspot that was not running. The report was "the password is wrong", which is
    // what a phone says when it cannot complete an association for any reason at all.
    if (s_page == NULL) {
        s_page = malloc(PAGE_MAX);
        if (s_page == NULL) {
            ESP_LOGE(TAG, "页面缓冲分配失败");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "热点网络接口创建失败");
            return ESP_FAIL;
        }
    }

    // APSTA so an already-configured device keeps answering on the station interface.
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    ESP_LOGI(TAG, "配网启动:3/6 模式已设为 APSTA (%s)", esp_err_to_name(err));
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
    ESP_LOGI(TAG, "配网启动:4/6 热点配置已下发 (%s)", esp_err_to_name(err));
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
    // 5120 rather than the default 8192. The handlers render a page into a heap buffer and apply a
    // form; they do not recurse and they do not hold large structures on the stack. The larger default
    // was not a decision, it was a copy — and it cost the whole web console: by the time this runs, the
    // join code's pixels are already reserved (they have to be, or they will not fit afterwards), and
    // eight kilobytes of contiguous stack was more than the remainder could give:
    //
    //     HTTP 服务启动失败: ESP_ERR_HTTPD_TASK
    //
    // which left a device showing a join code for a page that was not being served.
    http_cfg.stack_size = 5120;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;

    err = httpd_start(&s_server, &http_cfg);
    ESP_LOGI(TAG, "配网启动:5/6 HTTP 服务 (%s)", esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败: %s", esp_err_to_name(err));
        return err;
    }
    err = register_routes();
    ESP_LOGI(TAG, "配网启动:6/6 路由注册 (%s)", esp_err_to_name(err));
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    s_active = true;
    ESP_LOGI(TAG, "热点已启动: ssid=%s password=%s 地址=%s", s_ssid, s_password, s_address);
    // Recorded from this side rather than inferred: the interface exists, it has an address, and the
    // events above say whether the driver raised it.
    record_event("ap configured: ssid %s, address %s, channel %u", s_ssid, s_address,
                 (unsigned)channel);
    return ESP_OK;
}
esp_err_t passport_provision_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_active) {
        // Keep the station, drop only the access point. The interface itself stays: it is created once
        // and reused, which is what keeps a second visit to the setup screen from aborting the device.
        // The route handlers and the page buffer survive too, and are reused as they are.
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_active = false;
        ESP_LOGI(TAG, "热点已关闭");
    }
    return ESP_OK;
}
