// main/passport_voice.h
// The turn-taking logic of a spoken conversation, as pure C.
//
// This is the part of the voice feature that can be settled without a provider, a network, or
// a microphone: who may open the microphone and when, what happens if the owner presses the
// button mid-answer, what a silent recording means, and how a failure at each stage recovers.
// Getting those rules wrong is what makes a voice interface feel broken, and they are exactly
// the rules that are hard to test once they are tangled up with an audio driver.
//
// The one invariant that matters most: **the microphone is never open while the device is
// speaking.** This board cannot do acoustic echo cancellation, so a microphone left open
// during playback records the device's own voice. The machine closes the microphone before
// asking it to speak, and the host test asserts that no transition ever leaves it open.
//
// The provider calls themselves stay outside: the caller receives an action and performs it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PASSPORT_VOICE_IDLE = 0,
    PASSPORT_VOICE_LISTENING,      // the microphone is open, capturing the owner's speech
    PASSPORT_VOICE_TRANSCRIBING,   // the microphone is closed, waiting for the transcript
    PASSPORT_VOICE_THINKING,       // waiting for the model's answer
    PASSPORT_VOICE_SPEAKING,       // playing the answer; the microphone stays closed
    PASSPORT_VOICE_FAILED,         // a stage failed; the reason is the caller's to show
} passport_voice_state_t;

typedef enum {
    PASSPORT_VOICE_EV_TALK_PRESS = 0,   // the talk button went down
    PASSPORT_VOICE_EV_TALK_RELEASE,     // the button came up: stop capturing and transcribe
    PASSPORT_VOICE_EV_RECORDING_FULL,   // the recording hit its length limit
    PASSPORT_VOICE_EV_TRANSCRIPT,       // a usable transcript arrived
    PASSPORT_VOICE_EV_REPLY,            // the model answered (may be empty)
    PASSPORT_VOICE_EV_SPEECH_DONE,      // playback finished
    PASSPORT_VOICE_EV_FAILED,           // the current stage failed
    PASSPORT_VOICE_EV_RESET,            // return to idle from any state
} passport_voice_event_t;

typedef enum {
    PASSPORT_VOICE_ACT_NONE = 0,
    PASSPORT_VOICE_ACT_OPEN_MIC,
    PASSPORT_VOICE_ACT_TRANSCRIBE,       // the microphone is now closed; send what was captured
    PASSPORT_VOICE_ACT_ASK_MODEL,
    PASSPORT_VOICE_ACT_SPEAK,
    PASSPORT_VOICE_ACT_REPORT_FAILURE,
    PASSPORT_VOICE_ACT_RETURN_IDLE,
} passport_voice_action_t;

typedef struct {
    passport_voice_state_t state;
    bool mic_open;    // this machine's own view of the microphone, not the driver's
    uint32_t turns;   // completed turns, for logging and for the tests
} passport_voice_t;

void passport_voice_init(passport_voice_t *voice);

// Apply one event and return the single action the caller should perform. An event that the
// current state does not accept changes nothing and returns PASSPORT_VOICE_ACT_NONE — notably
// a talk press while an answer is being spoken, which is refused rather than queued: on a
// half-duplex device, cutting in means cancelling the answer first.
//
// Two contracts the caller has to keep:
//   * ACT_TRANSCRIBE means the microphone has already been closed; send the captured audio.
//   * An empty transcript is reported with PASSPORT_VOICE_EV_RESET, not with
//     PASSPORT_VOICE_EV_TRANSCRIPT: this machine cannot see the text, and a silent room should
//     return quietly to idle rather than look like a failure.
passport_voice_action_t passport_voice_handle(passport_voice_t *voice, passport_voice_event_t event);

const char *passport_voice_state_name(passport_voice_state_t state);

// Whether a turn is in flight.
bool passport_voice_busy(const passport_voice_t *voice);
