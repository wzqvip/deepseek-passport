// main/passport_qr.c
#include "passport_qr.h"

#include <stdlib.h>
#include <string.h>

// --- parameters -------------------------------------------------------------
//
// Byte mode at error correction level L. Every version here is a single Reed-Solomon
// block, which is why there is no block interleaving in this file: the specification only
// needs it from version 3-M and 3-Q onward, and level L stays single-block through
// version 4.

typedef struct {
    uint8_t data_codewords;
    uint8_t ec_codewords;
    uint8_t capacity_bytes;   // byte-mode payload
} qr_params_t;

static const qr_params_t PARAMS[PASSPORT_QR_MAX_VERSION] = {
    { 19, 7, 17 },    // version 1
    { 34, 10, 32 },   // version 2
    { 55, 15, 53 },   // version 3
    { 80, 20, 78 },   // version 4
};

// --- Galois field -----------------------------------------------------------

static uint8_t g_exp[512];
static uint8_t g_log[256];
static bool g_ready;

static void gf_init(void)
{
    if (g_ready) {
        return;
    }
    uint16_t x = 1u;
    for (int i = 0; i < 255; i++) {
        g_exp[i] = (uint8_t)x;
        g_log[x] = (uint8_t)i;
        x <<= 1;
        if ((x & 0x100u) != 0u) {
            x ^= 0x11Du;   // the QR field polynomial
        }
    }
    for (int i = 255; i < 512; i++) {
        g_exp[i] = g_exp[i - 255];
    }
    g_ready = true;
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0u || b == 0u) {
        return 0u;
    }
    return g_exp[g_log[a] + g_log[b]];
}

// The generator polynomial for `ec` error correction codewords, highest power first with a
// leading coefficient of one.
static void rs_generator(int ec, uint8_t *g)
{
    uint8_t tmp[64];
    g[0] = 1u;
    int degree = 0;

    for (int i = 0; i < ec; i++) {
        const uint8_t root = g_exp[i];
        for (int j = 0; j <= degree + 1; j++) {
            tmp[j] = 0u;
        }
        for (int j = 0; j <= degree; j++) {
            tmp[j] ^= g[j];                          // multiply by x
            tmp[j + 1] ^= gf_mul(root, g[j]);        // and add the root
        }
        degree++;
        for (int j = 0; j <= degree; j++) {
            g[j] = tmp[j];
        }
    }
}

// Polynomial long division: the remainder is the error correction.
static void rs_remainder(const uint8_t *data, int data_len, const uint8_t *g, int ec,
                         uint8_t *out)
{
    uint8_t rem[64];
    memset(rem, 0, sizeof(rem));

    for (int i = 0; i < data_len; i++) {
        const uint8_t factor = (uint8_t)(data[i] ^ rem[0]);
        memmove(rem, rem + 1, (size_t)(ec - 1));
        rem[ec - 1] = 0u;
        if (factor != 0u) {
            for (int j = 0; j < ec; j++) {
                rem[j] ^= gf_mul(g[j + 1], factor);
            }
        }
    }
    memcpy(out, rem, (size_t)ec);
}

// --- bit stream -------------------------------------------------------------

typedef struct {
    uint8_t *bytes;
    int bit_pos;
} bit_writer_t;

static void put_bits(bit_writer_t *w, uint32_t value, int count)
{
    for (int i = count - 1; i >= 0; i--) {
        const int pos = w->bit_pos++;
        if ((value & (1u << i)) != 0u) {
            w->bytes[pos >> 3] |= (uint8_t)(0x80u >> (pos & 7));
        }
    }
}

// --- symbol construction ----------------------------------------------------

typedef struct {
    uint8_t modules[PASSPORT_QR_MAX_SIZE][PASSPORT_QR_MAX_SIZE];
    bool is_function[PASSPORT_QR_MAX_SIZE][PASSPORT_QR_MAX_SIZE];
    int size;
} builder_t;

// Column/row order follows the specification's figures: x is the column, y is the row.
static void set_function(builder_t *b, int x, int y, bool dark)
{
    b->modules[y][x] = dark ? 1u : 0u;
    b->is_function[y][x] = true;
}

static void draw_finder(builder_t *b, int cx, int cy)
{
    for (int dy = -1; dy <= 7; dy++) {
        for (int dx = -1; dx <= 7; dx++) {
            const int x = cx + dx;
            const int y = cy + dy;
            if (x < 0 || y < 0 || x >= b->size || y >= b->size) {
                continue;
            }
            const bool in_ring = (dx >= 0 && dx <= 6 && (dy == 0 || dy == 6)) ||
                                 (dy >= 0 && dy <= 6 && (dx == 0 || dx == 6));
            const bool in_core = (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4);
            set_function(b, x, y, in_ring || in_core);
        }
    }
}

static void draw_alignment(builder_t *b, int cx, int cy)
{
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            const bool ring = (abs(dx) == 2 || abs(dy) == 2);
            const bool core = (dx == 0 && dy == 0);
            set_function(b, cx + dx, cy + dy, ring || core);
        }
    }
}

static void draw_function_patterns(builder_t *b, int version)
{
    const int size = b->size;

    draw_finder(b, 0, 0);
    draw_finder(b, size - 7, 0);
    draw_finder(b, 0, size - 7);

    // Timing patterns.
    for (int i = 8; i < size - 8; i++) {
        const bool dark = ((i % 2) == 0);
        set_function(b, i, 6, dark);
        set_function(b, 6, i, dark);
    }

    // Alignment patterns. Version 1 has none; for 2 to 4 the only centre that does not
    // collide with a finder is the bottom-right one.
    if (version > 1) {
        const int far = size - 7;
        const int centers[2] = { 6, far };
        for (int a = 0; a < 2; a++) {
            for (int c = 0; c < 2; c++) {
                const int cx = centers[c];
                const int cy = centers[a];
                if ((cx == 6 && cy == 6) || (cx == 6 && cy == far) || (cx == far && cy == 6)) {
                    continue;   // overlaps a finder pattern
                }
                draw_alignment(b, cx, cy);
            }
        }
    }
}

// Reserve the format information areas so data placement skips them; their values are
// written once the mask is known.
static void reserve_format(builder_t *b)
{
    const int size = b->size;
    for (int i = 0; i <= 5; i++) {
        set_function(b, 8, i, false);
    }
    set_function(b, 8, 7, false);
    set_function(b, 8, 8, false);
    set_function(b, 7, 8, false);
    for (int i = 9; i < 15; i++) {
        set_function(b, 14 - i, 8, false);
    }
    for (int i = 0; i < 8; i++) {
        set_function(b, size - 1 - i, 8, false);
    }
    for (int i = 8; i < 15; i++) {
        set_function(b, 8, size - 15 + i, false);
    }
    set_function(b, 8, size - 8, true);   // the always-dark module
}

static void place_codewords(builder_t *b, const uint8_t *codewords, int total_cw)
{
    int bit = 0;
    const int total_bits = total_cw * 8;

    for (int right = b->size - 1; right >= 1; right -= 2) {
        if (right == 6) {
            right = 5;   // the vertical timing pattern is not a data column
        }
        for (int vert = 0; vert < b->size; vert++) {
            for (int j = 0; j < 2; j++) {
                const int x = right - j;
                const bool upward = (((right + 1) & 2) == 0);
                const int y = upward ? (b->size - 1 - vert) : vert;
                if (b->is_function[y][x] || bit >= total_bits) {
                    continue;
                }
                const uint8_t byte = codewords[bit >> 3];
                b->modules[y][x] = (uint8_t)((byte >> (7 - (bit & 7))) & 1u);
                bit++;
            }
        }
    }
}

static bool mask_bit(int mask, int x, int y)
{
    switch (mask) {
    case 0: return ((x + y) % 2) == 0;
    case 1: return (y % 2) == 0;
    case 2: return (x % 3) == 0;
    case 3: return ((x + y) % 3) == 0;
    case 4: return ((x / 3 + y / 2) % 2) == 0;
    case 5: return ((x * y % 2) + (x * y % 3)) == 0;
    case 6: return (((x * y % 2) + (x * y % 3)) % 2) == 0;
    case 7: return ((((x + y) % 2) + (x * y % 3)) % 2) == 0;
    default: return false;
    }
}

static void apply_mask(builder_t *b, int mask)
{
    for (int y = 0; y < b->size; y++) {
        for (int x = 0; x < b->size; x++) {
            if (b->is_function[y][x]) {
                continue;
            }
            if (mask_bit(mask, x, y)) {
                b->modules[y][x] ^= 1u;
            }
        }
    }
}

// Format information: five data bits with ten BCH check bits, masked by a fixed pattern.
static void draw_format(builder_t *b, int mask)
{
    const int size = b->size;
    const int data = (1 << 3) | mask;   // level L is 01 in the two-bit indicator field

    int rem = data;
    for (int i = 0; i < 10; i++) {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    }
    const int bits = ((data << 10) | rem) ^ 0x5412;

    for (int i = 0; i <= 5; i++) {
        set_function(b, 8, i, ((bits >> i) & 1) != 0);
    }
    set_function(b, 8, 7, ((bits >> 6) & 1) != 0);
    set_function(b, 8, 8, ((bits >> 7) & 1) != 0);
    set_function(b, 7, 8, ((bits >> 8) & 1) != 0);
    for (int i = 9; i < 15; i++) {
        set_function(b, 14 - i, 8, ((bits >> i) & 1) != 0);
    }
    for (int i = 0; i < 8; i++) {
        set_function(b, size - 1 - i, 8, ((bits >> i) & 1) != 0);
    }
    for (int i = 8; i < 15; i++) {
        set_function(b, 8, size - 15 + i, ((bits >> i) & 1) != 0);
    }
    set_function(b, 8, size - 8, true);
}

// --- penalty ----------------------------------------------------------------
//
// Rules 1 and 3 are computed in one pass with a run-length history, which is how the
// reference implementations do it. Scoring them separately is easy to get subtly wrong:
// rule 3 counts a 1:1:3:1:1 ratio pattern when four light modules precede or follow it, and
// the light border outside the symbol counts, so a pattern touching an edge is a real
// occurrence. Getting that wrong changes the scores by a few points, which is enough to
// select a different mask — as it did here, until the fixtures caught it.

static void history_add(int *history, int run, int size)
{
    if (history[0] == 0) {
        run += size;   // the light border at the start of the line counts as light area
    }
    for (int i = 6; i > 0; i--) {
        history[i] = history[i - 1];
    }
    history[0] = run;
}

static int count_finder_patterns(const int *history)
{
    const int n = history[1];
    if (n == 0) {
        return 0;
    }
    const bool core = (history[2] == n) && (history[3] == n * 3) && (history[4] == n) &&
                      (history[5] == n);
    if (!core) {
        return 0;
    }
    int count = 0;
    if (history[0] >= n * 4 && history[6] >= n) {
        count++;
    }
    if (history[6] >= n * 4 && history[0] >= n) {
        count++;
    }
    return count;
}

static int terminate_and_count(bool run_color, int run_len, int *history, int size)
{
    if (run_color) {
        history_add(history, run_len, size);
        run_len = 0;
    }
    run_len += size;   // and the light border at the end
    history_add(history, run_len, size);
    return count_finder_patterns(history);
}

// `axis` selects rows (0) or columns (1).
static int line_penalty(const passport_qr_t *symbol, int axis)
{
    const int size = (int)symbol->size;
    int penalty = 0;

    for (int i = 0; i < size; i++) {
        bool run_color = false;
        int run_len = 0;
        int history[7] = { 0, 0, 0, 0, 0, 0, 0 };

        for (int j = 0; j < size; j++) {
            const bool color = (axis == 0) ? (symbol->modules[i][j] != 0u)
                                           : (symbol->modules[j][i] != 0u);
            if (color == run_color) {
                run_len++;
                if (run_len == 5) {
                    penalty += 3;
                } else if (run_len > 5) {
                    penalty++;
                }
            } else {
                history_add(history, run_len, size);
                if (!run_color) {
                    penalty += count_finder_patterns(history) * 40;
                }
                run_color = color;
                run_len = 1;
            }
        }
        penalty += terminate_and_count(run_color, run_len, history, size) * 40;
    }
    return penalty;
}

int passport_qr_penalty(const passport_qr_t *symbol)
{
    if (symbol == NULL || symbol->size == 0u) {
        return 0;
    }
    const int size = (int)symbol->size;

    // Rules 1 and 3, once per row and once per column.
    int penalty = line_penalty(symbol, 0) + line_penalty(symbol, 1);

    // Rule 2: every two-by-two block of one colour.
    for (int y = 0; y + 1 < size; y++) {
        for (int x = 0; x + 1 < size; x++) {
            const uint8_t a = symbol->modules[y][x];
            if (a == symbol->modules[y][x + 1] && a == symbol->modules[y + 1][x] &&
                a == symbol->modules[y + 1][x + 1]) {
                penalty += 3;
            }
        }
    }

    // Rule 4: how far the proportion of dark modules is from half.
    int dark = 0;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (symbol->modules[y][x] != 0u) {
                dark++;
            }
        }
    }
    const int total = size * size;
    int k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;
    if (k > 0) {
        penalty += k * 10;
    }
    return penalty;
}

// Encode the payload and the error correction into `codewords`. Returns the version, or 0
// when the text does not fit.
static int encode_codewords(const char *text, uint8_t *codewords, int *data_len, int *ec_len)
{
    gf_init();

    const size_t length = strlen(text);
    int version = 0;
    for (int v = 0; v < PASSPORT_QR_MAX_VERSION; v++) {
        if ((int)length <= PARAMS[v].capacity_bytes) {
            version = v + 1;
            break;
        }
    }
    if (version == 0) {
        return 0;
    }

    const qr_params_t params = PARAMS[version - 1];
    memset(codewords, 0, (size_t)(params.data_codewords + params.ec_codewords));

    bit_writer_t writer = { .bytes = codewords, .bit_pos = 0 };
    put_bits(&writer, 0x4u, 4);                     // byte mode
    put_bits(&writer, (uint32_t)length, 8);         // character count, 8 bits for versions 1..9
    for (size_t i = 0; i < length; i++) {
        put_bits(&writer, (uint8_t)text[i], 8);
    }
    // Terminator, then pad to a codeword boundary, then the alternating pad bytes.
    const int capacity_bits = params.data_codewords * 8;
    for (int i = 0; i < 4 && writer.bit_pos < capacity_bits; i++) {
        put_bits(&writer, 0u, 1);
    }
    while ((writer.bit_pos % 8) != 0) {
        put_bits(&writer, 0u, 1);
    }
    for (int i = params.data_codewords - (writer.bit_pos / 8), alt = 0; i > 0; i--, alt++) {
        codewords[writer.bit_pos / 8] = (alt % 2 == 0) ? 0xECu : 0x11u;
        writer.bit_pos += 8;
    }

    uint8_t generator[64];
    rs_generator(params.ec_codewords, generator);
    rs_remainder(codewords, params.data_codewords, generator, params.ec_codewords,
                 codewords + params.data_codewords);

    if (data_len != NULL) {
        *data_len = params.data_codewords;
    }
    if (ec_len != NULL) {
        *ec_len = params.ec_codewords;
    }
    return version;
}

bool passport_qr_codewords(const char *text, uint8_t *out, size_t out_size, int *data_len,
                           int *ec_len)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    uint8_t codewords[100];
    const int version = encode_codewords(text, codewords, data_len, ec_len);
    if (version == 0) {
        return false;
    }
    const qr_params_t params = PARAMS[version - 1];
    const size_t total = (size_t)params.data_codewords + (size_t)params.ec_codewords;
    if (out_size < total) {
        return false;
    }
    memcpy(out, codewords, total);
    return true;
}

// --- public entry points ----------------------------------------------------

static bool build_symbol(const char *text, int mask, passport_qr_t *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }

    uint8_t codewords[100];
    int data_cw = 0;
    int ec_cw = 0;
    const int version = encode_codewords(text, codewords, &data_cw, &ec_cw);
    if (version == 0) {
        return false;   // longer than version 4 at level L can hold
    }
    const int total_cw = data_cw + ec_cw;

    builder_t builder;
    memset(&builder, 0, sizeof(builder));
    builder.size = 17 + 4 * version;

    draw_function_patterns(&builder, version);
    reserve_format(&builder);
    place_codewords(&builder, codewords, total_cw);
    apply_mask(&builder, mask);
    draw_format(&builder, mask);

    out->version = (uint8_t)version;
    out->size = (uint8_t)builder.size;
    out->mask = (uint8_t)mask;
    memcpy(out->modules, builder.modules, sizeof(out->modules));
    return true;
}

bool passport_qr_encode_masked(const char *text, uint8_t mask, passport_qr_t *out)
{
    if (text == NULL || out == NULL || mask > 7u) {
        return false;
    }
    return build_symbol(text, (int)mask, out);
}

bool passport_qr_encode(const char *text, passport_qr_t *out)
{
    // Checked here as well as in the builder: this function writes to `out` at the end, so
    // relying on the builder's check meant a null output pointer was dereferenced. A null
    // argument should be refused, not followed.
    if (text == NULL || out == NULL) {
        return false;
    }

    passport_qr_t candidate;
    passport_qr_t best;
    int best_penalty = -1;
    bool found = false;

    for (int mask = 0; mask < 8; mask++) {
        if (!build_symbol(text, mask, &candidate)) {
            return false;
        }
        const int penalty = passport_qr_penalty(&candidate);
        if (best_penalty < 0 || penalty < best_penalty) {
            best_penalty = penalty;
            best = candidate;
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    *out = best;
    return true;
}
