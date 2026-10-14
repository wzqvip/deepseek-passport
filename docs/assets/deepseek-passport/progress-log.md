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

## 2026-10-13 — Session 1: reconnaissance, research, and the design record

**Milestone:** `M0` in progress.

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
| Host tests, Python modules | 5 of 6 **PASS**; `tests/test_install_passport_skills.py` fails three assertions because this machine cannot create directory symlinks without elevation. That is a pre-existing environment limitation, independent of a documentation-only change, and those assertions never read the repository |
| Firmware build | **NOT RUN** — no ESP-IDF environment is activated, and this change touches no firmware source |
| Device tests | **NOT RUN** — no board is connected |
| Unverified | The reported agent-side endpoints; the composition of the resolved LVGL component; every provider claim except those documented by the provider |

`tools/validate.sh` itself is not runnable on this host: it needs a POSIX shell,
`cc`, `python3`, and `actionlint`, and none of those names resolve here. Its steps
were run individually instead, with the host C compiler and the available Python.

Two environment findings are recorded so the next session does not rediscover
them:

- The repository's skill installer requires true directory symlinks. Windows
  blocks those without Developer Mode or elevation, so the five required skills
  could not be installed to the discovery path in this session; their canonical
  sources were read directly instead, which is not the same thing as a successful
  installation.
- The host `tempfile.mkdtemp` creates its directory with mode `0o700`, and this
  sandbox denies creating entries inside a directory created that way. The Python
  test modules were therefore run through a small wrapper that normalizes
  directory creation to the default mode. It is not part of the repository and it
  changes no behaviour under test.

No firmware source was changed. No flash, no device test, and no firmware build
was run.
