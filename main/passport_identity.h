// main/passport_identity.h
// The hotspot's name and password, derived from the device's MAC address.
//
// Pure on purpose. These are credentials a person reads off a small screen and types into a phone, so
// the details that make them typeable — the length, the alphabet, the absence of characters that look
// like other characters — are worth a host test rather than a code review. The ESP-IDF side only has to
// supply the MAC.
//
// Derived rather than random means the same device always produces the same credentials, which is what
// keeps three things in agreement: the code on the glass, the code carried in the QR payload, and the
// access point's own configuration.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Ten characters. Long enough for the WPA2 minimum of eight, short enough to type.
#define PASSPORT_AP_PASSWORD_LENGTH 10

// The alphabet: lowercase letters and digits with i, l, o, 0 and 1 removed.
//
// '1' is the character most often mistyped as 'l' or 'I', and it appeared twice in the hexadecimal
// password this replaced; a single mistyped character is reported by a phone as an incorrect password.
// Nothing in this alphabet can be confused with anything else in it, and thirty-one symbols taken ten
// at a time carry more entropy than the eight hex digits did.
extern const char PASSPORT_AP_ALPHABET[];

// Fill `ssid` with "Passport-XXXX" (uppercase, from the MAC's last two bytes) and `password` with
// PASSPORT_AP_PASSWORD_LENGTH characters. Both are always NUL-terminated; a buffer that is too small
// is filled as far as it fits and reported by the return value.
//
// Returns false when either buffer cannot hold the whole value, having written what it could.
bool passport_identity_from_mac(const uint8_t mac[6], char *ssid, size_t ssid_size,
                                char *password, size_t password_size);
