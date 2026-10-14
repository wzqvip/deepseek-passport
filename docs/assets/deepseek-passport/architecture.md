<p align="right">
  <a href="architecture.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# DeepSeek Passport Architecture

This document explains how the requirements in
[requirements.md](requirements.md) are met on the actual board, which designs
were rejected and why, and where the remaining risk is.

Hardware facts come from
[`components/bsp/include/bsp_pins.h`](../../../components/bsp/include/bsp_pins.h)
and the [hardware development guide](../../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md).
Anything labelled **estimate** must be replaced by a measurement before it is
relied upon.

## 0. Measured experience already in this repository

Four upstream reference notes describe work on this exact board. They are read
as primary evidence and are cited throughout this document; they are not
rewritten here.

| Reference | What it establishes |
| --- | --- |
| [Network audio streaming and memory](../../reference/phoenixzhc/network-audio-streaming-and-memory.md) | A bounded streaming pipeline: 24 KiB compressed input buffer, a 2304-sample PCM buffer, and an 8 KiB playback task stack were measured working points for one MP3 pipeline. Also: a fixed 4096-byte JSON document crashed on a real 6971-byte response. |
| [SoftAP provisioning and resource budgets](../../reference/phoenixzhc/softap-provisioning-and-resource-budget.md) | The **largest free block** before SoftAP was about 13 KiB on one firmware and rose to about 31 KiB by deferring audio initialization. A working single-client configuration server used three HTTP sockets, a backlog of two, a 6144-byte task stack, and 2880-byte TCP send/receive windows. |
| [LVGL pool budgeting without PSRAM](../../reference/starsms007/lvgl-pool-budget-without-psram.md) | With an 80 KB LVGL pool and a rich UI, the system heap settled near 131 KB free. `free_biggest_size`, not `free_size`, gates the next large allocation. Pool exhaustion freezes a half-drawn frame with a silent log. |
| [Audio compression trade-offs](../../reference/shinku-chen/audio-compression-trade-offs.md) | Measured decoder cost on this board: IMA-ADPCM is already present and negligible, MP3 costs 30–40 KB of flash, Opus costs 60–80 KB of flash plus RAM. Raw 16 kHz mono PCM is 32000 B/s. |

The single most important number above is the **largest free block**, not total
free heap. It is the resource this project will actually run out of.

Decisions taken by the product owner on 2026-10-13 are recorded in
[requirements.md](requirements.md#61-answered) and are reflected throughout this
document. The two that change the design most are that the device may hold
provider credentials and talk to providers directly, and that the default power
profile is always connected.


## 1. The one constraint that shapes everything

The board has **8 MB Flash and no PSRAM**. Flash is effectively free; RAM is not.
Every design decision below follows from that asymmetry:

- Code, fonts, sprites, and web assets live in Flash and are read in place.
- Audio is streamed in small chunks, never buffered whole.
- Only one network security context is ever open at a time.
- Provider orchestration, key storage, and audio transcoding move off the device
  and onto a companion process.

| Resource | Confirmed value | Consequence |
| --- | --- | --- |
| MCU | ESP32-C3, single-core RISC-V, no AI/vector extensions | No speech front end and no echo cancellation; only the small wake-word models fit (see below) |
| Flash | 8 MB, single factory application today | Room for a large CJK pixel font and sprite sheets, plus an OTA-capable layout if the partition table is reworked |
| RAM | Internal SRAM only, no PSRAM | All buffers, stacks, Wi-Fi, TLS, and the LVGL pool share one budget |
| Display | ST7789P3 240 × 320 RGB565 over SPI | One 9.6 KB partial draw buffer; no framebuffer |
| Audio | ES8311, full-duplex I2S, 16 kHz 16-bit mono, microphone and speaker | PCM only, in and out |
| Radio | 2.4 GHz Wi-Fi (STA and AP), Bluetooth 5 LE | Wi-Fi for providers and the bridge; BLE reserved |
| Input | Three buttons on one ADC ladder, plus a hardware power button | Navigation only; no keyboard, no touch |
| Battery | 500 mAh with a CW2017 gauge | Tethered and battery profiles must both exist |

### 1.1 What the chip cannot do

| Capability | Verdict | Reasoning |
| --- | --- | --- |
| Wake-word detection ("hey whale") | **Possible, deferred** | The small no-PSRAM wake-word models do support this chip, unlike the full audio front end. It is deferred because it costs a permanently listening microphone, flash, and CPU on a single-core part. The first release opens the microphone only when the user asks for it. |
| Full-duplex barge-in with echo cancellation | **Not available** | Two independent blockers: the Espressif audio front end does not support this chip, and every one of its echo-cancellation modes additionally requires 64–126 KB of PSRAM, which this board does not have. Half-duplex push-to-talk with the microphone hard-muted during playback is the design that works. Without that mute, the speaker output feeds straight back into the transcriber. |
| On-device speech-to-text or text-to-speech | **Not available** | Model size and RAM. Both are network services in this design. |
| Compressed audio decode (MP3, Opus, AAC) | **Possible but rejected as the default** | Measured on this board, an MP3 path costs 30–40 KB of flash and wanted a 24 KiB compressed-input buffer on top of decoder state, and an Opus path costs 60–80 KB of flash plus RAM ([decoder costs](../../reference/shinku-chen/audio-compression-trade-offs.md), [streaming buffers](../../reference/phoenixzhc/network-audio-streaming-and-memory.md)). Requesting linear PCM from the text-to-speech provider removes all of it. A decoder stays a deliberate later option, budgeted as its own milestone. |
| Parallel TLS sessions | **Avoid** | Each session costs tens of KB. Speech-to-text, the language model, and text-to-speech are serialized. |
| Full-screen animated framebuffer | **Rejected** | A full 240 × 320 RGB565 framebuffer is 150 KB. Rendering works from Flash-resident images through the 9.6 KB partial draw buffer instead. |
| A captive portal that needs no typing on any phone | **Best effort only** | Portal auto-popup varies by phone vendor and OS version. The design always shows the literal address and a QR code as the reliable path. |
| SoftAP and an active BLE connection at the same time | **Avoid** | Wi-Fi access point plus a connected BLE peer is documented by Espressif as supported-but-unstable on this chip family. Provisioning runs without BLE. |

## 2. System architecture

Three tiers. The device is a thin terminal; the bridge owns orchestration.

```text
+--------------------------------------------------------------------------+
|  Tier 1 - Device (ESP32-C3, this repository's firmware)                  |
|                                                                          |
|   pixel UI (LVGL)   pet state machine   voice state machine              |
|        |                   |                    |                        |
|   BSP display         button input          BSP audio (mic / speaker)    |
|        \___________________|____________________/                       |
|                            |                                             |
|              application services:                                       |
|                wifi manager | config store (NVS) | passport link client  |
|                audio codec path | provisioning (SoftAP + captive portal) |
|                web console (HTTP server + static SPA)                    |
+--------------------------------|-----------------------------------------+
                                 |  LAN, plain HTTP, pairing token
                                 |  POST /passport/v1/audio   (PCM up)
                                 |  POST /passport/v1/message (text up)
                                 |  GET  /passport/v1/events  (long poll / stream)
                                 |  POST /passport/v1/answer  (choice down->up)
+--------------------------------|-----------------------------------------+
|  Tier 2 - Passport Bridge (user's computer, Node or Python)              |
|                                                                          |
|   provider adapters      session store        DSH session mirror         |
|   - STT  (Whisper, Gemini, ...)                 |                        |
|   - LLM  (DeepSeek, OpenAI-compatible)          | reads 127.0.0.1 only   |
|   - TTS  (see section 3.6)                      v                        |
|   PCM normalization (resample, gain, VAD)   DSH local HTTP endpoints      |
+--------------------------------|-----------------------------------------+
                                 |  HTTPS, provider credentials
+--------------------------------|-----------------------------------------+
|  Tier 3 - Providers: DeepSeek, speech-to-text, text-to-speech            |
+--------------------------------------------------------------------------+
```

### 2.1 Two supported deployment modes

The product owner approved both modes (`Q-4`): the device may hold provider
credentials and call providers directly, and plain-text credential storage is
accepted. The bridge is therefore not a prerequisite for conversation. It is what
adds agent-session mirroring, provider switching without a firmware release, and
keeping credentials off a device that can be lost.

| Mode | What it needs | What it gives |
| --- | --- | --- |
| **Direct** — the standalone baseline | Provider credentials on the device, one serialized TLS session per stage, reduced TLS buffers | A complete voice turn with no computer involved |
| **Through the bridge** — recommended whenever a computer is available | A bridge on the same network | Work mode, provider flexibility, credentials kept on the computer, and one audio format on the device side |

The trade-offs that shaped the first draft still apply; they now explain why the
bridge is *preferred* rather than why direct mode is excluded:

| Concern | Direct | Through a bridge |
| --- | --- | --- |
| Peak RAM | One TLS session per stage, three stages, strictly serialized, plus response buffers, alongside the UI | One plain-HTTP socket on the LAN for all three stages |
| Credentials | Stored on a device that can be lost, in plain text as the owner accepted | Stay on the computer |
| Audio format | The device must accept whatever rate the provider emits | The bridge normalizes to one rate |
| Provider churn | Each provider change is a firmware release | A bridge configuration change |
| Latency | One fewer hop | One extra LAN hop, roughly 5–20 ms — negligible next to provider latency |
| Without a computer | Works | Conversation is unavailable; work mode needs the computer in both modes |

Because direct mode is now on the critical path, its memory cost is a first-class
risk rather than a footnote. What makes it plausible on this board:

- Stages are strictly serialized and each connection is closed before the next
  opens, so only one security context is live at a time.
- TLS input and output content lengths are reduced from their defaults, and the
  firmware's own buffers are allocated once at start-up rather than per turn.
- Every response has an explicit size cap, and each parser is sized from that
  cap, because a fixed 4096-byte document already caused a crash on this board.
- `M1` measures the largest free block with a live TLS session, before any
  provider code is written. A negative result changes this decision; it does not
  change the measurement.

## 3. Voice conversation pipeline

### 3.1 Interaction model

Half-duplex, button-owned, with an explicit visible state. No always-on
microphone, no wake word.

```text
idle -> [OK pressed] -> listening -> (silence or [OK]) -> thinking
     -> speaking -> idle
         ^                        |
         +---- [OK] interrupts ---+   (stop playback, start a new turn)
```

| State | Screen | Microphone | Speaker |
| --- | --- | --- | --- |
| `idle` | pet idle animation | closed | silent |
| `wake` | pet looks up, bubble appears | closed | short acknowledgement tick |
| `listening` | pet listening, level meter, "tap OK to send" | open, streaming | silent |
| `thinking` | pet thinking animation, transcript bubble shown | closed | silent |
| `speaking` | reply bubble grows with the text | closed | playing PCM |
| `error` | pet distressed, error bubble, retry hint | closed | optional short error tone |

`OK` while `speaking` stops playback and immediately starts `listening`: that is
the barge-in the hardware can actually deliver.

### 3.2 Capture

- Format: 16 kHz, 16-bit, mono PCM, matching the codec path already used by the
  upstream audio demo.
- The audio worker calls `bsp_audio_read()` in fixed chunks and pushes them into
  a small ring buffer, then to the network worker. Capture never runs in a button
  callback or in the LVGL task.
- End of utterance is decided by a lightweight energy threshold plus a silence
  timeout, both configurable, with a hard maximum utterance length. This is a
  cheap level detector, not a speech model.
- A user-visible level meter is fed from the same chunk statistics, so the user
  can see that the device is hearing them.
- Chunk size and ring depth are the core RAM knob: 16 KB of capture ring is
  about half a second of audio at 32 KB/s.

### 3.3 Transport to the bridge

Audio is uploaded while it is still being captured, not after the user stops.
Two equivalent encodings are specified; the implementation picks one per
milestone and records the choice in the progress log:

1. **Streamed POST** — `Content-Type: application/octet-stream`, chunked transfer
   encoding, raw PCM, with turn metadata in headers. Simple, and it works with
   `esp_http_client` streaming writes. The response carries the final transcript.
2. **Short-turn POST** — the whole utterance in one request with
   `Content-Length`. Simpler still, but it caps the utterance at the size of the
   capture ring.

The bridge replies with a single JSON document per turn describing the transcript
and the reply, or an error object. The device does not need to understand SSE
from the bridge; streaming is the bridge's internal concern.

In direct mode the device performs the same three stages itself, one serialized
HTTPS connection each: upload the utterance to the speech-to-text endpoint, stream
the language-model response, then synthesize sentence by sentence. It parses the
provider's server-sent-event stream directly. That is more device code than the
bridge path, and it is the point at which the device stops being a thin terminal.

### 3.4 Reply: text first, then speech

The bridge streams the language-model response, splits it into speakable
sentences, and synthesizes and returns each sentence as it completes. The device
therefore starts speaking after the first sentence rather than after the whole
reply.

Device-side playback rules:

- One audio worker task owns the speaker. It takes a sentence queue and calls
  `bsp_audio_write()` in chunks.
- Playback chunk size is chosen from the I2S DMA budget (six descriptors of 240
  frames is 90 ms of audio at 16 kHz) so that Flash writes or a UI redraw cannot
  open an audible gap.
- Sample rate is matched, not resampled, wherever possible. Providers emit raw
  PCM at different native rates, and the codec supports more than one: the rule
  is to open the audio path at the provider's native rate for the session rather
  than resample on the device. Where a provider's rate is fixed and awkward, the
  **bridge** resamples, so the device always sees one rate per session. A
  mid-session rate change goes through `bsp_audio_set_format()` and its
  documented close/open cost.
- The **microphone path is muted from the first playback byte until the speaker
  has drained**, and that rule is not optional: this device has no echo
  cancellation, so an open microphone during playback transcribes the pet's own
  voice. Push-to-talk makes the rule natural; the implementation must still
  enforce it in code rather than rely on the interaction model.
- Reply text is displayed as it arrives, in the pet's bubble.

### 3.5 Latency budget

Targets, not measurements. They exist so that a latency regression is visible.

| Segment | Target | Notes |
| --- | --- | --- |
| Capture stop to upload start | < 100 ms | Upload overlaps capture |
| Upload of a 4 s utterance | 100–400 ms | About 128 KB over Wi-Fi |
| Speech-to-text | 400–1200 ms | Provider dependent |
| Language model, first sentence | 400–1000 ms | Streaming |
| Text-to-speech, first audio | 300–900 ms | Provider and voice dependent |
| Bridge and LAN overhead | 20–60 ms | |
| **First audio, typical** | **1.2–3.5 s** | Requirement `R-B7` commits to a 2.5 s median and a visible thinking state |

### 3.6 Provider abstraction

The bridge defines three narrow interfaces. A back end is a name, a base URL, a
model, a key, and a small adapter:

```text
SpeechToText: transcribe(pcm, sampleRate, language) -> text
LanguageModel: stream(messages, systemPrompt) -> text chunks
TextToSpeech:  synthesize(text, voice, sampleRate) -> pcm chunks
```

The device stores only *which* back ends are selected and the bridge address; by
default it stores no provider keys at all. Every back end accepts a custom base
URL because some providers are not directly reachable from every network
(assumption `A-4`).

Candidate back ends, to be finalized against open question `Q-2`:

| Stage | Candidates | Selection criteria |
| --- | --- | --- |
| Speech-to-text | Whisper-style HTTP transcription, Gemini, Deepgram, AssemblyAI | Streaming partials, Chinese accuracy, price, reachability |
| Language model | DeepSeek (default), any OpenAI-compatible endpoint | Cost, Chinese quality, streaming support |
| Text-to-speech | OpenAI TTS, ElevenLabs, Google, Azure, MiniMax, CosyVoice-class services | Chinese naturalness, PCM output at 16 kHz, latency to first byte, price |

The decisive selection filter for text-to-speech is **linear PCM output**. A
provider that returns raw PCM at a rate the codec can open removes the decoder
from the firmware entirely; a provider that returns only MP3 makes a 30–40 KB
decoder and its CPU cost mandatory.

Three provider-level constraints are recorded here because none is visible in a
text-only test:

- The default language-model endpoint may **enable a reasoning mode by default**,
  which produces seconds of silence before the first spoken word in a voice loop.
  It must be disabled explicitly on the conversational path.
- Most text-to-speech providers **do not publish time to first byte**, so the
  latency target `R-B7` cannot be validated from documentation. It has to be
  measured with a harness on the bridge.
- Partial transcription is now a **requirement rather than a preference**
  (`Q-5`): the screen shows text as it arrives. A back end that returns only a
  final transcript cannot be the default. Batch transcription stays supported as
  a cost-saving alternative that simply shows no live text.

The provider survey, current prices, and the rejected options are recorded
separately in [voice back ends](voice-backends.md).

## 4. Work mode: mirroring a DeepSeek Harness session

### 4.1 What exists on the computer side

The companion plugin repository `wzqvip/dsh-app` exposes local read-only JSON
endpoints on the DeepSeek Harness web server. As reported from that repository on
2026-10-13 (**not yet verified against a running host** — verification is
milestone `M4`):

| Endpoint | Payload |
| --- | --- |
| `GET /dsh-pet-7340/work-status` | `{state, task, ts}` with `state` in `thinking`, `working`, `result`, `waiting`, `success`, `error`, or `null` |
| `GET /dsh-pet-7340/notify` | Incremental frames carrying a monotonic `seq`: turn end, approval requested, question requested, agent error |
| `GET /dsh-efficiency/api/pending` | Pending agent questions: `{callId, questions:[{id, question, detail, header, options, multiSelect}]}` |
| `POST /dsh-efficiency/api/answer` | `{callId, answers:[{id, selected:[label], custom}]}` |

Two constraints dominate the integration:

1. The Harness web server deliberately binds to loopback. Exposing it to the LAN
   is treated by its own documentation as remote-code-execution equivalent. The
   device must therefore **never** talk to it directly.
2. The plugin endpoint that is documented as question delivery is itself
   described in that repository as a degraded path. Question capture must be
   treated as best-effort and verified early.

### 4.2 The bridge is the only LAN surface

```text
device --(LAN, token)--> bridge --(127.0.0.1 only)--> Harness plugin endpoints
```

The bridge polls the loopback endpoints (a local process may do so safely) and
republishes a small, stable, authenticated protocol to the LAN. This keeps the
Harness host unexposed and gives the device a contract that does not change when
the Harness internals change.

Device-facing protocol, versioned and deliberately tiny:

| Method and path | Purpose |
| --- | --- |
| `GET /passport/v1/hello` | Bridge identity, protocol version, supported features |
| `GET /passport/v1/state?since=<seq>` | Long poll: current work state, task text, pending question, event sequence number |
| `POST /passport/v1/answer` | `{questionId, selected:[label], custom}` |
| `POST /passport/v1/message` | Free-text or transcribed instruction into the running session |
| `POST /passport/v1/audio` | Voice turn: PCM up, transcript and reply down |
| `POST /passport/v1/ack` | Acknowledge an event sequence number |

Every request carries the pairing token in a header.

### 4.3 State mapping and on-device presentation

| Bridge state | Pet | Screen | Sound |
| --- | --- | --- | --- |
| `idle` (no session) | sleeping or idle | home, clock and battery | none |
| `thinking` | thinking animation | three-dot progress, task text | none |
| `working` | working animation | progress indicator and task text | none |
| `waiting` | alert, waving | the question and its options, selection highlight | attention chime |
| `success` | happy jump | result summary bubble | success chime or spoken summary |
| `error` | distressed | error text and the action to take | error tone |
| `result` | curious | result text | per user setting |

Behavioural rules:

- A `waiting` state is never lost. If the device is asleep or the screen is off,
  it wakes, chimes, shows the question and keeps it until it is answered, expires,
  or is dismissed with a long press.
- Answering acknowledges the event sequence number so the question is not shown
  again after a restart.
- Notifications are user-configurable per event class: silent, chime, or speak.
- Delivery is poll-based by default. The power profile decides the poll interval;
  the always-connected profile keeps the network and the link alive, and the
  battery profile trades delivery delay for idle current and says so in the web
  console (requirement `R-D2`).

### 4.4 Standby default

The product owner selected the always-connected profile (`Q-8`): while the device
is idle the network and the bridge link stay alive, so a question that arrives
with the screen off is delivered within the polling interval and is never lost.
Deep sleep and the battery profile are deferred, not cancelled; the existing
upstream key-wake guard behavior is preserved so that profile remains reachable
without redesign.

The consequence to state plainly: idle current will be dominated by the Wi-Fi
station, and the first honest measurement of it belongs in `M1` alongside the
memory numbers.

## 5. Provisioning and the web console

### 5.1 First-run flow

```text
boot -> credentials in NVS?
          |            |
         no           yes
          |            |
          v            v
   SoftAP + QR     connect as STA
          |            |
   phone scans QR      +--> ok  -> web console on LAN address
          |            |
          v            +--> fail -> keep SoftAP, show failure and QR again
   captive portal page
          |
   SSID + password -> connect -> store -> restart networking -> web console
```

Design decisions:

- **The provisioning server is written by hand, not taken from the ESP-IDF
  provisioning manager.** `esp_wifi_prov_mgr` with the SoftAP transport speaks
  protobuf over protocomm and requires Espressif's phone provisioning SDK. It
  has no captive portal, no DNS hijack, and no HTML page, so it cannot satisfy
  requirement `R-E2` ("no separate application"). It is rejected for this
  product; the cost is that the provisioning flow is our code to maintain.
- The device raises its own access point with a bounded client count (one or two
  stations for a single-user setup page) and runs one `esp_http_server` on
  port 80. Access point and station interfaces coexist on this chip; the radio
  timeshares one 2.4 GHz path.
- A small wildcard DNS responder answers every name with the access point
  address, and the known connectivity-probe paths are answered with a redirect
  to the setup page rather than with a success status. Several probes are HTTPS
  and cannot be redirected by DNS alone, so automatic pop-up is treated as a
  compatibility bonus, never as the primary path.
- **The screen always shows both a QR code and the literal address.** Two
  payloads are shown: the standard Wi-Fi association payload
  (`WIFI:T:WPA;S:<ssid>;P:<pass>;;`), which the native phone camera can act on,
  and a URL to the setup page for joining and configuring manually. The QR
  widget is LVGL's own, available in the pinned LVGL version at one bit per
  pixel; a 200 px code costs about 6.4 KB of the LVGL pool. Enabling it is a
  Kconfig change, not a new dependency.
- The access point name includes a short device suffix so several devices are
  distinguishable, and the access point is protected rather than open.
- The setup page is served from the same HTTP server and the same asset bundle
  as the normal console, restricted to the setup routes while unprovisioned.
- Credentials are written to NVS **only after the station connection has
  verified successfully**; the previous working pair is kept until then, so a
  typo does not strand the device. Association and address assignment are
  distinguished when reporting progress: joining the access point is not the
  same event as receiving a DHCP lease.
- Provisioning tears down the DNS responder, the HTTP server, and the access
  point when it completes. Keeping an access point and station interface up
  permanently costs heap and sockets on a device that has neither to spare.
- **Optional subsystems are released before networking starts.** One measured
  firmware on this board had a largest free block of about 13 KiB before the
  soft access point, which rose to about 31 KiB simply by deferring audio
  initialization ([reference](../../reference/phoenixzhc/softap-provisioning-and-resource-budget.md)).
  Provisioning therefore runs without the audio subsystem initialized, and the
  measured largest free block is logged before the access point starts, after it
  starts, after HTTP starts, and after a phone joins.
- Bluetooth is not used in the provisioning flow: an access point combined with a
  connected BLE peer is documented as unstable on this chip family.
- Credential clearing exists both on the device (long-press path) and in the
  console, and requires an explicit confirmation.

### 5.2 Storage

| Data | Location | Notes |
| --- | --- | --- |
| Wi-Fi credentials | NVS namespace `passport.net` | Written only by the provisioning flow |
| Provider and preference configuration | NVS namespace `passport.cfg` | A single JSON document with a version field, small enough to rewrite atomically |
| Conversation history | Not persisted by default, or a bounded circular buffer | Requirement `R-B9` forbids unbounded growth |
| Web console assets | A `storage` partition (LittleFS) or embedded compressed | See section 5.4 |
| Pixel assets and fonts | Application `.rodata`, read from Flash in place | No runtime copy |

### 5.3 Web console API

A small REST surface behind the SPA:

| Route | Purpose |
| --- | --- |
| `GET /api/status` | Version, uptime, heap, largest free block, RSSI, battery, mode |
| `GET /api/config` | Current configuration with **secrets redacted to a boolean** |
| `PUT /api/config` | Write configuration; empty secret field means "leave unchanged" |
| `POST /api/test/{stt,llm,tts,bridge}` | Run a single provider or bridge self-test |
| `POST /api/wifi` | Replace credentials and reconnect |
| `POST /api/ota` | Firmware upload |
| `POST /api/restart`, `POST /api/factory-reset` | Restricted maintenance actions |

Secrets are write-only: the console never echoes an API key back to the browser
(requirement `R-F7`).

### 5.4 Serving the console from 8 MB of Flash

The HTTP server in this ESP-IDF version has no static-file or gzip helper; the
response handlers are written by hand. Two viable approaches:

| Approach | Flash cost | RAM cost | Tradeoff |
| --- | --- | --- | --- |
| One hand-written HTML file with inline styles and scripts, gzipped at build time and embedded in application `.rodata`, served with an explicit `Content-Encoding: gzip` | **Estimate** 8–40 KB | No decompression buffer, because it is stored compressed | No partition change; the browser must send `Accept-Encoding: gzip`, which the handler checks |
| `storage` partition with a filesystem, assets written at build time | Asset size plus filesystem overhead | A few KB of read buffers | Assets update independently of the application; costs a partition, a mount step, and another failure mode |

Embedding is the default for a single configuration page. Everything is inlined
into one file so a browser does not open extra sockets, and a conservative
measured server configuration from this board's reference note is used as the
starting point: three sockets, a backlog of two, a 6 KB task stack, and small TCP
send and receive windows ([reference](../../reference/phoenixzhc/softap-provisioning-and-resource-budget.md)).

A partition table change is required for over-the-air update (open question
`Q-6`), because the current layout has one application slot and no `otadata`
partition. The recommended layout, in the same order as the current file:

```text
nvs        0x6000
otadata    0x2000
phy_init   0x1000
ota_0      ~0x1F0000
ota_1      ~0x1F0000
storage    optional, if the console assets become a filesystem
```

With two application slots of about 2 MB each plus a storage partition, an 8 MB
part is still comfortable. **Note:** reflashing with a merged image after a
partition change can reset stored NVS data; segmented flashing preserves it.
Application rollback must be enabled deliberately if the update path is to be
recoverable.

## 6. Pixel user interface

### 6.1 Visual language

- Original pixel art only. The upstream demo menu, its cards, and its mascot are
  prohibited for the application UI and are not reused.
- The palette is derived from the DeepSeek brand blue, extended with a small,
  fixed set of accent colors so the art stays coherent.

| Role | Suggested value | Use |
| --- | --- | --- |
| Brand blue | `#4D6BFE` | Pet body, highlights, primary actions |
| Deep blue | `#2B3A8F` | Shading, outlines on blue |
| Ink | `#101828` | Outlines, text |
| Paper | `#F2F5FF` | Bubbles, panels |
| Sky | `#BFD4FF` | Background gradient bands |
| Accent warm | `#FFB23E` | Attention, warnings |
| Success | `#39C46B` | Completed work |
| Error | `#E4573B` | Failures |

- Text is set in a pixel font, not an anti-aliased vector font, so the UI reads
  as one coherent style.
- The sprite palette is a smaller subset of the UI palette; it is recorded with
  the artwork in [`art/README.md`](art/README.md).

### 6.2 The pet

An original Q-version whale facing right, designed in the DeepSeek brand blue.
The artwork and its reproducible generator live in [`art/`](art/README.md); a
first pass exists and is awaiting sign-off.

| Property | Value |
| --- | --- |
| Logical grid | 32 × 32, written at 2× (64 × 64) so pixels stay visibly chunky |
| Palette | Eleven opaque colours plus transparency, anchored on `#4D6BFE`, the value DeepSeek's own organisation publishes as its brand colour |
| Expressions | Idle (a two-frame breathing bob), blink, curious, listening, thinking, working, speaking, happy, sleeping, distressed |
| Flash cost | 12,288 B per frame as RGB565A8; eleven frames is 132 KiB, which is not a constraint against 8 MB |

The pet faces right rather than matching the official logo's left-facing pose,
and its `working` expression raises the tail because DeepSeek's own harness uses
an animated whale tail as its "agent is working" indicator. The artwork is
original: the official silhouette was not traced or modified, and the companion
desktop pet's assets are not reusable because their licence forbids commercial
use and requires attribution. The reasoning is recorded in
[`art/README.md`](art/README.md#reference-research-and-independence).

Every frame keeps identical bounds, so animation swaps the image source on a
single widget and the resident object count never changes. The sprite is drawn
from flash by LVGL, so the runtime cost is the draw buffer that already exists,
not the sprite size.

Still to be produced: the bubble tail, the status-strip icons (Wi-Fi, battery,
microphone, work badge), the option-card frame, the progress dots, and the QR
frame.

### 6.3 Rendering strategy

Each LVGL object and canvas buffer costs RAM, so the design is
asset-driven:

- Backgrounds and sprites are **Flash-resident LVGL image descriptors**
  (`LV_IMAGE_DECLARE` / a generated `lv_image_dsc_t`). LVGL reads them through
  its partial draw buffer; no full-screen framebuffer exists.
- Sprites that need transparency are stored as `RGB565A8` (color plus an 8-bit
  alpha plane). A 56 × 56 sprite frame is about 9.4 KB of Flash, so a
  twelve-frame animation is about 113 KB — irrelevant against 8 MB of Flash,
  free in RAM.
- Frame animation swaps the image source on one widget with a timer or an LVGL
  animation. The object count stays constant.
- Opaque full-screen pixel scenes are plain RGB565 images, also drawn from Flash.
- The provisioning QR code uses LVGL's own QR widget, which stores its canvas at
  one bit per pixel. A 200 px code costs roughly 6.4 KB of the LVGL pool plus a
  couple of hundred bytes of encoder scratch, and a typical payload is a low QR
  version. It must not be rebuilt on every animation frame.
- LVGL object counts per screen are budgeted and reviewed like any other
  allocation. A frozen half-drawn frame with a silent log means pool exhaustion,
  not a display fault.

An asset pipeline script (Python, checked in, reproducible) converts source PNGs
into C descriptors:

```text
tools/assets/png_to_lvgl.py
  assets/pixel/<name>/frame_XX.png  ->  main/assets/<name>.c + <name>.h
  - exact palette quantization and a palette report
  - optional RGB565A8 or RGB565 conversion
  - generated header records the source file, palette, size, and byte cost
```

Assets follow the repository asset convention: reusable sources and generated
files live under `assets/`, with license, converter version, and command
recorded in the assets index.

### 6.4 Fonts

The default Montserrat fonts contain no CJK glyphs; Chinese text renders as blank
boxes unless a CJK font is compiled in and selected. The plan:

- Use an open-licensed **pixel CJK font** (for example Ark Pixel or Fusion Pixel
  families) at 12 px for body text and a smaller size for the status strip.
- Generate a **subset** with the pinned LVGL font converter, covering the Chinese
  and English characters the UI needs plus the characters that can appear in
  model replies, or a full common-character set if the flash cost is acceptable.
- Verify glyph coverage programmatically (a build-time character inventory check)
  and, separately, on the device, because a successful build does not prove that
  a glyph renders.
- Follow the repository's Chinese-font checklist. Font assets go under
  `assets/fonts/` with license and converter command recorded.

### 6.5 Screens

```text
 home / pet                     conversation                  work progress
+----------------------+      +----------------------+      +----------------------+
|  (#)  wifi  bat  mic |      |  (#)                 |      |  (#)  WORK     45%   |
|                      |      |      [ bubble ]      |      |                      |
|        (o o)         |      |   "what should I..." |      |  refactor parser     |
|       (  ^  )        |      |                      |      |  [####------] steps  |
|        whale         |      |   [ bubble ]         |      |                      |
|                      |      |   "sure, I can..."   |      |  (pet working)       |
|   [ OK ] hold: menu  |      |                      |      |                      |
+----------------------+      +----------------------+      +----------------------+

 agent question                 provisioning + QR            web console notice
+----------------------+      +----------------------+      +----------------------+
|  AGENT ASKS          |      |  SETUP WIFI          |      |  WEB CONSOLE         |
|  Which branch?       |      |   +------------+     |      |  http://192.168.    |
|  > main              |      |   |  QR CODE   |     |      |  4.1                |
|    feature/x         |      |   +------------+     |      |                      |
|    custom...         |      |  1 join Passport-3F  |      |  open on your phone  |
|  up/down  ok confirm |      |  2 open 192.168.4.1  |      |  to configure        |
+----------------------+      +----------------------+      +----------------------+
```

Every screen must stop its own timers, animations, and workers before it is
deleted, and non-LVGL contexts must hold `bsp_lvgl_lock()` for short UI updates.

## 7. Memory budget

All numbers below are **estimates to be measured**. Two measured anchors bound the
problem. Espressif's own figures for this chip put free heap at about 332 KB for
an empty project, about 219 KB with the Wi-Fi station running, and about 210 KB
with the Wi-Fi station plus one TLS connection, with each further concurrent
session costing tens of KB; their documented RAM-reduction settings raise the
station-plus-TLS figure toward 300 KB. Independently, an application on this board
with an 80 KB LVGL pool and a rich UI settled near 131 KB of free system heap.
Reducing the LVGL pool moves memory to the system heap, where Wi-Fi and TLS need
it.

| Item | Estimated cost | Kind |
| --- | --- | --- |
| LVGL internal pool (`CONFIG_LV_MEM_SIZE_KILOBYTES`) | 32 KB | Static |
| LCD partial draw buffer (240 × 20 RGB565) | 9.6 KB | Static |
| I2S DMA, both directions | ~6 KB | Static |
| Capture ring | 16 KB | Heap, allocated once |
| Playback ring | 16 KB | Heap, allocated once |
| Wi-Fi station stack | 40–60 KB | Heap |
| One TLS session, tuned buffers | 25–35 KB | Heap, on the critical path: required in direct mode and in any bridge that uses TLS |
| HTTP server plus one response buffer | 8–12 KB | Heap |
| Task stacks (UI, audio, net, input, HTTP) | ~25 KB | Static/heap |
| Sprites, fonts, SPA | Flash only | Flash |

Peak system heap is therefore dominated by Wi-Fi, one security context, and the
audio buffers: roughly 130–160 KB in bridge mode, plus about 25–35 KB when the
device talks to providers itself. **Total free heap is still not the binding
constraint.** The measured reference from this board is that the largest
contiguous free block was about 13 KiB before the soft access point and about
31 KiB after deferring audio initialization. Provisioning, TLS, and a JSON parse
each need one large contiguous allocation, so this project budgets the largest
free block first and everything else second.

Standing rules that follow from that:

- Log `lv_mem_monitor()`, `esp_get_free_heap_size()`, and the largest free block
  at boot and at every major state transition.
- Track the **minimum-ever** free heap and largest block across repeated
  enter/exit cycles, not the last sample.
- Derive every JSON parser capacity from the accepted response limit, and reject
  an oversized response before parsing. One measured firmware on this board
  crashed because a fixed 4096-byte document met a 6971-byte response
  ([reference](../../reference/phoenixzhc/network-audio-streaming-and-memory.md)).
- Allocate hot buffers once at start-up rather than per turn, so a long session
  does not fragment the heap into unusable slivers.
- Initialize audio lazily: it is the largest optional consumer and is not needed
  during provisioning or while idle.

## 8. Firmware module layout

Board hardware stays in `components/bsp`; everything else is application code in
`main`, on a `feature/*` branch.

| Module | Responsibility |
| --- | --- |
| `main/passport_app.c` | Boot sequence, top-level state machine, screen routing |
| `main/passport_pet.c` | Pet state, expression selection, animation scheduling |
| `main/passport_voice.c` | Voice state machine, capture and playback coordination |
| `main/passport_link.c` | Passport Link client: state polling, events, answers |
| `main/passport_wifi.c` | Station and access point lifecycle, reconnect policy |
| `main/passport_prov.c` | SoftAP provisioning, QR payload, captive portal routes |
| `main/passport_web.c` | HTTP server, REST API, asset serving |
| `main/passport_config.c` | Configuration schema, NVS persistence, defaults, redaction |
| `main/passport_ui_*.c` | Screens and widgets, one file per screen group |
| `main/assets/*.c` | Generated pixel assets and font descriptors |
| `tests/` | Host tests for every state machine, protocol codec, and layout calculation |

Testable logic — the voice state machine, the link protocol codec, the
configuration codec, question rendering, idle timing, and QR payload construction
— must be written so it compiles and runs on the host without ESP-IDF or LVGL,
following the existing `tests/` pattern.

## 9. Security model

| Asset | Threat | Control |
| --- | --- | --- |
| Provider API keys | Device loss, log exfiltration | Stored on the device in NVS, in plain text as the product owner accepted (`Q-4`); this is a deliberate loss-of-device risk. The controls that remain are absolute: never logged, never echoed back by the console, never committed to the repository |
| Harness session | LAN attacker reaching the agent host | The Harness server stays on loopback; the device sees only the bridge's narrow authenticated API |
| Bridge API | Unauthorized LAN client | Per-device pairing token, shown on the device screen during pairing, required on every request |
| Wi-Fi credentials | Log or repository leak | Stored only in NVS; the repository gate scans for secrets |
| Voice audio | Eavesdropping on the LAN | The bridge is expected to be used on a trusted network; the protocol is plain HTTP on the LAN by default, with TLS to providers. A future option is TLS on the bridge, at a RAM cost |
| Web console | Unauthorized configuration change | LAN-only binding; optional console password; secrets never echoed |

Encrypted NVS is available on this part through an HMAC key burned into eFuse,
which requires neither a separate key partition nor full flash encryption. It is a
candidate hardening step for a later milestone; it must be measured and verified
rather than assumed, and burning eFuse keys is irreversible, so it is a
deliberate one-way decision for the device owner and never a build default.

What NVS encryption does and does not buy, stated plainly: it protects stored
secrets against reading the flash chip or a dumped image. It does not protect
against someone holding the device with a serial cable, a hostile page served
from the same access point, or a key that leaves the device over plain HTTP on
the LAN. The controls that actually pay off here are the ones already chosen:
keys default to the bridge, secrets are never logged, every HTTP input is bounded,
the setup access point is protected and short-lived, and the previous working
credentials survive a failed new pair.

## 10. Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Peak RAM does not fit with Wi-Fi, one TLS session, and audio simultaneously | Blocking for direct mode | Measure the largest free block with TLS live in `M1`, before writing provider code; serialize stages; reduce TLS content lengths; allocate hot buffers once; keep the LVGL pool small; routing through the bridge remains a way to avoid device TLS entirely |
| Plain-text credential storage on a device that can be lost | Whoever holds the device holds the keys | Accepted by the product owner; mitigate by never displaying a secret on the device screen, never logging it, and stating the risk in the web console |
| Harness question capture is the degraded path described by the plugin | Work mode cannot answer questions | Verify in `M4` first; if unavailable, fall back to an approval/question source that does work, or add a purpose-built bridge plugin |
| Captive portal behaves differently across phone vendors | Users cannot provision | Always display the literal address and a QR to the page; support manual entry |
| Largest free block too small to start the access point or TLS | Provisioning or direct mode fails only after other features are up | Defer audio initialization, release optional subsystems first, allocate hot buffers once, and log the largest block at every phase boundary |
| SoftAP combined with a connected BLE peer | Unstable radio behavior | The provisioning flow is Wi-Fi only |
| Text-to-speech provider returns only compressed audio | Adds a decoder and its RAM | Prefer providers with PCM output; treat a decoder as a separately budgeted milestone |
| CJK pixel font size | Flash and lookup cost | Subset generation, coverage check in CI, on-device verification |
| LVGL pool exhaustion with animation | Frozen screen with a silent log | Budget object counts per screen, monitor the pool, keep animation object-count constant |
| Provider availability and cost changes | Feature breakage | Three narrow interfaces and per-back-end base URLs; no provider is hard-coded |
| A default reasoning mode produces dead air before the first spoken word | The voice loop feels broken | Disable reasoning explicitly on the conversational path and verify time to first audio |
| Text-to-speech time to first byte is unpublished | The latency target cannot be validated from contract | Measure several providers with a harness before choosing a default voice |
| Upstream divergence | Merge conflicts | Application work on `feature/*`; fork `main` keeps only the readme pair and `docs/assets/` |

## 11. Validation and delivery

- Every milestone runs `./tools/validate.sh --static` and reports its result.
- Firmware milestones additionally run `./tools/validate.sh --firmware`.
- Host tests cover state machines, protocol codecs, configuration codecs, and
  layout math.
- Device acceptance is a separate, explicit step with a per-milestone checklist;
  a green build is never reported as device validation.
- Every report states Build, Host tests, Device tests, and Unverified fields
  separately, and the [progress log](progress-log.md) records the evidence.
