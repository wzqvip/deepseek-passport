// Host tests for the hotspot credential derivation.
//
// These are the properties that decide whether a person can join this access point: the password has to
// be long enough for WPA2, free of characters that look like other characters, the same on every boot,
// and different from one board to the next. The first version of this code was eight hexadecimal digits
// and contained '1' twice, which is exactly what this test would have caught.
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "passport_identity.h"

static bool is_unambiguous(const char *text)
{
    for (const char *p = text; *p != '\0'; p++) {
        if (strchr(PASSPORT_AP_ALPHABET, *p) == NULL) {
            return false;
        }
    }
    return true;
}

int main(void)
{
    char ssid[33];
    char password[24];

    // A MAC whose hexadecimal form is full of the characters that caused the trouble.
    const uint8_t mac[6] = { 0x4C, 0x11, 0xAE, 0x2F, 0x91, 0xE1 };
    assert(passport_identity_from_mac(mac, ssid, sizeof(ssid), password, sizeof(password)));

    // The name comes from the last two bytes, uppercase, and is stable.
    assert(strcmp(ssid, "Passport-91E1") == 0);

    // The password is exactly the advertised length, and WPA2 needs at least eight.
    assert(strlen(password) == PASSPORT_AP_PASSWORD_LENGTH);
    assert(strlen(password) >= 8u);

    // Nothing in it can be mistaken for something else: no i, l, o, 0 or 1 anywhere.
    assert(is_unambiguous(password));
    assert(strpbrk(password, "ilo01") == NULL);

    // Derived, not random: the same board produces the same credentials every time.
    char again[24];
    char ssid2[33];
    assert(passport_identity_from_mac(mac, ssid2, sizeof(ssid2), again, sizeof(again)));
    assert(strcmp(ssid2, ssid) == 0);
    assert(strcmp(again, password) == 0);

    // A neighbouring MAC — the kind a second board would have — gives a different password. This is
    // the property the xorshift exists for; a naive byte-by-byte copy would fail it.
    const uint8_t neighbour[6] = { 0x4C, 0x11, 0xAE, 0x2F, 0x91, 0xE2 };
    char neighbour_pw[24];
    assert(passport_identity_from_mac(neighbour, ssid2, sizeof(ssid2), neighbour_pw,
                                      sizeof(neighbour_pw)));
    assert(strcmp(neighbour_pw, password) != 0);
    // ...while the name still follows the last two bytes, which is what identifies the board.
    assert(strcmp(ssid2, "Passport-91E2") == 0);

    // Every byte position varies across a run of MACs, so no position is a constant that a person
    // would learn to ignore.
    bool position_varies[PASSPORT_AP_PASSWORD_LENGTH] = { false };
    char first[24];
    for (int i = 0; i < 32; i++) {
        uint8_t probe[6] = { 0x4C, 0x11, 0xAE, 0x2F, (uint8_t)i, 0x10 };
        char probe_pw[24];
        char probe_ssid[33];
        assert(passport_identity_from_mac(probe, probe_ssid, sizeof(probe_ssid), probe_pw,
                                         sizeof(probe_pw)));
        assert(is_unambiguous(probe_pw));
        if (i == 0) {
            memcpy(first, probe_pw, sizeof(first));
        }
        for (size_t c = 0u; c < PASSPORT_AP_PASSWORD_LENGTH; c++) {
            if (probe_pw[c] != first[c]) {
                position_varies[c] = true;
            }
        }
    }
    for (size_t c = 0u; c < PASSPORT_AP_PASSWORD_LENGTH; c++) {
        assert(position_varies[c]);
    }

    // A buffer that cannot hold the value is reported rather than silently truncated into a password
    // that would not match the access point's own configuration.
    char small[6];
    assert(!passport_identity_from_mac(mac, ssid, sizeof(ssid), small, sizeof(small)));
    assert(small[0] == '\0');          // nothing partial is left to be mistaken for a password
    char tiny_ssid[4];
    assert(!passport_identity_from_mac(mac, tiny_ssid, sizeof(tiny_ssid), password,
                                       sizeof(password)));
    assert(strlen(tiny_ssid) == 3u);   // filled as far as it fits, and terminated

    // Null pointers are refused, not dereferenced.
    assert(!passport_identity_from_mac(mac, NULL, 0u, password, sizeof(password)));
    assert(!passport_identity_from_mac(mac, ssid, sizeof(ssid), NULL, 0u));

    return 0;
}
