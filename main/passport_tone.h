// main/passport_tone.h
// A small integer tone generator, for the device's notification chimes.
//
// Pure C with no floating point and no library: the same code runs in the host test and on the
// device, and the test can check the waveform against the C library's own sine rather than
// against a copy of this approximation.
//
// Why not a sampled table: a table is a block of unexplained numbers in a header, and the chime
// needs two or three frequencies that are chosen for how they sound, not for being in the table.
// Bhaskara's approximation is four lines, accurate to about a fifth of a percent, and its error
// is something the test can state.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Full-scale amplitude of the approximation, used to scale the output.
#define PASSPORT_TONE_PEAK 32767

typedef struct {
    uint32_t phase;        // 0 to 2^32, wrapping; the top bits select the quadrant
    uint32_t step;         // phase increment per sample
    int32_t amplitude;     // peak amplitude of the note, 0 to PASSPORT_TONE_PEAK
    uint32_t attack;       // samples spent rising, to avoid a click
    uint32_t release;      // samples spent falling
    size_t total;          // samples in this note
    size_t emitted;
} passport_tone_t;

// Start a note. `fade_samples` is the length of the rise and of the fall, clamped so the two
// cannot overlap; a zero fade is allowed but clicks, which is why the default callers use a few
// milliseconds.
void passport_tone_start(passport_tone_t *tone, uint32_t hz, uint32_t sample_rate,
                         size_t samples, uint32_t amplitude, uint32_t fade_samples);

// Fill up to `samples` values, continuing the note. Returns how many were written, which is short
// at the end of the note. Values stay inside the note's amplitude, including during the fades.
size_t passport_tone_fill(passport_tone_t *tone, int16_t *out, size_t samples);

bool passport_tone_done(const passport_tone_t *tone);

// One sine sample for a phase, in the range -PASSPORT_TONE_PEAK to PASSPORT_TONE_PEAK. Exposed
// because the test checks it directly against the library's sine.
int32_t passport_tone_sine(uint32_t phase);
