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
    python tools/passport_bridge.py --fake --insecure-local

Normal serving requires PASSPORT_BRIDGE_TOKEN (the device's bridge_token); all
routes use Authorization: Bearer <token>. Default bind is 127.0.0.1. For a device
on a trusted LAN explicitly choose --host 0.0.0.0 and configure a token. HTTP does
not encrypt it; use TLS termination for untrusted networks. --insecure-local is
only for explicit loopback tests, and --fake alone does not disable authentication.

Protocol v2 adds turn_id to accepted voice uploads and transcript responses.
Poll /voice/transcript?turn_id=ID for exact correlation (last 16 turns retained).
Only one upload/transcription runs at a time; overlapping uploads receive 409.
Unidentified legacy polls snapshot the newest turn, so clients with multiple
independent users must use turn_id. Uploads are capped at 60 seconds of PCM.

Desktop integrations publish flat JSON via POST /work-status. Questions receive
a new question_id; the device must echo it alongside answer to POST /answer.
GET /answers?question_id=ID returns ready=0 or ready=1 with answer. Results are
in memory (last 64), not a durable queue. Legacy file status without question_id
still accepts label-only answers, without generation safety. Use the companion
CLI for explicit task reporting; this is not native DSH session mirroring.

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
import hmac
import ipaddress
import socket
from collections import OrderedDict
from dataclasses import dataclass, field
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

MAX_AUDIO_BYTES = SAMPLE_RATE * 2 * 60
MAX_JSON_BYTES = 4096
MAX_TEXT_BYTES = 4096
REQUEST_TIMEOUT_S = 15


@dataclass
class Turn:
    id: str = field(default_factory=lambda: uuid.uuid4().hex)
    done: threading.Event = field(default_factory=threading.Event)
    text: str = ""
    error: str = ""


class RequestError(Exception):
    def __init__(self, code, message):
        self.code, self.message = code, message


def validate_status(document):
    """Keep the flat wire document within the firmware's UTF-8 buffer limits."""
    if not isinstance(document, dict):
        raise RequestError(400, "status must be an object")
    allowed = {"task", "state", "progress", "question", "question_id", "selected"}
    allowed.update("option" + str(i) for i in range(4))
    if set(document) - allowed:
        raise RequestError(400, "unknown status fields")
    for key, limit in [("task", 63), ("question", 95), ("question_id", 63)] + [
            ("option" + str(i), 47) for i in range(4)]:
        value = document.get(key, "")
        try:
            valid = isinstance(value, str) and len(value.encode("utf-8")) <= limit and "\0" not in value
        except UnicodeError:
            valid = False
        if not valid:
            raise RequestError(400, "invalid " + key)
    if not document.get("task") and not document.get("question"):
        raise RequestError(400, "task or question required")
    if document.get("state", "idle") not in ("idle", "running", "waiting", "done", "failed"):
        raise RequestError(400, "invalid state")
    for key, low, high in (("progress", -1, 100), ("selected", 0, 3)):
        if key in document and (type(document[key]) is not int or not low <= document[key] <= high):
            raise RequestError(400, "invalid " + key)
    options = [document.get("option" + str(i), "") for i in range(4)]
    if document.get("question") and (not options[0] or len(set(filter(None, options))) != len(list(filter(None, options)))):
        raise RequestError(400, "question requires distinct options")
    if any(options[i] and not options[i - 1] for i in range(1, 4)):
        raise RequestError(400, "options must be contiguous")
    return dict(document)


class BridgeServer(ThreadingHTTPServer):
    """Per-server state; only one audio provider operation per direction at a time."""
    daemon_threads = True

    def __init__(self, address, provider, token="", insecure_local=False,
                 work_status=None, status_file=STATUS_FILE, slow_transcription_s=0):
        if not token:
            try:
                local = ipaddress.ip_address(address[0]).is_loopback
            except ValueError:
                local = False
            if not insecure_local or not local:
                raise ValueError("bridge token required (insecure mode permits loopback IP only)")
        self.request_slots = threading.BoundedSemaphore(32)
        self.provider, self.token = provider, token
        self.lock = threading.Lock()
        self.voice_gate = threading.Lock()
        self.speak_gate = threading.Lock()
        self.turns = OrderedDict()
        self.latest = None
        self.status = validate_status(work_status) if work_status is not None else None
        self.status_file = status_file
        self.answers = OrderedDict()
        self.slow_transcription_s = slow_transcription_s
        super().__init__(address, Handler)

    def process_request(self, request, client_address):
        if not self.request_slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.request_slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.request_slots.release()

    def get_status(self):
        if self.status is not None:
            return dict(self.status)
        if self.status_file:
            try:
                with open(self.status_file, encoding="utf-8") as handle:
                    raw = handle.read(MAX_JSON_BYTES + 1)
                if len(raw.encode("utf-8")) > MAX_JSON_BYTES:
                    raise RequestError(413, "status file too large")
                return validate_status(json.loads(raw))
            except (OSError, ValueError):
                pass
        raise RequestError(404, "no work to report")

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
    def setup(self):
        super().setup()
        self.connection.settimeout(REQUEST_TIMEOUT_S)

    def _send(self, code, body, content_type="application/json"):
        self.close_connection = True
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, payload, code=200):
        self._send(code, json.dumps(payload, ensure_ascii=False).encode("utf-8"))

    def _authorize(self):
        values = self.headers.get_all("Authorization", [])
        expected = "Bearer " + self.server.token
        if self.server.token and (len(values) != 1 or not hmac.compare_digest(
                values[0].encode(), expected.encode())):
            raise RequestError(401, "bearer token required")
        if len(self.path) > 16384:
            raise RequestError(414, "URL too long")

    def _exact(self, size):
        data = bytearray()
        while len(data) < size:
            remaining = self.body_deadline - time.monotonic()
            if remaining <= 0:
                raise RequestError(408, "body deadline exceeded")
            self.connection.settimeout(remaining)
            block = self.rfile.read1(size - len(data))
            if not block:
                raise RequestError(400, "truncated body")
            data.extend(block)
        return bytes(data)

    def _body(self, limit, chunked=False):
        self.body_deadline = time.monotonic() + (65 if chunked else REQUEST_TIMEOUT_S)
        lengths = self.headers.get_all("Content-Length", [])
        encodings = self.headers.get_all("Transfer-Encoding", [])
        if len(lengths) > 1 or len(encodings) > 1 or (lengths and encodings):
            raise RequestError(400, "ambiguous framing")
        if encodings:
            if not chunked or encodings[0].lower() != "chunked":
                raise RequestError(400, "unsupported transfer encoding")
            body = bytearray()
            chunks = 0
            while True:
                line = bytearray()
                while len(line) < 129:
                    line.extend(self._exact(1))
                    if line.endswith(b"\n"):
                        break
                if not re.fullmatch(rb"[0-9a-fA-F]{1,8}\r\n", line):
                    raise RequestError(400, "invalid chunk size")
                size = int(line, 16)
                chunks += 1
                if size + len(body) > limit or chunks > 16384:
                    raise RequestError(413, "body too large")
                if size == 0:
                    if self._exact(2) != b"\r\n":
                        raise RequestError(400, "trailers unsupported")
                    return bytes(body)
                body.extend(self._exact(size))
                if self._exact(2) != b"\r\n":
                    raise RequestError(400, "invalid chunk delimiter")
        if not lengths:
            raise RequestError(411, "Content-Length required")
        if not re.fullmatch(r"[0-9]{1,10}", lengths[0]):
            raise RequestError(400, "invalid Content-Length")
        size = int(lengths[0])
        if size > limit:
            raise RequestError(413, "body too large")
        return self._exact(size)

    def _document(self):
        try:
            value = json.loads(self._body(MAX_JSON_BYTES).decode("utf-8"))
            if not isinstance(value, dict):
                raise ValueError()
            return value
        except (ValueError, UnicodeError):
            raise RequestError(400, "invalid JSON object") from None

    def do_POST(self):  # noqa: N802
        self._dispatch(True)

    def do_GET(self):  # noqa: N802
        self._dispatch(False)

    def _dispatch(self, post):
        try:
            self._authorize()
            parsed = urllib.parse.urlsplit(self.path)
            path = parsed.path.rstrip("/")
            query = urllib.parse.parse_qs(parsed.query)
            if post:
                self._post(path)
            else:
                self._get(path, query)
        except RequestError as error:
            self._json({"error": error.message}, error.code)
        except (TimeoutError, socket.timeout):
            self._json({"error": "request timeout"}, 408)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _post(self, path):
        if path == "/voice":
            if not self.server.voice_gate.acquire(blocking=False):
                raise RequestError(409, "voice turn busy")
            turn = Turn()
            # Publish a new, unsignalled event BEFORE reading or acknowledging upload.
            with self.server.lock:
                self.server.turns[turn.id] = turn
                self.server.latest = turn
                while len(self.server.turns) > 16:
                    self.server.turns.popitem(last=False)
            try:
                pcm = self._body(MAX_AUDIO_BYTES, chunked=True)
                if not pcm or len(pcm) % 2:
                    raise RequestError(400, "expected nonempty 16-bit PCM")
                worker = threading.Thread(target=self._transcribe, args=(turn, pcm), daemon=True)
                worker.start()
            except BaseException:
                turn.error = "upload failed"
                turn.done.set()
                self.server.voice_gate.release()
                raise
            self._json({"status": "accepted", "bytes": len(pcm), "turn_id": turn.id}, 202)
            return
        if path == "/work-status":
            document = validate_status(self._document())
            # Every publication is a new question generation, even for identical labels.
            document.pop("question_id", None)
            if document.get("question"):
                document["question_id"] = uuid.uuid4().hex
            with self.server.lock:
                self.server.status = document
            self._json(document)
            return
        if path == "/answer":
            answer = self._document()
            if set(answer) - {"answer", "question_id"}:
                raise RequestError(400, "unknown answer fields")
            with self.server.lock:
                status = self.server.get_status()
                qid = status.get("question_id", "")
                if not status.get("question") or answer.get("question_id", "") != qid:
                    raise RequestError(409, "question changed")
                label = answer.get("answer")
                options = [status.get("option" + str(i)) for i in range(4)]
                if not isinstance(label, str) or not label or label not in options:
                    raise RequestError(400, "answer is not an offered option")
                key = qid or "legacy"
                previous = self.server.answers.get(key)
                result = {"question_id": qid, "answer": label}
                if previous and previous != result:
                    raise RequestError(409, "question already answered")
                self.server.answers[key] = result
                while len(self.server.answers) > 64:
                    self.server.answers.popitem(last=False)
            self._json({"ok": True})
            return
        raise RequestError(404, "not found")

    def _transcribe(self, turn, pcm):
        try:
            if self.server.slow_transcription_s:
                time.sleep(self.server.slow_transcription_s)
            turn.text = self.server.provider.transcribe(pcm_to_wav(pcm))
        except Exception:
            turn.error = "transcription failed"
        finally:
            # Each worker owns its PCM and event; it never clears another turn.
            turn.done.set()
            self.server.voice_gate.release()

    def _get(self, path, query):
        if path == "/voice/transcript":
            with self.server.lock:
                ids = query.get("turn_id")
                turn = self.server.turns.get(ids[0]) if ids else self.server.latest
            if turn is None:
                raise RequestError(404, "unknown voice turn")
            ready = turn.done.wait(WAIT_FOR_TRANSCRIPT_S)
            result = {"ready": int(ready), "text": turn.text if ready else "", "turn_id": turn.id}
            if ready and turn.error:
                result["error"] = turn.error
            self._json(result)
            return
        if path == "/speak":
            text = (query.get("text") or [""])[0]
            if not text or len(text.encode("utf-8")) > MAX_TEXT_BYTES:
                raise RequestError(400, "invalid speech text")
            if not self.server.speak_gate.acquire(blocking=False):
                raise RequestError(409, "speech busy")
            try:
                pcm = self.server.provider.speak(text)
            except Exception:
                raise RequestError(503, "synthesis failed") from None
            finally:
                self.server.speak_gate.release()
            self._send(200, pcm, "audio/L16;rate=16000;channels=1")
            return
        if path == "/work-status":
            with self.server.lock:
                status = self.server.get_status()
            self._json(status)
            return
        if path == "/answers":
            qid = (query.get("question_id") or [""])[0]
            if not qid or len(qid) > 63:
                raise RequestError(400, "question_id required")
            with self.server.lock:
                answer = self.server.answers.get(qid)
                status = self.server.get_status()
                if answer:
                    result = dict(answer, ready=1)
                elif status.get("question_id") == qid:
                    result = {"ready": 0, "question_id": qid}
                else:
                    raise RequestError(404, "unknown or expired question")
            self._json(result)
            return
        if not path:
            self._json({"service": "passport bridge", "protocol": 2})
            return
        raise RequestError(404, "not found")

    def log_message(self, *args):
        pass  # Do not log bearer tokens, transcript text, questions, or answers.


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
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--bridge-token", default=os.environ.get("PASSPORT_BRIDGE_TOKEN", ""),
                        help="device bridge_token; prefer PASSPORT_BRIDGE_TOKEN environment variable")
    parser.add_argument("--insecure-local", action="store_true",
                        help="explicitly allow no authentication on a loopback IP for local tests")
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

    provider = build_provider(args)
    if args.selftest:
        selftest(provider)
        return

    try:
        document = json.loads(args.work_status) if args.work_status else None
        server = BridgeServer((args.host, args.port), provider, args.bridge_token,
                              args.insecure_local, work_status=document,
                              slow_transcription_s=args.stt_delay)
    except (ValueError, RequestError) as error:
        parser.error(str(error))
    log(f"passport bridge on http://{args.host}:{args.port} using '{provider.name}'")
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
