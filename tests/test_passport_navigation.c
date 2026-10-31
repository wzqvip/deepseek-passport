#include <assert.h>
#include <stdio.h>
#include "passport_navigation.h"

static passport_nav_action_t key(passport_navigation_t *n, passport_nav_key_t k, passport_nav_event_t e)
{
    return passport_navigation_key(n, k, e, false);
}
int main(void)
{
    passport_navigation_t n = {0};
    assert(key(&n, PASSPORT_NAV_UP, PASSPORT_NAV_PRESS) == PASSPORT_NAV_NONE);
    assert(key(&n, PASSPORT_NAV_UP, PASSPORT_NAV_LONG) == PASSPORT_NAV_SETTINGS);
    passport_navigation_show(&n, PASSPORT_PAGE_SETTINGS);
    assert(key(&n, PASSPORT_NAV_UP, PASSPORT_NAV_RELEASE) == PASSPORT_NAV_NONE);
    assert(key(&n, PASSPORT_NAV_UP, PASSPORT_NAV_CLICK) == PASSPORT_NAV_NONE);
    assert(n.page == PASSPORT_PAGE_SETTINGS);
    assert(!passport_navigation_accept_work(&n, true, true));
    assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_PRESS) == PASSPORT_NAV_NONE);
    assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_CLICK) == PASSPORT_NAV_CONFIRM);
    passport_navigation_show(&n, PASSPORT_PAGE_SETUP);
    assert(!passport_navigation_accept_work(&n, true, true));
    assert(key(&n, PASSPORT_NAV_DOWN, PASSPORT_NAV_CLICK) == PASSPORT_NAV_NONE);
    assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_LONG) == PASSPORT_NAV_BACK);
    assert(passport_navigation_back(&n) == PASSPORT_PAGE_SETTINGS);
    passport_navigation_show(&n, passport_navigation_back(&n));
    assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_RELEASE) == PASSPORT_NAV_NONE);
    assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_CLICK) == PASSPORT_NAV_NONE);
    key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_PRESS);
    assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_LONG) == PASSPORT_NAV_BACK);
    passport_navigation_show(&n, PASSPORT_PAGE_HOME);
    assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_RELEASE) == PASSPORT_NAV_NONE);
    assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_PRESS) == PASSPORT_NAV_TALK_DOWN);
    assert(!passport_navigation_accept_work(&n, true, true));
    passport_navigation_show(&n, PASSPORT_PAGE_WORK);
    assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_RELEASE) == PASSPORT_NAV_TALK_UP);
    for (int page = PASSPORT_PAGE_HOME; page <= PASSPORT_PAGE_WORK; ++page) {
        passport_navigation_show(&n, (passport_page_t)page);
        // Waking the screen with OK starts talking on the one page that has a talk button, and stays
        // silent everywhere else: the press that lights the screen must not also act on a menu.
        const passport_nav_action_t woke_press =
            passport_navigation_key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_PRESS, true);
        assert(woke_press == (page == PASSPORT_PAGE_HOME ? PASSPORT_NAV_TALK_DOWN
                                                         : PASSPORT_NAV_NONE));
        if (woke_press == PASSPORT_NAV_TALK_DOWN) {
            // The release that ends that turn is honoured, whatever page is showing by then.
            passport_navigation_show(&n, PASSPORT_PAGE_WORK);
            assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_RELEASE) == PASSPORT_NAV_TALK_UP);
        }
        key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_PRESS);
        key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_RELEASE);
        passport_navigation_show(&n, (passport_page_t)page);
        // A waking click — the tail of a press already consumed — must not select anything either.
        assert(passport_navigation_key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_CLICK, true) ==
               PASSPORT_NAV_NONE);
        assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_RELEASE) == PASSPORT_NAV_NONE);
        assert(!n.talking);
    }
    passport_navigation_show(&n, PASSPORT_PAGE_HOME);
    assert(passport_navigation_accept_work(&n, true, true));
    assert(!passport_navigation_accept_work(&n, false, true));
    assert(!passport_navigation_accept_work(&n, true, false));
    assert(key(&n, PASSPORT_NAV_DOWN, PASSPORT_NAV_LONG) == PASSPORT_NAV_WORK);
    for (int i = 0; i <= 255; ++i) {
        uint8_t b = passport_navigation_brightness((uint8_t)i);
        assert(b == 40 || b == 70 || b == 100);
    }
    assert(passport_navigation_brightness(40) == 70);
    assert(passport_navigation_brightness(70) == 100);
    assert(passport_navigation_brightness(100) == 40);
    passport_navigation_show(&n, PASSPORT_PAGE_HOME);
    assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_PRESS) == PASSPORT_NAV_TALK_DOWN);
    uint32_t before = n.generation;
    assert(passport_navigation_cancel(&n));
    assert(n.generation != before);
    assert(!n.talking);
    assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_RELEASE) == PASSPORT_NAV_NONE);
    assert(key(&n, PASSPORT_NAV_UP, PASSPORT_NAV_LONG) == PASSPORT_NAV_NONE);
    assert(key(&n, PASSPORT_NAV_DOWN, PASSPORT_NAV_CLICK) == PASSPORT_NAV_NONE);
    assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_PRESS) == PASSPORT_NAV_TALK_DOWN);
    assert(key(&n, PASSPORT_NAV_OK, PASSPORT_NAV_RELEASE) == PASSPORT_NAV_TALK_UP);
    puts("passport_navigation: PASS");
    return 0;
}
