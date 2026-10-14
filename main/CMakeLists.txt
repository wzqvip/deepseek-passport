idf_component_register(
    SRCS "main.c"
         "passport_app.c"
         "passport_audio.c"
         "passport_audio_arbiter.c"
         "passport_pet.c"
         "passport_chat.c"
         "passport_cli.c"
         "passport_config.c"
         "passport_config_store.c"
         "passport_http.c"
         "passport_json.c"
         "passport_fonts.c"
         "passport_portal.c"
         "passport_provision.c"
         "passport_qr.c"
         "passport_utf8.c"
         "passport_wifi.c"
         "passport_metrics_math.c"
         "assets/passport_pet_assets.c"
         # Kept for the repository's host gate, which compiles these two sources
         # directly (tests/test_demo_navigation.c, tests/test_ui_pixel_math.c).
         # The application itself no longer uses them.
         "demo_navigation.c"
         "ui_pixel_math.c"
    INCLUDE_DIRS "." "assets"
    REQUIRES bsp bt console esp-tls esp_app_format esp_event esp_http_client esp_http_server esp_hw_support esp_netif esp_timer esp_wifi mbedtls nvs_flash
)

# The generated CJK font. Kept out of SRCS because it lives under the repository's
# reusable assets directory, with its inventory and generator beside it. Regenerate with
# "node tools/make_cjk_font.mjs"; do not edit the generated file.
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/passport_cjk_16.c"
)
