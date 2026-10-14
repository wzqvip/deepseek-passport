// main/passport_chat.h
// One conversational turn: take a prompt, ask the configured model, hand the answer
// to the interface.
//
// The work runs in its own task, not in the input task and not in the console task.
// A TLS handshake needs several kilobytes of stack, and neither of those tasks exists
// to host it.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Create the worker. Its stack is sized for a TLS handshake, and its response buffers
// are allocated once here rather than per turn, so a long session does not fragment
// the heap.
esp_err_t passport_chat_start(void);

// Queue a question. Non-blocking. Returns ESP_ERR_INVALID_STATE when a turn is already
// in flight, and ESP_ERR_INVALID_ARG for an empty prompt.
esp_err_t passport_chat_ask(const char *prompt);

bool passport_chat_busy(void);
