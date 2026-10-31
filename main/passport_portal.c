// main/passport_portal.c
#include "passport_portal.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *path;
    passport_portal_route_t route;
} route_entry_t;

static const route_entry_t ROUTES[] = {
    { "/", PASSPORT_PORTAL_ROUTE_ROOT },
    { "/index.html", PASSPORT_PORTAL_ROUTE_ROOT },
    { "/save", PASSPORT_PORTAL_ROUTE_SAVE },
    { "/api/status", PASSPORT_PORTAL_ROUTE_STATUS },
    { "/api/config", PASSPORT_PORTAL_ROUTE_CONFIG },
    { "/api/diagnostics", PASSPORT_PORTAL_ROUTE_DIAGNOSTICS },
    // The paths operating systems request to decide whether a network needs a login page.
    // Answering them with a redirect is what makes the portal appear by itself.
    { "/generate_204", PASSPORT_PORTAL_ROUTE_REDIRECT },          // Android
    { "/gen_204", PASSPORT_PORTAL_ROUTE_REDIRECT },               // Android (older)
    { "/hotspot-detect.html", PASSPORT_PORTAL_ROUTE_REDIRECT },   // Apple
    { "/library/test/success.html", PASSPORT_PORTAL_ROUTE_REDIRECT },
    { "/ncsi.txt", PASSPORT_PORTAL_ROUTE_REDIRECT },              // Windows
    { "/connecttest.txt", PASSPORT_PORTAL_ROUTE_REDIRECT },       // Windows
    { "/redirect", PASSPORT_PORTAL_ROUTE_REDIRECT },              // Firefox
};

const char *passport_portal_route_name(passport_portal_route_t route)
{
    switch (route) {
    case PASSPORT_PORTAL_ROUTE_ROOT:
        return "root";
    case PASSPORT_PORTAL_ROUTE_SAVE:
        return "save";
    case PASSPORT_PORTAL_ROUTE_STATUS:
        return "status";
    case PASSPORT_PORTAL_ROUTE_CONFIG:
        return "config";
    case PASSPORT_PORTAL_ROUTE_DIAGNOSTICS:
        return "diagnostics";
    case PASSPORT_PORTAL_ROUTE_REDIRECT:
        return "redirect";
    case PASSPORT_PORTAL_ROUTE_NOT_FOUND:
    default:
        return "not-found";
    }
}

passport_portal_route_t passport_portal_route(const char *path)
{
    if (path == NULL || path[0] != '/') {
        return PASSPORT_PORTAL_ROUTE_NOT_FOUND;
    }

    // Compare only up to a query string.
    size_t len = 0u;
    while (path[len] != '\0' && path[len] != '?' && path[len] != '#') {
        len++;
    }
    // A single trailing slash is not significant: "/api/status/" is the same request.
    if (len > 1u && path[len - 1u] == '/') {
        len--;
    }

    for (size_t i = 0u; i < sizeof(ROUTES) / sizeof(ROUTES[0]); i++) {
        if (strlen(ROUTES[i].path) == len && strncmp(ROUTES[i].path, path, len) == 0) {
            return ROUTES[i].route;
        }
    }
    return PASSPORT_PORTAL_ROUTE_NOT_FOUND;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Decode `in` (which may contain %XX and '+') into out. Appends a terminator. Returns the
// number of bytes the decoded value needs, which may exceed out_size - 1 when truncated.
static size_t decode_into(const char *in, size_t in_len, char *out, size_t out_size)
{
    size_t written = 0u;
    size_t total = 0u;

    for (size_t i = 0u; i < in_len; i++) {
        char c = in[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2u < in_len) {
            const int high = hex_digit(in[i + 1u]);
            const int low = hex_digit(in[i + 2u]);
            if (high >= 0 && low >= 0) {
                c = (char)((high << 4) | low);
                i += 2u;
            }
        }
        total++;
        if (out != NULL && written + 1u < out_size) {
            out[written++] = c;
        }
    }

    if (out != NULL && out_size > 0u) {
        out[written] = '\0';
    }
    return total;
}

bool passport_portal_form_get(const char *body, const char *key, char *out, size_t out_size)
{
    if (body == NULL || key == NULL || out == NULL || out_size == 0u) {
        return false;
    }
    out[0] = '\0';

    const size_t key_len = strlen(key);
    size_t i = 0u;

    while (body[i] != '\0') {
        // One field runs to the next '&'.
        const size_t start = i;
        while (body[i] != '\0' && body[i] != '&') {
            i++;
        }
        const size_t end = i;
        if (body[i] == '&') {
            i++;
        }

        // Find the '=' that separates key from value within this field.
        size_t equals = end;
        for (size_t k = start; k < end; k++) {
            if (body[k] == '=') {
                equals = k;
                break;
            }
        }
        if (equals == end) {
            continue;   // a field with no '=' is not a key/value pair
        }

        // The key itself is decoded before comparison, so an encoded key still matches.
        char decoded_key[64];
        decode_into(body + start, equals - start, decoded_key, sizeof(decoded_key));
        if (strlen(decoded_key) != key_len || strncmp(decoded_key, key, key_len) != 0) {
            continue;
        }
        decode_into(body + equals + 1u, end - (equals + 1u), out, out_size);
        return true;
    }
    return false;
}

// A blank field means "leave this alone" for every field, uniformly. A form is submitted
// from a phone that cannot show a stored secret, so treating blank as "clear" would let a
// careless save destroy a working key. Clearing a value is an explicit act with an
// explicit interface: "config set <field> \"\"" on the console.
//
// Every entry names a field the configuration schema really has; the host test checks that, because
// a typo here is discovered by a user whose setting silently did not save.
static const passport_portal_field_t FIELDS[] = {
    { "wifi_ssid", "wifi_ssid", "Wi-Fi 名称", "网络" },
    { "wifi_password", "wifi_password", "Wi-Fi 密码", "网络" },

    { "llm_base_url", "llm_base_url", "接口地址", "对话模型" },
    { "llm_api_key", "llm_api_key", "API Key", "对话模型" },
    { "llm_model", "llm_model", "模型", "对话模型" },

    { "stt_base_url", "stt_base_url", "转写接口地址", "语音" },
    { "stt_api_key", "stt_api_key", "转写密钥", "语音" },
    { "stt_model", "stt_model", "转写模型", "语音" },
    { "tts_base_url", "tts_base_url", "朗读接口地址", "语音" },
    { "tts_api_key", "tts_api_key", "朗读密钥", "语音" },
    { "tts_model", "tts_model", "朗读模型", "语音" },
    { "tts_voice", "tts_voice", "音色", "语音" },

    { "bridge_url", "bridge_url", "桥接服务地址", "工作模式" },
    { "bridge_token", "bridge_token", "桥接令牌", "工作模式" },

    { "pet_name", "pet_name", "宠物名字", "偏好" },
    { "volume", "volume", "音量 (0-100)", "偏好" },
    { "brightness", "brightness", "亮度 (0-100,0 为熄屏)", "偏好" },
    { "notify_mode", "notify_mode", "提示方式 (0 静音 / 1 提示音 / 2 朗读)", "偏好" },
    { "idle_off_seconds", "idle_off_seconds", "静置熄屏秒数 (0 为不熄)", "偏好" },
};

int passport_portal_field_count(void)
{
    return (int)(sizeof(FIELDS) / sizeof(FIELDS[0]));
}

const passport_portal_field_t *passport_portal_field(int index)
{
    if (index < 0 || index >= passport_portal_field_count()) {
        return NULL;
    }
    return &FIELDS[index];
}

size_t passport_portal_form_value(const passport_config_t *cfg, const passport_cfg_field_t *field,
                                  char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return 0u;
    }
    out[0] = '\0';
    if (cfg == NULL || field == NULL) {
        return 0u;
    }
    const uint8_t *base = (const uint8_t *)cfg;
    switch (field->type) {
    case PASSPORT_CFG_TYPE_STR: {
        // The stored bytes, whatever they are. An empty field is an empty string, not the console's
        // "(empty)" — a form value that says "(empty)" is how a placeholder becomes data.
        const char *value = (const char *)(base + field->offset);
        const size_t len = strnlen(value, field->str_size);
        const size_t copy = (len < out_size - 1u) ? len : out_size - 1u;
        memcpy(out, value, copy);
        out[copy] = '\0';
        return copy;
    }
    case PASSPORT_CFG_TYPE_U8:
        return (size_t)snprintf(out, out_size, "%u", (unsigned)*(const uint8_t *)(base + field->offset));
    case PASSPORT_CFG_TYPE_U16:
        return (size_t)snprintf(out, out_size, "%u", (unsigned)*(const uint16_t *)(base + field->offset));
    default:
        return 0u;
    }
}

size_t passport_portal_escape(const char *in, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return 0u;
    }
    out[0] = '\0';
    if (in == NULL) {
        return 0u;
    }

    size_t written = 0u;
    for (size_t i = 0u; in[i] != '\0'; i++) {
        const char *replacement = NULL;
        switch (in[i]) {
        case '&':  replacement = "&amp;";  break;
        case '<':  replacement = "&lt;";   break;
        case '>':  replacement = "&gt;";   break;
        case '"':  replacement = "&quot;"; break;
        case '\'': replacement = "&#39;";  break;
        default:   break;
        }

        if (replacement == NULL) {
            if (written + 1u < out_size) {
                out[written++] = in[i];
            }
            continue;
        }
        for (const char *p = replacement; *p != '\0'; p++) {
            if (written + 1u < out_size) {
                out[written++] = *p;
            }
        }
    }
    out[written] = '\0';
    return written;
}

// JSON string escaping for the diagnostics body. Kept separate from the HTML escaper above because the
// two formats have different dangerous characters: "<" is harmless inside a JSON string and "'" needs no
// escape there, while a quote or a backslash would end the string.
static bool json_append(const char *in, char *out, size_t out_size, size_t *used)
{
    if (*used + 1u >= out_size) {
        return false;
    }
    out[(*used)++] = '"';
    for (const char *p = (in != NULL) ? in : ""; *p != '\0'; p++) {
        char escape[7];
        size_t len = 1u;
        escape[0] = *p;
        if (*p == '"' || *p == '\\') {
            escape[0] = '\\';
            escape[1] = *p;
            len = 2u;
        } else if ((unsigned char)*p < 0x20u) {
            static const char hex[] = "0123456789abcdef";
            escape[0] = '\\';
            escape[1] = 'u';
            escape[2] = '0';
            escape[3] = '0';
            escape[4] = hex[((unsigned char)*p) >> 4];
            escape[5] = hex[((unsigned char)*p) & 0x0f];
            len = 6u;
        }
        if (*used + len + 2u >= out_size) {
            return false;
        }
        memcpy(out + *used, escape, len);
        *used += len;
    }
    out[(*used)++] = '"';
    out[*used] = '\0';
    return true;
}

size_t passport_portal_diagnostics_json(const passport_portal_diagnostics_t *diag,
                                        char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u || diag == NULL) {
        return 0u;
    }
    out[0] = '\0';
    size_t used = 0u;

    // Everything is built into the caller's buffer and only handed over once complete, so a browser
    // either receives a whole object or nothing at all.
    char body[480];
    size_t n = 0u;
    const int prefix = snprintf(body, sizeof(body),
                                "{\"firmware\":");
    if (prefix <= 0) {
        return 0u;
    }
    n = (size_t)prefix;
    if (!json_append(diag->firmware_version, body, sizeof(body), &n)) {
        return 0u;
    }
    static const char rest[] = ",\"ip\":";
    if (n + sizeof(rest) >= sizeof(body)) {
        return 0u;
    }
    memcpy(body + n, rest, sizeof(rest) - 1u);
    n += sizeof(rest) - 1u;
    if (!json_append(diag->ip, body, sizeof(body), &n)) {
        return 0u;
    }
    static const char mid[] = ",\"wifi\":";
    if (n + sizeof(mid) >= sizeof(body)) {
        return 0u;
    }
    memcpy(body + n, mid, sizeof(mid) - 1u);
    n += sizeof(mid) - 1u;
    if (!json_append(diag->wifi_state, body, sizeof(body), &n)) {
        return 0u;
    }
    static const char mood[] = ",\"mood\":";
    if (n + sizeof(mood) >= sizeof(body)) {
        return 0u;
    }
    memcpy(body + n, mood, sizeof(mood) - 1u);
    n += sizeof(mood) - 1u;
    if (!json_append(diag->pet_mood, body, sizeof(body), &n)) {
        return 0u;
    }
    static const char voice[] = ",\"voice\":";
    if (n + sizeof(voice) >= sizeof(body)) {
        return 0u;
    }
    memcpy(body + n, voice, sizeof(voice) - 1u);
    n += sizeof(voice) - 1u;
    if (!json_append(diag->voice_state, body, sizeof(body), &n)) {
        return 0u;
    }
    const int numbers = snprintf(body + n, sizeof(body) - n,
                                 ",\"heap\":%u,\"largest\":%u,\"uptime\":%u,\"turns\":%u,"
                                 "\"rssi\":%d,\"mic\":%s}",
                                 diag->heap_free, diag->heap_largest, diag->uptime_seconds,
                                 diag->turns, diag->rssi, diag->microphone_open ? "true" : "false");
    if (numbers <= 0 || (size_t)numbers >= sizeof(body) - n) {
        return 0u;
    }
    n += (size_t)numbers;
    if (n + 1u > out_size) {
        return 0u;
    }
    memcpy(out, body, n + 1u);
    used = n;
    return used;
}
