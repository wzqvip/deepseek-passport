// main/passport_audio_arbiter.h
// Who owns the audio path, as pure logic so it can be tested on the host.
//
// The device cannot run acoustic echo cancellation, so the microphone must be
// closed for the whole time the speaker is playing. This module is where that
// guarantee lives: capture and playback cannot both hold the path, and the
// microphone is only permitted while nothing is playing. Making it a state
// machine rather than a convention means a future voice feature cannot
// accidentally transcribe the pet's own voice.
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    PASSPORT_AUDIO_FREE = 0,   // the path is idle
    PASSPORT_AUDIO_CAPTURE,    // the microphone is open
    PASSPORT_AUDIO_PLAYBACK,   // the speaker is playing
} passport_audio_owner_t;

// Take the path for `want`. Succeeds only from PASSPORT_AUDIO_FREE, and only for
// CAPTURE or PLAYBACK: FREE is not something a caller can acquire. On success the
// owner is set; on failure it is left untouched.
bool passport_audio_arbiter_acquire(passport_audio_owner_t *owner, passport_audio_owner_t want);

// Give the path back. Only the current owner may release it; any other value is
// ignored, so a stale worker cannot free a path a new worker now holds.
void passport_audio_arbiter_release(passport_audio_owner_t *owner, passport_audio_owner_t held);

// True when opening the microphone is legal, which excludes playback.
bool passport_audio_arbiter_mic_allowed(passport_audio_owner_t owner);

const char *passport_audio_owner_name(passport_audio_owner_t owner);
