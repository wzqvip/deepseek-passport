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
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
// Not re-exported by lvgl.h: the QR descriptor hands LVGL a raw buffer from the system heap, so the
// cached entry has to be dropped when that buffer goes away. See qr_release().
#include "misc/cache/instance/lv_image_cache.h"
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
#include "passport_work_client.h"
#include "passport_navigation.h"

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

static settings_act_t s_settings_act;
static uint32_t s_settings_generation;
static passport_navigation_t s_nav;
static atomic_bool s_input_cancel_pending;

// Caller holds the LVGL lock. Preserve recovery across failed lock attempts.
static void apply_input_cancel(void)
{
    if (atomic_exchange(&s_input_cancel_pending, false)) {
        (void)passport_navigation_cancel(&s_nav);
        s_settings_act = SETTINGS_ACT_NONE;
    }
}
static bool s_answer_pending;
static passport_work_view_t s_answer_view;
static lv_obj_t *s_settings;
static lv_obj_t *s_settings_info;
static lv_obj_t *s_settings_items[SETTINGS_ITEMS];
static int s_settings_index;
static lv_obj_t *s_bubble;
static lv_obj_t *s_mood;
static lv_obj_t *s_hint;
static TaskHandle_t s_task;
static volatile bool s_running;


// The setup screen. It exists because the hotspot credentials were otherwise only
// readable over the serial console, which defeats the point of provisioning without a
// cable: the person setting the device up is holding a phone, not a terminal.
static lv_obj_t *s_setup;
static lv_obj_t *s_setup_hotspot;
static lv_obj_t *s_setup_password;
static lv_obj_t *s_setup_address;
// Page and gesture state are protected by the LVGL lock.

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

// The QR pixels belong to the setup screen and are released only after its image
// widget is deleted. Keeping them off the heap between visits leaves room for TLS.
static void qr_release(void)
{
    if (s_qr_pixels != NULL) {
        // The image cache keys decoded images by their descriptor address, and this descriptor is
        // reused for the next symbol. Dropping the entry before the pixels are freed is what stops
        // a later draw from reading them. LVGL 9.5 declares this in an internal header that lvgl.h
        // does not re-export, so the header is included by path where it lives in the component.
        lv_image_cache_drop(&s_qr_dsc);
        heap_caps_free(s_qr_pixels);
        s_qr_pixels = NULL;
        s_qr_side = 0;
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

    plain(s_setup, 0, 268, 240, 3, APP_ACCENT);
    lv_label_set_text(label(s_setup, 10, 272, passport_font_body(), APP_MUTED, "setup_back"),
                      "长按OK 返回");

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
    passport_navigation_show(&s_nav, PASSPORT_PAGE_SETUP);
    bsp_lvgl_unlock();
}

// Delete the image widget before releasing its backing pixels; rebuild on re-entry.
// Caller holds the LVGL lock and has already loaded another screen.
static void setup_release(void)
{
    if (s_setup != NULL) {
        lv_obj_delete(s_setup);
        s_setup = NULL;
        s_setup_hotspot = NULL;
        s_setup_password = NULL;
        s_setup_address = NULL;
    }
    qr_release();
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
    passport_navigation_show(&s_nav, PASSPORT_PAGE_HOME);
    setup_release();
    bsp_lvgl_unlock();
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
    passport_navigation_show(&s_nav, PASSPORT_PAGE_WORK);
    setup_release();
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
    lv_obj_set_height(s_settings_info, 60);
    lv_label_set_long_mode(s_settings_info, LV_LABEL_LONG_WRAP);

    for (int i = 0; i < SETTINGS_ITEMS; i++) {
        char name[24];
        snprintf(name, sizeof(name), "settings_item%d", i);
        s_settings_items[i] = label(s_settings, 10, 102 + i * 34, passport_font_body(), APP_PAPER, name);
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
            const int keep = (int)passport_utf8_truncate(cfg->wifi_ssid, 18);
            snprintf(line, sizeof(line), "网络 %.*s\nhttp://%.15s/", keep, cfg->wifi_ssid, ip);
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
        if (i == 1) {
            snprintf(text, sizeof(text), "%s%s %u%%", selected ? "> " : "   ",
                     SETTINGS_LABELS[i], (unsigned)passport_config_current()->brightness);
        } else {
            snprintf(text, sizeof(text), "%s%s", selected ? "> " : "   ", SETTINGS_LABELS[i]);
        }
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
    passport_navigation_show(&s_nav, PASSPORT_PAGE_SETTINGS);
    setup_release();
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
                         !passport_work_same_question(&parsed, &s_work_view);

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

    if (passport_work_same_question(&parsed, &s_work_view)) {
        parsed.selected = s_work_view.selected;
        parsed.answered = s_work_view.answered;
    }
    s_work_view = parsed;
    if (s_work == NULL) {
        build_work();
    }
    // A document that arrives while the home screen is showing takes it over, the way a notification
    // would. It does not interrupt the setup screen: someone mid-provisioning should not be thrown out
    // of it by a background update.
    if (passport_navigation_accept_work(&s_nav, changed, worth_showing)) {
        lv_screen_load(s_work);
        passport_navigation_show(&s_nav, PASSPORT_PAGE_WORK);
    }
    work_refresh();
    bsp_lvgl_unlock();
    return true;
}

passport_work_action_t passport_app_work_press(passport_work_key_t key)
{
    if (!bsp_lvgl_lock(500)) return PASSPORT_WORK_ACT_NONE;
    const passport_work_action_t action = passport_work_key(&s_work_view, key);
    if (action == PASSPORT_WORK_ACT_REDRAW) work_refresh();
    bsp_lvgl_unlock();
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
    // A completed HTTP operation must never be repeated just because UI locking
    // timed out. Retain its result locally until it can be applied under the lock.
    bool answer_result_ready = false;
    esp_err_t answer_result = ESP_OK;
    passport_work_view_t completed_answer;

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
            settings_act_t act = SETTINGS_ACT_NONE;
            uint32_t generation = 0;
            passport_work_view_t answer;
            bool send_answer = false;
            if (bsp_lvgl_lock(200)) {
                apply_input_cancel();
                if (answer_result_ready) {
                    s_answer_pending = false;
                    if (passport_work_same_question(&completed_answer, &s_work_view)) {
                        if (answer_result != ESP_OK) s_work_view.answered = false;
                        work_refresh();
                        lv_label_set_text(s_work_state, answer_result == ESP_OK ? "sent" : "retry");
                    }
                    answer_result_ready = false;
                }
                generation = s_settings_generation;
                if (generation == s_nav.generation) act = s_settings_act;
                s_settings_act = SETTINGS_ACT_NONE;
                if (s_answer_pending) {
                    answer = s_answer_view;
                    send_answer = true;
                }
                bsp_lvgl_unlock();
            }
            if (send_answer) {
                completed_answer = answer;
                answer_result = passport_work_client_answer_for(answer.question_id,
                    answer.options[answer.selected]);
                answer_result_ready = true;
            }
            // HTTP above can take seconds. Discard actions from a page/gesture
            // that was left or cancelled while waiting, before any side effect.
            if (act != SETTINGS_ACT_NONE) {
                if (bsp_lvgl_lock(200)) {
                    apply_input_cancel();
                    if (!s_running || generation != s_nav.generation) act = SETTINGS_ACT_NONE;
                    bsp_lvgl_unlock();
                } else {
                    act = SETTINGS_ACT_NONE;
                }
            }
            switch (act) {
            case SETTINGS_ACT_SETUP: {
                const esp_err_t err = passport_provision_active() ? ESP_OK : passport_provision_start();
                if (bsp_lvgl_lock(1000)) {
                    if (s_running && generation == s_nav.generation) {
                        if (err == ESP_OK) passport_app_show_setup();
                        else if (s_settings_info != NULL) lv_label_set_text(s_settings_info, "配网失败");
                        else set_bubble("配网失败");
                    }
                    bsp_lvgl_unlock();
                }
                break;
            }
            case SETTINGS_ACT_BRIGHTNESS: {
                const uint8_t now = passport_config_current()->brightness;
                const uint8_t next = passport_navigation_brightness(now);
                passport_config_mutable()->brightness = next;
                const esp_err_t err = passport_config_store_save();
                if (err != ESP_OK) passport_config_mutable()->brightness = now;
                bsp_display_backlight(err == ESP_OK ? next : now);
                settings_refresh_locked();
                if (err != ESP_OK && bsp_lvgl_lock(500)) {
                    lv_label_set_text(s_settings_info, "设置失败");
                    bsp_lvgl_unlock();
                }
                break;
            }
            case SETTINGS_ACT_VOICE_TEST: {
                if (bsp_lvgl_lock(500)) {
                    lv_label_set_text(s_settings_info, "已提交试听");
                    bsp_lvgl_unlock();
                }
                const esp_err_t err = passport_speech_speak("你好，我是 DeepSeek 随身宠物。");
                if (err != ESP_OK && bsp_lvgl_lock(500)) {
                    lv_label_set_text(s_settings_info, "试听失败,请重试");
                    bsp_lvgl_unlock();
                }
                break;
            }
            case SETTINGS_ACT_STANDBY:
                passport_power_standby();
                break;
            default:
                break;
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
    if (passport_config_current()->wifi_ssid[0] == '\0') {
        s_settings_act = SETTINGS_ACT_SETUP;
        s_settings_generation = s_nav.generation;
    }
    // QR encoding runs on this worker; its scratch buffers require several kilobytes.
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
    // Unconfigured startup is queued before worker creation; QR encoding never runs
    // on the smaller main-task stack.

    ESP_LOGI(TAG, "Passport 应用已启动");
    return ESP_OK;
}

// --- the re-provisioning question -------------------------------------------
// The hint line, which the settings menu left with one set of words to say.

void passport_app_cancel_input(void)
{
    // Close/cancel audio even if rendering is stalled; never submit a broken gesture.
    atomic_store(&s_input_cancel_pending, true);
    passport_speech_talk_cancel();
    if (bsp_lvgl_lock(100)) {
        apply_input_cancel();
        bsp_lvgl_unlock();
    }
}

void passport_app_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_running) return;
    if (!bsp_lvgl_lock(300)) {
        // Any missing event breaks gesture ordering; release must close audio even
        // while LVGL is unavailable. Recovery stays pending until a lock succeeds.
        atomic_store(&s_input_cancel_pending, true);
        passport_speech_talk_cancel();
        return;
    }
    apply_input_cancel();
    const passport_page_t page = s_nav.page;
    const passport_nav_action_t action = passport_navigation_key(&s_nav,
        (passport_nav_key_t)btn, (passport_nav_event_t)ev, passport_power_activity());
    switch (action) {
    case PASSPORT_NAV_SETTINGS:
        passport_app_show_settings();
        break;
    case PASSPORT_NAV_WORK:
        passport_app_show_work();
        break;
    case PASSPORT_NAV_BACK:
        if (passport_navigation_back(&s_nav) == PASSPORT_PAGE_SETTINGS)
            passport_app_show_settings();
        else passport_app_show_home();
        break;
    case PASSPORT_NAV_TALK_DOWN:
        passport_speech_talk_down();
        break;
    case PASSPORT_NAV_TALK_UP:
        passport_speech_talk_up();
        break;
    case PASSPORT_NAV_SLEEP:
        s_settings_act = SETTINGS_ACT_STANDBY;
        s_settings_generation = s_nav.generation;
        break;
    case PASSPORT_NAV_PREVIOUS:
    case PASSPORT_NAV_NEXT:
    case PASSPORT_NAV_CONFIRM:
        if (page == PASSPORT_PAGE_SETTINGS) {
            if (action == PASSPORT_NAV_CONFIRM) {
                static const settings_act_t acts[SETTINGS_ITEMS] = {
                    SETTINGS_ACT_SETUP, SETTINGS_ACT_BRIGHTNESS,
                    SETTINGS_ACT_VOICE_TEST, SETTINGS_ACT_STANDBY
                };
                if (s_settings_act == SETTINGS_ACT_NONE) {
                    s_settings_act = acts[s_settings_index];
                    s_settings_generation = s_nav.generation;
                }
            } else {
                s_settings_index = (s_settings_index +
                    (action == PASSPORT_NAV_PREVIOUS ? SETTINGS_ITEMS - 1 : 1)) % SETTINGS_ITEMS;
                settings_refresh();
            }
        } else if (page == PASSPORT_PAGE_WORK) {
            const passport_work_key_t key = action == PASSPORT_NAV_CONFIRM ? PASSPORT_WORK_KEY_CONFIRM :
                action == PASSPORT_NAV_PREVIOUS ? PASSPORT_WORK_KEY_UP : PASSPORT_WORK_KEY_DOWN;
            if (!s_answer_pending && passport_work_key(&s_work_view, key) == PASSPORT_WORK_ACT_SEND_ANSWER) {
                s_answer_view = s_work_view;
                s_answer_pending = true;
            }
            work_refresh();
        } else if (action != PASSPORT_NAV_CONFIRM) {
            int next = ((int)passport_pet_mood() + PASSPORT_PET_MOOD_COUNT +
                (action == PASSPORT_NAV_PREVIOUS ? -1 : 1)) % PASSPORT_PET_MOOD_COUNT;
            set_mood((passport_pet_mood_t)next, 0);
        }
        break;
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
    const char *message = (reason != NULL) ? reason : "Something went wrong.";
    set_bubble(message);
    if (s_nav.page == PASSPORT_PAGE_SETTINGS && s_settings_info != NULL) {
        lv_label_set_text(s_settings_info, message);
    }
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
        if (s_nav.page == PASSPORT_PAGE_SETTINGS && s_settings_info != NULL) {
            lv_label_set_text(s_settings_info, text);
        }
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
    // A network operation may still own this worker. Keep its UI alive rather than
    // deleting objects it can touch after the bounded wait.
    if (s_task != NULL) {
        ESP_LOGW(TAG, "应用工作线程仍在退出;保留界面");
        return;
    }
    if (bsp_lvgl_lock(1000)) {
        passport_pet_destroy();
        if (s_settings != NULL) {
            lv_obj_delete(s_settings);
            s_settings = NULL;
            s_settings_info = NULL;
            memset(s_settings_items, 0, sizeof(s_settings_items));
        }
        memset(&s_nav, 0, sizeof(s_nav));
        s_settings_act = SETTINGS_ACT_NONE;
        s_answer_pending = false;
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

        }
        if (s_setup != NULL) {
            lv_obj_delete(s_setup);
            s_setup = NULL;
            s_setup_hotspot = NULL;
            s_setup_password = NULL;
            s_setup_address = NULL;

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
