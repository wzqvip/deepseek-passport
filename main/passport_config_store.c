// main/passport_config_store.c
#include "passport_config_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "passport_cfg";

#define CFG_NVS_NAMESPACE "passport_cfg"
#define CFG_NVS_KEY       "blob"

static passport_config_t s_config;
static bool s_dirty;
static bool s_ready;

static esp_err_t open_nvs(nvs_handle_t *handle)
{
    return nvs_open(CFG_NVS_NAMESPACE, NVS_READWRITE, handle);
}

esp_err_t passport_config_store_init(bool *loaded)
{
    if (loaded != NULL) {
        *loaded = false;
    }

    // Defaults first: whatever happens below, the live instance is valid.
    passport_cfg_defaults(&s_config);

    nvs_handle_t handle;
    esp_err_t err = open_nvs(&handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s;使用默认配置(无法保存)", esp_err_to_name(err));
        s_ready = false;
        s_dirty = true;   // the live configuration is not persisted
        return err;
    }

    passport_config_t stored;
    size_t size = sizeof(stored);
    err = nvs_get_blob(handle, CFG_NVS_KEY, &stored, &size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // First boot, or the configuration was cleared. The live defaults are not
        // what is stored — nothing is — so the instance is dirty until saved.
        ESP_LOGI(TAG, "未找到已保存的配置;使用默认配置");
        s_ready = true;
        s_dirty = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取配置失败: %s;使用默认配置", esp_err_to_name(err));
        s_ready = true;
        s_dirty = true;
        return err;
    }
    if (size != sizeof(stored) || stored.version != PASSPORT_CFG_VERSION) {
        // A shorter blob or a different version means the layout changed. Keeping
        // the defaults is safer than reinterpreting old bytes as new fields, and
        // the instance is dirty because the live configuration is not the stored
        // one — reporting "stored" here would be untrue.
        ESP_LOGW(TAG, "已保存配置与当前版本不符(大小 %u/%u,版本 %u/%u);改用默认配置,需要保存",
                 (unsigned)size, (unsigned)sizeof(stored),
                 (unsigned)stored.version, (unsigned)PASSPORT_CFG_VERSION);
        s_ready = true;
        s_dirty = true;
        return ESP_ERR_INVALID_VERSION;
    }

    s_config = stored;
    s_ready = true;
    s_dirty = false;
    if (loaded != NULL) {
        *loaded = true;
    }
    ESP_LOGI(TAG, "已载入配置(版本 %u,结构 %u 字节)", (unsigned)s_config.version,
             (unsigned)sizeof(s_config));
    return ESP_OK;
}

const passport_config_t *passport_config_current(void)
{
    return &s_config;
}

passport_config_t *passport_config_mutable(void)
{
    s_dirty = true;
    return &s_config;
}

bool passport_config_dirty(void)
{
    return s_dirty;
}

esp_err_t passport_config_store_save(void)
{
    nvs_handle_t handle;
    esp_err_t err = open_nvs(&handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败,配置未保存: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, CFG_NVS_KEY, &s_config, sizeof(s_config));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置保存失败: %s", esp_err_to_name(err));
        return err;
    }
    s_dirty = false;
    ESP_LOGI(TAG, "配置已保存(%u 字节)", (unsigned)sizeof(s_config));
    return ESP_OK;
}

esp_err_t passport_config_store_reset(void)
{
    passport_cfg_defaults(&s_config);

    nvs_handle_t handle;
    esp_err_t err = open_nvs(&handle);
    if (err != ESP_OK) {
        s_dirty = true;
        return err;
    }
    err = nvs_erase_key(handle, CFG_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;   // nothing was stored; the live defaults are already installed
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    s_dirty = false;
    ESP_LOGI(TAG, "配置已恢复默认并清除已保存副本: %s", esp_err_to_name(err));
    return err;
}
