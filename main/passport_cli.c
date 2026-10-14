// main/passport_cli.c
//
// A serial console that lets a host inspect the live LVGL tree and drive the
// device through the same input path a person uses.
//
// Design notes:
//   * Element identity comes from LVGL's named-object support, not from a
//     parallel table. `ui tree` prints the resolved name of every object, so the
//     name printed for a widget is exactly the name `ui get`/`ui tap` accept.
//   * Every LVGL access happens under bsp_lvgl_lock(), because this runs in the
//     console task and LVGL is not thread-safe.
//   * Button injection goes through the application sink, so navigation behaves
//     identically to a physical press.
#include "passport_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "passport_app.h"
#include "passport_audio.h"
#include "passport_chat.h"
#include "passport_config.h"
#include "passport_config_store.h"
#include "passport_http.h"
#include "passport_json.h"
#include "passport_fonts.h"
#include "passport_utf8.h"
#include "passport_notify.h"
#include "passport_portal.h"
#include "passport_power.h"
#include "passport_provision.h"
#include "passport_qr.h"
#include "passport_speech.h"
#include "passport_work.h"
#include "passport_work_client.h"
#include "passport_wifi.h"

#define CLI_NAME_MAX 96
#define CLI_TAP_ANCESTOR_MAX 6
#define SCAN_LIST_MAX 20

static passport_cli_key_sink_t s_key_sink;
static bool s_started;

// --- helpers ----------------------------------------------------------------

// lv_obj_class_t is opaque outside LVGL's private headers, so the class is
// identified by comparing the public class symbols rather than reading a field.
// The object's *name* is the meaningful identifier for the console anyway.
static const char *class_name(const lv_obj_t *obj)
{
    const lv_obj_class_t *cls = lv_obj_get_class(obj);
    if (cls == &lv_label_class) {
        return "label";
    }
    if (cls == &lv_obj_class) {
        return "obj";
    }
    return "widget";
}

// Resolve a display name without LVGL's resolver. lv_obj_get_name_resolved()
// faults on an unnamed root screen in this LVGL version, so a user-set name is
// read directly and anything else gets a synthetic label from its class plus its
// position among its siblings.
static void safe_name(const lv_obj_t *obj, int index, char *buf, size_t size)
{
    const char *set = lv_obj_get_name(obj);
    if (set != NULL) {
        snprintf(buf, size, "%s", set);
    } else {
        snprintf(buf, size, "%s_%d", class_name(obj), index);
    }
}

// Printable text of a label, or NULL for anything else.
static const char *label_text(const lv_obj_t *obj)
{
    if (lv_obj_get_class(obj) != &lv_label_class) {
        return NULL;
    }
    return lv_label_get_text(obj);
}

static void print_metrics(const char *tag)
{
    const size_t free_heap = esp_get_free_heap_size();
    const size_t min_heap = esp_get_minimum_free_heap_size();
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    printf("[%s] free=%u B (%u KiB)  min=%u B  largest=%u B (%u KiB)\n",
           tag, (unsigned)free_heap, (unsigned)(free_heap / 1024u),
           (unsigned)min_heap, (unsigned)largest, (unsigned)(largest / 1024u));

    if (bsp_lvgl_lock(500)) {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        printf("[%s] lvgl free=%u biggest=%u frag=%u%% max_used=%u\n",
               tag, (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
               (unsigned)mon.frag_pct, (unsigned)mon.max_used);
        bsp_lvgl_unlock();
    } else {
        printf("[%s] lvgl metrics unavailable: LVGL lock timeout\n", tag);
    }
}

// --- ui commands ------------------------------------------------------------

static void walk_tree(lv_obj_t *obj, int index, int depth, int max_depth)
{
    char name[CLI_NAME_MAX];
    char textbuf[CLI_NAME_MAX + 8];
    safe_name(obj, index, name, sizeof(name));

    const char *text = label_text(obj);
    if (text != NULL) {
        snprintf(textbuf, sizeof(textbuf), "  \"%s\"", text);
    } else {
        textbuf[0] = '\0';
    }

    printf("%*s%-26s %-10s %4d,%4d %4dx%-4d%s%s\n", depth * 2, "", name, class_name(obj),
           (int)lv_obj_get_x(obj), (int)lv_obj_get_y(obj),
           (int)lv_obj_get_width(obj), (int)lv_obj_get_height(obj),
           lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) ? " hidden" : "", textbuf);

    if (depth >= max_depth) {
        const uint32_t children = lv_obj_get_child_count(obj);
        if (children > 0u) {
            printf("%*s... %u child object(s) below\n", (depth + 1) * 2, "", (unsigned)children);
        }
        return;
    }
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0u; i < count; i++) {
        walk_tree(lv_obj_get_child(obj, (int32_t)i), (int)i, depth + 1, max_depth);
    }
}

// Resolve a slash-separated path against the active screen.
//
// A segment matches either the name the interface set or the synthesized handle the
// listings print, so every path printed by "ui names" and "ui tree" is also addressable
// by "ui get" and "ui tap". Before this, the listings happily printed "obj_3/bubble"
// while the resolver refused it, which made the answer text in the conversation panel
// unreadable over the console — the one string that proves the model replied.
static lv_obj_t *find_by_path(lv_obj_t *parent, const char *path)
{
    const char *slash = strchr(path, '/');
    const size_t len = (slash != NULL) ? (size_t)(slash - path) : strlen(path);
    if (len == 0u || len >= CLI_NAME_MAX) {
        return NULL;
    }
    char segment[CLI_NAME_MAX];
    memcpy(segment, path, len);
    segment[len] = '\0';

    const uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0u; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(parent, (int32_t)i);
        char candidate[CLI_NAME_MAX];
        safe_name(child, (int)i, candidate, sizeof(candidate));
        if (strcmp(candidate, segment) != 0) {
            continue;
        }
        return (slash == NULL) ? child : find_by_path(child, slash + 1);
    }
    return NULL;
}

static lv_obj_t *resolve(const char *term)
{
    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL || term == NULL) {
        return NULL;
    }
    // Accept the screen's own name as a leading segment, because that is how the
    // listings present every path.
    char screen_name[CLI_NAME_MAX];
    safe_name(screen, 0, screen_name, sizeof(screen_name));
    const size_t screen_len = strlen(screen_name);
    if (strncmp(term, screen_name, screen_len) == 0 && term[screen_len] == '/') {
        term += screen_len + 1u;
    }

    lv_obj_t *found = find_by_path(screen, term);
    if (found != NULL) {
        return found;
    }
    // Fall back to another screen of the display, addressed by name. An inactive screen is
    // still part of what the device can show — the setup screen holds the hotspot
    // credentials while the home screen is in front, and both have to be readable.
    lv_display_t *display = lv_display_get_default();
    if (display != NULL) {
        const char *slash = strchr(term, '/');
        const size_t head_len = (slash != NULL) ? (size_t)(slash - term) : strlen(term);
        if (head_len > 0u && head_len < CLI_NAME_MAX) {
            char head[CLI_NAME_MAX];
            memcpy(head, term, head_len);
            head[head_len] = '\0';
            lv_obj_t *other = lv_display_get_screen_by_name(display, head);
            if (other != NULL && other != screen) {
                return (slash == NULL) ? other : find_by_path(other, slash + 1);
            }
        }
    }
    return NULL;
}

// A tap should land on something clickable: if the named object is a plain
// label, walk up to the nearest clickable ancestor, which is what a finger
// actually hits.
static lv_obj_t *clickable_target(lv_obj_t *obj)
{
    lv_obj_t *cur = obj;
    for (int i = 0; i < CLI_TAP_ANCESTOR_MAX && cur != NULL; i++) {
        if (lv_obj_has_flag(cur, LV_OBJ_FLAG_CLICKABLE)) {
            return cur;
        }
        cur = lv_obj_get_parent(cur);
    }
    return obj;
}

static int cmd_ui_tree(int argc, char **argv)
{
    int max_depth = 6;
    if (argc > 1) {
        max_depth = atoi(argv[1]);
        if (max_depth < 0) {
            max_depth = 0;
        }
    }
    if (!bsp_lvgl_lock(1000)) {
        printf("ui tree: LVGL lock timeout\n");
        return 1;
    }
    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        printf("ui tree: no active screen\n");
        bsp_lvgl_unlock();
        return 1;
    }
    printf("active screen:\n");
    walk_tree(screen, 0, 0, max_depth);
    bsp_lvgl_unlock();
    return 0;
}

// Print every object as a path relative to the active screen. These are exactly
// the strings that `ui get` and `ui tap` accept, which is what ties the console
// list to what is drawn on the display.
static void print_names(lv_obj_t *obj, int index, const char *prefix)
{
    char name[CLI_NAME_MAX];
    char path[CLI_NAME_MAX * 2];
    safe_name(obj, index, name, sizeof(name));
    if (prefix[0] == '\0') {
        snprintf(path, sizeof(path), "%s", name);
    } else {
        snprintf(path, sizeof(path), "%s/%s", prefix, name);
    }
    const char *text = label_text(obj);
    if (text != NULL) {
        printf("%-34s %-10s \"%s\"\n", path, class_name(obj), text);
    } else {
        printf("%-34s %s\n", path, class_name(obj));
    }
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0u; i < count; i++) {
        print_names(lv_obj_get_child(obj, (int32_t)i), (int)i, path);
    }
}

static int cmd_ui_names(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!bsp_lvgl_lock(1000)) {
        printf("ui names: LVGL lock timeout\n");
        return 1;
    }
    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        printf("ui names: no active screen\n");
        bsp_lvgl_unlock();
        return 1;
    }
    char sname[CLI_NAME_MAX];
    safe_name(screen, 0, sname, sizeof(sname));
    printf("%-34s %-10s (active screen)\n", sname, class_name(screen));
    const uint32_t count = lv_obj_get_child_count(screen);
    for (uint32_t i = 0u; i < count; i++) {
        print_names(lv_obj_get_child(screen, (int32_t)i), (int)i, "");
    }
    bsp_lvgl_unlock();
    return 0;
}

static int cmd_ui_get(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: ui get <name-or-path>\n");
        return 1;
    }
    if (!bsp_lvgl_lock(1000)) {
        printf("ui get: LVGL lock timeout\n");
        return 1;
    }
    lv_obj_t *obj = resolve(argv[1]);
    if (obj == NULL) {
        printf("ui get: nothing named \"%s\" on the active screen\n", argv[1]);
        bsp_lvgl_unlock();
        return 1;
    }
    char name[CLI_NAME_MAX];
    safe_name(obj, 0, name, sizeof(name));
    const char *text = label_text(obj);
    printf("%-26s %-10s %4d,%4d %4dx%-4d%s%s\n", name, class_name(obj),
           (int)lv_obj_get_x(obj), (int)lv_obj_get_y(obj),
           (int)lv_obj_get_width(obj), (int)lv_obj_get_height(obj),
           lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) ? " hidden" : "",
           lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) ? " clickable" : "");
    if (text != NULL) {
        printf("text: %s\n", text);
    }
    bsp_lvgl_unlock();
    return 0;
}

static int cmd_ui_tap(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: ui tap <name-or-path>\n");
        return 1;
    }
    if (!bsp_lvgl_lock(1000)) {
        printf("ui tap: LVGL lock timeout\n");
        return 1;
    }
    lv_obj_t *obj = resolve(argv[1]);
    if (obj == NULL) {
        printf("ui tap: nothing named \"%s\" on the active screen\n", argv[1]);
        bsp_lvgl_unlock();
        return 1;
    }
    lv_obj_t *target = clickable_target(obj);
    char name[CLI_NAME_MAX];
    safe_name(target, 0, name, sizeof(name));
    const lv_result_t res = lv_obj_send_event(target, LV_EVENT_CLICKED, NULL);
    printf("ui tap: sent CLICKED to %s (%s) -> %s\n", name, class_name(target),
           res == LV_RESULT_OK ? "handled" : "no handler / deleted");
    bsp_lvgl_unlock();
    return 0;
}

static int cmd_ui_screen(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!bsp_lvgl_lock(1000)) {
        printf("ui screen: LVGL lock timeout\n");
        return 1;
    }
    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        printf("ui screen: no active screen\n");
        bsp_lvgl_unlock();
        return 1;
    }
    char name[CLI_NAME_MAX];
    safe_name(screen, 0, name, sizeof(name));
    printf("screen %s  %dx%d  children=%u\n", name,
           (int)lv_obj_get_width(screen), (int)lv_obj_get_height(screen),
           (unsigned)lv_obj_get_child_count(screen));
    bsp_lvgl_unlock();
    return 0;
}

static bool parse_btn(const char *s, bsp_btn_t *out)
{
    if (strcmp(s, "up") == 0) { *out = BSP_BTN_UP; return true; }
    if (strcmp(s, "down") == 0) { *out = BSP_BTN_DOWN; return true; }
    if (strcmp(s, "ok") == 0) { *out = BSP_BTN_OK; return true; }
    return false;
}

static bool parse_ev(const char *s, bsp_btn_ev_t *out)
{
    if (strcmp(s, "press") == 0) { *out = BSP_BTN_PRESS; return true; }
    if (strcmp(s, "click") == 0) { *out = BSP_BTN_CLICK; return true; }
    if (strcmp(s, "double") == 0) { *out = BSP_BTN_DOUBLE; return true; }
    if (strcmp(s, "long") == 0) { *out = BSP_BTN_LONG; return true; }
    if (strcmp(s, "release") == 0) { *out = BSP_BTN_RELEASE; return true; }
    return false;
}

static int cmd_ui_key(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: ui key <up|down|ok> <press|click|double|long|release>\n");
        return 1;
    }
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
    if (!parse_btn(argv[1], &btn)) {
        printf("ui key: unknown button \"%s\"\n", argv[1]);
        return 1;
    }
    if (!parse_ev(argv[2], &ev)) {
        printf("ui key: unknown event \"%s\"\n", argv[2]);
        return 1;
    }
    if (s_key_sink == NULL) {
        printf("ui key: no input sink registered by the application\n");
        return 1;
    }
    s_key_sink(btn, ev);
    printf("ui key: injected %s %s\n", argv[1], argv[2]);
    return 0;
}

// --- app and system commands ------------------------------------------------

static int cmd_ver(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const esp_app_desc_t *app = esp_app_get_description();
    printf("project   : %s\n", app->project_name);
    printf("version   : %s\n", app->version);
    printf("built     : %s %s\n", app->date, app->time);
    printf("esp-idf   : %s\n", app->idf_ver);
    return 0;
}

static int cmd_app(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const int soc = bsp_battery_soc();
    printf("uptime    : %lld ms\n", (long long)(esp_timer_get_time() / 1000));
    printf("battery   : %s\n", soc >= 0 ? "" : "unavailable");
    if (soc >= 0) {
        printf("            soc=%d%%\n", soc);
    }
    printf("keypad    : %d mV\n", bsp_button_read_mv());
    print_metrics("app");
    return 0;
}

static int cmd_mem(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    print_metrics("mem");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("rebooting\n");
    fflush(stdout);
    esp_restart();
    return 0;  // not reached
}

static int cmd_audio(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: audio <status|tone [ms]|loopback [ms]|mute-check>\n");
        printf("  status           which subsystem holds the audio path\n");
        printf("  tone [ms]        play a 1 kHz tone (default 300 ms)\n");
        printf("  loopback [ms]    record, then play it back (default 1000 ms, max 2000)\n");
        printf("  mute-check       prove a capture is refused while playback holds the path\n");
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        const passport_audio_owner_t owner = passport_audio_owner();
        printf("audio owner : %s\n", passport_audio_owner_name(owner));
        printf("microphone  : %s\n", passport_audio_arbiter_mic_allowed(owner)
                                        ? "allowed"
                                        : "closed (playback holds the path)");
        return 0;
    }

    if (strcmp(argv[1], "tone") == 0) {
        int ms = (argc > 2) ? atoi(argv[2]) : 300;
        if (ms <= 0) { ms = 300; }
        if (ms > 3000) { ms = 3000; }
        const size_t samples = (size_t)ms * 16u;   // 16 samples per ms at 16 kHz
        int16_t *buf = malloc(samples * sizeof(int16_t));
        if (buf == NULL) {
            printf("audio tone: %u B allocation failed\n", (unsigned)(samples * sizeof(int16_t)));
            return 1;
        }
        passport_audio_fill_tone(buf, samples);
        const esp_err_t err = passport_audio_play(buf, samples);
        free(buf);
        printf("audio tone: %d ms -> %s\n", ms, esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }

    if (strcmp(argv[1], "loopback") == 0) {
        int ms = (argc > 2) ? atoi(argv[2]) : 1000;
        if (ms <= 0) { ms = 1000; }
        if (ms > 2000) { ms = 2000; }
        const size_t samples = (size_t)ms * 16u;
        int16_t *buf = malloc(samples * sizeof(int16_t));
        if (buf == NULL) {
            printf("audio loopback: %u B allocation failed\n", (unsigned)(samples * sizeof(int16_t)));
            return 1;
        }

        passport_audio_level_t level = { 0u, 0u, 0u };
        printf("audio loopback: recording %d ms (%u samples, %u B)\n", ms,
               (unsigned)samples, (unsigned)(samples * sizeof(int16_t)));
        esp_err_t err = passport_audio_capture(buf, samples, &level);
        printf("  captured %u samples  rms=%u peak=%u\n",
               (unsigned)level.samples, level.rms, level.peak);
        if (err == ESP_OK && level.peak == 0u) {
            printf("  the microphone heard nothing: silence, or a dead input path\n");
        }
        if (err == ESP_OK) {
            printf("  playing back; a capture during playback is refused\n");
            err = passport_audio_play(buf, samples);
            printf("  playback %s (owner now %s)\n",
                   (err == ESP_OK) ? "done" : esp_err_to_name(err),
                   passport_audio_owner_name(passport_audio_owner()));
        }
        free(buf);
        printf("audio loopback: %s\n", (err == ESP_OK) ? "ok" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }

    if (strcmp(argv[1], "mute-check") == 0) {
        bool refused = false;
        const esp_err_t err = passport_audio_verify_mute(&refused);
        if (err != ESP_OK) {
            printf("audio mute-check: could not take the audio path: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("audio mute-check: capture refused while playback held the path: %s\n",
               refused ? "yes" : "NO - the mute rule is broken");
        return refused ? 0 : 1;
    }

    printf("audio: unknown subcommand \"%s\"\n", argv[1]);
    return 1;
}

static void print_http_result(const char *label, const passport_http_result_t *res,
                              const char *body)
{
    printf("%s: HTTP %d, %d bytes%s, %lld ms\n", label, res->status, res->body_len,
           res->truncated ? " (truncated)" : "", (long long)res->elapsed_ms);
    printf("  with the session live: free heap %u B, largest free block %u B (%u KiB)\n",
           (unsigned)res->heap_during, (unsigned)res->largest_during,
           (unsigned)(res->largest_during / 1024u));
    if (body != NULL && body[0] != '\0') {
        printf("  body: %.240s\n", body);
    }
}

static int cmd_net(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[1], "probe") != 0) {
        printf("usage: net probe <url>\n");
        printf("  GET a URL over HTTPS and report the status, the bytes, and the heap\n");
        printf("  measured while the TLS session was open\n");
        return 1;
    }

    char body[512];
    passport_http_result_t res;
    const passport_http_request_t req = { .url = argv[2], .timeout_ms = 15000u };
    const esp_err_t err = passport_http_request(&req, &res, body, sizeof(body));
    if (err != ESP_OK) {
        printf("net probe: transport failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    print_http_result("net probe", &res, body);
    return 0;
}

// Escape a user prompt so it can be embedded in a JSON string body.
static void json_escape(const char *in, char *out, size_t out_size)
{
    size_t j = 0u;
    for (size_t i = 0u; in[i] != '\0' && j + 2u < out_size; i++) {
        const char c = in[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = c;
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if ((unsigned char)c < 0x20u) {
            out[j++] = ' ';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

// Build "<base>/chat/completions", leaving an already-complete endpoint alone.
static void build_endpoint(const char *base, char *out, size_t out_size)
{
    if (strstr(base, "/chat/completions") != NULL) {
        snprintf(out, out_size, "%s", base);
    } else {
        snprintf(out, out_size, "%s/chat/completions", base);
    }
}

static int cmd_llm(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "probe") != 0 && strcmp(argv[1], "ask") != 0)) {
        printf("usage: llm <probe|ask <text>>\n");
        printf("  probe        POST a minimal chat completion; a 401 still proves the transport\n");
        printf("  ask <text>   ask a question and print the reply\n");
        return 1;
    }

    const passport_config_t *cfg = passport_config_current();
    if (cfg->llm_base_url[0] == '\0' || cfg->llm_model[0] == '\0') {
        printf("llm: set the endpoint and model first:\n");
        printf("  config set llm_base_url https://api.deepseek.com\n");
        printf("  config set llm_model <model>\n");
        printf("  config set llm_api_key <key>\n");
        return 1;
    }

    char url[192];
    build_endpoint(cfg->llm_base_url, url, sizeof(url));

    if (strcmp(argv[1], "ask") == 0) {
        if (argc < 3) {
            printf("usage: llm ask <text>\n");
            return 1;
        }

        // Join the remaining arguments so a prompt can contain spaces without
        // needing quotes from the caller.
        char prompt[200];
        size_t used = 0u;
        prompt[0] = '\0';
        for (int i = 2; i < argc; i++) {
            const size_t n = strlen(argv[i]);
            if (used + n + 2u >= sizeof(prompt)) {
                break;
            }
            if (used > 0u) {
                prompt[used++] = ' ';
            }
            memcpy(prompt + used, argv[i], n);
            used += n;
            prompt[used] = '\0';
        }
        if (prompt[0] == '\0') {
            printf("llm ask: empty prompt\n");
            return 1;
        }

        char escaped[320];
        json_escape(prompt, escaped, sizeof(escaped));

        char body[640];
        snprintf(body, sizeof(body),
                 "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
                 "\"max_tokens\":256,\"stream\":false}",
                 cfg->llm_model, escaped);

        // Heap, not stack: this task's stack already has to hold a TLS handshake,
        // and the response buffer is the largest thing here.
        char *resp = malloc(2048);
        char *answer = malloc(1024);
        if (resp == NULL || answer == NULL) {
            free(resp);
            free(answer);
            printf("llm ask: out of memory for the response buffers\n");
            return 1;
        }

        passport_http_result_t res;
        const passport_http_request_t req = {
            .url = url,
            .api_key = cfg->llm_api_key,
            .body = body,
            .timeout_ms = 30000u,
        };

        printf("llm ask: POST %s (model %s, key %s, prompt %u B)\n", url, cfg->llm_model,
               (cfg->llm_api_key[0] != '\0') ? "set" : "not set", (unsigned)strlen(prompt));
        const esp_err_t err = passport_http_request(&req, &res, resp, 2048);

        int rc = 1;
        if (err != ESP_OK) {
            printf("llm ask: transport failed: %s\n", esp_err_to_name(err));
        } else if (res.status != 200) {
            printf("llm ask: HTTP %d\n  body: %.200s\n", res.status, resp);
        } else {
            size_t decoded = 0u;
            const passport_json_status_t js =
                passport_json_string(resp, strlen(resp), "content", answer, 1024, &decoded);
            printf("  %lld ms, %d B response, largest free block with the session live: %u KiB\n",
                   (long long)res.elapsed_ms, res.body_len,
                   (unsigned)(res.largest_during / 1024u));
            if (js == PASSPORT_JSON_OK) {
                printf("  reply: %s\n", answer);
                rc = 0;
            } else if (js == PASSPORT_JSON_TRUNCATED) {
                printf("  reply (truncated from %u B): %s\n", (unsigned)decoded, answer);
                rc = 0;
            } else {
                printf("  could not read the reply (status %d); raw body: %.200s\n",
                       (int)js, resp);
            }
        }
        free(resp);
        free(answer);
        return rc;
    }

    char body[256];
    snprintf(body, sizeof(body),
             "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}],"
             "\"max_tokens\":1,\"stream\":false}",
             cfg->llm_model);

    char resp[768];
    passport_http_result_t res;
    const passport_http_request_t req = {
        .url = url,
        .api_key = cfg->llm_api_key,
        .body = body,
        .timeout_ms = 20000u,
    };

    printf("llm probe: POST %s (model %s, key %s)\n", url, cfg->llm_model,
           (cfg->llm_api_key[0] != '\0') ? "set" : "not set - a 401 is expected");
    const esp_err_t err = passport_http_request(&req, &res, resp, sizeof(resp));
    if (err != ESP_OK) {
        printf("llm probe: transport failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    print_http_result("llm probe", &res, resp);
    if (res.status == 401) {
        printf("  401 means the transport worked and the credential was rejected.\n");
    }
    return (res.status == 200) ? 0 : 1;
}

// The conversation-text probe. Written as escapes so the console never has to accept
// non-ASCII input, which it does unreliably. The emoji is a deliberate negative control:
// it lies outside the CJK font's coverage, so a check that reports zero missing glyphs
// would be reporting nothing at all.
#define FONT_PROBE "\xE4\xB8\xAD\xE6\x96\x87\xEF\xBC\x8C ABC 123\xE3\x80\x82\xF0\x9F\x98\x80"

// The application's palette, used to tell text from everything that is not text in a
// snapshot: the panel, the screen behind its rounded corners, and the accent bar.
#define PANEL_RGB565  0x18233A
#define SCREEN_RGB565 0x101828
#define ACCENT_RGB565 0x4D6BFE

// A rough luma for RGB565, used by the snapshot's dark mode. Light text on a dark panel and
// dark modules on a light quiet zone need opposite definitions of "ink", so the caller picks.
static bool pixel_is_dark(uint16_t px)
{
    const unsigned r = (px >> 11) & 0x1Fu;
    const unsigned g = (px >> 5) & 0x3Fu;
    const unsigned b = px & 0x1Fu;
    return (r * 3u + g * 6u + b * 3u) < 282u;   // half of the maximum weighted sum
}

static int cmd_font(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "probe") != 0 && strcmp(argv[1], "shot") != 0 &&
                     strcmp(argv[1], "check") != 0)) {
        printf("usage: font <probe|check|shot [path] [dark]>\n");
        printf("  probe        report coverage for a fixed Chinese sample and show it\n");
        printf("  check        report coverage for the text actually on the display\n");
        printf("  shot [path]  render an object's pixels as ASCII art (default chat)\n");
        printf("               add \"dark\" for dark-on-light content such as the setup code\n");
        return 1;
    }

    if (strcmp(argv[1], "check") == 0) {
        // Check what is really on the screen rather than a sample someone chose. A probe
        // built from characters the font happens to include will pass while the model's
        // actual answer renders as empty placeholder boxes.
        if (!bsp_lvgl_lock(1000)) {
            printf("font check: could not take the LVGL lock\n");
            return 1;
        }
        char copy[256];
        copy[0] = '\0';
        const lv_font_t *font = passport_font_body();
        lv_obj_t *label = resolve("chat/bubble");
        if (label != NULL) {
            const char *text = lv_label_get_text(label);
            if (text != NULL) {
                snprintf(copy, sizeof(copy), "%s", text);
            }
            const lv_font_t *active = lv_obj_get_style_text_font(label, LV_PART_MAIN);
            if (active != NULL) {
                font = active;
            }
        }
        bsp_lvgl_unlock();

        if (copy[0] == '\0') {
            printf("font check: the conversation bubble is empty\n");
            return 1;
        }
        char report[256];
        const int missing = passport_font_check(font, copy, report, sizeof(report));
        printf("font check: %u code points on screen; %d have no real glyph\n",
               (unsigned)passport_utf8_count(copy), missing);
        printf("  text   : %s\n", copy);
        printf("  missing: %s\n", (report[0] != '\0') ? report : "(none)");
        return (missing == 0) ? 0 : 1;
    }

    if (strcmp(argv[1], "probe") == 0) {
        char report[256];
        const int missing = passport_font_check(passport_font_body(), FONT_PROBE, report,
                                                sizeof(report));
        printf("font probe: sample is %u code points; %d have no real glyph\n",
               (unsigned)passport_utf8_count(FONT_PROBE), missing);
        printf("  missing: %s\n", (report[0] != '\0') ? report : "(none)");

        passport_app_show_reply(FONT_PROBE);
        printf("  sample written to the conversation bubble; read it back with"
               " \"ui get chat/bubble\"\n");
        return (missing == 0) ? 0 : 1;
    }

    // Snapshot the panel rather than the label: the label is transparent, so its
    // snapshot would have no background to compare ink against.
    const char *target_path = (argc > 2) ? argv[2] : "chat";
    if (!bsp_lvgl_lock(2000)) {
        printf("font shot: could not take the LVGL lock\n");
        return 1;
    }
    lv_obj_t *panel = resolve(target_path);
    if (panel == NULL) {
        bsp_lvgl_unlock();
        printf("font shot: nothing named \"%s\" on the active screen\n", target_path);
        return 1;
    }

    // The panel is far larger than LVGL's own memory pool, which is where
    // lv_snapshot_take() would try to allocate the image — that is why it failed here.
    // The buffer is therefore provided from the system heap instead.
    const uint32_t width = (uint32_t)lv_obj_get_width(panel);
    const uint32_t height = (uint32_t)lv_obj_get_height(panel);
    const uint32_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
    const size_t data_size = (size_t)stride * (size_t)height;

    void *pixels = heap_caps_aligned_alloc(64u, data_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        bsp_lvgl_unlock();
        printf("font shot: could not allocate %u B for the snapshot\n", (unsigned)data_size);
        return 1;
    }

    lv_draw_buf_t canvas;
    if (lv_draw_buf_init(&canvas, width, height, LV_COLOR_FORMAT_RGB565, stride, pixels,
                         (uint32_t)data_size) != LV_RESULT_OK ||
        lv_snapshot_take_to_draw_buf(panel, LV_COLOR_FORMAT_RGB565, &canvas) != LV_RESULT_OK) {
        heap_caps_free(pixels);
        bsp_lvgl_unlock();
        printf("font shot: the snapshot could not be rendered\n");
        return 1;
    }

    const uint16_t background = lv_color_to_u16(lv_color_hex(PANEL_RGB565));
    const uint16_t screen_bg = lv_color_to_u16(lv_color_hex(SCREEN_RGB565));
    const bool dark_ink = (argc > 3 && strcmp(argv[3], "dark") == 0);

    // Ink is anything that is not one of the two dark backgrounds. Colours are compared
    // by exclusion rather than by matching the text colour, so this works on the setup
    // screen as well, where the text is partly the accent colour. In dark mode the test is
    // inverted, which is what makes a black-on-white symbol legible here.
    #define IS_INK(px) (dark_ink ? pixel_is_dark(px) : ((px) != background && (px) != screen_bg))

    printf("font shot: %s,%s %ux%u RGB565, stride %u, %u B from the system heap\n", target_path,
           dark_ink ? " dark ink," : "",
           (unsigned)width, (unsigned)height, (unsigned)stride, (unsigned)data_size);

    for (uint32_t y = 0u; y < height; y++) {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)pixels + ((size_t)y * stride));
        bool has_ink = false;
        for (uint32_t x = 6u; x < width; x++) {   // skip the accent bar
            if (IS_INK(row[x])) {
                has_ink = true;
                break;
            }
        }
        if (!has_ink) {
            continue;   // skip the empty bands between the panel edge and the text
        }
        char line[320];
        int used = 0;
        for (uint32_t x = 6u; x < width && used < (int)sizeof(line) - 2; x++) {
            line[used++] = IS_INK(row[x]) ? '#' : '.';
        }
        line[used] = '\0';
        printf("%s\n", line);
    }

    heap_caps_free(pixels);
    bsp_lvgl_unlock();
    return 0;
}

// Work mode over the console. The view itself lives in the application, so a document fed here
// updates exactly what a button press would and there is only one copy of the state.
static void print_work_view(const passport_work_view_t *view)
{
    if (view == NULL) {
        return;
    }
    printf("work: state=%s progress=%d%% task=\"%s\"\n", passport_work_state_name(view->state),
           view->progress, view->task);
    if (view->option_count > 0) {
        printf("work: question=\"%s\"\n", view->question);
        for (int i = 0; i < view->option_count; i++) {
            printf("  %s option%d: %s\n", (i == view->selected) ? ">" : " ", i, view->options[i]);
        }
    }
    if (view->answered) {
        printf("work: this question has already been answered\n");
    }
}

static int cmd_voice(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: voice <status|hold|release|gate [peak]|say <text>>\n");
        printf("  status       the turn state, the microphone, and the last transcript\n");
        printf("  hold/release drive a push-to-talk turn without a finger on the button\n");
        printf("  gate [peak]  show or set the silence threshold (0 disables it)\n");
        printf("  say <text>   feed a transcript straight to the model, skipping the recording\n");
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        char report[256];
        passport_speech_report(report, sizeof(report));
        printf("voice: %s\n", report);
        printf("voice config: stt_base_url=%s stt_model=%s tts_voice=%s\n",
               (passport_config_current()->stt_base_url[0] != '\0')
                   ? passport_config_current()->stt_base_url : "(empty)",
               (passport_config_current()->stt_model[0] != '\0')
                   ? passport_config_current()->stt_model : "(empty)",
               (passport_config_current()->tts_voice[0] != '\0')
                   ? passport_config_current()->tts_voice : "(empty)");
        return 0;
    }

    if (strcmp(argv[1], "gate") == 0) {
        char report[256];
        passport_speech_report(report, sizeof(report));
        if (argc < 3) {
            printf("voice gate: %s\n", report);
            printf("  (a number pins the threshold; 0 never treats anything as silence)\n");
            return 0;
        }
        const int value = atoi(argv[2]);
        if (value < 0) {
            printf("voice gate: a negative gate is meaningless\n");
            return 1;
        }
        passport_speech_set_silence_peak(value);
        printf("voice gate: %d\n", passport_speech_silence_peak());
        return 0;
    }

    if (strcmp(argv[1], "speak") == 0) {
        // Play the model's last answer, or the text given, through the bridge's synthesis. This is
        // how the playback path is checked without speaking into the microphone.
        if (argc > 2) {
            static char sentence[256];
            size_t used = 0u;
            sentence[0] = '\0';
            for (int i = 2; i < argc; i++) {
                const size_t n = strlen(argv[i]);
                if (used + n + 2u >= sizeof(sentence)) {
                    break;
                }
                if (used > 0u) {
                    sentence[used++] = ' ';
                }
                memcpy(sentence + used, argv[i], n);
                used += n;
                sentence[used] = '\0';
            }
            const esp_err_t err = passport_speech_speak(sentence);
            printf("voice speak: %s\n", esp_err_to_name(err));
            return (err == ESP_OK) ? 0 : 1;
        }
        const esp_err_t err = passport_speech_speak(NULL);
        printf("voice speak: %s (the model's last answer)\n", esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }

    if (strcmp(argv[1], "hold") == 0) {
        passport_speech_talk_down();
        printf("voice hold: the microphone should be open (see \"voice status\")\n");
        return 0;
    }

    if (strcmp(argv[1], "release") == 0) {
        passport_speech_talk_up();
        printf("voice release: the turn is finishing on the speech task\n");
        return 0;
    }

    if (strcmp(argv[1], "say") == 0) {
        if (argc < 3) {
            printf("usage: voice say <text>\n");
            return 1;
        }
        static char sentence[256];
        size_t used = 0u;
        sentence[0] = '\0';
        for (int i = 2; i < argc; i++) {
            const size_t n = strlen(argv[i]);
            if (used + n + 2u >= sizeof(sentence)) {
                break;
            }
            if (used > 0u) {
                sentence[used++] = ' ';
            }
            memcpy(sentence + used, argv[i], n);
            used += n;
            sentence[used] = '\0';
        }
        const esp_err_t err = passport_chat_ask(sentence);
        printf("voice say: %s\n", esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }

    printf("voice: unknown subcommand \"%s\"\n", argv[1]);
    return 1;
}

// --- the font audit ---------------------------------------------------------
//
// The repository's rule is that Chinese interface text must be checked for glyph coverage, not
// assumed from a successful build. This walks the interface and applies that rule to the running
// device: for every label whose text contains a non-ASCII character, ask the font that label is
// actually drawn with whether it can render that character.
//
// It is worth being a runtime check rather than a source review because the two facts live in
// different places — the text is set somewhere, the font is chosen somewhere else, and the default
// font has no CJK glyphs at all. A label given Chinese text and the default font renders as empty
// boxes, which a build cannot see.
static void audit_object(lv_obj_t *obj, const char *screen, int *checked, int *missing)
{
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *text = lv_label_get_text(obj);
        (*checked)++;
        size_t index = 0u;
        uint32_t codepoint = 0u;
        while (passport_utf8_next(text, &index, &codepoint) != 0u) {
            if (codepoint < 0x80u) {
                continue;
            }
            const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
            lv_font_glyph_dsc_t dsc;
            if (font != NULL && !lv_font_get_glyph_dsc(font, &dsc, codepoint, 0)) {
                char name[CLI_NAME_MAX];
                safe_name(obj, 0, name, sizeof(name));
                printf("  MISSING: %s/%s can not draw U+%04X, so \"%s\" has holes\n", screen, name,
                       (unsigned)codepoint, text);
                (*missing)++;
                break;   // one finding per label is enough
            }
        }
    }
    const uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0u; i < children; i++) {
        audit_object(lv_obj_get_child(obj, (int32_t)i), screen, checked, missing);
    }
}

static int cmd_ui_audit(void)
{
    if (!bsp_lvgl_lock(1500)) {
        printf("ui audit: LVGL lock timeout\n");
        return 1;
    }
    lv_display_t *display = lv_display_get_default();
    if (display == NULL) {
        printf("ui audit: no display\n");
        bsp_lvgl_unlock();
        return 1;
    }
    static const char *const SCREENS[] = { "passport", "setup", "work" };
    int checked = 0;
    int missing = 0;
    for (size_t i = 0u; i < sizeof(SCREENS) / sizeof(SCREENS[0]); i++) {
        lv_obj_t *screen = lv_display_get_screen_by_name(display, SCREENS[i]);
        if (screen == NULL) {
            printf("ui audit: no screen named \"%s\"\n", SCREENS[i]);
            continue;
        }
        audit_object(screen, SCREENS[i], &checked, &missing);
    }
    printf("ui audit: %d labels checked, %d with characters their font cannot draw\n", checked,
           missing);
    bsp_lvgl_unlock();
    return (missing == 0) ? 0 : 1;
}

static int cmd_power(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: power <status|screen on|off|sleep [seconds]>\n");
        printf("  status       whether the screen is on, and how long it has been idle\n");
        printf("  screen on/off  light or darken the screen by hand\n");
        printf("  sleep [s]    enter deep sleep, waking on a key; with seconds, also wake on a\n");
        printf("               timer, which is a testing safety net rather than the shipped path\n");
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        char report[128];
        passport_power_report(report, sizeof(report));
        printf("power: %s\n", report);
        return 0;
    }

    if (strcmp(argv[1], "screen") == 0) {
        if (argc < 3) {
            printf("usage: power screen <on|off>\n");
            return 1;
        }
        if (strcmp(argv[2], "on") == 0) {
            // Through the same path a key press uses, so the idle clock is reset too.
            (void)passport_power_activity();
            printf("power: screen on\n");
            return 0;
        }
        if (strcmp(argv[2], "off") == 0) {
            bsp_display_backlight(0);
            printf("power: screen off (use \"power screen on\", or press a key)\n");
            return 0;
        }
        printf("power screen: unknown \"%s\"\n", argv[2]);
        return 1;
    }

    if (strcmp(argv[1], "sleep") == 0) {
        uint32_t seconds = 0u;
        if (argc > 2) {
            const int value = atoi(argv[2]);
            if (value > 0) {
                seconds = (uint32_t)value;
            }
        }
        printf("power sleep: entering deep sleep%s; a key press wakes it\n",
               (seconds > 0u) ? " with a timer safety net" : "");
        const esp_err_t err = passport_power_sleep(seconds);
        // Reaching here means it did not sleep.
        printf("power sleep: refused: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("power: unknown subcommand \"%s\"\n", argv[1]);
    return 1;
}

static int cmd_notify(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "test") != 0 && strcmp(argv[1], "status") != 0)) {
        printf("usage: notify <test [question|done|failed]|status>\n");
        printf("  test [kind]  play the chime for a kind of announcement (default question)\n");
        printf("  status       whether the buffer is ready and what was played last\n");
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        char report[128];
        passport_notify_report(report, sizeof(report));
        printf("notify: %s\n", report);
        printf("notify_mode: %u (0 silent, 1 chime, 2 speak)\n",
               (unsigned)passport_config_current()->notify_mode);
        return 0;
    }

    passport_work_announcement_t kind = PASSPORT_WORK_ANNOUNCE_QUESTION;
    if (argc > 2) {
        if (strcmp(argv[2], "done") == 0) {
            kind = PASSPORT_WORK_ANNOUNCE_DONE;
        } else if (strcmp(argv[2], "failed") == 0) {
            kind = PASSPORT_WORK_ANNOUNCE_FAILED;
        } else if (strcmp(argv[2], "question") != 0) {
            printf("notify test: unknown kind \"%s\"\n", argv[2]);
            return 1;
        }
    }
    const esp_err_t err = passport_notify_play(kind);
    char report[128];
    passport_notify_report(report, sizeof(report));
    printf("notify test: %s (%s)\n", esp_err_to_name(err), report);
    return (err == ESP_OK) ? 0 : 1;
}

static int cmd_work(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: work <feed <json>|key <up|down|ok>|show|screen|poll|answer|client>\n");
        printf("  feed <json>  hand the device a status document, as the bridge would\n");
        printf("  key <k>      press a button against the current document\n");
        printf("  show         print what the device is showing for work\n");
        printf("  screen       bring the work screen to the front\n");
        printf("  poll         fetch from the configured bridge now\n");
        printf("  answer       send the chosen option to the bridge\n");
        printf("  client       what the bridge client has been doing\n");
        return 1;
    }

    if (strcmp(argv[1], "feed") == 0) {
        if (argc < 3) {
            printf("usage: work feed {\"task\":\"...\",\"progress\":42,\"state\":\"running\"}\n");
            return 1;
        }
        // The console splits arguments on spaces, and a status document contains them, so the
        // remaining arguments are joined back together. Passing only argv[2] truncated the
        // document at its first space: the task lost its tail and every later field was
        // discarded, which looked exactly like a parser that ignores progress and state.
        static char document[512];
        size_t used = 0u;
        document[0] = '\0';
        for (int i = 2; i < argc; i++) {
            const size_t n = strlen(argv[i]);
            if (used + n + 2u >= sizeof(document)) {
                break;
            }
            if (used > 0u) {
                document[used++] = ' ';
            }
            memcpy(document + used, argv[i], n);
            used += n;
            document[used] = '\0';
        }
        if (!passport_app_work_feed(document)) {
            printf("work feed: the document names neither a task nor a question;"
                   " the previous view is kept\n");
            return 1;
        }
        print_work_view(passport_app_work_current());
        if (passport_work_awaiting_answer(passport_app_work_current())) {
            printf("work: a question is waiting; use \"work key down\" then \"work key ok\"\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "key") == 0) {
        if (argc < 3) {
            printf("usage: work key <up|down|ok>\n");
            return 1;
        }
        passport_work_key_t key = PASSPORT_WORK_KEY_NONE;
        if (strcmp(argv[2], "up") == 0) {
            key = PASSPORT_WORK_KEY_UP;
        } else if (strcmp(argv[2], "down") == 0) {
            key = PASSPORT_WORK_KEY_DOWN;
        } else if (strcmp(argv[2], "ok") == 0) {
            key = PASSPORT_WORK_KEY_CONFIRM;
        } else {
            printf("work key: unknown key \"%s\"\n", argv[2]);
            return 1;
        }

        const passport_work_view_t *view = passport_app_work_current();
        const passport_work_action_t action = passport_app_work_press(key);
        switch (action) {
        case PASSPORT_WORK_ACT_REDRAW:
            printf("work key: selection moved\n");
            break;
        case PASSPORT_WORK_ACT_SEND_ANSWER:
            // The bridge is not wired yet, so the answer is printed rather than posted. The
            // exact text is what the bridge will receive.
            printf("work key: answer -> %s\n", view->options[view->selected]);
            break;
        case PASSPORT_WORK_ACT_CHIME:
            printf("work key: chime\n");
            break;
        default:
            printf("work key: nothing to do in state %s\n",
                   passport_work_state_name(view->state));
            break;
        }
        print_work_view(view);
        return 0;
    }

    if (strcmp(argv[1], "show") == 0) {
        print_work_view(passport_app_work_current());
        return 0;
    }

    if (strcmp(argv[1], "screen") == 0) {
        passport_app_show_work();
        printf("work: the work screen is in front\n");
        return 0;
    }

    if (strcmp(argv[1], "poll") == 0) {
        passport_work_client_poll_now();
        // The fetch happens on the client's task, so this reports the request rather than the
        // result; "work client" shows what came back.
        printf("work poll: asked the client for an immediate fetch\n");
        return 0;
    }

    if (strcmp(argv[1], "answer") == 0) {
        const passport_work_view_t *view = passport_app_work_current();
        if (!view->answered || view->option_count == 0) {
            printf("work answer: nothing has been chosen; use \"work key down\" and"
                   " \"work key ok\" first\n");
            return 1;
        }
        const esp_err_t err = passport_work_client_answer(view->options[view->selected]);
        char status[160];
        passport_work_client_report(status, sizeof(status));
        printf("work answer: %s (%s)\n", esp_err_to_name(err), status);
        return (err == ESP_OK) ? 0 : 1;
    }

    if (strcmp(argv[1], "client") == 0) {
        char status[160];
        passport_work_client_report(status, sizeof(status));
        printf("work client: %s\n", status);
        return 0;
    }

    printf("work: unknown subcommand \"%s\"\n", argv[1]);
    return 1;
}

static int cmd_prov(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: prov <status|start|stop|page [route]|qr|form <k=v&k=v>>\n");
        printf("  status        whether the setup access point is up, and its name\n");
        printf("  start / stop  raise or drop the access point and its web console\n");
        printf("  page [route]  print what a request would return: root, status, or config\n");
        printf("  qr            print the module matrix of the code shown on the panel\n");
        printf("  form <k=v..>  apply a submission exactly as a POST to /save would\n");
        return 1;
    }

    const char *sub = argv[1];

    if (strcmp(sub, "status") == 0) {
        printf("access point : %s\n", passport_provision_active() ? "up" : "down");
        printf("ssid         : %s\n", passport_provision_ssid());
        printf("password     : %s\n", passport_provision_password());
        printf("address      : http://%s/\n", passport_provision_address());
        printf("station      : %s\n", passport_wifi_state_name(passport_wifi_state()));
        return 0;
    }

    if (strcmp(sub, "start") == 0 || strcmp(sub, "stop") == 0) {
        const bool start = (sub[0] == 's' && sub[1] == 't' && sub[2] == 'a');
        const esp_err_t err = start ? passport_provision_start() : passport_provision_stop();
        printf("prov %s: %s\n", sub, esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }

    if (strcmp(sub, "page") == 0) {
        passport_portal_route_t route = PASSPORT_PORTAL_ROUTE_ROOT;
        if (argc > 2) {
            if (strcmp(argv[2], "status") == 0) {
                route = PASSPORT_PORTAL_ROUTE_STATUS;
            } else if (strcmp(argv[2], "config") == 0) {
                route = PASSPORT_PORTAL_ROUTE_CONFIG;
            } else if (strcmp(argv[2], "root") != 0) {
                // Accept a literal path too, so the routing table can be exercised.
                route = passport_portal_route(argv[2]);
            }
        }
    // The same size the HTTP path uses. A smaller buffer here cut the preview short and made the
    // page look broken when it was only the console that was short of room.
    static char page[4096];
        const int written = passport_provision_render(route, page, sizeof(page));
        if (written < 0) {
            printf("prov page: route \"%s\" resolves to %s, which has no rendered body\n",
                   (argc > 2) ? argv[2] : "/", passport_portal_route_name(route));
            return 1;
        }
        printf("prov page: route=%s, %d bytes\n", passport_portal_route_name(route), written);
        printf("%s\n", page);
        return 0;
    }

    if (strcmp(sub, "qr") == 0) {
        // Print the matrix the panel is showing, so the code on the glass can be compared
        // against an independent implementation of the same payload. The payload text
        // itself is not printed: it contains the hotspot password, and the matrix is
        // already displayed on the device by design.
        char payload[192];
        snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;",
                 passport_provision_ssid(), passport_provision_password());
        passport_qr_t code;
        if (!passport_qr_encode(payload, &code)) {
            printf("prov qr: payload does not fit (%u bytes)\n", (unsigned)strlen(payload));
            return 1;
        }
        printf("prov qr: %u-byte payload, version %u, %u modules, mask %u\n",
               (unsigned)strlen(payload), code.version, (unsigned)code.size, code.mask);
        for (int y = -2; y < (int)code.size + 2; y++) {
            for (int x = -2; x < (int)code.size + 2; x++) {
                const bool inside = (x >= 0 && y >= 0 && x < (int)code.size && y < (int)code.size);
                putchar((inside && code.modules[y][x] != 0u) ? '#' : '.');
            }
            putchar('\n');
        }
        return 0;
    }

    if (strcmp(sub, "pixels") == 0) {
        // Sample the join code as it was actually drawn. The encoder's matrix is already checked
        // against a reference implementation by `prov qr` and by the host tests; what this adds is
        // the render path — the scale, the quiet zone and, the reason it exists, whether the two
        // colours went the right way round. An inverted symbol is correct cell for cell and
        // unscannable.
        const uint16_t *pixels = NULL;
        int side = 0;
        int scale = 0;
        int modules = 0;
        int quiet = 0;
        if (!passport_app_qr_drawn(&pixels, &side, &scale, &modules, &quiet)) {
            printf("prov pixels: no join code is drawn; open the setup screen first\n");
            return 1;
        }
        if (scale <= 0 || modules <= 0) {
            printf("prov pixels: the render geometry was not recorded\n");
            return 1;
        }

        // The symbol's own matrix, for comparison.
        char payload[192];
        snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", passport_provision_ssid(),
                 passport_provision_password());
        passport_qr_t code;
        if (!passport_qr_encode(payload, &code)) {
            printf("prov pixels: the payload does not fit\n");
            return 1;
        }

        unsigned dark = 0u;
        unsigned total = 0u;
        unsigned wrong = 0u;
        const uint16_t dark_pixel = 0x0000u;   // black in RGB565
        printf("prov pixels: %d px, scale %d, %d modules, quiet zone %d\n", side, scale, modules,
               quiet);
        for (int my = 0; my < modules; my++) {
            for (int mx = 0; mx < modules; mx++) {
                const int px = mx * scale + scale / 2;
                const int py = my * scale + scale / 2;
                const uint16_t sample = pixels[(size_t)py * (size_t)side + (size_t)px];
                const bool is_dark = (sample == dark_pixel);
                if (is_dark) {
                    dark++;
                }
                total++;
                // The expected colour: a module of the symbol, or the quiet zone around it.
                const int cx = mx - quiet;
                const int cy = my - quiet;
                const bool inside = (cx >= 0 && cy >= 0 && cx < (int)code.size && cy < (int)code.size);
                const bool expected_dark = inside && (code.modules[cy][cx] != 0u);
                if (is_dark != expected_dark) {
                    wrong++;
                }
            }
        }
        printf("prov pixels: %u of %u module centres are dark (%.0f%%), %u disagree with the symbol\n",
               dark, total, total ? (100.0 * (double)dark / (double)total) : 0.0, wrong);
        // Show the top-left corner as drawn and as encoded. A finder pattern is distinctive enough
        // that a shift, an inversion or a stale buffer is obvious from seven rows.
        printf("prov pixels: drawn top-left 7x7:\n");
        for (int my = 0; my < 7 && my < modules; my++) {
            char line[64];
            int used = 0;
            for (int mx = 0; mx < 7 && mx < modules; mx++) {
                const int px = mx * scale + scale / 2;
                const int py = my * scale + scale / 2;
                line[used++] = (pixels[(size_t)py * (size_t)side + (size_t)px] == dark_pixel) ? '#' : '.';
            }
            line[used] = '\0';
            printf("  %s\n", line);
        }
        printf("prov pixels: encoded top-left 7x7:\n");
        for (int my = 0; my < 7; my++) {
            char line[64];
            int used = 0;
            for (int mx = 0; mx < 7; mx++) {
                const int cx = mx - quiet;
                const int cy = my - quiet;
                const bool inside = (cx >= 0 && cy >= 0 && cx < (int)code.size && cy < (int)code.size);
                line[used++] = (inside && code.modules[cy][cx] != 0u) ? '#' : '.';
            }
            line[used] = '\0';
            printf("  %s\n", line);
        }
        if (wrong != 0u) {
            printf("prov pixels: MISMATCH - the drawn code is not the symbol the encoder produced\n");
            return 1;
        }
        if (dark * 10u > total * 7u) {
            // A QR symbol is roughly half dark; far more than that usually means the colours are
            // inverted, which no cell-by-cell comparison would notice.
            printf("prov pixels: WARNING - that is far more dark than a symbol should be;"
                   " check the colours are not inverted\n");
        }
        printf("prov pixels: MATCH - the drawn pixels are the encoded symbol\n");
        return 0;
    }

    if (strcmp(sub, "form") == 0) {
        if (argc < 3) {
            printf("usage: prov form wifi_ssid=MyNet&llm_model=deepseek-chat\n");
            printf("  applies the submission exactly as a POST to /save would; a blank value\n");
            printf("  leaves that setting unchanged\n");
            return 1;
        }
        const esp_err_t err = passport_provision_apply(argv[2]);
        if (err == ESP_OK) {
            printf("prov form: applied and saved\n");
        } else if (err == ESP_ERR_INVALID_STATE) {
            printf("prov form: nothing to apply (every field was blank or unknown)\n");
        } else {
            printf("prov form: %s\n", esp_err_to_name(err));
        }
        return (err == ESP_OK) ? 0 : 1;
    }

    printf("prov: unknown subcommand \"%s\"\n", sub);
    return 1;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: wifi <status|scan|connect [ms]|disconnect>\n");
        printf("  status           station state, address, and the configured SSID\n");
        printf("  scan             list nearby access points (no credentials needed)\n");
        printf("  connect [ms]     associate using the stored credentials (default 15000 ms)\n");
        printf("  disconnect       drop the association; stored credentials are kept\n");
        return 1;
    }

    const char *sub = argv[1];
    char ip[24];

    if (strcmp(sub, "status") == 0) {
        passport_wifi_ip(ip, sizeof(ip));
        const passport_config_t *cfg = passport_config_current();
        printf("state    : %s\n", passport_wifi_state_name(passport_wifi_state()));
        printf("address  : %s\n", (ip[0] != '\0') ? ip : "(none)");
        printf("ssid     : %s\n", (cfg->wifi_ssid[0] != '\0') ? cfg->wifi_ssid : "(not configured)");
        printf("password : %s\n", (cfg->wifi_password[0] != '\0') ? "(set)" : "(empty)");
        return 0;
    }

    if (strcmp(sub, "scan") == 0) {
        passport_wifi_ap_t aps[SCAN_LIST_MAX];
        const int found = passport_wifi_scan(aps, SCAN_LIST_MAX, 15000u);
        if (found < 0) {
            printf("wifi scan: failed\n");
            return 1;
        }
        printf("wifi scan: %d access point(s)\n", found);
        for (int i = 0; i < found; i++) {
            printf("  %-32s rssi=%4d ch=%2u %s\n", aps[i].ssid, aps[i].rssi,
                   (unsigned)aps[i].channel, aps[i].secure ? "secured" : "open");
        }
        return 0;
    }

    if (strcmp(sub, "connect") == 0) {
        uint32_t timeout = 15000u;
        if (argc > 2) {
            const int ms = atoi(argv[2]);
            if (ms > 0) {
                timeout = (uint32_t)ms;
            }
        }
        const esp_err_t err = passport_wifi_connect(timeout);
        if (err == ESP_ERR_INVALID_STATE) {
            printf("wifi connect: no SSID configured. Set one first:\n");
            printf("  config set wifi_ssid <name>\n");
            printf("  config set wifi_password <password>\n");
            printf("  config save\n");
            return 1;
        }
        passport_wifi_ip(ip, sizeof(ip));
        printf("wifi connect: %s%s%s\n", esp_err_to_name(err),
               (ip[0] != '\0') ? "  address " : "", ip);
        return (err == ESP_OK) ? 0 : 1;
    }

    if (strcmp(sub, "disconnect") == 0) {
        const esp_err_t err = passport_wifi_disconnect();
        printf("wifi disconnect: %s\n", esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }

    printf("wifi: unknown subcommand \"%s\"\n", sub);
    return 1;
}

static int cmd_config(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: config <list|get <field>|set <field> <value>|save|reset>\n");
        printf("  list             every field, with secrets shown as set or empty\n");
        printf("  get <field>      one field\n");
        printf("  set <field> <v>  change a field in RAM; \"\" clears a string\n");
        printf("  save             persist the live configuration\n");
        printf("  reset            restore defaults and erase the stored copy\n");
        return 1;
    }

    const char *sub = argv[1];

    if (strcmp(sub, "list") == 0) {
        char value[128];
        printf("configuration: %u bytes, stored=%s\n",
               (unsigned)sizeof(passport_config_t),
               passport_config_dirty() ? "no (unsaved changes)" : "yes");
        for (int i = 0; i < passport_cfg_field_count(); i++) {
            const passport_cfg_field_t *f = passport_cfg_field(i);
            passport_cfg_format(passport_config_current(), f, value, sizeof(value));
            printf("  %-18s %s%s\n", f->name, value, f->secret ? "   [secret]" : "");
        }
        return 0;
    }

    if (strcmp(sub, "get") == 0) {
        if (argc < 3) {
            printf("usage: config get <field>\n");
            return 1;
        }
        const passport_cfg_field_t *f = passport_cfg_find(argv[2]);
        if (f == NULL) {
            printf("config get: unknown field \"%s\" (try \"config list\")\n", argv[2]);
            return 1;
        }
        char value[128];
        passport_cfg_format(passport_config_current(), f, value, sizeof(value));
        printf("%s = %s%s\n", f->name, value,
               f->secret ? "   (a secret is never printed)" : "");
        return 0;
    }

    if (strcmp(sub, "set") == 0) {
        if (argc < 4) {
            printf("usage: config set <field> <value>   (use \"\" to clear a string)\n");
            return 1;
        }
        const passport_cfg_field_t *f = passport_cfg_find(argv[2]);
        if (f == NULL) {
            printf("config set: unknown field \"%s\" (try \"config list\")\n", argv[2]);
            return 1;
        }
        const passport_cfg_status_t st = passport_cfg_apply(passport_config_mutable(), f, argv[3]);
        if (st != PASSPORT_CFG_OK) {
            printf("config set: %s: %s\n", f->name, passport_cfg_status_name(st));
            return 1;
        }
        char value[128];
        passport_cfg_format(passport_config_current(), f, value, sizeof(value));
        printf("config set: %s = %s%s\n", f->name, value, f->secret ? "   [secret]" : "");
        printf("  not persisted yet: run \"config save\"\n");
        return 0;
    }

    if (strcmp(sub, "save") == 0) {
        const esp_err_t err = passport_config_store_save();
        printf("config save: %s\n", esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }

    if (strcmp(sub, "reset") == 0) {
        const esp_err_t err = passport_config_store_reset();
        printf("config reset: %s\n", esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }

    printf("config: unknown subcommand \"%s\"\n", sub);
    return 1;
}

static int cmd_ui(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: ui <tree|names|get|tap|key|screen|audit> [args]\n");
        printf("  tree [depth]            dump the live object tree\n");
        printf("  names                   list every object as a name path\n");
        printf("  get <name-or-path>      print one object\n");
        printf("  tap <name-or-path>      send a synthetic click\n");
        printf("  key <up|down|ok> <ev>   inject a button event\n");
        printf("  screen                  active screen summary\n");
        printf("  audit                   check every label against the font it is drawn with\n");
        return 1;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "audit") == 0)  { return cmd_ui_audit(); }
    if (strcmp(sub, "tree") == 0)   { return cmd_ui_tree(argc - 1, argv + 1); }
    if (strcmp(sub, "names") == 0)  { return cmd_ui_names(argc - 1, argv + 1); }
    if (strcmp(sub, "get") == 0)    { return cmd_ui_get(argc - 1, argv + 1); }
    if (strcmp(sub, "tap") == 0)    { return cmd_ui_tap(argc - 1, argv + 1); }
    if (strcmp(sub, "key") == 0)    { return cmd_ui_key(argc - 1, argv + 1); }
    if (strcmp(sub, "screen") == 0) { return cmd_ui_screen(argc - 1, argv + 1); }
    printf("ui: unknown subcommand \"%s\"\n", sub);
    return 1;
}

// --- registration -----------------------------------------------------------

typedef int (*cli_func_t)(int argc, char **argv);

static void register_command(const char *command, const char *help, cli_func_t func)
{
    const esp_console_cmd_t cmd = {
        .command = command,
        .help = help,
        .hint = NULL,
        .func = func,
    };
    const esp_err_t err = esp_console_cmd_register(&cmd);
    if (err != ESP_OK) {
        printf("passport_cli: cannot register \"%s\": %s\n", command, esp_err_to_name(err));
    }
}

void passport_cli_set_key_sink(passport_cli_key_sink_t sink)
{
    s_key_sink = sink;
}

esp_err_t passport_cli_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    const esp_console_repl_config_t repl_config = {
        .max_history_len = 32,
        .history_save_path = NULL,
        // A TLS handshake needs far more than the usual 4 KB. Running `llm probe`
        // from this task overflowed a 4096-byte stack and panicked with a stack
        // protection fault, so the console task is sized for the work it can be
        // asked to do. Any command that performs networking runs here.
        .task_stack_size = 10240,
        .task_priority = 2,
        .task_core_id = tskNO_AFFINITY,
        .prompt = "passport> ",
        .max_cmdline_length = 0,
    };
    const esp_console_dev_usb_serial_jtag_config_t dev_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_console_repl_t *repl = NULL;
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl);
    if (err != ESP_OK) {
        printf("passport_cli: REPL unavailable: %s\n", esp_err_to_name(err));
        return err;
    }

    register_command("ver", "Print the application and ESP-IDF build identity", &cmd_ver);
    register_command("app", "Print uptime, battery, keypad voltage, and memory", &cmd_app);
    register_command("mem", "Print heap, largest free block, and the LVGL pool", &cmd_mem);
    register_command("net", "HTTPS transport: net probe <url>", &cmd_net);
    register_command("llm", "Language model: llm probe | llm ask <text>", &cmd_llm);
    register_command("font", "Text font: font probe | font shot", &cmd_font);
    register_command("wifi", "Wi-Fi station: status | scan | connect [ms] | disconnect", &cmd_wifi);
    register_command("prov", "Setup portal: status | start | stop | page [route] | qr | form <k=v>", &cmd_prov);
    register_command("work", "Work mode: feed <json> | key <up|down|ok> | show | screen | poll | answer | client", &cmd_work);
    register_command("notify", "Notification chimes: test [question|done|failed] | status", &cmd_notify);
    register_command("power", "Standby: status | screen on|off | sleep [seconds]", &cmd_power);
    register_command("voice", "Push-to-talk: status | hold | release | say <text>", &cmd_voice);
    register_command("config", "Configuration: list | get | set | save | reset", &cmd_config);
    register_command("audio", "Audio path: status | tone [ms] | loopback [ms] | mute-check", &cmd_audio);
    register_command("ui", "Screen inspection: tree | names | get | tap | key | screen", &cmd_ui);
    register_command("reboot", "Restart the device", &cmd_reboot);
    esp_console_register_help_command();

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        printf("passport_cli: cannot start REPL: %s\n", esp_err_to_name(err));
        return err;
    }
    s_started = true;
    printf("passport_cli: console ready. Type \"help\".\n");
    return ESP_OK;
}
