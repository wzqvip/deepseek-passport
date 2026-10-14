// Host tests for the UTF-8 helpers.
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "passport_utf8.h"

// Spelled as bytes so this file stays pure ASCII and the expectations cannot be mangled
// by an editor. U+4E2D, U+6587; then a 4-byte emoji U+1F600.
#define ZH_TWO   "\xE4\xB8\xAD\xE6\x96\x87"
#define E_ACUTE  "\xC3\xA9"
#define EMOJI    "\xF0\x9F\x98\x80"

int main(void)
{
    // --- character lengths --------------------------------------------------
    assert(passport_utf8_char_len('A') == 1u);
    assert(passport_utf8_char_len(0x80u) == 1u);   // stray continuation
    assert(passport_utf8_char_len(0xC3u) == 2u);
    assert(passport_utf8_char_len(0xE4u) == 3u);
    assert(passport_utf8_char_len(0xF0u) == 4u);

    // --- decoding -----------------------------------------------------------
    size_t index = 0u;
    uint32_t cp = 0u;

    assert(passport_utf8_next("A", &index, &cp) == 1u);
    assert(cp == 0x41u && index == 1u);
    assert(passport_utf8_next("A", &index, &cp) == 0u);   // end of string

    index = 0u;
    assert(passport_utf8_next(E_ACUTE, &index, &cp) == 2u);
    assert(cp == 0xE9u && index == 2u);

    index = 0u;
    assert(passport_utf8_next(ZH_TWO, &index, &cp) == 3u);
    assert(cp == 0x4E2Du && index == 3u);
    assert(passport_utf8_next(ZH_TWO, &index, &cp) == 3u);
    assert(cp == 0x6587u && index == 6u);

    index = 0u;
    assert(passport_utf8_next(EMOJI, &index, &cp) == 4u);
    assert(cp == 0x1F600u && index == 4u);

    // An invalid lead byte becomes U+FFFD and still advances one byte.
    index = 0u;
    assert(passport_utf8_next("\xFFx", &index, &cp) == 1u);
    assert(cp == 0xFFFDu && index == 1u);

    // A bad continuation becomes U+FFFD and advances one byte, so no infinite loop.
    index = 0u;
    assert(passport_utf8_next("\xE4\x41\x41", &index, &cp) == 1u);
    assert(cp == 0xFFFDu && index == 1u);

    // A sequence cut short by the end of the buffer.
    index = 0u;
    assert(passport_utf8_next("\xE4\xB8", &index, &cp) == 1u);
    assert(cp == 0xFFFDu && index == 1u);

    // --- counting -----------------------------------------------------------
    assert(passport_utf8_count("") == 0u);
    assert(passport_utf8_count("abc") == 3u);
    assert(passport_utf8_count(ZH_TWO) == 2u);
    assert(passport_utf8_count("a" E_ACUTE EMOJI) == 3u);
    assert(passport_utf8_count(NULL) == 0u);

    // --- truncation never splits a character --------------------------------
    // This is the property that matters: a Chinese reply cut by a byte limit must not
    // end mid-character, because the invalid tail renders as nothing.
    assert(passport_utf8_truncate("abc", 100u) == 3u);
    assert(passport_utf8_truncate("abc", 3u) == 3u);
    assert(passport_utf8_truncate("abc", 2u) == 2u);
    assert(passport_utf8_truncate("abc", 0u) == 0u);
    assert(passport_utf8_truncate("", 5u) == 0u);

    // Two 3-byte characters: byte limits of 3, 4 and 5 all cut before the second.
    assert(passport_utf8_truncate(ZH_TWO, 6u) == 6u);
    assert(passport_utf8_truncate(ZH_TWO, 5u) == 3u);
    assert(passport_utf8_truncate(ZH_TWO, 4u) == 3u);
    assert(passport_utf8_truncate(ZH_TWO, 3u) == 3u);
    assert(passport_utf8_truncate(ZH_TWO, 2u) == 0u);
    assert(passport_utf8_truncate(ZH_TWO, 1u) == 0u);

    // Mixed content: "a" + 2-byte + 4-byte = 7 bytes; a limit of 6 keeps "a" + 2-byte.
    assert(passport_utf8_truncate("a" E_ACUTE EMOJI, 7u) == 7u);
    assert(passport_utf8_truncate("a" E_ACUTE EMOJI, 6u) == 3u);
    assert(passport_utf8_truncate("a" E_ACUTE EMOJI, 4u) == 3u);
    assert(passport_utf8_truncate("a" E_ACUTE EMOJI, 3u) == 3u);
    assert(passport_utf8_truncate("a" E_ACUTE EMOJI, 2u) == 1u);
    assert(passport_utf8_truncate("a" E_ACUTE EMOJI, 1u) == 1u);

    // Whatever the limit, the kept prefix is valid and its own length matches.
    const char *mixed = "abc" ZH_TWO "def" EMOJI;
    for (size_t limit = 0u; limit <= strlen(mixed); limit++) {
        const size_t kept = passport_utf8_truncate(mixed, limit);
        assert(kept <= limit);
        assert(kept <= strlen(mixed));
        // Re-decoding the prefix must consume exactly `kept` bytes.
        size_t walk = 0u;
        size_t consumed = 0u;
        while (walk < kept) {
            consumed += passport_utf8_next(mixed, &walk, NULL);
        }
        assert(walk == kept);
        assert(consumed == kept);
    }

    assert(passport_utf8_truncate(NULL, 5u) == 0u);

    return 0;
}
