#include "passport_navigation.h"

void passport_navigation_show(passport_navigation_t *nav, passport_page_t page)
{
    if (page == PASSPORT_PAGE_SETUP && nav->page != PASSPORT_PAGE_SETUP)
        nav->setup_parent = nav->page == PASSPORT_PAGE_SETTINGS ? PASSPORT_PAGE_SETTINGS : PASSPORT_PAGE_HOME;
    if (nav->page != page) nav->generation++;
    nav->page = page;
}

bool passport_navigation_cancel(passport_navigation_t *nav)
{
    const bool talking = nav->talking;
    nav->talking = false;
    nav->suppressed = (1u << (PASSPORT_NAV_OK + 1)) - 1u;
    nav->generation++;
    return talking;
}

passport_page_t passport_navigation_back(const passport_navigation_t *nav)
{
    return nav->page == PASSPORT_PAGE_SETUP ? nav->setup_parent : PASSPORT_PAGE_HOME;
}

passport_nav_action_t passport_navigation_key(passport_navigation_t *nav,
    passport_nav_key_t key, passport_nav_event_t event, bool woke)
{
    if ((unsigned)key > PASSPORT_NAV_OK || (unsigned)event > PASSPORT_NAV_RELEASE) return PASSPORT_NAV_NONE;
    unsigned bit = 1u << key;
    if (event == PASSPORT_NAV_PRESS) nav->suppressed &= ~bit;
    /* A recording already started must always receive its release, even after a page change. */
    if (key == PASSPORT_NAV_OK && event == PASSPORT_NAV_RELEASE && nav->talking) {
        nav->talking = false;
        return PASSPORT_NAV_TALK_UP;
    }
    if (woke) nav->suppressed |= bit;
    if (nav->suppressed & bit) return PASSPORT_NAV_NONE;
    if (event == PASSPORT_NAV_LONG && key == PASSPORT_NAV_OK && nav->page != PASSPORT_PAGE_HOME) {
        nav->suppressed |= bit;
        return PASSPORT_NAV_BACK;
    }
    if (nav->page == PASSPORT_PAGE_SETUP) return PASSPORT_NAV_NONE;
    if (nav->page != PASSPORT_PAGE_HOME) {
        if (event != PASSPORT_NAV_CLICK) return PASSPORT_NAV_NONE;
        return key == PASSPORT_NAV_UP ? PASSPORT_NAV_PREVIOUS :
               key == PASSPORT_NAV_DOWN ? PASSPORT_NAV_NEXT : PASSPORT_NAV_CONFIRM;
    }
    if (event == PASSPORT_NAV_LONG && key != PASSPORT_NAV_OK && !nav->talking) {
        nav->suppressed |= bit;
        return key == PASSPORT_NAV_UP ? PASSPORT_NAV_SETTINGS : PASSPORT_NAV_WORK;
    }
    if (key == PASSPORT_NAV_OK && event == PASSPORT_NAV_PRESS) {
        nav->talking = true;
        return PASSPORT_NAV_TALK_DOWN;
    }
    if (key == PASSPORT_NAV_UP && event == PASSPORT_NAV_DOUBLE && !nav->talking)
        return PASSPORT_NAV_SLEEP;
    if (event == PASSPORT_NAV_CLICK && key != PASSPORT_NAV_OK)
        return key == PASSPORT_NAV_UP ? PASSPORT_NAV_PREVIOUS : PASSPORT_NAV_NEXT;
    return PASSPORT_NAV_NONE;
}

bool passport_navigation_accept_work(const passport_navigation_t *nav, bool changed, bool relevant)
{
    return nav->page == PASSPORT_PAGE_HOME && !nav->talking && changed && relevant;
}

uint8_t passport_navigation_brightness(uint8_t current)
{
    return current < 40 ? 40 : current < 70 ? 70 : current < 100 ? 100 : 40;
}
