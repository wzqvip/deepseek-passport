// Host tests for the spoken-turn state machine.
//
// The rules under test are the ones that decide whether a voice interface feels broken: who
// may open the microphone, what a refused press does, how each stage recovers from a failure,
// and — the invariant that matters most on a board with no echo cancellation — that the
// microphone is never open while the device is speaking.
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "passport_voice.h"

static passport_voice_action_t feed(passport_voice_t *voice, passport_voice_event_t event)
{
    return passport_voice_handle(voice, event);
}

int main(void)
{
    passport_voice_t voice;

    // --- a whole turn, in order ---------------------------------------------
    passport_voice_init(&voice);
    assert(voice.state == PASSPORT_VOICE_IDLE);
    assert(!voice.mic_open);
    assert(!passport_voice_busy(&voice));
    assert(voice.turns == 0u);

    assert(feed(&voice, PASSPORT_VOICE_EV_TALK_PRESS) == PASSPORT_VOICE_ACT_OPEN_MIC);
    assert(voice.state == PASSPORT_VOICE_LISTENING);
    assert(voice.mic_open);
    assert(passport_voice_busy(&voice));

    assert(feed(&voice, PASSPORT_VOICE_EV_TALK_RELEASE) == PASSPORT_VOICE_ACT_TRANSCRIBE);
    assert(voice.state == PASSPORT_VOICE_TRANSCRIBING);
    assert(!voice.mic_open);   // closed before anything is sent

    assert(feed(&voice, PASSPORT_VOICE_EV_TRANSCRIPT) == PASSPORT_VOICE_ACT_ASK_MODEL);
    assert(voice.state == PASSPORT_VOICE_THINKING);
    assert(!voice.mic_open);

    assert(feed(&voice, PASSPORT_VOICE_EV_REPLY) == PASSPORT_VOICE_ACT_SPEAK);
    assert(voice.state == PASSPORT_VOICE_SPEAKING);
    assert(!voice.mic_open);

    assert(feed(&voice, PASSPORT_VOICE_EV_SPEECH_DONE) == PASSPORT_VOICE_ACT_RETURN_IDLE);
    assert(voice.state == PASSPORT_VOICE_IDLE);
    assert(!voice.mic_open);
    assert(voice.turns == 1u);

    // --- the microphone invariant, asserted over every transition -----------
    // Every event is applied from every state, and the microphone is checked afterwards. This
    // is what would catch a future edit that opens the microphone to "listen for barge-in".
    const passport_voice_event_t events[] = {
        PASSPORT_VOICE_EV_TALK_PRESS,     PASSPORT_VOICE_EV_TALK_RELEASE,
        PASSPORT_VOICE_EV_RECORDING_FULL, PASSPORT_VOICE_EV_TRANSCRIPT,
        PASSPORT_VOICE_EV_REPLY,          PASSPORT_VOICE_EV_SPEECH_DONE,
        PASSPORT_VOICE_EV_FAILED,         PASSPORT_VOICE_EV_RESET,
    };
    const passport_voice_state_t states[] = {
        PASSPORT_VOICE_IDLE,   PASSPORT_VOICE_LISTENING, PASSPORT_VOICE_TRANSCRIBING,
        PASSPORT_VOICE_THINKING, PASSPORT_VOICE_SPEAKING, PASSPORT_VOICE_FAILED,
    };
    for (size_t s = 0; s < sizeof(states) / sizeof(states[0]); s++) {
        for (size_t e = 0; e < sizeof(events) / sizeof(events[0]); e++) {
            passport_voice_init(&voice);
            voice.state = states[s];
            voice.mic_open = (states[s] == PASSPORT_VOICE_LISTENING);
            (void)feed(&voice, events[e]);
            if (voice.state != PASSPORT_VOICE_LISTENING) {
                assert(!voice.mic_open);
            }
            // Whatever happens, the microphone is only ever open while listening.
            if (voice.mic_open) {
                assert(voice.state == PASSPORT_VOICE_LISTENING);
            }
        }
    }

    // --- the microphone is closed before the answer is spoken ---------------
    passport_voice_init(&voice);
    (void)feed(&voice, PASSPORT_VOICE_EV_TALK_PRESS);
    (void)feed(&voice, PASSPORT_VOICE_EV_TALK_RELEASE);
    (void)feed(&voice, PASSPORT_VOICE_EV_TRANSCRIPT);
    assert(voice.mic_open == false);
    (void)feed(&voice, PASSPORT_VOICE_EV_REPLY);
    assert(voice.state == PASSPORT_VOICE_SPEAKING);
    assert(voice.mic_open == false);

    // --- a press while the answer is being spoken is refused ----------------
    // Not queued and not obeyed: on a half-duplex device, cutting in has to cancel first.
    assert(feed(&voice, PASSPORT_VOICE_EV_TALK_PRESS) == PASSPORT_VOICE_ACT_NONE);
    assert(voice.state == PASSPORT_VOICE_SPEAKING);
    assert(!voice.mic_open);

    // --- a repeat press while listening is ignored, not a new turn ----------
    passport_voice_init(&voice);
    (void)feed(&voice, PASSPORT_VOICE_EV_TALK_PRESS);
    assert(feed(&voice, PASSPORT_VOICE_EV_TALK_PRESS) == PASSPORT_VOICE_ACT_NONE);
    assert(voice.state == PASSPORT_VOICE_LISTENING);
    assert(voice.turns == 0u);

    // --- a release without a press does nothing ----------------------------
    passport_voice_init(&voice);
    assert(feed(&voice, PASSPORT_VOICE_EV_TALK_RELEASE) == PASSPORT_VOICE_ACT_NONE);
    assert(voice.state == PASSPORT_VOICE_IDLE);

    // --- the recording limit ends the turn like a release -------------------
    passport_voice_init(&voice);
    (void)feed(&voice, PASSPORT_VOICE_EV_TALK_PRESS);
    assert(feed(&voice, PASSPORT_VOICE_EV_RECORDING_FULL) == PASSPORT_VOICE_ACT_TRANSCRIBE);
    assert(voice.state == PASSPORT_VOICE_TRANSCRIBING);
    assert(!voice.mic_open);

    // --- failure at each stage, and recovery --------------------------------
    const passport_voice_state_t failing[] = {
        PASSPORT_VOICE_LISTENING, PASSPORT_VOICE_TRANSCRIBING, PASSPORT_VOICE_THINKING,
        PASSPORT_VOICE_SPEAKING,
    };
    for (size_t i = 0; i < sizeof(failing) / sizeof(failing[0]); i++) {
        passport_voice_init(&voice);
        voice.state = failing[i];
        voice.mic_open = (failing[i] == PASSPORT_VOICE_LISTENING);
        assert(feed(&voice, PASSPORT_VOICE_EV_FAILED) == PASSPORT_VOICE_ACT_REPORT_FAILURE);
        assert(voice.state == PASSPORT_VOICE_FAILED);
        assert(!voice.mic_open);   // a failure never leaves the microphone open
        assert(!passport_voice_busy(&voice));

        // A reset clears it.
        assert(feed(&voice, PASSPORT_VOICE_EV_RESET) == PASSPORT_VOICE_ACT_RETURN_IDLE);
        assert(voice.state == PASSPORT_VOICE_IDLE);

        // And so does pressing the button again: a failure must not need a reboot.
        passport_voice_init(&voice);
        voice.state = PASSPORT_VOICE_FAILED;
        assert(feed(&voice, PASSPORT_VOICE_EV_TALK_PRESS) == PASSPORT_VOICE_ACT_OPEN_MIC);
        assert(voice.state == PASSPORT_VOICE_LISTENING);
    }

    // --- stray events in idle are ignored -----------------------------------
    passport_voice_init(&voice);
    assert(feed(&voice, PASSPORT_VOICE_EV_TRANSCRIPT) == PASSPORT_VOICE_ACT_NONE);
    assert(feed(&voice, PASSPORT_VOICE_EV_REPLY) == PASSPORT_VOICE_ACT_NONE);
    assert(feed(&voice, PASSPORT_VOICE_EV_SPEECH_DONE) == PASSPORT_VOICE_ACT_NONE);
    assert(feed(&voice, PASSPORT_VOICE_EV_FAILED) == PASSPORT_VOICE_ACT_NONE);
    assert(voice.state == PASSPORT_VOICE_IDLE);

    // --- reset from every state ---------------------------------------------
    for (size_t s = 0; s < sizeof(states) / sizeof(states[0]); s++) {
        passport_voice_init(&voice);
        voice.state = states[s];
        voice.mic_open = (states[s] == PASSPORT_VOICE_LISTENING);
        assert(feed(&voice, PASSPORT_VOICE_EV_RESET) == PASSPORT_VOICE_ACT_RETURN_IDLE);
        assert(voice.state == PASSPORT_VOICE_IDLE);
        assert(!voice.mic_open);
    }

    // --- a silent turn: the caller resets instead of reporting a failure -----
    passport_voice_init(&voice);
    (void)feed(&voice, PASSPORT_VOICE_EV_TALK_PRESS);
    (void)feed(&voice, PASSPORT_VOICE_EV_TALK_RELEASE);
    assert(feed(&voice, PASSPORT_VOICE_EV_RESET) == PASSPORT_VOICE_ACT_RETURN_IDLE);
    assert(voice.state == PASSPORT_VOICE_IDLE);
    assert(voice.turns == 0u);   // a turn with nothing in it does not count

    // --- several turns in a row ---------------------------------------------
    passport_voice_init(&voice);
    for (int turn = 0; turn < 3; turn++) {
        (void)feed(&voice, PASSPORT_VOICE_EV_TALK_PRESS);
        (void)feed(&voice, PASSPORT_VOICE_EV_TALK_RELEASE);
        (void)feed(&voice, PASSPORT_VOICE_EV_TRANSCRIPT);
        (void)feed(&voice, PASSPORT_VOICE_EV_REPLY);
        (void)feed(&voice, PASSPORT_VOICE_EV_SPEECH_DONE);
    }
    assert(voice.turns == 3u);

    // --- names, for logs and for the screen ---------------------------------
    assert(strcmp(passport_voice_state_name(PASSPORT_VOICE_IDLE), "idle") == 0);
    assert(strcmp(passport_voice_state_name(PASSPORT_VOICE_LISTENING), "listening") == 0);
    assert(strcmp(passport_voice_state_name(PASSPORT_VOICE_SPEAKING), "speaking") == 0);
    assert(strlen(passport_voice_state_name((passport_voice_state_t)99)) > 0);

    // --- null handling ------------------------------------------------------
    passport_voice_init(NULL);
    assert(passport_voice_handle(NULL, PASSPORT_VOICE_EV_TALK_PRESS) == PASSPORT_VOICE_ACT_NONE);
    assert(!passport_voice_busy(NULL));

    return 0;
}
