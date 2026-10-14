// main/passport_identity.c
#include "passport_identity.h"

#include <stdio.h>
#include <string.h>

const char PASSPORT_AP_ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";

bool passport_identity_from_mac(const uint8_t mac[6], char *ssid, size_t ssid_size,
                                char *password, size_t password_size)
{
    bool fits = true;

    if (ssid != NULL && ssid_size > 0u) {
        const int written = snprintf(ssid, ssid_size, "Passport-%02X%02X", mac[4], mac[5]);
        if (written < 0 || (size_t)written >= ssid_size) {
            fits = false;
            ssid[ssid_size - 1u] = '\0';
        }
    } else {
        fits = false;
    }

    if (password != NULL && password_size > PASSPORT_AP_PASSWORD_LENGTH) {
        const uint32_t alphabet = (uint32_t)(sizeof(PASSPORT_AP_ALPHABET) - 1u);
        // Seeded from the MAC's low four bytes and mixed with a small xorshift, so neighbouring MAC
        // addresses do not produce neighbouring passwords and the value is still deterministic.
        uint32_t state = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                         ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
        for (size_t i = 0u; i < PASSPORT_AP_PASSWORD_LENGTH; i++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            password[i] = PASSPORT_AP_ALPHABET[state % alphabet];
        }
        password[PASSPORT_AP_PASSWORD_LENGTH] = '\0';
    } else {
        fits = false;
        if (password != NULL && password_size > 0u) {
            password[0] = '\0';
        }
    }

    return fits;
}
