#!/usr/bin/env python3
"""Offline SDK class definitions versus actual boot-JAR DEX class definitions.

Class availability only: no methods, JNI registrations, semantics or APK scan.
Reports omit absolute paths, arbitrary class-file strings and DEX string data.
"""
from __future__ import annotations
import argparse
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import zipfile
import zlib


class InvalidInput(ValueError):
    """Only fixed diagnostic codes, never input bytes or exception payloads."""


@dataclass(frozen=True)
class Limits:
    max_archive_bytes: int = 128 << 20
    max_total_archive_bytes: int = 512 << 20
    max_entries: int = 50000
    max_central_directory_bytes: int = 16 << 20
    max_uncompressed_bytes: int = 128 << 20
    max_entry_bytes: int = 32 << 20
    max_compression_ratio: int = 1000
    max_strings: int = 1_000_000
    max_descriptor_bytes: int = 1024
    max_archives: int = 32


INTERNAL = re.compile(rb"[A-Za-z0-9_$-]+(?:/[A-Za-z0-9_$-]+)*\Z")
DEX_ENTRY = re.compile(r"classes(?:[2-9]|[1-9][0-9]+)?\.dex\Z")
KNOWN_NAMES = {"android.jar", "core-oj.jar", "core-libart.jar", "core-icu4j.jar",
               "framework.jar", "gesher-app.jar"}
FAMILIES = (("android.bluetooth.", "bluetooth_module", "module_or_feature_provider"),
            ("android.net.", "connectivity_modules", "module_or_framework_provider"),
            ("android.adservices.", "adservices_module", "module_or_feature_provider"),
            ("android.health.connect.", "healthconnect_module", "module_or_feature_provider"),
            ("android.nfc.", "nfc_module", "module_or_feature_provider"),
            ("android.uwb.", "uwb_module", "module_or_feature_provider"),
            ("android.app.sdksandbox.", "sdk_sandbox_module", "module_or_feature_provider"),
            ("android.test.", "android_test_libraries", "uses_library_provider_possible"),
            ("androidx.", "androidx_library", "apk_or_shared_library_provider_possible"),
            ("java.", "core_java", "core_boot_api"),
            ("javax.", "core_javax", "core_boot_api"),
            ("dalvik.", "core_dalvik", "core_boot_api"),
            ("android.", "android_framework_other", "framework_or_module_provider"))


def fail(code: str):
    raise InvalidInput(code)


def checked(data: bytes, offset: int, size: int):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        fail("out_of_bounds")


def u16(data, at):
    checked(data, at, 2)
    return struct.unpack_from("<H", data, at)[0]


def u32(data, at):
    checked(data, at, 4)
    return struct.unpack_from("<I", data, at)[0]


def class_descriptor(raw: bytes) -> str:
    if not raw.startswith(b"L") or not raw.endswith(b";") or not INTERNAL.fullmatch(raw[1:-1]):
        fail("invalid_or_unsupported_class_descriptor")
    return raw[1:-1].decode("ascii").replace("/", ".")


def validate_type(raw: bytes):
    depth = len(raw) - len(raw.lstrip(b"["))
    if depth > 255:
        fail("invalid_array_depth")
    base = raw[depth:]
    if base in (b"B", b"C", b"D", b"F", b"I", b"J", b"S", b"Z") or (base == b"V" and not depth):
        return
    class_descriptor(base)


def read_uleb(data: bytes, offset: int):
    value = 0
    for index in range(5):
        checked(data, offset + index, 1)
        byte = data[offset + index]
        if index == 4 and byte & 0xf0:
            fail("invalid_uleb128")
        value |= (byte & 0x7f) << (7 * index)
        if not byte & 0x80:
            return value, offset + index + 1
    fail("invalid_uleb128")


def dex_classes(data: bytes, limits: Limits = Limits()):
    checked(data, 0, 112)
    magic = data[:8]
    if magic == b"dex\n041\0":
        fail("unsupported_dex_041_container")
    if magic not in tuple(b"dex\n" + version + b"\0" for version in (b"035", b"037", b"038", b"039", b"040")):
        fail("unsupported_dex_magic_or_version")
    if u32(data, 32) != len(data) or u32(data, 36) != 112 or u32(data, 40) != 0x12345678:
        fail("invalid_dex_header_or_endianness")
    if zlib.adler32(data[12:]) & 0xffffffff != u32(data, 8):
        fail("dex_checksum_mismatch")
    if hashlib.sha1(data[32:]).digest() != data[12:32]:
        fail("dex_signature_mismatch")
    if u32(data, 44) or u32(data, 48):
        fail("unsupported_dex_link_section")
    data_size, data_at = u32(data, 104), u32(data, 108)
    if data_at < 112 or data_at % 4 or data_at + data_size != len(data):
        fail("invalid_dex_data_section")
    tables = {}
    intervals = []
    for name, at, width in (("strings", 56, 4), ("types", 64, 4), ("protos", 72, 12),
                            ("fields", 80, 8), ("methods", 88, 8), ("classes", 96, 32)):
        count, offset = u32(data, at), u32(data, at + 4)
        if (not count and offset) or (count and (offset < 112 or offset % 4)):
            fail("invalid_dex_table_offset")
        checked(data, offset, count * width)
        if count and offset + count * width > data_at:
            fail("dex_table_overlaps_data")
        tables[name] = count, offset
        if count:
            intervals.append((offset, offset + count * width))
    ordered = sorted(intervals)
    if any(a[1] > b[0] for a, b in zip(ordered, ordered[1:])):
        fail("overlapping_dex_tables")
    strings, string_at = tables["strings"]
    types, type_at = tables["types"]
    classes, class_at = tables["classes"]
    if strings > limits.max_strings or types > 65535 or classes > types:
        fail("dex_table_count_limit")
    map_at = u32(data, 52)
    if map_at < data_at or map_at % 4:
        fail("invalid_dex_map_offset")
    map_count = u32(data, map_at)
    if map_count > 65536:
        fail("dex_map_count_limit")
    checked(data, map_at + 4, map_count * 12)
    offsets = [u32(data, string_at + i * 4) for i in range(strings)]
    if any(offset < data_at or offset >= len(data) for offset in offsets):
        fail("invalid_dex_string_offset")
    descriptors = []
    previous = -1
    for index in range(types):
        string_index = u32(data, type_at + index * 4)
        if string_index >= strings or string_index <= previous:
            fail("invalid_dex_type_string_index")
        previous = string_index
        length, start = read_uleb(data, offsets[string_index])
        if length > limits.max_descriptor_bytes:
            fail("descriptor_length_limit")
        end = data.find(b"\0", start, min(len(data), start + limits.max_descriptor_bytes + 1))
        if end < 0 or end - start != length:
            fail("invalid_or_unsupported_descriptor_encoding")
        raw = data[start:end]
        validate_type(raw)
        descriptors.append(raw)
    names = set()
    seen_classes = set()
    for index in range(classes):
        at = class_at + index * 32
        class_index, superclass, source = u32(data, at), u32(data, at + 8), u32(data, at + 16)
        # class_defs are in superclass/interface dependency order, not sorted
        # by type index (DEX format, class_defs). Only duplicates are invalid.
        if class_index >= types or class_index in seen_classes:
            fail("invalid_dex_class_index")
        seen_classes.add(class_index)
        if superclass != 0xffffffff and superclass >= types or source != 0xffffffff and source >= strings:
            fail("invalid_dex_class_reference")
        for field in (12, 20, 24, 28):
            offset = u32(data, at + field)
            if offset and not data_at <= offset < len(data):
                fail("invalid_dex_class_data_offset")
        names.add(class_descriptor(descriptors[class_index]))
    return names, magic[4:7].decode("ascii")


def class_file(data: bytes, limits: Limits = Limits()):
    checked(data, 0, 10)
    if data[:4] != b"\xca\xfe\xba\xbe":
        fail("invalid_class_magic")
    be16 = lambda at: struct.unpack_from(">H", data, at)[0]
    major, count = be16(6), be16(8)
    if not 45 <= major <= 70 or count == 0:
        fail("unsupported_class_version_or_pool")
    pool, at, index = {}, 10, 1
    while index < count:
        checked(data, at, 1)
        tag, at = data[at], at + 1
        if tag == 1:
            checked(data, at, 2)
            size = be16(at)
            checked(data, at + 2, size)
            pool[index] = (tag, at + 2, size)
            at += 2 + size
        elif tag == 7:
            checked(data, at, 2)
            pool[index] = (tag, be16(at), 0)
            at += 2
        else:
            size = {3: 4, 4: 4, 5: 8, 6: 8, 8: 2, 9: 4, 10: 4, 11: 4, 12: 4,
                    15: 3, 16: 2, 17: 4, 18: 4, 19: 2, 20: 2}.get(tag)
            if size is None:
                fail("invalid_class_pool_tag")
            checked(data, at, size)
            at += size
            if tag in (5, 6):
                index += 1
                if index >= count:
                    fail("invalid_class_pool_wide_slot")
        index += 1
    checked(data, at, 6)
    flags, this_class = be16(at), be16(at + 2)
    item = pool.get(this_class)
    if not item or item[0] != 7:
        fail("invalid_class_this_index")
    name = pool.get(item[1])
    if not name or name[0] != 1 or name[2] > limits.max_descriptor_bytes:
        fail("invalid_class_name_index")
    raw = data[name[1]:name[1] + name[2]]
    return class_descriptor(b"L" + raw + b";"), bool(flags & 0x2000), major


def safe_file(path: Path, limits: Limits):
    path = path.absolute()
    for part in (path, *path.parents):
        info = part.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            fail("reparse_or_symbolic_input_path")
    if not path.is_file() or path.stat().st_size > limits.max_archive_bytes:
        fail("archive_file_size_limit")
    return path


def member_path(info):
    name = info.filename
    parts = name.rstrip("/").split("/")
    if (not name or "\\" in name or ":" in name or "\0" in name or name.startswith("/") or
            any(part in ("", ".", "..") or part.endswith((" ", ".")) for part in parts) or
            stat.S_ISLNK((info.external_attr >> 16) & 0xffff) or info.external_attr & 0x400):
        fail("unsafe_zip_member")


def archive_inventory(path: Path, role: str, source_id: str, limits: Limits):
    path = safe_file(path, limits)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1 << 20):
            digest.update(chunk)
        size = stream.tell()
        stream.seek(max(0, size - 65557))
        tail = stream.read(65557)
    eocd = tail.rfind(b"PK\x05\x06")
    if eocd < 0 or eocd + 22 > len(tail):
        fail("zip_eocd_missing")
    disk, cd_disk, disk_entries, entries, cd_size, cd_at, comment = struct.unpack_from("<HHHHIIH", tail, eocd + 4)
    if 0xffff in (disk_entries, entries) or 0xffffffff in (cd_size, cd_at):
        fail("unsupported_zip64_archive")
    if disk or cd_disk or disk_entries != entries or eocd + 22 + comment != len(tail):
        fail("invalid_zip_eocd")
    eocd_at = size - len(tail) + eocd
    if entries > limits.max_entries or cd_size > limits.max_central_directory_bytes or cd_at + cd_size > eocd_at:
        fail("zip_central_directory_limit")
    names, annotations, versions = set(), set(), Counter()
    total = selected = 0
    with zipfile.ZipFile(path) as archive:
        infos = archive.infolist()
        if len(infos) != entries:
            fail("zip_entry_count_mismatch")
        seen = set()
        for info in infos:
            member_path(info)
            if info.filename in seen:
                fail("duplicate_zip_member")
            seen.add(info.filename)
            total += info.file_size
            if (info.file_size > limits.max_entry_bytes or total > limits.max_uncompressed_bytes or
                    info.file_size > max(1, info.compress_size) * limits.max_compression_ratio or
                    info.flag_bits & 1 or info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)):
                fail("zip_member_size_or_compression_limit")
            if info.is_dir():
                continue
            if role == "sdk" and info.filename.endswith(".class") and not info.filename.startswith("META-INF/"):
                if info.filename == "module-info.class":
                    continue
                data = archive.read(info)
                name, annotation, version = class_file(data, limits)
                expected = info.filename[:-6].replace("/", ".")
                if name != expected or name in names:
                    fail("class_name_entry_mismatch")
                names.add(name)
                if annotation:
                    annotations.add(name)
                versions[f"class_major_{version}"] += 1
                selected += 1
            elif role == "boot" and info.filename.endswith(".dex"):
                if not DEX_ENTRY.fullmatch(info.filename):
                    fail("unsupported_boot_dex_member_name")
                classes, version = dex_classes(archive.read(info), limits)
                names.update(classes)
                versions[f"dex_{version}"] += 1
                selected += 1
    if not names:
        fail("no_class_definitions_in_input")
    manifest = {"source_id": source_id, "role": role, "name": path.name if path.name in KNOWN_NAMES else source_id,
                "sha256": digest.hexdigest(), "archive_bytes": size, "entries": entries,
                "uncompressed_bytes": total, "class_definitions": len(names), "selected_members": selected,
                "versions": dict(sorted(versions.items()))}
    return names, annotations, manifest


def family(name):
    for prefix, owner, scope in FAMILIES:
        if name.startswith(prefix):
            return owner, scope
    return "sdk_other", "framework_module_or_library_provider"


def inventory(sdk: Path, boots: list[Path], replacements: dict[str, Path] | None = None,
              limits: Limits = Limits(), sdk_api: int = 36, observed: set[str] | None = None):
    replacements = replacements or {}
    if any(value <= 0 for value in asdict(limits).values()) or not boots:
        fail("invalid_inventory_bounds_or_boot_list")
    if len(boots) + 1 + len(replacements) > limits.max_archives or len({p.name for p in boots}) != len(boots):
        fail("archive_count_or_duplicate_boot_name")
    if set(replacements) - {path.name for path in boots}:
        fail("candidate_replacement_has_no_base_input")
    all_paths = [sdk, *boots, *replacements.values()]
    if sum(safe_file(path, limits).stat().st_size for path in all_paths) > limits.max_total_archive_bytes:
        fail("total_archive_bytes_limit")
    sdk_names, annotations, sdk_manifest = archive_inventory(sdk, "sdk", "sdk", limits)
    manifests = [sdk_manifest]
    defined, candidate = set(), set()
    for index, path in enumerate(boots, 1):
        names, _, manifest = archive_inventory(path, "boot", f"boot-{index:02d}", limits)
        defined.update(names)
        manifests.append(manifest)
        if path.name in replacements:
            names, _, manifest = archive_inventory(replacements[path.name], "boot", f"candidate-{index:02d}", limits)
            manifest["replaces_source_id"] = f"boot-{index:02d}"
            manifests.append(manifest)
        candidate.update(names)
    observed = observed or set()
    def comparison(available):
        missing = []
        grouped = defaultdict(lambda: {"sdk_classes": 0, "boot_available": 0, "boot_absent": 0,
                                       "absent_annotations": 0})
        for name in sorted(sdk_names):
            owner, scope = family(name)
            group = grouped[owner]
            group["sdk_classes"] += 1
            group["boot_available" if name in available else "boot_absent"] += 1
            if name not in available:
                group["absent_annotations"] += name in annotations
                missing.append({"class": name, "family": owner, "provider_scope": scope,
                                "annotation_type": name in annotations,
                                "runtime_log_evidence": "observed_missing" if name in observed else "not_observed_in_selected_logs",
                                "outside_boot_provider_status": "not_assessed",
                                "method_and_behavior_status": "not_assessed"})
        return {"boot_defined_classes": len(available), "sdk_available": len(sdk_names & available),
                "sdk_absent_from_boot": len(missing), "missing_classes": missing,
                "families": dict(sorted(grouped.items()))}
    report = {"schema_version": 1, "scope": "public SDK static CLASS availability on selected boot path only",
              "declared_sdk_api": sdk_api, "sdk_api_claim": "caller_declared; exact input hash recorded",
              "not_a_defect_count": True, "limits": asdict(limits), "inputs": manifests,
              "sdk_class_definitions": len(sdk_names), "sdk_annotation_classes": len(annotations),
              "installed": comparison(defined),
              "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    if replacements:
        report["candidate"] = comparison(candidate)
        report["candidate_delta"] = {"added_boot_classes": sorted(candidate - defined),
                                     "removed_boot_classes": sorted(defined - candidate),
                                     "newly_sdk_available": sorted((candidate - defined) & sdk_names)}
    return report


def markdown(report):
    lines = ["# Static Java class availability", "", report["scope"], "",
             "Absent from this boot path is not absent globally: modules, uses-library and APK providers are not assessed.",
             "SDK stubs include annotation types. Counts do not measure method behavior, JNI registration or defects.", "",
             "| Input | SDK classes | Available on boot | Absent from boot |",
             "| --- | ---: | ---: | ---: |"]
    for name in ("installed", "candidate"):
        if name in report:
            item = report[name]
            lines.append(f"| {name} | {report['sdk_class_definitions']} | {item['sdk_available']} | {item['sdk_absent_from_boot']} |")
    lines += ["", "## Absent classes by family (installed)", "",
              "| Family | SDK classes | Absent | Absent annotation types |", "| --- | ---: | ---: | ---: |"]
    for name, item in report["installed"]["families"].items():
        lines.append(f"| {name} | {item['sdk_classes']} | {item['boot_absent']} | {item['absent_annotations']} |")
    if "candidate_delta" in report:
        lines += ["", "## Candidate added boot classes", ""]
        lines += [f"- `{name}`" for name in report["candidate_delta"]["added_boot_classes"]]
    lines += ["", "## SDK classes absent from installed boot path", ""]
    lines += [f"- `{item['class']}` ({item['family']}; annotation={item['annotation_type']})"
              for item in report["installed"]["missing_classes"]]
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-jar", required=True, type=Path)
    parser.add_argument("--sdk-api-level", default=36, type=int)
    parser.add_argument("--boot-jar", required=True, action="append", type=Path)
    parser.add_argument("--candidate-replace", action="append", default=[], metavar="BOOT_NAME=JAR")
    parser.add_argument("--observations-json", type=Path)
    parser.add_argument("--outdir", type=Path, default=Path("out/compat-audit/java"))
    args = parser.parse_args(argv)
    try:
        replacements = {}
        for value in args.candidate_replace:
            name, separator, path = value.partition("=")
            if not separator or name in replacements or name not in {p.name for p in args.boot_jar}:
                fail("invalid_candidate_replacement")
            replacements[name] = Path(path)
        observed = set()
        if args.observations_json:
            if args.observations_json.stat().st_size > 4 << 20:
                fail("observations_size_limit")
            data = json.loads(args.observations_json.read_text(encoding="utf-8"))
            for item in data.get("gaps", []):
                if item.get("category") == "java_class" and isinstance(item.get("identifier"), str):
                    name = item["identifier"]
                    if INTERNAL.fullmatch(name.replace(".", "/").encode("ascii", errors="ignore")):
                        observed.add(name)
        report = inventory(args.sdk_jar, args.boot_jar, replacements, sdk_api=args.sdk_api_level, observed=observed)
        args.outdir.mkdir(parents=True, exist_ok=True)
        for name, text in (("java-class-availability.json", json.dumps(report, indent=2) + "\n"),
                           ("java-class-availability.md", markdown(report))):
            target = args.outdir / name
            if target.is_symlink():
                fail("symbolic_output_path")
            target.write_text(text, encoding="utf-8")
        print(json.dumps({"sdk_classes": report["sdk_class_definitions"],
                          "installed_available": report["installed"]["sdk_available"],
                          "installed_absent": report["installed"]["sdk_absent_from_boot"],
                          "candidate_available": report.get("candidate", {}).get("sdk_available"),
                          "candidate_absent": report.get("candidate", {}).get("sdk_absent_from_boot")}))
    except (OSError, ValueError, zipfile.BadZipFile, RuntimeError) as error:
        code = str(error) if isinstance(error, InvalidInput) else type(error).__name__
        print(f"Java inventory failed: {code}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
