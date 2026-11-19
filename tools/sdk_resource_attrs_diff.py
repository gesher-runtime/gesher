#!/usr/bin/env python3
"""Compare explicit attribute/styleable metadata in pinned SDK attrs*.xml files.

The complete default-values attrs*.xml family is selected. This is an inventory
of source metadata, not resource compilation, public API or runtime support.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import sys
import xml.etree.ElementTree as ET
import zipfile

import sdk_platform_diff as platform


SCHEMA_VERSION = 1
MAX_XML_BYTES = 32 << 20
CATEGORIES = ("resource_roots", "styleables", "attributes", "symbols")
SCOPE = "Explicit SDK attribute/styleable source metadata; not compiled resources, public API availability, or runtime compatibility."
LIMITS = [
    "Declarations are scoped XML occurrences. Repeated names in different files or styleables are not collapsed into one effective resource definition.",
    "Every element attribute and namespace declaration is retained. Format unions, numeric spellings, localization hints and parent names remain raw; missing values are not synthesized.",
    "Styleable attribute order and enum/flag order are source order. Neither is claimed to equal generated R.styleable indices, compiled IDs or runtime iteration order.",
    "Styleable parent labels are not expanded. Name-only attribute occurrences are not resolved to other declarations, packages or runtime providers.",
    "Comments, processing instructions and formatting are bound by whole-file hashes but not interpreted. Documentation visibility, annotations in comments and global top-level declaration order are outside the semantic views.",
    "Attributes, their ordered symbol identities and symbol values are overlapping views; row counts are not additive API, defect or implementation counts.",
    "The selected family is data/res/values/attrs*.xml only. Other resource values, configurations, overlays, compiled tables and permission enforcement are not evaluated.",
]


def family_member(relative):
    path = PurePosixPath(relative)
    return path.parent.as_posix() == "data/res/values" and path.name.startswith("attrs") and path.suffix == ".xml"


def parse_file(data, relative):
    if len(data) > MAX_XML_BYTES:
        raise ValueError("resource_attribute_xml_exceeds_32_MiB_scope")
    pending, namespaces = [], {}
    parser = ET.iterparse(io.BytesIO(data), events=("start", "start-ns"))
    for event, value in parser:
        if event == "start-ns":
            pending.append({"prefix": value[0], "uri": value[1]})
        else:
            namespaces[value] = pending
            pending = []
    root = parser.root
    if root.tag != "resources":
        raise ValueError("unsupported_resource_attribute_root")
    records = {category: {} for category in CATEGORIES}
    repeated, counters = [], Counter()

    def metadata(node, require_name=True, leaf=False):
        if require_name and not node.attrib.get("name"):
            raise ValueError("missing_resource_attribute_name: " + node.tag)
        if (node.text or "").strip() or (node.tail or "").strip() or (leaf and len(node)):
            raise ValueError("unsupported_resource_attribute_content: " + node.tag)
        return {"attributes": dict(node.attrib), "namespace_declarations": namespaces[node]}

    def ignored(node):
        if node.tag != "eat-comment":
            return False
        metadata(node, require_name=False, leaf=True)
        if node.attrib or namespaces[node]:
            raise ValueError("unsupported_eat_comment_metadata")
        counters["eat_comment_markers"] += 1
        return True

    def occurrence(counts, name, context):
        counts[name] += 1
        number = counts[name]
        if number > 1:
            repeated.append({"context": list(context), "name": name, "occurrence": number})
        return number

    def attribute(node, scope, counts):
        item = metadata(node)
        name = node.attrib["name"]
        index = occurrence(counts, name, (relative, *scope, "attr"))
        key = (relative, *scope, name, index)
        symbols, symbol_counts = [], Counter()
        for child in node:
            if ignored(child):
                continue
            if child.tag not in ("enum", "flag"):
                raise ValueError("unsupported_attribute_child: " + child.tag)
            symbol = metadata(child, leaf=True)
            if "value" not in child.attrib:
                raise ValueError("missing_attribute_symbol_value")
            symbol_name = child.attrib["name"]
            number = occurrence(symbol_counts, (child.tag, symbol_name), (*key, "symbol"))
            symbol_key = (*key, child.tag, symbol_name, number)
            records["symbols"][symbol_key] = symbol
            symbols.append({"kind": child.tag, "name": symbol_name, "occurrence": number})
            counters[child.tag + "_symbols"] += 1
        item["symbol_order"] = symbols
        records["attributes"][key] = item
        counters["top_level_attributes" if scope[0] is None else "styleable_attributes"] += 1
        counters["attributes_with_format"] += int("format" in node.attrib)
        counters["attributes_with_symbols"] += bool(symbols)
        counters["name_only_attributes"] += int(set(node.attrib) == {"name"} and not symbols)
        return {"name": name, "occurrence": index}

    records["resource_roots"][(relative,)] = metadata(root, require_name=False)
    root_attrs, groups = Counter(), Counter()
    for node in root:
        if ignored(node):
            continue
        if node.tag == "attr":
            attribute(node, (None, 0), root_attrs)
        elif node.tag == "declare-styleable":
            item = metadata(node)
            name = node.attrib["name"]
            index = occurrence(groups, name, (relative, "declare-styleable"))
            order, attrs = [], Counter()
            for child in node:
                if ignored(child):
                    continue
                if child.tag != "attr":
                    raise ValueError("unsupported_styleable_child: " + child.tag)
                order.append(attribute(child, (name, index), attrs))
            item["attribute_order"] = order
            records["styleables"][(relative, name, index)] = item
        else:
            raise ValueError("unsupported_attribute_family_element: " + node.tag)
    counts = {category: len(values) for category, values in records.items()}
    counts.update({key: counters[key] for key in ("top_level_attributes", "styleable_attributes",
        "attributes_with_format", "attributes_with_symbols", "name_only_attributes",
        "enum_symbols", "flag_symbols", "eat_comment_markers")})
    counts["distinct_attribute_name_spellings"] = len({key[-2] for key in records["attributes"]})
    return records, {"counts": counts, "repeated_scoped_identities": repeated}


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
        selected = sorted(name[len(prefix):] for name in names if family_member(name[len(prefix):]))
        if not selected or set(pin["artifacts"]) != {"source.properties", *selected}:
            raise ValueError("attribute_family_inventory_mismatch")
        for name in selected:
            if archive.getinfo(prefix + name).file_size > MAX_XML_BYTES:
                raise ValueError("resource_attribute_xml_exceeds_32_MiB_scope")
        blobs = {name: archive.read(prefix + name) for name in ("source.properties", *selected)}
    artifacts = {name: platform.bytes_record(data) for name, data in blobs.items()}
    if artifacts != pin["artifacts"]:
        raise ValueError("resource_attribute_artifact_pin_mismatch")
    props = platform.properties(blobs["source.properties"])
    if props != pin["source_properties"]:
        raise ValueError("source_properties_pin_mismatch")
    profile = platform.sdk_profile(props, pin["repository_package"])
    if platform.archive_record(path) != receipt:
        raise ValueError("platform_archive_changed_during_comparison")
    records = {category: {} for category in CATEGORIES}
    observations = {}
    for name in selected:
        parsed, observations[name] = parse_file(blobs[name], name)
        for category in CATEGORIES:
            records[category].update(parsed[category])
    profile.update({"id": pin["id"], "package_path": pin["package_path"], "archive": pin["archive"],
                    "artifacts": artifacts, "selected_resource_files": selected, "observations": observations})
    return profile, records


def mapping_delta(category, before, after, rows):
    old_keys, new_keys = set(before), set(after)
    changed = {key for key in old_keys & new_keys if before[key] != after[key]}
    for key in sorted((old_keys ^ new_keys) | changed, key=platform.canonical):
        rows.append({"category": category, "key": list(key),
                     "change": "added" if key not in old_keys else "removed" if key not in new_keys else "changed",
                     "before": before.get(key), "after": after.get(key)})
    return {"before": len(before), "after": len(after), "added": len(new_keys - old_keys),
            "removed": len(old_keys - new_keys), "changed": len(changed),
            "unchanged": len(old_keys & new_keys) - len(changed)}


def compare(before_path, after_path, manifest):
    if manifest.get("schema_version") != SCHEMA_VERSION or manifest["before"]["id"] == manifest["after"]["id"]:
        raise ValueError("unsupported_resource_attribute_manifest")
    profiles, records = {}, {}
    for role, path in (("before", before_path), ("after", after_path)):
        profiles[role], records[role] = load_archive(path, manifest[role])
    rows = []
    dimensions = {category: mapping_delta(category, records["before"][category], records["after"][category], rows)
                  for category in CATEGORIES}
    encoded = "".join(platform.canonical(row) + "\n" for row in rows).encode("ascii")
    report = {"schema_version": SCHEMA_VERSION, "comparison_id": manifest["comparison_id"], "scope": SCOPE,
              "profiles": profiles, "dimensions": dimensions, "interpretation": LIMITS,
              "manifest_sha256": platform.fingerprint(manifest),
              "source_platform_manifest_sha256": manifest["source_platform_manifest_sha256"],
              "repository_metadata_provenance": manifest["repository_metadata_provenance"],
              "source_format_context": manifest["source_format_context"],
              "generator_sha256": {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                  for name in ("sdk_resource_attrs_diff.py", "sdk_platform_diff.py")},
              "delta_rows": len(rows), "delta_rows_sha256": hashlib.sha256(encoded).hexdigest(),
              "runtime_sdk_int_changed": False, "a07_complete": False}
    return report, rows


def markdown(report):
    lines = ["# SDK36 to SDK37 resource attribute metadata", "", report["scope"], "",
             "## Measured occurrences", "", "| Dimension | Before | After | Added | Removed | Changed | Unchanged |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for category, values in report["dimensions"].items():
        lines.append("| " + category + " | " + " | ".join(f"{values[key]:,}" for key in
            ("before", "after", "added", "removed", "changed", "unchanged")) + " |")
    lines += ["", "These overlapping source views are not additive API or compatibility counts.", "",
              "## Complete selected family", "", "| File | Before bytes | After bytes |", "| --- | ---: | ---: |"]
    before, after = (report["profiles"][role]["artifacts"] for role in ("before", "after"))
    for name in sorted(set(before) | set(after)):
        if name != "source.properties":
            left, right = before.get(name), after.get(name)
            lines.append(f"| {name} | {left['bytes'] if left else 'absent'} | {right['bytes'] if right else 'absent'} |")
    lines += ["", "The full archives and every selected XML file are pinned in manifest.json; profile evidence is retained in summary.json.",
              "", "## Interpretation", ""]
    lines.extend("- " + item for item in report["interpretation"])
    lines += ["", "Each dimension obeys before = removed + changed + unchanged and after = added + changed + unchanged.",
              "A07 and issue #56 remain open. No runtime code or reported capability is changed.", "",
              "See [the tool guide](../../sdk-resource-attrs-diff.md) for reproduction and evidence boundaries.", ""]
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
        print(f"SDK resource-attribute comparison failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    print(f"Compared {report['comparison_id']}: {len(rows)} metadata delta rows; no runtime qualification.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
