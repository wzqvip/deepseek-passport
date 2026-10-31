// main/passport_gate.h
// The threshold below which a recording is treated as an empty room.
//
// Pure logic, host-tested, because the number decides whether the owner's own voice is heard. The first
// version used twice the room's *peak*, and a real device measured room peak 1,847 during a noisy minute,
// which put the threshold at 3,694 — high enough that ordinary speech at this microphone would be discarded
// as silence, which looks exactly like a broken device.
//
// The rule now uses the room's RMS: a peak is what one door slam or one fan surge looks like, while RMS is
// what the room sounds like on average, and a person speaking into a close microphone is well above it.
// The result is clamped at both ends: below the codec's own noise nothing can be distinguished, and above
// the ceiling a loud transient could otherwise deafen the device for the whole session.
#pragma once

#include <stdbool.h>

#define PASSPORT_GATE_MIN 300
#define PASSPORT_GATE_MAX 1500
// Used when the room was never measured, so the caller has no evidence to act on. It is the middle of the
// range rather than an extreme: too low uploads silence, too high discards speech.
#define PASSPORT_GATE_UNMEASURED 600

// The threshold to use, from the room measurement taken at boot.
//
// `measured` false means there is no measurement and the fallback is returned; `rms` and `peak` are the
// levels the warm-up recorded (in the sample domain, 0..32768). `peak` is accepted so the console can report
// both numbers, but only `rms` moves the threshold — see the note above.
int passport_gate_from_floor(unsigned rms, unsigned peak, bool measured);

// A pinned value from the console overrides the derivation, including 0 for "never treat anything as
// silence". Negative means unpinned.
int passport_gate_effective(int pinned, unsigned rms, unsigned peak, bool measured);
