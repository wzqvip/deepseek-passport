<p align="right">
  <a href="bridge-protocol.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Passport Bridge Protocol

How the device and its companion bridge talk to each other. The device is a thin terminal:
the bridge owns the session, knows what the desktop is doing, and asks the device only for the
answers a person has to give.

This document is the contract. The device side is implemented and verified against a stand-in
server ([`tools/mock_bridge.py`](../../../tools/mock_bridge.py)); the bridge itself is the next
piece of work, and the desktop-session integration behind it is not designed here.

## 1. Transport

**Plain HTTP over the local network, not TLS.** The bridge is a companion process on the same
network as the device, and the document carries no credentials. Skipping the handshake keeps the
largest free heap block at 112 KiB instead of 68 KiB, which this board needs for the display and
the audio path.

The consequence is stated rather than hidden: **anything on the network can serve a status
document or read an answer.** Do not run this on a network you do not control. The configuration
already reserves `bridge_token` for the authentication this needs before it leaves a trusted LAN.

`bridge_url` is the base URL, for example `http://192.168.2.183:8080`. The device polls
`<bridge_url>/work-status` every five seconds while work mode is reachable and the station is
associated.

## 2. `GET /work-status`

Returns one JSON object. Every field is optional, and a field the device cannot read becomes a
default rather than a failure, because a bridge that grows a new field must not blank the screen.

| Field | Type | Meaning |
| --- | --- | --- |
| `task` | string | What the session is doing. Shown on the work screen. |
| `progress` | number 0–100 | Fraction complete. **Absent means unknown**, and is shown as `--` rather than as an empty bar; a value outside the range is clamped. |
| `state` | string | `running`, `waiting`, `done`, or `failed`. Anything else reads as idle. |
| `question` | string | A question for the owner. Present means the device can be answered. |
| `option0` … `option3` | string | The choices. **Numbered fields, not an array**: the device's reader is a few hundred bytes rather than a parser, and numbered fields need no delimiter escaping, so an option containing a comma or a quote cannot corrupt the list. |
| `selected` | number 0–3 | Which option to highlight first. Ignored when outside the list. |

Rules the device applies, all of them tested on the host and observed on hardware:

- **The option list ends at the first gap.** `option0` and `option1` followed by nothing is two
  options; a lone `option3` is not one option.
- **Options without a question are not selectable.**
- **A document with neither `task` nor `question` is refused**, and the device keeps showing the
  previous state rather than blanking.
- **A document arriving while the setup screen is open does not take over.** Someone halfway
  through provisioning should not be thrown out of it by a background update. From the home
  screen, the work screen does take over.

Example:

```json
{"task":"deploy","state":"waiting","question":"Which branch?",
 "option0":"main","option1":"develop","option2":"release","selected":0}
```

## 3. `POST /answer`

Sent when the owner confirms a choice with the buttons.

```json
{"answer":"develop"}
```

`answer` is the **text** of the chosen option, not its index: the text is what a person chose, and
it survives the option list changing underneath. The bridge answers `200` with a JSON body; the
device logs the status and records it for the console. A non-200 leaves the answer unsent, and the
device says so rather than pretending it was delivered.

**One question takes one answer.** The device refuses a second confirmation for the same question
until a new document arrives, so a double press cannot answer twice.

## 4. Voice

Push-to-talk uses **two requests**, and that is not an accident of implementation.

### `POST /voice`

The body is the recording, streamed as it is captured: raw little-endian 16-bit mono PCM at 16 kHz,
declared as `audio/L16;rate=16000;channels=1`, sent with `Transfer-Encoding: chunked` because the
length is unknown when the request starts. The device does **not** read this response, and the status
code it would carry is not consulted. It answers `202` with `{"status":"accepted","bytes":N}` for a
human reading a log.

The transfer framing is written by the device's own code. Its HTTP client declares the body as
chunked and then writes the caller's bytes unframed, which a server reads as a chunk size followed by
garbage; see `passport_speech.c`.

**A correction, because this document previously said something false.** It claimed the device cannot
read a response after a chunked upload, and offered that as the reason for the two-request shape. The
claim came from a misread signature: `esp_http_client_fetch_headers` returns a *length* — a positive
content length, 0 for a chunked body, -1 on error — and the code compared it with `ESP_OK`, so every
response with a body looked like a transport failure. The client is fine; the mistake was ours.

The two-request shape is kept, for a reason that survives the correction: a real bridge may spend
seconds transcribing, and a separate `GET` lets it take that time without holding this socket open,
while the answer arrives through the plain request path already proven on this device.

### `GET /voice/transcript`

Returns the transcript of the most recent upload:

```json
{"ready": 1, "text": "which branch should I deploy"}
```

`ready` is **an integer, 1 or 0**, and a missing `ready` counts as ready. The device's reader is an
integer reader — the same one the work protocol uses — so a bridge that answers `"ready": true` is
polled until the attempts run out while its answer sits there unread. That is exactly what happened
the first time, and it is why the field is specified as a number.

`ready: 0` means still working; the device retries every 400 ms, six times, then gives up and reports
that the bridge was still transcribing. An empty `text` with `ready: 1` means the audio held no
speech, and the device says so rather than asking the model about nothing.

The recording's own success is judged **here**, not by the upload's response: a failed upload shows up
as a bridge that never becomes ready. That is deliberate, because the upload's response arrives on a
connection the device has already finished with.

### `GET /speak?text=<percent-encoded>`

Returns the spoken form of `text` as raw little-endian 16-bit mono PCM at 16 kHz — the same shape the
device uploads, so one format covers both directions. The device streams it into the speaker as it
arrives, holding the audio path for the whole utterance, which is what keeps the microphone shut from
the first sample to the last.

- `404` or an empty body is **not a failure**: the answer is on the screen, so the turn simply ends
  without sound.
- The text is percent-encoded UTF-8, and **the device speaks a bounded prefix** of a long answer —
  currently about 190 bytes, cut on a character boundary. A kilobyte of Chinese is half a minute of
  audio, and the encoding triples whatever it carries. The full text is on the screen; the log says
  when it was shortened.
- The device does not read a `Content-Length` and does not care whether the body is chunked.

## 5. What the device does not do

- It does not stream. Polling every five seconds is the whole mechanism; a session that changes
  faster than that is not what this is for.
- It does not speak or chime yet. The document carries no announcement instruction, and the audio
  path for work mode is not wired. This is the next thing after the bridge exists.
- It does not authenticate. See section 1.
