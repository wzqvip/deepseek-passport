// main/passport_config.c
#include "passport_config.h"

#include <stdio.h>
#include <string.h>

#define STR_FIELD(name_, member_, secret_)                                     \
    {                                                                          \
        name_, PASSPORT_CFG_TYPE_STR, offsetof(passport_config_t, member_),    \
            secret_, PASSPORT_CFG_STR_MAX, 0u, 0u                              \
    }

#define NUM_FIELD(name_, member_, type_, min_, max_)                           \
    {                                                                          \
        name_, type_, offsetof(passport_config_t, member_),                    \
            false, 0u, min_, max_                                              \
    }

// The single field table. Every console command and, later, the web console read
// the schema from here, so a new setting is added in exactly one place.
static const passport_cfg_field_t FIELDS[] = {
    STR_FIELD("stt_base_url", stt_base_url, false),
    STR_FIELD("stt_api_key", stt_api_key, true),
    STR_FIELD("stt_model", stt_model, false),

    STR_FIELD("llm_base_url", llm_base_url, false),
    STR_FIELD("llm_api_key", llm_api_key, true),
    STR_FIELD("llm_model", llm_model, false),

    STR_FIELD("tts_base_url", tts_base_url, false),
    STR_FIELD("tts_api_key", tts_api_key, true),
    STR_FIELD("tts_model", tts_model, false),
    STR_FIELD("tts_voice", tts_voice, false),

    STR_FIELD("wifi_ssid", wifi_ssid, false),
    STR_FIELD("wifi_password", wifi_password, true),

    STR_FIELD("bridge_url", bridge_url, false),
    STR_FIELD("bridge_token", bridge_token, true),

    STR_FIELD("pet_name", pet_name, false),
    NUM_FIELD("volume", volume, PASSPORT_CFG_TYPE_U8, 0u, 100u),
    NUM_FIELD("brightness", brightness, PASSPORT_CFG_TYPE_U8, 0u, 100u),
    NUM_FIELD("notify_mode", notify_mode, PASSPORT_CFG_TYPE_U8, 0u, 2u),
    NUM_FIELD("idle_off_seconds", idle_off_seconds, PASSPORT_CFG_TYPE_U16, 0u, 3600u),
};

#define FIELD_COUNT ((int)(sizeof(FIELDS) / sizeof(FIELDS[0])))

void passport_cfg_defaults(passport_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = PASSPORT_CFG_VERSION;

    // The one endpoint worth defaulting: DeepSeek's documented OpenAI-compatible
    // base URL. Model names change, so they are left for the console to set rather
    // than shipping a default that may already be wrong.
    snprintf(cfg->llm_base_url, sizeof(cfg->llm_base_url), "%s", "https://api.deepseek.com");

    cfg->volume = 80u;
    cfg->brightness = 100u;
    cfg->notify_mode = 1u;          // chime
    cfg->idle_off_seconds = 60u;
    snprintf(cfg->pet_name, sizeof(cfg->pet_name), "%s", "Whale");
}

int passport_cfg_field_count(void)
{
    return FIELD_COUNT;
}

const passport_cfg_field_t *passport_cfg_field(int index)
{
    if (index < 0 || index >= FIELD_COUNT) {
        return NULL;
    }
    return &FIELDS[index];
}

const passport_cfg_field_t *passport_cfg_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < FIELD_COUNT; i++) {
        if (strcmp(FIELDS[i].name, name) == 0) {
            return &FIELDS[i];
        }
    }
    return NULL;
}

int passport_cfg_format(const passport_config_t *cfg, const passport_cfg_field_t *field,
                        char *out, size_t out_size)
{
    if (cfg == NULL || field == NULL || out == NULL || out_size == 0u) {
        return 0;
    }
    const uint8_t *base = (const uint8_t *)cfg;

    switch (field->type) {
    case PASSPORT_CFG_TYPE_STR: {
        const char *value = (const char *)(base + field->offset);
        if (field->secret) {
            return snprintf(out, out_size, "%s", (value[0] != '\0') ? "(set)" : "(empty)");
        }
        return snprintf(out, out_size, "%s", (value[0] != '\0') ? value : "(empty)");
    }
    case PASSPORT_CFG_TYPE_U8:
        return snprintf(out, out_size, "%u", (unsigned)*(const uint8_t *)(base + field->offset));
    case PASSPORT_CFG_TYPE_U16:
        return snprintf(out, out_size, "%u", (unsigned)*(const uint16_t *)(base + field->offset));
    default:
        return snprintf(out, out_size, "?");
    }
}

passport_cfg_status_t passport_cfg_apply(passport_config_t *cfg,
                                         const passport_cfg_field_t *field,
                                         const char *value)
{
    if (cfg == NULL || field == NULL || value == NULL) {
        return PASSPORT_CFG_ERR_BAD_VALUE;
    }
    uint8_t *base = (uint8_t *)cfg;

    if (field->type == PASSPORT_CFG_TYPE_STR) {
        const size_t len = strlen(value);
        if (len >= field->str_size) {
            return PASSPORT_CFG_ERR_TOO_LONG;
        }
        char *dst = (char *)(base + field->offset);
        memcpy(dst, value, len + 1u);
        return PASSPORT_CFG_OK;
    }

    if (value[0] == '\0') {
        return PASSPORT_CFG_ERR_BAD_VALUE;
    }
    uint32_t parsed = 0u;
    for (const char *p = value; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return PASSPORT_CFG_ERR_BAD_VALUE;
        }
        parsed = parsed * 10u + (uint32_t)(*p - '0');
        if (parsed > 1000000u) {
            return PASSPORT_CFG_ERR_BAD_VALUE;
        }
    }
    if (parsed < field->min_value) {
        parsed = field->min_value;
    }
    if (parsed > field->max_value) {
        parsed = field->max_value;
    }

    if (field->type == PASSPORT_CFG_TYPE_U8) {
        *(uint8_t *)(base + field->offset) = (uint8_t)parsed;
    } else {
        *(uint16_t *)(base + field->offset) = (uint16_t)parsed;
    }
    return PASSPORT_CFG_OK;
}

const char *passport_cfg_status_name(passport_cfg_status_t status)
{
    switch (status) {
    case PASSPORT_CFG_OK:
        return "ok";
    case PASSPORT_CFG_ERR_UNKNOWN_FIELD:
        return "unknown field";
    case PASSPORT_CFG_ERR_BAD_VALUE:
        return "bad value";
    case PASSPORT_CFG_ERR_TOO_LONG:
        return "value too long";
    default:
        return "?";
    }
}
