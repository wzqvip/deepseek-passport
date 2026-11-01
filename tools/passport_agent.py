#!/usr/bin/env python3
"""Publish a task to the Passport and act on the button the owner presses.

This is the missing piece between "an agent produced something on the computer" and "the owner picked a
thing on the device". It exists because the device cannot show an agent's answer: a 240x320 panel holds
about two lines, and a device that tries to display a long reply is a device nobody uses. So the long text
stays here, and what travels is one line plus a short list of actions.

Two ways to use it.

Publish something you already have:

    python tools/passport_agent.py publish --title "refactor index.ts" \
        --preview "3 files, 84 lines changed, tests pass" \
        --actions "details:Open the diff,continue:Continue,revert:Undo"

Run a command and publish its outcome, then do what the owner chose:

    python tools/passport_agent.py run --title "build" -- python -m pytest -q

The preview is derived automatically from the command's output — the first meaningful line, cut to fit —
because that is exactly the information a person needs to decide, and no model is required to produce it.
The bridge must be running (`tools/start_bridge.py`), and the token comes from `PASSPORT_BRIDGE_TOKEN`.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

PREVIEW_CHARS = 60
DEFAULT_TIMEOUT_S = 120


def bridge_url(base: str, path: str) -> str:
    return base.rstrip("/") + path


def token() -> str:
    value = os.environ.get("PASSPORT_BRIDGE_TOKEN", "")
    if not value:
        raise SystemExit("passport_agent: set PASSPORT_BRIDGE_TOKEN to the bridge token")
    return value


def request(base: str, method: str, path: str, payload: dict | None = None) -> dict:
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(
        bridge_url(base, path), data=data, method=method,
        headers={"Authorization": "Bearer " + token(), "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as response:
        body = response.read()
    return json.loads(body) if body else {}


def summarize(text: str, limit: int = PREVIEW_CHARS) -> str:
    """One line describing an output: the last meaningful line, shortened to fit the panel.

    The last line rather than the first, because a command's outcome is at the end ("3 passed", "error: ...")
    — and a preview that shows the opening line of a build log tells the owner nothing they can act on.
    """
    for raw in reversed(text.splitlines()):
        line = raw.strip()
        if not line:
            continue
        # Keep it one line: a wrapped preview costs the options below it on the panel.
        if len(line) > limit:
            line = line[: limit - 1] + "…"
        return line
    return "no output"


def parse_actions(spec: str) -> list[tuple[str, str]]:
    """`id:Label` pairs, at most four — the number of rows the work screen can show."""
    actions: list[tuple[str, str]] = []
    for item in [s for s in spec.split(",") if s.strip()]:
        if ":" not in item:
            raise SystemExit(f"passport_agent: action {item!r} must be id:Label")
        action_id, label = item.split(":", 1)
        action_id, label = action_id.strip(), label.strip()
        if not action_id or not label:
            raise SystemExit(f"passport_agent: action {item!r} needs both an id and a label")
        actions.append((action_id, label))
    if not actions:
        raise SystemExit("passport_agent: at least one action is required")
    if len(actions) > 4:
        raise SystemExit("passport_agent: at most four actions fit on the screen")
    return actions


def publish_document(title: str, preview: str, question: str, actions: list[tuple[str, str]]) -> dict:
    document = {
        "task": title[:63],
        "state": "waiting",
        "preview": preview[:95],
        "question": question[:95],
    }
    for index, (action_id, label) in enumerate(actions):
        document[f"option{index}"] = label[:47]
        document[f"action{index}"] = action_id[:31]
    return document


def wait_for_answer(base: str, question_id: str, timeout_s: float) -> dict:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        result = request(base, "GET", "/answers?question_id=" + urllib.parse.quote(question_id))
        if result.get("ready"):
            return result
        time.sleep(0.5)
    raise SystemExit("passport_agent: no choice was made before the timeout")


def cmd_publish(args: argparse.Namespace) -> int:
    actions = parse_actions(args.actions)
    document = publish_document(args.title, args.preview or "", args.question, actions)
    request(args.bridge, "POST", "/work-status", document)
    status = request(args.bridge, "GET", "/work-status")
    question_id = status.get("question_id", "")
    print(f"passport_agent: published on the device: {document['task']} / {document['preview']}")
    if args.no_wait:
        return 0
    answer = wait_for_answer(args.bridge, question_id, args.timeout)
    print(f"passport_agent: chose {answer.get('action') or answer.get('answer')!r}")
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    actions = parse_actions(args.actions)
    command = [a for a in args.command if a != "--"]
    if not command:
        raise SystemExit("passport_agent: give a command after --")
    started = time.time()
    completed = subprocess.run(command, capture_output=True, text=True)
    output = completed.stdout + completed.stderr
    elapsed = time.time() - started
    preview = summarize(output)
    title = args.title or " ".join(command)[:63]
    document = publish_document(title, preview, args.question, actions)
    request(args.bridge, "POST", "/work-status", document)
    status = request(args.bridge, "GET", "/work-status")
    question_id = status.get("question_id", "")
    print(f"passport_agent: {title!r} exited {completed.returncode} in {elapsed:.1f}s")
    print(f"passport_agent: preview on the device: {preview}")
    if args.no_wait:
        return 0
    answer = wait_for_answer(args.bridge, question_id, args.timeout)
    chosen = answer.get("action") or answer.get("answer")
    print(f"passport_agent: chose {chosen!r}")
    if args.on_action:
        script = args.on_action.replace("{action}", str(chosen)).replace("{output}", output)
        return subprocess.call(script, shell=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bridge", default=os.environ.get("PASSPORT_BRIDGE_URL", "http://127.0.0.1:8080"))
    sub = parser.add_subparsers(dest="mode", required=True)

    publish = sub.add_parser("publish", help="publish a task you already have")
    publish.add_argument("--title", required=True)
    publish.add_argument("--preview", default="", help=f"one line, cut to {PREVIEW_CHARS} characters")
    publish.add_argument("--question", default="What next?")
    publish.add_argument("--actions", required=True, help="id:Label,id:Label (at most four)")
    publish.add_argument("--no-wait", action="store_true")
    publish.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_S)
    publish.set_defaults(func=cmd_publish)

    run = sub.add_parser("run", help="run a command, publish its outcome, then act on the choice")
    run.add_argument("--title", default="")
    run.add_argument("--question", default="What next?")
    run.add_argument("--actions", required=True, help="id:Label,id:Label (at most four)")
    run.add_argument("--on-action", default="", help="shell command; {action} and {output} are substituted")
    run.add_argument("--no-wait", action="store_true")
    run.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_S)
    run.add_argument("command", nargs=argparse.REMAINDER)
    run.set_defaults(func=cmd_run)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
