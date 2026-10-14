// main/passport_pet.c
#include "passport_pet.h"

#include <stdbool.h>
#include <stddef.h>

#include "passport_pet_assets.h"

// Animation cadence. 90 ms gives ~11 frames/s, which reads as a deliberate pixel
// animation rather than a smooth but expensive one.
#define PET_TICK_MS 90
#define PET_BLINK_EVERY_TICKS 26   // roughly every 2.3 s while idle
#define PET_BOB_TICKS 4            // breathing period in ticks

static lv_obj_t *s_pet;
static lv_timer_t *s_timer;
static passport_pet_mood_t s_mood = PASSPORT_PET_IDLE;
static uint32_t s_mood_remaining_ms;
static uint32_t s_tick;
static int s_base_x;
static int s_base_y;

static const char *const MOOD_NAMES[PASSPORT_PET_MOOD_COUNT] = {
    [PASSPORT_PET_IDLE] = "idle",
    [PASSPORT_PET_CURIOUS] = "curious",
    [PASSPORT_PET_LISTENING] = "listening",
    [PASSPORT_PET_THINKING] = "thinking",
    [PASSPORT_PET_WORKING] = "working",
    [PASSPORT_PET_SPEAKING] = "speaking",
    [PASSPORT_PET_HAPPY] = "happy",
    [PASSPORT_PET_SLEEPING] = "sleeping",
    [PASSPORT_PET_DISTRESSED] = "distressed",
};

const char *passport_pet_mood_name(passport_pet_mood_t mood)
{
    if (mood < 0 || mood >= PASSPORT_PET_MOOD_COUNT) {
        return "?";
    }
    return MOOD_NAMES[mood];
}

passport_pet_mood_t passport_pet_mood(void)
{
    return s_mood;
}

static void show(passport_pet_frame_t frame)
{
    lv_image_set_src(s_pet, passport_pet_frames[frame]);
}

// One visual step. Runs in the LVGL timer context, so it touches objects directly
// and must never block: no I/O, no allocation, no logging.
static void pet_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_pet == NULL) {
        return;
    }

    s_tick++;

    if (s_mood != PASSPORT_PET_IDLE && s_mood_remaining_ms > 0u) {
        if (s_mood_remaining_ms <= PET_TICK_MS) {
            s_mood_remaining_ms = 0u;
            s_mood = PASSPORT_PET_IDLE;
        } else {
            s_mood_remaining_ms -= PET_TICK_MS;
        }
    }

    int offset_y = 0;
    int offset_x = 0;
    passport_pet_frame_t frame;

    switch (s_mood) {
    case PASSPORT_PET_IDLE:
        // Two-frame breathing bob with an occasional blink.
        frame = ((s_tick / PET_BOB_TICKS) % 2u == 0u) ? PASSPORT_PET_FRAME_IDLE_A
                                                     : PASSPORT_PET_FRAME_IDLE_B;
        if ((s_tick % PET_BLINK_EVERY_TICKS) == 0u) {
            frame = PASSPORT_PET_FRAME_BLINK;
        } else if (frame == PASSPORT_PET_FRAME_IDLE_B) {
            offset_y = 1;
        }
        break;
    case PASSPORT_PET_WORKING:
        // Tail up while a task runs, with a slow bob so it reads as active.
        frame = PASSPORT_PET_FRAME_WORKING;
        offset_y = ((s_tick / PET_BOB_TICKS) % 2u == 0u) ? 0 : 1;
        break;
    case PASSPORT_PET_SPEAKING:
        // Spout on the even beat, open mouth on the odd one.
        frame = ((s_tick / 2u) % 2u == 0u) ? PASSPORT_PET_FRAME_SPEAKING
                                           : PASSPORT_PET_FRAME_HAPPY;
        break;
    case PASSPORT_PET_HAPPY:
        frame = PASSPORT_PET_FRAME_HAPPY;
        offset_y = ((s_tick / 2u) % 2u == 0u) ? 0 : -2;
        break;
    case PASSPORT_PET_DISTRESSED:
        frame = PASSPORT_PET_FRAME_DISTRESSED;
        offset_y = ((s_tick / 3u) % 2u == 0u) ? 0 : 2;
        offset_x = ((s_tick / 3u) % 2u == 0u) ? 0 : -1;
        break;
    case PASSPORT_PET_CURIOUS:
        frame = PASSPORT_PET_FRAME_CURIOUS;
        break;
    case PASSPORT_PET_LISTENING:
        frame = PASSPORT_PET_FRAME_LISTENING;
        break;
    case PASSPORT_PET_THINKING:
        frame = PASSPORT_PET_FRAME_THINKING;
        break;
    case PASSPORT_PET_SLEEPING:
        frame = PASSPORT_PET_FRAME_SLEEPING;
        break;
    default:
        frame = PASSPORT_PET_FRAME_IDLE_A;
        break;
    }

    show(frame);
    lv_obj_set_pos(s_pet, s_base_x + offset_x, s_base_y + offset_y);
}

lv_obj_t *passport_pet_create(lv_obj_t *parent, int x, int y, const char *name)
{
    s_pet = lv_image_create(parent);
    s_base_x = x;
    s_base_y = y;
    lv_obj_set_pos(s_pet, x, y);
    lv_obj_remove_flag(s_pet, LV_OBJ_FLAG_SCROLLABLE);
    if (name != NULL) {
        lv_obj_set_name(s_pet, name);
    }
    show(PASSPORT_PET_FRAME_IDLE_A);

    s_mood = PASSPORT_PET_IDLE;
    s_mood_remaining_ms = 0u;
    s_tick = 0u;
    s_timer = lv_timer_create(pet_tick, PET_TICK_MS, NULL);
    if (s_timer == NULL) {
        LV_LOG_WARN("passport_pet: animation timer unavailable; the pet will not animate");
    }
    return s_pet;
}

void passport_pet_set_mood(passport_pet_mood_t mood, uint32_t duration_ms)
{
    if (mood < 0 || mood >= PASSPORT_PET_MOOD_COUNT) {
        return;
    }
    s_mood = mood;
    s_mood_remaining_ms = duration_ms;
    // React immediately rather than on the next tick, so a press feels answered.
    s_tick = 0u;
}

void passport_pet_destroy(void)
{
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_pet = NULL;
}
