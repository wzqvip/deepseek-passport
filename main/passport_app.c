// main/passport_app.c
//
// The application's home screen. This is a deliberate redesign: dark canvas, a
// status strip, a conversation area, the pet, and a footer hint. It does not use
// the upstream hardware-test menu, its cards, or its ui_pixel visual shell.
//
// Runtime rules honoured here:
//   * The pet animates from an LVGL timer, which may touch objects directly and
//     must never block. Battery I/O happens in the worker task instead, because
//     reading the gauge over I2C would stall the rendering task.
//   * The worker takes bsp_lvgl_lock() around every object it touches.
//   * Every element the console should be able to address carries a name derived
//     from what the user sees.
#include "passport_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "passport_audio.h"
#include "passport_chat.h"
#include "passport_config_store.h"
#include "esp_timer.h"
#include "passport_fonts.h"
#include "passport_pet.h"
#include "passport_power.h"
#include "passport_provision.h"
#include "passport_speech.h"
#include "passport_qr.h"
#include "passport_utf8.h"
#include "passport_wifi.h"
#include "passport_work.h"

static const char *TAG = "passport_app";

// Own palette. Deliberately not the demo shell's sky/grass/cloud scheme.
#define APP_BG      0x101828
#define APP_PANEL   0x18233A
#define APP_ACCENT  0x4D6BFE
#define APP_PAPER   0xEAF0FF
#define APP_MUTED   0x8FA3C8
// An unlit signal bar: visible enough to show how many there are, dark enough not to read as signal.
#define WIFI_BAR_OFF 0x24314F

#define BATTERY_POLL_MS 5000
#define REACT_MS        1400
// The bubble shows roughly four wrapped lines at this font and width. The limit is in
// bytes but is always applied on a character boundary: cutting a Chinese answer by byte
// count alone would split a character, and the invalid tail renders as nothing at all.
#define BUBBLE_MAX      100

// What pressing OK asks. Chinese now that the display can actually render it: the
// default prompt is the product's own language, and a Chinese answer is also the honest
// test of the font, since an English-only sample would pass with no CJK glyphs at all.
#define DEFAULT_PROMPT  "用一句话跟我打个招呼"

// One line the pet says for each mood. Short, because the bubble is small.
static const char *const MOOD_LINES[PASSPORT_PET_MOOD_COUNT] = {
    [PASSPORT_PET_IDLE] = "你好,按 OK 开始",
    [PASSPORT_PET_CURIOUS] = "刚刚是什么?",
    [PASSPORT_PET_LISTENING] = "我在听。",
    [PASSPORT_PET_THINKING] = "让我想想...",
    [PASSPORT_PET_WORKING] = "正在处理。",
    [PASSPORT_PET_SPEAKING] = "我找到了这个。",
    [PASSPORT_PET_HAPPY] = "好了!",
    [PASSPORT_PET_SLEEPING] = "Zzz...",
    [PASSPORT_PET_DISTRESSED] = "出了点问题。",
};

static lv_obj_t *s_screen;
static lv_obj_t *s_battery;
static lv_obj_t *s_wifi;
static lv_obj_t *s_wifi_bars[3];

// The hint line, and the menu.
#define HINT_DEFAULT "按住OK说话   长按DOWN工作\n长按UP设置   双击UP待机"

// A small settings menu, entered by holding UP and left by holding OK.
//
// The owner asked for this shape in as many words: holding a button should *enter* something and stay
// there, not display something for as long as the finger is down. A menu also gives the network details a
// home: they were previously only visible during a question that expired after twenty seconds, which is
// no way to read an address.
#define SETTINGS_ITEMS 4
static const char *const SETTINGS_LABELS[SETTINGS_ITEMS] = {
    "网络与配网", "屏幕亮度", "试听语音", "待机休眠"
};

// What the menu asked for, done by the worker: the button task must not start a hotspot, write flash, or
// spend seconds talking to a bridge.
typedef enum {
    SETTINGS_ACT_NONE = 0,
    SETTINGS_ACT_SETUP,
    SETTINGS_ACT_BRIGHTNESS,
    SETTINGS_ACT_VOICE_TEST,
    SETTINGS_ACT_STANDBY,
} settings_act_t;

static volatile settings_act_t s_settings_act;
static bool s_settings_requested;
static bool s_on_settings;
static lv_obj_t *s_settings;
static lv_obj_t *s_settings_info;
static lv_obj_t *s_settings_items[SETTINGS_ITEMS];
static int s_settings_index;
static lv_obj_t *s_bubble;
static lv_obj_t *s_mood;
static lv_obj_t *s_hint;
static TaskHandle_t s_task;
static volatile bool s_running;
static bool s_sleeping;

// The setup screen. It exists because the hotspot credentials were otherwise only
// readable over the serial console, which defeats the point of provisioning without a
// cable: the person setting the device up is holding a phone, not a terminal.
static lv_obj_t *s_setup;
static lv_obj_t *s_setup_hotspot;
static lv_obj_t *s_setup_password;
static lv_obj_t *s_setup_address;
static bool s_on_setup;
static bool s_on_work;
// Set by the button task, acted on by the app worker: see the long-press-of-UP branch.
static volatile bool s_setup_requested;

// --- small LVGL helpers -----------------------------------------------------

static lv_obj_t *plain(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, const lv_font_t *font,
                       uint32_t color, const char *name)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_label_set_text(lbl, "");
    if (name != NULL) {
        lv_obj_set_name(lbl, name);
    }
    return lbl;
}

// --- UI ---------------------------------------------------------------------

static void set_bubble(const char *text)
{
    if (s_bubble != NULL) {
        lv_label_set_text(s_bubble, text);
    }
}

static void set_mood(passport_pet_mood_t mood, uint32_t hold_ms)
{
    passport_pet_set_mood(mood, hold_ms);
    lv_label_set_text_fmt(s_mood, "%s", passport_pet_mood_name(mood));
    set_bubble(MOOD_LINES[mood]);
}

static void build_home(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(APP_BG), 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_name(s_screen, "passport");

    // Status strip. The battery sits top-right, per the repository convention,
    // and shows a placeholder rather than a number when the gauge is absent.
    lv_obj_t *title = label(s_screen, 8, 6, &lv_font_montserrat_14, APP_MUTED, "title");
    lv_label_set_text(title, "PASSPORT");

    // The top-right corner answers the two questions a glance at a pocket device should answer: is it
    // on the network, and how much battery is left.
    //
    // The Wi-Fi part is a word and three bars rather than a word alone, because "connected" and
    // "connected but barely" are different situations: the bars are lit from the associated signal,
    // and none are lit when there is no association at all.
    s_wifi = label(s_screen, 140, 6, &lv_font_montserrat_14, APP_MUTED, "wifi");
    lv_obj_set_width(s_wifi, 34);
    lv_label_set_text(s_wifi, "--");
    for (int i = 0; i < 3; i++) {
        static const int heights[3] = { 5, 8, 11 };
        char name[16];
        snprintf(name, sizeof(name), "wifi_bar%d", i);
        s_wifi_bars[i] = plain(s_screen, 178 + 5 * i, 19 - heights[i], 3, heights[i], WIFI_BAR_OFF);
        lv_obj_set_name(s_wifi_bars[i], name);
    }

    s_battery = label(s_screen, 196, 6, &lv_font_montserrat_14, APP_PAPER, "battery");
    lv_obj_set_width(s_battery, 36);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_battery, "--");
    plain(s_screen, 0, 26, 240, 2, APP_ACCENT);

    // Conversation area: a panel with an accent edge, not a demo card. Named so the
    // answer inside it is addressable from the console as "chat/bubble".
    lv_obj_t *panel = plain(s_screen, 12, 34, 216, 90, APP_PANEL);
    lv_obj_set_name(panel, "chat");
    lv_obj_set_style_radius(panel, 6, 0);
    plain(panel, 0, 8, 3, 74, APP_ACCENT);
    s_bubble = label(panel, 12, 10, passport_font_body(), APP_PAPER, "bubble");
    lv_obj_set_width(s_bubble, 190);
    lv_obj_set_height(s_bubble, 68);   // clip rather than spill out of the panel
    lv_label_set_long_mode(s_bubble, LV_LABEL_LONG_WRAP);

    // The pet, then the line that names its current expression. The mood label is
    // what makes the screen readable from the serial console.
    passport_pet_create(s_screen, 88, 136, "pet");
    s_mood = label(s_screen, 0, 204, &lv_font_montserrat_14, APP_MUTED, "mood");
    lv_obj_set_width(s_mood, 240);
    lv_obj_set_style_text_align(s_mood, LV_TEXT_ALIGN_CENTER, 0);

    plain(s_screen, 0, 232, 240, 3, APP_ACCENT);

    // Every label that carries Chinese uses passport_font_body(). The default font has no CJK glyphs
    // at all, and text set with it renders as empty boxes — a defect a build cannot see and a
    // snapshot would only reveal if someone looked. `ui audit` now checks this on the device, and it
    // is what found these: the hint line and two setup labels were drawn with the Latin font.
    s_hint = label(s_screen, 6, 240, passport_font_body(), APP_MUTED, "hint");
    lv_obj_set_width(s_hint, 228);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_hint, HINT_DEFAULT);

    set_mood(PASSPORT_PET_IDLE, 0);
    lv_screen_load(s_screen);
}

// --- setup screen -----------------------------------------------------------

// The code carries the hotspot's own credentials, so joining needs no typing. Size is
// chosen for a phone camera: the panel is 240 pixels wide, and three pixels per module
// plus the four-module quiet zone on each side is about as large as this screen allows.
#define QR_TARGET_SIDE 128
// Two modules of quiet zone rather than the specification's four. Every reader in practice needs at
// least two, the panel is 240 pixels wide, and four of them on each side is another 5 KB of pixel buffer
// on a device where the web console and the code compete for the same memory. The value is reported to
// the console by `prov pixels`, so a check of the drawn symbol knows what to expect.
#define QR_QUIET_ZONE  2

static lv_image_dsc_t s_qr_dsc;
static uint16_t *s_qr_pixels;
static int s_qr_side;
// The geometry the last render used, so the console can sample the drawn pixels by module.
static int s_qr_scale;
static int s_qr_modules;

// Build the standard Wi-Fi join payload. The escaping rules for ';', ',' ':' and '\' are
// not applied: the access point name and password are derived from the MAC, so they never
// contain those characters. If they ever do, this is the line that must learn to escape.
static void qr_payload(char *out, size_t out_size)
{
    snprintf(out, out_size, "WIFI:T:WPA;S:%s;P:%s;;", passport_provision_ssid(),
             passport_provision_password());
}

// Render the code into a pixel buffer. Returns the side length, or 0 on failure.
static int qr_render(void)
{
    char payload[192];
    qr_payload(payload, sizeof(payload));

    passport_qr_t code;
    if (!passport_qr_encode(payload, &code)) {
        ESP_LOGW(TAG, "二维码载荷过长,未生成");
        return 0;
    }

    const int modules = (int)code.size + 2 * QR_QUIET_ZONE;
    int scale = QR_TARGET_SIDE / modules;
    if (scale < 2) {
        scale = 2;
    }
    const int side = modules * scale;

    // The buffer comes from the system heap. A canvas of this size would have to fit
    // LVGL's own pool, which is an order of magnitude smaller.
    if (s_qr_pixels == NULL || s_qr_side != side) {
        heap_caps_free(s_qr_pixels);
        s_qr_pixels = heap_caps_malloc((size_t)side * (size_t)side * sizeof(uint16_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s_qr_side = side;
    }
    if (s_qr_pixels == NULL) {
        ESP_LOGE(TAG, "二维码像素缓冲分配失败(%d 字节)", side * side * 2);
        return 0;
    }

    const uint16_t dark = lv_color_to_u16(lv_color_hex(0x000000));
    const uint16_t light = lv_color_to_u16(lv_color_hex(0xFFFFFF));
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            const int mx = x / scale - QR_QUIET_ZONE;
            const int my = y / scale - QR_QUIET_ZONE;
            bool is_dark = false;
            if (mx >= 0 && my >= 0 && mx < (int)code.size && my < (int)code.size) {
                is_dark = (code.modules[my][mx] != 0u);
            }
            s_qr_pixels[(size_t)y * (size_t)side + (size_t)x] = is_dark ? dark : light;
        }
    }

    s_qr_scale = scale;
    s_qr_modules = modules;

    s_qr_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_qr_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_qr_dsc.header.w = (uint32_t)side;
    s_qr_dsc.header.h = (uint32_t)side;
    s_qr_dsc.header.stride = (uint32_t)(side * 2);
    s_qr_dsc.data_size = (uint32_t)(side * side * 2);
    s_qr_dsc.data = (const uint8_t *)s_qr_pixels;

    ESP_LOGI(TAG, "二维码已生成:版本 %u,%d 模块,缩放 %d,%d 像素,载荷 %u 字节", code.version,
             (int)code.size, scale, side, (unsigned)strlen(payload));
    return side;
}

// Reserve the code's pixel buffer now, at start-up, rather than when the setup screen is first
// built. Allocating it late split whatever large block remained and left only about 22 KB
// contiguous, which is not enough for a snapshot of the same code — the verification tool was
// starved by the thing it was trying to verify. Reserving first also matches the rule this
// project follows elsewhere: allocate a hot buffer once.
// The QR pixels are allocated when the setup screen opens and released when it closes.
//
// They used to be reserved once at boot, so that the verification could not be starved by the thing
// it was trying to verify. That reasoning stopped paying for itself: the buffer is tens of kilobytes
// — 32,768 of them for a 128-pixel code — and holding it for the life of the device left too little
// contiguous heap for anything else. It was measured, not guessed: with the buffer held, the largest
// free block at rest was 7,680 bytes, and `llm ask` failed with ESP_ERR_HTTP_CONNECT because a TLS
// session could no longer be allocated. Provisioning and provider calls are never needed at the same
// moment, so the pixels only need to exist while the screen that shows them does.
static void qr_release(void)
{
    if (s_qr_pixels != NULL) {
        heap_caps_free(s_qr_pixels);
        s_qr_pixels = NULL;
        s_qr_side = 0;
    }
}

// Take the pixels for the join code *before* the access point starts.
//
// This is the reservation, back again, but only for the few milliseconds it is needed rather than for
// the life of the device. Its original reasoning was right — the code cannot be retried if the
// allocation fails, and the radio fragments what is left — and it was only the *duration* that cost
// too much: held for ever, these tens of kilobytes left no room for a TLS session, which is how the
// cost was found. Allocating it at the moment the setup screen is requested keeps both properties.
static void qr_reserve(void)
{
    char payload[192];
    qr_payload(payload, sizeof(payload));

    passport_qr_t code;
    if (!passport_qr_encode(payload, &code)) {
        return;
    }
    const int modules = (int)code.size + 2 * QR_QUIET_ZONE;
    int scale = QR_TARGET_SIDE / modules;
    if (scale < 2) {
        scale = 2;
    }
    const int side = modules * scale;
    if (s_qr_pixels != NULL && s_qr_side == side) {
        return;   // already the right size
    }
    qr_release();
    s_qr_pixels = heap_caps_malloc((size_t)side * (size_t)side * sizeof(uint16_t),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_qr_side = (s_qr_pixels != NULL) ? side : 0;
    if (s_qr_pixels != NULL) {
        ESP_LOGI(TAG, "二维码缓冲已就绪:%d 字节(%d 像素)", side * side * 2, side);
    } else {
        ESP_LOGE(TAG, "二维码缓冲分配失败(%d 字节)", side * side * 2);
    }
}

bool passport_app_qr_drawn(const uint16_t **pixels, int *side, int *scale, int *modules,
                           int *quiet_zone)
{
    if (s_qr_pixels == NULL || s_qr_side <= 0) {
        return false;
    }
    if (pixels != NULL) {
        *pixels = s_qr_pixels;
    }
    if (side != NULL) {
        *side = s_qr_side;
    }
    if (scale != NULL) {
        *scale = s_qr_scale;
    }
    if (modules != NULL) {
        *modules = s_qr_modules;
    }
    // The quiet zone is reported rather than duplicated: this project has twice been misled by a
    // checker that kept its own copy of a constant, and the second time the copy was wrong.
    if (quiet_zone != NULL) {
        *quiet_zone = QR_QUIET_ZONE;
    }
    return true;
}

static void setup_refresh(void)
{
    if (s_setup_hotspot == NULL) {
        return;
    }
    lv_label_set_text_fmt(s_setup_hotspot, "热点 %s", passport_provision_ssid());
    lv_label_set_text_fmt(s_setup_password, "密码 %s", passport_provision_password());
    lv_label_set_text_fmt(s_setup_address, "打开 http://%s/", passport_provision_address());
}

static void build_setup(void)
{
    s_setup = lv_obj_create(NULL);
    lv_obj_remove_flag(s_setup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_setup, lv_color_hex(APP_BG), 0);
    lv_obj_set_style_border_width(s_setup, 0, 0);
    lv_obj_set_style_pad_all(s_setup, 0, 0);
    lv_obj_set_name(s_setup, "setup");

    lv_obj_t *title = label(s_setup, 10, 6, passport_font_body(), APP_PAPER, "setup_title");
    lv_label_set_text(title, "配网设置");
    lv_label_set_text(label(s_setup, 10, 26, passport_font_body(), APP_MUTED, "setup_step"),
                      "手机扫码,或连接下面的热点");
    plain(s_setup, 0, 44, 240, 2, APP_ACCENT);

    const int side = qr_render();
    int y = 52;
    if (side > 0) {
        // The code lives in a container with a white background for two reasons: the quiet
        // zone has to be light for a reader to find the symbol, and a snapshot of a bare
        // image widget has no background to compare ink against.
        lv_obj_t *qr_panel = plain(s_setup, (240 - side) / 2, y, side, side, 0xFFFFFF);
        lv_obj_set_name(qr_panel, "qr");
        lv_obj_t *img = lv_image_create(qr_panel);
        lv_image_set_src(img, &s_qr_dsc);
        lv_obj_set_pos(img, 0, 0);
        lv_obj_set_name(img, "qr_code");
        y += side + 8;
    } else {
        lv_label_set_text(label(s_setup, 10, y, passport_font_body(), APP_MUTED, "qr_missing"),
                          "二维码生成失败");
        y += 32;
    }

    s_setup_hotspot = label(s_setup, 10, y, passport_font_body(), APP_ACCENT, "hotspot");
    s_setup_password = label(s_setup, 10, y + 22, passport_font_body(), APP_PAPER, "password");
    s_setup_address = label(s_setup, 10, y + 44, passport_font_body(), APP_MUTED, "address");

    plain(s_setup, 0, 232, 240, 3, APP_ACCENT);
    lv_label_set_text(label(s_setup, 10, 244, passport_font_body(), APP_MUTED, "setup_back"),
                      "OK: 返回");

    setup_refresh();
}

void passport_app_show_setup(void)
{
    if (!bsp_lvgl_lock(1000)) {
        return;
    }
    if (s_setup == NULL) {
        build_setup();
    }
    setup_refresh();
    lv_screen_load(s_setup);
    s_on_setup = true;
    bsp_lvgl_unlock();
}

void passport_app_show_home(void)
{
    if (s_screen == NULL) {
        return;
    }
    if (!bsp_lvgl_lock(1000)) {
        return;
    }
    lv_screen_load(s_screen);
    s_on_setup = false;
    s_on_work = false;
    s_on_settings = false;
    bsp_lvgl_unlock();

    // Leaving the setup screen releases the join code's pixels: see qr_release() for why they are
    // not held for the life of the device.
    qr_release();
}

// --- work screen ------------------------------------------------------------

static lv_obj_t *s_work;
static lv_obj_t *s_work_task;
static lv_obj_t *s_work_state;
static lv_obj_t *s_work_bar;
static lv_obj_t *s_work_percent;
static lv_obj_t *s_work_question;
static lv_obj_t *s_work_options[PASSPORT_WORK_MAX_OPTIONS];
static passport_work_view_t s_work_view;

static void build_work(void)
{
    s_work = lv_obj_create(NULL);
    lv_obj_remove_flag(s_work, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_work, lv_color_hex(APP_BG), 0);
    lv_obj_set_style_border_width(s_work, 0, 0);
    lv_obj_set_style_pad_all(s_work, 0, 0);
    lv_obj_set_name(s_work, "work");

    lv_label_set_text(label(s_work, 10, 6, passport_font_body(), APP_PAPER, "work_title"),
                      "工作模式");
    s_work_state = label(s_work, 130, 8, &lv_font_montserrat_14, APP_ACCENT, "work_state");
    lv_obj_set_width(s_work_state, 100);
    lv_obj_set_style_text_align(s_work_state, LV_TEXT_ALIGN_RIGHT, 0);
    plain(s_work, 0, 28, 240, 2, APP_ACCENT);

    s_work_task = label(s_work, 10, 36, passport_font_body(), APP_PAPER, "work_task");
    lv_obj_set_width(s_work_task, 220);
    lv_obj_set_height(s_work_task, 44);
    lv_label_set_long_mode(s_work_task, LV_LABEL_LONG_WRAP);

    // The bar and its number: the number is separate so the console can read the percentage
    // without interpreting a widget's value.
    s_work_bar = lv_bar_create(s_work);
    lv_obj_set_size(s_work_bar, 180, 12);
    lv_obj_set_pos(s_work_bar, 10, 86);
    lv_bar_set_range(s_work_bar, 0, 100);
    lv_obj_set_style_bg_color(s_work_bar, lv_color_hex(APP_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_work_bar, lv_color_hex(APP_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_name(s_work_bar, "work_bar");
    s_work_percent = label(s_work, 196, 84, &lv_font_montserrat_14, APP_PAPER, "work_percent");

    s_work_question = label(s_work, 10, 108, passport_font_body(), APP_PAPER, "work_question");
    lv_obj_set_width(s_work_question, 220);
    lv_obj_set_height(s_work_question, 44);
    lv_label_set_long_mode(s_work_question, LV_LABEL_LONG_WRAP);

    // Four option rows. The selected one is drawn in the accent colour with a marker, which is
    // also how the console can tell which one is chosen. Every object on this screen carries the
    // "work_" prefix so its console name is unique across screens: two screens sharing a name
    // like "title" would mean the same path resolves to different objects depending on which
    // screen happens to be in front.
    for (int i = 0; i < PASSPORT_WORK_MAX_OPTIONS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "work_option%d", i);
        s_work_options[i] = label(s_work, 10, 158 + 26 * i, passport_font_body(), APP_MUTED, name);
        lv_obj_set_width(s_work_options[i], 220);
    }

    plain(s_work, 0, 268, 240, 2, APP_ACCENT);
    lv_label_set_text(label(s_work, 6, 272, passport_font_body(), APP_MUTED, "work_hint"),
                      "UP/DOWN 选择  OK 确认  长按OK 返回");
}

// Draw the current view. Called with the LVGL lock held.
static void work_refresh(void)
{
    if (s_work_task == NULL) {
        return;
    }
    lv_label_set_text(s_work_task, (s_work_view.task[0] != '\0') ? s_work_view.task : "-");
    lv_label_set_text(s_work_state, passport_work_state_name(s_work_view.state));

    if (s_work_view.progress < 0) {
        // "Unknown" is drawn as absent, not as an empty bar, so a task whose bridge does not
        // report progress does not look stalled.
        lv_obj_add_flag(s_work_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_work_percent, "--");
    } else {
        lv_obj_remove_flag(s_work_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_work_bar, s_work_view.progress, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_work_percent, "%d%%", s_work_view.progress);
    }

    lv_label_set_text(s_work_question, (s_work_view.question[0] != '\0') ? s_work_view.question : "");
    lv_obj_add_flag(s_work_question, (s_work_view.question[0] != '\0') ? 0 : LV_OBJ_FLAG_HIDDEN);
    if (s_work_view.question[0] != '\0') {
        lv_obj_remove_flag(s_work_question, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < PASSPORT_WORK_MAX_OPTIONS; i++) {
        if (i >= s_work_view.option_count) {
            lv_obj_add_flag(s_work_options[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_work_options[i], LV_OBJ_FLAG_HIDDEN);
        const bool chosen = (i == s_work_view.selected);
        lv_label_set_text_fmt(s_work_options[i], "%s %s", chosen ? ">" : " ",
                              s_work_view.options[i]);
        lv_obj_set_style_text_color(s_work_options[i],
                                    lv_color_hex(chosen ? APP_ACCENT : APP_MUTED), 0);
    }
}

void passport_app_show_work(void)
{
    if (!bsp_lvgl_lock(1000)) {
        return;
    }
    if (s_work == NULL) {
        build_work();
    }
    work_refresh();
    lv_screen_load(s_work);
    s_on_work = true;
    bsp_lvgl_unlock();
}

// --- the settings menu ------------------------------------------------------
//
// One screen, four items, the same buttons as the work screen: UP and DOWN move, OK chooses, a long OK
// comes back. It exists because the alternative — details that are visible only while a button is held,
// and a question that expires — is not a thing a person can use.

static void build_settings(void)
{
    s_settings = lv_obj_create(NULL);
    lv_obj_remove_flag(s_settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_settings, lv_color_hex(APP_BG), 0);
    lv_obj_set_style_border_width(s_settings, 0, 0);
    lv_obj_set_style_pad_all(s_settings, 0, 0);
    lv_obj_set_name(s_settings, "settings");

    lv_label_set_text(label(s_settings, 10, 6, passport_font_body(), APP_PAPER, "settings_title"), "设置");
    plain(s_settings, 0, 28, 240, 2, APP_ACCENT);

    // The network details live here, permanently, instead of appearing only inside a question that
    // expires. This is the answer to "what address is it on?" and it is readable at leisure.
    s_settings_info = label(s_settings, 10, 34, passport_font_body(), APP_MUTED, "settings_info");
    lv_obj_set_width(s_settings_info, 222);
    lv_obj_set_height(s_settings_info, 40);
    lv_label_set_long_mode(s_settings_info, LV_LABEL_LONG_WRAP);

    for (int i = 0; i < SETTINGS_ITEMS; i++) {
        char name[24];
        snprintf(name, sizeof(name), "settings_item%d", i);
        s_settings_items[i] = label(s_settings, 10, 86 + i * 34, passport_font_body(), APP_PAPER, name);
        lv_obj_set_width(s_settings_items[i], 222);
    }

    lv_label_set_text(label(s_settings, 6, 268, passport_font_body(), APP_MUTED, "settings_hint"),
                      "UP/DOWN 选择   OK 进入\n长按OK 返回宠物");
}

// Called with the LVGL lock held.
static void settings_refresh(void)
{
    if (s_settings_info != NULL) {
        const passport_config_t *cfg = passport_config_current();
        char ip[20];
        passport_wifi_ip(ip, sizeof(ip));
        char line[BUBBLE_MAX];
        if (ip[0] != '\0') {
            snprintf(line, sizeof(line), "网络 %.18s · 地址 %.15s\n网页 http://%.15s/", cfg->wifi_ssid, ip, ip);
        } else {
            snprintf(line, sizeof(line), "未联网。进入\"网络与配网\"\n用手机连上它的热点");
        }
        lv_label_set_text(s_settings_info, line);
    }
    for (int i = 0; i < SETTINGS_ITEMS; i++) {
        if (s_settings_items[i] == NULL) {
            continue;
        }
        const bool selected = (i == s_settings_index);
        // The selected row is marked by colour and by a caret, because colour alone is not a selection
        // on a 240-pixel panel held at arm's length in daylight.
        char text[64];
        snprintf(text, sizeof(text), "%s%s", selected ? "> " : "   ", SETTINGS_LABELS[i]);
        lv_label_set_text(s_settings_items[i], text);
        lv_obj_set_style_text_color(s_settings_items[i], lv_color_hex(selected ? APP_ACCENT : APP_PAPER), 0);
    }
}

void passport_app_show_settings(void)
{
    if (!bsp_lvgl_lock(1000)) {
        return;
    }
    if (s_settings == NULL) {
        build_settings();
    }
    settings_refresh();
    lv_screen_load(s_settings);
    s_on_settings = true;
    bsp_lvgl_unlock();
}

// For the worker, which holds no lock when it acts on what the menu asked for.
static void settings_refresh_locked(void)
{
    if (bsp_lvgl_lock(500)) {
        settings_refresh();
        bsp_lvgl_unlock();
    }
}

bool passport_app_work_feed(const char *json)
{
    passport_work_view_t parsed;
    if (!passport_work_parse(json, &parsed)) {
        return false;
    }

    if (!bsp_lvgl_lock(1000)) {
        return false;
    }

    // Does this document say anything the owner has not already been shown? The screen is taken over
    // only when it does.
    //
    // The first version took it over on every document, which meant a bridge reporting the same task
    // every five seconds dragged the interface back to the work screen for ever: pressing OK to go home
    // lasted until the next poll. A status that has not changed is not news, and the screen is
    // refreshed below either way, so looking at it always shows the latest.
    const bool changed = (strcmp(parsed.task, s_work_view.task) != 0) ||
                         (parsed.state != s_work_view.state) ||
                         (strcmp(parsed.question, s_work_view.question) != 0) ||
                         (parsed.option_count != s_work_view.option_count);

    // And "nothing is running" is not news either, whatever it says about the task.
    //
    // This is the second half of the same lesson, learned from the owner: a bridge that answers every
    // poll with an idle document — "waiting for a task", state idle — took the screen over once and left
    // it there, and on the work screen OK answers an option instead of starting a conversation. The
    // owner held the talk button and nothing happened, because the device was showing a screen that has
    // no talk button. A notification has to be about something happening: an idle report is not a
    // reason to leave the screen where talking works.
    const bool worth_showing = (parsed.state == PASSPORT_WORK_RUNNING) ||
                               (parsed.state == PASSPORT_WORK_WAITING) ||
                               (parsed.state == PASSPORT_WORK_DONE) ||
                               (parsed.state == PASSPORT_WORK_FAILED);

    s_work_view = parsed;
    if (s_work == NULL) {
        build_work();
    }
    // A document that arrives while the home screen is showing takes it over, the way a notification
    // would. It does not interrupt the setup screen: someone mid-provisioning should not be thrown out
    // of it by a background update.
    if (!s_on_setup && changed && worth_showing) {
        lv_screen_load(s_work);
        s_on_work = true;
    }
    work_refresh();
    bsp_lvgl_unlock();
    return true;
}

passport_work_action_t passport_app_work_press(passport_work_key_t key)
{
    const passport_work_action_t action = passport_work_key(&s_work_view, key);
    if (action == PASSPORT_WORK_ACT_REDRAW) {
        if (bsp_lvgl_lock(500)) {
            work_refresh();
            bsp_lvgl_unlock();
        }
    }
    return action;
}

const passport_work_view_t *passport_app_work_current(void)
{
    return &s_work_view;
}

// --- worker -----------------------------------------------------------------

static void battery_refresh(void)
{
    const int soc = bsp_battery_soc();
    if (!bsp_lvgl_lock(200)) {
        return;
    }
    if (s_battery != NULL) {
        if (soc < 0) {
            lv_label_set_text(s_battery, "--");
        } else {
            lv_label_set_text_fmt(s_battery, "%d%%", soc);
        }
    }
    bsp_lvgl_unlock();
}

// The top-right corner's other half: whether this device is on a network, and how well.
static void wifi_refresh(void)
{
    if (!bsp_lvgl_lock(200)) {
        return;
    }
    const passport_wifi_state_t state = passport_wifi_state();
    const int rssi = passport_wifi_rssi();
    const bool online = (state == PASSPORT_WIFI_CONNECTED);

    if (s_wifi != NULL) {
        lv_label_set_text(s_wifi, online ? "WiFi" : (state == PASSPORT_WIFI_CONNECTING) ? "..." : "--");
        lv_obj_set_style_text_color(s_wifi, lv_color_hex(online ? APP_ACCENT : APP_MUTED), 0);
    }

    // Three bars are enough to tell "next to the router" from "the far end of the flat", which is what
    // this is for. The thresholds are the usual ones for consumer Wi-Fi.
    int lit = 0;
    if (online) {
        lit = (rssi >= -67) ? 3 : (rssi >= -80) ? 2 : 1;
    }
    for (int i = 0; i < 3; i++) {
        if (s_wifi_bars[i] == NULL) {
            continue;
        }
        lv_obj_set_style_bg_color(s_wifi_bars[i], lv_color_hex(i < lit ? APP_ACCENT : WIFI_BAR_OFF), 0);
    }
    bsp_lvgl_unlock();
}

static void app_task(void *arg)
{
    (void)arg;

    // No QR reservation here any more. The pixels are allocated when the setup screen opens, which
    // is a few milliseconds before the code is drawn, and released when it closes. Holding tens of
    // kilobytes for the life of the device to make that allocation certain cost more than the
    // uncertainty did: it is what stopped a TLS session from being allocated at all.

    // Join the network once at start. A configured device should come up online without
    // anyone pressing anything, and this task is allowed to block while it does — the
    // station only retried on a disconnect event before, so a reboot left it offline until
    // a person asked.
    if (passport_config_current()->wifi_ssid[0] != '\0') {
        const esp_err_t err = passport_wifi_connect(20000u);
        ESP_LOGI(TAG, "开机自动连接: %s", esp_err_to_name(err));
    }

    while (s_running) {
        battery_refresh();
        // Between polls the task sleeps, so the gauge read never competes with
        // rendering and the pet timer keeps its cadence. The standby check rides along here:
        // turning the screen off needs no task of its own.
        for (int waited = 0; s_running && waited < BATTERY_POLL_MS; waited += 100) {
            passport_power_tick();
            // The network indicator is refreshed about once a second: association and signal change on
            // their own schedule, not on the battery's, and the corner would otherwise show a stale
            // "connecting" for up to five seconds after the link came up.
            if (waited % 1000 == 0) {
                wifi_refresh();
            }
            // The settings menu: built here, and everything it asks for done here, because none of it
            // belongs on the button task — it encodes a join code, writes flash, or spends seconds
            // talking to a bridge.
            if (s_settings_requested) {
                s_settings_requested = false;
                passport_app_show_settings();
            }
            const settings_act_t act = s_settings_act;
            if (act != SETTINGS_ACT_NONE) {
                s_settings_act = SETTINGS_ACT_NONE;
                switch (act) {
                case SETTINGS_ACT_SETUP:
                    qr_reserve();
                    if (!passport_provision_active()) {
                        passport_provision_start();
                    }
                    passport_app_show_setup();
                    break;
                case SETTINGS_ACT_BRIGHTNESS: {
                    // Three steps round: dim, normal, bright. Written through and saved, so the choice
                    // survives a reboot, and applied immediately because that is the point of the item.
                    const uint8_t now = passport_config_current()->brightness;
                    const uint8_t next = (now >= 90u) ? 40u : (uint8_t)(now + 30u);
                    passport_config_mutable()->brightness = next;
                    passport_config_store_save();
                    bsp_display_backlight(next);
                    settings_refresh_locked();
                    ESP_LOGI(TAG, "亮度 %u%%", (unsigned)next);
                    break;
                }
                case SETTINGS_ACT_VOICE_TEST:
                    // The whole voice path in one press: the bridge's synthesis, the device's speaker.
                    // This exists because "is the voice side working?" should not require holding a
                    // button and talking to a machine that may be listening to a quiet room.
                    passport_app_show_status("试听语音…", PASSPORT_PET_THINKING);
                    if (passport_speech_speak("你好，我是 DeepSeek 随身宠物。") != ESP_OK) {
                        passport_app_show_error("网关没回应,检查中继和 bridge_url");
                    }
                    break;
                case SETTINGS_ACT_STANDBY:
                    passport_app_show_status("待机…", PASSPORT_PET_SLEEPING);
                    vTaskDelay(pdMS_TO_TICKS(500));   // let the screen and the pet settle before dark
                    // No timer: any key wakes it, and a device in a pocket should not wake itself.
                    passport_power_sleep(0u);
                    break;
                default:
                    break;
                }
            }
            // Requests from the button task that are too heavy for it: building the setup screen
            // means encoding the join code. See the long-press-of-UP branch in passport_app_key().
            if (s_setup_requested) {
                s_setup_requested = false;
                // Everything heavy about showing the setup screen happens here, on the one task big
                // enough for it: encoding the join code needs about six kilobytes, and starting the
                // access point is not small either. The button task asks; this task works.
                //
                // The pixels are taken before the access point starts, because the radio costs tens
                // of kilobytes and fragments what is left. Order, not luck: the allocation that
                // cannot be retried goes before the one that can.
                qr_reserve();
                if (!passport_provision_active()) {
                    passport_provision_start();
                }
                passport_app_show_setup();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

// --- public API -------------------------------------------------------------

esp_err_t passport_app_start(void)
{
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "无法获取 LVGL 锁,应用未启动");
        return ESP_ERR_TIMEOUT;
    }
    // Fonts first: every label created below selects one, and the conversation text
    // must already point at the CJK-capable face.
    passport_fonts_init();
    build_home();
    bsp_lvgl_unlock();

    s_running = true;
    // 12288 rather than 4608: this task reserves the QR pixel buffer, and reserving it encodes
    // a symbol, which needs about six kilobytes on top of the rest.
    if (xTaskCreate(app_task, "passport_app", 10240, NULL, 4, &s_task) != pdPASS) {
        s_running = false;
        s_task = NULL;
        ESP_LOGE(TAG, "应用工作线程创建失败;界面仍可用,但电量不刷新");
        return ESP_ERR_NO_MEM;
    }

    // Open the codec and discard its settling window now, so the first voice turn
    // does not mistake a boot transient for loud input. A failure is not fatal: the
    // device simply keeps whatever audio behaviour it had.
    if (passport_audio_warmup() != ESP_OK) {
        ESP_LOGW(TAG, "音频预热失败;首次录音可能包含编解码器起振瞬态");
    } else {
        ESP_LOGI(TAG, "音频通路已预热(16kHz/16bit/单声道)");
    }
    // The QR buffer is reserved by the worker task, not here: reserving it means encoding a
    // symbol, and this function runs on the main task, whose stack cannot hold the encoder.
    // Calling it here rebooted the device in a loop.

    // A device with no network configured has nothing to say and no way to be set up
    // without a cable, so it raises its own access point and says so on the panel.
    if (passport_config_current()->wifi_ssid[0] == '\0') {
        if (passport_provision_start() == ESP_OK) {
            ESP_LOGI(TAG, "未配置 Wi-Fi;已启动配网热点并显示设置屏");
            passport_app_show_setup();
        } else {
            ESP_LOGW(TAG, "未配置 Wi-Fi,但配网热点启动失败");
        }
    }

    ESP_LOGI(TAG, "Passport 应用已启动");
    return ESP_OK;
}

// --- the re-provisioning question -------------------------------------------
// The hint line, which the settings menu left with one set of words to say.

void passport_app_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!bsp_lvgl_lock(300)) {
        return;
    }

    // A press that only lights the screen again should not also do whatever that key normally does.
    // Standby and the application share the same three buttons, so this is the one place that has
    // to arbitrate between "wake up" and "act".
    if (passport_power_activity()) {
        // Except for the talk button. Someone who holds OK on a dark screen is pressing the button that
        // means "I am about to speak", and requiring them to press it twice — once to light the screen
        // and once to talk — reads as a device that ignored them. The owner reported exactly that. The
        // silence gate is what makes this safe: a press in a pocket opens the microphone, hears a room,
        // and uploads nothing.
        //
        // The release matters just as much as the press, and for a worse reason: if the screen dims
        // again while the button is still held — which it will, because a held button is not the kind of
        // activity that resets the idle timer — a swallowed release leaves the microphone open until the
        // thirty-second ceiling, and the turn is never sent. Both ends of the press are honoured.
        const bool talks = (btn == BSP_BTN_OK);
        bsp_lvgl_unlock();
        if (talks) {
            if (ev == BSP_BTN_PRESS) {
                passport_speech_talk_down();
            } else if (ev == BSP_BTN_RELEASE) {
                passport_speech_talk_up();
            }
        }
        return;
    }

    // Inside the settings menu, the buttons mean what the menu says they mean. This sits above
    // everything else, including push-to-talk, so that choosing an item cannot also start a recording.
    if (s_on_settings) {
        bsp_lvgl_unlock();
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            passport_app_show_home();
            return;
        }
        if (ev != BSP_BTN_CLICK) {
            return;
        }
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            const int step = (btn == BSP_BTN_UP) ? SETTINGS_ITEMS - 1 : 1;
            s_settings_index = (s_settings_index + step) % SETTINGS_ITEMS;
            settings_refresh_locked();
            return;
        }
        if (btn == BSP_BTN_OK) {
            // Only an intent is recorded here. Everything these items actually do belongs to the
            // worker: one raises a hotspot, one writes flash, one spends seconds on the network.
            static const settings_act_t ACTS[SETTINGS_ITEMS] = {
                SETTINGS_ACT_SETUP, SETTINGS_ACT_BRIGHTNESS, SETTINGS_ACT_VOICE_TEST,
                SETTINGS_ACT_STANDBY
            };
            s_settings_act = ACTS[s_settings_index];
            ESP_LOGI(TAG, "设置菜单选择: %s", SETTINGS_LABELS[s_settings_index]);
        }
        return;
    }

    // On the setup screen the only button action is to go back; a second OK press must not
    // start a conversation from a screen that is showing a password.
    if (s_on_setup) {
        bsp_lvgl_unlock();
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            passport_app_show_home();
        }
        return;
    }

    // On the work screen the buttons drive the selection. UP and DOWN move it, OK answers, and
    // a long OK goes back. Releasing the lock first keeps the press path from holding the lock
    // while the redraw happens inside the app's own helper.
    if (s_on_work) {
        bsp_lvgl_unlock();
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            passport_app_show_home();
            return;
        }
        if (ev != BSP_BTN_CLICK) {
            return;
        }
        passport_work_key_t key = PASSPORT_WORK_KEY_NONE;
        if (btn == BSP_BTN_UP) {
            key = PASSPORT_WORK_KEY_UP;
        } else if (btn == BSP_BTN_DOWN) {
            key = PASSPORT_WORK_KEY_DOWN;
        } else if (btn == BSP_BTN_OK) {
            key = PASSPORT_WORK_KEY_CONFIRM;
        }
        if (key == PASSPORT_WORK_KEY_NONE) {
            return;
        }
        if (passport_app_work_press(key) == PASSPORT_WORK_ACT_SEND_ANSWER) {
            // Sending it to the bridge is the next milestone; what the device decided is
            // logged now so the behaviour is observable.
            ESP_LOGI(TAG, "工作模式:回答 %s", s_work_view.options[s_work_view.selected]);
        }
        return;
    }

    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        // No longer a sleep gesture: holding OK now means talking. Deep sleep is on a double press
        // of UP. This branch stays only to swallow the long-press that a long utterance produces,
        // which would otherwise fall through to the release handling.
        bsp_lvgl_unlock();
        return;
    }

    if (btn == BSP_BTN_UP && ev == BSP_BTN_LONG) {
        // Enter the settings menu, and stay there until a long OK comes back.
        //
        // This used to open the setup screen directly, and for a device that was already on a network it
        // asked a yes-or-no question that expired after twenty seconds. The owner asked for the shape
        // that replaced both: a held button *enters* something, and the network details and the join code
        // are items inside it rather than a decision forced on the way past.
        //
        // Only the request is made here. Building a screen and (for the join code) encoding a symbol is
        // the worker's job: this task's stack is smaller than the encoder needs.
        bsp_lvgl_unlock();
        s_settings_requested = true;
        return;
    }

    if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG) {
        // Reach work mode by hand. The bridge will raise it on its own later; this is how a
        // person asks for it, and how the console's feed is reached without a bridge at all.
        bsp_lvgl_unlock();
        passport_app_show_work();
        return;
    }

    if (ev == BSP_BTN_PRESS && btn == BSP_BTN_OK) {
        // Push-to-talk: the microphone opens on the press and the turn is sent on the release.
        // The worker does the audio and the network; this task only records the intent.
        bsp_lvgl_unlock();
        passport_speech_talk_down();
        return;
    }

    if (ev == BSP_BTN_RELEASE && btn == BSP_BTN_OK) {
        bsp_lvgl_unlock();
        passport_speech_talk_up();
        return;
    }

    if (btn == BSP_BTN_UP && ev == BSP_BTN_DOUBLE) {
        // Deep sleep moved here when OK became the talk button: holding OK to speak and holding it
        // to sleep cannot both be true. A double press is deliberate enough to be safe.
        bsp_lvgl_unlock();
        passport_power_sleep(0u);
        return;
    }

    if (ev != BSP_BTN_CLICK) {
        bsp_lvgl_unlock();
        return;
    }

    switch (btn) {
    case BSP_BTN_OK:
        // A tap of OK is a very short recording, which the voice path reports as "nothing heard".
        // It deliberately does not also start a text turn: one button, one meaning. The text-only
        // turn is still available from the console as "llm ask".
        bsp_lvgl_unlock();
        return;
    case BSP_BTN_UP:
    case BSP_BTN_DOWN: {
        // Step through the expressions. This is how a host verifies the whole
        // sprite set over the serial console without a UI menu.
        int next = (int)passport_pet_mood() + (btn == BSP_BTN_DOWN ? 1 : -1);
        if (next < 0) {
            next = PASSPORT_PET_MOOD_COUNT - 1;
        }
        if (next >= PASSPORT_PET_MOOD_COUNT) {
            next = 0;
        }
        s_sleeping = (next == PASSPORT_PET_SLEEPING);
        set_mood((passport_pet_mood_t)next, 0);
        break;
    }
    default:
        break;
    }
    bsp_lvgl_unlock();
}

void passport_app_begin_turn(const char *prompt)
{
    if (!bsp_lvgl_lock(500)) {
        return;
    }
    set_mood(PASSPORT_PET_THINKING, 0);
    if (prompt != NULL && prompt[0] != '\0') {
        set_bubble(prompt);
    }
    bsp_lvgl_unlock();
}

void passport_app_show_reply(const char *text)
{
    if (text == NULL) {
        return;
    }
    if (!bsp_lvgl_lock(500)) {
        return;
    }
    // Order matters: set_mood() writes the mood's stock line into the bubble, so the
    // answer has to go in after it or it would be overwritten by "Here is what I found."
    set_mood(PASSPORT_PET_SPEAKING, 2500u);
    char shown[BUBBLE_MAX];
    const size_t keep = passport_utf8_truncate(text, sizeof(shown) - 1u);
    memcpy(shown, text, keep);
    shown[keep] = '\0';
    set_bubble(shown);
    bsp_lvgl_unlock();
}

void passport_app_show_error(const char *reason)
{
    if (!bsp_lvgl_lock(500)) {
        return;
    }
    set_mood(PASSPORT_PET_DISTRESSED, 4000u);
    set_bubble((reason != NULL) ? reason : "Something went wrong.");
    bsp_lvgl_unlock();
}

void passport_app_show_status(const char *text, passport_pet_mood_t mood){
    if (!bsp_lvgl_lock(500)) {
        return;
    }
    // The mood goes first: set_mood() writes that mood's stock line into the bubble, so a status
    // written before it would be overwritten a moment later.
    set_mood(mood, 0);
    if (text != NULL && text[0] != '\0') {
        set_bubble(text);
    }
    bsp_lvgl_unlock();
}

void passport_app_stop(void)
{
    s_running = false;
    // The worker only touches LVGL under the lock and only between sleeps, so
    // waiting for it here cannot deadlock against the caller.
    for (int waited = 0; s_task != NULL && waited < 1000; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (bsp_lvgl_lock(1000)) {
        passport_pet_destroy();
        // The QR pixels came from the system heap, not from LVGL, so they are freed here
        // rather than by deleting the widget.
        heap_caps_free(s_qr_pixels);
        s_qr_pixels = NULL;
        s_qr_side = 0;
        if (s_work != NULL) {
            lv_obj_delete(s_work);
            s_work = NULL;
            s_work_task = NULL;
            s_work_state = NULL;
            s_work_bar = NULL;
            s_work_percent = NULL;
            s_work_question = NULL;
            for (int i = 0; i < PASSPORT_WORK_MAX_OPTIONS; i++) {
                s_work_options[i] = NULL;
            }
            s_on_work = false;
        }
        if (s_setup != NULL) {
            lv_obj_delete(s_setup);
            s_setup = NULL;
            s_setup_hotspot = NULL;
            s_setup_password = NULL;
            s_setup_address = NULL;
            s_on_setup = false;
        }
        if (s_screen != NULL) {
            lv_obj_delete(s_screen);
        }
        s_screen = NULL;
        s_battery = NULL;
        s_bubble = NULL;
        s_mood = NULL;
        s_hint = NULL;
        bsp_lvgl_unlock();
    }
}
