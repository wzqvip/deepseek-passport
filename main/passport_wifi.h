// main/passport_wifi.h
// The Wi-Fi station: bring up the stack, scan, connect with stored credentials.
//
// Credentials come from the configuration store, never from source. Scan and
// connect both block, so they belong in a task — the console task or the
// application worker — never in a button callback or the LVGL task.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    PASSPORT_WIFI_IDLE = 0,
    PASSPORT_WIFI_CONNECTING,
    PASSPORT_WIFI_CONNECTED,
    PASSPORT_WIFI_FAILED,
} passport_wifi_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool secure;
} passport_wifi_ap_t;

// One-time stack setup and start. Idempotent. NVS must already be initialized.
esp_err_t passport_wifi_init(void);

// Scan and return the number of access points written to `out`, capped at `max`.
// A negative return is an error. Blocking, bounded by `timeout_ms`.
int passport_wifi_scan(passport_wifi_ap_t *out, int max, uint32_t timeout_ms);

// Connect using the SSID and password from the configuration. Blocking, bounded by
// `timeout_ms`. Refuses before touching the radio when no SSID is configured.
esp_err_t passport_wifi_connect(uint32_t timeout_ms);

// Drop the association. The stored credentials are left alone.
esp_err_t passport_wifi_disconnect(void);

passport_wifi_state_t passport_wifi_state(void);
const char *passport_wifi_state_name(passport_wifi_state_t state);

// Current IPv4 address as text, or an empty string when not connected.
void passport_wifi_ip(char *out, size_t out_size);
