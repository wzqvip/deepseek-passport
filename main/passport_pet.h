// main/passport_pet.h
// The pet character: a sprite widget whose expression reflects application state.
//
// The sprite is drawn straight from flash through the generated descriptors in
// main/assets. Animation swaps the image source on one widget, so the resident
// LVGL object count never changes while it animates.
#pragma once

#include "lvgl.h"

typedef enum {
    PASSPORT_PET_IDLE = 0,   // breathing, blinking
    PASSPORT_PET_CURIOUS,    // a notification or a key press arrived
    PASSPORT_PET_LISTENING,  // the microphone is open
    PASSPORT_PET_THINKING,   // waiting on the model
    PASSPORT_PET_WORKING,    // an agent task is running
    PASSPORT_PET_SPEAKING,   // reply audio is playing
    PASSPORT_PET_HAPPY,      // a turn succeeded
    PASSPORT_PET_SLEEPING,   // standby
    PASSPORT_PET_DISTRESSED, // a failure, or a low battery
    PASSPORT_PET_MOOD_COUNT
} passport_pet_mood_t;

// Create the pet at (x, y) on parent, addressable as `name` from the console.
lv_obj_t *passport_pet_create(lv_obj_t *parent, int x, int y, const char *name);

// Display a mood. duration_ms > 0 makes it temporary: the pet falls back to
// PASSPORT_PET_IDLE by itself. duration_ms == 0 holds until changed.
void passport_pet_set_mood(passport_pet_mood_t mood, uint32_t duration_ms);

passport_pet_mood_t passport_pet_mood(void);
const char *passport_pet_mood_name(passport_pet_mood_t mood);

// Stop the animation. The caller must call this before deleting the screen.
void passport_pet_destroy(void);
