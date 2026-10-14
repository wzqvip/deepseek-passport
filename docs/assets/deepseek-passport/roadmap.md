<p align="right">
  <a href="roadmap.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# DeepSeek Passport Roadmap

Milestones are ordered by risk: the parts that can invalidate the whole design
come first, and visible product polish comes last. Requirements are referenced by
the identifiers in [requirements.md](requirements.md); design detail is in
[architecture.md](architecture.md).

## Status

| Milestone | Goal | Status |
| --- | --- | --- |
| `M0` | Requirements, architecture, and decisions recorded | **Complete** — documents written and the product owner answered `Q-1`, `Q-4`, `Q-5`, `Q-6`, `Q-8`, and `Q-10` on 2026-10-13. `Q-2`, `Q-3`, `Q-7`, and `Q-9` stay open and block nothing |
| `M1` | Prove the memory and audio budget on hardware | Not started |
| `M2` | Bridge, configuration store, network, and a text-only conversation | Not started |
| `M3` | Voice in and voice out | Not started |
| `M4` | Work mode with a live agent session | Not started |
| `M5` | Provisioning and the web console | Not started |
| `M6` | Pixel pet, art, fonts, and localization | Not started |
| `M7` | Standby, power profiles, and release polish | Not started |

## M0 — Requirements, architecture, and decisions

**Deliverable:** the document set under `docs/assets/deepseek-passport/`, the fork
readme pair, and a dated decision log.

**Exit criteria:**

- Every capability the product owner asked for is either an identified
  requirement or an explicitly recorded assumption.
- Hardware constraints that forbid a capability are documented with a reason, not
  a guess.
- Open questions are listed with the consequence of each answer.
- The [progress log](progress-log.md) records what was read, decided, and left
  open.

## M1 — Memory and audio budget spike

**Goal:** replace every estimate in the architecture document with a measurement
before writing application code. This milestone can end in a "no" that changes the
design, and it is deliberately first.

**Deliverables:**

- A minimal firmware that initializes the display, LVGL, the button path, and the
  audio path, brings up the Wi-Fi station, and logs free heap, largest free
  block, and `lv_mem_monitor()` at each step.
- A measurement report: boot, after Wi-Fi, after access point start, after a
  sustained PCM capture, after a sustained PCM playback, and after ten
  screen enter/exit cycles.
- A decision on the LVGL pool size, the capture and playback ring sizes, and what
  the direct-mode provider path costs in RAM — measured with a live TLS session,
  not estimated.
- The always-connected idle state entered and held, so that idle current can be
  measured externally with a USB power meter. The firmware cannot measure its own
  current — the board has no current-sense path — so this number is an instrument
  check and stays under `Unverified` until someone takes it.
- An audio loopback check: microphone to speaker through the real codec path,
  confirming the mute-during-playback rule is enforceable.

**Exit criteria:** measured headroom is recorded for each phase, the largest free
block is known at every phase boundary, and the architecture document's numbers
are corrected with the measurements.

## M2 — Bridge, configuration, and text conversation

**Goal:** a text-only conversation with DeepSeek works end to end, through the
bridge, with configuration stored on the device.

**Deliverables:**

- The bridge service with the three provider interfaces, one working adapter for
  each stage, and a plain-HTTP, token-authenticated device API.
- Device-side configuration store in NVS with a versioned schema, defaults, and
  secret redaction.
- Wi-Fi station connection with reconnect policy, credential storage, and a
  missing-network state that does not block the UI.
- A minimal application shell that is not the upstream demo menu: a home screen,
  a text entry path, and bubbles showing the reply.
- Host tests for the configuration codec, the bridge protocol codec, and the
  conversation state machine.

**Exit criteria:** a message typed into the device is answered by DeepSeek and
shown on screen; disconnecting the bridge produces a visible, recoverable error;
the configuration survives a power cycle.

## M3 — Voice in and voice out

**Goal:** the spoken loop of requirement group `R-B`.

**Deliverables:**

- Push-to-talk capture at 16 kHz mono, chunked to the bridge while still
  recording, with an energy-based end of utterance and a hard maximum length.
- Speech-to-text wired through the bridge, with the transcript displayed.
- Streaming reply text, sentence segmentation on the bridge, and per-sentence
  synthesis.
- Playback of raw PCM at the provider's native rate, with the codec opened at
  that rate for the session.
- The enforced microphone mute during playback, with a host test for the state
  machine and an on-device check for echo.
- A latency instrumentation path that records each segment of the turn.
- The direct-mode path: the same three stages performed by the device against
  provider endpoints, with strict serialization of TLS sessions and a size cap on
  every response. This is in scope because the product owner approved it (`Q-4`).

**Exit criteria:** requirement `R-B7` is measured, not asserted; the reply is
audible and intelligible; the device does not transcribe its own voice; playback
does not underrun while the screen is animating.

## M4 — Work mode with a live agent session

**Goal:** requirement group `R-C`, including the security constraint `R-C7`.

**Deliverables:**

- Bridge-side attachment to the local agent host over its loopback interface
  only, with the question and notification sources verified early rather than
  assumed.
- The device-facing state protocol with sequence numbers, long polling, and
  acknowledgement.
- Progress rendering, a question screen with `UP` / `DOWN` / `OK` selection, and
  the answer round trip.
- Completion notification: silent, chime, or spoken, per setting.
- Voice instruction into the running session from work mode.
- A guaranteed-delivery rule for a question that arrives while the device is
  asleep.

**Exit criteria:** a real session's progress appears on the device; a real
question is answered from the device and the agent proceeds; a turn ending while
the device is idle produces the configured notification; the agent host is not
reachable from the LAN in any configuration used by this milestone.

## M5 — Provisioning and web console

**Goal:** requirement groups `R-E` and `R-F`.

**Deliverables:**

- Hand-written access point provisioning with a QR code on screen, both the Wi-Fi
  association payload and the page address, a wildcard DNS responder, and the
  connectivity-probe handlers.
- Credentials persisted only after a successful station connection.
- The web console: status, configuration, provider tests, Wi-Fi changes, and
  maintenance actions.
- The console asset pipeline: one self-contained page, gzipped at build time,
  embedded in flash.
- Version reporting in the console. Firmware update over the air is deferred
  (`Q-6`), so the partition table is not changed in this milestone.

**Exit criteria:** a factory-fresh device is provisioned from a phone without any
companion application; the console configures every provider and runs a
successful test of each; secrets are never echoed back; the provisioning service
is fully torn down afterwards.

## M6 — Pixel pet, art, fonts, and localization

**Goal:** requirement group `R-G`, and the product's actual identity.

**Deliverables:**

- The asset pipeline script and its reproducible output, checked in with licence,
  converter version, and byte costs recorded.
- The Q-style whale pet with its expression set and idle animation, drawn from
  flash-resident image descriptors.
- The pixel CJK font subset with a coverage check in the automated gate.
- The full screen set: home, conversation, work progress, question, provisioning,
  QR, error, and standby.
- Simplified Chinese and English strings for every screen.

**Exit criteria:** the pet animates without growing the resident object count;
Chinese renders on the device with no blank boxes; the LVGL pool high-water mark
is documented for every screen; no screen, card, or menu from the upstream demo
is reused.

## M7 — Standby, power, and release

**Goal:** requirement groups `R-D` and `N-1` to `N-6`.

**Deliverables:**

- Idle screen-off and wake on any function button, preserving the existing
  key-wake guard behavior.
- The always-connected profile as the default, with its idle current measured and
  stated in the console. The battery profile and deep sleep are deferred (`Q-8`);
  the upstream key-wake guard behavior is kept intact for them.
- Deep-sleep entry conditions and idle-current measurements per profile.
- Battery indication and a low-battery warning animation.
- Release packaging: a merged image, version reporting, and the completion
  checklist.

**Exit criteria:** measured idle current per profile; a question is not lost in
either profile within its stated delay; repeated navigation and idle cycles leak
no heap, task, socket, or timer; the release checklist is complete.

## Validation plan

| Layer | Method | Applies to |
| --- | --- | --- |
| Repository gate | `./tools/validate.sh --static` — repository checks plus host tests | Every change |
| Host tests | Pure logic compiled and run without ESP-IDF or LVGL: state machines, protocol codecs, configuration codec, question rendering, idle timing, QR payload construction | Every milestone |
| Firmware gate | `./tools/validate.sh --firmware` — build plus merged-image verification | Every firmware milestone |
| Memory checks | Boot and phase-boundary logs of free heap, largest free block, and the LVGL pool monitor; minimum-ever values across repeated cycles | `M1` onward |
| Device acceptance | Explicit, per-milestone, human-observed checks | `M1` onward |
| Security checks | Secret scan, credential-log check, and a check that the agent host is unreachable from the LAN | `M2`, `M4`, `M5` |

Every delivery report states Build, Host tests, Device tests, and Unverified
separately. A green build is never reported as device validation, and flashing a
device always requires explicit approval.

## Definition of done for the project

- Every must-level requirement in [requirements.md](requirements.md) is either
  satisfied and verified, or formally descoped with the product owner's decision
  recorded in the [progress log](progress-log.md).
- The web console configures every provider without editing source.
- A factory-fresh device reaches a working conversation from a phone-only setup.
- The agent host is never exposed to the network by this project.
- The [progress log](progress-log.md) contains the evidence for each claim.
