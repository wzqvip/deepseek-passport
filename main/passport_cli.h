// main/passport_cli.h
// Serial console for driving and inspecting the device from a host.
//
// The screen and the console describe the same objects by the same names: the
// application names its widgets with lv_obj_set_name(), and every `ui` command
// accepts those names as a slash-separated path from the active screen. There is
// no second element registry to drift out of sync with what is on the display.
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

// Receives button events injected with `ui key`. The application registers its
// own input path, so an injected event travels exactly the same route as a
// physical press. Without a sink, `ui key` reports that injection is unavailable.
typedef void (*passport_cli_key_sink_t)(bsp_btn_t btn, bsp_btn_ev_t ev);

void passport_cli_set_key_sink(passport_cli_key_sink_t sink);

// Start the REPL on the USB Serial/JTAG console. Call once, after the display,
// LVGL, and the application input path are up. Calling it twice is harmless.
esp_err_t passport_cli_start(void);
