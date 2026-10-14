<p align="right">
  <a href="serial-cli.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Serial Console

A command console on the USB Serial/JTAG port that lets a host **read the live
screen** and **drive the device**. It exists so that an agent or a person can
verify what is actually drawn, instead of inferring it from source code.

Implemented in [`main/passport_cli.c`](../../../main/passport_cli.c). Started by
`passport_cli_start()` after the display, LVGL, and the application input path are
up.

## How the screen and the console stay in correspondence

There is no second element registry. Widget identity comes from **LVGL's named
objects**, and names are derived from the text the user actually reads:

```c
lv_obj_set_name(s_card, "audio");   /* the card labelled "Audio" */
```

The application derives each name from the same string it draws
([`slugify()`](../../../main/main.c) lowercases, hyphenates, and strips the rest),
so `Low Power` becomes `low-power` and `Wi-Fi` becomes `wi-fi`. Every `ui` command
addresses those names as a slash-separated path from the active screen. **The name
printed for an object is exactly the name a command accepts**, so the list cannot
drift away from what is on the display.

Requires `CONFIG_LV_USE_OBJ_NAME=y` (set in `sdkconfig.defaults`).

## Commands

| Command | What it does |
| --- | --- |
| `ver` | Project name, application version, build date and time, ESP-IDF version |
| `app` | Uptime, battery state of charge, keypad voltage, heap, largest free block, LVGL pool |
| `mem` | Heap, largest free block, and the LVGL pool, without the rest |
| `ui tree [depth]` | Dump the active screen's object tree: path, kind, geometry, `hidden`, and label text. Default depth 6 |
| `ui names` | List every object as a name path, its kind, and its on-screen text |
| `ui get <name-or-path>` | Print one object's geometry, flags, and text |
| `ui tap <name-or-path>` | Send a synthetic `LV_EVENT_CLICKED`; if the named object is a plain label, the nearest clickable ancestor receives it |
| `ui key <up\|down\|ok> <press\|click\|double\|long\|release>` | Inject a button event |
| `ui screen` | Active screen name, size, and child count |
| `reboot` | Restart the device |
| `audio status\|tone [ms]\|loopback [ms]\|mute-check` | Which subsystem holds the audio path; play a 1 kHz tone; record and play it back with a level reading; prove that a capture is refused while playback holds the path |
| `net probe <url>` | GET a URL over HTTPS; report the status, the bytes, and the heap measured while the TLS session was open |
| `llm probe` \| `llm ask <text>` | POST to the configured endpoint: `probe` sends a minimal completion, where a 401 still proves the transport worked; `ask` sends a question and prints the reply |
| `wifi status\|scan\|connect [ms]\|disconnect` | Station state and address; list nearby access points, which needs no credentials; associate using the stored credentials; drop the association |
| `config list\|get <field>\|set <field> <value>\|save\|reset` | The configuration: every field with secrets shown as set or empty, change a field in RAM, persist it, or restore defaults |
| `help` | The ESP-IDF console's own command list |

## Two contracts worth knowing before scripting against it

**Only objects with a user-set name are addressable.** Unnamed objects still appear
in `ui tree` and `ui names`, but with a synthetic `kind_index` label that no
command accepts. Name the widget you want to address.

**`ui key` is the reliable way to drive navigation; `ui tap` only works where the
application registers click handlers.** Button injection enters the application's
own input queue, so it travels the same route as a physical press and drives the
state machine. A synthetic click does nothing if the widget has no handler — which
is the case for the upstream hardware-test demo, whose menu is button-driven.

## Why LVGL's name resolver is not used

`lv_obj_get_name_resolved()` triggers a load access fault on an **unnamed root
screen** in the LVGL version this project pins. It only appears to work while the
active screen has a user-set name. The console therefore reads
`lv_obj_get_name()` directly, builds a synthetic label for unnamed objects, and
resolves paths with its own recursive walk. Do not reintroduce
`lv_obj_get_name_resolved()`, `lv_obj_get_child_by_name()`, or
`lv_obj_find_by_name()` without re-testing the unnamed-screen case on hardware.

## Worked example

```text
passport> ver
project   : FoloToy-AI-Passport
version   : 4d05296
built     : Oct 14 2026 00:58:13
esp-idf   : v5.5.3

passport> ui screen
screen menu  240x320  children=39

passport> ui names
menu                               obj        (active screen)
display                            obj
display/lv_label_0                 label      "Display"
audio                              obj
audio/lv_label_0                   label      "Audio"

passport> ui key down click
ui key: injected down click
passport> ui key ok click
ui key: injected ok click
passport> ui screen
screen obj_0  240x320  children=27
passport> ui key ok long
ui key: injected ok long
passport> ui screen
screen menu  240x320  children=39
```

The measured values from the same session are recorded in the
[progress log](progress-log.md).

## Extending it

Add a command with `register_command()` in
[`passport_cli.c`](../../../main/passport_cli.c), and add a row to the table
above in the same change. Two rules apply to any new command:

- Take `bsp_lvgl_lock()` around every LVGL access. The console runs in its own
  task and LVGL is not thread-safe.
- Reach the application through its own paths — the input queue, a state-machine
  getter — rather than by calling into another task's data structures.

The console is not a required subsystem: if it fails to start, the device logs a
warning and runs on normally.
