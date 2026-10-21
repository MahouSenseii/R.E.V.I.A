"""Deterministic contracts for the reconstructed retrieval experiment."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

SPEC = importlib.util.spec_from_file_location(
    "retrieval_benchmark", Path(__file__).with_name("retrievalBenchmark.py"))
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


class RetrievalBenchmarkTests(unittest.TestCase):
    def test_metrics_distinguish_recall_hit_and_mrr_cutoffs(self):
        values = BENCH.metrics(list(range(20)), [1, 11])
        self.assertEqual(values["recall@5"], 0.5)
        self.assertEqual(values["hit@6"], 1.0)
        self.assertEqual(values["mrr@10"], 0.5)
        late = BENCH.metrics(list(range(20)), [11])
        self.assertEqual(late["mrr@10"], 0.0)
        self.assertAlmostEqual(late["mrr@100"], 1 / 12)

    def test_metrics_refuse_duplicate_ranks_and_missing_gold(self):
        with self.assertRaises(ValueError):
            BENCH.metrics([1, 1], [1])
        with self.assertRaises(ValueError):
            BENCH.metrics([1], [])

    def test_revia_prefix_query_matches_owner_tokenization(self):
        self.assertEqual(BENCH.fts_query("What did ALICE, Alice's plans 42 say?"),
                         '"alice"* OR "plans"* OR "42"* OR "say"*')

    def test_archive_terms_remove_scaffolding_numbers_and_bound_six(self):
        self.assertEqual(BENCH.archive_topic_terms(
            "What did I say yesterday about 42 Mara blue library bicycles gardens maps boats?"),
            ["mara", "blue", "library", "bicycles", "gardens", "maps"])
        self.assertEqual(BENCH.archive_topic_terms("What did we say yesterday 42?"), [])

    def test_weighted_rrf_applies_floor_and_candidate_limit(self):
        rank = BENCH.rrf_rank([0, 1, 2], np.array([0.1, 0.9, 0.34, 0.8]), 2)
        self.assertEqual(rank[:2], [1, 3])
        self.assertNotIn(2, rank)

    def test_lift_only_reaches_previous_turn_in_same_session(self):
        scores = np.array([0.0, 4.0, 0.0, 4.0], dtype=np.float32)
        lifted = BENCH.backward_lift(scores, [0, 0, 1, 1])
        self.assertGreater(lifted[0], 0.0)
        self.assertGreater(lifted[2], 0.0)
        self.assertEqual(lifted[1], 4.0)
        self.assertEqual(lifted[3], 4.0)

    def test_fts_indexes_are_isolated_and_prefix_search_is_not_phrase_search(self):
        with BENCH.LexicalIndex(["Alice: The red bicycle broke.", "Bob: A red boat."]) as index:
            archive, _ = index.search("bicycles bicycle", archive=True)
            self.assertEqual(archive, [0])
        with BENCH.LexicalIndex(["No matching text."]) as index:
            archive, _ = index.search("bicycle", archive=True)
            self.assertEqual(archive, [])

    def test_loader_keeps_evidence_categories_and_speaker_caption(self):
        conversation = {
            "sample_id": "fixture",
            "conversation": {
                "session_1_date_time": "10:00 am on 01 January, 2024",
                "session_1": [{"dia_id": "D1:1", "speaker": "A", "text": "Hi",
                               "blip_caption": "a map"}],
            },
            "qa": [
                {"question": "Who?", "answer": "A", "category": 4, "evidence": ["D1:1"]},
                {"question": "No evidence", "answer": "?", "category": 1, "evidence": ["bad"]},
                {"question": "Adversarial", "answer": "?", "category": 5, "evidence": ["D1:1"]},
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.json"
            path.write_text(json.dumps([conversation]), encoding="utf-8")
            hays, queries = BENCH.load_locomo(path)
        self.assertEqual(hays[0]["texts"], ["A: Hi [shares a photo of a map]"])
        self.assertEqual(hays[0]["contents"], ["Hi [shares a photo of a map]"])
        self.assertEqual(len(queries), 1)
        self.assertEqual(queries[0]["category"], "single-hop")

    def test_paired_bootstrap_uses_same_questions(self):
        rows = [{"conversation": "a", "recall@5": 0.0},
                {"conversation": "b", "recall@5": 0.0}]
        better = [{"conversation": "a", "recall@5": 1.0},
                  {"conversation": "b", "recall@5": 1.0}]
        result = BENCH.paired_bootstrap(rows, better, "recall@5", 100, 0)
        self.assertEqual(result["delta"], 1.0)
        self.assertEqual(result["question_ci95"], [1.0, 1.0])
        self.assertEqual(result["conversation_ci95"], [1.0, 1.0])

    def test_paired_bootstrap_rejects_reordered_questions(self):
        left = [{"id": "a", "conversation": "one", "recall@5": 0.0},
                {"id": "b", "conversation": "one", "recall@5": 1.0}]
        with self.assertRaises(ValueError):
            BENCH.paired_bootstrap(left, list(reversed(left)), "recall@5", 100, 0)

    def test_constant_score_channel_has_no_normalized_signal(self):
        self.assertTrue(np.array_equal(BENCH.zscore([1.0, 1.0, 1.0]), np.zeros(3)))

    def test_archive_lexical_does_not_match_speaker_metadata(self):
        with BENCH.LexicalIndex(["I enjoy painting."]) as index:
            ranking, _ = index.search("Mara", archive=True)
            self.assertEqual(ranking, [])
        with BENCH.LexicalIndex(["Mara: I enjoy painting."]) as index:
            ranking, _ = index.search("Mara", archive=True)
            self.assertEqual(ranking, [0])

    def test_ncm_lexical_stems_plural_and_keeps_exact_names(self):
        index = BENCH.NcmLexical(["Mara studies libraries.", "Ivo studies cooking."])
        scores = index.search("Mara library")
        self.assertGreater(scores[0], 0.0)
        self.assertEqual(scores[1], 0.0)


if __name__ == "__main__":
    unittest.main()
