"""Normal SDK artifact fixtures test distinct evidence dimensions and replay."""
from copy import deepcopy
import hashlib
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sdk_platform_diff as diff
from test_java_member_inventory import classfile


def repository(api="36", extension="17", revision="2"):
    return {"api_level_raw": api, "extension_level_raw": extension, "base_extension_raw": "true",
            "revision": revision, "channel": {"id": "channel-0", "name": "stable"}, "codename": None}


def sdk_properties(api="36", extension="17", revision="2", preview=None):
    result = {"AndroidVersion.ApiLevel": api, "AndroidVersion.ExtensionLevel": extension,
              "AndroidVersion.IsBaseSdk": "true", "Pkg.Revision": revision, "Platform.CodeName": ""}
    if preview is not None:
        result["AndroidVersion.PreviewSdkInt"] = preview
    return result


def write_zip(path, contents):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(contents.items()):
            info = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)


def fixture_platform(path, identity, api, extension, contents=None):
    jar = io.BytesIO()
    write_zip(jar, {
        "java/lang/Object.class": classfile("java/lang/Object", None),
        "sample/AChild.class": classfile(members=(("method", "method", "()V", diff.java.PUBLIC),)),
        "sample/ZParent.class": classfile("sample/ZParent", "java/lang/Object"),
    })
    props = sdk_properties(api, extension, preview="0" if "." in api else None)
    files = {
        "android.jar": jar.getvalue(),
        "source.properties": "".join(f"{key}={value}\n" for key, value in sorted(props.items())).encode(),
        "build.prop": f"ro.build.version.sdk={api.split('.')[0]}\n".encode(),
        "framework.aidl": b"@JavaOnlyStableParcelable parcelable sample.Item;\n",
        "data/res/values/public-final.xml": b'<resources><public type="attr" name="old" id="0x01010001"/></resources>',
        "data/res/values/public-staging.xml": b'<resources><staging-public-group type="attr" first-id="0x01ff0000"/></resources>',
        "data/res/drawable/image.xml": b"<shape/>\n",
    }
    files.update(contents or {})
    write_zip(path, {f"android-test/{name}": value for name, value in files.items()})
    recorded = diff.archive_record(path)
    package = repository(api, extension)
    package["complete_archive"] = {"url": "platform.zip", "bytes": recorded["bytes"],
                                   "checksum_type": "sha1", "checksum": recorded["sha1"]}
    pin = {
        "id": identity, "package_path": f"platforms;android-{api}",
        "archive": {**recorded, "member_root": "android-test", "url": "fixture://platform.zip"},
        "source_properties": props, "repository_package": package,
        "artifacts": {name: diff.bytes_record(files[name]) for name in diff.MEASURED_FILES},
    }
    return pin


def cls(name, parent="java.lang.Object", items=(), interfaces=(), flags=diff.java.PUBLIC):
    members = [diff.java.Member(*item) for item in items]
    return diff.java.Class(name, flags, parent, interfaces, {item.key: item for item in members})


class SdkPlatformDiffTest(unittest.TestCase):
    def test_profiles_keep_minor_extension_revision_and_absent_preview_separate(self):
        before = diff.sdk_profile(sdk_properties(), repository())
        after = diff.sdk_profile(sdk_properties("36.1", "22", "3", "0"), repository("36.1", "22", "3"))
        self.assertEqual((before["api_major"], before["api_minor"], before["extension_level"], before["sdk_package_revision"]), (36, 0, 17, "2"))
        self.assertEqual((after["api_major"], after["api_minor"], after["extension_level"], after["sdk_package_revision"]), (36, 1, 22, "3"))
        self.assertIsNone(before["preview_sdk_int"])
        self.assertEqual(after["preview_sdk_int"], "0")
        self.assertNotIn("AndroidVersion.PreviewSdkInt", before["source_properties"])
        self.assertFalse(before["api_minor_explicit_in_source_properties"])
        self.assertTrue(after["api_minor_explicit_in_source_properties"])
        with self.assertRaisesRegex(ValueError, "repository_and_source_properties_disagree"):
            diff.sdk_profile(sdk_properties("37.0", "22"), repository())

    def test_declaration_movement_is_inherited_but_constructor_is_not(self):
        method = ("method", "moved", "()V", diff.java.PUBLIC)
        constructor = ("method", "<init>", "()V", diff.java.PUBLIC)
        before = {item.name: item for item in (
            cls("java.lang.Object", None), cls("sample.Parent"), cls("sample.Child", "sample.Parent", (method, constructor)),
        )}
        after = {item.name: item for item in (
            cls("java.lang.Object", None), cls("sample.Parent", items=(method, constructor)), cls("sample.Child", "sample.Parent"),
        )}
        rows = []
        summary = diff.java_delta(before, after, rows)
        removed = {row["name"]: row for row in rows if row.get("change") == "removed"}
        self.assertEqual(removed["moved"]["target_lookup"]["location"], "inherited")
        self.assertEqual(removed["moved"]["target_lookup"]["candidates"], [{"class": "sample.Parent", "access_flags": 1}])
        self.assertEqual(removed["<init>"]["target_lookup"]["status"], "descriptor_not_found")
        self.assertEqual(summary["public_protected_declarations"], {"before": 2, "after": 2, "added": 2, "removed": 2, "flags_changed": 0, "unchanged": 0})

    def test_flags_headers_visibility_and_class_denominators(self):
        first = cls("sample.Hidden", items=(("field", "VALUE", "I", 1), ("method", "visible", "()V", 1)), flags=0)
        second = cls("sample.Hidden", items=(("field", "VALUE", "I", 9), ("method", "visible", "()V", 2)), flags=16)
        rows = []
        summary = diff.java_delta({first.name: first}, {second.name: second}, rows)
        self.assertEqual(summary["encoded_classfile_visibility"], {"before": {"other": 1}, "after": {"other": 1}})
        self.assertEqual(summary["encoded_class_definitions"]["changed"], 1)
        self.assertEqual(summary["public_protected_declarations"], {"before": 2, "after": 1, "added": 0, "removed": 1, "flags_changed": 1, "unchanged": 0})
        removed = next(row for row in rows if row.get("name") == "visible")
        self.assertEqual(removed["target_lookup"]["status"], "descriptor_found")
        self.assertEqual(removed["target_lookup"]["possible_flag_differences"], ["public_visibility_reduced"])

    def test_missing_and_ambiguous_hierarchies_remain_uncertain(self):
        method = ("method", "operation", "()V", diff.java.PUBLIC | diff.java.ABSTRACT)
        before = {item.name: item for item in (cls("sample.Missing", items=(method,)), cls("sample.Ambiguous", items=(method,)))}
        after = {item.name: item for item in (
            cls("java.lang.Object", None), cls("sample.Missing", "unavailable.Parent"),
            cls("sample.Ambiguous", interfaces=("sample.One", "sample.Two")),
            cls("sample.One", None, (method,), flags=diff.java.PUBLIC | diff.java.INTERFACE | diff.java.ABSTRACT),
            cls("sample.Two", None, (method,), flags=diff.java.PUBLIC | diff.java.INTERFACE | diff.java.ABSTRACT),
        )}
        rows = []
        summary = diff.java_delta(before, after, rows)
        removed = {row["class"]: row["target_lookup"] for row in rows if row.get("category") == "java_member_declaration" and row["change"] == "removed"}
        self.assertEqual(removed["sample.Missing"], {"status": "unresolved_hierarchy", "unresolved_parents": ["unavailable.Parent"]})
        self.assertEqual(removed["sample.Ambiguous"]["status"], "resolution_uncertain")
        self.assertEqual(len(removed["sample.Ambiguous"]["candidates"]), 2)
        self.assertEqual(summary["before_declaration_target_lookup"]["denominator"], 2)

    def test_aidl_annotations_are_distinct_from_declaration_membership(self):
        before = diff.parse_aidl(b"@JavaOnlyStableParcelable parcelable sample.Item;\ninterface sample.Old;\n")
        after = diff.parse_aidl(b"parcelable sample.Item;\n@JavaOnlyStableParcelable parcelable sample.New;\n")
        rows = []
        counts = diff.mapping_delta(before, after, "preprocessed_aidl_declaration", rows)
        self.assertEqual(counts, {"before": 2, "after": 2, "added": 1, "removed": 1, "changed": 1, "unchanged": 0})
        change = next(row for row in rows if row["change"] == "changed")
        self.assertEqual(change["before"]["annotations"], ["@JavaOnlyStableParcelable"])
        self.assertEqual(change["after"]["annotations"], [])
        # Source interfaces require a separate source-AIDL parser, not silent dropping.
        with self.assertRaisesRegex(ValueError, "unsupported_preprocessed_aidl_form"):
            diff.parse_aidl(b"interface sample.Service { void invoke(); }\n")

    def test_staging_promotion_aliases_and_repeated_removed_slots(self):
        final = b'''<resources>
          <public type="attr" name="first" id="0x1010001"/>
          <public type="attr" name="last" id="0x01010002"/>
          <staging-public-group-final type="attr" first-id="0x01ff0000">
            <public name="first"/><public name="removed_"/><eat-comment/>
            <public name="removed_"/><public name="last"/>
          </staging-public-group-final>
        </resources>'''
        staging = b'''<resources>
          <staging-public-group type="attr" first-id="0x01fe0000">
            <public name="next"/><public name="removed_previous"/><public name="later"/>
          </staging-public-group><staging-public-group type="string" first-id="0x01fd0000"/>
        </resources>'''
        result = diff.parse_resources(final, staging)
        aliases = result["finalized_staging_alias_slots"]
        self.assertEqual(len(aliases), 4)
        self.assertEqual(aliases["0x01ff0003"]["final_id"], "0x01010002")
        self.assertTrue(aliases["0x01ff0001"]["removed_placeholder"])
        self.assertIsNone(aliases["0x01ff0002"]["final_id"])
        self.assertEqual(result["active_staging_names"]["attr/later"]["id"], "0x01fe0002")
        self.assertEqual(len(result["active_staging_names"]), 2)
        self.assertEqual(len(result["staging_slots"]), 3)
        self.assertEqual(result["groups"]["staging-public-group/string/0x01fd0000"]["slot_count"], 0)
        promoted = diff.parse_resources(final, b"<resources/>")
        before = diff.parse_resources(b"<resources/>", final.replace(b"staging-public-group-final", b"staging-public-group").replace(b'<public type="attr" name="first" id="0x1010001"/>', b"").replace(b'<public type="attr" name="last" id="0x01010002"/>', b""))
        rows = []
        self.assertEqual(diff.mapping_delta(before["active_staging_names"], promoted["active_staging_names"], "staging", rows)["removed"], 2)
        self.assertEqual(promoted["finalized_staging_alias_slots"]["0x01ff0000"]["final_id"], "0x01010001")
        with self.assertRaisesRegex(ValueError, "unsupported_resource_group_form"):
            diff.parse_resources(b'<resources><public-group type="attr" first-id="0x01010000"/></resources>', b"<resources/>")

    def test_real_encoding_replay_is_deterministic_and_counts_all_resource_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            old_path, new_path = root / "old.zip", root / "new.zip"
            before = fixture_platform(old_path, "sdk36-fixture", "36", "17")
            after = fixture_platform(new_path, "sdk37-fixture", "37.0", "22", {
                "framework.aidl": b"@JavaOnlyStableParcelable parcelable sample.Item;\nparcelable sample.Added;\n",
                "data/res/drawable/image.xml": b"<shape><solid/></shape>\n",
                "data/res/values/new.xml": b"<resources/>\n",
            })
            manifest = {"schema_version": 1, "comparison_id": "fixture-comparison", "before": before, "after": after,
                        "repository_metadata_provenance": {"kind": "synthetic_test_fixture"},
                        "project_source_context": {"kind": "not_measured"}}
            report, rows = diff.compare(old_path, new_path, manifest)
            self.assertEqual(report["profiles"]["before"]["api_major"], 36)
            self.assertEqual(report["profiles"]["after"]["api_major"], 37)
            self.assertEqual(report["java"]["public_protected_declarations"]["unchanged"], 1)
            self.assertEqual(report["preprocessed_aidl"]["added"], 1)
            self.assertEqual(report["resources"]["files"], {"before": 3, "after": 4, "added": 1, "removed": 0, "changed": 1, "unchanged": 2})
            self.assertIn("java_constant_values", report["unmeasured"])
            moved = root / "different-directory"
            moved.mkdir()
            shutil.copyfile(old_path, moved / "before.zip")
            shutil.copyfile(new_path, moved / "after.zip")
            again, rows_again = diff.compare(moved / "before.zip", moved / "after.zip", manifest)
            self.assertEqual((report, rows), (again, rows_again))
            diff.write_report(root / "one", report, rows)
            diff.write_report(root / "two", again, rows_again)
            ordinary_open = Path.open

            def windows_text_open(path, mode="r", buffering=-1, encoding=None,
                                  errors=None, newline=None):
                # Exercise the actual writer with Windows' default text-output
                # translation, while honoring an explicitly selected newline.
                if "w" in mode and "b" not in mode and newline is None:
                    newline = "\r\n"
                return ordinary_open(path, mode, buffering, encoding, errors, newline)

            with patch.object(Path, "open", windows_text_open):
                diff.write_report(root / "windows", report, rows)
            for filename in ("summary.json", "delta.jsonl", "README.md"):
                expected = (root / "one" / filename).read_bytes()
                self.assertEqual(expected, (root / "two" / filename).read_bytes())
                translated = (root / "windows" / filename).read_bytes()
                self.assertEqual(expected, translated)
                self.assertNotIn(b"\r\n", translated)
            self.assertEqual(hashlib.sha256((root / "windows" / "delta.jsonl").read_bytes()).hexdigest(),
                             report["delta_rows_sha256"])
            self.assertNotIn(temporary, json.dumps(report))
            wrong = deepcopy(manifest)
            wrong["after"]["archive"]["sha256"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "platform_archive_pin_mismatch"):
                diff.compare(old_path, new_path, wrong)


if __name__ == "__main__":
    unittest.main()
