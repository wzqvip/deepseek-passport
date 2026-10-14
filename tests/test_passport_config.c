// Host tests for the configuration schema: defaults, the field table, numeric
// clamping, refusal of over-long values, and the rule that a secret is never
// rendered.
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "passport_config.h"

int main(void)
{
    passport_config_t cfg;
    char out[128];

    // --- defaults -----------------------------------------------------------
    passport_cfg_defaults(&cfg);
    assert(cfg.version == PASSPORT_CFG_VERSION);
    assert(strcmp(cfg.llm_base_url, "https://api.deepseek.com") == 0);
    assert(cfg.volume == 80u);
    assert(cfg.brightness == 100u);
    assert(cfg.notify_mode == 1u);
    assert(cfg.idle_off_seconds == 60u);
    assert(strcmp(cfg.pet_name, "Whale") == 0);
    // Model names are deliberately not defaulted: a wrong default fails at runtime
    // in a way that looks like a provider fault.
    assert(cfg.llm_model[0] == '\0');
    assert(cfg.stt_base_url[0] == '\0');
    assert(cfg.tts_voice[0] == '\0');
    // Credentials start empty, never pre-filled.
    assert(cfg.stt_api_key[0] == '\0');
    assert(cfg.llm_api_key[0] == '\0');
    assert(cfg.tts_api_key[0] == '\0');
    assert(cfg.bridge_token[0] == '\0');

    // Defaults are idempotent.
    passport_cfg_defaults(NULL);   // must not crash

    // --- field table --------------------------------------------------------
    assert(passport_cfg_field_count() == 17);
    assert(passport_cfg_field(-1) == NULL);
    assert(passport_cfg_field(passport_cfg_field_count()) == NULL);
    assert(passport_cfg_find(NULL) == NULL);
    assert(passport_cfg_find("nope") == NULL);

    const passport_cfg_field_t *f_llm_key = passport_cfg_find("llm_api_key");
    const passport_cfg_field_t *f_llm_url = passport_cfg_find("llm_base_url");
    const passport_cfg_field_t *f_volume = passport_cfg_find("volume");
    const passport_cfg_field_t *f_idle = passport_cfg_find("idle_off_seconds");
    const passport_cfg_field_t *f_notify = passport_cfg_find("notify_mode");
    const passport_cfg_field_t *f_name = passport_cfg_find("pet_name");
    assert(f_llm_key != NULL && f_llm_url != NULL && f_volume != NULL);
    assert(f_idle != NULL && f_notify != NULL && f_name != NULL);
    assert(f_llm_key->secret);
    assert(!f_llm_url->secret);
    assert(f_volume->type == PASSPORT_CFG_TYPE_U8);
    assert(f_idle->type == PASSPORT_CFG_TYPE_U16);
    assert(f_idle->max_value == 3600u);

    // Every field's offset must land inside the struct.
    for (int i = 0; i < passport_cfg_field_count(); i++) {
        const passport_cfg_field_t *f = passport_cfg_field(i);
        assert(f != NULL);
        assert(f->offset < sizeof(passport_config_t));
    }

    // --- formatting ---------------------------------------------------------
    assert(passport_cfg_format(&cfg, f_llm_url, out, sizeof(out)) > 0);
    assert(strcmp(out, "https://api.deepseek.com") == 0);
    assert(passport_cfg_format(&cfg, f_name, out, sizeof(out)) > 0);
    assert(strcmp(out, "Whale") == 0);
    assert(passport_cfg_format(&cfg, f_volume, out, sizeof(out)) > 0);
    assert(strcmp(out, "80") == 0);

    // An empty non-secret renders as a placeholder, and an empty secret too.
    const passport_cfg_field_t *f_model = passport_cfg_find("llm_model");
    assert(passport_cfg_format(&cfg, f_model, out, sizeof(out)) > 0);
    assert(strcmp(out, "(empty)") == 0);
    assert(passport_cfg_format(&cfg, f_llm_key, out, sizeof(out)) > 0);
    assert(strcmp(out, "(empty)") == 0);

    // --- the secret rule ----------------------------------------------------
    assert(passport_cfg_apply(&cfg, f_llm_key, "sk-super-secret-value") == PASSPORT_CFG_OK);
    assert(strcmp(cfg.llm_api_key, "sk-super-secret-value") == 0);   // stored
    assert(passport_cfg_format(&cfg, f_llm_key, out, sizeof(out)) > 0);
    assert(strcmp(out, "(set)") == 0);                               // reported
    assert(strstr(out, "secret") == NULL);                           // not leaked

    // Clearing a secret puts it back to empty.
    assert(passport_cfg_apply(&cfg, f_llm_key, "") == PASSPORT_CFG_OK);
    assert(cfg.llm_api_key[0] == '\0');
    assert(passport_cfg_format(&cfg, f_llm_key, out, sizeof(out)) > 0);
    assert(strcmp(out, "(empty)") == 0);

    // --- string limits ------------------------------------------------------
    char long_value[PASSPORT_CFG_STR_MAX + 8];
    memset(long_value, 'x', sizeof(long_value) - 1u);
    long_value[sizeof(long_value) - 1u] = '\0';
    assert(passport_cfg_apply(&cfg, f_llm_url, long_value) == PASSPORT_CFG_ERR_TOO_LONG);
    // The refused value must not have damaged the stored one.
    assert(strcmp(cfg.llm_base_url, "https://api.deepseek.com") == 0);

    // Exactly one byte short of the limit is accepted.
    char fits[PASSPORT_CFG_STR_MAX];
    memset(fits, 'y', sizeof(fits) - 1u);
    fits[sizeof(fits) - 1u] = '\0';
    assert(passport_cfg_apply(&cfg, f_llm_url, fits) == PASSPORT_CFG_OK);
    assert(strlen(cfg.llm_base_url) == PASSPORT_CFG_STR_MAX - 1u);

    // --- numeric parsing and clamping --------------------------------------
    assert(passport_cfg_apply(&cfg, f_volume, "0") == PASSPORT_CFG_OK);
    assert(cfg.volume == 0u);
    assert(passport_cfg_apply(&cfg, f_volume, "55") == PASSPORT_CFG_OK);
    assert(cfg.volume == 55u);
    assert(passport_cfg_apply(&cfg, f_volume, "250") == PASSPORT_CFG_OK);
    assert(cfg.volume == 100u);                 // clamped to the maximum
    assert(passport_cfg_apply(&cfg, f_idle, "99999") == PASSPORT_CFG_OK);
    assert(cfg.idle_off_seconds == 3600u);      // clamped
    assert(passport_cfg_apply(&cfg, f_idle, "0") == PASSPORT_CFG_OK);
    assert(cfg.idle_off_seconds == 0u);         // 0 is meaningful, not empty

    // Refused numeric input leaves the stored value alone.
    assert(passport_cfg_apply(&cfg, f_notify, "chime") == PASSPORT_CFG_ERR_BAD_VALUE);
    assert(cfg.notify_mode == 1u);
    assert(passport_cfg_apply(&cfg, f_notify, "") == PASSPORT_CFG_ERR_BAD_VALUE);
    assert(cfg.notify_mode == 1u);
    assert(passport_cfg_apply(&cfg, f_notify, "12x") == PASSPORT_CFG_ERR_BAD_VALUE);
    assert(cfg.notify_mode == 1u);
    assert(passport_cfg_apply(&cfg, f_notify, "-1") == PASSPORT_CFG_ERR_BAD_VALUE);
    assert(cfg.notify_mode == 1u);
    assert(passport_cfg_apply(&cfg, f_notify, "2") == PASSPORT_CFG_OK);
    assert(cfg.notify_mode == 2u);

    // Invalid arguments are refused rather than crashing.
    assert(passport_cfg_apply(NULL, f_notify, "1") == PASSPORT_CFG_ERR_BAD_VALUE);
    assert(passport_cfg_apply(&cfg, NULL, "1") == PASSPORT_CFG_ERR_BAD_VALUE);
    assert(passport_cfg_apply(&cfg, f_notify, NULL) == PASSPORT_CFG_ERR_BAD_VALUE);
    assert(passport_cfg_format(NULL, f_notify, out, sizeof(out)) == 0);
    assert(passport_cfg_format(&cfg, NULL, out, sizeof(out)) == 0);
    assert(passport_cfg_format(&cfg, f_notify, NULL, sizeof(out)) == 0);
    assert(passport_cfg_format(&cfg, f_notify, out, 0u) == 0);

    // --- status names -------------------------------------------------------
    assert(strcmp(passport_cfg_status_name(PASSPORT_CFG_OK), "ok") == 0);
    assert(strlen(passport_cfg_status_name(PASSPORT_CFG_ERR_UNKNOWN_FIELD)) > 0);
    assert(strlen(passport_cfg_status_name(PASSPORT_CFG_ERR_BAD_VALUE)) > 0);
    assert(strlen(passport_cfg_status_name(PASSPORT_CFG_ERR_TOO_LONG)) > 0);
    assert(strlen(passport_cfg_status_name((passport_cfg_status_t)99)) > 0);

    return 0;
}
