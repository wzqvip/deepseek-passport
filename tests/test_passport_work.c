// Host tests for work mode's document parsing and button rules.
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "passport_work.h"

static const char *RUNNING =
    "{\"task\":\"indexing repository\",\"progress\":42,\"state\":\"running\"}";

static const char *WAITING =
    "{\"task\":\"deploy\",\"state\":\"waiting\",\"question\":\"Which branch?\","
    "\"option0\":\"main\",\"option1\":\"develop\",\"option2\":\"release\",\"selected\":1}";

int main(void)
{
    passport_work_view_t view;

    // --- a running task -----------------------------------------------------
    assert(passport_work_parse(RUNNING, &view));
    assert(strcmp(view.task, "indexing repository") == 0);
    assert(view.progress == 42);
    assert(view.state == PASSPORT_WORK_RUNNING);
    assert(view.option_count == 0);
    assert(view.question[0] == '\0');
    assert(!passport_work_awaiting_answer(&view));

    // --- a question with options -------------------------------------------
    assert(passport_work_parse(WAITING, &view));
    assert(view.state == PASSPORT_WORK_WAITING);
    assert(strcmp(view.question, "Which branch?") == 0);
    assert(view.option_count == 3);
    assert(strcmp(view.options[0], "main") == 0);
    assert(strcmp(view.options[2], "release") == 0);
    assert(view.selected == 1);
    assert(passport_work_awaiting_answer(&view));

    // --- a missing progress is not a progress of zero ----------------------
    // This is the distinction the `found` flag exists for: a bridge that omits progress must
    // not make the screen claim the task is stalled at nothing.
    assert(passport_work_parse("{\"task\":\"x\",\"state\":\"running\"}", &view));
    assert(view.progress == -1);
    assert(passport_work_parse("{\"task\":\"x\",\"progress\":0}", &view));
    assert(view.progress == 0);

    // --- out of range is clamped, not rejected -----------------------------
    assert(passport_work_parse("{\"task\":\"x\",\"progress\":250}", &view));
    assert(view.progress == 100);
    assert(passport_work_parse("{\"task\":\"x\",\"progress\":-5}", &view));
    assert(view.progress == 0);
    // A non-integer progress reads as absent.
    assert(passport_work_parse("{\"task\":\"x\",\"progress\":\"half\"}", &view));
    assert(view.progress == -1);

    // --- the option list ends at the first gap ------------------------------
    // A gap means "no more options", so a stray option3 cannot appear on its own.
    assert(passport_work_parse("{\"question\":\"q\",\"state\":\"waiting\",\"option0\":\"a\","
                               "\"option2\":\"c\"}", &view));
    assert(view.option_count == 1);

    // Empty options do not count either.
    assert(passport_work_parse("{\"question\":\"q\",\"state\":\"waiting\",\"option0\":\"\","
                               "\"option1\":\"b\"}", &view));
    assert(view.option_count == 0);

    // --- a selected index outside the list is ignored -----------------------
    assert(passport_work_parse("{\"question\":\"q\",\"option0\":\"a\",\"selected\":9}",
                               &view));
    assert(view.option_count == 1);
    assert(view.selected == 0);
    assert(passport_work_parse("{\"question\":\"q\",\"option0\":\"a\",\"selected\":-1}",
                               &view));
    assert(view.selected == 0);

    // --- options without a question are not selectable ----------------------
    assert(passport_work_parse("{\"task\":\"t\",\"option0\":\"a\",\"option1\":\"b\"}", &view));
    assert(view.option_count == 0);

    // --- states ------------------------------------------------------------
    assert(passport_work_parse("{\"task\":\"t\",\"state\":\"done\"}", &view));
    assert(view.state == PASSPORT_WORK_DONE);
    assert(passport_work_parse("{\"task\":\"t\",\"state\":\"failed\"}", &view));
    assert(view.state == PASSPORT_WORK_FAILED);
    assert(passport_work_parse("{\"task\":\"t\",\"state\":\"nonsense\"}", &view));
    assert(view.state == PASSPORT_WORK_IDLE);
    assert(passport_work_parse("{\"task\":\"t\"}", &view));
    assert(view.state == PASSPORT_WORK_IDLE);

    // --- a document that says nothing must not wipe the screen --------------
    passport_work_view_t kept;
    assert(passport_work_parse(WAITING, &kept));
    assert(!passport_work_parse("{}", &kept));
    assert(strcmp(kept.question, "Which branch?") == 0);   // unchanged
    assert(!passport_work_parse("", &kept));
    assert(!passport_work_parse("not json at all", &kept));
    assert(strcmp(kept.question, "Which branch?") == 0);
    assert(!passport_work_parse(NULL, &kept));
    assert(!passport_work_parse(WAITING, NULL));

    // --- button navigation --------------------------------------------------
    assert(passport_work_parse(WAITING, &view));
    assert(passport_work_key(&view, PASSPORT_WORK_KEY_DOWN) == PASSPORT_WORK_ACT_REDRAW);
    assert(view.selected == 2);
    assert(passport_work_key(&view, PASSPORT_WORK_KEY_DOWN) == PASSPORT_WORK_ACT_REDRAW);
    assert(view.selected == 0);   // wraps
    assert(passport_work_key(&view, PASSPORT_WORK_KEY_UP) == PASSPORT_WORK_ACT_REDRAW);
    assert(view.selected == 2);   // wraps the other way

    // Confirm sends the answer once, and only while a question is waiting.
    assert(passport_work_key(&view, PASSPORT_WORK_KEY_CONFIRM) ==
           PASSPORT_WORK_ACT_SEND_ANSWER);
    assert(view.answered);
    assert(!passport_work_awaiting_answer(&view));
    // A second press does nothing: one question, one answer.
    assert(passport_work_key(&view, PASSPORT_WORK_KEY_CONFIRM) == PASSPORT_WORK_ACT_NONE);
    // Moving the selection after answering is still allowed, but confirming is not.
    assert(passport_work_key(&view, PASSPORT_WORK_KEY_DOWN) == PASSPORT_WORK_ACT_REDRAW);
    assert(passport_work_key(&view, PASSPORT_WORK_KEY_CONFIRM) == PASSPORT_WORK_ACT_NONE);

    // --- confirm on a running task does nothing -----------------------------
    assert(passport_work_parse(RUNNING, &view));
    assert(!passport_work_awaiting_answer(&view));
    assert(passport_work_key(&view, PASSPORT_WORK_KEY_CONFIRM) == PASSPORT_WORK_ACT_NONE);
    assert(!view.answered);

    // --- navigation with nothing to choose from -----------------------------
    assert(passport_work_key(&view, PASSPORT_WORK_KEY_UP) == PASSPORT_WORK_ACT_NONE);
    assert(passport_work_key(&view, PASSPORT_WORK_KEY_DOWN) == PASSPORT_WORK_ACT_NONE);

    // --- a waiting state with no options cannot be answered -----------------
    assert(passport_work_parse("{\"question\":\"q\",\"state\":\"waiting\"}", &view));
    assert(view.state == PASSPORT_WORK_WAITING);
    assert(!passport_work_awaiting_answer(&view));
    assert(passport_work_key(&view, PASSPORT_WORK_KEY_CONFIRM) == PASSPORT_WORK_ACT_NONE);

    // --- argument handling --------------------------------------------------
    assert(passport_work_key(NULL, PASSPORT_WORK_KEY_DOWN) == PASSPORT_WORK_ACT_NONE);
    assert(passport_work_key(&view, PASSPORT_WORK_KEY_NONE) == PASSPORT_WORK_ACT_NONE);
    assert(!passport_work_awaiting_answer(NULL));

    // --- what deserves a sound ----------------------------------------------
    // A poll runs every few seconds, so "nothing" has to be the common answer.
    assert(passport_work_announce(false, PASSPORT_WORK_WAITING, 1u) ==
           PASSPORT_WORK_ANNOUNCE_NONE);
    assert(passport_work_announce(true, PASSPORT_WORK_RUNNING, 1u) ==
           PASSPORT_WORK_ANNOUNCE_NONE);
    assert(passport_work_announce(true, PASSPORT_WORK_IDLE, 1u) ==
           PASSPORT_WORK_ANNOUNCE_NONE);

    // A question and a finished task are worth interrupting for; running is not.
    assert(passport_work_announce(true, PASSPORT_WORK_WAITING, 1u) ==
           PASSPORT_WORK_ANNOUNCE_QUESTION);
    assert(passport_work_announce(true, PASSPORT_WORK_DONE, 1u) ==
           PASSPORT_WORK_ANNOUNCE_DONE);
    assert(passport_work_announce(true, PASSPORT_WORK_FAILED, 1u) ==
           PASSPORT_WORK_ANNOUNCE_FAILED);

    // Silent mode means silent, whatever happened.
    for (int s = 0; s <= (int)PASSPORT_WORK_FAILED; s++) {
        assert(passport_work_announce(true, (passport_work_state_t)s, 0u) ==
               PASSPORT_WORK_ANNOUNCE_NONE);
    }

    // Speaking is asked for but not wired, and it is reported as such rather than as a chime.
    assert(passport_work_announce(true, PASSPORT_WORK_WAITING, 2u) ==
           PASSPORT_WORK_ANNOUNCE_SPEAK);
    assert(passport_work_announce(true, PASSPORT_WORK_WAITING, 9u) ==
           PASSPORT_WORK_ANNOUNCE_SPEAK);
    // A finished task is a chime even in speak mode: there is nothing to say about it.
    assert(passport_work_announce(true, PASSPORT_WORK_DONE, 2u) ==
           PASSPORT_WORK_ANNOUNCE_DONE);

    // --- names --------------------------------------------------------------
    assert(strcmp(passport_work_state_name(PASSPORT_WORK_WAITING), "waiting") == 0);
    assert(strcmp(passport_work_state_name(PASSPORT_WORK_DONE), "done") == 0);
    assert(strlen(passport_work_state_name((passport_work_state_t)99)) > 0);

    return 0;
}
