// main/passport_speech.h
// Push-to-talk: hold OK, speak, release. The recording is streamed to the bridge while it is still
// being captured, and the transcript that comes back is what the existing chat path is given.
//
// Why the bridge and not the provider directly: the audio has to leave the device while it is still
// being recorded, because a few seconds of speech is tens of kilobytes and this board does not have
// them to spare. Streaming means the request body's length is unknown when it starts, and a WAV
// container needs its length in the first 44 bytes. A companion on the network has the memory to
// wrap it for whichever provider is configured; the device cannot. The direct path — a bounded
// recording posted as a container to `stt_base_url` — is deferred, and this comment is where that
// choice is recorded rather than implied.
//
// The turn rules themselves stay in passport_voice, which is pure and host-tested. This module owns
// the audio and the socket, and nothing else.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Start the worker. Requires NVS and the network stack.
esp_err_t passport_speech_start(void);

// Push-to-talk, from the application's key path. Both are non-blocking: they set an intent and wake
// the worker, because the button callback runs on a shared timer task that must not block.
void passport_speech_talk_down(void);
void passport_speech_talk_up(void);

// True while the microphone is open for a turn.
bool passport_speech_listening(void);

// Speak `text` through the bridge's synthesis, or the model's last answer when `text` is NULL.
// Blocking for the length of the utterance; the microphone is closed throughout by the audio arbiter.
esp_err_t passport_speech_speak(const char *text);

// Describe the voice path for the console.
void passport_speech_report(char *out, size_t out_size);

// The peak below which a recording counts as an empty room. The default is **-1, meaning "derive it
// from the room"**: the boot warm-up measures the room and the gate becomes twice that peak, clamped
// between 300 and 4000. A fixed number was a guess about a room nobody had measured, and the guesses
// in this session ranged from 216 to 886 depending on the hour. A value of 0 or more pins the
// threshold, and 0 disables the check — which is how a silent test bench exercises the upload path.
void passport_speech_set_silence_peak(int peak);
int passport_speech_silence_peak(void);
