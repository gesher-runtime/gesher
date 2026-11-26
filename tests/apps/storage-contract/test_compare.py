# SPDX-License-Identifier: Apache-2.0
import json
from pathlib import Path
import tempfile
import unittest

from compare import CASES, TAG, compare, observations


def sample(nonce):
    return [
        {"schema": 1, "phase": phase, "status": "observed", "pid": pid,
         "target_sdk": 36, "nonce": nonce, "checks": dict.fromkeys(CASES, True)}
        for phase, pid in (("seed", 100), ("restart", 101))
    ]


class CompareTest(unittest.TestCase):
    def parse(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.txt"
            path.write_text("noise\n" + "\n".join(
                "I/" + TAG + ": " + json.dumps(row) for row in rows), encoding="utf-8")
            return observations(path, 36)

    def setUp(self):
        self.android = sample("f933ac72-8cd5-4c7a-bf42-4e67c7a0877c")
        self.windows = sample("3ec456c4-5aaf-4f46-8b44-95d5879d9ff5")

    def test_accepts_independent_paired_observations(self):
        self.assertEqual(compare(self.parse(self.android), self.parse(self.windows)), [])

    def test_reports_differences_and_shared_failures(self):
        self.windows[1]["checks"]["de_preferences"] = False
        self.android[0]["checks"]["ce_database"] = False
        self.windows[0]["checks"]["ce_database"] = False
        self.assertEqual(compare(self.parse(self.android), self.parse(self.windows)), [
            "seed/ce_database: Android=False, Windows=False",
            "restart/de_preferences: Android=True, Windows=False",
        ])

    def test_rejects_replayed_or_missing_observation(self):
        with self.assertRaisesRegex(ValueError, "independent seed runs"):
            compare(self.parse(self.android), self.parse(self.android))
        for rows in ([], self.android[:1], self.android + self.android[1:]):
            with self.subTest(rows=len(rows)), self.assertRaisesRegex(ValueError, "expected two"):
                self.parse(rows)

    def test_rejects_same_process_or_different_storage_instance(self):
        for key, value in (("pid", 100), ("nonce", self.windows[0]["nonce"])):
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "restart must read"):
                rows = [self.android[0], {**self.android[1], key: value}]
                self.parse(rows)

    def test_rejects_incomplete_and_wrong_target_rows(self):
        for patch in ({"checks": {}}, {"target_sdk": 35}, {"phase": "seed"},
                      {"checks": {**self.android[1]["checks"], "ce_file": 1}}):
            with self.subTest(patch=patch), self.assertRaisesRegex(ValueError, "invalid restart"):
                self.parse([self.android[0], {**self.android[1], **patch}])

    def test_blocked_execution_is_not_a_pass(self):
        row = {**self.android[0], "status": "blocked", "reason": "user_not_unlocked"}
        with self.assertRaisesRegex(ValueError, "user_not_unlocked"):
            self.parse([row, self.android[1]])


if __name__ == "__main__":
    unittest.main()
