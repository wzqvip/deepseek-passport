// main/passport_power.h
// Connected standby: only the backlight turns off; networking and input remain
// alive. Deep sleep is deliberately refused until all shared-resource producers
// have a stop/join and reject-new-work lifecycle.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Note the boot time as activity, so a device that just started does not immediately dim.
esp_err_t passport_power_start(void);

// Any key press: reset the idle deadline, and light the screen again if it was off.
// Returns true when the screen had been off, which means the press was only a wake-up and should
// not also trigger whatever that key normally does.
bool passport_power_activity(void);

// Called regularly by the application worker. Turns the screen off once the configured idle time
// has passed.
void passport_power_tick(void);

bool passport_power_screen_on(void);
uint32_t passport_power_idle_seconds(void);

// Turn off only the backlight until the next activity; no peripheral teardown.
// Safe during recording/playback/network work. Does not claim deep-sleep current.
void passport_power_standby(void);

// Currently returns ESP_ERR_NOT_SUPPORTED without mutating hardware or wake
// sources, regardless of timer_wake_seconds. A speech busy snapshot is NOT a
// resource-quiescence barrier. Do not restore terminal BSP shutdown until input,
// battery, audio, networking, provisioning and LVGL producers are stopped/joined
// and new work is excluded; failures after terminal prepare cannot return live.
esp_err_t passport_power_sleep(uint32_t timer_wake_seconds);

// Describe the standby state for the console.
void passport_power_report(char *out, size_t out_size);
