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
    read_text(json, len, "preview", next.preview, sizeof(next.preview));
    const passport_json_status_t id_status = passport_json_string(
        json, len, "question_id", next.question_id, sizeof(next.question_id), NULL);
    if (id_status != PASSPORT_JSON_OK && id_status != PASSPORT_JSON_NOT_FOUND) {
        return false; // A truncated ID could answer a different question.
    }

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
        // The machine name of the same option, when the publisher supplied one. Absent is normal: a
        // question typed at the console has labels but no action names, and the label is then the answer.
        char action_key[16];
        snprintf(action_key, sizeof(action_key), "action%d", i);
        read_text(json, len, action_key, next.actions[next.option_count],
                  sizeof(next.actions[0]));
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

bool passport_work_same_question(const passport_work_view_t *a, const passport_work_view_t *b)
{
    if (a == NULL || b == NULL || a->state != b->state ||
        strcmp(a->task, b->task) != 0 || strcmp(a->question_id, b->question_id) != 0 ||
        strcmp(a->question, b->question) != 0 || a->option_count != b->option_count) {
        return false;
    }
    // The preview is compared too: it is the line the owner reads, so a new answer with the same question
    // has to reach the screen even when nothing else about the question changed.
    if (strcmp(a->preview, b->preview) != 0) {
        return false;
    }
    for (int i = 0; i < a->option_count; ++i) {
        if (strcmp(a->options[i], b->options[i]) != 0 ||
            strcmp(a->actions[i], b->actions[i]) != 0) {
            return false;
        }
    }
    return true;
}

const char *passport_work_selected_action(const passport_work_view_t *view)
{
    if (view == NULL || view->selected < 0 || view->selected >= view->option_count) {
        return NULL;
    }
    return (view->actions[view->selected][0] != '\0') ? view->actions[view->selected] : NULL;
}

static bool append_json_string(const char *text, char *out, size_t cap, size_t *used)
{
    static const char hex[] = "0123456789abcdef";
    if (*used + 1 >= cap) return false;
    out[(*used)++] = '"';
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        const size_t n = (*p < 0x20) ? 6u : ((*p == '"' || *p == '\\') ? 2u : 1u);
        if (*used + n >= cap) return false;
        if (*p < 0x20) {
            out[(*used)++] = '\\'; out[(*used)++] = 'u';
            out[(*used)++] = '0'; out[(*used)++] = '0';
            out[(*used)++] = hex[*p >> 4]; out[(*used)++] = hex[*p & 15];
        } else {
            if (n == 2) out[(*used)++] = '\\';
            out[(*used)++] = (char)*p;
        }
    }
    if (*used + 1 >= cap) return false;
    out[(*used)++] = '"';
    out[*used] = '\0';
    return true;
}

bool passport_work_choice_json(const char *question_id, const char *answer, const char *action,
                               char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return false;
    out[0] = '\0';
    const bool has_answer = (answer != NULL && answer[0] != '\0');
    const bool has_action = (action != NULL && action[0] != '\0');
    if (!has_answer && !has_action) return false;
    if (out_size < 48) return false;

    const char *id = question_id != NULL ? question_id : "";
    size_t used = 0;
    const char prefix[] = "{\"question_id\":";
    memcpy(out, prefix, sizeof(prefix) - 1);
    used = sizeof(prefix) - 1;
    if (!append_json_string(id, out, out_size, &used)) goto fail;
    if (has_answer) {
        const char middle[] = ",\"answer\":";
        if (used + sizeof(middle) >= out_size) goto fail;
        memcpy(out + used, middle, sizeof(middle) - 1);
        used += sizeof(middle) - 1;
        if (!append_json_string(answer, out, out_size, &used)) goto fail;
    }
    if (has_action) {
        // The machine name of the choice. The receiving program switches on this, so a renamed button does
        // not change behaviour; the label travels beside it for logs and for humans.
        const char action_prefix[] = ",\"action\":";
        if (used + sizeof(action_prefix) >= out_size) goto fail;
        memcpy(out + used, action_prefix, sizeof(action_prefix) - 1);
        used += sizeof(action_prefix) - 1;
        if (!append_json_string(action, out, out_size, &used)) goto fail;
    }
    if (used + 1 >= out_size) goto fail;
    out[used++] = '}';
    out[used] = '\0';
    return true;
fail:
    out[0] = '\0';
    return false;
}

bool passport_work_answer_json(const char *question_id, const char *answer,
                               char *out, size_t out_size)
{
    return passport_work_choice_json(question_id, answer, NULL, out, out_size);
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
