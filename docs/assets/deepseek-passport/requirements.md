<p align="right">
  <a href="requirements.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# DeepSeek Passport Product Requirements

This document records what the product owner asked for, how it was decomposed,
and what is still undecided. It is the authoritative requirement source for this
fork. Hardware facts come from
[`components/bsp/include/bsp_pins.h`](../../../components/bsp/include/bsp_pins.h)
and the [hardware guide](../../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md);
the design that satisfies these requirements is in
[architecture.md](architecture.md).

Recording date: 2026-10-13. Recorded from a single spoken/written product brief;
every interpretation added by the recording agent is marked as an assumption
(`A-*`) rather than a requirement.

The brief was given in Simplified Chinese. Rows tagged **Owner brief** are
English paraphrases; the product owner's verbatim wording is preserved in the
Chinese version of this document (`requirements.zh_CN.md`).

## 1. Product summary

A pocket-sized DeepSeek companion built on the FoloToy AI Passport board. It is a
pet first — an interactive pixel character that lives on the home screen — and a
voice terminal and desktop-agent remote second. It is normally idle; a button
press wakes it.

## 2. Scope

In scope:

- Application firmware for the stock AI Passport board (`ESP32-C3`, 8 MB Flash,
  no PSRAM).
- A companion bridge process on the user's computer that performs provider
  orchestration and mirrors a DeepSeek Harness session to the device.
- Wi-Fi provisioning by access point, QR code, and captive portal.
- A device-hosted web console for configuration and updates.
- Original pixel-art UI, animation assets, and a pixel CJK font.
- Bilingual documentation and a dated progress log.

Out of scope for this project:

- Any hardware modification, additional wiring, or board revision.
- Replacing or rewriting upstream BSP internals.
- A multi-tenant cloud service.
- Reusing the upstream demo test menu or its visual shell as the application UI.

## 3. Functional requirements

Identifiers are stable and are referenced by the architecture and roadmap
documents. `MUST` is a hard requirement; `SHOULD` is a requirement whose
omission is a documented tradeoff.

### 3.1 R-A Home screen and pet interaction

| ID | Requirement | Notes |
| --- | --- | --- |
| R-A1 | The home screen MUST be the pet itself, not a list or menu. Every other screen is reached by an explicit action. | Owner brief: the home screen is the Q-style character, not a menu |
| R-A2 | The pet MUST be an original Q-style pixel character with idle breathing, blink, and at least four distinct reactions (happy, thinking, listening, sleeping). | User asked for a pixel-art style; assets must be original |
| R-A3 | The pet MUST show conversation as on-screen speech bubbles for both the user's transcription and the reply. | Owner brief: conversation appears as on-screen bubbles |
| R-A4 | The home screen MUST show a compact status strip: Wi-Fi state, battery level, work-mode state, and microphone state. | Needed to make the pet usable without entering menus |
| R-A5 | `OK` click MUST wake the device from standby and start a voice turn. `OK` long press MUST open the action sheet (settings, work mode, sleep). `UP` / `DOWN` MUST cycle the pet's reaction or page when a page is open. | Keeps the existing non-blocking input contract |
| R-A6 | The pet MUST react visually to input, audio level, and agent state changes. | Owner brief: the pet must be interactive |

### 3.2 R-B Real-time voice conversation

| ID | Requirement | Notes |
| --- | --- | --- |
| R-B1 | The device MUST capture microphone audio and obtain a text transcription from a configurable speech-to-text back end. | Suggested back ends include Whisper-style HTTP transcription and Gemini |
| R-B2 | The transcription MUST be sent to a configurable language-model back end, defaulting to DeepSeek, with OpenAI-compatible endpoints supported through a configurable base URL. | |
| R-B3 | The reply MUST be spoken through a configurable text-to-speech back end selected for natural-sounding Chinese and English output. | Owner brief: find a text-to-speech API that sounds good |
| R-B4 | Speech-to-text, language model, and text-to-speech MUST each be independently selectable, with per-back-end API key, base URL, model, and voice settings. | Owner brief: several API back ends must be configurable |
| R-B5 | Both the recognized user text and the assistant reply MUST be displayed as bubbles before or while audio plays. | |
| R-B6 | The default interaction MUST be half-duplex push-to-talk or a bounded listening window with a visible end-of-speech cue. Full-duplex barge-in is not a requirement because the target chip cannot run acoustic echo cancellation. | See `A-5` and the architecture document |
| R-B7 | The design MUST target a median first-audio latency of at most 2.5 s on a healthy network for the cascade, and MUST display a distinct "thinking" state beyond 1.5 s. | |
| R-B8 | Network or provider failures MUST produce a visible error state, a bounded retry policy, and no audio path deadlock. | |
| R-B9 | The conversation MUST keep a bounded multi-turn history and MUST NOT grow internal RAM without limit. | |

### 3.3 R-C Work mode: desktop linkage

| ID | Requirement | Notes |
| --- | --- | --- |
| R-C1 | The device MUST mirror the work state of a DeepSeek Harness session running on the user's computer: idle, thinking, working, waiting for input, success, and error. | Owner brief: push DeepSeek progress to the Passport |
| R-C2 | While the device is idle or on standby, receiving new progress MUST update the home screen without requiring a button press. | |
| R-C3 | When the agent offers choices, the device MUST render the question and options, allow selection with `UP` / `DOWN`, and send the answer back on `OK`. | Owner brief: show options on screen, then select and confirm with buttons |
| R-C4 | Question and answer payloads MUST preserve each option's label and the user's selection, and MUST support a free-text answer. | |
| R-C5 | When a turn completes, the device MUST play a chime, speak the result, or stay silent, according to a user setting. | Owner brief: play a notification sound, or read the result aloud |
| R-C6 | The device MUST support speaking a new instruction into the running session from work mode. | Owner brief: voice input is also supported |
| R-C7 | Work-mode integration MUST NOT require exposing the computer's agent web server to the local network. The device MUST reach the agent only through a purpose-built bridge with its own authentication. | Security constraint derived from the computer-side threat model; see the architecture document |

### 3.4 R-D Standby and power

| ID | Requirement | Notes |
| --- | --- | --- |
| R-D1 | The display MUST turn off after a configurable idle period and MUST return on any function-button press. | Owner brief: normally standby, and one button press wakes it |
| R-D2 | Standby MUST still surface a new work-mode question within a configurable delay budget. | Conflicts with deep sleep; resolved by an explicit power mode |
| R-D3 | The device MUST show battery level and warn on low battery with a distinguishable animation and chime. | |
| R-D4 | The device MUST enter deep sleep after a configurable idle period when work-mode delivery is disabled, and MUST wake on any function button. | The key-wake guard behavior already exists upstream and must be preserved |
| R-D5 | The device MUST offer at least two power profiles: a tethered profile that keeps the network and work-mode link alive, and a battery profile that sleeps and polls. | Explicit tradeoff, user-selectable |

### 3.5 R-E Wi-Fi provisioning

| ID | Requirement | Notes |
| --- | --- | --- |
| R-E1 | With no saved credentials, or on an explicit request, the device MUST start a SoftAP and display a QR code on screen. | Owner brief: with no network, raise a hotspot and provide a QR code for joining |
| R-E2 | The QR code MUST be scannable and MUST lead the phone to the provisioning page without requiring a separate application. | |
| R-E3 | The provisioning page MUST accept an SSID and password, MUST reject or warn about 5 GHz-only networks, and MUST report the connection result. | |
| R-E4 | The user MUST be able to clear saved credentials and re-enter provisioning from the device UI and from the web console. | |
| R-E5 | The device MUST NOT log or persist Wi-Fi passwords in plain text outside the credentials store. | |

### 3.6 R-F Web console

| ID | Requirement | Notes |
| --- | --- | --- |
| R-F1 | After provisioning, the device MUST serve a web console on its LAN address, usable from a phone browser. | Owner brief: a web management page follows |
| R-F2 | The console MUST configure every provider: API key, base URL, model, and voice, for speech-to-text, the language model, and text-to-speech. | Owner brief: configure the API keys and similar settings |
| R-F3 | The console MUST configure device preferences: pet name, persona, UI language, volume, brightness, idle timeout, chime policy, work-mode polling, and bridge address. | |
| R-F4 | The console MUST offer self-tests: play a tone, speak a test phrase, run a chat completion, and test the bridge connection. | |
| R-F5 | The console MUST support firmware update over the air and MUST show the running version. | |
| R-F6 | The console MUST show device diagnostics: heap, largest free block, uptime, Wi-Fi RSSI, and recent error codes, without exposing secrets. | |
| R-F7 | Secrets MUST be write-only from the console: stored values are shown as configured or not configured, never echoed back. | |

### 3.7 R-G Pixel UI, assets, and localization

| ID | Requirement | Notes |
| --- | --- | --- |
| R-G1 | The application MUST implement its own pixel-art visual language and MUST NOT reuse the upstream demo menu, screens, or shell. | Mandatory upstream rule for derivative applications |
| R-G2 | All UI text MUST be available in Simplified Chinese and English, rendered with a pixel font whose glyph coverage is verified on the device. | Chinese text in Montserrat renders as blank boxes |
| R-G3 | Animation MUST be asset-driven and MUST NOT exhaust the LVGL memory pool. Every new resident object and every canvas buffer is a memory decision. | |
| R-G4 | The palette SHOULD be derived from the DeepSeek brand blue and remain legible on the 240 × 320 display at arm's length. | |

### 3.8 R-H Data and security

| ID | Requirement | Notes |
| --- | --- | --- |
| R-H1 | API keys and the bridge token MUST be stored in NVS, MUST NOT appear in source, and MUST NOT be logged. | |
| R-H2 | The bridge MUST authenticate the device with a pairing token and MUST bind to the local network only. | |
| R-H3 | The repository MUST NOT contain credentials, device QR secrets, or unsanitized logs. | Enforced by the repository gate |

## 4. Non-functional requirements

| ID | Requirement |
| --- | --- |
| N-1 | The complete application MUST run on internal SRAM only. Every milestone MUST report measured free heap and largest free block alongside the feature. |
| N-2 | The firmware MUST survive long idle periods without watchdog resets, assertions, or task starvation, and MUST return to the same heap high-water mark after repeated screen enter/exit cycles. |
| N-3 | Hardware interaction MUST stay in `components/bsp`; application state machines, protocols, and layout calculations MUST be testable without ESP-IDF or LVGL and covered by host tests. |
| N-4 | Every maintained document MUST exist as a paired English and Simplified Chinese file, and the [progress log](progress-log.md) MUST be updated as work proceeds. |
| N-5 | Firmware MUST be developed on a `feature/*` branch so fork `main` stays aligned with upstream. |
| N-6 | Firmware MUST tolerate the absence of every optional subsystem: no bridge, no network, no battery gauge, or no audio device must each degrade gracefully with a visible state. |

## 5. Assumptions

These were decided by the recording agent to keep the requirement set
actionable. Each is reversible; confirm or correct them before implementation
freezes.

| ID | Assumption |
| --- | --- |
| A-1 | The board is a stock AI Passport. No wiring, pin, or component change is planned. |
| A-2 | The pet is an original Q-style whale that echoes the DeepSeek brand color, not the official DeepSeek logo. No upstream or third-party artwork is copied. |
| A-3 | The companion bridge runs on the same local network as the device, on the user's computer. |
| A-4 | Provider endpoints are reachable from the user's network. Because some overseas providers are not directly reachable from mainland China, every back end must accept a custom base URL. |
| A-5 | Wake-word detection is out of scope for the first release. The small no-PSRAM wake-word models do support this chip, but a permanently listening microphone costs flash, CPU on a single-core part, and idle power. The microphone is opened only when the user asks for it. Revisit if hands-free operation turns out to matter. |
| A-6 | The first release supports Simplified Chinese and English only, in portrait orientation. |
| A-7 | A single user and a single device profile are assumed. There is no multi-account model. |
| A-8 | The device is charged over USB-C and is expected to be on a desk or in a pocket, so a tethered power profile is a normal operating mode rather than an exception. |

## 6. Open questions

| ID | Question | Why it matters |
| --- | --- | --- |
| Q-1 | Confirm the pet's species, design direction, and on-screen name. | Drives every pixel asset and the whole UI |
| Q-2 | Which text-to-speech back end and voice is the default? | Determines the default configuration, latency budget, and whether the device can decode PCM directly |
| Q-3 | Is out-of-home work-mode delivery required, or is same-LAN only acceptable for the first release? | Same-LAN needs only a local bridge; remote delivery needs a relay service the project does not have yet |
| Q-4 | Should the device be able to talk to cloud providers directly when the bridge is absent? | Direct mode puts API keys and TLS on the device and is the largest memory risk in the project |
| Q-5 | Are streaming partial transcriptions required on screen, or is the final transcript enough? | Streaming is a much larger protocol and memory commitment |
| Q-6 | Is over-the-air update required for the first release, or is USB flashing acceptable? | OTA costs a partition-layout change and a second application slot |
| Q-7 | Should the passive NFC tag be written with a setup URL? | Cheap to do, but it is a product decision and a data-format commitment |
| Q-8 | What is the default power profile, and what standby-delivery delay is acceptable in the battery profile? | Directly sets idle current and the "no missed question" guarantee |
| Q-9 | Should the pet persona be a fixed system prompt, or user-editable from the web console? | Affects prompt storage and the configuration surface |
| Q-10 | Is a one-hour, in-pocket voice-memo or long-recording feature wanted later? | Would change the storage design now |

## 7. Acceptance summary

`Automated` means a host test or build-time check can gate it. `Device` means it
requires a person, the physical board, and an observation. A successful build is
never device acceptance.

| Requirement group | Automated | Device |
| --- | --- | --- |
| R-A pet and home screen | Layout math, state machine, asset decoding | Animation smoothness, legibility, blink and reaction timing |
| R-B voice conversation | Provider request/response parsing, chunking, retry and timeout logic, latency accounting | Real microphone capture, real playback, perceived latency and audio quality |
| R-C work mode | Bridge protocol encode/decode, question rendering model, selection state machine, answer payload | Real DSH session mirroring, chime, spoken result, no missed question |
| R-D standby and power | Idle timer logic, sleep-entry conditions, wake-cause handling | Idle current per profile, wake latency, screen and audio restore |
| R-E provisioning | Credential store, QR payload construction, page form validation | Phone scan, captive-portal behavior on iOS and Android, connection result |
| R-F web console | Configuration schema validation, secret redaction | Real browser on a phone, live provider tests, OTA update |
| R-G pixel UI | Font glyph coverage check, asset size and pool budgeting | On-device rendering of Chinese text, animation memory headroom |
| R-H security | Secret scan, no-credential-in-log check, NVS round trip | Encrypted NVS behavior if enabled |
