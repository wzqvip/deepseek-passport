// main/passport_app.h
// The Passport application: its own screens, layout, and controls.
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

// Build the application UI and start its worker task. Takes the LVGL lock itself.
esp_err_t passport_app_start(void);

// Route one button event from the application's input task.
void passport_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// Conversation hooks, called from the chat worker. Each takes the LVGL lock itself,
// so callers must not already hold it.
void passport_app_begin_turn(const char *prompt);
void passport_app_show_reply(const char *text);
void passport_app_show_error(const char *reason);

// Stop the worker and delete the UI. Called before teardown.
void passport_app_stop(void);
