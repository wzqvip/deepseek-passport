// main/passport_audio.h
// Microphone capture and speaker playback for the application, on top of the BSP
// codec path.
//
// All reads and writes go through the arbiter in passport_audio_arbiter.h, so the
// microphone cannot be open while the speaker is playing. Reads and writes block
// and belong in a task, never in a button callback or the LVGL task.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "passport_audio_arbiter.h"

// Peak-hold level of a capture, in the sample domain (0..32768).
typedef struct {
    unsigned rms;      // largest per-chunk RMS seen
    unsigned peak;     // largest absolute sample seen
    size_t samples;
} passport_audio_level_t;

// Prepare the codec for 16 kHz 16-bit mono. Idempotent; cheap when already set.
esp_err_t passport_audio_open(void);

// Open the codec and throw away a 500 ms settling window. Call once at application
// start, before any user-facing capture.
//
// Measured on hardware: with only the per-open 100 ms guard, the first capture
// after boot still read rms 7,627 / peak 9,087 while later captures of the same
// room read rms ~100 / peak ~237 — a 76x error that would look like continuous
// loud input to a voice state machine. Warming up once at start means no user turn
// ever sees the transient.
esp_err_t passport_audio_warmup(void);

// Who holds the audio path right now, for the console and for state machines.
passport_audio_owner_t passport_audio_owner(void);

// Capture `samples` mono samples into `buffer`. The level is measured on the same
// data, chunk by chunk, so a meter costs no extra buffer or pass.
//
// The first capture after the codec is opened discards a 100 ms warm-up first: the
// ES8311 emits a settling transient on its first samples, and measuring it would
// report a clipped, full-scale "input" that never happened. This was observed on
// hardware: the first capture after boot read rms 31,106 / peak 32,768, while later
// captures of the same quiet room read rms ~90 / peak ~230.
esp_err_t passport_audio_capture(int16_t *buffer, size_t samples, passport_audio_level_t *level);

// Play `samples` mono samples. The microphone stays closed for the whole call.
esp_err_t passport_audio_play(const int16_t *buffer, size_t samples);

// Fill `buffer` with a 1 kHz tone, for a speaker check that needs no microphone.
void passport_audio_fill_tone(int16_t *buffer, size_t samples);

// Prove on this device that the microphone cannot be opened while the speaker
// holds the path. Takes and releases the path internally; `*refused` reports
// whether the nested capture was rejected. Returns an error only if the path
// could not be taken at all.
esp_err_t passport_audio_verify_mute(bool *refused);
