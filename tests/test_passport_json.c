// Host tests for the bounded JSON string decoder.
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "passport_json.h"

// UTF-8 bytes for a two-character Chinese string, spelled as bytes so this test
// file stays pure ASCII and the expectation cannot be mangled by an editor.
#define CJK_TWO "\xE4\xBD\xA0\xE5\xA5\xBD"

static passport_json_status_t get(const char *json, const char *key,
                                 char *out, size_t out_size, size_t *len)
{
    return passport_json_string(json, strlen(json), key, out, out_size, len);
}

int main(void)
{
    char out[256];
    size_t len = 0u;

    // --- plain values -------------------------------------------------------
    assert(get("{\"content\":\"hello\"}", "content", out, sizeof(out), &len) == PASSPORT_JSON_OK);
    assert(strcmp(out, "hello") == 0);
    assert(len == 5u);

    assert(get("{\"content\":\"\"}", "content", out, sizeof(out), &len) == PASSPORT_JSON_OK);
    assert(out[0] == '\0');
    assert(len == 0u);

    // Whitespace around the colon and after it is tolerated.
    assert(get("{ \"content\" : \t \"spaced\" }", "content", out, sizeof(out), &len) == PASSPORT_JSON_OK);
    assert(strcmp(out, "spaced") == 0);

    // --- the reasoning trap -------------------------------------------------
    // "content" must not match inside "reasoning_content", and the answer must be
    // the content field, not the model's reasoning.
    assert(get("{\"reasoning_content\":\"thinking hard\",\"content\":\"the answer\"}",
               "content", out, sizeof(out), &len) == PASSPORT_JSON_OK);
    assert(strcmp(out, "the answer") == 0);
    // Asking for the reasoning field explicitly still works.
    assert(get("{\"reasoning_content\":\"thinking hard\",\"content\":\"the answer\"}",
               "reasoning_content", out, sizeof(out), &len) == PASSPORT_JSON_OK);
    assert(strcmp(out, "thinking hard") == 0);

    // --- missing and null ---------------------------------------------------
    assert(get("{\"other\":\"x\"}", "content", out, sizeof(out), &len) == PASSPORT_JSON_NOT_FOUND);
    assert(get("{\"content\":null}", "content", out, sizeof(out), &len) == PASSPORT_JSON_NOT_FOUND);
    assert(get("", "content", out, sizeof(out), &len) == PASSPORT_JSON_NOT_FOUND);
    // A key name that is a prefix of another must not match.
    assert(get("{\"contents\":\"x\"}", "content", out, sizeof(out), &len) == PASSPORT_JSON_NOT_FOUND);

    // --- escapes ------------------------------------------------------------
    assert(get("{\"content\":\"line1\\nline2\"}", "content", out, sizeof(out), &len) == PASSPORT_JSON_OK);
    assert(strcmp(out, "line1\nline2") == 0);
    assert(len == 11u);

    assert(get("{\"content\":\"say \\\"hi\\\"\"}", "content", out, sizeof(out), &len) == PASSPORT_JSON_OK);
    assert(strcmp(out, "say \"hi\"") == 0);

    assert(get("{\"content\":\"back\\\\slash\"}", "content", out, sizeof(out), &len) == PASSPORT_JSON_OK);
    assert(strcmp(out, "back\\slash") == 0);

    assert(get("{\"content\":\"tab\\there\"}", "content", out, sizeof(out), &len) == PASSPORT_JSON_OK);
    assert(strcmp(out, "tab\there") == 0);

    // --- \u escapes ---------------------------------------------------------
    assert(get("{\"content\":\"\\u0041\\u0042\"}", "content", out, sizeof(out), &len) == PASSPORT_JSON_OK);
    assert(strcmp(out, "AB") == 0);
    assert(len == 2u);

    // Two Chinese characters as escapes, expected as their UTF-8 bytes.
    assert(get("{\"content\":\"\\u4f60\\u597d\"}", "content", out, sizeof(out), &len) == PASSPORT_JSON_OK);
    assert(strcmp(out, CJK_TWO) == 0);
    assert(len == 6u);

    // A surrogate pair is outside the BMP: one '?' per escape, deliberately.
    assert(get("{\"content\":\"\\ud83d\\ude00\"}", "content", out, sizeof(out), &len) == PASSPORT_JSON_OK);
    assert(strcmp(out, "??") == 0);

    // A malformed escape is refused rather than guessed at.
    assert(get("{\"content\":\"\\u00zz\"}", "content", out, sizeof(out), &len) == PASSPORT_JSON_BAD_INPUT);

    // --- truncation is reported, never silent -------------------------------
    // A value far longer than the buffer: the copy is cut, the true length is
    // reported, and the buffer stays terminated. This is the shape of the failure
    // the repository already recorded once, when a fixed 4096-byte document met a
    // 6971-byte response.
    char big[600];
    memset(big, 'z', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = '\0';
    char doc[700];
    snprintf(doc, sizeof(doc), "{\"content\":\"%s\"}", big);
    char small[64];
    const passport_json_status_t st = get(doc, "content", small, sizeof(small), &len);
    assert(st == PASSPORT_JSON_TRUNCATED);
    assert(len == sizeof(big) - 1u);          // the full decoded length is known
    assert(strlen(small) == sizeof(small) - 1u);
    assert(small[sizeof(small) - 1u] == '\0');

    // Exactly filling the buffer is not truncation of the given buffer, but the
    // value plus terminator needs one more byte, so it reports truncated.
    char exact[6];
    assert(get("{\"content\":\"12345\"}", "content", exact, sizeof(exact), &len) == PASSPORT_JSON_OK);
    assert(strcmp(exact, "12345") == 0);
    assert(len == 5u);

    // --- malformed documents ------------------------------------------------
    assert(get("{\"content\" \"x\"}", "content", out, sizeof(out), &len) == PASSPORT_JSON_BAD_INPUT);
    assert(get("{\"content\":\"unterminated", "content", out, sizeof(out), &len) == PASSPORT_JSON_BAD_INPUT);
    assert(get("{\"content\":123}", "content", out, sizeof(out), &len) == PASSPORT_JSON_BAD_INPUT);

    // --- argument validation ------------------------------------------------
    assert(passport_json_string(NULL, 5u, "content", out, sizeof(out), &len) == PASSPORT_JSON_BAD_INPUT);
    assert(passport_json_string("{}", 2u, NULL, out, sizeof(out), &len) == PASSPORT_JSON_BAD_INPUT);
    assert(passport_json_string("{}", 2u, "content", NULL, 8u, &len) == PASSPORT_JSON_BAD_INPUT);
    assert(passport_json_string("{}", 2u, "content", out, 0u, &len) == PASSPORT_JSON_BAD_INPUT);
    // A NULL decoded_len is allowed; the call still succeeds.
    assert(get("{\"content\":\"ok\"}", "content", out, sizeof(out), NULL) == PASSPORT_JSON_OK);
    assert(strcmp(out, "ok") == 0);

    assert(passport_json_len("abc") == 3u);
    assert(passport_json_len(NULL) == 0u);

    return 0;
}
