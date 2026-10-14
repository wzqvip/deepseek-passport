// main/passport_power.h
// Standby: turning the screen off when nobody is there, and sleeping deeply when asked.
//
// The wake side of this already existed: waking by key reboots the chip with that key still held,
// which is why key_guard swallows key actions until the press comes up, and why the BSP must hand
// the button pin back as a plain digital input before sleeping — with the ADC still driving it the
// pin reads low, which is exactly the level the wake condition tests, so the device would wake
// instantly. This module is the missing half: deciding when to sleep and going there.
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

// Enter deep sleep, waking when a key is pressed.
//
// `timer_wake_seconds` arms a timer wake as well. Zero means "only the key", which is the shipping
// behaviour; a non-zero value is a safety net for testing, so a device cannot be left asleep by a
// mistake. Returns only on failure: on success the chip does not come back to this function.
esp_err_t passport_power_sleep(uint32_t timer_wake_seconds);

// Describe the standby state for the console.
void passport_power_report(char *out, size_t out_size);
