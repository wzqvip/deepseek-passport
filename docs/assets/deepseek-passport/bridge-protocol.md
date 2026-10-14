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

## 4. What the device does not do

- It does not stream. Polling every five seconds is the whole mechanism; a session that changes
  faster than that is not what this is for.
- It does not speak or chime yet. The document carries no announcement instruction, and the audio
  path for work mode is not wired. This is the next thing after the bridge exists.
- It does not authenticate. See section 1.
