// main/passport_http.h
// A small HTTPS client for provider calls.
//
// Two deliberate properties:
//   * The response is read into a caller-supplied buffer with a hard cap, and
//     truncation is reported rather than hidden. A fixed 4096-byte JSON document
//     once crashed on this board when the real response needed 6971 bytes, so the
//     cap is explicit and the caller must size its parser from it.
//   * The heap readings are taken *while the connection is open*, because that is
//     the number the direct-mode memory budget depends on. Measuring before or
//     after the handshake misses the whole cost.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const char *url;
    const char *api_key;   // sent as "Authorization: Bearer ..."; never logged
    const char *body;      // NULL performs a GET
    uint32_t timeout_ms;
} passport_http_request_t;

typedef struct {
    int status;            // HTTP status, or 0 when the request never completed
    int body_len;          // bytes written into the caller's buffer, excluding the terminator
    bool truncated;        // the response was longer than the caller's buffer
    size_t heap_during;    // free heap measured with the TLS session established
    size_t largest_during; // largest free block measured at the same moment
    int64_t elapsed_ms;
} passport_http_result_t;

// Perform one request. `body_out` receives a NUL-terminated, capped copy of the
// response. Returns an error only for a transport failure; an HTTP error status is
// a successful call and appears in result->status.
esp_err_t passport_http_request(const passport_http_request_t *request,
                                passport_http_result_t *result,
                                char *body_out, size_t body_out_size);
