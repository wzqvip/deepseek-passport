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
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "passport_audio.h"
#include "passport_chat.h"
#include "passport_fonts.h"
#include "passport_pet.h"
#include "passport_utf8.h"

static const char *TAG = "passport_app";

// Own palette. Deliberately not the demo shell's sky/grass/cloud scheme.
#define APP_BG      0x101828
#define APP_PANEL   0x18233A
#define APP_ACCENT  0x4D6BFE
#define APP_PAPER   0xEAF0FF
#define APP_MUTED   0x8FA3C8

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
static lv_obj_t *s_bubble;
static lv_obj_t *s_mood;
static lv_obj_t *s_hint;
static TaskHandle_t s_task;
static volatile bool s_running;
static bool s_sleeping;

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
    lv_obj_t *title = label(s_screen, 10, 6, &lv_font_montserrat_14, APP_MUTED, "title");
    lv_label_set_text(title, "PASSPORT");
    s_battery = label(s_screen, 150, 6, &lv_font_montserrat_14, APP_PAPER, "battery");
    lv_obj_set_width(s_battery, 80);
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

    s_hint = label(s_screen, 10, 244, &lv_font_montserrat_14, APP_MUTED, "hint");
    lv_label_set_text(s_hint, "OK:提问    长按OK:待机");

    set_mood(PASSPORT_PET_IDLE, 0);
    lv_screen_load(s_screen);
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

static void app_task(void *arg)
{
    (void)arg;
    while (s_running) {
        battery_refresh();
        // Between polls the task sleeps, so the gauge read never competes with
        // rendering and the pet timer keeps its cadence.
        for (int waited = 0; s_running && waited < BATTERY_POLL_MS; waited += 100) {
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
    if (xTaskCreate(app_task, "passport_app", 3584, NULL, 4, &s_task) != pdPASS) {
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

    ESP_LOGI(TAG, "Passport 应用已启动");
    return ESP_OK;
}

void passport_app_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!bsp_lvgl_lock(300)) {
        return;
    }

    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        // Local standby preview. Real power management is a later milestone; this
        // only shows the sleeping expression and states it on screen.
        s_sleeping = !s_sleeping;
        set_mood(s_sleeping ? PASSPORT_PET_SLEEPING : PASSPORT_PET_IDLE, 0);
        bsp_lvgl_unlock();
        return;
    }

    if (ev != BSP_BTN_CLICK) {
        bsp_lvgl_unlock();
        return;
    }

    switch (btn) {
    case BSP_BTN_OK:
        // Start a conversational turn. The network work belongs to the chat worker, so
        // this task only queues it — and it releases the lock first, because the worker
        // takes the same lock to show the thinking expression.
        bsp_lvgl_unlock();
        if (passport_chat_ask(DEFAULT_PROMPT) != ESP_OK) {
            passport_app_show_error("One moment, still thinking.");
        }
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
