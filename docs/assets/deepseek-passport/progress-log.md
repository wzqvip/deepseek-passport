<p align="right">
  <a href="progress-log.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# DeepSeek Passport Progress Log

A dated record of what was done, what was decided, what evidence supports it, and
what is still open. New entries are appended at the top of the history section.

Rules for this log:

- Record decisions, not activity. A commit message already records that code
  changed.
- Every factual claim is either cited to a file or a reference, or marked as
  reported and unverified.
- Never record a credential, a key, a token, or a QR secret.
- A milestone is only complete when the corresponding exit criteria in
  [roadmap.md](roadmap.md) are met and their evidence is recorded here.

---

## 2026-10-14 — Session 4: the application replaces the demo menu

**Milestone:** the application body has begun; its first screen is the pet home
screen.

### What was done

The upstream hardware-test menu is gone from the build. `main/main.c` is now the
Passport entry point: BSP initialization, the deep-sleep key-guard hand-off from the
previous session, an input queue whose consumer drives the application, the serial
console, and one call to start the application.

The first screen is designed for this product rather than inherited: a status strip
with the battery top-right, an accent rule, a conversation panel with an accent
edge, the pet sprite, a line naming the current expression, a ground line, and a
footer hint. The demo's cards, clouds, grass, and `ui_pixel` shell are **deleted**,
not recolored.

The pet (`passport_pet.c`) is a sprite widget driven by a 90 ms LVGL timer: a
two-frame breathing bob with an occasional blink while idle, a raised tail while
working, a spout while speaking, and a small vertical reaction for happy and
distressed. Animation swaps the image source on one widget, so the resident object
count never changes. Every mood is reachable with `UP`/`DOWN`, which is how a host
verifies the whole sprite set over the console.

Battery reads happen in the application's worker task, not in the LVGL timer: the
gauge is on I2C, and blocking I/O inside the rendering task is exactly what the
hardware guide warns against. The worker takes `bsp_lvgl_lock()` to update the label.

### Verified on the device

Boot log: two I2C devices found, display and LVGL up, the application started, the
console ready, and a readiness line reporting Display=1 Button=1 Audio=1 Battery=1
App=1. The asset line reports 11 frames at 64x64, a first frame of 12,288 B, and
135,168 B of flash — matching the predicted RGB565A8 cost, so the sprites are
genuinely in the image rather than merely compiled.

Screen read back over the console, which is the correspondence working as intended:

```text
passport            obj      (active screen)
title               label    "PASSPORT"
battery             label    "97%"
obj_3/bubble        label    "Hi. Press OK."
pet                 widget
mood                label    "idle"
hint                label    "OK: react    hold OK: standby"
```

Driven with injected keys: `ui key down click` twice took `mood` from "idle" to
"curious" to "listening", and `ui key ok click` produced "happy", each read back
with `ui get mood`.

### Measurements

| Reading | Baseline demo | This application |
| --- | --- | --- |
| Free heap | 142,600 B | **211,884 B** |
| Largest free block | 112 KiB | 112 KiB |
| LVGL pool high-water mark | 18,452 B | **9,380 B** |
| LVGL pool free | 4,008 B | **14,100 B** |
| Application image | 0x1a39c0 | **0xd0d10 (854 KB)** |

The redesigned UI uses about half the LVGL pool of the demo it replaced, and
removing the demo pages released the unused NimBLE and Wi-Fi stacks from the image,
which is why the binary shrank by roughly half while gaining 135 KB of sprites.

---

## 2026-10-14 — Session 3: device identified, toolchain gap, M1 started

**Milestone:** `M1` in progress.

### What was done

1. **Device identified.** After a cable and USB-port change the Passport
   enumerates as an Espressif USB Serial/JTAG device, `USB\VID_303A&PID_1001`,
   with its console on `COM47`. That matches the documented board mapping: a
   native USB Serial/JTAG console and no UART bridge. The WCH CH342 dual-UART
   bridge on `COM11`/`COM13` belongs to some other device and is not part of this
   board.
2. **Toolchain gap found, and closed.** The machine had only ESP-IDF v6.1
   (`C:\esp\v6.1\esp-idf`, the single entry in the EIM registry, with only a v6.1
   offline archive). The repository pins `>=5.5.3,<5.6.0`, so v6.1 cannot build
   this firmware. ESP-IDF v5.5.3 was then installed **side by side** through EIM
   into `C:\esp`, leaving v6.1 untouched and still the selected version, and the
   firmware was built with v5.5.3 to prove it.
3. **Branch created.** `feature/deepseek-passport`, branched from fork `main`,
   which is level with `upstream/main` — so the baseline is aligned and the design
   documents travel with the branch.
4. **First M1 increment landed and verified.** The pure measurement arithmetic
   plus its host test, wired into the static gate.

### Why the arithmetic came first

The firmware's ESP-IDF modules cannot be compiled until 5.5.3 exists, but the
measurement math can be, and that is exactly where off-by-one and overflow
mistakes hide. Writing the verifiable part first leaves a working checkpoint on
the branch instead of one large change that nothing can build.

### Scope correction for M1

Idle current **cannot** be measured by the firmware. The board has no
current-sense path, and the CW2017 gauge reports the battery, not the board's draw
from USB. M1 can only enter the always-connected idle state and hold it, so that a
person with a USB power meter can read the number. It therefore stays under
`Unverified` until someone takes it, and the `M1` exit criteria in
[roadmap.md](roadmap.md) are corrected accordingly.

### Also recorded

- The pet sprites currently sit beside their design document under `docs/assets/`,
  which the fork `main` scope allows. When the firmware build first consumes them
  they must move to the repository-root `assets/images/` tree with the generator
  under `tools/`, as the asset-placement rule requires, and the design document
  must link to the new location.
- The firmware will read its Wi-Fi credentials from Kconfig, so a real SSID and
  password live only in the gitignored `sdkconfig` and never in source.

### The product owner's hands-on test of the flashed demo

The owner exercised the flashed baseline application and reported that everything
works, with one observation: waking from deep sleep restarts the application.

That is the documented contract, not a defect. `esp_deep_sleep_start()` does not
return; the chip resets and the application boots fresh. The repository's own
reference note records the same behaviour on this board, including that the USB
Serial/JTAG console goes down for the duration of deep sleep and returns after the
wake, and that the boot log prints the wake cause — which is the documented way to
confirm a wake source on real hardware.

Two consequences worth keeping:

- **Light sleep is different.** It resumes in place and does not restart. A design
  that needs a seamless resume must use light sleep or the always-connected
  profile, which is the default the owner already chose (`Q-8`). The shipped
  default therefore will not restart on idle.
- **Deep sleep is why the console vanished** during the first flash attempt, and
  it is why the baseline demo's Low Power page is not a debugging tool while a
  host is attached to the USB port.

Unverified: the observation was not instrumented. No serial capture of the wake
cause was taken at the time, so the wake source rests on documentation rather than
on a log from this device. A future capture of the wake-cause line would close
that gap.

### Serial console verified on the device

The console was flashed and exercised over `COM47`. Inspection and control both
work, and the exercise found one real defect in the console itself.

Working: `ver` reports build `4d05296`; `ui screen` reports `menu 240x320
children=39`; `ui names` prints every object as a path with its on-screen text,
including names derived from the visible labels (`display`, `button`, `audio`,
`battery`, `wi-fi`, `ble`, `low-power`); `ui key down click` followed by
`ui key ok click` enters a page; `ui tree 1` dumps the new screen; `ui key ok
long` returns to the menu. The loop closes, so a host can drive the device and
read back what is drawn.

Defect found and fixed: `lv_obj_get_name_resolved()` faults with a load access
fault on an **unnamed root screen** in this LVGL version. It appeared to work only
because the menu screen carries a user-set name. The console now reads
`lv_obj_get_name()` directly, builds a synthetic label for unnamed objects, and
resolves paths with its own walk instead of LVGL's name lookup. The consequence is
worth stating plainly: **only objects with a user-set name are addressable**;
unnamed objects still appear in the tree for inspection.

Also learned: the baseline demo's menu is driven by button events, not by widget
clicks, so `ui tap` cannot enter a page there — its cards register no click
handler. `ui tap` stays correct for a UI that does register handlers, which the M1
application will.

Measurements taken while the console was up, on the menu screen: free heap
142,600 B, minimum 138,544 B, **largest free block 112 KiB**; the LVGL pool free
4,008 B of 24 KB with a high-water mark of 18,452 B and 9% fragmentation; battery
97%; released keypad 2,964 mV. The LVGL pool figure is the first concrete sign
that the baseline demo already spends most of a 24 KB pool.

### Validation for this session

| Check | Result |
| --- | --- |
| Host tests | **PASS** — the new `test_passport_metrics_math` and the pre-existing pure tests compile with `-Wall -Wextra -Werror` and pass |
| Firmware build | **PASS** — ESP-IDF v5.5.3 from `C:\esp\v5.5.3\esp-idf`, target `esp32c3`, isolated build in `build/fw` with its own `sdkconfig`. Image `FoloToy-AI-Passport.bin`, 0x174ea0 bytes (about 1.5 MB) in an 8.3 MB application partition, 82% free. This builds the **baseline** application; the M1 application is not implemented yet, so this proves the toolchain and the repository, not M1 |
| Device tests | **PASS for the board sanity check** — the validated v5.5.3 build was flashed to `COM47` (`USB\VID_303A&PID_1001`) using segmented flashing, after explicit approval. The boot log reports application version `208f1ff`, chip revision v1.1, an I2C scan finding the ES8311 codec at `0x18` and the CW2017 gauge at `0x63`, the display and LVGL task starting, the ES8311 in slave mode, the 500 mAh battery profile matching, and a readiness line reporting all four subsystems up. This verifies the board and the toolchain end to end. It does **not** verify M1, which is not implemented yet. The firmware's Chinese log text renders as mojibake in `idf.py monitor` on this host, which is a terminal decoding artifact and not a firmware fault |
| Unverified | Idle current (an external instrument check); every firmware measurement; the TLS probe's behaviour on this network |

---

## 2026-10-13 (later) — Session 2: product owner answers and the pet design

**Milestone:** `M0` complete.

### Answers received

Six of the ten questions were answered. The answers are recorded as decisions in
[requirements.md](requirements.md#61-answered); what follows is their effect on
the design.

| ID | Answer | Effect |
| --- | --- | --- |
| `Q-1` | An original Q-version whale in the brand blue; the project may draw it | The pet design and its generator were produced (below) |
| `Q-4` | Provider credentials live on the device; plain text is accepted | Device-direct becomes a supported deployment rather than a fallback, which puts TLS on the critical path. `M1` must now measure the largest free block with a live TLS session before any provider code is written |
| `Q-5` | Partial transcription is required on screen | A streaming transcription service becomes the baseline and batch transcription the cost-saving alternative |
| `Q-6` | Over-the-air update is deferred | The partition table stays as it is: one factory application, no second slot |
| `Q-8` | The always-connected profile is the default | Deep sleep becomes opt-in; idle current is now a number the project owes the user, measured in `M1` |
| `Q-10` | Long in-pocket recording is deferred | The storage design is unchanged |

### Decisions

| ID | Decision | Reason |
| --- | --- | --- |
| D-13 | The pet is an original Q-version whale drawn by this project, not a copy of the DeepSeek logo | The owner asked for the brand blue and a Q-style character while allowing the project to draw it; keeping the artwork original avoids a trademark problem and gives the project a character it owns |
| D-14 | Device-direct provider calls are supported, with provider credentials stored on the device in plain text | Product owner answer to `Q-4`. The consequence is accepted explicitly: a lost device is a lost key |
| D-15 | A streaming transcription back end is the default; batch transcription remains available | Product owner answer to `Q-5` |
| D-16 | Over-the-air update is deferred and the partition table is left alone | Product owner answer to `Q-6` |
| D-17 | The always-connected power profile is the default; the battery profile and deep sleep are deferred, and the upstream key-wake guard is preserved for them | Product owner answer to `Q-8` |
| D-18 | The pet is generated procedurally from a checked-in script rather than hand-drawn pixel by pixel | Redrawing a 32 × 32 character by hand for every expression is slow and unreviewable; a generator makes the geometry, palette, and frame list reviewable and reproducible |

### The pet design

A first-pass design exists, produced by a new generator.

- The character is a 32 × 32 logical whale facing right, written at 2× as 64 × 64
  sprites, with eleven opaque colors anchored on the brand blue `#4D6BFE`.
- Eleven frames cover idle (a two-frame breathing bob), blink, curious,
  listening, thinking, working, speaking, happy, sleeping, and distressed.
- Each frame is 12,288 B as RGB565A8; eleven frames total 132 KiB of flash, which
  is irrelevant against 8 MB. Frames share identical bounds, so animation swaps
  one image source and never changes the resident object count.
- The generator uses the Python standard library only, so it runs anywhere and
  produces byte-identical output.
- The design is awaiting product-owner sign-off, and redrawing is cheap: the
  geometry constants and the palette are the whole surface area.

Artwork and generator: [`art/`](art/README.md).

### Reference research

The design was checked against published references before it was drawn, and it
copies none of them.

| Finding | Source | Effect |
| --- | --- | --- |
| The official brand colour is `#4D6BFE` | DeepSeek's own organisation publishes a colour swatch whose only fill is that value | The body colour is exactly it, so the palette is verified rather than estimated |
| DeepSeek's mark is a whale, and its eye is lost at small sizes | Press coverage of the logo | The pet's eyes are deliberately oversized and high-contrast |
| No official pixel-art mascot or Q-version character exists | Reference search | The project duplicates nothing, and there was nothing to reuse |
| The logo whale faces left with its flukes raised at the upper right | A rendered copy of the official logo, inspected directly | See the orientation decision below |
| DeepSeek's own harness uses an animated whale tail as its "agent is working" indicator | The harness client sources | The pet's `working` frame raises the tail instead of inventing a new signal |
| The companion desktop pet's artwork is not reusable | That project's own notice: no commercial use, mandatory attribution, and its art is soft-shaded animation rather than pixel art | Confirms that original artwork is required; nothing from it was copied |

Orientation: the pet faces right rather than matching the logo's left-facing pose.
Matching the official pose would move the character toward the part most protected
by DeepSeek's brand rules, and facing right points the pet into the screen
content. It is a single constant to change if the owner prefers the logo pose.

The official silhouette was not traced or modified, no wordmark is used, and this
project is not affiliated with DeepSeek.

### Documents updated

- `requirements.md`: two new requirements (`R-B10`, `R-B11`), four modified
  requirements, one new assumption (`A-9`), and the question table split into
  answered and still open.
- `architecture.md`: the deployment-mode section now describes both modes as
  supported; a standby-default subsection; TLS moved onto the critical path in
  the memory budget; the security table states the accepted plain-text risk; the
  risk table gained the credential-loss row; and the pet section now carries the
  concrete design.
- `voice-backends.md`: the decision record reflects device-direct support, and the
  transcription selection changed to a streaming baseline.
- `roadmap.md`: `M0` marked complete; `M1` now includes a live-TLS memory
  measurement and an idle-current measurement; `M3` includes the direct-mode path;
  `M5` and `M7` reflect the deferred update and battery work.
- `art/`: new — the sprite generator, eleven frames, and a README recording the
  design, the palette, the reference research, and the licence position.

### Still open

`Q-2` (default voice), `Q-3` (out-of-home delivery), `Q-7` (writing a setup URL
to the NFC tag), and `Q-9` (whether the persona is user-editable). None blocks
`M1`.

### Next steps

1. Start `M1`: the measurement-only firmware. It now answers two questions that
   can change the design: the largest free block with a live TLS session, and the
   idle current with the network held open.
2. Audio loopback through the real codec path, confirming the mute-during-playback
   rule is enforceable in code.
3. Collect product-owner feedback on the pet art before `M6` depends on it.

### Validation run for this session

| Check | Result |
| --- | --- |
| Repository checks (`tools/check_repo.py`) | **PASS** — all text files scanned; every English/Chinese pair present and structurally aligned; links, secret patterns, and root-Markdown rules clean |
| Host tests | **PASS, unchanged** — no C source, Python source, or build configuration was touched, so the suite was not re-run |
| Firmware build | **NOT RUN** — no ESP-IDF environment is activated, and no firmware source changed |
| Device tests | **NOT RUN** — no board is connected |
| Unverified | The same items as Session 1, plus the pet design, which is a proposal awaiting the owner's sign-off |

---

## 2026-10-13 — Session 1: reconnaissance, research, and the design record

**Milestone:** `M0` in progress.

**First project commit:** `059c74a` — the design record and the fork readme.

### What was done

1. Read the repository's mandatory agent entry point and its routing table, and
   confirmed the fork workflow rules that constrain where fork-owned content may
   live.
2. Established the hardware facts from the single source of truth rather than
   from the chip datasheet, and read the board's own measured experience notes.
3. Researched three open questions in parallel: what the companion agent-side
   project actually exposes, which real-time voice architectures fit this chip,
   and how provisioning plus a local web console should be built.
4. Wrote the requirement record from the product owner's brief, the architecture,
   the provider survey, and this roadmap.
5. Created the fork readme pair.

### Hardware facts confirmed

Taken from the board's pin definition file and its hardware guide, not inferred.

| Fact | Value |
| --- | --- |
| MCU | ESP32-C3, single core, no PSRAM, 8 MB flash |
| Display | ST7789P3 240 × 320, SPI, one 9.6 KB partial draw buffer |
| Audio | ES8311, full-duplex I2S, 16 kHz 16-bit mono, microphone and speaker |
| Input | Three buttons on one ADC ladder, plus a hardware power button |
| Radio | 2.4 GHz Wi-Fi station and access point, Bluetooth 5 LE peripheral |
| Battery | 500 mAh with a fuel gauge, USB Type-C charging |
| Console | Native USB serial/JTAG on fixed pins; the UART default conflicts with the backlight |
| Partitions | One factory application; no OTA slots |

### Measured evidence found inside this repository

These are the numbers the design leans on, all from existing upstream notes on
the same board.

| Measurement | Value | Source |
| --- | --- | --- |
| Free system heap with an 80 KB LVGL pool and a rich UI | About 131 KB | LVGL pool budgeting reference note |
| Largest free block before a soft access point | About 13 KiB, rising to about 31 KiB after deferring audio initialization | SoftAP provisioning reference note |
| Working single-client HTTP server configuration | 3 sockets, backlog 2, 6 KB task stack, 2880-byte TCP windows | SoftAP provisioning reference note |
| A real JSON failure | A fixed 4096-byte document met a 6971-byte response | Network audio streaming reference note |
| MP3 decoder cost | 30–40 KB flash plus a measured 24 KiB compressed-input buffer | Audio compression and streaming reference notes |
| Opus decoder cost | 60–80 KB flash plus RAM | Audio compression reference note |
| Raw PCM at 16 kHz mono | 32000 B/s | Audio compression reference note |

### Research findings

**Companion agent-side project (reported, not verified against a running host).**
The companion plugin repository is a DeepSeek Harness plugin package rather than a
standalone application. It publishes read-only local JSON endpoints for work
status and incremental notification frames, and an endpoint accepting answers to
pending questions. Two constraints came out of that work and both matter:

- Its own documentation treats exposing the harness web server to the network as
  remote-code-execution equivalent, and its server binds to loopback only.
- The question-delivery endpoint is described inside that repository as a
  degraded path that does not fire reliably, so question capture must be verified
  first and must not be assumed.

Consequence: work-mode integration goes through a purpose-built bridge that is the
only LAN-facing surface.

**Voice architecture.** Two published facts collapsed the design space. Free heap
with the Wi-Fi station plus one TLS connection is about 210 KB on this chip with a
default configuration, and the Espressif audio front end — including every
echo-cancellation mode, which additionally needs PSRAM — does not support this
target. The one genuinely useful positive finding is that raw linear PCM output is
available from several synthesis providers, which removes the audio decoder from
the firmware entirely.

**Provisioning and the console.** The ESP-IDF provisioning manager's soft-access-
point transport speaks protobuf and requires a vendor phone SDK, so it cannot
satisfy "no separate application"; the provisioning server and captive portal must
be written directly. The pinned LVGL version already contains a QR widget that
stores its canvas at one bit per pixel, so no new dependency is needed for the
code. A partition-table change becomes necessary only if over-the-air update is
required.

### Decisions

| ID | Decision | Reason |
| --- | --- | --- |
| D-1 | The new fork readme is a repository-root readme pair; the upstream project overview document is left untouched | The fork workflow reserves the root readme for the fork owner, forbids editing the upstream overview in place, and renaming it would create a conflict on every upstream sync. Creating the root readme also wins GitHub's display priority, so it replaces the old readme as the visible project page |
| D-2 | Fork-private design documents live under `docs/assets/` | That is the documented fork-owned location for architecture notes, product design, and supplementary documents |
| D-3 | Provider orchestration lives in a companion bridge by default; device-direct calls are a configurable fallback only | Removes the largest RAM risk, keeps keys off a losable device, makes providers configurable without a firmware release, and normalizes audio format |
| D-4 | Work mode reaches the agent host only through the bridge, never directly | The agent host's local server is loopback-only for a documented security reason |
| D-5 | Half-duplex push-to-talk with the microphone muted during playback | Echo cancellation is unavailable on this hardware for two independent reasons; without the mute the device transcribes its own speaker |
| D-6 | Raw PCM end to end; no audio codec on the device | Measured decoder costs are large relative to a 210 KB heap, and providers offer PCM for free |
| D-7 | Pixel art is rendered from flash-resident image descriptors, not from many LVGL objects or per-frame canvases | Object and canvas allocations come out of a small LVGL pool; flash is effectively free |
| D-8 | The provisioning server is written directly rather than using the provisioning manager's soft-access-point transport | The manager's transport requires a vendor phone application and provides no portal page |
| D-9 | Over-the-air update is deferred until the product owner answers `Q-6` | It forces a partition-table change and a rollback policy, which is not a decision to make silently |
| D-10 | Wake-word activation is deferred rather than declared impossible | The small no-PSRAM wake-word models do support this chip; the reason to defer is cost and idle power, not capability |
| D-11 | The design documents are this set: requirements, architecture, voice back ends, roadmap, and this log | One authoritative home per question, no duplicated source of truth |
| D-12 | Every quantitative claim in the architecture document is labelled as a measurement, a published figure, or an estimate | Two of the first drafts contained estimates that read as facts; the board has enough measured experience available to avoid that |

### Verification status of the research

| Claim | Status |
| --- | --- |
| Companion plugin endpoints, payload shapes, and the loopback-only bind | Reported from the project's public sources; **not** exercised against a running host. Verify in `M4` |
| The question endpoint is a degraded path | Reported by that project's own documentation; treat as a hard risk |
| Free heap with Wi-Fi plus one TLS connection | Published figure for this chip, not measured on this board. `M1` measures it |
| Audio front end excludes this target, and echo cancellation additionally needs PSRAM | Published component and documentation facts |
| Wake-word models support this target | Published component fact |
| Raw PCM output available from synthesis providers | Documented by the providers |
| LVGL's in-tree QR widget at one bit per pixel | Reported from the pinned LVGL version's sources; verify against the resolved component after the first build |
| Time to first byte for synthesis providers | **Unverified.** Not published; must be measured |
| Provider prices | Indicative only, as of the survey date |

### Open questions carried forward

`Q-1` through `Q-10` in [requirements.md](requirements.md#6-open-questions) are
unanswered. The ones that block implementation are `Q-1` (the pet's design), `Q-4`
(whether device-direct calls are needed at all) and `Q-8` (the default power
profile and the acceptable standby delivery delay). `Q-2`, `Q-5`, `Q-6`, `Q-7`,
`Q-9`, and `Q-10` can be deferred to the milestone that needs them.

### Next steps

1. Collect the product owner's answers, starting with `Q-1`, `Q-4`, and `Q-8`.
2. Start `M1`: a measurement-only firmware that logs free heap, largest free
   block, and the LVGL pool at every phase boundary, plus an audio loopback check.
3. Verify the reported companion endpoints against a running host as soon as `M4`
   can begin, because a negative result changes the work-mode design.

### Repository state at the end of this session

```text
README.md / README.zh_CN.md                       new fork readme pair
docs/assets/README.md(+zh)                        fork asset and design index
docs/assets/deepseek-passport/requirements.md      requirement record
docs/assets/deepseek-passport/architecture.md      design and constraints
docs/assets/deepseek-passport/voice-backends.md    provider survey and decision
docs/assets/deepseek-passport/roadmap.md           milestones and validation plan
docs/assets/deepseek-passport/progress-log.md      this file
```

### Validation run for this session

| Check | Result |
| --- | --- |
| Repository checks (`tools/check_repo.py`) | **PASS** — all text files scanned; links, language pairs, secret patterns, and root-Markdown rules clean |
| Host tests, C logic suite | **PASS** — 8 of 8 test binaries compiled and passed with the host compiler |
| Host tests, Python modules | **PASS** — 6 of 6 modules passed, including the repository-checker and skill-installer suites |
| Required AI skills | **PASS** — all five required skills are installed at the discovery path `.agents/skills/` as relative links, and each `SKILL.md` is readable through that path |
| Firmware build | **NOT RUN** — no ESP-IDF environment is activated, and this change touches no firmware source |
| Device tests | **NOT RUN** — no board is connected |
| Unverified | The reported agent-side endpoints; the composition of the resolved LVGL component; every provider claim except those documented by the provider |

`tools/validate.sh` itself is not runnable on this host: it needs a POSIX shell,
`cc`, `python3`, and `actionlint`, and none of those names resolve here. Its steps
were run individually instead, with the host C compiler and the available Python.

One environment finding is recorded so the next session does not rediscover it:
under the session's initial confined file policy, both directory-symlink creation
and `tempfile.mkdtemp` directories failed with access denials, the latter blocking
any child directory. Both are confinement artifacts rather than repository
defects: with unrestricted file access in the same session, the skill installer
succeeded and every host test passed with no repository change in between.

No firmware source was changed. No flash, no device test, and no firmware build
was run.
