// Host tests for the pure arithmetic behind the M1 measurement dashboard.
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "passport_metrics_math.h"

int main(void)
{
    // KiB flooring: never round a reading up past what was allocated.
    assert(passport_kb(0u) == 0);
    assert(passport_kb(1023u) == 0);
    assert(passport_kb(1024u) == 1);
    assert(passport_kb(2047u) == 1);
    assert(passport_kb(4096u) == 4);
    assert(passport_kb(102400u) == 100);

    // Worst (smallest) largest-free-block reading wins.
    assert(passport_worst_largest(NULL, 3) == -1);
    assert(passport_worst_largest((const int[]){ 1 }, 0) == -1);
    assert(passport_worst_largest((const int[]){ 1 }, -1) == -1);
    assert(passport_worst_largest((const int[]){ 5 }, 1) == 0);
    assert(passport_worst_largest((const int[]){ 3, 7, 2, 9 }, 4) == 2);
    assert(passport_worst_largest((const int[]){ 9, 7, 8 }, 3) == 1);
    // A tie reports the earliest stage that reached the low-water mark.
    assert(passport_worst_largest((const int[]){ 4, 4, 6 }, 3) == 0);
    assert(passport_worst_largest((const int[]){ 6, 4, 4 }, 3) == 1);

    assert(passport_delta_kb(100, 60) == -40);
    assert(passport_delta_kb(10, 25) == 15);
    assert(passport_delta_kb(7, 7) == 0);

    // PCM level helpers.
    assert(passport_pcm_rms(NULL, 4u) == 0u);
    const int16_t silence[] = { 0, 0, 0, 0 };
    assert(passport_pcm_rms(silence, 4u) == 0u);
    assert(passport_pcm_rms(silence, 0u) == 0u);

    // A full-scale square wave has an RMS equal to its amplitude.
    const int16_t square[] = { 1000, -1000, 1000, -1000 };
    assert(passport_pcm_rms(square, 4u) == 1000u);

    // A single-sample impulse spreads its energy across the window.
    const int16_t impulse[] = { 30000, 0, 0, 0 };
    assert(passport_pcm_rms(impulse, 4u) == 15000u);

    // Non-perfect squares floor, which keeps the meter from overstating level.
    const int16_t near_square[] = { 1000, 1001 };
    assert(passport_pcm_rms(near_square, 2u) == 1000u);

    // Constant DC reads as its magnitude.
    const int16_t dc[] = { 100, 100, 100 };
    assert(passport_pcm_rms(dc, 3u) == 100u);

    assert(passport_pcm_peak(NULL, 3u) == 0u);
    assert(passport_pcm_peak(silence, 4u) == 0u);
    assert(passport_pcm_peak(silence, 0u) == 0u);
    const int16_t mixed[] = { 100, -200, 50 };
    assert(passport_pcm_peak(mixed, 3u) == 200u);
    // The most negative sample is the edge case: negating it overflows int16_t.
    const int16_t floor_sample[] = { -32768, 0 };
    assert(passport_pcm_peak(floor_sample, 2u) == 32768u);

    return 0;
}
