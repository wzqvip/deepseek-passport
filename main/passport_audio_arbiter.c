// main/passport_audio_arbiter.c
#include "passport_audio_arbiter.h"

bool passport_audio_arbiter_acquire(passport_audio_owner_t *owner, passport_audio_owner_t want)
{
    if (owner == NULL) {
        return false;
    }
    if (want != PASSPORT_AUDIO_CAPTURE && want != PASSPORT_AUDIO_PLAYBACK) {
        return false;
    }
    if (*owner != PASSPORT_AUDIO_FREE) {
        return false;
    }
    *owner = want;
    return true;
}

void passport_audio_arbiter_release(passport_audio_owner_t *owner, passport_audio_owner_t held)
{
    if (owner == NULL) {
        return;
    }
    if (held != PASSPORT_AUDIO_CAPTURE && held != PASSPORT_AUDIO_PLAYBACK) {
        return;
    }
    if (*owner == held) {
        *owner = PASSPORT_AUDIO_FREE;
    }
}

bool passport_audio_arbiter_mic_allowed(passport_audio_owner_t owner)
{
    return owner != PASSPORT_AUDIO_PLAYBACK;
}

const char *passport_audio_owner_name(passport_audio_owner_t owner)
{
    switch (owner) {
    case PASSPORT_AUDIO_FREE:
        return "free";
    case PASSPORT_AUDIO_CAPTURE:
        return "capture";
    case PASSPORT_AUDIO_PLAYBACK:
        return "playback";
    default:
        return "?";
    }
}
