// main/passport_power.c
#include "passport_power.h"

#include <stdio.h>

#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "passport_config_store.h"

static const char *TAG = "passport_power";

// ESP32-C3 cannot atomically load/store an int64_t. Button callbacks, the
// application worker, and console tasks all access this state. Never call a
// driver or wait for a lock while holding this short critical section.
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_activity_us;
static bool s_screen_on = true;       // requested state, not a hardware readback
static bool s_applied_on = true;      // written only by the backlight mutex owner
static uint32_t s_off_after_seconds;
static bool s_started;
static bool s_manual_standby;
static SemaphoreHandle_t s_backlight_lock;
static uint8_t s_brightness;
static uint32_t s_generation;

// Every writer reconciles the newest request before releasing the mutex. This
// makes a callback's nonblocking try-lock sufficient even when a tick is doing
// backlight I/O: that owner applies the callback's wake, without waiting for the
// application worker's next (possibly network-delayed) iteration.
static void apply_backlight(void)
{
    if (s_backlight_lock == NULL || xSemaphoreTake(s_backlight_lock, 0) != pdTRUE) {
        return;
    }
    for (;;) {
        portENTER_CRITICAL(&s_state_lock);
        const bool target_on = s_screen_on;
        const uint8_t brightness = s_brightness;
        const uint32_t generation = s_generation;
        const bool changed = s_applied_on != target_on;
        portEXIT_CRITICAL(&s_state_lock);
        if (changed) {
            bsp_display_backlight(target_on ? brightness : 0);
        }
        portENTER_CRITICAL(&s_state_lock);
        s_applied_on = target_on;
        const bool stable = generation == s_generation;
        if (stable) {
            // Unlock before leaving the state critical section closes the lost
            // wake window between the final generation check and mutex give.
            xSemaphoreGive(s_backlight_lock);
        }
        portEXIT_CRITICAL(&s_state_lock);
        if (stable) {
            break;
        }
    }
}

esp_err_t passport_power_start(void)
{
    // Startup is called once by app_main. Pre-start callbacks only record
    // activity; apply_backlight safely skips I/O until this mutex exists.
    if (s_backlight_lock == NULL) {
        s_backlight_lock = xSemaphoreCreateMutex();
        if (s_backlight_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    const int64_t now = esp_timer_get_time();
    const uint32_t off_after = passport_config_current()->idle_off_seconds;
    const uint8_t brightness = passport_config_current()->brightness;
    portENTER_CRITICAL(&s_state_lock);
    if (!s_started) {
        s_last_activity_us = now;
        s_screen_on = true;
        s_applied_on = true;
        s_brightness = brightness;
        s_off_after_seconds = off_after;
        s_started = true;
        ++s_generation;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

uint32_t passport_power_idle_seconds(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool started = s_started;
    const int64_t last = s_last_activity_us;
    portEXIT_CRITICAL(&s_state_lock);
    const int64_t elapsed = esp_timer_get_time() - last;
    return (!started || elapsed <= 0) ? 0u : (uint32_t)(elapsed / 1000000);
}

bool passport_power_screen_on(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool on = s_screen_on;
    portEXIT_CRITICAL(&s_state_lock);
    return on;
}

bool passport_power_activity(void)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    const bool was_off = !s_screen_on;
    // A callback sampled before another task must not move the deadline back.
    if (now > s_last_activity_us) {
        s_last_activity_us = now;
    }
    s_screen_on = true;
    s_manual_standby = false;
    ++s_generation;
    portEXIT_CRITICAL(&s_state_lock);
    apply_backlight();
    return was_off;
}

void passport_power_standby(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_manual_standby = true;
    s_screen_on = false;
    ++s_generation;
    portEXIT_CRITICAL(&s_state_lock);
    apply_backlight();
}

void passport_power_tick(void)
{
    const uint32_t off_after = passport_config_current()->idle_off_seconds;
    const uint8_t brightness = passport_config_current()->brightness;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    if (!s_started) {
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }
    const int64_t idle_us = now - s_last_activity_us;
    s_off_after_seconds = off_after;
    s_brightness = brightness;
    s_screen_on = !s_manual_standby && (off_after == 0u || idle_us < 0 ||
        idle_us < (int64_t)off_after * 1000000);
    ++s_generation;
    portEXIT_CRITICAL(&s_state_lock);
    apply_backlight();
}

esp_err_t passport_power_sleep(uint32_t timer_wake_seconds)
{
    (void)timer_wake_seconds;
    // Fail BEFORE the first terminal BSP operation. Button preparation can
    // partially delete input devices even when it reports failure; returning
    // afterwards would leave a running application with dead buttons. Likewise
    // LVGL locking alone does not stop battery/I2C, audio, speech/network and
    // provisioning producers, and a busy snapshot cannot exclude a new request.
    // No public application-wide stop/join + reject-new-work handshake exists.
    // Until one is implemented, do not mutate wake sources, release shared pins,
    // disable the codec, or tear down the display (locked OR unlocked).
    ESP_LOGW(TAG, "Deep sleep unavailable: resource producers cannot be quiesced safely");
    return ESP_ERR_NOT_SUPPORTED;
}

void passport_power_report(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    const bool on = s_screen_on;
    const uint32_t off_after = s_off_after_seconds;
    portEXIT_CRITICAL(&s_state_lock);
    snprintf(out, out_size, "screen=%s idle=%us off_after=%us deep_sleep=unsupported",
             on ? "on" : "off", (unsigned)passport_power_idle_seconds(),
             (unsigned)off_after);
}
