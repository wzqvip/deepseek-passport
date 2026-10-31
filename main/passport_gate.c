// main/passport_gate.c
#include "passport_gate.h"

int passport_gate_from_floor(unsigned rms, unsigned peak, bool measured)
{
    (void)peak;   // reported, never used: a transient is exactly what must not set the threshold.
    if (!measured) {
        return PASSPORT_GATE_UNMEASURED;
    }
    // Twice the average room level, which is where a voice has to rise above to count as speech.
    const unsigned doubled = rms > 16384u ? 32768u : rms * 2u;
    if (doubled < PASSPORT_GATE_MIN) {
        return PASSPORT_GATE_MIN;
    }
    if (doubled > PASSPORT_GATE_MAX) {
        return PASSPORT_GATE_MAX;
    }
    return (int)doubled;
}

int passport_gate_effective(int pinned, unsigned rms, unsigned peak, bool measured)
{
    if (pinned >= 0) {
        return pinned;
    }
    return passport_gate_from_floor(rms, peak, measured);
}
