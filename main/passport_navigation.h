#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { PASSPORT_PAGE_HOME, PASSPORT_PAGE_SETTINGS, PASSPORT_PAGE_SETUP,
               PASSPORT_PAGE_WORK } passport_page_t;
typedef enum { PASSPORT_NAV_UP, PASSPORT_NAV_DOWN, PASSPORT_NAV_OK } passport_nav_key_t;
typedef enum { PASSPORT_NAV_PRESS, PASSPORT_NAV_CLICK, PASSPORT_NAV_DOUBLE,
               PASSPORT_NAV_LONG, PASSPORT_NAV_RELEASE } passport_nav_event_t;
typedef enum {
    PASSPORT_NAV_NONE, PASSPORT_NAV_SETTINGS, PASSPORT_NAV_WORK, PASSPORT_NAV_BACK,
    PASSPORT_NAV_PREVIOUS, PASSPORT_NAV_NEXT, PASSPORT_NAV_CONFIRM,
    PASSPORT_NAV_TALK_DOWN, PASSPORT_NAV_TALK_UP, PASSPORT_NAV_SLEEP
} passport_nav_action_t;
typedef struct {
    passport_page_t page;
    passport_page_t setup_parent;
    bool talking;
    unsigned suppressed; /* Swallow the rest of a wake/transition gesture until a fresh press. */
    uint32_t generation;
} passport_navigation_t;
void passport_navigation_show(passport_navigation_t *nav, passport_page_t page);
// Recover a broken input sequence. Returns whether a talk gesture was active.
bool passport_navigation_cancel(passport_navigation_t *nav);
passport_page_t passport_navigation_back(const passport_navigation_t *nav);
passport_nav_action_t passport_navigation_key(passport_navigation_t *nav,
    passport_nav_key_t key, passport_nav_event_t event, bool woke);
bool passport_navigation_accept_work(const passport_navigation_t *nav, bool changed, bool relevant);
uint8_t passport_navigation_brightness(uint8_t current);
