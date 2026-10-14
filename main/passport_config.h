// main/passport_config.h
// The device configuration: provider endpoints and credentials, plus preferences.
//
// The schema and its accessors are pure C with no ESP-IDF dependency, so the field
// table, the defaults, the clamping, and — importantly — the rule that a secret is
// never rendered can all be tested on the host. Persistence lives separately in
// passport_config_store.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bump this whenever the struct layout changes. A stored blob of a different size
// or version is discarded in favour of the defaults, which is safer than
// reinterpreting old bytes as new fields.
#define PASSPORT_CFG_VERSION 2u
#define PASSPORT_CFG_STR_MAX 96

typedef struct {
    uint32_t version;

    // Speech to text.
    char stt_base_url[PASSPORT_CFG_STR_MAX];
    char stt_api_key[PASSPORT_CFG_STR_MAX];
    char stt_model[PASSPORT_CFG_STR_MAX];

    // Language model.
    char llm_base_url[PASSPORT_CFG_STR_MAX];
    char llm_api_key[PASSPORT_CFG_STR_MAX];
    char llm_model[PASSPORT_CFG_STR_MAX];

    // Text to speech.
    char tts_base_url[PASSPORT_CFG_STR_MAX];
    char tts_api_key[PASSPORT_CFG_STR_MAX];
    char tts_model[PASSPORT_CFG_STR_MAX];
    char tts_voice[PASSPORT_CFG_STR_MAX];

    // Wi-Fi station.
    char wifi_ssid[PASSPORT_CFG_STR_MAX];
    char wifi_password[PASSPORT_CFG_STR_MAX];

    // Companion bridge for work mode.
    char bridge_url[PASSPORT_CFG_STR_MAX];
    char bridge_token[PASSPORT_CFG_STR_MAX];

    // Preferences.
    char pet_name[PASSPORT_CFG_STR_MAX];
    uint8_t volume;            // 0..100
    uint8_t brightness;        // 0..100
    uint8_t notify_mode;       // 0 silent, 1 chime, 2 speak
    uint16_t idle_off_seconds; // 0 disables the idle screen-off
} passport_config_t;

typedef enum {
    PASSPORT_CFG_TYPE_STR = 0,
    PASSPORT_CFG_TYPE_U8,
    PASSPORT_CFG_TYPE_U16,
} passport_cfg_type_t;

typedef struct {
    const char *name;
    passport_cfg_type_t type;
    size_t offset;
    bool secret;        // never rendered, only reported as set or empty
    uint32_t str_size;  // storage size for strings, including the terminator
    uint32_t min_value; // numeric bounds, applied by clamping
    uint32_t max_value;
} passport_cfg_field_t;

typedef enum {
    PASSPORT_CFG_OK = 0,
    PASSPORT_CFG_ERR_UNKNOWN_FIELD,
    PASSPORT_CFG_ERR_BAD_VALUE,
    PASSPORT_CFG_ERR_TOO_LONG,
} passport_cfg_status_t;

void passport_cfg_defaults(passport_config_t *cfg);

int passport_cfg_field_count(void);
const passport_cfg_field_t *passport_cfg_field(int index);
const passport_cfg_field_t *passport_cfg_find(const char *name);

// Render a field for display into `out`. A secret renders as "(set)" or "(empty)"
// and never as its value, which is what keeps keys out of logs and transcripts.
// Returns the number of characters that would have been written, like snprintf.
int passport_cfg_format(const passport_config_t *cfg, const passport_cfg_field_t *field,
                        char *out, size_t out_size);

// Apply a textual value. Numeric values are clamped to the field's bounds and
// non-numeric input is refused; an over-long string is refused rather than
// truncated, so a caller never silently stores a mangled key.
passport_cfg_status_t passport_cfg_apply(passport_config_t *cfg,
                                         const passport_cfg_field_t *field,
                                         const char *value);

const char *passport_cfg_status_name(passport_cfg_status_t status);
