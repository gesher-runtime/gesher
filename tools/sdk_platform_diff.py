#!/usr/bin/env python3
"""Reproducible, offline comparison of two separately pinned SDK platforms.

SDK declarations and resource artifacts are evidence inputs, not runtime providers.
The existing JVM descriptor parser is deliberately unchanged. This tool does not
read constant values, signatures, annotations, exceptions, or method bodies.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

import java_member_inventory as java


SCHEMA_VERSION = 1
MEASURED_FILES = (
    "android.jar", "framework.aidl", "source.properties", "build.prop",
    "data/res/values/public-final.xml", "data/res/values/public-staging.xml",
)
UNMEASURED = {
    "java_constant_values": "ConstantValue attributes are not decoded.",
    "java_generics_annotations_exceptions":
        "Signature, annotation, annotation-default and Exceptions attributes are not decoded; data/annotations.zip is not compared.",
    "java_source_access_and_full_linkage":
        "InnerClasses source access, generic typing, protected receiver rules, class loaders, optional SDK jars and complete JVM linkage are not evaluated.",
    "binder_transactions_and_source_aidl":
        "framework.aidl is a preprocessed declaration list; source methods, transaction IDs, parcel layouts and Binder behavior are not inputs.",
    "compiled_and_runtime_resources":
        "XML IDs and file hashes do not establish compiled resource tables, overlays, device configuration or runtime provisioning.",
    "ndk_exports": "No separately pinned NDK sysroot/export artifacts are inputs.",
    "source_compat_changes": "No separately pinned source CompatChanges inventory is an input.",
    "cts_cdd_and_runtime_behavior": "No CTS/CDD qualification, runtime API37 execution, services, permissions or behavior is measured.",
    "aosp_module_revision_alignment":
        "An SDK API/extension/revision does not identify the project's AOSP tag or individual module commits.",
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


def properties(data):
    """The selected SDK files use plain key=value properties, without escapes."""
    result = {}
    for line in data.decode("utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith(("#", "!")):
            continue
        if "=" not in line or "\\" in line:
            raise ValueError("unsupported_properties_form")
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip()
        if not key or key in result:
            raise ValueError("duplicate_or_empty_property")
        result[key] = value
    return result


def sdk_profile(props, repository):
    """Keep raw package fields, absent fields and channel evidence distinct."""
    api = props["AndroidVersion.ApiLevel"]
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)?", api):
        raise ValueError("unsupported_sdk_api_level")
    major, _, minor = api.partition(".")
    revision = props["Pkg.Revision"]
    extension = props.get("AndroidVersion.ExtensionLevel")
    base_sdk = props.get("AndroidVersion.IsBaseSdk")
    if base_sdk not in (None, "true", "false"):
        raise ValueError("unsupported_base_sdk_property")
    if (api != repository["api_level_raw"] or revision != repository["revision"]
            or extension != repository.get("extension_level_raw")
            or base_sdk != repository.get("base_extension_raw")):
        raise ValueError("repository_and_source_properties_disagree")
    return {
        "api_level_raw": api, "api_major": int(major), "api_minor": int(minor or "0"),
        "api_minor_explicit_in_source_properties": bool(minor),
        "extension_level": int(extension) if extension is not None else None,
        "base_sdk": base_sdk == "true" if base_sdk is not None else None,
        "sdk_package_revision": revision,
        "platform_version": props.get("Platform.Version"),
        "platform_codename": props.get("Platform.CodeName"),
        "android_codename": props.get("AndroidVersion.CodeName"),
        "preview_sdk_int": props.get("AndroidVersion.PreviewSdkInt"),
        "beta_version": props.get("AndroidVersion.BetaVersion"),
        "repository_channel": repository["channel"],
        "repository_codename": repository.get("codename"),
        "release_classification_basis":
            "Repository channel and observed codename/preview fields are reported separately; absent properties remain null.",
        "source_properties": props,
    }


def parse_aidl(data):
    """Only preprocessed declarations with marker annotations; no source AIDL."""
    result = {}
    identifier = r"[A-Za-z_$][A-Za-z0-9_$]*(?:\.[A-Za-z_$][A-Za-z0-9_$]*)*"
    pattern = re.compile(rf"((?:@{identifier}\s+)*)(parcelable|interface)\s+({identifier})\s*;")
    for raw in data.decode("utf-8").splitlines():
        line = raw.strip()
        if not line:
            continue
        match = pattern.fullmatch(line)
        if not match:
            raise ValueError("unsupported_preprocessed_aidl_form")
        annotations, kind, name = match.groups()
        key = f"{kind} {name}"
        if key in result:
            raise ValueError("duplicate_preprocessed_aidl_declaration")
        # Preserve order as encoded; even a reordered list is a textual metadata delta.
        result[key] = {"kind": kind, "name": name, "annotations": annotations.split()}
    return result


def resource_id(text):
    if not re.fullmatch(r"0x[0-9a-fA-F]{1,8}", text):
        raise ValueError("unsupported_resource_id")
    return int(text, 16)


def parse_resources(final_data, staging_data):
    """Keep final IDs, staging slots, and finalized staging alias slots separate.

    Every <public> slot consumes an ID, including repeated removed_ placeholders.
    Empty groups are retained. Unrecognized XML forms fail rather than disappear.
    Comments (including source API annotations) are not interpreted.
    """
    final, staging, aliases, groups = {}, {}, {}, {}

    def insert(target, key, value):
        if key in target:
            raise ValueError("duplicate_resource_key")
        target[key] = value

    for filename, data in (("public-final.xml", final_data), ("public-staging.xml", staging_data)):
        root = ET.fromstring(data)
        if root.tag != "resources" or root.attrib or (root.text or "").strip():
            raise ValueError("unsupported_resource_root")
        for node in root:
            if (node.tail or "").strip():
                raise ValueError("unsupported_resource_text")
            if node.tag in ("eat-comment", "skip") and not node.attrib and not list(node):
                continue
            if node.tag == "public" and filename == "public-final.xml":
                if set(node.attrib) != {"type", "name", "id"} or list(node) or (node.text or "").strip():
                    raise ValueError("unsupported_final_public_form")
                kind, name = node.attrib["type"], node.attrib["name"]
                insert(final, f"{kind}/{name}", {"type": kind, "name": name,
                       "id": f"0x{resource_id(node.attrib['id']):08x}"})
                continue
            expected = "staging-public-group-final" if filename == "public-final.xml" else "staging-public-group"
            if node.tag != expected or set(node.attrib) != {"type", "first-id"} or (node.text or "").strip():
                raise ValueError("unsupported_resource_group_form")
            first = resource_id(node.attrib["first-id"])
            kind = node.attrib["type"]
            target = aliases if filename == "public-final.xml" else staging
            count = 0
            for child in node:
                if (child.tail or "").strip():
                    raise ValueError("unsupported_resource_group_text")
                if child.tag in ("eat-comment", "skip") and not child.attrib and not list(child):
                    continue
                if child.tag != "public" or set(child.attrib) != {"name"} or list(child) or (child.text or "").strip():
                    raise ValueError("unsupported_resource_slot_form")
                name = child.attrib["name"]
                value = first + count
                if value > 0xffffffff or value >> 16 != first >> 16:
                    raise ValueError("resource_group_id_overflow")
                key = f"0x{value:08x}"
                insert(target, key, {"type": kind, "name": name, "id": key,
                                    "removed_placeholder": name.startswith("removed_")})
                count += 1
            insert(groups, f"{node.tag}/{kind}/0x{first:08x}",
                   {"kind": node.tag, "type": kind, "first_id": f"0x{first:08x}", "slot_count": count})
    active_names = {}
    for item in staging.values():
        if not item["removed_placeholder"]:
            insert(active_names, f"{item['type']}/{item['name']}", item)
    for item in aliases.values():
        target = final.get(f"{item['type']}/{item['name']}")
        item["final_id"] = target["id"] if target and not item["removed_placeholder"] else None
    return {"final_public_ids": final, "active_staging_names": active_names,
            "staging_slots": staging, "finalized_staging_alias_slots": aliases, "groups": groups}


def exposed(item):
    return bool(item.flags & (java.PUBLIC | java.PROTECTED)) and item.name != "<clinit>"


def header(cls):
    return {"access_flags": cls.flags, "superclass": cls.superclass, "interfaces": list(cls.interfaces)}


def member_key(owner, item):
    return owner, item.kind, item.name, item.descriptor


def member_identity(key):
    owner, kind, name, descriptor = key
    return {"class": owner, "kind": "constructor" if name == "<init>" else kind,
            "name": name, "descriptor": descriptor}


def lookup(before_class, item, after, resolver):
    if before_class.name not in after:
        return {"status": "declaring_class_absent"}
    found, unknown = resolver.lookup(before_class.name, item.key)
    if not found:
        result = {"status": "unresolved_hierarchy" if unknown else "descriptor_not_found"}
    else:
        differences = sorted({issue for _, candidate in found for issue in java.flag_issues(item, candidate)})
        result = {
            "status": "resolution_uncertain" if unknown or len(found) > 1 else "descriptor_found",
            "location": "declared" if any(owner == before_class.name for owner, _ in found) else "inherited",
            "candidates": [{"class": owner, "access_flags": candidate.flags} for owner, candidate in sorted(found)],
            "possible_flag_differences": differences,
        }
    if unknown:
        result["unresolved_parents"] = sorted(resolver.ancestry(before_class.name)[1])
    return result


def mapping_delta(before, after, category, rows):
    counts = {"before": len(before), "after": len(after), "added": 0, "removed": 0, "changed": 0, "unchanged": 0}
    for key in sorted(before.keys() | after.keys()):
        old, new = before.get(key), after.get(key)
        change = "added" if old is None else "removed" if new is None else "changed" if old != new else "unchanged"
        counts[change] += 1
        if change != "unchanged":
            rows.append({"category": category, "change": change, "key": key, "before": old, "after": new})
    return counts


def java_delta(before, after, rows):
    result = {"encoded_class_definitions": mapping_delta(
        {name: header(cls) for name, cls in before.items()},
        {name: header(cls) for name, cls in after.items()}, "java_class_header", rows)}
    old = {member_key(name, item): item for name, cls in before.items() for item in cls.members.values() if exposed(item)}
    new = {member_key(name, item): item for name, cls in after.items() for item in cls.members.values() if exposed(item)}
    counts = {"before": len(old), "after": len(new), "added": 0, "removed": 0, "flags_changed": 0, "unchanged": 0}
    resolver = java.Resolver(after)
    # Validate every hierarchy, including classes with no public/protected members.
    for name in sorted(after):
        resolver.ancestry(name)
    lookup_counts, lookup_locations, lookup_flag_counts = Counter(), Counter(), Counter()
    for key in sorted(old.keys() | new.keys()):
        first, second = old.get(key), new.get(key)
        change = "added" if first is None else "removed" if second is None else "flags_changed" if first.flags != second.flags else "unchanged"
        counts[change] += 1
        outcome = None
        if first is not None:
            outcome = lookup(before[key[0]], first, after, resolver)
            lookup_counts[outcome["status"]] += 1
            if "location" in outcome:
                lookup_locations[outcome["location"]] += 1
            lookup_flag_counts.update(outcome.get("possible_flag_differences", []))
        if change != "unchanged":
            row = {"category": "java_member_declaration", "change": change, **member_identity(key),
                   "before_access_flags": first.flags if first else None,
                   "after_access_flags": second.flags if second else None}
            if outcome is not None:
                row["target_lookup"] = outcome
            rows.append(row)
    result["public_protected_declarations"] = counts
    result["before_declaration_target_lookup"] = {
        "denominator": len(old), "status_counts": dict(sorted(lookup_counts.items())),
        "location_counts": dict(sorted(lookup_locations.items())),
        "possible_flag_difference_counts": dict(sorted(lookup_flag_counts.items())),
    }
    result["encoded_classfile_visibility"] = {
        role: dict(sorted(Counter("public" if cls.flags & java.PUBLIC else "other" for cls in classes.values()).items()))
        for role, classes in (("before", before), ("after", after))
    }
    result["declaration_kinds"] = {
        role: dict(sorted(Counter("constructor" if item.name == "<init>" else item.kind for item in items.values()).items()))
        for role, items in (("before", old), ("after", new))
    }
    return result


def platform(path, pin):
    archive = archive_record(path)
    if any(archive[key] != pin["archive"][key] for key in ("bytes", "sha1", "sha256")):
        raise ValueError("platform_archive_pin_mismatch")
    complete = pin["repository_package"]["complete_archive"]
    if (complete["checksum_type"] != "sha1" or archive["sha1"] != complete["checksum"]
            or archive["bytes"] != complete["bytes"]
            or not pin["archive"]["url"].endswith("/" + complete["url"])):
        raise ValueError("repository_and_archive_pins_disagree")
    root = pin["archive"]["member_root"].rstrip("/") + "/"
    with zipfile.ZipFile(path) as zf:
        files = {info.filename: info for info in zf.infolist() if not info.is_dir()}
        if len(files) != sum(not info.is_dir() for info in zf.infolist()):
            raise ValueError("duplicate_platform_archive_entry")
        if any(not name.startswith(root) for name in files):
            raise ValueError("unexpected_platform_archive_root")
        blobs = {name: zf.read(root + name) for name in MEASURED_FILES}
        artifacts = {name: bytes_record(data) for name, data in blobs.items()}
        if artifacts != pin["artifacts"]:
            raise ValueError("platform_artifact_pin_mismatch")
        props = properties(blobs["source.properties"])
        if props != pin["source_properties"]:
            raise ValueError("source_properties_pin_mismatch")
        profile = sdk_profile(props, pin["repository_package"])
        resource_files = {}
        for name in sorted(files):
            relative = name[len(root):]
            if relative.startswith("data/res/"):
                resource_files[relative] = bytes_record(zf.read(name))
    if archive_record(path) != archive:
        raise ValueError("platform_archive_changed_during_comparison")
    with tempfile.TemporaryDirectory(prefix="sdk-platform-diff-") as temporary:
        jar = Path(temporary) / "android.jar"
        jar.write_bytes(blobs["android.jar"])
        classes, java_manifest = java.archive_members(jar, "sdk", pin["id"])
    profile.update({
        "id": pin["id"], "package_path": pin["package_path"], "archive": pin["archive"],
        "repository_package": pin["repository_package"], "artifacts": artifacts,
        "archive_file_count": len(files), "java_inventory": java_manifest,
        "resource_file_count": len(resource_files),
        "resource_file_total_bytes": sum(item["bytes"] for item in resource_files.values()),
        "resource_file_inventory_sha256": fingerprint(resource_files),
    })
    return {"profile": profile, "classes": classes, "aidl": parse_aidl(blobs["framework.aidl"]),
            "resources": parse_resources(blobs["data/res/values/public-final.xml"], blobs["data/res/values/public-staging.xml"]),
            "resource_files": resource_files}


def compare(before_path, after_path, manifest):
    if manifest.get("schema_version") != SCHEMA_VERSION or manifest["before"]["id"] == manifest["after"]["id"]:
        raise ValueError("invalid_comparison_manifest")
    before, after = platform(before_path, manifest["before"]), platform(after_path, manifest["after"])
    rows = []
    java_summary = java_delta(before["classes"], after["classes"], rows)
    aidl_summary = mapping_delta(before["aidl"], after["aidl"], "preprocessed_aidl_declaration", rows)
    resource_summary = {
        category: mapping_delta(before["resources"][category], after["resources"][category], f"resource_{category}", rows)
        for category in before["resources"]
    }
    resource_summary["files"] = mapping_delta(before["resource_files"], after["resource_files"], "resource_file_bytes", rows)
    resource_summary["slot_classification"] = {
        role: {
            category: {
                "removed_placeholders": sum(item["removed_placeholder"] for item in value["resources"][category].values()),
                "named_slots": sum(not item["removed_placeholder"] for item in value["resources"][category].values()),
            } for category in ("staging_slots", "finalized_staging_alias_slots")
        } for role, value in (("before", before), ("after", after))
    }
    report = {
        "schema_version": SCHEMA_VERSION, "comparison_id": manifest["comparison_id"],
        "scope": "Offline SDK platform artifact changes; no runtime compatibility conclusion.",
        "manifest_sha256": fingerprint(manifest),
        "repository_metadata_provenance": manifest["repository_metadata_provenance"],
        "project_source_context": manifest["project_source_context"],
        "profiles": {"before": before["profile"], "after": after["profile"]},
        "java": java_summary, "preprocessed_aidl": aidl_summary, "resources": resource_summary,
        "unmeasured": UNMEASURED,
        "generator_files_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                   for path in (Path(__file__), Path(java.__file__), Path(java.base.__file__))},
        "row_count": len(rows),
        "interpretation": [
            "Encoded classes include all classfile visibilities. Public/protected declarations are counted once on their encoded declaring class; inherited members are not expanded for every subclass.",
            "A declaration removed from one class can resolve in a superclass/interface. Removed rows include conservative lookup in the after SDK, with unresolved and ambiguous outcomes retained.",
            "Descriptor presence and flag differences do not establish binary or source compatibility. Classfile flags are raw JVM flags, not reconstructed InnerClasses source modifiers.",
            "All measured before declarations form the target-lookup denominator, independently of whether a changed row is emitted.",
            "Final public XML entries include declarations marked hidden/system in comments. Comments and resource annotations are not interpreted as a public SDK API filter.",
            "Active staging names exclude removed_ placeholders; slot inventories include every allocated placeholder. Finalized staging alias slots retain their old IDs and a final ID matched by type/name when present.",
            "Resource file changes compare complete bytes under data/res; hash changes alone are not semantic changes. Archives pin other files but those files are not interpreted.",
            "No aggregate coverage percentage combines Java, preprocessed AIDL, resources, NDK, CompatChanges or behavior. Unmeasured dimensions have no denominator.",
        ],
    }
    rows.sort(key=canonical)
    report["delta_rows_sha256"] = hashlib.sha256("".join(canonical(row) + "\n" for row in rows).encode("ascii")).hexdigest()
    return report, rows


def markdown(report):
    before, after = (report["profiles"][role] for role in ("before", "after"))
    lines = [f"# SDK platform artifact comparison: {before['api_level_raw']} to {after['api_level_raw']}", "", report["scope"], "",
             "This report measures artifact changes. It does not complete A07 or change Gesher's API level.", "",
             "## Separately pinned profiles", "", "| Field | Before | After |", "| --- | --- | --- |"]
    for label, key in (("Package", "package_path"), ("API (raw)", "api_level_raw"),
                       ("Extension", "extension_level"), ("SDK package revision", "sdk_package_revision"),
                       ("PreviewSdkInt (absent stays null)", "preview_sdk_int")):
        values = ["absent (null)" if profile[key] is None else str(profile[key]) for profile in (before, after)]
        lines.append(f"| {label} | {values[0]} | {values[1]} |")
    lines.append(f"| Repository channel | {before['repository_channel']['name']} | {after['repository_channel']['name']} |")
    lines += ["", "The complete profiles, observed properties, input hashes and generator hashes are in `summary.json`. The archive pins and official repository evidence are in `manifest.json`.",
              "The project's `aosp.lock` tag is a separate source profile; neither SDK archive is claimed to equal that tag's module revisions.", "",
              "## Measured changes", "", "| Dimension | Before | After | Added | Removed | Changed | Unchanged |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    entries = [("Encoded class headers", report["java"]["encoded_class_definitions"]),
               ("Public/protected declarations", report["java"]["public_protected_declarations"]),
               ("Preprocessed AIDL declarations", report["preprocessed_aidl"])]
    entries += [(name.replace("_", " "), report["resources"][name]) for name in
                ("final_public_ids", "active_staging_names", "staging_slots", "finalized_staging_alias_slots", "groups", "files")]
    for label, value in entries:
        numbers = [value[key] for key in ("before", "after", "added", "removed")]
        numbers += [value.get("changed", value.get("flags_changed", 0)), value["unchanged"]]
        lines.append("| " + label + " | " + " | ".join(f"{number:,}" for number in numbers) + " |")
    lines += ["", "Changed means differing encoded headers, raw member access flags, annotation lists, ID/slot/group records, or file bytes in the respective row. Added/removed declarations are membership in the public/protected declared set, including visibility transitions.",
              "Resource views overlap: active names, slots, alias slots and file hashes are separate descriptions, not additive API counts.", "",
              "## Lookup of every before declaration in the after SDK", "", "| Outcome | Count |", "| --- | ---: |"]
    for status, count in report["java"]["before_declaration_target_lookup"]["status_counts"].items():
        lines.append(f"| {status} | {count:,} |")
    locations = report["java"]["before_declaration_target_lookup"]["location_counts"]
    lines += ["", f"Found descriptor locations: {locations.get('declared', 0):,} declared, {locations.get('inherited', 0):,} inherited.",
              "Removed declarations carry lookup candidates in `delta.jsonl`; inherited descriptor matches remain visible. A removed declaration is not automatically a binary incompatibility. This lookup uses SDK stubs and does not test runtime linkage.",
              "", "## Interpretation", ""]
    lines.extend(f"- {text}" for text in report["interpretation"])
    lines += ["", "## Explicitly unmeasured", ""]
    lines.extend(f"- **{key.replace('_', ' ')}:** {text}" for key, text in report["unmeasured"].items())
    lines += ["", "See [the tool guide](../../sdk-platform-diff.md) for acquisition, commands, categories, evidence boundaries and follow-up work.", ""]
    return "\n".join(lines)


def write_report(output, report, rows):
    output.mkdir(parents=True, exist_ok=True)
    # The report hashes exact LF bytes; platform text translation must not
    # alter those bytes on Windows.
    (output / "summary.json").write_text(json.dumps(report, sort_keys=True, indent=2) + "\n", encoding="utf-8", newline="\n")
    (output / "delta.jsonl").write_text("".join(canonical(row) + "\n" for row in rows), encoding="ascii", newline="\n")
    (output / "README.md").write_text(markdown(report), encoding="utf-8", newline="\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--before-archive", required=True, type=Path)
    parser.add_argument("--after-archive", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        report, rows = compare(args.before_archive, args.after_archive, manifest)
        write_report(args.out_dir, report, rows)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, ET.ParseError) as error:
        print(f"SDK platform comparison failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    print(f"Compared {report['comparison_id']}: {len(rows)} changed records; no runtime compatibility conclusion.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
