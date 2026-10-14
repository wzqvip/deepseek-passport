// main/passport_utf8.h
// UTF-8 handling, kept pure so it can be host-tested.
//
// This exists because of a specific hazard: cutting a byte string at a fixed byte count
// can split a multi-byte character. The split produce invalid UTF-8, LVGL renders
// nothing for it, and the visible result is a truncated Chinese sentence with a blank
// tail. The repository's font guide calls this out directly, and a reply from the model
// is exactly the kind of text that reaches a small display through a byte-count limit.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Decode the next code point at or after *index. Returns the number of bytes consumed
// (1 to 4), or 0 at the end of the string. An invalid byte yields U+FFFD and consumes
// one byte, so a caller always makes progress and never loops forever.
size_t passport_utf8_next(const char *s, size_t *index, uint32_t *codepoint);

// The longest prefix of `s` that is at most `max_bytes` bytes and ends on a character
// boundary. A result of 0 means even the first character does not fit.
size_t passport_utf8_truncate(const char *s, size_t max_bytes);

// Number of code points, counting an invalid byte as one replacement character.
size_t passport_utf8_count(const char *s);

// Bytes that the character starting at `s` occupies, or 1 for an invalid lead byte.
size_t passport_utf8_char_len(unsigned char lead);

// Percent-encode text for a query string: letters, digits and `-_.~` pass through, everything else
// becomes `%XX` from its UTF-8 bytes. Returns the number of bytes written, excluding the terminator;
// when it does not fit, returns what the whole encoding would have needed, so a caller can tell
// truncation from success. The output is always terminated when out_size > 0.
size_t passport_url_encode(const char *text, char *out, size_t out_size);
