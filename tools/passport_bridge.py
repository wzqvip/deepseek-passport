#!/usr/bin/env python3
"""The Passport bridge: the computer half of the device's voice path.

The device is a thin terminal. It streams push-to-talk audio to this, asks for the transcript, sends the
transcript to DeepSeek itself, and asks for the answer's audio back. This implements the contract in
docs/assets/deepseek-passport/bridge-protocol.md:

    POST /voice              the recording, raw 16 kHz mono PCM, streamed while it is captured
    GET  /voice/transcript   {"ready": 1, "text": "..."}  (the device polls while you transcribe)
    GET  /speak?text=...     raw 16 kHz mono PCM, which the device plays as it arrives
    GET  /work-status        a status document for the work screen
    POST /answer             the option the owner chose with the buttons

Run it with one of three providers:

    # SiliconeFlow: one key for both directions, OpenAI-compatible (the recommended default)
    python tools/passport_bridge.py --api-key sk-... 

    # Alibaba DashScope: qwen3-asr-flash transcribes on the compatible path, qwen-tts speaks on its
    # native path, which is why it needs its own code path here
    python tools/passport_bridge.py --provider dashscope --api-key sk-...

    # No key, no network: a canned transcript and a tone. This is how the device side is verified
    # without spending anything, and how you can check your device is set up correctly.
    python tools/passport_bridge.py --fake

Then, on the device (its serial console):

    config set bridge_url http://<this computer>:8080
    config save

Prices and model names change; check the provider's own pages before relying on the defaults:
  https://docs.siliconflow.cn/docs/api/audio-transcriptions-post
  https://siliconflow.cn/models?q=CosyVoice2-0.5B
  https://help.aliyun.com/zh/model-studio/qwen3-asr-flash
  https://www.alibabacloud.com/help/zh/model-studio/qwen-tts-api
"""

import argparse
import json
import math
import os
import re
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---------------------------------------------------------------------------
# What the device and the providers speak
# ---------------------------------------------------------------------------

SAMPLE_RATE = 16000          # what the device captures and plays
TTS_SAMPLE_RATE = 24000      # what OpenAI-compatible speech synthesis returns as raw PCM
STATUS_FILE = "work-status.json"
ANSWERS_FILE = "answers.jsonl"

STATE = {
    "audio": bytearray(),    # the most recent upload
    "transcript": "",
    "ready": False,
    # Set when the current recording has been transcribed. /voice/transcript waits on it, so the device
    # gets its answer as soon as there is one instead of asking repeatedly.
    "done": threading.Event(),
    "status": {"task": "waiting for a task", "state": "idle", "progress": -1},
}
LOCK = threading.Lock()

# How long /voice/transcript may hold a request waiting for the transcription.
#
# Deliberately shorter than the device's per-request timeout (15 s), so a provider that is merely slow is
# answered inside the request the device already has open instead of the device timing out on a request
# that was about to succeed — which is exactly what happened when the device's timeout was eight seconds
# and this hold was longer.
WAIT_FOR_TRANSCRIPT_S = 12.0


def log(message):
    # Explicitly UTF-8, whatever the console's own code page is. The transcripts and the answers are
    # Chinese, and a log that turns them into replacement characters is a log that cannot be read back —
    # which is exactly what happened: a redirect through a GBK console made correct UTF-8 on the wire
    # look like a device bug for a while.
    try:
        print(message, flush=True)
    except UnicodeEncodeError:
        print(message.encode("ascii", "backslashreplace").decode("ascii"), flush=True)


def pcm_to_wav(pcm: bytes, rate: int = SAMPLE_RATE) -> bytes:
    """Wrap raw mono 16-bit PCM in a WAV container.

    Transcription endpoints want a container, because a container is what tells them the sample rate.
    The device cannot send one: a WAV needs its length in the first 44 bytes and the device is still
    recording when the upload starts. Wrapping it here is a large part of why this bridge exists.
    """
    header = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt "
    header += struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    header += b"data" + struct.pack("<I", len(pcm))
    return header + pcm


def strip_wav(data: bytes) -> bytes:
    """Pull the audio out of a WAV container, for providers that will not return raw PCM."""
    marker = data.find(b"data")
    if data[:4] != b"RIFF" or marker < 0:
        return data   # not a container; assume it is already audio
    return data[marker + 8:]


def wav_rate(data: bytes) -> int:
    """The sample rate out of a WAV header, or 0 when this is not a container."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return 0
    return struct.unpack_from("<I", data, 24)[0]


def resample(pcm: bytes, from_rate: int, to_rate: int) -> bytes:
    """Linear resampling between two rates, for the one direction it is needed in.

    Speech synthesis returns 24 kHz and this device plays 16 kHz. The arithmetic is a decimation with
    interpolation: no dependency on a decoder or a resampler library, and nothing here is doing anything
    clever with the signal.
    """
    if from_rate == to_rate or not pcm:
        return pcm
    count = len(pcm) // 2
    samples = struct.unpack("<%dh" % count, pcm[: count * 2])
    out_count = int(count * to_rate / from_rate)
    out = []
    step = from_rate / to_rate
    for i in range(out_count):
        pos = i * step
        left = int(pos)
        right = min(left + 1, count - 1)
        frac = pos - left
        value = samples[left] * (1.0 - frac) + samples[right] * frac
        out.append(max(-32768, min(32767, int(value))))
    return struct.pack("<%dh" % len(out), *out)


def tone(seconds: float = 1.0, hz: float = 660.0, rate: int = SAMPLE_RATE) -> bytes:
    """A tone, for the fake provider: obviously not silence, and obviously not speech."""
    frames = int(rate * seconds)
    return struct.pack(
        "<%dh" % frames, *[int(6000 * math.sin(2 * math.pi * hz * i / rate)) for i in range(frames)]
    )


# ---------------------------------------------------------------------------
# Providers
# ---------------------------------------------------------------------------


class FakeProvider:
    """No key and no network. A canned transcript and a tone."""

    name = "fake"

    def transcribe(self, wav: bytes) -> str:
        return "which branch should I deploy"

    def speak(self, text: str) -> bytes:
        return tone()


class OpenAICompatibleProvider:
    """Any service exposing /v1/audio/transcriptions and /v1/audio/speech.

    SiliconFlow is the default because one key covers both directions. The same shape is what most
    providers offer, so this class is also the fallback for anyone else.
    """

    name = "openai-compatible"

    def __init__(self, base_url, api_key, stt_model, tts_model, tts_voice, tts_gain=0.0):
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.stt_model = stt_model
        self.tts_model = tts_model
        self.tts_voice = tts_voice
        self.tts_gain = tts_gain

    def _post(self, path, data, content_type):
        request = urllib.request.Request(self.base_url + path, data=data)
        request.add_header("Authorization", "Bearer " + self.api_key)
        request.add_header("Content-Type", content_type)
        try:
            with urllib.request.urlopen(request, timeout=90) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            # The provider's own explanation, because a wrong model name is the most likely mistake here
            # and "HTTP 400" on its own does not say which one.
            detail = error.read().decode("utf-8", errors="replace")[:300]
            raise RuntimeError(f"HTTP {error.code} from {path}: {detail}") from None

    def transcribe(self, wav: bytes) -> str:
        boundary = "----passport" + uuid.uuid4().hex
        body = bytearray()
        body += f"--{boundary}\r\n".encode()
        body += b'Content-Disposition: form-data; name="model"\r\n\r\n'
        body += self.stt_model.encode() + b"\r\n"
        body += f"--{boundary}\r\n".encode()
        body += b'Content-Disposition: form-data; name="file"; filename="speech.wav"\r\n'
        body += b"Content-Type: audio/wav\r\n\r\n"
        body += wav + b"\r\n"
        body += f"--{boundary}--\r\n".encode()
        raw = self._post(
            "/audio/transcriptions", bytes(body), f"multipart/form-data; boundary={boundary}"
        )
        parsed = json.loads(raw.decode("utf-8", errors="replace"))
        return (parsed.get("text") or "").strip()

    def speak(self, text: str) -> bytes:
        # Ask for raw PCM at the rate this device plays, so nothing has to be decoded or resampled.
        # Not every service honours that, so a WAV response is the fallback, its header is stripped and
        # its declared rate is read from the header rather than assumed.
        try:
            return self._speak_as(text, "pcm")
        except RuntimeError as error:
            log(f"  raw PCM refused ({error}); asking for WAV instead")
        wav = self._speak_as(text, "wav")
        rate = wav_rate(wav) or TTS_SAMPLE_RATE
        return resample(strip_wav(wav), rate, SAMPLE_RATE)

    def _speak_as(self, text: str, fmt: str) -> bytes:
        payload = {
            "model": self.tts_model,
            "input": text,
            "voice": self.tts_voice,
            "response_format": fmt,
            "sample_rate": SAMPLE_RATE,
        }
        if self.tts_gain:
            # A small speaker at low volume is a real problem for a pocket device; the provider's own
            # gain control is a better answer than scaling samples here.
            payload["gain"] = self.tts_gain
        return self._post("/audio/speech", json.dumps(payload).encode(), "application/json")


class DashScopeProvider:
    """Alibaba's Model Studio: transcription on the OpenAI-compatible path, speech on the native one.

    Its compatible mode exposes chat completions and transcription but not synthesis, so the speech call
    is shaped the way the native API documents it. That asymmetry is the reason this class exists rather
    than being a base URL in the class above.
    """

    name = "dashscope"

    def __init__(self, api_key, stt_model, tts_model, tts_voice):
        self.api_key = api_key
        self.stt_model = stt_model
        self.tts_model = tts_model
        self.tts_voice = tts_voice

    def transcribe(self, wav: bytes) -> str:
        compatible = OpenAICompatibleProvider(
            "https://dashscope.aliyuncs.com/compatible-mode/v1",
            self.api_key,
            self.stt_model,
            self.tts_model,
            self.tts_voice,
        )
        return compatible.transcribe(wav)

    def speak(self, text: str) -> bytes:
        payload = json.dumps(
            {
                "model": self.tts_model,
                "input": {"text": text, "voice": self.tts_voice, "format": "pcm"},
            }
        ).encode()
        request = urllib.request.Request(
            "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation",
            data=payload,
        )
        request.add_header("Authorization", "Bearer " + self.api_key)
        request.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(request, timeout=60) as response:
            parsed = json.loads(response.read().decode("utf-8", errors="replace"))
        # The audio comes back base64 in the response body; the exact path through the JSON differs by
        # model, so it is searched for rather than assumed.
        import base64

        found = re.search(r'"data"\s*:\s*"([A-Za-z0-9+/=]+)"', json.dumps(parsed))
        if not found:
            raise RuntimeError("no audio in the synthesis response: " + json.dumps(parsed)[:200])
        return resample(base64.b64decode(found.group(1)), TTS_SAMPLE_RATE, SAMPLE_RATE)


# ---------------------------------------------------------------------------
# The HTTP surface the device talks to
# ---------------------------------------------------------------------------


class Handler(BaseHTTPRequestHandler):
    provider = None
    work_status = None     # a document served as-is, or None to read the file each time

    def _send(self, code, body: bytes, content_type="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _json(self, payload, code=200):
        self._send(code, json.dumps(payload).encode())

    # --- the device's upload -------------------------------------------------
    def do_POST(self):  # noqa: N802 - fixed by the base class
        path = self.path.rstrip("/")
        if path == "/voice":
            total = 0
            if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
                while True:
                    line = self.rfile.readline()
                    if not line:
                        break
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        size = int(line.split(b";")[0], 16)
                    except ValueError:
                        log(f"  not a chunk size: {line[:32]!r}")
                        break
                    if size == 0:
                        break
                    chunk = self.rfile.read(size)
                    total += len(chunk)
                    with LOCK:
                        STATE["audio"] += chunk
                    self.rfile.readline()
            else:
                stated = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(stated) if stated else b""
                total = len(body)
                with LOCK:
                    STATE["audio"] += body
            seconds = total / 2.0 / SAMPLE_RATE
            log(f"POST /voice -> {total} bytes ({seconds:.1f} s at {SAMPLE_RATE} Hz)")
            self._send(202, json.dumps({"status": "accepted", "bytes": total}).encode())
            threading.Thread(target=self._transcribe, daemon=True).start()
            return

        if path == "/answer":
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length).decode("utf-8", errors="replace") if length else ""
            with open(ANSWERS_FILE, "a", encoding="utf-8") as handle:
                handle.write(raw + "\n")
            log(f"POST /answer -> {raw}")
            self._json({"ok": True})
            return

        self._json({"error": "not found"}, 404)

    def _transcribe(self):
        """Transcribe in the background.

        The device polls /voice/transcript rather than waiting on the upload's response, so transcription
        may take as long as the provider needs.
        """
        with LOCK:
            pcm = bytes(STATE["audio"])
            STATE["ready"] = False
            STATE["done"].clear()
        if self.server.slow_transcription_s:
            # A deliberately slow provider, for testing that the device waits rather than giving up.
            log(f"  (transcription delayed by {self.server.slow_transcription_s:.1f} s for testing)")
            time.sleep(self.server.slow_transcription_s)
        try:
            text = self.provider.transcribe(pcm_to_wav(pcm))
        except Exception as error:   # noqa: BLE001 - reported to the device as no transcript
            log(f"  transcription failed: {error}")
            text = ""
        with LOCK:
            STATE["transcript"] = text
            STATE["ready"] = True
            STATE["audio"].clear()
            STATE["done"].set()      # releases whatever is waiting on /voice/transcript
        log(f"  transcript: {text!r}")

    # --- what the device asks for -------------------------------------------
    def do_GET(self):  # noqa: N802
        path = self.path.split("?")[0].rstrip("/")

        if path == "/voice/transcript":
            # Hold the request until the transcription finishes, rather than answering "not ready" and
            # making the device poll. The device does poll, and patiently, but a transcription is
            # seconds of work at a provider and there is no reason to answer before it is done: the wait
            # costs one thread and removes a whole class of races where the answer arrives between two
            # polls.
            if not STATE["done"].wait(timeout=WAIT_FOR_TRANSCRIPT_S):
                log(f"GET /voice/transcript -> not ready after {WAIT_FOR_TRANSCRIPT_S:.0f} s")
                self._json({"ready": 0, "text": ""})
                return
            with LOCK:
                # `ready` is an integer, not true/false: the device reads it with an integer reader.
                payload = {"ready": 1, "text": STATE["transcript"]}
                log(f"GET /voice/transcript -> {STATE['transcript']!r}")
            self._json(payload)
            return

        if path == "/speak":
            query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            text = (query.get("text") or [""])[0]
            # The raw escapes are logged alongside the decoded text: if a device percent-encodes
            # something other than UTF-8, the decoded text becomes replacement characters and the
            # question "who mangled it" is answered by the two lines together.
            raw = urllib.parse.urlparse(self.path).query
            log(f"GET /speak raw={raw[:160]!r}")
            if not text:
                self._send(404, b"no text", "text/plain")
                return
            log(f"GET /speak -> {text[:60]!r}{'...' if len(text) > 60 else ''}")
            try:
                pcm = self.provider.speak(text)
            except Exception as error:   # noqa: BLE001 - the device shows the answer anyway
                log(f"  synthesis failed: {error}")
                self._send(503, b"synthesis failed", "text/plain")
                return
            self._send(200, pcm, "audio/L16;rate=16000;channels=1")
            return

        if path == "/work-status":
            document = self.work_status
            if document is None:
                try:
                    with open(STATUS_FILE, encoding="utf-8") as handle:
                        document = handle.read()
                except OSError:
                    with LOCK:
                        document = json.dumps(STATE["status"])
            self._send(200, document.encode(), "application/json")
            return

        if path in ("", "/index.html"):
            # A tiny page, because "what is this port?" deserves an answer.
            body = (
                "<!doctype html><meta charset=utf-8><title>Passport bridge</title>"
                "<body style='font-family:system-ui;padding:20px'>"
                f"<h1>Passport bridge</h1><p>provider: <b>{self.provider.name}</b></p>"
                "<ul><li><code>POST /voice</code> — the recording"
                "<li><code>GET /voice/transcript</code> — the transcript"
                "<li><code>GET /speak?text=…</code> — the spoken answer"
                "<li><code>GET /work-status</code> — the work screen's document"
                "<li><code>POST /answer</code> — the chosen option</ul>"
                "<p>Point the device here: <code>config set bridge_url http://&lt;this host&gt;:"
                f"{self.server.server_port}</code></p>"
            ).encode()
            self._send(200, body, "text/html; charset=utf-8")
            return

        self._json({"error": "not found"}, 404)

    def log_message(self, *args):
        pass   # the prints above are the log


def build_provider(args):
    if args.fake:
        return FakeProvider()
    if not args.api_key:
        raise SystemExit(
            "an API key is required unless --fake is used:\n"
            "  --api-key sk-...            (SiliconeFlow, or any OpenAI-compatible service)\n"
            "  --provider dashscope        (Alibaba Model Studio)\n"
            "  --fake                      (no key, canned answers, for checking the device)"
        )
    if args.provider == "dashscope":
        return DashScopeProvider(
            args.api_key,
            args.stt_model or "qwen3-asr-flash",
            args.tts_model or "qwen-tts",
            args.tts_voice or "Cherry",
        )
    return OpenAICompatibleProvider(
        args.base_url,
        args.api_key,
        args.stt_model or "FunAudioLLM/SenseVoiceSmall",
        args.tts_model or "FunAudioLLM/CosyVoice2-0.5B",
        # SiliconFlow wants the model name in front of the voice: `model:voice`. Its preset voices are
        # alex, benjamin, charles, david, anna, bella, claire and diana; claire is the gentle one, which
        # suits a small creature more than a newsreader does.
        args.tts_voice or "FunAudioLLM/CosyVoice2-0.5B:claire",
        args.tts_gain,
    )


def selftest(provider, phrase="你好，这是随身宠物在测试语音通路。"):
    """Round-trip both directions with the configured provider, without a device or a person.

    Speech synthesis is asked for a sentence, and the audio that comes back is fed straight into speech
    recognition as if the device had recorded it. That exercises both credentials, both request shapes
    and the resampling, and it is the only way to test the microphone half without someone talking.
    """
    log(f"selftest: asking the provider to speak {phrase!r}")
    audio = provider.speak(phrase)
    seconds = len(audio) / 2.0 / SAMPLE_RATE
    log(f"  synthesis returned {len(audio)} bytes of 16 kHz PCM ({seconds:.1f} s)")
    if seconds < 0.4:
        raise SystemExit("selftest: too little audio came back to be speech; check the model and voice")
    log("selftest: feeding that audio back as if the device had spoken it")
    heard = provider.transcribe(pcm_to_wav(audio))
    log(f"  recognition heard: {heard!r}")
    log(f"  the sentence was:  {phrase!r}")
    overlap = sum(1 for ch in set(heard) if ch in phrase)
    log(f"  {overlap} of the {len(set(heard))} distinct characters heard appear in the sentence")
    log("selftest: both directions answered. A round trip through two models is not expected to be"
        " character-perfect; what this proves is that the credentials, the request shapes and the"
        " resampling all work.")


def main():
    # The device's transcripts and answers are Chinese; make sure this process can write them.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
        except (AttributeError, ValueError):
            pass

    parser = argparse.ArgumentParser(description="The Passport device's companion bridge.")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--provider", choices=["siliconflow", "dashscope"], default="siliconflow")
    parser.add_argument("--base-url", default="https://api.siliconflow.cn/v1",
                        help="for any OpenAI-compatible service")
    parser.add_argument("--api-key", default=os.environ.get("PASSPORT_BRIDGE_KEY", ""))
    parser.add_argument("--stt-model", default="")
    parser.add_argument("--tts-model", default="")
    parser.add_argument("--tts-voice", default="")
    parser.add_argument("--tts-gain", type=float, default=0.0,
                        help="the provider's own gain in dB, for the small speaker (range -10 to 10)")
    parser.add_argument("--fake", action="store_true",
                        help="no key and no network: a canned transcript and a tone")
    parser.add_argument("--selftest", action="store_true",
                        help="round-trip speech synthesis and recognition once, then exit")
    parser.add_argument("--work-status", default="",
                        help="a fixed document for the work screen, instead of reading " + STATUS_FILE)
    parser.add_argument("--stt-delay", type=float, default=0.0,
                        help="pretend the provider is this slow, to check the device keeps waiting")
    args = parser.parse_args()

    Handler.provider = build_provider(args)
    Handler.work_status = args.work_status or None

    if args.selftest:
        selftest(Handler.provider)
        return

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.slow_transcription_s = args.stt_delay
    log(f"passport bridge on http://{args.host}:{args.port} using '{Handler.provider.name}'")
    log(f"  point the device at it:  config set bridge_url http://<this host>:{args.port}")
    log(f"  work screen document:    {args.work_status or STATUS_FILE + ' (if it exists)'}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
