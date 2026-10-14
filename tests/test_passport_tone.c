// Host tests for the tone generator.
//
// The approximation is checked against the C library's sine rather than against a copy of itself,
// so the test says something about accuracy instead of about consistency.
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "passport_tone.h"

#define SAMPLE_RATE 16000u

// One turn of the phase is 2^32.
static double phase_to_radians(uint32_t phase)
{
    return 2.0 * 3.14159265358979323846 * ((double)phase / 4294967296.0);
}

int main(void)
{
    // --- the sine approximation --------------------------------------------
    // Sampled across a whole turn, worst case error stated rather than assumed.
    double worst = 0.0;
    for (int i = 0; i < 4096; i++) {
        const uint32_t phase = (uint32_t)(((uint64_t)i << 32) / 4096u);
        const double expected = sin(phase_to_radians(phase)) * PASSPORT_TONE_PEAK;
        const double got = (double)passport_tone_sine(phase);
        const double error = fabs(got - expected);
        if (error > worst) {
            worst = error;
        }
    }
    // Bhaskara's approximation is good to about a fifth of a percent; allow a little more.
    assert(worst < 0.005 * PASSPORT_TONE_PEAK);

    // The cardinal points are exact enough to be recognisable.
    assert(passport_tone_sine(0u) == 0);
    assert(passport_tone_sine(0x40000000u) > 32000);          // a quarter turn, +1
    assert(passport_tone_sine(0x80000000u) == 0);
    assert(passport_tone_sine(0xC0000000u) < -32000);         // three quarters, -1
    // The sign flips with the half turn.
    assert(passport_tone_sine(0x20000000u) > 0);
    assert(passport_tone_sine(0xA0000000u) < 0);

    // --- a note's length and bounds ----------------------------------------
    passport_tone_t tone;
    static int16_t buffer[4096];

    passport_tone_start(&tone, 1000u, SAMPLE_RATE, 512u, 12000u, 64u);
    assert(!passport_tone_done(&tone));
    const size_t written = passport_tone_fill(&tone, buffer, sizeof(buffer) / sizeof(buffer[0]));
    assert(written == 512u);              // the note's length, not the buffer's
    assert(passport_tone_done(&tone));

    for (size_t i = 0; i < written; i++) {
        assert(buffer[i] <= 12000 && buffer[i] >= -12000);
    }
    // The fades mean the ends are quiet: a chime that starts at full scale clicks.
    assert(buffer[0] == 0);
    assert(buffer[written - 1u] == 0 || abs(buffer[written - 1u]) < 200);

    // Filling in chunks gives the same samples as filling at once.
    passport_tone_t one;
    passport_tone_t two;
    passport_tone_start(&one, 440u, SAMPLE_RATE, 400u, 9000u, 32u);
    passport_tone_start(&two, 440u, SAMPLE_RATE, 400u, 9000u, 32u);
    static int16_t whole[400];
    static int16_t chunked[400];
    assert(passport_tone_fill(&one, whole, 400u) == 400u);
    size_t offset = 0u;
    while (!passport_tone_done(&two)) {
        const size_t part = passport_tone_fill(&two, chunked + offset, 37u);
        assert(part > 0u);
        offset += part;
    }
    assert(offset == 400u);
    assert(memcmp(whole, chunked, sizeof(whole)) == 0);

    // --- the pitch is the pitch ---------------------------------------------
    // Count rising zero crossings over a window of a known frequency. The window has to fit the
    // buffer: asking the generator for more samples than the array holds is the caller's mistake,
    // and it is a mistake this test made once.
    const uint32_t hz = 1000u;
    const size_t window = 4000u;   // a quarter second, so about 250 periods
    passport_tone_start(&tone, hz, SAMPLE_RATE, window, 8000u, 0u);
    const size_t filled = passport_tone_fill(&tone, buffer, window);
    assert(filled == window);
    int crossings = 0;
    for (size_t i = 1; i < filled; i++) {
        if (buffer[i - 1] < 0 && buffer[i] >= 0) {
            crossings++;
        }
    }
    // 4000 samples at 16 kHz is 0.25 s, which is 250 periods of 1 kHz.
    assert(crossings >= 249 && crossings <= 251);

    // --- degenerate cases ---------------------------------------------------
    // A note with no fade at all is allowed.
    passport_tone_start(&tone, 500u, SAMPLE_RATE, 16u, 1000u, 0u);
    assert(passport_tone_fill(&tone, buffer, 16u) == 16u);
    assert(passport_tone_done(&tone));

    // A fade longer than the note is clamped rather than overlapping: the note still rises and
    // falls and still has the length that was asked for.
    passport_tone_start(&tone, 500u, SAMPLE_RATE, 100u, 1000u, 999u);
    assert(passport_tone_fill(&tone, buffer, 100u) == 100u);
    assert(buffer[0] == 0);
    assert(abs(buffer[50]) <= 1000);

    // A zero-length note is finished before it starts.
    passport_tone_start(&tone, 500u, SAMPLE_RATE, 0u, 1000u, 0u);
    assert(passport_tone_done(&tone));
    assert(passport_tone_fill(&tone, buffer, 8u) == 0u);

    // An amplitude above full scale is clamped, so the output cannot clip.
    passport_tone_start(&tone, 500u, SAMPLE_RATE, 256u, 999999u, 0u);
    const size_t n = passport_tone_fill(&tone, buffer, 256u);
    for (size_t i = 0; i < n; i++) {
        assert(buffer[i] <= PASSPORT_TONE_PEAK && buffer[i] >= -PASSPORT_TONE_PEAK);
    }

    // A zero sample rate falls back rather than dividing by zero.
    passport_tone_start(&tone, 1000u, 0u, 64u, 1000u, 0u);
    assert(passport_tone_fill(&tone, buffer, 64u) == 64u);

    // --- argument handling --------------------------------------------------
    passport_tone_t untouched;
    memset(&untouched, 0, sizeof(untouched));
    passport_tone_start(NULL, 1000u, SAMPLE_RATE, 10u, 1000u, 0u);
    assert(passport_tone_fill(NULL, buffer, 8u) == 0u);
    assert(passport_tone_fill(&untouched, NULL, 8u) == 0u);
    assert(passport_tone_fill(&untouched, buffer, 0u) == 0u);
    assert(passport_tone_done(NULL));

    return 0;
}
