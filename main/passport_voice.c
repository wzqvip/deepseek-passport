// main/passport_voice.c
#include "passport_voice.h"

#include <stddef.h>

void passport_voice_init(passport_voice_t *voice)
{
    if (voice == NULL) {
        return;
    }
    voice->state = PASSPORT_VOICE_IDLE;
    voice->mic_open = false;
    voice->turns = 0u;
}

const char *passport_voice_state_name(passport_voice_state_t state)
{
    switch (state) {
    case PASSPORT_VOICE_IDLE:
        return "idle";
    case PASSPORT_VOICE_LISTENING:
        return "listening";
    case PASSPORT_VOICE_TRANSCRIBING:
        return "transcribing";
    case PASSPORT_VOICE_THINKING:
        return "thinking";
    case PASSPORT_VOICE_SPEAKING:
        return "speaking";
    case PASSPORT_VOICE_FAILED:
        return "failed";
    default:
        return "?";
    }
}

bool passport_voice_busy(const passport_voice_t *voice)
{
    return voice != NULL && voice->state != PASSPORT_VOICE_IDLE &&
           voice->state != PASSPORT_VOICE_FAILED;
}

// Close the microphone if this machine believes it is open. Every path that leaves the
// listening state goes through here, which is what keeps the invariant in one place instead of
// repeated at each transition. It reports nothing: the action that follows already means "the
// microphone is closed and the audio is on its way".
static void close_mic(passport_voice_t *voice)
{
    voice->mic_open = false;
}

static void to_idle(passport_voice_t *voice)
{
    close_mic(voice);   // defensive: idle must never leave the microphone open
    voice->state = PASSPORT_VOICE_IDLE;
}

passport_voice_action_t passport_voice_handle(passport_voice_t *voice, passport_voice_event_t event)
{
    if (voice == NULL) {
        return PASSPORT_VOICE_ACT_NONE;
    }

    // A reset is honoured from anywhere, and is the documented way out of a failure.
    if (event == PASSPORT_VOICE_EV_RESET) {
        to_idle(voice);
        return PASSPORT_VOICE_ACT_RETURN_IDLE;
    }

    // From a failed state only a reset or a fresh press is meaningful. A press starts a new
    // turn, so a failure does not leave the device needing a reboot to talk again.
    if (voice->state == PASSPORT_VOICE_FAILED && event == PASSPORT_VOICE_EV_TALK_PRESS) {
        voice->state = PASSPORT_VOICE_IDLE;
    }

    switch (voice->state) {
    case PASSPORT_VOICE_IDLE:
        if (event == PASSPORT_VOICE_EV_TALK_PRESS) {
            voice->state = PASSPORT_VOICE_LISTENING;
            voice->mic_open = true;
            return PASSPORT_VOICE_ACT_OPEN_MIC;
        }
        // Anything else in idle is noise: a stray transcript, a late playback callback.
        return PASSPORT_VOICE_ACT_NONE;

    case PASSPORT_VOICE_LISTENING:
        if (event == PASSPORT_VOICE_EV_TALK_RELEASE || event == PASSPORT_VOICE_EV_RECORDING_FULL) {
            // The release closes the microphone and asks for the transcript in one step: a
            // single action keeps the caller from having to remember an ordering that matters.
            close_mic(voice);
            voice->state = PASSPORT_VOICE_TRANSCRIBING;
            return PASSPORT_VOICE_ACT_TRANSCRIBE;
        }
        if (event == PASSPORT_VOICE_EV_FAILED) {
            close_mic(voice);
            voice->state = PASSPORT_VOICE_FAILED;
            return PASSPORT_VOICE_ACT_REPORT_FAILURE;
        }
        // A press while already listening is a repeat, not a new turn.
        return PASSPORT_VOICE_ACT_NONE;
    case PASSPORT_VOICE_TRANSCRIBING:
        if (event == PASSPORT_VOICE_EV_TRANSCRIPT) {
            voice->state = PASSPORT_VOICE_THINKING;
            return PASSPORT_VOICE_ACT_ASK_MODEL;
        }
        if (event == PASSPORT_VOICE_EV_FAILED) {
            voice->state = PASSPORT_VOICE_FAILED;
            return PASSPORT_VOICE_ACT_REPORT_FAILURE;
        }
        return PASSPORT_VOICE_ACT_NONE;

    case PASSPORT_VOICE_THINKING:
        if (event == PASSPORT_VOICE_EV_REPLY) {
            // The answer is spoken, so the microphone is closed here for certain. It was
            // already closed on leaving listening, and this second close is what makes the
            // invariant independent of that path.
            close_mic(voice);
            voice->state = PASSPORT_VOICE_SPEAKING;
            return PASSPORT_VOICE_ACT_SPEAK;
        }
        if (event == PASSPORT_VOICE_EV_FAILED) {
            voice->state = PASSPORT_VOICE_FAILED;
            return PASSPORT_VOICE_ACT_REPORT_FAILURE;
        }
        return PASSPORT_VOICE_ACT_NONE;

    case PASSPORT_VOICE_SPEAKING:
        if (event == PASSPORT_VOICE_EV_SPEECH_DONE) {
            to_idle(voice);
            voice->turns++;
            return PASSPORT_VOICE_ACT_RETURN_IDLE;
        }
        if (event == PASSPORT_VOICE_EV_FAILED) {
            voice->state = PASSPORT_VOICE_FAILED;
            return PASSPORT_VOICE_ACT_REPORT_FAILURE;
        }
        // A press while speaking is refused. The caller may cancel the answer and then start a
        // new turn; this machine will not open the microphone on top of its own voice.
        return PASSPORT_VOICE_ACT_NONE;

    case PASSPORT_VOICE_FAILED:
        return PASSPORT_VOICE_ACT_NONE;

    default:
        to_idle(voice);
        return PASSPORT_VOICE_ACT_RETURN_IDLE;
    }
}
