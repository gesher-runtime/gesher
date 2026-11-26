"""Check the peer-clock differential's verdict and evidence gates."""
import unittest

from compare_peer_clock import MARKER, compare, observations


def trace(**changes):
    rows = {
        "elf_machine": 62,
        "positive_resolution": 1,
        "same_live_id": 1024,
        "peer_delta_ns": 101_000_000,
        "caller_delta_ns": 12_000,
        "retired_gettime_errno": 22,
        "retired_getres_errno": 22,
        "capacity_acquired": 257,
        "capacity_distinct": 257,
        "capacity_first_errno": 0,
        "complete": 1,
    }
    rows.update(changes)
    return "\n".join(f"{MARKER}{key}={value}" for key, value in rows.items())


class PeerClockComparisonTest(unittest.TestCase):
    def setUp(self):
        self.reference = observations(trace(), 62)

    def test_different_cpu_times_can_mean_the_same_behavior(self):
        windows = observations(trace(peer_delta_ns=120_000_000, caller_delta_ns=100_000), 62)
        self.assertEqual(compare(self.reference, windows), {})

    def test_reports_capacity_and_retired_resolution_differences(self):
        windows = observations(trace(capacity_acquired=256, capacity_distinct=256,
                                     capacity_first_errno=11, retired_getres_errno=0), 62)
        self.assertEqual(compare(self.reference, windows), {
            "capacity_acquired": {"android": 257, "windows": 256},
            "capacity_distinct": {"android": 257, "windows": 256},
            "capacity_first_errno": {"android": 0, "windows": 11},
            "retired_getres_errno": {"android": 22, "windows": 0},
        })

    def test_rejects_incomplete_and_malformed_evidence(self):
        for output in (trace().rsplit("\n", 1)[0], trace() + f"\n{MARKER}extra=1",
                       trace() + f"\n{MARKER}complete=1", trace(complete=0),
                       trace(capacity_distinct=258), trace(capacity_first_errno=11),
                       trace(peer_delta_ns="unknown")):
            with self.subTest(output=output), self.assertRaises(ValueError):
                observations(output, 62)

    def test_rejects_output_from_other_abi(self):
        with self.assertRaisesRegex(ValueError, "ABI mismatch"):
            observations(trace(), 183)

    def test_cannot_match_against_an_unusable_android_oracle(self):
        for bad_reference in (trace(peer_delta_ns=40_000_000),
                              trace(caller_delta_ns=101_000_000),
                              trace(same_live_id=1023), trace(retired_gettime_errno=0)):
            with self.subTest(bad_reference=bad_reference), self.assertRaises(ValueError):
                parsed = observations(bad_reference, 62)
                compare(parsed, parsed)


if __name__ == "__main__":
    unittest.main()
