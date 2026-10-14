// tools/make_cjk_font.mjs
// Generate the application's CJK font from a committed character inventory.
//
// Why this exists: the Source Han Sans subset that ships with LVGL is a curated sample
// for LVGL's own tests. Measured against a real model reply it left 8 of 22 code points
// without a glyph, including the full-width exclamation and question marks. A product
// whose interface answers in Chinese needs a font built from a real inventory.
//
// The inventory is committed rather than computed here, so the font can be regenerated
// byte-for-byte from what the repository contains:
//
//   assets/fonts/cjk-inventory.txt   8,178 characters derived from the GB2312/CP936
//                                    lead and trail byte ranges (0xA1A1-0xF7FE) plus
//                                    printable ASCII. That covers the common simplified
//                                    Chinese set and the full-width punctuation.
//
// Source font: managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf
// Licensed under the SIL Open Font License, which permits redistribution of a generated
// bitmap subset. Do not substitute a system font here: most of them cannot be
// redistributed, and a repository that ships such a subset has a licensing problem.
//
// Usage, from a machine with the converter installed:
//   npm install lv_font_conv@1.5.3 --prefix build/fontgen
//   node tools/make_cjk_font.mjs

import { readFileSync, statSync, existsSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

// Pinned deliberately; the generated file depends on it.
const CONVERTER_VERSION = '1.5.3';
const SIZE = 16;
// 2 bits per pixel: legible for CJK at this size without the flash cost of 4bpp.
// The pet art is pixel-styled, but antialiased text at 16px reads far better than 1bpp.
const BPP = 2;
const FONT_NAME = 'passport_cjk_16';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const inventoryPath = join(root, 'assets', 'fonts', 'cjk-inventory.txt');
const outputPath = join(root, 'assets', 'fonts', `${FONT_NAME}.c`);
const sourceFont = join(root, 'managed_components', 'lvgl__lvgl', 'scripts', 'built_in_font',
                        'SourceHanSansSC-Normal.otf');
const converter = join(root, 'build', 'fontgen', 'node_modules', 'lv_font_conv', 'lv_font_conv.js');

function fail(message) {
    console.error(`make_cjk_font: ${message}`);
    process.exit(1);
}

for (const [label, path] of [
    ['inventory', inventoryPath],
    ['source font', sourceFont],
    ['converter (run "npm install lv_font_conv@' + CONVERTER_VERSION + ' --prefix build/fontgen" first)',
     converter],
]) {
    if (!existsSync(path)) {
        fail(`${label} not found at ${path}`);
    }
}

const installed = JSON.parse(
    readFileSync(join(root, 'build', 'fontgen', 'node_modules', 'lv_font_conv', 'package.json'), 'utf8'),
).version;
if (installed !== CONVERTER_VERSION) {
    fail(`converter is ${installed}, expected the pinned ${CONVERTER_VERSION}`);
}

const inventory = readFileSync(inventoryPath, 'utf8');
// The inventory is a set; duplicates would only slow the conversion down.
const symbols = Array.from(new Set(Array.from(inventory))).join('');
console.log(`make_cjk_font: inventory ${inventory.length} code points (${symbols.length} unique)`);

const args = [
    converter,
    '--font', sourceFont,
    '--symbols', symbols,
    '--size', String(SIZE),
    '--bpp', String(BPP),
    '--format', 'lvgl',
    // Uncompressed, as the repository's font guide advises for the first version: RLE
    // would also require CONFIG_LV_USE_FONT_COMPRESSED.
    '--no-compress',
    '--no-kerning',
    '--lv-font-name', FONT_NAME,
    '--lv-include', 'lvgl.h',
    '--output', outputPath,
];

console.log(`make_cjk_font: lv_font_conv ${CONVERTER_VERSION}, size ${SIZE}, bpp ${BPP}`);
const started = Date.now();
const result = spawnSync(process.execPath, args, { encoding: 'utf8' });
if (result.stdout) {
    process.stdout.write(result.stdout);
}
if (result.stderr) {
    process.stderr.write(result.stderr);
}
if (result.status !== 0) {
    fail(`converter exited with ${result.status}`);
}

const bytes = statSync(outputPath).size;
const text = readFileSync(outputPath, 'utf8');
const glyphs = (text.match(/GlyphId\d+|\.bitmap_index\s*=/g) || []).length;
console.log(`make_cjk_font: wrote ${outputPath}`);
console.log(`make_cjk_font: ${(bytes / 1024).toFixed(1)} KiB of C source, ` +
            `${((Date.now() - started) / 1000).toFixed(1)} s, ${glyphs} glyph markers`);
console.log('make_cjk_font: verify on the device with "font check" against a real reply');
