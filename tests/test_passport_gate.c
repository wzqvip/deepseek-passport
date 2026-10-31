// Host tests for the silence threshold: the number that decides whether the owner's voice is heard.
#include <assert.h>
#include <stdio.h>

#include "passport_gate.h"

int main(void)
{
    // Unmeasured: a fixed fallback, and it is the middle of the range rather than an extreme.
    assert(passport_gate_from_floor(0, 0, false) == PASSPORT_GATE_UNMEASURED);
    assert(PASSPORT_GATE_UNMEASURED > PASSPORT_GATE_MIN);
    assert(PASSPORT_GATE_UNMEASURED < PASSPORT_GATE_MAX);

    // A quiet room: the floor, not zero, because the codec's own noise is below it.
    assert(passport_gate_from_floor(7, 211, true) == PASSPORT_GATE_MIN);
    assert(passport_gate_from_floor(100, 237, true) == PASSPORT_GATE_MIN);
    // Twice the average, once the average is above half the floor.
    assert(passport_gate_from_floor(400, 900, true) == 800);
    assert(passport_gate_from_floor(700, 1400, true) == 1400);

    // The measured device state that motivated this: room peak 1,847 with a modest average. The old rule
    // (twice the peak) produced 3,694 and would have discarded ordinary speech; the average keeps it usable.
    const int noisy = passport_gate_from_floor(300, 1847, true);
    assert(noisy == 600);
    assert(noisy < 1847);

    // A genuinely loud room is capped, so one loud minute cannot deafen the device for the session.
    assert(passport_gate_from_floor(900, 4000, true) == PASSPORT_GATE_MAX);
    assert(passport_gate_from_floor(16000, 32000, true) == PASSPORT_GATE_MAX);
    // The doubling cannot overflow, whatever the measurement claims.
    assert(passport_gate_from_floor(70000u, 70000u, true) == PASSPORT_GATE_MAX);
    assert(passport_gate_from_floor(20000u, 0, true) == PASSPORT_GATE_MAX);

    // The peak is reported but never moves the threshold on its own: same average, wild peak.
    assert(passport_gate_from_floor(400, 0, true) == passport_gate_from_floor(400, 32000, true));

    // A pinned value wins, including zero, which disables the check for a bench test.
    assert(passport_gate_effective(0, 900, 4000, true) == 0);
    assert(passport_gate_effective(1200, 900, 4000, true) == 1200);
    assert(passport_gate_effective(-1, 400, 900, true) == 800);
    assert(passport_gate_effective(-1, 7, 211, true) == PASSPORT_GATE_MIN);
    assert(passport_gate_effective(-1, 0, 0, false) == PASSPORT_GATE_UNMEASURED);

    // The range itself is a contract the caller documents.
    assert(PASSPORT_GATE_MIN > 0);
    assert(PASSPORT_GATE_MAX > PASSPORT_GATE_MIN);

    puts("passport_gate: PASS");
    return 0;
}
