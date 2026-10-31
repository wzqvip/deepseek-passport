<p align="right">
  <a href="bridge-protocol.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Passport Bridge Protocol

Current contract, updated 2026-10-31. Implemented by
[passport_bridge.py](../../../tools/passport_bridge.py) and the device work/speech clients.
The desktop [companion CLI](../../../tools/passport_work_companion.py) publishes real task
status and retrieves answers. It does **not** automatically mirror an existing DeepSeek Harness
Web session. No supported subscription/answer hook for that session has been verified.

## Authentication and deployment

Every endpoint requires `Authorization: Bearer <bridge_token>`. Configure the same secret in device
`bridge_token` and the process environment `PASSPORT_BRIDGE_TOKEN`; do not put it in source or logs.
The bridge defaults to `127.0.0.1`. To serve a physical device, explicitly bind `--host` to a reachable
trusted-LAN address and set device `bridge_url`. Plain HTTP exposes the token and task/audio contents
to network observers: use a trusted LAN or a properly configured HTTPS front end. Never expose the
agent Web server to the LAN just to connect this device.

`--insecure-local` disables authentication only on loopback, for local tests. `--fake` does not bypass
authentication. Provider credentials are separately supplied as `PASSPORT_BRIDGE_KEY`.
No permanent service is installed automatically.

## Work status and answers

`GET /work-status` returns a flat JSON document; `404` means no task has been published, not a
network failure. `POST /work-status` publishes a document from the companion:

```json
{"task":"deploy","state":"waiting","question":"Which branch?",
 "option0":"main","option1":"develop","selected":0}
```

Fields: `task`, `state` (`idle`, `running`, `waiting`, `done`, `failed`), optional `progress` (0–100),
`question`, contiguous `option0` through `option3`, and initial `selected`. Publishing a question
assigns an opaque `question_id`. The device retains that identity, selection and acknowledged state
across duplicate polls. Changed identity/question/options creates a new choice. Settings and setup
pages are not replaced by notifications; an active talk gesture is not interrupted.

Physical confirmation queues a worker request, never performs networking on the input task:

```json
{"question_id":"<opaque-id>","answer":"develop"}
```

`POST /answer` preserves the selected label using JSON escaping. Same question/same answer retries
are idempotent; a conflicting or stale answer returns `409`. On transport failure the device permits
retry for the same current question. Legacy file-backed documents without an ID accept label-only
answers, without the generation protection of published questions.

`GET /answers?question_id=<id>` returns `ready: 0` while waiting and `ready: 1` with `answer` when
available. Results are memory-only (last 64); restarting the bridge loses them. The bridge no longer
writes answers or transcripts to a persistent log by default.

Example companion commands (the token is read from the environment):

```text
python tools/passport_work_companion.py --help
python tools/passport_work_companion.py status --help
python tools/passport_work_companion.py ask --help
python tools/passport_work_companion.py answer --help
```

These interfaces support actual desktop applications that explicitly publish tasks. Automatic DSH
mirroring, durable delivery, free-text voice instructions into that session and remote relay remain
unfinished product requirements; the CLI is not a claim that those features exist.

## Voice uploads

`POST /voice` accepts bounded little-endian PCM16, mono, 16 kHz, including chunked uploads.
The device writes all framing bytes, checks HTTP `202`, parses the complete acceptance response,
and retains its 32-hex `turn_id`. Every upload owns a separate buffer/event/result; overlapping
uploads are rejected with `409` rather than mixing audio. The bridge retains at most 16 turns.
The maximum upload is 60 seconds / 1,920,000 bytes; malformed framing, excessive sizes and deadlines
are rejected. Local silence gating happens **after** bridge audio upload, not before transmission.

`GET /voice/transcript?turn_id=<id>` returns the matching `turn_id`, integer `ready` (0 or 1), and
`text` once ready. The device verifies the ID and polls with a bounded budget while showing a thinking
state. Empty text is not sent to the language model. Legacy no-ID reads are supported by the bridge,
but current firmware requires identified turns; update bridge and firmware together.

## Speech output

`GET /speak?text=<percent-encoded-UTF8>` returns PCM16 little-endian, mono, 16 kHz. Playback and
recording requests are serialized by the speech worker. Output is bounded, odd-byte PCM and transport
errors are failures, and a partially played response does not turn a failed request into success.
Device speech is a bounded prefix of the displayed response; it is not full-response streaming.
Submitting a speech request means **queued**, not audible playback verified.

Device-direct STT/TTS now use separately configured provider fields; see
[voice back ends](voice-backends.md). The bridge is still needed for desktop work integration, and
remains an optional voice transport. Loopback fake-provider integration tests establish protocol
behavior, not live providers, real microphone quality, LAN reachability, or on-device acceptance.
