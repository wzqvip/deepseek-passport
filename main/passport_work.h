// main/passport_work.h
// Work mode: showing what the desktop session is doing, and answering its questions from the
// device's buttons.
//
// The device is a thin terminal here. The companion bridge owns the session; this side reads a
// small status document and turns button presses into one of a few answers. Two parts are kept
// pure so they can be host-tested: parsing that document, and the selection rules.
//
// **The status document is deliberately flat:**
//
//   {"task":"indexing","progress":42,"state":"running",
//    "question":"Which branch?","option0":"main","option1":"develop","selected":0}
//
// Options are numbered fields rather than a JSON array. That is a deliberate contract with the
// bridge, not an oversight: the device's reader is a few hundred bytes instead of a parser, and
// numbered fields need no delimiter escaping, so a branch name containing a comma or a quote
// cannot corrupt the list. Up to four options are read, which is what the panel can show with a
// highlighted row.
#pragma once

#include <stdbool.h>

#define PASSPORT_WORK_MAX_OPTIONS 4
#define PASSPORT_WORK_TASK_MAX    64
#define PASSPORT_WORK_QUESTION_MAX 96
#define PASSPORT_WORK_OPTION_MAX  48

typedef enum {
    PASSPORT_WORK_IDLE = 0,     // nothing is running
    PASSPORT_WORK_RUNNING,
    PASSPORT_WORK_WAITING,      // a question is waiting for an answer
    PASSPORT_WORK_DONE,
    PASSPORT_WORK_FAILED,
} passport_work_state_t;

typedef struct {
    char task[PASSPORT_WORK_TASK_MAX];
    char question[PASSPORT_WORK_QUESTION_MAX];
    char options[PASSPORT_WORK_MAX_OPTIONS][PASSPORT_WORK_OPTION_MAX];
    int option_count;
    int selected;
    int progress;               // 0 to 100, or -1 when the document did not say
    passport_work_state_t state;
    bool answered;              // an answer has been sent for the current question
} passport_work_view_t;

typedef enum {
    PASSPORT_WORK_KEY_NONE = 0,
    PASSPORT_WORK_KEY_UP,
    PASSPORT_WORK_KEY_DOWN,
    PASSPORT_WORK_KEY_CONFIRM,
} passport_work_key_t;

typedef enum {
    PASSPORT_WORK_ACT_NONE = 0,
    PASSPORT_WORK_ACT_REDRAW,      // something changed on screen
    PASSPORT_WORK_ACT_CHIME,       // the state changed in a way worth a sound
    PASSPORT_WORK_ACT_SEND_ANSWER, // post the selected option back to the bridge
} passport_work_action_t;

// Parse a status document into `view`, replacing whatever was there. A missing or malformed
// field becomes a default rather than a failure: a bridge that grows a new field, or omits one,
// must not blank the screen. Returns false only when the document has no usable task and no
// question, which is a document that says nothing.
bool passport_work_parse(const char *json, passport_work_view_t *view);

// Apply one button press. Returns what the caller should do.
passport_work_action_t passport_work_key(passport_work_view_t *view, passport_work_key_t key);

const char *passport_work_state_name(passport_work_state_t state);

// Whether the device is showing a question the owner can answer.
bool passport_work_awaiting_answer(const passport_work_view_t *view);
