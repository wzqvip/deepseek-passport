// main/passport_json.h
// Just enough JSON to read a value out of a provider response.
//
// Why not a full parser: the responses this project reads are small and the shape
// is known, and every buffer on this board is a budget decision. What is *not*
// optional is the discipline around it — a bounded decode, explicit truncation
// reporting, and correct handling of escapes, because the model answers in Chinese
// and those characters may arrive either as raw UTF-8 or as \uXXXX escapes.
//
// Pure C with no ESP-IDF dependency, so all of the above is host-tested.
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    PASSPORT_JSON_OK = 0,
    PASSPORT_JSON_NOT_FOUND,
    PASSPORT_JSON_TRUNCATED,   // the value was longer than the caller's buffer
    PASSPORT_JSON_BAD_INPUT,   // null arguments, or an unterminated value
} passport_json_status_t;

// Decode the string value of the first `"key"` in `json[0..json_len)` into `out`.
//
// The key is matched with its surrounding quotes, so asking for "content" does not
// accidentally match "reasoning_content".
//
// `decoded_len`, when not NULL, receives the length the full value would have had
// excluding the terminator, so a caller can tell how much it lost. The result is
// always NUL-terminated when out_size > 0.
//
// Handles \" \\ \/ \b \f \n \r \t and \uXXXX. A \uXXXX escape outside the Basic
// Multilingual Plane is replaced with '?' rather than guessed at: emoji fidelity is
// not worth an unverified surrogate-pair decoder.
passport_json_status_t passport_json_string(const char *json, size_t json_len,
                                            const char *key,
                                            char *out, size_t out_size,
                                            size_t *decoded_len);

// Length of the JSON document, for callers that hold a NUL-terminated buffer.
size_t passport_json_len(const char *json);

// Read the integer value of the first `"key"` in `json[0..json_len)`.
//
// `found` reports whether the key was present at all, which matters when zero is a meaningful
// value: a missing progress field and a progress of zero are different states, and a caller
// that cannot tell them apart shows a stalled task when the field was simply absent.
// Returns 0 when the key is missing or its value is not an integer.
int passport_json_int(const char *json, size_t json_len, const char *key, bool *found);
