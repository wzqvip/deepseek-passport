// main/passport_work_client.h
// The device side of work mode: fetch a status document from the bridge, and send an answer
// back.
//
// Plain HTTP on the local network, not TLS. The bridge is a companion process on the same LAN
// as the device, the document carries no credentials, and skipping the handshake keeps the
// largest free heap block at 112 KiB instead of 68 — which this board needs for the display and
// the audio path. The security consideration is recorded rather than hidden: anything on the LAN
// can serve a document or read an answer, and the configuration's bridge token is the place a
// future revision would add authentication. Do not point this at a network you do not control.
//
// The parsing and the button rules live in passport_work, which is host-tested. This module is
// the part that needs a socket.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define PASSPORT_WORK_PATH_STATUS "/work-status"
#define PASSPORT_WORK_PATH_ANSWER "/answer"

// Start the poll task. Requires NVS (for the configuration) and the network stack. Idempotent.
esp_err_t passport_work_client_start(void);

// Ask the poll task for an immediate fetch, rather than waiting for the interval.
void passport_work_client_poll_now(void);

// Send one answer to the bridge and report what came back. Blocking, for a few seconds at most;
// call it from a task, not from the interface.
esp_err_t passport_work_client_answer(const char *answer);

// Describe the client's last attempt for the console.
void passport_work_client_report(char *out, size_t out_size);
