#!/usr/bin/env python3
"""Compare pinned SDK API-history XML, without treating history as availability.

This reader retains explicit metadata and its absences. It does not expand
parent defaults, resolve a module label to a provider, or execute Android code.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET
import zipfile

import sdk_platform_diff as platform


SCHEMA_VERSION = 1
KINDS = ("class", "method", "field", "extends", "implements")
VERSION_ATTRIBUTES = ("since", "deprecated", "removed")
RECORD_ATTRIBUTES = {"name", "since", "deprecated", "removed", "sdks", "module", "jar"}
MEASURED_FILES = ("source.properties", "data/api-versions.xml")
MAX_XML_BYTES = 32 << 20
SCOPE = "Explicit SDK API-history metadata; not current declarations, installed providers, or runtime compatibility."
LIMITS = [
    "A historical record can describe an API removed before either selected SDK; record additions are not proof of API introduction in the later release.",
    "An omitted since/deprecated/removed/sdks attribute is retained as absent. Parent metadata is not expanded; absent does not mean zero, unsupported, or never deprecated.",
    "Module and jar labels are verbatim metadata. They do not identify a Gesher class loader, installed Mainline module, provider revision, or runtime capability.",
    "SDK extension catalog entries and sdks membership versions are metadata, not observations of SdkExtensions.getExtensionVersion on a device.",
    "Method identity includes the complete XML name/descriptor. Fields and inheritance edges retain their encoded identities; JVM access, linkage and behavior are not tested.",
    "The selected SDK packages, their XML schema versions, platform major/minor versions, extension levels and the project's AOSP source tag are distinct profiles.",
]


def version(raw, schema):
    """Preserve an absent or explicit minor part; never round through float."""
    if raw is None:
        return None
    expression = r"(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*))?" if schema == "4" else r"(?:0|[1-9][0-9]*)"
    if not re.fullmatch(expression, raw):
        raise ValueError("unsupported_api_history_version: " + repr(raw))
    major, separator, minor = raw.partition(".")
    return {"raw": raw, "major": int(major), "minor": int(minor) if separator else None,
            "minor_explicit": bool(separator)}


def sdk_memberships(raw, schema):
    if raw is None:
        return None
    if raw == "":
        return []
    result, seen = [], set()
    for item in raw.split(","):
        pieces = item.split(":")
        if len(pieces) != 2 or not re.fullmatch(r"0|[1-9][0-9]*", pieces[0]):
            raise ValueError("unsupported_api_history_sdk_membership: " + repr(item))
        sdk_id, raw_version = pieces
        if sdk_id in seen:
            raise ValueError("duplicate_api_history_sdk_membership: " + sdk_id)
        seen.add(sdk_id)
        if sdk_id == "0":
            decoded = {"platform_version": version(raw_version, schema), "extension_version": None}
        else:
            if not re.fullmatch(r"0|[1-9][0-9]*", raw_version):
                raise ValueError("unsupported_extension_version: " + repr(raw_version))
            decoded = {"platform_version": None, "extension_version": int(raw_version)}
        result.append({"sdk_id_raw": sdk_id, "version_raw": raw_version, **decoded})
    return result


def element(node, allowed, required=(), leaf=False):
    if set(node.attrib) - allowed or not set(required).issubset(node.attrib):
        raise ValueError("unsupported_api_history_attributes: " + node.tag)
    if (node.text or "").strip() or (node.tail or "").strip() or (leaf and len(node)):
        raise ValueError("unsupported_api_history_content: " + node.tag)
    if "name" in node.attrib and not node.attrib["name"]:
        raise ValueError("empty_api_history_name: " + node.tag)


def insert(mapping, key, value):
    if key in mapping:
        raise ValueError("duplicate_api_history_identity: " + repr(key))
    mapping[key] = dict(value)


def parse_history(data):
    if len(data) > MAX_XML_BYTES:
        raise ValueError("api_history_xml_exceeds_32_MiB_scope")
    root = ET.fromstring(data)
    if root.tag != "api" or root.attrib.get("version") not in ("3", "4"):
        raise ValueError("unsupported_api_history_root_or_schema")
    element(root, {"version", "min"}, {"version"})
    schema = root.attrib["version"]
    version(root.attrib.get("min"), schema)
    catalog = {}
    records = {kind: {} for kind in KINDS}

    def record(kind, key, node):
        element(node, RECORD_ATTRIBUTES, {"name"}, leaf=kind != "class")
        for attr in VERSION_ATTRIBUTES:
            version(node.attrib.get(attr), schema)
        sdk_memberships(node.attrib.get("sdks"), schema)
        insert(records[kind], key, node.attrib)

    for node in root:
        if node.tag == "sdk":
            element(node, {"id", "shortname", "name", "reference"}, {"id"}, leaf=True)
            identifier = node.attrib["id"]
            if not re.fullmatch(r"[1-9][0-9]*", identifier):
                raise ValueError("unsupported_extension_catalog_id")
            insert(catalog, identifier, node.attrib)
        elif node.tag == "class":
            name = node.attrib.get("name")
            record("class", (name,), node)
            for child in node:
                if child.tag not in KINDS[1:]:
                    raise ValueError("unsupported_api_history_class_child: " + child.tag)
                record(child.tag, (name, child.attrib.get("name")), child)
        else:
            raise ValueError("unsupported_api_history_root_child: " + node.tag)
    return {"root": dict(root.attrib), "catalog": catalog, "records": records}


def describe_record(attrs, schema):
    if attrs is None:
        return None
    return {"attributes": attrs,
            "version_components": {key: version(attrs.get(key), schema) for key in VERSION_ATTRIBUTES},
            "sdk_memberships": sdk_memberships(attrs.get("sdks"), schema)}


def observations(history):
    schema = history["root"]["version"]
    result = {"root_attributes": history["root"], "minimum_version": version(history["root"].get("min"), schema),
              "extension_catalog": history["catalog"], "records_by_kind": {}, "explicit_metadata": {}}
    unresolved = Counter()
    for kind, records in history["records"].items():
        result["records_by_kind"][kind] = len(records)
        versions = {key: Counter() for key in VERSION_ATTRIBUTES}
        modules, jars, sdk_ids, platform_versions = Counter(), Counter(), Counter(), Counter()
        presence = Counter()
        for attrs in records.values():
            for key in VERSION_ATTRIBUTES:
                if key in attrs:
                    versions[key][attrs[key]] += 1
            for key, target in (("module", modules), ("jar", jars)):
                if key in attrs:
                    target[attrs[key]] += 1
            if "sdks" in attrs:
                presence["present"] += 1
                if not attrs["sdks"]:
                    presence["empty"] += 1
                for member in sdk_memberships(attrs["sdks"], schema):
                    identifier = member["sdk_id_raw"]
                    sdk_ids[identifier] += 1
                    if identifier == "0":
                        platform_versions[member["version_raw"]] += 1
                    elif identifier not in history["catalog"]:
                        unresolved[identifier] += 1
        result["explicit_metadata"][kind] = {
            "versions": {key: {"raw_values": dict(sorted(values.items())), "present": sum(values.values()),
                               "absent": len(records) - sum(values.values()),
                               "explicit_minor": sum(count for raw, count in values.items() if "." in raw)}
                         for key, values in versions.items()},
            "module_labels": dict(sorted(modules.items())), "module_absent": len(records) - sum(modules.values()),
            "jar_labels": dict(sorted(jars.items())), "jar_absent": len(records) - sum(jars.values()),
            "sdks": {"present": presence["present"], "absent": len(records) - presence["present"],
                     "empty": presence["empty"], "membership_id_counts": dict(sorted(sdk_ids.items())),
                     "platform_version_raw_counts": dict(sorted(platform_versions.items()))},
        }
    result["unresolved_extension_catalog_memberships"] = dict(sorted(unresolved.items()))
    return result


def load_archive(path, pin):
    receipt = platform.archive_record(path)
    if any(receipt[key] != pin["archive"][key] for key in ("bytes", "sha1", "sha256")):
        raise ValueError("platform_archive_pin_mismatch")
    official = pin["repository_package"]["complete_archive"]
    if (official["checksum_type"] != "sha1" or receipt["sha1"] != official["checksum"]
            or receipt["bytes"] != official["bytes"]
            or not pin["archive"]["url"].endswith("/" + official["url"])):
        raise ValueError("repository_and_archive_pins_disagree")
    prefix = pin["archive"]["member_root"].rstrip("/") + "/"
    with zipfile.ZipFile(path) as archive:
        names = [info.filename for info in archive.infolist() if not info.is_dir()]
        if len(names) != len(set(names)) or any(not name.startswith(prefix) for name in names):
            raise ValueError("ambiguous_platform_archive_layout")
        if archive.getinfo(prefix + "data/api-versions.xml").file_size > MAX_XML_BYTES:
            raise ValueError("api_history_xml_exceeds_32_MiB_scope")
        data = {name: archive.read(prefix + name) for name in MEASURED_FILES}
    artifacts = {name: platform.bytes_record(value) for name, value in data.items()}
    if artifacts != pin["artifacts"]:
        raise ValueError("api_history_artifact_pin_mismatch")
    props = platform.properties(data["source.properties"])
    if props != pin["source_properties"]:
        raise ValueError("source_properties_pin_mismatch")
    profile = platform.sdk_profile(props, pin["repository_package"])
    if platform.archive_record(path) != receipt:
        raise ValueError("platform_archive_changed_during_comparison")
    history = parse_history(data["data/api-versions.xml"])
    profile.update({"id": pin["id"], "package_path": pin["package_path"], "archive": pin["archive"],
                    "artifacts": artifacts, "history": observations(history)})
    return profile, history


def mapping_delta(category, before, after, rows, transform=lambda value, role: value):
    old_keys, new_keys = set(before), set(after)
    changed = {key for key in old_keys & new_keys if before[key] != after[key]}
    for key in sorted(old_keys ^ new_keys | changed):
        old, new = before.get(key), after.get(key)
        rows.append({"category": category, "key": list(key) if isinstance(key, tuple) else key,
                     "change": "added" if key not in old_keys else "removed" if key not in new_keys else "changed",
                     "before": transform(old, "before") if old is not None else None,
                     "after": transform(new, "after") if new is not None else None})
    return {"before": len(before), "after": len(after), "added": len(new_keys - old_keys),
            "removed": len(old_keys - new_keys), "changed": len(changed),
            "unchanged": len(old_keys & new_keys) - len(changed)}


def compare(before_path, after_path, manifest):
    if manifest.get("schema_version") != SCHEMA_VERSION or manifest["before"]["id"] == manifest["after"]["id"]:
        raise ValueError("unsupported_history_manifest")
    profiles, histories = {}, {}
    for role, path in (("before", before_path), ("after", after_path)):
        profiles[role], histories[role] = load_archive(path, manifest[role])
    rows = []
    dimensions = {
        "root_metadata": mapping_delta("root_metadata", {"api": histories["before"]["root"]},
                                       {"api": histories["after"]["root"]}, rows),
        "extension_catalog": mapping_delta("extension_catalog", histories["before"]["catalog"],
                                            histories["after"]["catalog"], rows),
    }
    for kind in KINDS:
        dimensions[kind] = mapping_delta(kind, histories["before"]["records"][kind],
            histories["after"]["records"][kind], rows,
            lambda value, role: describe_record(value, histories[role]["root"]["version"]))
    encoded_rows = "".join(platform.canonical(row) + "\n" for row in rows).encode("ascii")
    report = {"schema_version": SCHEMA_VERSION, "comparison_id": manifest["comparison_id"],
        "scope": SCOPE, "profiles": profiles, "dimensions": dimensions, "interpretation": LIMITS,
        "manifest_sha256": platform.fingerprint(manifest),
        "repository_metadata_provenance": manifest["repository_metadata_provenance"],
        "source_platform_manifest_sha256": manifest["source_platform_manifest_sha256"],
        "generator_sha256": {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                             for name in ("sdk_api_history_diff.py", "sdk_platform_diff.py")},
        "delta_rows": len(rows), "delta_rows_sha256": hashlib.sha256(encoded_rows).hexdigest(),
        "runtime_sdk_int_changed": False, "a07_complete": False}
    return report, rows


def markdown(report):
    lines = ["# SDK36 to SDK37 API-history metadata", "", report["scope"], "",
             "## Measured records", "", "| Dimension | Before | After | Added | Removed | Changed | Unchanged |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for kind, values in report["dimensions"].items():
        lines.append("| " + kind + " | " + " | ".join(f"{values[key]:,}" for key in
            ("before", "after", "added", "removed", "changed", "unchanged")) + " |")
    lines += ["", "These are historical XML identities and explicit metadata changes, not live API counts.", "",
              "## Input profiles", "", "| Field | Before | After |", "| --- | --- | --- |"]
    for label, getter in (("Package", lambda p: p["package_path"]),
                          ("API property (raw)", lambda p: p["api_level_raw"]),
                          ("XML schema", lambda p: p["history"]["root_attributes"]["version"]),
                          ("XML bytes", lambda p: p["artifacts"]["data/api-versions.xml"]["bytes"]),
                          ("XML SHA-256", lambda p: p["artifacts"]["data/api-versions.xml"]["sha256"])):
        lines.append(f"| {label} | {getter(report['profiles']['before'])} | {getter(report['profiles']['after'])} |")
    lines += ["", "Both XML files and source.properties are read from fully pinned official platform archives.",
              "The input manifest retains archive hashes, selected repository package records, and metadata provenance.",
              "", "## Interpretation", ""]
    lines.extend("- " + text for text in report["interpretation"])
    lines += ["", "Each dimension obeys before = removed + changed + unchanged and after = added + changed + unchanged.",
              "No compatibility percentage is calculated. Issue #56 and A07 remain open.", "",
              "See [the tool guide](../../sdk-api-history-diff.md) for reproduction, metadata absence and version handling.", ""]
    return "\n".join(lines)


def write_report(output, report, rows):
    output.mkdir(parents=True, exist_ok=True)
    (output / "summary.json").write_text(json.dumps(report, sort_keys=True, indent=2) + "\n", encoding="utf-8", newline="\n")
    (output / "delta.jsonl").write_text("".join(platform.canonical(row) + "\n" for row in rows), encoding="ascii", newline="\n")
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
        print(f"SDK API-history comparison failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    print(f"Compared {report['comparison_id']}: {len(rows)} historical metadata changes; no runtime qualification.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
