// main/passport_metrics_math.c
#include "passport_metrics_math.h"

int passport_kb(size_t bytes)
{
    return (int)(bytes / 1024u);
}

int passport_worst_largest(const int *largest_kb, int count)
{
    if (largest_kb == NULL || count <= 0) {
        return -1;
    }
    int worst = 0;
    for (int i = 1; i < count; i++) {
        if (largest_kb[i] < largest_kb[worst]) {
            worst = i;
        }
    }
    return worst;
}

int passport_delta_kb(int before_kb, int after_kb)
{
    return after_kb - before_kb;
}

// Integer square root by Newton's method; exact for every uint64_t input.
static uint64_t isqrt_u64(uint64_t value)
{
    if (value < 2u) {
        return value;
    }
    uint64_t x = value;
    uint64_t y = (x + 1u) / 2u;
    while (y < x) {
        x = y;
        y = (x + value / x) / 2u;
    }
    return x;
}

unsigned passport_pcm_rms(const int16_t *samples, size_t count)
{
    if (samples == NULL || count == 0u) {
        return 0u;
    }
    // Accumulate in 64 bits: 32768^2 * count overflows 32 bits well before a
    // realistic chunk length, and this runs on the audio path.
    uint64_t sum = 0u;
    for (size_t i = 0u; i < count; i++) {
        int32_t sample = samples[i];
        sum += (uint64_t)((int64_t)sample * (int64_t)sample);
    }
    return (unsigned)isqrt_u64(sum / (uint64_t)count);
}

unsigned passport_pcm_peak(const int16_t *samples, size_t count)
{
    if (samples == NULL || count == 0u) {
        return 0u;
    }
    uint32_t peak = 0u;
    for (size_t i = 0u; i < count; i++) {
        int32_t sample = samples[i];
        uint32_t magnitude = (sample < 0) ? (uint32_t)(-(int32_t)sample) : (uint32_t)sample;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    return (unsigned)peak;
}
