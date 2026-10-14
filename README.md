<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

<h1 align="center">DeepSeek Passport</h1>

<p align="center">
  <strong>A pocket-sized DeepSeek companion you can talk to, that watches your agent work, and that answers when you press a button.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/status-firmware%20verified-16a34a?style=flat-square" alt="Status: firmware verified on hardware">
  <img src="https://img.shields.io/badge/target-ESP32--C3-2563eb?style=flat-square" alt="Target: ESP32-C3">
  <img src="https://img.shields.io/badge/upstream-FoloToy%2Fai--passport-14b8a6?style=flat-square" alt="Upstream: FoloToy ai-passport">
  <img src="https://img.shields.io/badge/License-MIT-64748b?style=flat-square" alt="MIT License">
</p>

---

**DeepSeek Passport** turns the open **[FoloToy AI Passport](https://github.com/FoloToy/ai-passport)**
wearable into a DeepSeek-branded pocket pet: a pixel-art whale that lives on a
240 × 320 screen, listens through the built-in microphone, speaks through the
built-in speaker, and doubles as a remote control and notification pane for a
DeepSeek Harness (DSH) session running on your computer.

The hardware is not modified. Everything in this repository is firmware, a
companion bridge, and design documentation layered on top of the upstream board
support package.

> [!IMPORTANT]
> The application is **implemented and running on hardware**. The upstream demo menu, screens and
> visual shell are gone; the device boots into this project's own interface. Every milestone has been
> verified on the board over the serial console, and the parts that have *not* been verified are listed
> explicitly rather than left implied — see the [handover](docs/assets/deepseek-passport/handover.md)
> and the [progress log](docs/assets/deepseek-passport/progress-log.md).
>
> One piece is not written yet: the companion bridge, which is a program for a computer rather than
> firmware. Its contract is complete and the device has been verified against a stand-in
> implementation of it.

## What it does

| Capability | User-visible behavior |
| --- | --- |
| **Pocket pet** | An original Q-style pixel whale idles, blinks, breathes, reacts to button input, and shows speech bubbles. It is the home screen, not a menu. |
| **Voice conversation** | Press a button, speak, and the pet answers with on-screen text plus spoken audio. Speech-to-text, the language model, and text-to-speech are three independently configurable back ends. |
| **Work companion** | While a DSH session runs on the computer, the device mirror-shows progress, renders option prompts for `UP` / `DOWN` / `OK`, sends the chosen answer back, and plays a chime or speaks the result when the turn ends. |
| **Setup and web console** | With no saved network the device raises its own access point, shows a QR code, and serves a captive-portal page for Wi-Fi setup. Afterwards it serves a local web console for API keys, providers, voices, brightness, volume, and firmware updates. |
| **Standby** | Idle screen-off, chime-free standby, and wake on any function button; a dedicated hardware power button remains the only true power control. |

## Hardware at a glance

The board is the standard AI Passport; the numbers below are confirmed board
facts, not targets.

| Item | Value |
| --- | --- |
| MCU | ESP32-C3, single core, 8 MB Flash, **no PSRAM** |
| Usable RAM | Internal SRAM only; display, LVGL, audio DMA, Wi-Fi, and TLS all compete for it |
| Display | ST7789P3, 240 × 320 portrait, RGB565, SPI |
| Input | `UP`, `DOWN`, `OK` on one ADC resistor ladder (GPIO0) plus a hardware power button |
| Audio | ES8311 codec, full-duplex I2S, 16 kHz 16-bit mono capture and playback, microphone and speaker |
| Wireless | 2.4 GHz Wi-Fi (station and access point) and Bluetooth 5 LE |
| Battery | 500 mAh cell with a CW2017 fuel gauge; USB Type-C charging |
| NFC | Passive NTAG213 tag with no MCU-facing interface |

"8 MB Flash and no PSRAM" is the single most important design constraint in this
project. It is the reason the architecture keeps audio streaming rather than
buffered, uses PCM rather than compressed audio on the device, and pushes
provider orchestration to a companion bridge. The reasoning is recorded in the
[architecture document](docs/assets/deepseek-passport/architecture.md).

## Design records

Fork-specific product design lives under `docs/assets/`, which upstream keeps
empty on purpose. These are the authoritative documents for this project.

| Document | Contents |
| --- | --- |
| [Requirements](docs/assets/deepseek-passport/requirements.md) | The product owner's request, decomposed into identified requirements, assumptions, acceptance criteria, and open questions |
| [Architecture](docs/assets/deepseek-passport/architecture.md) | Hardware constraints, memory budget, firmware and bridge architecture, voice pipeline, DSH integration, provisioning, pixel UI, and provider configuration |
| [Roadmap](docs/assets/deepseek-passport/roadmap.md) | Milestones `M0`–`M7` with deliverables, entry and exit criteria, and the validation plan |
| [Progress log](docs/assets/deepseek-passport/progress-log.md) | Dated record of decisions, evidence, and what remains open |
| [Handover](docs/assets/deepseek-passport/handover.md) | How to flash and use the device, the bridge it needs, and an explicit list of what is verified and what is not |
| [Bridge protocol](docs/assets/deepseek-passport/bridge-protocol.md) | The contract between the device and its companion bridge |
| [Serial console](docs/assets/deepseek-passport/serial-cli.md) | The console used to verify everything, and the contracts it keeps |
| [Voice back ends](docs/assets/deepseek-passport/voice-backends.md) | The provider survey behind the voice pipeline decision |

## Repository layout and fork policy

This repository is a fork. Only two locations are fork-owned; everything else
tracks upstream `FoloToy/ai-passport` and is updated by the sync workflow.

```text
README.md / README.zh_CN.md        fork-owned project readme (this file)
docs/assets/                       fork-owned design notes and assets
docs/                              upstream documentation, kept in sync
components/bsp/                    upstream board support package
main/                              upstream hardware-test demo, replaced by the application on a feature branch
skills/                            upstream AI-agent skills
sdkconfig.defaults, partitions.csv upstream build baseline
```

Application firmware is developed on a `feature/*` branch; the fork `main` stays
aligned with upstream. See the [fork guide](docs/fork-guide.md).

## Building

The firmware target is ESP-IDF 5.5.3 for ESP32-C3. Prepare the toolchain with the
[environment setup guide](docs/development/engineering/environment-setup.md),
then use the repository validation gate:

```bash
./tools/validate.sh --static    # repository checks and host tests
./tools/validate.sh --firmware  # ESP-IDF build and merged-image verification
./tools/validate.sh             # complete gate
```

A successful build is not hardware validation. Report build, host test, and
device test results separately.

## Related projects

| Project | Role |
| --- | --- |
| [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) | Upstream open wearable AI platform: hardware facts, BSP, and the demo baseline |
| [wzqvip/dsh-app](https://github.com/wzqvip/dsh-app) | DSH plugin package that surfaces agent questions and work progress; the device-friendly work-status and notification endpoints live here |
| [DeepSeek Harness](https://github.com/deepseek-ai) | The agent host whose sessions this device mirrors |

## Hosted projects

| Project | Branch | Status |
| --- | --- | --- |
| DeepSeek Passport firmware | `feature/deepseek-passport` (implemented, verified on hardware) | Active |

## License

MIT, inherited from the upstream project. See [LICENSE](LICENSE). Product,
brand, and hardware facts remain those of the upstream project; see the
[brand and product notes](docs/brand/brand-and-product.md).
