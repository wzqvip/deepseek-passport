"""Tests for the desktop wrapper that turns output into a preview plus actions.

The point of this tool is that the device never sees the long text. What is worth testing is therefore the
truncation, the action spec, and the document that is published — not a live bridge.
"""
import importlib.util
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("passport_agent", REPO / "tools" / "passport_agent.py")
agent = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(agent)


class Summary(unittest.TestCase):
    def test_last_meaningful_line_is_the_preview(self):
        # A build log's outcome is at the end; the first line tells the owner nothing they can act on.
        output = "collecting...\nbuilding...\n84 passed, 2 skipped\n\n"
        self.assertEqual(agent.summarize(output), "84 passed, 2 skipped")

    def test_one_line_only(self):
        self.assertNotIn("\n", agent.summarize("a\nb\nc"))

    def test_long_line_is_cut_to_fit_the_panel(self):
        line = "x" * 200
        result = agent.summarize(line)
        self.assertLessEqual(len(result), agent.PREVIEW_CHARS)
        self.assertTrue(result.endswith("…"))

    def test_empty_output_says_so(self):
        self.assertEqual(agent.summarize("   \n\n"), "no output")

    def test_unicode_is_kept(self):
        self.assertIn("通过", agent.summarize("第一行\n全部通过"))


class ActionSpec(unittest.TestCase):
    def test_id_and_label_pairs(self):
        self.assertEqual(agent.parse_actions("details:Open diff,continue:Continue"),
                         [("details", "Open diff"), ("continue", "Continue")])

    def test_a_label_may_contain_a_colon(self):
        self.assertEqual(agent.parse_actions("retry:Retry: now"), [("retry", "Retry: now")])

    def test_missing_label_or_id_is_refused(self):
        for spec in ("details", ":Label", "id:"):
            with self.assertRaises(SystemExit):
                agent.parse_actions(spec)

    def test_at_most_four_fit_the_screen(self):
        with self.assertRaises(SystemExit):
            agent.parse_actions("a:A,b:B,c:C,d:D,e:E")

    def test_empty_spec_is_refused(self):
        with self.assertRaises(SystemExit):
            agent.parse_actions("  ")


class Document(unittest.TestCase):
    def test_shape_matches_the_wire_document(self):
        document = agent.publish_document(
            "t" * 100, "p" * 200, "q" * 200, [("details", "Open"), ("retry", "Retry")])
        self.assertLessEqual(len(document["task"]), 63)
        self.assertLessEqual(len(document["preview"]), 95)
        self.assertLessEqual(len(document["question"]), 95)
        self.assertEqual(document["state"], "waiting")
        self.assertEqual(document["option0"], "Open")
        self.assertEqual(document["action0"], "details")
        self.assertEqual(document["action1"], "retry")
        self.assertNotIn("option2", document)

    def test_preview_survives_utf8(self):
        document = agent.publish_document("任务", "第 3 个文件已修改", "继续吗?", [("go", "继续")])
        self.assertEqual(document["preview"], "第 3 个文件已修改")
        document["preview"].encode("utf-8")


if __name__ == "__main__":
    unittest.main()
