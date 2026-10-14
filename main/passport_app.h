// main/passport_app.h
// The Passport application: its own screens, layout, and controls.
#pragma once

#include <stdbool.h>

#include "bsp_button.h"
#include "esp_err.h"
#include "passport_pet.h"
#include "passport_work.h"

// Build the application UI and start its worker task. Takes the LVGL lock itself.
esp_err_t passport_app_start(void);

// Route one button event from the application's input task.
void passport_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// Conversation hooks, called from the chat worker. Each takes the LVGL lock itself,
// so callers must not already hold it.
void passport_app_begin_turn(const char *prompt);
void passport_app_show_reply(const char *text);
void passport_app_show_error(const char *reason);

// Say what the device is doing right now, with the expression that goes with it: listening,
// transcribing, hearing nothing. Also takes the LVGL lock itself.
void passport_app_show_status(const char *text, passport_pet_mood_t mood);

// Switch between the home screen and the setup screen that shows the hotspot credentials.
void passport_app_show_setup(void);
void passport_app_show_home(void);

// Work mode. The application owns the view and the screen; the console and the input task are
// both thin drivers, so a document fed from the console updates exactly what a button press
// would, and there is one copy of the state rather than two that can disagree.
void passport_app_show_work(void);
bool passport_app_work_feed(const char *json);
passport_work_action_t passport_app_work_press(passport_work_key_t key);
const passport_work_view_t *passport_app_work_current(void);

// The join code as it was actually drawn, for a console check of the pixels rather than of the
// encoder. Returns false when no code is currently rendered.
//
// This exists because a symbol can be perfect and still be unscannable: inverting the two colours
// leaves a matrix that is correct cell for cell and cannot be read by any camera. Only the pixels
// can tell you that.
bool passport_app_qr_drawn(const uint16_t **pixels, int *side, int *scale, int *modules,
                           int *quiet_zone);

// Stop the worker and delete the UI. Called before teardown.
void passport_app_stop(void);
