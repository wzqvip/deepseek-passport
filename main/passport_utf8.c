// main/passport_utf8.c
#include "passport_utf8.h"

#include <string.h>

#define REPLACEMENT 0xFFFDu

size_t passport_utf8_char_len(unsigned char lead)
{
    if (lead < 0x80u) {
        return 1u;
    }
    if ((lead & 0xE0u) == 0xC0u) {
        return 2u;
    }
    if ((lead & 0xF0u) == 0xE0u) {
        return 3u;
    }
    if ((lead & 0xF8u) == 0xF0u) {
        return 4u;
    }
    return 1u;   // a continuation or invalid lead byte stands alone
}

size_t passport_utf8_next(const char *s, size_t *index, uint32_t *codepoint)
{
    if (s == NULL || index == NULL) {
        return 0u;
    }
    const size_t len = strlen(s);
    size_t i = *index;
    if (i >= len) {
        return 0u;
    }

    const unsigned char lead = (unsigned char)s[i];
    const size_t char_len = passport_utf8_char_len(lead);

    if (char_len == 1u) {
        // Anything that is not a valid lead byte becomes U+FFFD rather than a bogus
        // code point, so a caller can report it instead of drawing nonsense.
        const uint32_t value = (lead < 0x80u) ? (uint32_t)lead : REPLACEMENT;
        if (codepoint != NULL) {
            *codepoint = value;
        }
        *index = i + 1u;
        return 1u;
    }

    if (i + char_len > len) {
        // A truncated sequence at the end of the buffer.
        if (codepoint != NULL) {
            *codepoint = REPLACEMENT;
        }
        *index = i + 1u;
        return 1u;
    }

    uint32_t value = 0u;
    switch (char_len) {
    case 2u:
        value = (uint32_t)(lead & 0x1Fu);
        break;
    case 3u:
        value = (uint32_t)(lead & 0x0Fu);
        break;
    default:
        value = (uint32_t)(lead & 0x07u);
        break;
    }

    for (size_t k = 1u; k < char_len; k++) {
        const unsigned char c = (unsigned char)s[i + k];
        if ((c & 0xC0u) != 0x80u) {
            if (codepoint != NULL) {
                *codepoint = REPLACEMENT;
            }
            *index = i + 1u;
            return 1u;
        }
        value = (value << 6) | (uint32_t)(c & 0x3Fu);
    }

    if (codepoint != NULL) {
        *codepoint = value;
    }
    *index = i + char_len;
    return char_len;
}

size_t passport_utf8_truncate(const char *s, size_t max_bytes)
{
    if (s == NULL || max_bytes == 0u) {
        return 0u;
    }
    size_t index = 0u;
    for (;;) {
        const size_t before = index;
        if (passport_utf8_next(s, &index, NULL) == 0u) {
            break;   // end of string: everything fits
        }
        if (index > max_bytes) {
            return before;   // this character does not fit; stop before it
        }
    }
    return index;
}

size_t passport_utf8_count(const char *s)
{
    if (s == NULL) {
        return 0u;
    }
    size_t count = 0u;
    size_t index = 0u;
    while (passport_utf8_next(s, &index, NULL) != 0u) {
        count++;
    }
    return count;
}
