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
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "passport_audio.h"
#include "passport_config.h"
#include "passport_config_store.h"
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

// Resolve a slash-separated path of user-set names against the active screen.
// Only named objects are addressable, which is exactly the set that carries
// meaning on the display; unnamed ones still appear in the tree for inspection.
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
        const char *name = lv_obj_get_name(child);
        if (name == NULL || strcmp(name, segment) != 0) {
            continue;
        }
        return (slash == NULL) ? child : find_by_path(child, slash + 1);
    }
    return NULL;
}

static lv_obj_t *resolve(const char *term)
{
    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        return NULL;
    }
    return find_by_path(screen, term);
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
        printf("usage: ui <tree|names|get|tap|key|screen> [args]\n");
        printf("  tree [depth]            dump the live object tree\n");
        printf("  names                   list every object as a name path\n");
        printf("  get <name-or-path>      print one object\n");
        printf("  tap <name-or-path>      send a synthetic click\n");
        printf("  key <up|down|ok> <ev>   inject a button event\n");
        printf("  screen                  active screen summary\n");
        return 1;
    }
    const char *sub = argv[1];
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
        .task_stack_size = 4096,
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
    register_command("wifi", "Wi-Fi station: status | scan | connect [ms] | disconnect", &cmd_wifi);
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
