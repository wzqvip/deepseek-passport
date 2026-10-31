// main/passport_speech.h
// Push-to-talk: hold OK, speak, release. Configured STT base+model selects direct
// HTTPS multipart WAV upload (up to 3 seconds, reduced on low RAM with visible limit).
// Otherwise stream up to 30 seconds to the companion bridge. No automatic network
// failure retry/fallback: this avoids duplicate billable provider requests.
// TTS base+model independently selects direct HTTPS PCM16k synthesis; otherwise bridge.
// All audio remains bounded in internal RAM; no filesystem or NVS layout changes.
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
// Nonblocking overflow/navigation recovery: stop capture and suppress subsequent
// model/speech stages. In-flight bounded I/O finishes before worker cleanup.
// Already uploaded bridge/provider requests or dispatched LLM cannot be recalled.
void passport_speech_talk_cancel(void);

// True while the microphone is open for a turn.
bool passport_speech_listening(void);

// Compatibility alias for request_speak below. Nonblocking: ESP_OK means accepted,
// not playback completed. A bounded UTF-8 copy is owned by the speech worker;
// TLS never runs on the caller's stack. Recording and playback turns are serialized.
esp_err_t passport_speech_speak(const char *text);

// Nonblocking bounded copy for UI/work notifications. Executes on the existing
// 12KB speech worker when idle; refuses while recording/turn/queued speech is active.
// NULL snapshots the last reply. ESP_OK means accepted, not playback completed.
esp_err_t passport_speech_request_speak(const char *text);

// Describe the voice path for the console.
void passport_speech_report(char *out, size_t out_size);

// The peak below which a recording counts as an empty room. The default is **-1, meaning "derive it
// from the room"**: the boot warm-up measures the room and the gate becomes twice that peak, clamped
// between 300 and 4000. A fixed number was a guess about a room nobody had measured, and the guesses
// in this session ranged from 216 to 886 depending on the hour. A value of 0 or more pins the
// threshold, and 0 disables the check — which is how a silent test bench exercises the upload path.
void passport_speech_set_silence_peak(int peak);
int passport_speech_silence_peak(void);
