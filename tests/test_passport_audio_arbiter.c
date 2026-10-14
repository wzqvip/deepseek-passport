// Host tests for the audio path arbiter: the guarantee that capture and playback
// cannot overlap, and that the microphone is closed while the speaker plays.
#include <assert.h>
#include <stdbool.h>

#include "passport_audio_arbiter.h"

int main(void)
{
    passport_audio_owner_t owner = PASSPORT_AUDIO_FREE;

    assert(passport_audio_owner_name(PASSPORT_AUDIO_FREE)[0] == 'f');
    assert(passport_audio_owner_name(PASSPORT_AUDIO_CAPTURE)[0] == 'c');
    assert(passport_audio_owner_name(PASSPORT_AUDIO_PLAYBACK)[0] == 'p');

    // The path starts free and the microphone is legal there.
    assert(passport_audio_arbiter_mic_allowed(owner));

    // Capture is only legal while nothing plays.
    assert(passport_audio_arbiter_acquire(&owner, PASSPORT_AUDIO_CAPTURE));
    assert(owner == PASSPORT_AUDIO_CAPTURE);
    assert(passport_audio_arbiter_mic_allowed(owner));

    // Capture cannot be taken twice, and playback cannot start over it.
    assert(!passport_audio_arbiter_acquire(&owner, PASSPORT_AUDIO_CAPTURE));
    assert(!passport_audio_arbiter_acquire(&owner, PASSPORT_AUDIO_PLAYBACK));
    assert(owner == PASSPORT_AUDIO_CAPTURE);

    // A stale release from the wrong holder must not free the path.
    passport_audio_arbiter_release(&owner, PASSPORT_AUDIO_PLAYBACK);
    assert(owner == PASSPORT_AUDIO_CAPTURE);

    // The real holder releases it.
    passport_audio_arbiter_release(&owner, PASSPORT_AUDIO_CAPTURE);
    assert(owner == PASSPORT_AUDIO_FREE);

    // While playback holds the path the microphone is closed: this is the rule
    // that stops the device transcribing its own speaker.
    assert(passport_audio_arbiter_acquire(&owner, PASSPORT_AUDIO_PLAYBACK));
    assert(!passport_audio_arbiter_mic_allowed(owner));
    assert(!passport_audio_arbiter_acquire(&owner, PASSPORT_AUDIO_CAPTURE));
    assert(owner == PASSPORT_AUDIO_PLAYBACK);

    passport_audio_arbiter_release(&owner, PASSPORT_AUDIO_PLAYBACK);
    assert(owner == PASSPORT_AUDIO_FREE);
    assert(passport_audio_arbiter_mic_allowed(owner));

    // FREE is not acquirable, and invalid arguments are refused rather than
    // corrupting the state.
    assert(!passport_audio_arbiter_acquire(&owner, PASSPORT_AUDIO_FREE));
    assert(owner == PASSPORT_AUDIO_FREE);
    assert(!passport_audio_arbiter_acquire(NULL, PASSPORT_AUDIO_CAPTURE));
    assert(!passport_audio_arbiter_acquire(&owner, (passport_audio_owner_t)99));
    assert(owner == PASSPORT_AUDIO_FREE);

    passport_audio_arbiter_release(NULL, PASSPORT_AUDIO_CAPTURE);
    passport_audio_arbiter_release(&owner, PASSPORT_AUDIO_FREE);
    assert(owner == PASSPORT_AUDIO_FREE);

    return 0;
}
