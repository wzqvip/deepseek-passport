<p align="right">
  <a href="voice-backends.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Voice Back-End Options and Decision

This document answers one question: what is the most appropriate way to build a
real-time spoken conversation on this board, and which provider options were
considered. It records the survey, the decision, and the options that were
rejected. The pipeline itself is specified in
[architecture.md](architecture.md#3-voice-conversation-pipeline).

Survey date: 2026-10-13. **Provider names, prices, and endpoint shapes change
often and must be re-checked before implementation.** Every price below is
indicative only.

## 1. Two hardware facts that decide the design

| Fact | Value | Consequence |
| --- | --- | --- |
| Free heap with the Wi-Fi station plus one TLS connection | About 210 KB, from published figures for this chip with a default configuration; documented RAM-reduction settings raise it toward 300 KB | Each additional concurrent secure connection costs tens of KB. The pipeline must reuse one connection per phase, or better, one connection per session |
| Audio front end with echo cancellation | Not available: the chip is absent from the supported-target list, and every echo-cancellation mode additionally requires 64–126 KB of PSRAM | Full-duplex barge-in is impossible. Half-duplex push-to-talk with the microphone muted during playback is the only honest design |

A third fact shapes the transport choice: the wake-word models **are** supported
on this chip, unlike the full front end, so hands-free activation is a future
option rather than an impossibility.

## 2. The decision

**Cascade through the bridge, PCM end to end, no audio codec on the device.**

```text
device: mic -> 16 kHz mono PCM16 -> one connection to the bridge
bridge: speech-to-text -> DeepSeek streaming -> text-to-speech -> PCM16
device: PCM16 -> ring buffer -> I2S -> speaker        (microphone muted)
```

### Which providers to use right now

The bridge is written ([`tools/passport_bridge.py`](../../../tools/passport_bridge.py)) and takes the
provider from its flags, so this is a choice of one key rather than one firmware release:

| Step | First choice | Why | Where |
| --- | --- | --- | --- |
| Speech to text | **SiliconFlow** `FunAudioLLM/SenseVoiceSmall` | OpenAI-compatible `/v1/audio/transcriptions`, good Mandarin, one key, reachable from the mainland | [docs](https://docs.siliconflow.cn/docs/api/audio-transcriptions-post) |
| Text to speech | **SiliconFlow** `FunAudioLLM/CosyVoice2-0.5B` | The **same key and the same OpenAI-compatible shape**, and `response_format: pcm` with `sample_rate: 16000` returns exactly what the device plays, so nothing in the path decodes or resamples | [models](https://siliconflow.cn/models?q=CosyVoice2-0.5B) |
| Language model | **DeepSeek** | Already on the device and verified; nothing to change | [platform](https://platform.deepseek.com/) |

Two things the service's documentation says that are easy to get wrong, both learned the hard way:
the **voice name must carry the model** (`FunAudioLLM/CosyVoice2-0.5B:claire`, and the preset voices are
alex, benjamin, charles, david, anna, bella, claire and diana), and **`sample_rate` is a request
parameter** rather than something to fix up afterwards. Both are handled by the bridge.

Verify the key without spending anything on a device or a person:

```bash
python tools/passport_bridge.py --selftest        # speaks a sentence, transcribes it back, compares
```

That round trip is how the key was checked: it returned the sentence character for character.

One service for both directions is the point: the device knows one `bridge_url`, and one key covers the
two directions that need one, which is the difference between a working device this evening and a
half-configured one.

Second choice, when Chinese quality matters more than effort: **Alibaba Model Studio** —
[qwen3-asr-flash](https://help.aliyun.com/zh/model-studio/qwen3-asr-flash) transcribes on the
OpenAI-compatible path, while [qwen-tts](https://www.alibabacloud.com/help/zh/model-studio/qwen-tts-api)
speaks on its native path; the bridge has a `--provider dashscope` mode for exactly that asymmetry.
**Volcengine Doubao** has the best-regarded Mandarin voices but needs multi-field credentials, which is
why it is not the default.

Prices and model identifiers change often. The defaults in the bridge are a starting point; check the
provider's own page before paying for anything. This document deliberately does not quote per-character
prices it cannot keep current.

Reasons for the cascade itself:

1. It removes the largest RAM risk (an Opus codec) and the second largest (an MP3
   decoder plus resampling). Both ends of the wire are raw PCM.
2. Switching provider becomes a configuration change instead of a firmware
   release. This *is* requirement `R-B4`, solved once. Routing through the bridge
   also keeps credentials off a device that can be lost, but the product owner
   accepted on-device plain-text storage on `Q-4`, so that is a preference rather
   than a rule.
3. Sentence-level text-to-speech pipelining on the bridge is the single biggest
   lever on time to first audio, and it is much easier to implement there.
4. Good cloud turn detection replaces approximating it on one 160 MHz core.

The device transmits about 32 KB/s and receives about 32–48 KB/s depending on the
provider's rate. A 32 KB playback ring is roughly 0.7–1 s of slack.

The cascade is the same in both deployment modes. What changes between them is
only **where the three stages run**: on the computer in bridge mode, or on the
device itself in direct mode, which the product owner approved on `Q-4`. The
device-side audio rules do not change at all, and that is exactly why direct mode
is affordable: PCM in and PCM out means the device needs no codec, no resampler,
and no large staging buffer wherever the stages run.

## 3. Speech-to-text options

| Option | Transport | Partial results | Notes |
| --- | --- | --- | --- |
| Whisper-style HTTP transcription, several hosts | HTTP multipart, whole utterance | No | Cheapest per minute by a wide margin at one host, and the simplest possible client. No partial transcript, so the device shows a level meter rather than live text |
| Cloud streaming transcription with raw PCM input | WebSocket, `linear16` plus an explicit sample rate | Yes | The most device-friendly streaming option: no container, no codec, interim text |
| Gemini Live | Stateful WebSocket, raw PCM 16 kHz in and 24 kHz out | Yes | Also does native turn detection, and it is the only single-connection PCM-in/PCM-out option. But it is an end-to-end speech model, so DeepSeek could not be the brain in that configuration |
| Chinese streaming recognition services | WebSocket with interim results | Yes | Best Chinese accuracy and lowest latency from within mainland China; endpoint shapes differ per vendor |
| Batch transcription at a high-throughput host | HTTP multipart | No | Very cheap, very fast turnaround, no streaming |

Selection changed once the product owner required partial transcription on screen
(`Q-5`): a streaming service with interim results is now the baseline, and batch
HTTP transcription is the cost-saving alternative that simply shows no live text.
A Chinese-first deployment should prefer a domestic streaming service, which is
also the strongest option for Mandarin accuracy.

## 4. Language model

DeepSeek is the default: OpenAI-compatible chat completions, bearer
authentication, server-sent-event streaming terminated by a sentinel chunk, and
price points that make an always-available pocket companion affordable. Any
OpenAI-compatible endpoint can be substituted through a base URL, which is what
makes the requirement configurable in practice rather than in principle.

DeepSeek has **no audio capability at all** — no transcription, no synthesis, no
realtime session. That is a further argument for the three-stage cascade over any
single speech-to-speech model.

One mandatory configuration detail: the conversational path must **disable the
reasoning mode explicitly**. It is enabled by default, and in a spoken loop its
deliberation is dead air between the user's question and the first word of the
answer.

## 5. Text-to-speech options

The decisive column is whether raw PCM is available, and at which rate.

| Option | Raw PCM output | Chinese quality | Notes |
| --- | --- | --- | --- |
| OpenAI speech endpoint | Yes: raw 24 kHz 16-bit little-endian, no header, over chunked transfer | Correct but audibly non-native prosody | The zero-decoder path, and the simplest to integrate |
| ElevenLabs | Yes: selectable 16/22.05/24 kHz, plus a bidirectional streaming socket | Good | Fast first byte is claimed by the vendor and reproduced by third parties, not independently measured here |
| Google Cloud, Azure | Yes, and many sample rates; Azure has a very broad Chinese neural voice set | Good (Azure especially for Chinese) | Azure's fixed raw 16 kHz mono output matches this codec without any device-side work |
| Volcengine (Doubao) | Yes: 24 kHz PCM, and a duplex realtime endpoint | Best in class for Mandarin | Strongest choice for a Chinese-first product |
| Alibaba CosyVoice | Yes, realtime streaming synthesis | Excellent Mandarin | Competitive Chinese pricing |
| MiniMax, Fish Audio | PCM available | Strong Mandarin | Fish Audio also offers a realtime socket |
| On-device synthesis | Not applicable | — | The only chip-supported on-device engine is Chinese-only and still costs about 2.2 MB of flash and 20 KB of RAM for a robotic result. Rejected |

Selection for the first implementation: a provider with raw PCM output, to keep
the no-decoder property. A Chinese-first deployment should route synthesis to a
domestic service, which the bridge makes a configuration change.

**No provider publishes a usable time-to-first-byte figure.** A measurement
harness on the bridge is a deliverable, not a nice-to-have, because requirement
`R-B7` depends on it.

## 6. Codec verdict on this hardware

| Codec | Verdict | Measured or published cost on this board |
| --- | --- | --- |
| Raw PCM16 | **Chosen** | 32 KB/s at 16 kHz mono; no CPU, no flash, no RAM |
| IMA-ADPCM 4-bit | Available if bandwidth ever matters | 8 KB/s at 16 kHz mono, negligible cost; an existing repository experience note records a working software decoder |
| MP3 | Rejected as the default | 30–40 KB of flash, plus a measured 24 KiB compressed-input buffer in one real pipeline |
| Opus | **Rejected** | 60–80 KB of flash plus RAM for decode; the encoder side additionally wants a large per-thread working buffer and states plainly that PSRAM is strongly recommended. On a 210 KB heap with one core and no floating-point unit this is the highest-risk choice available |

Raw PCM is 32 KB/s of LAN traffic, which is nothing on Wi-Fi and is the entire
reason the device needs no codec at all.

## 7. Rejected designs and why

| Design | Why rejected |
| --- | --- |
| Opus encode and decode on the device, as some comparable projects do | Their memory strategy moves the heavy allocations into PSRAM, which this board does not have, and their current firmware requires a newer ESP-IDF major version than this project's pinned toolchain. Their protocol is worth studying; their code is not drop-in |
| Full-duplex speech-to-speech over one realtime socket | Requires echo cancellation the hardware cannot run, and it would replace DeepSeek as the brain |
| Porting an existing voice-assistant firmware wholesale | Toolchain mismatch plus a PSRAM assumption; it also brings a UI that this project must not reuse |
| Compressed audio because "audio must be compressed" | It is a reflex from bandwidth-limited links. On a LAN, PCM is cheaper in every resource that this board is short of |

## 8. Cost sanity check

At roughly ten conversational turns per minute, a cascade using a cheap batch
transcription, DeepSeek, and a mid-priced synthesis service costs well under a
few US cents per minute. A single realtime speech-to-speech session at a premium
vendor costs more than the entire cascade. Cost is not the deciding factor here,
but it does mean the cheap path is also the low-risk path.

## 9. What must be measured before committing

| Measurement | Why |
| --- | --- |
| Time to first audio byte per candidate synthesis provider | No provider publishes it, and the latency requirement depends on it |
| Time to first token from the language model with reasoning disabled | Confirms the dead-air fix |
| Peak and minimum free heap, and largest free block, during a full turn | The real constraint on this board |
| Audio quality on the actual speaker at the actual volume | The speaker and enclosure decide more than the codec does |
