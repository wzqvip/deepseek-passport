// main/passport_tone.c
#include "passport_tone.h"

#include <string.h>

// One sine sample from a phase, using Bhaskara I's approximation on the half wave and mirroring
// for the other half. The phase is a full turn in 32 bits, so a half turn is 31 bits.
//
// The angle is kept in units of 1/1024 degree rather than whole degrees. That is not a detail: at
// whole-degree resolution the quantisation alone puts the sample up to 1.75% away from the true
// sine near the ends of the half wave, which is an order of magnitude worse than the
// approximation's own error of about 0.16%. The test caught exactly that.
int32_t passport_tone_sine(uint32_t phase)
{
    const bool negative = (phase & 0x80000000u) != 0u;
    const uint32_t half = phase & 0x7FFFFFFFu;   // 0 .. 2^31 - 1

    const uint32_t span = 180u * 1024u;          // a half turn, in 1/1024 degree
    const uint32_t sub = (uint32_t)(((uint64_t)half * span) >> 31);   // 0 .. span

    // sin(x) ~= 4x(180-x) / (40500 - x(180-x)), on 0..180 degrees. In these units both the
    // numerator and the denominator carry a factor of 1024 squared, which cancels.
    const uint64_t term = (uint64_t)sub * (uint64_t)(span - sub);
    const uint64_t denominator = 40500ull * (1ull << 20) - term;
    const uint64_t numerator = 4ull * term;
    int32_t value =
        (int32_t)((numerator * (uint64_t)PASSPORT_TONE_PEAK) / denominator);

    if (negative) {
        value = -value;
    }
    return value;
}

void passport_tone_start(passport_tone_t *tone, uint32_t hz, uint32_t sample_rate,
                         size_t samples, uint32_t amplitude, uint32_t fade_samples)
{
    if (tone == NULL) {
        return;
    }
    memset(tone, 0, sizeof(*tone));
    if (sample_rate == 0u) {
        sample_rate = 16000u;
    }
    if (amplitude > (uint32_t)PASSPORT_TONE_PEAK) {
        amplitude = (uint32_t)PASSPORT_TONE_PEAK;
    }

    // A phase step of one full turn per period, in 32 bits.
    tone->step = (uint32_t)(((uint64_t)hz << 32) / sample_rate);
    tone->amplitude = (int32_t)amplitude;
    tone->total = samples;

    // The fades must leave a middle for the note itself: a note shorter than twice the fade gets
    // half its length for each fade instead, so it still rises and falls rather than being cut.
    uint32_t fade = fade_samples;
    if (samples > 0u && (size_t)fade * 2u > samples) {
        fade = (uint32_t)(samples / 2u);
    }
    tone->attack = fade;
    tone->release = fade;
}

// The envelope at a sample position, as a fraction of full scale in 1/65536 units.
static uint32_t envelope_at(const passport_tone_t *tone, size_t index)
{
    const uint32_t full = 65536u;
    if (tone->attack > 0u && index < (size_t)tone->attack) {
        return (uint32_t)(((uint64_t)full * index) / tone->attack);
    }
    const size_t remaining = tone->total - index;
    if (tone->release > 0u && remaining <= (size_t)tone->release) {
        return (uint32_t)(((uint64_t)full * remaining) / tone->release);
    }
    return full;
}

size_t passport_tone_fill(passport_tone_t *tone, int16_t *out, size_t samples)
{
    if (tone == NULL || out == NULL || samples == 0u) {
        return 0u;
    }
    size_t written = 0u;
    while (written < samples && tone->emitted < tone->total) {
        const int32_t wave = passport_tone_sine(tone->phase);
        const uint32_t envelope = envelope_at(tone, tone->emitted);
        // Both factors are bounded, so the product fits in 64 bits and then a plain divide.
        const int64_t scaled = (int64_t)wave * (int64_t)tone->amplitude;
        const int64_t shaped = (scaled * (int64_t)envelope) >> 16;
        int32_t value = (int32_t)(shaped / (int64_t)PASSPORT_TONE_PEAK);

        // The fades and the division can only shrink the value, but clamp anyway: a chime that
        // clips is worse than one that is quiet.
        if (value > (int32_t)tone->amplitude) {
            value = tone->amplitude;
        } else if (value < -(int32_t)tone->amplitude) {
            value = -tone->amplitude;
        }

        out[written++] = (int16_t)value;
        tone->phase += tone->step;
        tone->emitted++;
    }
    return written;
}

bool passport_tone_done(const passport_tone_t *tone)
{
    return tone == NULL || tone->emitted >= tone->total;
}
