// main/passport_config_store.h
// Persistence for the device configuration, and the process-wide instance the
// application and the console share.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "passport_config.h"

// Load the stored configuration into the live instance, or install the defaults
// when nothing is stored, the blob is the wrong size, or its version differs.
// `loaded` reports whether a stored configuration was accepted; a failure to read
// is reported but never leaves the live instance uninitialised.
esp_err_t passport_config_store_init(bool *loaded);

// The live configuration. Read it through the const pointer; write it through the
// mutable one and then persist with passport_config_store_save().
const passport_config_t *passport_config_current(void);
passport_config_t *passport_config_mutable(void);

// Whether the live instance matches what is stored.
bool passport_config_dirty(void);

// Persist the live instance. One blob under one key.
esp_err_t passport_config_store_save(void);

// Replace the live instance with the defaults and remove the stored copy.
esp_err_t passport_config_store_reset(void);
