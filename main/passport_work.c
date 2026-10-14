// main/passport_work.c
#include "passport_work.h"

#include <stdio.h>
#include <string.h>

#include "passport_json.h"

const char *passport_work_state_name(passport_work_state_t state)
{
    switch (state) {
    case PASSPORT_WORK_IDLE:
        return "idle";
    case PASSPORT_WORK_RUNNING:
        return "running";
    case PASSPORT_WORK_WAITING:
        return "waiting";
    case PASSPORT_WORK_DONE:
        return "done";
    case PASSPORT_WORK_FAILED:
        return "failed";
    default:
        return "?";
    }
}

static passport_work_state_t state_from_text(const char *text)
{
    if (text == NULL) {
        return PASSPORT_WORK_IDLE;
    }
    if (strcmp(text, "running") == 0) {
        return PASSPORT_WORK_RUNNING;
    }
    if (strcmp(text, "waiting") == 0) {
        return PASSPORT_WORK_WAITING;
    }
    if (strcmp(text, "done") == 0) {
        return PASSPORT_WORK_DONE;
    }
    if (strcmp(text, "failed") == 0) {
        return PASSPORT_WORK_FAILED;
    }
    return PASSPORT_WORK_IDLE;
}

static void read_text(const char *json, size_t len, const char *key, char *out, size_t out_size)
{
    const passport_json_status_t status =
        passport_json_string(json, len, key, out, out_size, NULL);
    if (status == PASSPORT_JSON_NOT_FOUND) {
        out[0] = '\0';
    }
}

bool passport_work_parse(const char *json, passport_work_view_t *view)
{
    if (json == NULL || view == NULL) {
        return false;
    }

    passport_work_view_t next;
    memset(&next, 0, sizeof(next));
    next.progress = -1;
    next.state = PASSPORT_WORK_IDLE;

    const size_t len = passport_json_len(json);

    read_text(json, len, "task", next.task, sizeof(next.task));
    read_text(json, len, "question", next.question, sizeof(next.question));

    char state_text[16];
    read_text(json, len, "state", state_text, sizeof(state_text));
    next.state = state_from_text(state_text);

    bool had_progress = false;
    const int progress = passport_json_int(json, len, "progress", &had_progress);
    if (had_progress) {
        // Clamped rather than rejected: a progress bar that refuses an out-of-range number
        // would show nothing at all, which is worse than showing full.
        next.progress = (progress < 0) ? 0 : ((progress > 100) ? 100 : progress);
    }

    // Numbered option fields, up to what the panel can show.
    for (int i = 0; i < PASSPORT_WORK_MAX_OPTIONS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "option%d", i);
        char value[PASSPORT_WORK_OPTION_MAX];
        const passport_json_status_t status =
            passport_json_string(json, len, key, value, sizeof(value), NULL);
        if (status == PASSPORT_JSON_NOT_FOUND || value[0] == '\0') {
            break;   // the list ends at the first gap
        }
        memcpy(next.options[next.option_count], value, sizeof(next.options[0]));
        next.options[next.option_count][sizeof(next.options[0]) - 1u] = '\0';
        next.option_count++;
    }

    bool had_selected = false;
    const int selected = passport_json_int(json, len, "selected", &had_selected);
    if (had_selected && selected >= 0 && selected < next.option_count) {
        next.selected = selected;
    }

    // A document that names neither a task nor a question says nothing, and taking it would
    // wipe a screen that was showing something useful.
    if (next.task[0] == '\0' && next.question[0] == '\0') {
        return false;
    }
    if (next.question[0] == '\0') {
        next.option_count = 0;   // options without a question are not selectable
    }

    *view = next;
    return true;
}

passport_work_announcement_t passport_work_announce(bool changed,
                                                    passport_work_state_t current,
                                                    uint8_t notify_mode)
{
    // Silence is a setting, not a failure: mode 0 means the owner does not want to be interrupted.
    if (!changed || notify_mode == 0u) {
        return PASSPORT_WORK_ANNOUNCE_NONE;
    }

    switch (current) {
    case PASSPORT_WORK_WAITING:
        // Mode 2 asks for speech. The voice pipeline is not wired to work mode yet, so this is
        // reported as its own answer and the caller decides; pretending it spoke would be worse.
        return (notify_mode >= 2u) ? PASSPORT_WORK_ANNOUNCE_SPEAK
                                   : PASSPORT_WORK_ANNOUNCE_QUESTION;
    case PASSPORT_WORK_DONE:
        return PASSPORT_WORK_ANNOUNCE_DONE;
    case PASSPORT_WORK_FAILED:
        return PASSPORT_WORK_ANNOUNCE_FAILED;
    default:
        // Running and idle are not worth interrupting anyone for.
        return PASSPORT_WORK_ANNOUNCE_NONE;
    }
}

bool passport_work_awaiting_answer(const passport_work_view_t *view)
{
    return view != NULL && view->state == PASSPORT_WORK_WAITING && view->option_count > 0 &&
           !view->answered;
}

passport_work_action_t passport_work_key(passport_work_view_t *view, passport_work_key_t key)
{
    if (view == NULL) {
        return PASSPORT_WORK_ACT_NONE;
    }

    switch (key) {
    case PASSPORT_WORK_KEY_UP:
        if (view->option_count == 0) {
            return PASSPORT_WORK_ACT_NONE;
        }
        view->selected = (view->selected == 0) ? view->option_count - 1 : view->selected - 1;
        return PASSPORT_WORK_ACT_REDRAW;

    case PASSPORT_WORK_KEY_DOWN:
        if (view->option_count == 0) {
            return PASSPORT_WORK_ACT_NONE;
        }
        view->selected = (view->selected + 1) % view->option_count;
        return PASSPORT_WORK_ACT_REDRAW;

    case PASSPORT_WORK_KEY_CONFIRM:
        // Confirm only means something while a question is waiting and has options. On a
        // running task it does nothing, so a stray press cannot send a meaningless answer.
        if (!passport_work_awaiting_answer(view)) {
            return PASSPORT_WORK_ACT_NONE;
        }
        view->answered = true;
        return PASSPORT_WORK_ACT_SEND_ANSWER;

    case PASSPORT_WORK_KEY_NONE:
    default:
        return PASSPORT_WORK_ACT_NONE;
    }
}
