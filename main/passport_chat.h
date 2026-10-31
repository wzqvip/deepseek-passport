// main/passport_chat.h
// One conversational turn: take a prompt, ask the configured model, hand the answer
// to the interface.
//
// The work runs in its own task, not in the input task and not in the console task.
// A TLS handshake needs several kilobytes of stack, and neither of those tasks exists
// to host it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Create the worker. Its stack is sized for a TLS handshake, and its response buffers
// are allocated once here rather than per turn, so a long session does not fragment
// the heap.
esp_err_t passport_chat_start(void);

// Queue a question. Non-blocking. Returns ESP_ERR_INVALID_STATE when a turn is already
// in flight, ESP_ERR_INVALID_ARG for an empty prompt, and INVALID_SIZE for a
// prompt of 200 bytes or more (never silently split UTF-8).
esp_err_t passport_chat_ask(const char *prompt);

bool passport_chat_busy(void);

// Reserve a request-specific result. Poll returns NOT_FINISHED while running;
// any final result consumes the reservation and copies only this request's answer.
// Abandon on caller timeout (does not cancel the network operation).
esp_err_t passport_chat_submit(const char *prompt, uint32_t *request_id);
esp_err_t passport_chat_result(uint32_t request_id, char *out, size_t out_size);
void passport_chat_abandon(uint32_t request_id);

// The model's most recent answer, or an empty string before the first one. Used by the voice path to
// ask for that answer to be spoken; the text is owned by the chat worker and valid until the next
// turn starts.
const char *passport_chat_last_reply(void);
// Safe snapshot for callers that may overlap a newly submitted turn.
esp_err_t passport_chat_copy_last_reply(char *out, size_t out_size);
