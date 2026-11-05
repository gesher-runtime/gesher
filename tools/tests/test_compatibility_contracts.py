import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("contracts", Path(__file__).parents[1] / "compatibility_contracts.py")
contracts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(contracts)


class ContractIndexTest(unittest.TestCase):
    def test_groups_markers_by_section_and_records_line(self):
        rows = contracts.index_markdown("docs/a.md", "# A\nKnown gaps\nRemaining work\n## B\nnot implemented\n")
        self.assertEqual([(row["heading"], row["heading_line"]) for row in rows], [("A", 1), ("B", 4)])
        self.assertEqual([row["line"] for row in rows[0]["markers"]], [2, 3])

    def test_code_fences_not_treated_as_requirements(self):
        rows = contracts.index_markdown("a.md", "~~~python\n# TODO\nnot implemented\n~~~\n# Limits\n")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["heading_line"], 5)

    def test_negative_and_historical_text_never_becomes_asserted_gap(self):
        rows = contracts.index_markdown("a.md", "# Done\nNo remaining gaps\n")
        self.assertEqual(rows[0]["status"], "unreviewed_navigation_candidate")

    def test_scan_hashes_all_documents_including_without_markers(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "docs/decisions").mkdir(parents=True)
            (root / "docs/a.md").write_text("# Implemented\n", encoding="utf-8")
            (root / "docs/decisions/b.md").write_text("# Limits\n", encoding="utf-8")
            result = contracts.scan(root)
            self.assertEqual(result["summary"]["documents"], 2)
            self.assertEqual(result["summary"]["candidate_sections"], 1)
            self.assertEqual(result["errors"], [])
            self.assertEqual(len(result["documents"][0]["sha256"]), 64)

    def test_invalid_utf8_is_explicit_error(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "docs").mkdir()
            (root / "docs/a.md").write_bytes(b"\xff")
            self.assertEqual(contracts.scan(root)["errors"][0]["reason"], "UnicodeDecodeError")


if __name__ == "__main__":
    unittest.main()
