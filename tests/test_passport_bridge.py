"""Loopback-only HTTP integration tests; never contact a real provider."""
import http.client
import json
from pathlib import Path
import socket
import sys
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import passport_bridge as bridge


class ControlledProvider:
    def __init__(self):
        self.started = threading.Event()
        self.release = threading.Event()
        self.inputs = []

    def transcribe(self, wav):
        self.inputs.append(wav[44:])
        self.started.set()
        if not self.release.wait(5):
            raise RuntimeError("test timeout")
        return str(len(self.inputs))

    def speak(self, text):
        return b"\x00\x00"


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.provider = ControlledProvider()
        self.server = bridge.BridgeServer(("127.0.0.1", 0), self.provider, "test-only-token", status_file=None)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.provider.release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)

    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection(*self.server.server_address, timeout=3)
        supplied = {"Authorization": "Bearer test-only-token"}
        supplied.update(headers or {})
        connection.request(method, path, body=body, headers=supplied)
        response = connection.getresponse()
        result = response.status, response.read()
        connection.close()
        return result

    def post(self, path, document):
        code, data = self.request("POST", path, json.dumps(document).encode())
        return code, json.loads(data)

    def test_auth_every_endpoint_and_no_side_effects(self):
        for path in ("/", "/voice", "/voice/transcript", "/speak?text=a", "/work-status", "/answer", "/answers"):
            code, _ = self.request("POST" if path in ("/voice", "/answer") else "GET", path,
                                   headers={"Authorization": "Bearer wrong"})
            self.assertEqual(code, 401)
        self.assertIsNone(self.server.latest)

    def test_safe_startup(self):
        for address, insecure in (("127.0.0.1", False), ("0.0.0.0", True)):
            with self.assertRaises(ValueError):
                bridge.BridgeServer((address, 0), self.provider, insecure_local=insecure)
        local = bridge.BridgeServer(("127.0.0.1", 0), self.provider, insecure_local=True)
        local.server_close()

    def test_isolated_turns_and_concurrent_rejection(self):
        code, raw = self.request("POST", "/voice", b"aa")
        self.assertEqual(code, 202)
        first = json.loads(raw)["turn_id"]
        self.assertTrue(self.provider.started.wait(1))
        self.assertEqual(self.request("POST", "/voice", b"bb")[0], 409)
        self.provider.release.set()
        self.assertTrue(self.server.latest.done.wait(1))
        self.assertEqual(json.loads(self.request("GET", "/voice/transcript")[1])["text"], "1")
        self.provider.release.clear()
        self.provider.started.clear()
        code, raw = self.request("POST", "/voice", b"cc")
        self.assertEqual(code, 202)
        second = self.server.latest
        self.assertNotEqual(second.id, first)
        self.assertFalse(second.done.is_set())
        self.assertEqual(json.loads(self.request("GET", "/voice/transcript?turn_id=" + first)[1])["text"], "1")
        self.provider.release.set()
        self.assertTrue(second.done.wait(1))
        self.assertEqual(self.provider.inputs, [b"aa", b"cc"])

    def test_body_boundaries(self):
        for headers, body, expected in [
            ({"Content-Length": "-1"}, b"", 400),
            ({"Content-Length": str(bridge.MAX_AUDIO_BYTES + 1)}, b"", 413),
            ({}, b"x", 400),
            ({"Content-Length": "2", "Transfer-Encoding": "chunked"}, b"aa", 400),
            ({"Transfer-Encoding": "chunked"}, b"xyz\r\n", 400),
            ({"Transfer-Encoding": "chunked"}, b"2\r\naaZZ", 400),
        ]:
            self.assertEqual(self.request("POST", "/voice", body, headers)[0], expected)
        self.provider.release.set()
        self.assertEqual(self.request("POST", "/voice", b"2\r\naa\r\n0\r\n\r\n",
                                      {"Transfer-Encoding": "chunked"})[0], 202)

    def test_truncated_body_releases_turn(self):
        connection = socket.create_connection(self.server.server_address, timeout=2)
        connection.sendall(b"POST /voice HTTP/1.1\r\nHost: localhost\r\n"
                           b"Authorization: Bearer test-only-token\r\nContent-Length: 4\r\n\r\naa")
        connection.shutdown(socket.SHUT_WR)
        response = connection.recv(4096)
        connection.close()
        self.assertIn(b"400", response.split(b"\r\n")[0])
        self.assertTrue(self.server.latest.done.wait(1))
        self.assertEqual(self.server.latest.error, "upload failed")
        self.assertEqual(self.provider.inputs, [])

    def test_provider_failure_finishes_own_event(self):
        def fail(wav):
            raise RuntimeError("secret-provider-detail")
        self.provider.transcribe = fail
        self.assertEqual(self.request("POST", "/voice", b"aa")[0], 202)
        code, body = self.request("GET", "/voice/transcript")
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)["ready"], 1)
        self.assertEqual(json.loads(body)["error"], "transcription failed")
        self.assertNotIn(b"secret", body)

    def test_work_question_correlation_and_idempotence(self):
        self.assertEqual(self.request("GET", "/work-status")[0], 404)
        document = {"task": "Deploy", "state": "waiting", "question": "Branch?", "option0": "main", "option1": "dev"}
        code, first = self.post("/work-status", document)
        self.assertEqual(code, 200)
        self.assertEqual(self.post("/answer", {"answer": "main"})[0], 409)
        answer = {"question_id": first["question_id"], "answer": "main"}
        self.assertEqual(self.post("/answer", answer)[0], 200)
        self.assertEqual(self.post("/answer", answer)[0], 200)
        self.assertEqual(self.post("/answer", dict(answer, answer="dev"))[0], 409)
        result = json.loads(self.request("GET", "/answers?question_id=" + first["question_id"])[1])
        self.assertEqual(result["answer"], "main")
        self.assertEqual(result["ready"], 1)
        _, second = self.post("/work-status", document)
        self.assertNotEqual(first["question_id"], second["question_id"])
        self.assertEqual(self.post("/answer", answer)[0], 409)

    def test_malformed_status_and_legacy(self):
        for value in ([], {"task": "\ud800"}, {"task": "x" * 64}, {"task": "x", "progress": True},
                      {"task": "x", "option0": "x", "option2": "z"}):
            self.assertEqual(self.post("/work-status", value)[0], 400)
        self.server.status = {"task": "test", "question": "Proceed?", "option0": "yes"}
        self.assertEqual(self.post("/answer", {"answer": "yes"})[0], 200)
        self.assertEqual(self.post("/answer", {"answer": "no"})[0], 400)


if __name__ == "__main__":
    unittest.main()
