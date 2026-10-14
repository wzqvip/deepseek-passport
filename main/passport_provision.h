// main/passport_provision.h
// First-time setup and the device's own web console.
//
// The device is meant to be usable without a serial cable. It raises a protected access
// point of its own, shows its name and password on the display, and serves a small setup
// page over it. The station interface stays up in APSTA mode, so a device that is already
// on a network keeps answering while someone configures it.
//
// The request logic lives in passport_portal.c, which is pure and host-tested. This module
// is the part that needs a radio and an HTTP server.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "passport_portal.h"

// Raise the access point and start the server. Requires NVS and the network stack.
// Idempotent. Returns an error only when the radio or the server could not start.
esp_err_t passport_provision_start(void);

// Drop the access point and stop the server. The configuration is left alone.
esp_err_t passport_provision_stop(void);

bool passport_provision_active(void);
// The hotspot's name and password, derived from the device's MAC so they are stable across reboots and
// unique per board. Both are shown on the setup screen and carried in the join code.
//
// The password is ten characters from an alphabet with i, l, o, 0 and 1 removed, because it is read off
// a small screen and typed by a person: those five are the ones that get mistyped, and a phone reports
// a mistyped character as an incorrect password.
#define PASSPORT_AP_PASSWORD_LENGTH 10

const char *passport_provision_ssid(void);
const char *passport_provision_password(void);
const char *passport_provision_address(void);

// Render exactly what a request would return, for verification over the console. Returns
// the number of bytes written, or a negative value for a bad route or a small buffer.
int passport_provision_render(passport_portal_route_t route, char *out, size_t out_size);

// Apply a submitted form the way a POST would. A blank value leaves that setting
// unchanged, so a careless save cannot destroy a working key.
esp_err_t passport_provision_apply(const char *form);
