#!/usr/bin/env python3
"""Offline NDK public platform-stub export comparison for separately pinned APIs.

Reads a pinned NDK ZIP in place. No libraries are loaded, tools from the archive
are run, or full toolchain is extracted. Packaged stub exports do not establish
runtime implementation, C/C++ signatures, header semantics or device support.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import stat
import sys
import zipfile

import elf_symbol_inventory as elf


SCHEMA_VERSION = 1
ARCH_MACHINES = {"arm": 40, "arm64": 183, "x86": 3, "x86_64": 62, "riscv64": 243}
METADATA_PATHS = ("source.properties", "meta/platforms.json", "meta/abis.json", "meta/system_libs.json")
COMPILER_METADATA = ("AndroidVersion.txt", "clang_source_info.md")
UNMEASURED = {
    "runtime_implementation": "Stub exports do not implement functions or prove runtime availability, correctness, permissions, services or hardware support.",
    "header_and_language_abi": "Headers, types, prototypes, layouts, inline functions, macros, constants, C++ mangling semantics and full C/C++ ABI compatibility are not parsed or qualified.",
    "data_values_and_function_bodies": "Object/TLS sizes are raw ELF st_size observations. Data values, pointers, function addresses, instructions and function sizes are not compared.",
    "private_source_api_classification": "ELF has no Android-public API bit. The platform-stub library set comes from system_libs.json; no source/header API-visibility filter or name-prefix heuristic is applied.",
    "loader_and_linker_execution": "Relocations, loader search order, symbol-version matching at runtime, linker scripts, CRT behavior, static archives and C++ runtime providers are not executed or resolved.",
    "source_revision_alignment": "NDK release/compiler identity, API slices, SDK artifact profiles and Gesher's AOSP source tag are separate evidence dimensions; source/module equivalence is not inferred.",
    "other_android_contracts": "Source AIDL, CompatChanges, CTS/CDD, Java behavior and API37 runtime support are not measured by this NDK comparison.",
}


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(",", ":"))


def fingerprint(value):
    return hashlib.sha256(canonical(value).encode("ascii")).hexdigest()


def bytes_record(data):
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def archive_record(path):
    sha1, sha256, size = hashlib.sha1(), hashlib.sha256(), 0
    with path.open("rb") as stream:
        while chunk := stream.read(1 << 20):
            size += len(chunk)
            sha1.update(chunk)
            sha256.update(chunk)
    return {"bytes": size, "sha1": sha1.hexdigest(), "sha256": sha256.hexdigest()}


def properties(text):
    result = {}
    for line in text.splitlines():
        if not line.strip() or line.lstrip().startswith(("#", "!")):
            continue
        if "=" not in line or "\\" in line:
            raise ValueError("unsupported_ndk_properties_form")
        key, value = (part.strip() for part in line.split("=", 1))
        if not key or key in result:
            raise ValueError("duplicate_ndk_property")
        result[key] = value
    return result


def api_profile(requested, platforms, abi):
    """Resolve only explicit aliases, never select a guessed nearby directory."""
    if not isinstance(requested, int):
        raise ValueError("numeric_requested_api_required")
    result = {"requested_api": requested, "effective_api": None, "alias_chain": [],
              "abi_min_os_version": abi["min_os_version"], "status": "available"}
    if requested < abi["min_os_version"]:
        result["status"] = "below_abi_minimum"
        return result
    if not platforms["min"] <= requested <= platforms["max"]:
        result["status"] = "outside_ndk_api_range"
        return result
    current, seen = requested, set()
    while str(current) in platforms["aliases"]:
        if current in seen:
            raise ValueError("cyclic_platform_api_alias")
        seen.add(current)
        target = platforms["aliases"][str(current)]
        if not isinstance(target, int):
            raise ValueError("non_numeric_platform_api_alias")
        result["alias_chain"].append({"from": current, "to": target})
        current = target
    if current < abi["min_os_version"] or not platforms["min"] <= current <= platforms["max"]:
        result["status"] = "alias_outside_supported_range"
        return result
    result["effective_api"] = current
    return result


def symbol_identity(row):
    return {key: row[key] for key in ("symbol", "version", "version_kind", "version_provider")}


def symbol_groups(rows, category):
    """Retain duplicate records, and keep differently versioned aliases distinct."""
    groups = defaultdict(list)
    for row in rows:
        if row["category"] == category:
            groups[canonical(symbol_identity(row))].append(row)
    return {key: sorted(value, key=canonical) for key, value in sorted(groups.items())}


def delta(before, after, category, context, rows):
    counts = {"before": len(before), "after": len(after), "added": 0, "removed": 0, "changed": 0, "unchanged": 0}
    for key in sorted(before.keys() | after.keys()):
        old, new = before.get(key), after.get(key)
        change = "added" if old is None else "removed" if new is None else "changed" if old != new else "unchanged"
        counts[change] += 1
        if change != "unchanged":
            rows.append({"category": category, "change": change, **context, "key": key, "before": old, "after": new})
    return counts


def named_versions(items, requirements=False):
    result = {}
    for item in items:
        value = {key: val for key, val in item.items() if key != "index"}
        key = canonical([item.get("provider"), item["name"]]) if requirements else item["name"]
        if key in result:
            raise ValueError("duplicate_named_version_record")
        result[key] = value
    return result


def library_summary(item):
    observation = item["elf"]
    exports = symbol_groups(observation["symbols"], "defined_export")
    defined = [row for row in observation["symbols"] if row["category"] == "defined_export"]
    result = {"path": item["path"], "artifact": item["artifact"],
            **{key: observation[key] for key in ("header", "dynamic", "version_definitions", "version_requirements", "dynamic_symbol_count", "category_counts")},
            "defined_export_records": observation["category_counts"].get("defined_export", 0),
            "defined_export_identities": len(exports),
            "defined_export_inventory_sha256": fingerprint(exports),
            "all_dynamic_symbol_records_sha256": fingerprint(sorted(observation["symbols"], key=canonical)),
            "export_attribute_counts": {key: dict(sorted(Counter(row[key] for row in defined).items()))
                                        for key in ("binding", "type", "visibility")},
            "export_version_form_counts": dict(sorted(Counter(
                "unversioned" if row["version"] is None else "named_default" if row["default_version"] else "named_nondefault"
                for row in defined).items()))}
    result["category_counts"] = {category: observation["category_counts"].get(category, 0) for category in
                                  ("defined_export", "undefined_reference", "local_defined", "nonvisible_defined",
                                   "absolute_version_label", "non_api_metadata", "null_symbol")}
    return result


def slice_inventory(zf, files, prefix, metadata, abi_name, requested, collected, expected_files=None):
    abi = metadata["meta/abis.json"][abi_name]
    profile = {"abi": abi_name, "abi_metadata": abi, **api_profile(requested, metadata["meta/platforms.json"], abi),
               "directory": None, "expected_system_libraries": None, "missing_system_libraries": None,
               "non_elf_system_libraries": None, "observed_library_count": None,
               "public_export_denominator": None, "observed_export_records": None,
               "libraries": None, "excluded_files": None}
    if profile["status"] != "available":
        return profile, {}
    if abi["arch"] not in ARCH_MACHINES:
        raise ValueError("unsupported_metadata_elf_architecture")
    relative = f"toolchains/llvm/prebuilt/{metadata['host_tag']}/sysroot/usr/lib/{abi['triple']}/{profile['effective_api']}/"
    profile["directory"] = relative
    selected = sorted(name for name in files if name.startswith(prefix + relative))
    if any("/" in name[len(prefix + relative):] for name in selected):
        raise ValueError("unsupported_nested_api_slice_files")
    expected = sorted(name for name, minimum in metadata["meta/system_libs.json"].items()
                      if int(minimum) <= profile["effective_api"])
    profile["expected_system_libraries"] = expected
    if not selected:
        profile["status"] = "missing_api_directory"
        profile["missing_system_libraries"] = expected
        return profile, {}
    libraries, exclusions, non_elf = {}, [], []
    for name in selected:
        path, filename = name[len(prefix):], name[len(prefix + relative):]
        data = zf.read(name)
        artifact = bytes_record(data)
        collected[path] = artifact
        if expected_files is not None and expected_files.get(path) != artifact:
            raise ValueError("selected_ndk_file_pin_mismatch")
        mode = files[name].external_attr >> 16
        if stat.S_ISLNK(mode):
            reason = "archive_symlink_not_resolved"
        elif filename in expected and data.startswith(b"\x7fELF"):
            parsed = elf.inventory(data)
            if parsed["header"]["elf_class"] != abi["bitness"] or parsed["header"]["machine"] != ARCH_MACHINES[abi["arch"]]:
                raise ValueError("elf_header_disagrees_with_abi_metadata")
            libraries[filename] = {"path": path, "artifact": artifact, "elf": parsed}
            continue
        elif filename.endswith((".so", ".a")) and data.lstrip().startswith((b"INPUT(", b"GROUP(")):
            reason = "linker_script_not_resolved"
        elif filename.endswith(".o"):
            reason = "crt_or_relocatable_object_not_a_platform_shared_stub"
        elif filename.endswith(".a"):
            reason = "static_archive_not_in_platform_shared_export_scope"
        elif filename not in expected:
            reason = "not_listed_as_platform_system_library"
        else:
            reason = "non_elf_system_library_not_interpreted"
        entry = {"path": path, "artifact": artifact, "reason": reason}
        if reason == "linker_script_not_resolved":
            entry["text"] = data.decode("utf-8")
        if filename in expected:
            non_elf.append(filename)
        exclusions.append(entry)
    present_names = {name[len(prefix + relative):] for name in selected}
    missing = sorted(set(expected) - present_names)
    profile.update({"status": "incomplete_library_set" if missing or non_elf else "available",
                    "missing_system_libraries": missing, "non_elf_system_libraries": sorted(non_elf),
                    "observed_library_count": len(libraries),
                    "observed_export_records": sum(item["elf"]["category_counts"].get("defined_export", 0) for item in libraries.values()),
                    "libraries": {name: library_summary(item) for name, item in sorted(libraries.items())},
                    "excluded_files": exclusions})
    if profile["status"] == "available":
        profile["public_export_denominator"] = sum(
            library["defined_export_identities"] for library in profile["libraries"].values())
    return profile, libraries


def collect(path, manifest):
    archive = archive_record(path)
    if any(archive[key] != manifest["archive"][key] for key in ("bytes", "sha1", "sha256")):
        raise ValueError("ndk_archive_pin_mismatch")
    repository = manifest["repository_package"]
    if (repository["checksum_type"] != "sha1" or repository["sha1"] != archive["sha1"] or repository["bytes"] != archive["bytes"]
            or not manifest["archive"]["url"].endswith("/" + repository["url"])):
        raise ValueError("repository_and_ndk_archive_pins_disagree")
    prefix = manifest["archive"]["member_root"].rstrip("/") + "/"
    compiler_prefix = f"toolchains/llvm/prebuilt/{manifest['host_tag']}/"
    paths = [*METADATA_PATHS, *(compiler_prefix + name for name in COMPILER_METADATA)]
    raw_metadata, metadata, collected, profiles, inventories = {}, {"host_tag": manifest["host_tag"]}, {}, {}, {}
    with zipfile.ZipFile(path) as zf:
        files = {entry.filename: entry for entry in zf.infolist() if not entry.is_dir()}
        if len(files) != sum(not entry.is_dir() for entry in zf.infolist()):
            raise ValueError("duplicate_ndk_archive_path")
        for relative in paths:
            data = zf.read(prefix + relative)
            text, artifact = data.decode("utf-8"), bytes_record(data)
            raw_metadata[relative] = {"text": text, **artifact}
            if raw_metadata[relative] != manifest["metadata_files"][relative]:
                raise ValueError("ndk_metadata_pin_mismatch")
            if relative.endswith(".json"):
                metadata[relative] = json.loads(text)
        if not metadata["meta/abis.json"]:
            raise ValueError("ndk_abi_metadata_required")
        props = properties(raw_metadata["source.properties"]["text"])
        if props["Pkg.Revision"] != repository["revision"]:
            raise ValueError("ndk_revision_metadata_mismatch")
        for abi in sorted(metadata["meta/abis.json"]):
            profiles[abi], inventories[abi] = {}, {}
            for role in ("before", "after"):
                profile, items = slice_inventory(zf, files, prefix, metadata, abi, manifest[role]["requested_api"], collected, manifest["selected_files"])
                profiles[abi][role], inventories[abi][role] = profile, items
    if collected != manifest["selected_files"]:
        raise ValueError("selected_ndk_file_set_mismatch")
    if archive_record(path) != archive:
        raise ValueError("ndk_archive_changed_during_inventory")
    return {"source_properties": props, "metadata_files": raw_metadata, "parsed_metadata": metadata,
            "profiles": profiles, "inventories": inventories}


def compare(archive_path, manifest):
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("unsupported_ndk_manifest_schema")
    data = collect(archive_path, manifest)
    rows, comparisons = [], {}
    for abi, profiles in data["profiles"].items():
        first, second = profiles["before"], profiles["after"]
        paired = first["status"] == second["status"] == "available"
        comparison = {"status": "paired" if paired else "unpaired_or_incomplete",
                      "before_status": first["status"], "after_status": second["status"],
                      "defined_exports": None, "library_files": None, "libraries": None}
        comparisons[abi] = comparison
        if not paired:
            continue
        before, after = (data["inventories"][abi][role] for role in ("before", "after"))
        comparison["library_files"] = delta({name: item["artifact"] for name, item in before.items()},
                                             {name: item["artifact"] for name, item in after.items()}, "library_file_bytes", {"abi": abi}, rows)
        libraries = {}
        aggregate = Counter({key: 0 for key in ("before", "after", "added", "removed", "changed", "unchanged", "before_records", "after_records")})
        for library in sorted(before.keys() | after.keys()):
            context = {"abi": abi, "library": library}
            old, new = before.get(library), after.get(library)
            result = {}
            categories = sorted({row["category"] for item in (old, new) if item for row in item["elf"]["symbols"]} - {"null_symbol"})
            for category in categories:
                maps = [symbol_groups(item["elf"]["symbols"], category) if item else {} for item in (old, new)]
                result[category] = delta(*maps, category, context, rows)
                result[category]["before_records"] = sum(map(len, maps[0].values()))
                result[category]["after_records"] = sum(map(len, maps[1].values()))
            aggregate.update(result.get("defined_export", {}))
            for category, key in (("elf_header", "header"), ("dynamic_names", "dynamic")):
                result[category] = delta({library: old["elf"][key]} if old else {},
                                         {library: new["elf"][key]} if new else {}, category, context, rows)
            for category, requirements in (("version_definitions", False), ("version_requirements", True)):
                result[category] = delta(named_versions(old["elf"][category], requirements) if old else {},
                                         named_versions(new["elf"][category], requirements) if new else {}, category, context, rows)
            libraries[library] = result
        comparison["defined_exports"], comparison["libraries"] = dict(sorted(aggregate.items())), libraries
    rows.sort(key=canonical)
    report = {
        "schema_version": SCHEMA_VERSION, "comparison_id": manifest["comparison_id"],
        "scope": "Static public platform-stub ELF export metadata; no runtime implementation or complete C/C++ ABI conclusion.",
        "archive": manifest["archive"], "repository_package": manifest["repository_package"],
        "repository_metadata_provenance": manifest["repository_metadata_provenance"],
        "project_source_context": manifest["project_source_context"], "manifest_sha256": fingerprint(manifest),
        "source_properties": data["source_properties"], "metadata_files": data["metadata_files"],
        "parsed_metadata": data["parsed_metadata"], "profiles": data["profiles"], "comparisons": comparisons,
        "unmeasured": UNMEASURED,
        "generator_files_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in (Path(__file__), Path(elf.__file__))},
        "row_count": len(rows), "delta_rows_sha256": hashlib.sha256("".join(canonical(row) + "\n" for row in rows).encode("ascii")).hexdigest(),
        "interpretation": [
            "Every ABI declared by this package's abis.json is considered; default=false and minimum OS versions are retained. Packaged ABI presence is not device/runtime qualification.",
            "Only complete, supported API slices are paired. An unavailable or incomplete baseline has a null denominator and no symbol delta; an API37-only profile is not counted as additions from zero.",
            "The measured library set comes from system_libs.json and the effective API. Every file in a selected slice is hashed, including excluded linker scripts, CRT objects and archives.",
            "Defined exports are named dynamic definitions with GLOBAL/WEAK/GNU_UNIQUE binding and DEFAULT/PROTECTED visibility. Undefined, local, hidden/internal, null and absolute version-label records have separate categories.",
            "Aliases retain symbol name, resolved version, version role and provider. Numeric version indices are kept in library metadata but not used as cross-file identities.",
            "The GNU version hidden bit controls default-version selection and is distinct from ELF visibility. Nondefault versions remain defined exports; unknown forms fail explicitly.",
            "Per-symbol object_size is raw st_size for OBJECT/TLS only. Neither that size nor symbol presence specifies a C/C++ type, value, call signature or working implementation.",
            "Version names, including deprecated or private-looking names, are preserved. No leading-underscore or private-name heuristic silently removes symbols; source/header publicness is not independently measured.",
            "Comparison before/after/changed counts are identity groups. before_records/after_records and library summaries also retain actual alias-record counts; duplicates are never overwritten.",
            "Library byte changes, version records and export changes overlap and must not be added together as independent APIs or defects.",
        ],
    }
    return report, rows


def markdown(report):
    sample = next(iter(report["profiles"].values()))
    release = report["source_properties"].get("Pkg.ReleaseName", report["source_properties"]["Pkg.Revision"])
    first, second = sample["before"]["requested_api"], sample["after"]["requested_api"]
    lines = [f"# NDK {release} API{first} to API{second} platform-stub comparison", "", report["scope"], "",
             "This is a bounded A07 evidence increment for issue #56. SDK artifact profiles, NDK/compiler identity and the project's AOSP source tag remain separate.", "",
             "## Measured profiles", "", f"| ABI | Metadata default | API{first} status / exports | API{second} status / exports | Export additions | Removals | Changed metadata |",
             "| --- | --- | --- | --- | ---: | ---: | ---: |"]
    for abi, pair in report["profiles"].items():
        display = []
        for role in ("before", "after"):
            item = pair[role]
            value = item["public_export_denominator"]
            display.append(f"{item['status']} / {value:,}" if value is not None else f"{item['status']} / null")
        delta_ = report["comparisons"][abi]["defined_exports"]
        changes = [str(delta_[key]) if delta_ is not None else "not compared" for key in ("added", "removed", "changed")]
        lines.append(f"| {abi} | {pair['after']['abi_metadata']['default']} | {display[0]} | {display[1]} | " + " | ".join(changes) + " |")
    lines += ["", "Counts describe defined, externally visible dynamic-symbol identities in the selected public platform stubs. Version aliases remain distinct. The full report also records alias-row counts, every selected library/hash, exclusions, version definitions/requirements, binding, visibility and object/TLS sizes.",
              "An unavailable before baseline is null, not zero. The after profile's observations are not called additions. No library or target code was executed.",
              "", "## Interpretation", ""]
    lines.extend(f"- {text}" for text in report["interpretation"])
    lines += ["", "## Explicitly unmeasured", ""]
    lines.extend(f"- **{key.replace('_', ' ')}:** {text}" for key, text in report["unmeasured"].items())
    lines += ["", "See [the tool guide](../../ndk-platform-diff.md) for acquisition, profile resolution, symbol identities, replay and validation limits.", ""]
    return "\n".join(lines)


def write_report(output, report, rows):
    output.mkdir(parents=True, exist_ok=True)
    # Report digests bind LF bytes on every host, including Windows.
    (output / "summary.json").write_text(json.dumps(report, sort_keys=True, indent=2) + "\n", encoding="utf-8", newline="\n")
    (output / "delta.jsonl").write_text("".join(canonical(row) + "\n" for row in rows), encoding="ascii", newline="\n")
    (output / "README.md").write_text(markdown(report), encoding="utf-8", newline="\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        report, rows = compare(args.archive, manifest)
        write_report(args.out_dir, report, rows)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"NDK platform comparison failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    print(f"Compared {report['comparison_id']}: {len(rows)} changed records; no runtime compatibility conclusion.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
