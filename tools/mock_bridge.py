#!/usr/bin/env python3
"""A stand-in for the Passport bridge, for testing work mode without DSH.

The real bridge is a companion process on the same network as the device: it knows what the
desktop session is doing and answers the device's questions. This serves the same two endpoints
from a script, so the device's polling, its screen, and its answer can be exercised end to end
before that companion exists.

    GET  /work-status   the flat status document the device parses
    POST /answer        the chosen option, recorded and printed

The document is deliberately flat — task, progress, state, question, and numbered option
fields — which is the contract passport_work.h documents. Options are numbered rather than an
array so a branch name containing a comma or a quote cannot corrupt the list.

Usage:
    python tools/mock_bridge.py [--port 8080] [--scenario waiting|running|done]

Then point the device at it:
    config set bridge_url http://<this-host>:8080
    config save
    work poll
"""

import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SCENARIOS = {
    # A task in progress: the device should show the task and a percentage.
    "running": {
        "task": "indexing repository",
        "progress": 42,
        "state": "running",
    },
    # A question the owner must answer with the buttons.
    "waiting": {
        "task": "deploy",
        "state": "waiting",
        "question": "Which branch?",
        "option0": "main",
        "option1": "develop",
        "option2": "release",
        "selected": 0,
    },
    # A finished task with no question.
    "done": {
        "task": "indexing repository",
        "progress": 100,
        "state": "done",
    },
}

ANSWERS = []
LOCK = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    scenario = "waiting"

    def _send(self, code, body, content_type="application/json"):
        payload = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):  # noqa: N802 - the name is fixed by the base class
        if self.path.rstrip("/") == "/work-status":
            document = SCENARIOS[self.scenario]
            print(f"GET /work-status -> {json.dumps(document)}", flush=True)
            self._send(200, json.dumps(document))
            return
        if self.path.rstrip("/") == "/answers":
            # A convenience for the test, not part of the device's contract.
            with LOCK:
                self._send(200, json.dumps(ANSWERS))
            return
        self._send(404, json.dumps({"error": "not found"}))

    def do_POST(self):  # noqa: N802
        if self.path.rstrip("/") != "/answer":
            self._send(404, json.dumps({"error": "not found"}))
            return
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length).decode("utf-8", errors="replace") if length else ""
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            parsed = {"raw": raw}
        with LOCK:
            ANSWERS.append(parsed)
            count = len(ANSWERS)
        print(f"POST /answer -> {json.dumps(parsed)}  (answer {count})", flush=True)
        self._send(200, json.dumps({"ok": True, "received": count}))

    def log_message(self, *args):
        pass   # the prints above are the log


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--scenario", choices=sorted(SCENARIOS), default="waiting")
    args = parser.parse_args()

    Handler.scenario = args.scenario
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"mock bridge on http://{args.host}:{args.port} serving scenario '{args.scenario}'",
          flush=True)
    print("  GET  /work-status", flush=True)
    print("  POST /answer", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
