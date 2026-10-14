// main/passport_fonts.c
#include "passport_fonts.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "passport_utf8.h"

static const char *TAG = "passport_fonts";

// The built-in Source Han Sans SC subset, enabled by
// CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK. It covers the CJK ranges plus Latin, so it
// can carry the conversation text on its own.
LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk);

static lv_font_t s_body;

void passport_fonts_init(void)
{
    // A shallow copy of a static bitmap font, which is the pattern the repository's font
    // guide prescribes for attaching a fallback without touching the read-only original.
    s_body = lv_font_source_han_sans_sc_16_cjk;
    // The fallback is for symbols the CJK face does not carry. It is deliberately not a
    // metrics match: Source Han Sans covers Latin itself, so the fallback should rarely
    // be reached in conversation text.
    s_body.fallback = &lv_font_montserrat_14;
    ESP_LOGI(TAG, "正文字体:Source Han Sans SC 16 + Montserrat 14 回退");
}

const lv_font_t *passport_font_body(void)
{
    return &s_body;
}

bool passport_font_has_glyph(const lv_font_t *font, uint32_t codepoint)
{
    if (font == NULL) {
        return false;
    }
    lv_font_glyph_dsc_t glyph;
    memset(&glyph, 0, sizeof(glyph));
    return lv_font_get_glyph_dsc(font, &glyph, codepoint, 0) && !glyph.is_placeholder;
}

int passport_font_check(const lv_font_t *font, const char *utf8, char *report,
                        size_t report_size)
{
    if (font == NULL || utf8 == NULL) {
        return 0;
    }
    if (report != NULL && report_size > 0u) {
        report[0] = '\0';
    }

    int missing = 0;
    size_t index = 0u;
    size_t used = 0u;
    uint32_t codepoint = 0u;

    while (passport_utf8_next(utf8, &index, &codepoint) != 0u) {
        // Control characters have no glyph to look for and would only add noise.
        if (codepoint < 0x20u) {
            continue;
        }
        if (passport_font_has_glyph(font, codepoint)) {
            continue;
        }
        missing++;
        if (report == NULL || report_size == 0u) {
            continue;
        }
        char entry[16];
        snprintf(entry, sizeof(entry), "U+%04X ", (unsigned)codepoint);
        const size_t entry_len = strlen(entry);
        if (used + entry_len + 4u >= report_size) {
            // Keep the report bounded, but say that it is bounded.
            snprintf(report + used, report_size - used, "...");
            used = report_size - 1u;
            break;
        }
        memcpy(report + used, entry, entry_len);
        used += entry_len;
        report[used] = '\0';
    }
    return missing;
}
