// main/passport_fonts.h
// The application's text fonts.
//
// The display must render Chinese, and the default Montserrat fonts have no CJK glyphs
// at all — UTF-8 and a successful build prove neither coverage nor rendering. This
// module wires the CJK font in and provides the coverage check that the repository's
// font guide asks for, so a missing glyph is reported as a code point instead of
// silently becoming a blank box.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

// Build the font descriptors. Call once after LVGL is initialized and before any widget
// uses the fonts.
void passport_fonts_init(void);

// The font conversation text uses.
const lv_font_t *passport_font_body(void);

// Does the font, or anything in its fallback chain, have a real glyph for this code
// point? A placeholder counts as absent, which is what makes the answer meaningful.
bool passport_font_has_glyph(const lv_font_t *font, uint32_t codepoint);

// Check every code point of `utf8` against `font` and write a human-readable report
// naming the missing ones as U+XXXX. Returns how many code points have no real glyph.
// The report is truncated rather than overflowing when many are missing.
int passport_font_check(const lv_font_t *font, const char *utf8, char *report,
                        size_t report_size);
