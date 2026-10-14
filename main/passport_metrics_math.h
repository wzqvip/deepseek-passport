// main/passport_metrics_math.h
// Pure arithmetic behind the M1 measurement dashboard.
//
// Deliberately free of ESP-IDF, LVGL, and libm so it compiles and runs on the
// host under tests/test_passport_metrics_math.c.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Floor of bytes in kibibytes. Memory dashboards report whole KB; flooring keeps
// a reading from ever rounding up past what the allocator actually handed out.
int passport_kb(size_t bytes);

// Index of the smallest entry in largest_kb[0..count). Returns -1 for a null
// pointer or a non-positive count. On a tie the earliest index wins, so the
// dashboard shows the stage that first reached that low-water mark.
int passport_worst_largest(const int *largest_kb, int count);

// Change between two readings, in kibibytes.
int passport_delta_kb(int before_kb, int after_kb);

// Root-mean-square level of signed 16-bit PCM, in the sample domain. Zero
// samples or a null pointer yield 0. Integer only: the square root is computed
// by Newton's method so the host test does not need -lm.
unsigned passport_pcm_rms(const int16_t *samples, size_t count);

// Largest absolute sample, in the sample domain. This is the peak level, which
// catches a short transient that an RMS average would smooth away.
unsigned passport_pcm_peak(const int16_t *samples, size_t count);
