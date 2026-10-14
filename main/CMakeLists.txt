idf_component_register(
    SRCS "main.c"
         "passport_app.c"
         "passport_audio.c"
         "passport_audio_arbiter.c"
         "passport_pet.c"
         "passport_cli.c"
         "passport_config.c"
         "passport_config_store.c"
         "passport_http.c"
         "passport_wifi.c"
         "passport_metrics_math.c"
         "assets/passport_pet_assets.c"
         # Kept for the repository's host gate, which compiles these two sources
         # directly (tests/test_demo_navigation.c, tests/test_ui_pixel_math.c).
         # The application itself no longer uses them.
         "demo_navigation.c"
         "ui_pixel_math.c"
    INCLUDE_DIRS "." "assets"
    REQUIRES bsp bt console esp-tls esp_app_format esp_event esp_http_client esp_hw_support esp_netif esp_timer esp_wifi mbedtls nvs_flash
)
