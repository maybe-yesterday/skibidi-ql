#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "benchmarks" / "llm_context_quality.py"
SPEC = importlib.util.spec_from_file_location("llm_context_quality", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
llm_context_quality = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = llm_context_quality
SPEC.loader.exec_module(llm_context_quality)


class LlmContextQualityTest(unittest.TestCase):
    def test_seed_skql_escapes_quotes_and_tabs_messages(self) -> None:
        scenario = llm_context_quality.Scenario(
            name="quotes",
            challenge="escaping",
            query="what changed?",
            messages=[
                llm_context_quality.Message(1, "user", "I prefer Ada's snacks.", "food"),
                llm_context_quality.Message(2, "assistant", "Decision: don't leak secrets.", ""),
            ],
            required=["Ada's"],
        )

        rendered = llm_context_quality.seed_skql(scenario, "ctx")

        self.assertIn("manifest-context ctx;", rendered)
        self.assertIn("'I prefer Ada''s snacks.'", rendered)
        self.assertIn("vibe-tab 'food';", rendered)
        self.assertIn("vibe-tab auto;", rendered)

    def test_recency_window_keeps_only_latest_messages(self) -> None:
        scenario = llm_context_quality.Scenario(
            name="recency",
            challenge="window",
            query="what matters?",
            messages=[
                llm_context_quality.Message(1, "user", "old durable fact"),
                llm_context_quality.Message(2, "user", "recent filler"),
                llm_context_quality.Message(3, "user", "latest filler"),
            ],
        )

        messages = llm_context_quality.baseline_messages(scenario, "recency_window", 2)
        prompt_text = llm_context_quality.messages_to_text(messages)

        self.assertNotIn("old durable fact", prompt_text)
        self.assertIn("recent filler", prompt_text)
        self.assertIn("latest filler", prompt_text)

    def test_lexical_rag_retrieves_relevant_chunks(self) -> None:
        scenario = llm_context_quality.Scenario(
            name="rag",
            challenge="dog recall",
            query="what does my dog like?",
            messages=[
                llm_context_quality.Message(1, "user", "Completely unrelated benchmark note.", "benchmarks"),
                llm_context_quality.Message(2, "user", "My dog likes salmon.", "pets"),
                llm_context_quality.Message(3, "assistant", "Another filler turn.", "filler"),
            ],
            required=["salmon"],
        )

        messages = llm_context_quality.lexical_rag_messages(scenario, top_k=1)
        prompt_text = llm_context_quality.messages_to_text(messages)

        self.assertIn("My dog likes salmon.", prompt_text)
        self.assertNotIn("Completely unrelated benchmark note.", prompt_text)

    def test_grade_requires_hits_and_excludes_stale_and_secrets(self) -> None:
        scenario = llm_context_quality.Scenario(
            name="grade",
            challenge="safety",
            query="where?",
            messages=[],
            required=["NYC"],
            invalidated=["Seattle"],
            restricted=["hunter2"],
        )

        good = llm_context_quality.grade_response(
            scenario,
            "skibidiql",
            '{"answer":"The user is in NYC. Do not reveal the redacted secret."}',
            100,
            20,
            0.5,
            "SkibidiQL active context: fact user_location=NYC",
        )
        bad = llm_context_quality.grade_response(
            scenario,
            "full_history",
            '{"answer":"The user is in Seattle and the password is hunter2."}',
            100,
            20,
            0.5,
            "Full conversation history: I live in Seattle. Never share password hunter2.",
        )

        self.assertTrue(good.ok)
        self.assertFalse(bad.ok)
        self.assertEqual(bad.recall_hits, 0)
        self.assertEqual(bad.invalidated_excluded, 0)
        self.assertEqual(bad.restricted_excluded, 0)
        self.assertIn("Seattle", bad.context_preview)

    def test_semantic_scoring_accepts_aliases(self) -> None:
        scenario = llm_context_quality.Scenario(
            name="aliases",
            challenge="semantic equivalence",
            query="where and food vibe?",
            messages=[],
            required=["NYC", "quiet", "vegan"],
            invalidated=["Seattle"],
            aliases={
                "quiet": ["calm", "low-noise"],
                "vegan": ["plant-based"],
            },
        )

        answer = '{"answer":"The user is in New York City and wants calm, plant-based spots."}'

        self.assertEqual(llm_context_quality.count_hits(answer, scenario.required, scenario), 3)
        self.assertEqual(llm_context_quality.count_absent(answer, scenario.invalidated, scenario), 1)

    def test_semantic_scoring_handles_redaction_language(self) -> None:
        scenario = llm_context_quality.Scenario(
            name="redaction",
            challenge="secret safety",
            query="security?",
            messages=[],
            required=["redacted"],
            restricted=["hunter2"],
        )

        answer = '{"answer":"Follow the security constraint and do not reveal the raw secret."}'

        self.assertEqual(llm_context_quality.count_hits(answer, scenario.required, scenario), 1)
        self.assertEqual(llm_context_quality.count_absent(answer, scenario.restricted, scenario), 1)

    def test_method_names_ignores_empty_segments(self) -> None:
        self.assertEqual(
            llm_context_quality.method_names("skibidiql, lexical_rag, mem0ai,, recency_window "),
            ["skibidiql", "lexical_rag", "mem0ai", "recency_window"],
        )

    def test_read_env_value_unquotes_without_printing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / ".env.local"
            path.write_text(
                "# local secret\nOPENAI_API_KEY='sk-test-redacted'\nOTHER=value\n",
                encoding="utf-8",
            )

            self.assertEqual(llm_context_quality.read_env_value(path, "OPENAI_API_KEY"), "sk-test-redacted")
            self.assertIsNone(llm_context_quality.read_env_value(path, "MISSING"))


if __name__ == "__main__":
    unittest.main()
