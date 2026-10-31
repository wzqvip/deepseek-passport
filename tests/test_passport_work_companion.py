"""Exercise the actual companion HTTP client against a temporary local bridge."""
from pathlib import Path
import sys
import threading
import unittest
import urllib.error

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from passport_bridge import BridgeServer, FakeProvider
from passport_work_companion import Companion


class CompanionTests(unittest.TestCase):
    def test_full_status_question_answer_roundtrip(self):
        server = BridgeServer(("127.0.0.1", 0), FakeProvider(), "test-only", status_file=None)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            client = Companion("http://127.0.0.1:" + str(server.server_port), "test-only")
            self.assertEqual(client.status("Build", progress=30)["progress"], 30)
            question = client.ask("Deploy", "Choose branch", ["main", "dev"])
            qid = question["question_id"]
            self.assertEqual(client.answer(qid)["ready"], 0)
            client.request("/answer", {"question_id": qid, "answer": "dev"})
            self.assertEqual(client.answer(qid)["answer"], "dev")
            client.status("Finished", "done", 100)
            self.assertEqual(client.answer(qid)["ready"], 1)
            wrong = Companion(client.url, "wrong")
            with self.assertRaises(urllib.error.HTTPError) as caught:
                wrong.status("Forbidden")
            self.assertEqual(caught.exception.code, 401)
            caught.exception.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(2)

    def test_client_safe_defaults(self):
        for url, token, insecure in [("http://127.0.0.1:8080", "", False),
                                     ("http://example.invalid", "", True),
                                     ("file:///tmp/a", "x", False),
                                     ("http://user:pass@localhost", "x", False)]:
            with self.assertRaises(ValueError):
                Companion(url, token, insecure)


if __name__ == "__main__":
    unittest.main()
