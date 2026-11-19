"""Tests for scoped resource metadata and complete pinned family selection."""
from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sdk_platform_diff as platform
import sdk_resource_attrs_diff as attrs


FILE = "data/res/values/attrs.xml"


def parsed(body, extra=""):
    return attrs.parse_file(f'<resources{extra}>{body}</resources>'.encode(), FILE)


class MetadataTests(unittest.TestCase):
    def test_same_name_in_different_scopes_is_not_merged(self):
        records, facts = parsed('<attr name="color" format="color"/><declare-styleable name="View"><attr name="color"/></declare-styleable><declare-styleable name="Other"><attr name="color" format="integer"/></declare-styleable>')
        self.assertEqual(len(records["attributes"]), 3)
        self.assertEqual(records["attributes"][(FILE, None, 0, "color", 1)]["attributes"]["format"], "color")
        self.assertNotIn("format", records["attributes"][(FILE, "View", 1, "color", 1)]["attributes"])
        self.assertEqual(facts["counts"]["distinct_attribute_name_spellings"], 1)

    def test_metadata_spellings_absences_and_unknown_attributes_are_retained(self):
        records, _ = parsed('<attr name="a" format="reference|integer" min="-1" localization="suggested" future="literal"/><attr name="b" format=""/>')
        a = records["attributes"][(FILE, None, 0, "a", 1)]["attributes"]
        self.assertEqual(a, {"name": "a", "format": "reference|integer", "min": "-1", "localization": "suggested", "future": "literal"})
        self.assertNotIn("max", a)
        self.assertEqual(records["attributes"][(FILE, None, 0, "b", 1)]["attributes"]["format"], "")

    def test_styleable_parent_is_not_expanded(self):
        records, _ = parsed('<declare-styleable name="Base"><attr name="a"/></declare-styleable><declare-styleable name="Child" parent="Base"><attr name="b"/></declare-styleable>')
        child = records["styleables"][(FILE, "Child", 1)]
        self.assertEqual(child["attributes"]["parent"], "Base")
        self.assertEqual(child["attribute_order"], [{"name": "b", "occurrence": 1}])

    def test_source_attribute_order_is_visible_without_becoming_an_index(self):
        before, _ = parsed('<declare-styleable name="View"><attr name="z"/><attr name="a"/></declare-styleable>')
        after, _ = parsed('<declare-styleable name="View"><attr name="a"/><attr name="z"/></declare-styleable>')
        rows = []
        counts = attrs.mapping_delta("styleables", before["styleables"], after["styleables"], rows)
        self.assertEqual(counts["changed"], 1)
        self.assertEqual(rows[0]["before"]["attribute_order"][0]["name"], "z")
        self.assertNotIn("resource_id", rows[0]["after"])
        self.assertEqual(before["attributes"], after["attributes"])

    def test_enum_flag_aliases_and_integer_spellings_stay_distinct(self):
        records, facts = parsed('<attr name="mode"><enum name="none" value="0"/><enum name="zero" value="0x0"/><enum name="all" value="0xffffffff"/></attr><attr name="mask"><flag name="high" value="0x80000000"/></attr>')
        values = [v["attributes"]["value"] for v in records["symbols"].values()]
        self.assertEqual(values, ["0", "0x0", "0xffffffff", "0x80000000"])
        self.assertEqual((facts["counts"]["enum_symbols"], facts["counts"]["flag_symbols"]), (3, 1))

    def test_symbol_source_order_is_retained(self):
        records, _ = parsed('<attr name="mode"><enum name="z" value="9"/><enum name="a" value="1"/></attr>')
        order = records["attributes"][(FILE, None, 0, "mode", 1)]["symbol_order"]
        self.assertEqual([s["name"] for s in order], ["z", "a"])

    def test_duplicate_occurrences_are_diagnosed_without_silent_overwrite(self):
        records, facts = parsed('<declare-styleable name="View"><attr name="a"/><attr name="a"/></declare-styleable><declare-styleable name="View"><attr name="a"><enum name="x" value="1"/><enum name="x" value="2"/></attr></declare-styleable>')
        self.assertEqual((len(records["styleables"]), len(records["attributes"]), len(records["symbols"])), (2, 3, 2))
        self.assertEqual(len(facts["repeated_scoped_identities"]), 3)
        self.assertIn((FILE, "View", 1, "a", 2), records["attributes"])
        self.assertIn((FILE, "View", 2, "a", 1, "enum", "x", 2), records["symbols"])

    def test_namespace_declarations_keep_their_element_scope(self):
        records, _ = parsed('<declare-styleable name="View" xmlns:p="urn:child"><attr name="p:color" p:hint="literal"/></declare-styleable>', ' xmlns:p="urn:root"')
        self.assertEqual(records["resource_roots"][(FILE,)]["namespace_declarations"], [{"prefix": "p", "uri": "urn:root"}])
        self.assertEqual(records["styleables"][(FILE, "View", 1)]["namespace_declarations"], [{"prefix": "p", "uri": "urn:child"}])
        field = records["attributes"][(FILE, "View", 1, "p:color", 1)]
        self.assertEqual(field["attributes"]["{urn:child}hint"], "literal")
        self.assertEqual(field["namespace_declarations"], [])

    def test_root_metadata_is_preserved(self):
        records, _ = parsed('', ' product="car"')
        self.assertEqual(records["resource_roots"][(FILE,)]["attributes"], {"product": "car"})

    def test_documentation_is_not_misread_as_resource_declarations(self):
        records, facts = parsed('<!-- <attr name="fiction"/> --><eat-comment/><attr name="a"/><?note ignored?>')
        self.assertEqual(len(records["attributes"]), 1)
        self.assertEqual(facts["counts"]["eat_comment_markers"], 1)

    def test_uninterpreted_global_order_does_not_rekey_occurrences(self):
        before, _ = parsed('<attr name="a"/><attr name="b"/>')
        after, _ = parsed('<attr name="b"/><attr name="a"/>')
        self.assertEqual(before, after)

    def test_unsupported_structure_fails_instead_of_disappearing(self):
        for body in ('<string name="x">value</string>', '<attr name="a"><item name="b"/></attr>',
                     '<declare-styleable name="View"><flag name="b" value="1"/></declare-styleable>',
                     '<attr name="a">text</attr>', '<eat-comment metadata="not-discarded"/>'):
            with self.subTest(body=body), self.assertRaises(ValueError):
                parsed(body)

    def test_missing_names_or_symbol_values_fail(self):
        for body in ('<attr/>', '<declare-styleable/>', '<attr name="a"><enum name="x"/></attr>'):
            with self.subTest(body=body), self.assertRaises(ValueError):
                parsed(body)

    def test_complete_default_family_selection_is_explicit(self):
        self.assertTrue(attrs.family_member("data/res/values/attrs.xml"))
        self.assertTrue(attrs.family_member("data/res/values/attrs_new.xml"))
        for path in ("data/res/values-v37/attrs.xml", "data/res/values/strings.xml", "other/attrs.xml"):
            self.assertFalse(attrs.family_member(path))


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.before, old = self.archive("before", "36", {FILE: '<resources><attr name="old"/><attr name="common" format="integer"/></resources>'})
        self.after, new = self.archive("after", "37.0", {FILE: '<resources><attr name="common" format="reference|integer"/><declare-styleable name="New"><attr name="fresh"><enum name="yes" value="1"/></attr></declare-styleable></resources>',
            "data/res/values/attrs_extra.xml": '<resources><attr name="extra"/></resources>'})
        self.manifest = {"schema_version": 1, "comparison_id": "owned-resource-attrs-fixture", "before": old, "after": new,
            "source_platform_manifest_sha256": "0" * 64, "repository_metadata_provenance": {"source_url": "fixture://repository"},
            "source_format_context": {"url": "fixture://source"}}

    def archive(self, label, api, values):
        path, prefix = self.root / (label + ".zip"), "android-" + api
        props = {"AndroidVersion.ApiLevel": api, "Pkg.Revision": "2"}
        blobs = {"source.properties": "".join(f"{k}={v}\n" for k, v in props.items()).encode()}
        blobs.update({name: text.encode() for name, text in values.items()})
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in blobs.items():
                archive.writestr(prefix + "/" + name, data)
            archive.writestr(prefix + "/data/res/values/strings.xml", '<resources/>')
        receipt = platform.archive_record(path)
        return path, {"id": label, "package_path": "platforms;android-" + api,
            "archive": {**receipt, "member_root": prefix, "url": "fixture://repository/" + path.name},
            "repository_package": {"api_level_raw": api, "revision": "2", "channel": {"name": "fixture"},
                "complete_archive": {"checksum_type": "sha1", "checksum": receipt["sha1"], "bytes": receipt["bytes"], "url": path.name}},
            "source_properties": props, "artifacts": {name: platform.bytes_record(data) for name, data in blobs.items()}}

    def test_full_report_tracks_add_remove_change_with_accountable_counts(self):
        report, rows = attrs.compare(self.before, self.after, self.manifest)
        self.assertEqual(report["dimensions"]["attributes"], {"before": 2, "after": 3, "added": 2, "removed": 1, "changed": 1, "unchanged": 0})
        self.assertEqual(report["dimensions"]["resource_roots"]["added"], 1)
        for numbers in report["dimensions"].values():
            self.assertEqual(numbers["before"], numbers["removed"] + numbers["changed"] + numbers["unchanged"])
            self.assertEqual(numbers["after"], numbers["added"] + numbers["changed"] + numbers["unchanged"])
        self.assertFalse(report["a07_complete"])
        self.assertFalse(report["runtime_sdk_int_changed"])

    def test_omitted_member_of_selected_family_is_rejected(self):
        pin = copy.deepcopy(self.manifest["after"])
        del pin["artifacts"]["data/res/values/attrs_extra.xml"]
        with self.assertRaisesRegex(ValueError, "attribute_family_inventory_mismatch"):
            attrs.load_archive(self.after, pin)

    def test_non_family_pin_cannot_expand_scope_silently(self):
        pin = copy.deepcopy(self.manifest["before"])
        pin["artifacts"]["data/res/values/strings.xml"] = platform.bytes_record(b'<resources/>')
        with self.assertRaisesRegex(ValueError, "attribute_family_inventory_mismatch"):
            attrs.load_archive(self.before, pin)

    def test_archive_xml_and_package_pins_are_all_checked(self):
        changes = (("archive", "sha256", "platform_archive_pin_mismatch"),
                   ("xml", "sha256", "resource_attribute_artifact_pin_mismatch"),
                   ("repository_package", "api_level_raw", "repository_and_source_properties_disagree"))
        for section, key, error in changes:
            pin = copy.deepcopy(self.manifest["before"])
            if section == "xml":
                pin["artifacts"][FILE][key] = "0" * 64
            else:
                pin[section][key] = "0" * 64
            with self.subTest(section=section), self.assertRaisesRegex(ValueError, error):
                attrs.load_archive(self.before, pin)

    def test_repeated_archive_comparison_is_byte_stable_and_lf_only(self):
        for label in ("one", "two"):
            report, rows = attrs.compare(self.before, self.after, self.manifest)
            attrs.write_report(self.root / label, report, rows)
        for name in ("summary.json", "delta.jsonl", "README.md"):
            first = (self.root / "one" / name).read_bytes()
            self.assertEqual(first, (self.root / "two" / name).read_bytes())
            self.assertNotIn(b"\r\n", first)
        self.assertEqual(hashlib.sha256((self.root / "one/delta.jsonl").read_bytes()).hexdigest(), report["delta_rows_sha256"])

    def test_cli_failure_does_not_publish_partial_report(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["after"]["archive"]["sha1"] = "0" * 40
        path = self.root / "manifest.json"
        path.write_text(json.dumps(manifest))
        output = self.root / "output"
        with contextlib.redirect_stderr(io.StringIO()):
            status = attrs.main(["--manifest", str(path), "--before-archive", str(self.before),
                                "--after-archive", str(self.after), "--out-dir", str(output)])
        self.assertEqual(status, 1)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
