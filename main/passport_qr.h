// main/passport_qr.h
// A QR encoder, because the device must draw a code with no network and no camera: the
// person setting it up points a phone at the panel, and the code carries the Wi-Fi details
// so nobody types a password.
//
// Scope is deliberate rather than general: byte mode, error correction level L, versions 1
// to 4. The longest thing this device encodes is a Wi-Fi join string, which is under 78
// bytes, so the extra versions and levels would be code with no user.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PASSPORT_QR_MAX_VERSION 4
#define PASSPORT_QR_MAX_SIZE    (17 + 4 * PASSPORT_QR_MAX_VERSION)   // 33 modules

typedef struct {
    uint8_t version;    // 1..4
    uint8_t size;       // modules per side: 21, 25, 29 or 33
    uint8_t mask;       // the mask the specification's penalty rules selected
    uint8_t modules[PASSPORT_QR_MAX_SIZE][PASSPORT_QR_MAX_SIZE];   // non-zero is dark
} passport_qr_t;

// Encode `text` in byte mode at level L, choosing the smallest version that fits and the
// mask the penalty rules prefer. Returns false when the text does not fit version 4.
bool passport_qr_encode(const char *text, passport_qr_t *out);

// The same symbol with a fixed mask. The host test compares this against an independent
// implementation whose automatic mask choice uses a different heuristic, so a matrix
// disagreement cannot be confused with a mask disagreement.
bool passport_qr_encode_masked(const char *text, uint8_t mask, passport_qr_t *out);

// The penalty score of a built symbol, exposed so the test can check that the chosen mask
// really is the cheapest of the eight.
int passport_qr_penalty(const passport_qr_t *symbol);

// The final codeword sequence for `text`: data codewords followed by the error correction,
// before placement and masking. Exposed so the host test can check the encoding and the
// Reed-Solomon arithmetic against an independent implementation separately from the module
// placement — when a matrix disagrees, this says which half is wrong.
bool passport_qr_codewords(const char *text, uint8_t *out, size_t out_size, int *data_len,
                           int *ec_len);
