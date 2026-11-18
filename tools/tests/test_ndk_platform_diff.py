"""Normal profile, alias, missing-input and deterministic offline NDK fixtures."""
from copy import deepcopy
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ndk_platform_diff as diff
from test_elf_symbol_inventory import static_elf_fixture


def abi(arch="x86_64", minimum=21, default=True):
    return {"bitness": 64, "default": default, "deprecated": False, "arch": arch,
            "triple": arch + "-linux-android", "min_os_version": minimum,
            "proc": arch, "llvm_triple": arch + "-linux-android"}


def symbol(version="V1", default=True, size=None):
    return {"symbol": "sample", "version": version, "version_kind": "definition" if version else None,
            "version_provider": None, "category": "defined_export", "binding": "GLOBAL",
            "type": "OBJECT" if size is not None else "FUNC", "object_size": size,
            "visibility": "DEFAULT", "other_raw": 0, "definition_kind": "section",
            "version_local": False, "version_hidden_bit": not default if version else False,
            "default_version": default if version else None}


def ndk_fixture(path):
    compiler = "toolchains/llvm/prebuilt/linux-x86_64/"
    metadata = {
        "source.properties": "Pkg.Desc = Synthetic NDK fixture\nPkg.Revision = 30.0.0\nPkg.ReleaseName = fixture\n",
        "meta/platforms.json": json.dumps({"min": 21, "max": 37, "aliases": {}}),
        "meta/abis.json": json.dumps({"x86_64": abi(), "riscv64": abi("riscv64", 37, False)}),
        "meta/system_libs.json": json.dumps({"libplatform.so": "21", "liblater.so": "37"}),
        compiler + "AndroidVersion.txt": "Synthetic compiler metadata\n",
        compiler + "clang_source_info.md": "Synthetic source metadata\n",
    }
    files = {name: text.encode() for name, text in metadata.items()}
    for arch, machine, apis in (("x86_64", 62, (36, 37)), ("riscv64", 243, (37,))):
        for api in apis:
            prefix = f"{compiler}sysroot/usr/lib/{arch}-linux-android/{api}/"
            names = ["existing"] + (["added"] if api == 37 else [])
            files[prefix + "libplatform.so"] = static_elf_fixture(machine=machine, symbols=[{"name": n} for n in names])
            if api == 37:
                files[prefix + "liblater.so"] = static_elf_fixture(machine=machine, symbols=[{"name": "introduced_library_api"}])
            files[prefix + "libc++.so"] = b"INPUT(-lc++_shared)"
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo("ndk/" + name, (2020, 1, 1, 0, 0, 0))
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
    record = diff.archive_record(path)
    return {
        "schema_version": 1, "comparison_id": "synthetic-ndk-profile-fixture", "host_tag": "linux-x86_64",
        "archive": {**record, "member_root": "ndk", "url": "fixture://ndk/fixture.zip"},
        "repository_package": {"revision": "30.0.0", "checksum_type": "sha1", "sha1": record["sha1"], "bytes": record["bytes"], "url": "fixture.zip"},
        "repository_metadata_provenance": {"kind": "synthetic_fixture"}, "project_source_context": {"kind": "not_measured"},
        "before": {"requested_api": 36}, "after": {"requested_api": 37},
        "metadata_files": {name: {"text": text, **diff.bytes_record(text.encode())} for name, text in metadata.items()},
        "selected_files": {name: diff.bytes_record(data) for name, data in files.items() if "/sysroot/usr/lib/" in name},
    }


class NdkPlatformDiffTests(unittest.TestCase):
    def test_explicit_api_alias_and_abi_minimum(self):
        platforms = {"min": 21, "max": 37, "aliases": {"25": 24, "N-MR1": 24}}
        result = diff.api_profile(25, platforms, abi())
        self.assertEqual((result["requested_api"], result["effective_api"], result["alias_chain"]), (25, 24, [{"from": 25, "to": 24}]))
        self.assertEqual(diff.api_profile(36, platforms, abi())["effective_api"], 36)
        unavailable = diff.api_profile(36, platforms, abi("riscv64", 37, False))
        self.assertEqual(unavailable["status"], "below_abi_minimum")
        self.assertIsNone(unavailable["effective_api"])
        self.assertEqual(diff.api_profile(38, platforms, abi())["status"], "outside_ndk_api_range")

    def test_aliases_default_bit_sizes_and_duplicate_records(self):
        old = [symbol("V1", False), symbol("V2"), symbol(None, size=4)]
        new = [symbol("V1"), symbol("V2", False), symbol(None, size=8)]
        new[2]["binding"] = "WEAK"
        before, after = (diff.symbol_groups(items, "defined_export") for items in (old, new))
        rows = []
        counts = diff.delta(before, after, "defined_export", {}, rows)
        self.assertEqual(counts, {"before": 3, "after": 3, "added": 0, "removed": 0, "changed": 3, "unchanged": 0})
        self.assertEqual({json.loads(row["key"])["version"] for row in rows}, {"V1", "V2", None})
        duplicates = diff.symbol_groups([old[0], old[0]], "defined_export")
        self.assertEqual((len(duplicates), len(next(iter(duplicates.values())))), (1, 2))

    def test_numeric_version_indices_and_name_prefixes_do_not_remove_exports(self):
        old = [{"index": 13, "name": "LIBC_DEPRECATED", "flags": 0, "parents": []}]
        new = [{"index": 13, "name": "LIBC_37", "flags": 0, "parents": []}, {"index": 14, **{k: v for k, v in old[0].items() if k != "index"}}]
        rows = []
        counts = diff.delta(diff.named_versions(old), diff.named_versions(new), "version_definitions", {}, rows)
        self.assertEqual((counts["added"], counts["changed"], counts["unchanged"]), (1, 0, 1))
        self.assertEqual(rows[0]["key"], "LIBC_37")
        self.assertEqual(len(diff.symbol_groups([symbol("LIBC_DEPRECATED"), symbol("LIBC_PRIVATE")], "defined_export")), 2)

    def test_missing_directory_and_non_elf_library_keep_null_denominator(self):
        metadata = {"host_tag": "linux-x86_64", "meta/platforms.json": {"min": 21, "max": 37, "aliases": {}}, "meta/abis.json": {"x86_64": abi()}, "meta/system_libs.json": {"libplatform.so": "21"}}
        base = "ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/x86_64-linux-android/"
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "fixture.zip"
            with zipfile.ZipFile(path, "w") as zf:
                zf.writestr(base + "37/libplatform.so", "INPUT(-lprovider)")
            with zipfile.ZipFile(path) as zf:
                files, collected = {entry.filename: entry for entry in zf.infolist()}, {}
                missing, _ = diff.slice_inventory(zf, files, "ndk/", metadata, "x86_64", 36, collected)
                partial, _ = diff.slice_inventory(zf, files, "ndk/", metadata, "x86_64", 37, collected)
            self.assertEqual(missing["status"], "missing_api_directory")
            self.assertIsNone(missing["public_export_denominator"])
            self.assertEqual(missing["missing_system_libraries"], ["libplatform.so"])
            self.assertEqual(partial["status"], "incomplete_library_set")
            self.assertIsNone(partial["public_export_denominator"])
            self.assertEqual(partial["non_elf_system_libraries"], ["libplatform.so"])
            self.assertEqual(partial["excluded_files"][0]["reason"], "linker_script_not_resolved")

    def test_real_elf_zip_replay_and_unpaired_abi(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "ndk.zip"
            manifest = ndk_fixture(archive)
            report, rows = diff.compare(archive, manifest)
            paired = report["comparisons"]["x86_64"]
            self.assertEqual(paired["defined_exports"], {"before": 1, "after": 3, "added": 2, "removed": 0, "changed": 0, "unchanged": 1, "before_records": 1, "after_records": 3})
            self.assertEqual(paired["library_files"]["added"], 1)
            riscv = report["profiles"]["riscv64"]
            self.assertIsNone(riscv["before"]["public_export_denominator"])
            self.assertEqual(riscv["after"]["public_export_denominator"], 3)
            self.assertFalse(riscv["after"]["abi_metadata"]["default"])
            self.assertIsNone(report["comparisons"]["riscv64"]["defined_exports"])
            self.assertFalse(any(row["abi"] == "riscv64" for row in rows))
            shutil.copyfile(archive, root / "different.zip")
            again, again_rows = diff.compare(root / "different.zip", manifest)
            self.assertEqual((report, rows), (again, again_rows))
            diff.write_report(root / "one", report, rows)
            diff.write_report(root / "two", again, again_rows)
            for name in ("summary.json", "delta.jsonl", "README.md"):
                self.assertEqual((root / "one" / name).read_bytes(), (root / "two" / name).read_bytes())
            self.assertNotIn(temporary, json.dumps(report))
            wrong = deepcopy(manifest)
            wrong["archive"]["sha256"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "ndk_archive_pin_mismatch"):
                diff.compare(archive, wrong)


if __name__ == "__main__":
    unittest.main()
