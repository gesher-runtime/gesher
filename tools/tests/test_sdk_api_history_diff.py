"""Behavioral tests for explicit, pinned SDK API-history comparison."""
from __future__ import annotations

import contextlib
import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sdk_api_history_diff as history
import sdk_platform_diff as platform


def xml(body, schema="4", extra=""):
    return f'<api version="{schema}"{extra}>{body}</api>'.encode()


class HistoryTests(unittest.TestCase):
    def test_minor_versions_keep_precision_and_explicitness(self):
        self.assertEqual(history.version("36.10", "4"),
                         {"raw": "36.10", "major": 36, "minor": 10, "minor_explicit": True})
        self.assertEqual(history.version("36.0", "4")["minor"], 0)
        self.assertIsNone(history.version("36", "4")["minor"])
        self.assertIsNone(history.version(None, "4"))

    def test_schema_three_does_not_silently_accept_minor_history(self):
        with self.assertRaisesRegex(ValueError, "unsupported_api_history_version"):
            history.parse_history(xml('<class name="a/A" since="36.1"/>', "3"))

    def test_absent_attributes_remain_absent_and_do_not_inherit(self):
        parsed = history.parse_history(xml('<class name="a/A" since="30" deprecated="36.1" sdks="0:30"><method name="call()V"/></class>'))
        attrs = parsed["records"]["method"][("a/A", "call()V")]
        row = history.describe_record(attrs, "4")
        self.assertNotIn("since", attrs)
        self.assertEqual(row["version_components"], {"since": None, "deprecated": None, "removed": None})
        self.assertIsNone(row["sdk_memberships"])

    def test_parent_minimum_is_recorded_without_manufacturing_class_since(self):
        parsed = history.parse_history(xml('<class name="a/A"/>', extra=' min="36.1"'))
        observed = history.observations(parsed)
        self.assertEqual(observed["minimum_version"]["minor"], 1)
        self.assertEqual(observed["explicit_metadata"]["class"]["versions"]["since"]["absent"], 1)

    def test_extension_and_platform_versions_are_separate(self):
        values = history.sdk_memberships("33:18,0:36.1,1000000:6", "4")
        self.assertEqual([v["sdk_id_raw"] for v in values], ["33", "0", "1000000"])
        self.assertEqual(values[0]["extension_version"], 18)
        self.assertIsNone(values[0]["platform_version"])
        self.assertEqual(values[1]["platform_version"]["minor"], 1)
        self.assertIsNone(values[1]["extension_version"])

    def test_unknown_catalog_memberships_are_visible(self):
        parsed = history.parse_history(xml('<sdk id="33" name="T"/><class name="a/A" sdks="33:4,999:8,0:36.1"/>'))
        observed = history.observations(parsed)
        self.assertEqual(observed["unresolved_extension_catalog_memberships"], {"999": 1})
        self.assertNotIn("shortname", observed["extension_catalog"]["33"])

    def test_empty_and_missing_sdks_are_distinct(self):
        parsed = history.parse_history(xml('<class name="a/A"/><class name="a/B" sdks=""/>'))
        observed = history.observations(parsed)["explicit_metadata"]["class"]["sdks"]
        self.assertEqual((observed["present"], observed["absent"], observed["empty"]), (1, 1, 1))
        self.assertIsNone(history.sdk_memberships(None, "4"))
        self.assertEqual(history.sdk_memberships("", "4"), [])

    def test_module_jar_and_empty_labels_are_not_conflated(self):
        parsed = history.parse_history(xml('<class name="a/A" module="framework-pdf"/><class name="a/B" jar="framework-pdf"/><class name="a/C" module=""/>'))
        observed = history.observations(parsed)["explicit_metadata"]["class"]
        self.assertEqual(observed["module_labels"], {"": 1, "framework-pdf": 1})
        self.assertEqual(observed["jar_labels"], {"framework-pdf": 1})
        self.assertEqual((observed["module_absent"], observed["jar_absent"]), (1, 2))

    def test_descriptors_and_inheritance_edges_have_distinct_identities(self):
        parsed = history.parse_history(xml('<class name="a/A"><method name="run()I"/><method name="run()V"/><field name="run"/><extends name="a/Base"/><implements name="a/Base"/></class>'))
        self.assertEqual({k: len(v) for k, v in parsed["records"].items()},
                         {"class": 1, "method": 2, "field": 1, "extends": 1, "implements": 1})

    def test_old_removed_record_is_counted_as_history(self):
        parsed = history.parse_history(xml('<class name="a/Retired" since="1" removed="20"/>'))
        observed = history.observations(parsed)
        self.assertEqual(observed["records_by_kind"]["class"], 1)
        self.assertEqual(observed["explicit_metadata"]["class"]["versions"]["removed"]["raw_values"], {"20": 1})

    def test_unknown_structure_fails_instead_of_disappearing(self):
        for data in (b'<api version="5"/>', xml('<package name="a"/>'),
                     xml('<class name="a/A" future="1"/>'),
                     xml('<class name="a/A"><future name="x"/></class>'),
                     xml('<class name="a/A">text</class>')):
            with self.subTest(data=data), self.assertRaises(ValueError):
                history.parse_history(data)

    def test_duplicate_identities_and_memberships_are_not_overwritten(self):
        for body in ('<class name="a/A"/><class name="a/A"/>',
                     '<sdk id="33"/><sdk id="33"/>',
                     '<class name="a/A"><field name="x"/><field name="x"/></class>',
                     '<class name="a/A" sdks="33:4,33:5"/>'):
            with self.subTest(body=body), self.assertRaisesRegex(ValueError, "duplicate"):
                history.parse_history(xml(body))

    def test_metadata_diff_counts_and_stable_order(self):
        before = {("a/A",): {"name": "a/A"}, ("a/B",): {"name": "a/B", "since": "36"}, ("a/Old",): {"name": "a/Old"}}
        after = {("a/New",): {"name": "a/New"}, ("a/B",): {"name": "a/B", "since": "36.0"}, ("a/A",): {"name": "a/A"}}
        rows = []
        result = history.mapping_delta("class", before, after, rows)
        self.assertEqual(result, {"before": 3, "after": 3, "added": 1, "removed": 1, "changed": 1, "unchanged": 1})
        self.assertEqual([r["key"] for r in rows], [["a/B"], ["a/New"], ["a/Old"]])
        self.assertEqual(rows[0]["before"]["since"], "36")
        self.assertEqual(rows[0]["after"]["since"], "36.0")


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.before, before_pin = self.archive("before", "36", xml('<class name="a/A" since="36"><method name="run()V"/></class>', "3"))
        self.after, after_pin = self.archive("after", "37.0", xml('<sdk id="37" name="C"/><class name="a/A" since="36"><method name="run()V" since="36.1"/></class><class name="a/B" since="37.0"/>'))
        self.manifest = {"schema_version": 1, "comparison_id": "owned-history-fixture",
                         "before": before_pin, "after": after_pin,
                         "repository_metadata_provenance": {"source_url": "fixture://repository"},
                         "source_platform_manifest_sha256": "0" * 64}

    def archive(self, label, api, data):
        path, prefix = self.root / (label + ".zip"), "android-" + api
        props = {"AndroidVersion.ApiLevel": api, "Pkg.Revision": "2"}
        props_data = "".join(f"{k}={v}\n" for k, v in props.items()).encode()
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(prefix + "/source.properties", props_data)
            archive.writestr(prefix + "/data/api-versions.xml", data)
        receipt = platform.archive_record(path)
        official = {"checksum_type": "sha1", "checksum": receipt["sha1"], "bytes": receipt["bytes"], "url": path.name}
        pin = {"id": label, "package_path": "platforms;android-" + api,
               "archive": {**receipt, "member_root": prefix, "url": "fixture://repository/" + path.name},
               "repository_package": {"complete_archive": official, "api_level_raw": api, "revision": "2", "channel": {"name": "fixture"}},
               "source_properties": props,
               "artifacts": {"source.properties": platform.bytes_record(props_data), "data/api-versions.xml": platform.bytes_record(data)}}
        return path, pin

    def test_full_pinned_report_has_accountable_denominators(self):
        report, rows = history.compare(self.before, self.after, self.manifest)
        self.assertEqual(report["dimensions"]["method"]["changed"], 1)
        self.assertEqual(report["dimensions"]["class"]["added"], 1)
        self.assertEqual(report["dimensions"]["extension_catalog"]["added"], 1)
        self.assertEqual(report["dimensions"]["root_metadata"]["changed"], 1)
        for values in report["dimensions"].values():
            self.assertEqual(values["before"], values["removed"] + values["changed"] + values["unchanged"])
            self.assertEqual(values["after"], values["added"] + values["changed"] + values["unchanged"])
        self.assertFalse(report["a07_complete"])
        self.assertFalse(report["runtime_sdk_int_changed"])
        method = next(r for r in rows if r["category"] == "method")
        self.assertIsNone(method["before"]["version_components"]["since"])
        self.assertEqual(method["after"]["version_components"]["since"]["minor"], 1)

    def test_changed_archive_pin_fails(self):
        pin = copy.deepcopy(self.manifest["before"])
        pin["archive"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "platform_archive_pin_mismatch"):
            history.load_archive(self.before, pin)

    def test_changed_xml_pin_fails(self):
        pin = copy.deepcopy(self.manifest["before"])
        pin["artifacts"]["data/api-versions.xml"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "api_history_artifact_pin_mismatch"):
            history.load_archive(self.before, pin)

    def test_inconsistent_package_metadata_fails(self):
        pin = copy.deepcopy(self.manifest["before"])
        pin["repository_package"]["api_level_raw"] = "37"
        with self.assertRaisesRegex(ValueError, "repository_and_source_properties_disagree"):
            history.load_archive(self.before, pin)

    def test_outputs_are_repeatable_and_use_exact_lf_bytes(self):
        report, rows = history.compare(self.before, self.after, self.manifest)
        for label in ("one", "two"):
            history.write_report(self.root / label, report, rows)
        for name in ("summary.json", "delta.jsonl", "README.md"):
            data = (self.root / "one" / name).read_bytes()
            self.assertEqual(data, (self.root / "two" / name).read_bytes())
            self.assertNotIn(b"\r\n", data)
        import hashlib
        self.assertEqual(hashlib.sha256((self.root / "one/delta.jsonl").read_bytes()).hexdigest(), report["delta_rows_sha256"])

    def test_cli_rejects_bad_pin_without_publishing_partial_report(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["after"]["archive"]["sha1"] = "0" * 40
        path = self.root / "manifest.json"
        path.write_text(json.dumps(manifest))
        output = self.root / "output"
        with contextlib.redirect_stderr(io.StringIO()):
            result = history.main(["--manifest", str(path), "--before-archive", str(self.before),
                                   "--after-archive", str(self.after), "--out-dir", str(output)])
        self.assertEqual(result, 1)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
