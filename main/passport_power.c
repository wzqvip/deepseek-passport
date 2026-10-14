// main/passport_power.c
#include "passport_power.h"

#include <stdio.h>
#include <string.h>

#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "passport_config_store.h"

static const char *TAG = "passport_power";

static int64_t s_last_activity_us;
static bool s_screen_on = true;
static uint32_t s_off_after_seconds;
static bool s_started;

esp_err_t passport_power_start(void)
{
    s_last_activity_us = esp_timer_get_time();
    s_screen_on = true;
    s_off_after_seconds = passport_config_current()->idle_off_seconds;
    s_started = true;
    ESP_LOGI(TAG, "待机:静置 %u 秒后熄屏(0 表示不熄)", (unsigned)s_off_after_seconds);
    return ESP_OK;
}

uint32_t passport_power_idle_seconds(void)
{
    if (!s_started) {
        return 0u;
    }
    const int64_t elapsed = esp_timer_get_time() - s_last_activity_us;
    return (uint32_t)(elapsed / 1000000);
}

bool passport_power_screen_on(void)
{
    return s_screen_on;
}

bool passport_power_activity(void)
{
    const bool was_off = !s_screen_on;
    s_last_activity_us = esp_timer_get_time();
    if (was_off) {
        bsp_display_backlight(passport_config_current()->brightness);
        s_screen_on = true;
        ESP_LOGI(TAG, "屏幕已点亮");
    }
    return was_off;
}

void passport_power_tick(void)
{
    if (!s_started || !s_screen_on) {
        return;
    }
    // Read live rather than once at start: a setting changed from the console or the web console
    // should take effect without a reboot.
    const uint32_t off_after = passport_config_current()->idle_off_seconds;
    s_off_after_seconds = off_after;
    // Zero means "never turn it off", which is the default this project ships with: the owner asked
    // for an always-connected device first and power later.
    if (off_after == 0u) {
        return;
    }
    if (passport_power_idle_seconds() >= off_after) {
        // The backlight only: the interface keeps running, so the screen is instantly there again
        // rather than needing a redraw from nothing.
        bsp_display_backlight(0);
        s_screen_on = false;
        ESP_LOGI(TAG, "静置 %u 秒,已熄屏", (unsigned)off_after);
    }
}

esp_err_t passport_power_sleep(uint32_t timer_wake_seconds)
{
    // A key that is already held would satisfy the wake condition the moment we sleep, so the
    // device would wake immediately and appear not to have slept at all. The BSP still owns the
    // pin here, which is the only moment this can be checked.
    const int mv = bsp_button_read_mv();
    if (mv >= 0 && mv < BSP_BTN_MV_RELEASED_MIN) {
        ESP_LOGW(TAG, "有按键被按住(%d mV),此时入睡会立刻被唤醒;请松手后重试", mv);
        return ESP_ERR_INVALID_STATE;
    }

    // Hands the pin back as a digital input and stops the button devices. After this the buttons are
    // unusable for the rest of this run, so the only correct next step is to sleep.
    int level = 0;
    const esp_err_t prepared = bsp_button_prepare_deep_sleep(&level);
    if (prepared != ESP_OK) {
        ESP_LOGE(TAG, "深睡准备失败: %s;不入睡,避免无法唤醒", esp_err_to_name(prepared));
        return prepared;
    }

    // This chip has no EXT1 source; the GPIO wake is what deep sleep offers here.
    //
    // The level to wake on is the one a *press* produces, and the BSP reports the level the pin
    // settles at with nothing pressed — 1 on this board, because the keypad node is pulled up and a
    // key pulls it down. Arming the wake on the idle level instead makes the condition true at the
    // instant of sleep, so the device wakes immediately and looks as though it never slept. That is
    // what happened the first time this ran: the fix is to invert the mapping, and the observation
    // is recorded because the mistake is easy to repeat.
    const uint64_t mask = 1ULL << BSP_BTN_GPIO;
    const esp_deepsleep_gpio_wake_up_mode_t mode =
        (level != 0) ? ESP_GPIO_WAKEUP_GPIO_LOW : ESP_GPIO_WAKEUP_GPIO_HIGH;
    esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(mask, mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键唤醒配置失败: %s;不入睡", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "按键唤醒电平: 空闲=%d,入睡后等待相反电平", level);

    if (timer_wake_seconds > 0u) {
        err = esp_sleep_enable_timer_wakeup((uint64_t)timer_wake_seconds * 1000000ULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "定时唤醒配置失败: %s;不入睡", esp_err_to_name(err));
            return err;
        }
        ESP_LOGW(TAG, "已配置 %u 秒定时唤醒作为保险(仅测试用)", (unsigned)timer_wake_seconds);
    }

    // Shut the peripherals down in order before sleeping. Skipping this is the difference between
    // deep sleep and pretending: the panel keeps its charge pumps running, the codec stays powered,
    // and the pins sit at whatever level they were left at, which is both a current cost and a way to
    // wake to a glitched screen. The order below is the one the board's own reference application
    // used, and a repository test guards it — that test read the deleted demo's sleep task, which is
    // how this omission was found.
    bsp_battery_sleep();
    bsp_audio_sleep();
    bsp_audio_prepare_deep_sleep();
    bsp_i2c_prepare_deep_sleep();
    // The panel teardown must not race a redraw, so it happens under the interface's lock.
    if (bsp_lvgl_lock(1000)) {
        bsp_display_prepare_deep_sleep();
        bsp_lvgl_unlock();
    } else {
        ESP_LOGW(TAG, "界面锁超时,仍要熄灭面板");
        bsp_display_prepare_deep_sleep();
    }

    ESP_LOGI(TAG, "即将深睡;按任意键唤醒");
    // Flush before the console disappears: deep sleep takes the USB serial with it.
    fflush(stdout);
    esp_deep_sleep_start();
    return ESP_OK;   // not reached
}

void passport_power_report(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return;
    }
    snprintf(out, out_size, "screen=%s idle=%us off_after=%us",
             s_screen_on ? "on" : "off", (unsigned)passport_power_idle_seconds(),
             (unsigned)s_off_after_seconds);
}
