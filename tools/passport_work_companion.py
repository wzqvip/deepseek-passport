#!/usr/bin/env python3
"""Explicit desktop status/answer adapter; not a DSH session mirror or automation hook.

Set PASSPORT_BRIDGE_TOKEN, then use:
  python tools/passport_work_companion.py status --task Building --state running --progress 25
  python tools/passport_work_companion.py ask --task Deploy --question 'Which branch?' --option main --option dev
  python tools/passport_work_companion.py answer QUESTION_ID

The ask command prints a question_id. Poll answer with that id; ready=0 means
pending. Answers are retained in bridge memory (last 64), not executed as commands.
HTTP is suitable only for a trusted local network; use an authenticated TLS proxy
for untrusted networks. A token authenticates callers but does not encrypt traffic.
"""
import argparse
import json
import os
import urllib.parse
import urllib.request


class Companion:
    def __init__(self, url, token, insecure_local=False):
        parsed = urllib.parse.urlsplit(url)
        if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username or parsed.password:
            raise ValueError("expected http(s) bridge URL without credentials")
        if parsed.query or parsed.fragment or parsed.path not in ("", "/"):
            raise ValueError("bridge URL must be an origin")
        if not token and not (insecure_local and parsed.hostname in ("127.0.0.1", "::1")):
            raise ValueError("PASSPORT_BRIDGE_TOKEN required")
        self.url, self.token = url.rstrip("/"), token

    def request(self, path, document=None):
        data = json.dumps(document, ensure_ascii=False).encode("utf-8") if document is not None else None
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        req = urllib.request.Request(self.url + path, data=data, headers=headers)
        # Never forward the bearer token through redirects to another host.
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, req, fp, code, msg, headers, newurl):
                return None
        with urllib.request.build_opener(NoRedirect).open(req, timeout=15) as response:
            return json.load(response)

    def status(self, task, state="running", progress=-1):
        return self.request("/work-status", {"task": task, "state": state, "progress": progress})

    def ask(self, task, question, options):
        if not 1 <= len(options) <= 4:
            raise ValueError("one to four options required")
        document = {"task": task, "state": "waiting", "question": question}
        document.update(("option" + str(i), value) for i, value in enumerate(options))
        return self.request("/work-status", document)

    def answer(self, question_id):
        return self.request("/answers?" + urllib.parse.urlencode({"question_id": question_id}))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    parser.add_argument("--insecure-local", action="store_true")
    commands = parser.add_subparsers(dest="command", required=True)
    status = commands.add_parser("status")
    status.add_argument("--task", required=True)
    status.add_argument("--state", choices=["idle", "running", "done", "failed"], default="running")
    status.add_argument("--progress", type=int, default=-1)
    ask = commands.add_parser("ask")
    ask.add_argument("--task", required=True)
    ask.add_argument("--question", required=True)
    ask.add_argument("--option", action="append", required=True)
    answer = commands.add_parser("answer")
    answer.add_argument("question_id")
    args = parser.parse_args()
    client = Companion(args.url, os.environ.get("PASSPORT_BRIDGE_TOKEN", ""), args.insecure_local)
    if args.command == "status":
        result = client.status(args.task, args.state, args.progress)
    elif args.command == "ask":
        result = client.ask(args.task, args.question, args.option)
    else:
        result = client.answer(args.question_id)
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
