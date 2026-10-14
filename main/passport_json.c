// main/passport_json.c
#include "passport_json.h"

#include <string.h>

// Defined below; declared here because the integer reader above it needs it.
static const char *find_key(const char *json, size_t len, const char *key, size_t key_len);

int passport_json_int(const char *json, size_t json_len, const char *key, bool *found)
{
    if (found != NULL) {
        *found = false;
    }
    if (json == NULL || key == NULL) {
        return 0;
    }

    const char *at = find_key(json, json_len, key, strlen(key));
    if (at == NULL) {
        return 0;
    }

    const char *p = at + strlen(key) + 2u;
    const char *end = json + json_len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    if (p >= end || *p != ':') {
        return 0;
    }
    p++;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }

    bool negative = false;
    if (p < end && (*p == '-' || *p == '+')) {
        negative = (*p == '-');
        p++;
    }

    int value = 0;
    bool digits = false;
    while (p < end && *p >= '0' && *p <= '9') {
        digits = true;
        // Bounded so a malformed document cannot overflow the accumulator.
        if (value < 1000000) {
            value = value * 10 + (*p - '0');
        }
        p++;
    }
    if (!digits) {
        return 0;   // a string, an object, or nothing at all
    }
    if (found != NULL) {
        *found = true;
    }
    return negative ? -value : value;
}

size_t passport_json_len(const char *json)
{
    return (json == NULL) ? 0u : strlen(json);
}

// Match `"key"` starting at or after `from`. The surrounding quotes are part of the
// match, which is what keeps "content" from matching inside "reasoning_content".
static const char *find_key(const char *json, size_t len, const char *key, size_t key_len)
{
    if (key_len == 0u || len < key_len + 2u) {
        return NULL;
    }
    for (size_t i = 0u; i + key_len + 2u <= len; i++) {
        if (json[i] != '"') {
            continue;
        }
        if (memcmp(json + i + 1u, key, key_len) != 0) {
            continue;
        }
        if (json[i + key_len + 1u] != '"') {
            continue;
        }
        return json + i;
    }
    return NULL;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Encode one BMP code point as UTF-8. Returns the number of bytes written.
static size_t encode_utf8(unsigned code_point, char *out)
{
    if (code_point < 0x80u) {
        out[0] = (char)code_point;
        return 1u;
    }
    if (code_point < 0x800u) {
        out[0] = (char)(0xC0u | (code_point >> 6));
        out[1] = (char)(0x80u | (code_point & 0x3Fu));
        return 2u;
    }
    out[0] = (char)(0xE0u | (code_point >> 12));
    out[1] = (char)(0x80u | ((code_point >> 6) & 0x3Fu));
    out[2] = (char)(0x80u | (code_point & 0x3Fu));
    return 3u;
}

passport_json_status_t passport_json_string(const char *json, size_t json_len,
                                            const char *key,
                                            char *out, size_t out_size,
                                            size_t *decoded_len)
{
    if (decoded_len != NULL) {
        *decoded_len = 0u;
    }
    if (json == NULL || key == NULL || out == NULL || out_size == 0u) {
        return PASSPORT_JSON_BAD_INPUT;
    }
    out[0] = '\0';

    const char *found = find_key(json, json_len, key, strlen(key));
    if (found == NULL) {
        return PASSPORT_JSON_NOT_FOUND;
    }

    // Advance past `"key"`, whitespace, ':' and more whitespace, then the opening quote.
    const char *p = found + strlen(key) + 2u;
    const char *end = json + json_len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    if (p >= end || *p != ':') {
        return PASSPORT_JSON_BAD_INPUT;
    }
    p++;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    if (p >= end) {
        return PASSPORT_JSON_BAD_INPUT;
    }
    if (*p == 'n' && (size_t)(end - p) >= 4u && memcmp(p, "null", 4u) == 0) {
        return PASSPORT_JSON_NOT_FOUND;
    }
    if (*p != '"') {
        return PASSPORT_JSON_BAD_INPUT;
    }
    p++;

    size_t written = 0u;
    size_t total = 0u;
    bool closed = false;

    while (p < end) {
        char c = *p++;

        if (c == '"') {
            closed = true;
            break;
        }

        char piece[4];
        size_t piece_len = 1u;
        piece[0] = c;

        if (c == '\\') {
            if (p >= end) {
                break;
            }
            const char esc = *p++;
            switch (esc) {
            case '"':  piece[0] = '"';  break;
            case '\\': piece[0] = '\\'; break;
            case '/':  piece[0] = '/';  break;
            case 'b':  piece[0] = '\b'; break;
            case 'f':  piece[0] = '\f'; break;
            case 'n':  piece[0] = '\n'; break;
            case 'r':  piece[0] = '\r'; break;
            case 't':  piece[0] = '\t'; break;
            case 'u': {
                if ((size_t)(end - p) < 4u) {
                    return PASSPORT_JSON_BAD_INPUT;
                }
                unsigned code_point = 0u;
                bool valid = true;
                for (int i = 0; i < 4; i++) {
                    const int value = hex_value(p[i]);
                    if (value < 0) {
                        valid = false;
                        break;
                    }
                    code_point = (code_point << 4) | (unsigned)value;
                }
                p += 4;
                if (!valid) {
                    return PASSPORT_JSON_BAD_INPUT;
                }
                if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
                    // A surrogate half: outside the BMP. See the header for why this
                    // is not decoded.
                    piece[0] = '?';
                    piece_len = 1u;
                } else {
                    char utf8[3];
                    piece_len = encode_utf8(code_point, utf8);
                    memcpy(piece, utf8, piece_len);
                }
                break;
            }
            default:
                // An unknown escape keeps its literal character rather than failing
                // the whole read: a slightly wrong byte beats no answer at all.
                piece[0] = esc;
                break;
            }
        }

        total += piece_len;
        for (size_t i = 0u; i < piece_len; i++) {
            if (written + 1u < out_size) {
                out[written++] = piece[i];
            }
        }
    }

    out[written] = '\0';
    if (decoded_len != NULL) {
        *decoded_len = total;
    }

    if (!closed) {
        return PASSPORT_JSON_BAD_INPUT;
    }
    return (total >= out_size) ? PASSPORT_JSON_TRUNCATED : PASSPORT_JSON_OK;
}
