#!/usr/bin/env python3
"""Offline, source-only release profiles, Java compat annotations and AIDL file bytes."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import copy
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import tarfile
import tempfile
import xml.etree.ElementTree as ET


class InputError(ValueError):
    pass


PREFIX = "android.compat.annotation."
ANNOTATIONS = ("ChangeId", "Disabled", "EnabledAfter", "EnabledSince", "LoggingOnly", "NoLogging", "Overridable")
VERSION_FIELDS = ("RELEASE_PLATFORM_SDK_VERSION", "RELEASE_PLATFORM_SDK_VERSION_FULL",
                  "RELEASE_PLATFORM_VERSION_CODENAME", "RELEASE_PLATFORM_PREVIEW_SDK_INT",
                  "RELEASE_PLATFORM_VERSION_LAST_STABLE")
MAX_FILES, MAX_FILE_BYTES, MAX_ARCHIVE_BYTES = 20000, 16 * 1024 * 1024, 64 * 1024 * 1024
MAX_SOURCE_BYTES = 256 * 1024 * 1024
TOOL_DIR = Path(__file__).resolve().parent


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def git_object(kind, data):
    return hashlib.sha1(kind.encode() + b" " + str(len(data)).encode() + b"\0" + data).hexdigest()


def checked_bytes(input_dir, pin):
    name = pin["filename"]
    if PurePosixPath(name).name != name:
        raise InputError("Input filename must be a basename: " + name)
    data = (input_dir / name).read_bytes()
    if len(data) != pin["bytes"] or sha256(data) != pin["sha256"]:
        raise InputError("Pinned input differs: " + name)
    if "git_blob" in pin and git_object("blob", data) != pin["git_blob"]:
        raise InputError("Pinned Git blob differs: " + name)
    return data


def read_archive(input_dir, pin):
    data = checked_bytes(input_dir, pin)
    if len(data) > MAX_ARCHIVE_BYTES:
        raise InputError("Source archive exceeds supported size")
    nodes, contents, files = {}, {}, []
    total = 0
    # No archive member is extracted or executed. Symlinks are Git blob data only.
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        for member in archive:
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts or not path.parts:
                raise InputError("Unsupported source path: " + member.name)
            if member.isdir():
                continue
            if member.isfile():
                if member.size > MAX_FILE_BYTES:
                    raise InputError("Source file exceeds supported size: " + member.name)
                body = archive.extractfile(member).read()
                mode = "100755" if member.mode & 0o111 else "100644"
            elif member.issym():
                body, mode = member.linkname.encode(), "120000"
            else:
                raise InputError("Unsupported source member: " + member.name)
            total += len(body)
            if total > MAX_SOURCE_BYTES or len(files) >= MAX_FILES:
                raise InputError("Source tree exceeds supported bounds")
            node = nodes
            for part in path.parts[:-1]:
                node = node.setdefault(part, {})
                if not isinstance(node, dict):
                    raise InputError("Source path collides with a file")
            if path.name in node:
                raise InputError("Duplicate source member: " + member.name)
            blob = git_object("blob", body)
            node[path.name] = (mode, blob)
            name = path.as_posix()
            contents[name] = body
            files.append({"path": name, "mode": mode, "bytes": len(body), "blob": blob, "sha256": sha256(body)})

    def tree(node):
        entries = []
        for name, value in node.items():
            directory = isinstance(value, dict)
            mode, blob = ("40000", tree(value)) if directory else value
            sort = name.encode() + (b"/" if directory else b"")
            entries.append((sort, mode.encode() + b" " + name.encode() + b"\0" + bytes.fromhex(blob)))
        return git_object("tree", b"".join(body for _, body in sorted(entries)))

    actual_tree = tree(nodes)
    if actual_tree != pin["git_tree"] or len(files) != pin["files"]:
        raise InputError("Source archive does not reproduce pinned Git tree: " + pin["filename"])
    files.sort(key=lambda f: f["path"])
    return {"files": files, "contents": contents, "git_tree": actual_tree,
            "file_inventory_sha256": digest(files), "uncompressed_bytes": total}


# A strict reader of the selected textproto forms, not a general Soong evaluator.
TOKEN = re.compile(r'\s+|\#[^\n]*|//[^\n]*|"(?:[^"\\]|\\.)*"|[A-Za-z_][A-Za-z_0-9]*|-?[0-9]+|[{}:]')


def textproto(data):
    source = data.decode("utf-8") if isinstance(data, bytes) else data
    tokens, position = [], 0
    for match in TOKEN.finditer(source):
        if match.start() != position:
            raise InputError("Unsupported textproto syntax at offset " + str(position))
        token = match.group()
        position = match.end()
        if token.isspace() or token.startswith(("#", "//")):
            continue
        tokens.append(token)
    if position != len(source):
        raise InputError("Unsupported trailing textproto syntax")
    index = 0

    def message(nested=False):
        nonlocal index
        result = defaultdict(list)
        while index < len(tokens):
            if tokens[index] == "}":
                if not nested:
                    raise InputError("Unexpected textproto close")
                index += 1
                return dict(result)
            name = tokens[index]
            if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name):
                raise InputError("Unsupported textproto field")
            index += 1
            if index < len(tokens) and tokens[index] == ":":
                index += 1
            if index >= len(tokens):
                raise InputError("Missing textproto value")
            token = tokens[index]
            index += 1
            if token == "{":
                value = message(True)
            elif token.startswith('"'):
                try:
                    value = json.loads(token)
                except ValueError as exc:
                    raise InputError("Unsupported textproto string escape") from exc
            elif token in ("true", "false"):
                value = token == "true"
            elif re.fullmatch(r"-?[0-9]+", token):
                value = int(token)
            elif re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", token):
                value = token
            else:
                raise InputError("Unsupported textproto value")
            result[name].append(value)
        if nested:
            raise InputError("Unclosed textproto message")
        return dict(result)

    return message()


def one(message, field, default=None):
    values = message.get(field, [])
    if len(values) > 1:
        raise InputError("Repeated singular field: " + field)
    return values[0] if values else default


def fields(message, allowed):
    unknown = set(message) - set(allowed)
    if unknown:
        raise InputError("Unsupported textproto fields: " + ", ".join(sorted(unknown)))


def proto_value(message):
    if message is None:
        return {"status": "absent", "value": None}
    fields(message, ("string_value", "bool_value", "unspecified_value", "obsolete"))
    if len(message) != 1:
        raise InputError("Unsupported Value oneof")
    kind = next(iter(message))
    value = one(message, kind)
    if kind == "string_value" and isinstance(value, str):
        return {"status": "value", "value": value}
    if kind in ("bool_value", "unspecified_value", "obsolete") and isinstance(value, bool):
        return {"status": kind, "value": value if kind == "bool_value" else None}
    raise InputError("Unsupported typed Value")


def release_config_profile(config_files, declarations, value_files, map_bytes, build_id_bytes):
    mapping = textproto(map_bytes)
    fields(mapping, ("aliases", "description", "default_containers"))
    aliases = {}
    for entry in mapping.get("aliases", []):
        fields(entry, ("name", "target"))
        name, target = one(entry, "name"), one(entry, "target")
        if not isinstance(name, str) or not isinstance(target, str) or name in aliases:
            raise InputError("Unsupported/duplicate release alias")
        aliases[name] = target
    selected = "aosp_current"
    alias_chain = []
    while selected in aliases:
        if selected in alias_chain:
            raise InputError("Release alias cycle")
        alias_chain.append(selected)
        selected = aliases[selected]
    configs = {}
    config_allowed = ("name", "inherits", "aconfig_value_sets", "aconfig_flags_only", "prior_stages",
                      "release_config_type", "disallow_lunch_use")
    for path, body in sorted(config_files.items()):
        if not path.endswith(".textproto"):
            raise InputError("Unsupported release-config file: " + path)
        parsed = textproto(body)
        fields(parsed, config_allowed)
        name = one(parsed, "name")
        if name != Path(path).stem or name in configs:
            raise InputError("Config name/path mismatch or duplicate")
        for key in ("aconfig_flags_only", "disallow_lunch_use"):
            if not isinstance(one(parsed, key, False), bool):
                raise InputError("Invalid config boolean: " + key)
        configs[name] = {"path": "release_configs/" + path, "metadata": parsed}
    if selected not in configs:
        raise InputError("Selected release config is absent")
    cache, active, used = {}, set(), set()

    def resolve(name, flag):
        if (name, flag) in cache:
            return copy.deepcopy(cache[name, flag])
        if (name, flag) in active or name not in configs:
            raise InputError("Cyclic or missing release-config parent: " + name)
        active.add((name, flag)); used.add(name)
        metadata = configs[name]["metadata"]
        if one(metadata, "aconfig_flags_only", False):
            raise InputError("Aconfig-only release inheritance is outside this version-metadata evaluator")
        config_type = one(metadata, "release_config_type", "CONFIG_TYPE_UNSPECIFIED")
        if config_type not in ("CONFIG_TYPE_UNSPECIFIED", "RELEASE_CONFIG", "EXPLICIT_INHERITANCE_CONFIG", "BUILD_VARIANT"):
            raise InputError("Unsupported release config type")
        declaration_path = flag + ".textproto"
        declaration = None
        if declaration_path in declarations:
            declaration = textproto(declarations[declaration_path])
            fields(declaration, ("name", "namespace", "description", "bugs", "value", "workflow", "containers"))
            if one(declaration, "name") != flag:
                raise InputError("Declaration name differs from path")
        workflow = one(declaration, "workflow") if declaration else None
        if workflow not in (None, "MANUAL", "MANUAL_NO_INHERIT"):
            raise InputError("Selected metadata flag has unsupported workflow: " + str(workflow))
        initial = proto_value(one(declaration, "value")) if declaration else {"status": "undeclared", "value": None}
        if initial["status"] not in ("value", "absent", "undeclared"):
            raise InputError("Selected version declaration requires a supported string/default form")
        state = {"status": initial["status"], "value": initial["value"], "declaration_present": declaration is not None,
                 "workflow": workflow, "default": copy.deepcopy(initial), "assigned": False,
                 "assignment_sources": [], "steps": [], "declaration": declaration,
                 "declaration_path": "flag_declarations/" + declaration_path if declaration else None}
        parents = list(metadata.get("inherits", []))
        if config_type in ("CONFIG_TYPE_UNSPECIFIED", "RELEASE_CONFIG") and name != "root" and "root" in configs:
            parents.insert(0, "root")
        parents = list(dict.fromkeys(parents))
        for parent in parents:
            if parent in aliases:
                raise InputError("Parent alias resolution is outside the selected profile contract")
            inherited = resolve(parent, flag)
            apply = inherited["assigned"] and workflow != "MANUAL_NO_INHERIT"
            before = {"status": state["status"], "value": state["value"], "assigned": state["assigned"]}
            conflict = apply and state["assigned"] and (state["status"], state["value"]) != (inherited["status"], inherited["value"])
            state["steps"].append({"kind": "inherit", "parent": parent, "applied": apply,
                                   "reason": "non-inheriting workflow" if workflow == "MANUAL_NO_INHERIT" else
                                   "assigned parent value" if apply else "parent has only a declaration default or no declaration",
                                   "before": before, "parent_status": inherited["status"], "parent_value": inherited["value"],
                                   "parent_assignment_sources": inherited["assignment_sources"], "conflict": conflict})
            if apply:
                state.update(status=inherited["status"], value=inherited["value"], assigned=True)
                state["assignment_sources"] += inherited["assignment_sources"]
        path = name + "/" + flag + ".textproto"
        if path in value_files:
            if declaration is None:
                raise InputError("Value assigned to undeclared selected flag: " + path)
            value = textproto(value_files[path]); fields(value, ("name", "value", "redacted"))
            if one(value, "name") != flag:
                raise InputError("Flag value name/path mismatch")
            redacted = one(value, "redacted", False)
            if not isinstance(redacted, bool):
                raise InputError("Unsupported redaction value")
            if redacted:
                raise InputError("Selected version flag redaction requires a broader evaluator")
            updated = {"status": "redacted", "value": None} if redacted else proto_value(one(value, "value"))
            if updated["status"] != "value":
                raise InputError("Selected version assignment requires a string value")
            source = "flag_values/" + path
            state["steps"].append({"kind": "local_assignment", "source": source, "before":
                                   {"status": state["status"], "value": state["value"]}, "after": updated})
            state.update(status=updated["status"], value=updated["value"], assigned=True)
            state["assignment_sources"].append(source)
        active.remove((name, flag)); cache[name, flag] = copy.deepcopy(state)
        return state

    resolved = {flag: resolve(selected, flag) for flag in VERSION_FIELDS}
    # Variant/product overlays are not evaluated. Detect any selected-field variant assignments.
    variant_assignments = [p for p in value_files if Path(p).name.removesuffix(".textproto") in VERSION_FIELDS
                           and p.split("/")[0] in configs and one(configs[p.split("/")[0]]["metadata"], "release_config_type") == "BUILD_VARIANT"]
    build_ids = [m.group(1) for line in build_id_bytes.decode().splitlines()
                 if (m := re.fullmatch(r"\s*BUILD_ID\s*=\s*([A-Za-z0-9_.-]+)\s*", line))]
    if len(build_ids) != 1:
        raise InputError("Unsupported BUILD_ID assignment")
    major = resolved["RELEASE_PLATFORM_SDK_VERSION"]["value"]
    full = resolved["RELEASE_PLATFORM_SDK_VERSION_FULL"]["value"]
    version = {"major": None, "minor": None, "full_spelling": full, "status": "unresolved"}
    if isinstance(major, str) and re.fullmatch(r"[0-9]+", major) and isinstance(full, str) and re.fullmatch(r"[0-9]+\.[0-9]+", full):
        full_major, minor = map(int, full.split("."))
        if full_major == int(major) and not variant_assignments:
            version.update(major=full_major, minor=minor, status="resolved_selected_platform_config")
    return {"selection_alias": "aosp_current", "alias_chain": alias_chain + [selected], "selected_config": selected,
            "build_id": build_ids[0], "config_graph": {k: configs[k] for k in sorted(used)},
            "resolved_fields": resolved, "sdk": version,
            "all_config_evaluations": {name: {flag: cache[name, flag] for flag in VERSION_FIELDS} for name in sorted(used)},
            "selected_field_variant_assignments": sorted(variant_assignments),
            "scope": "Platform build/release repository only; product/vendor overlays and aconfig selection are unmeasured"}


def literal_value(expression):
    if expression is None:
        return {"status": "unresolved", "value": None, "reason": "initializer absent"}
    kind = expression["kind"]
    if kind in ("INT_LITERAL", "LONG_LITERAL", "CHAR_LITERAL") and "value" in expression:
        return {"status": "resolved", "value": expression["value"], "java_type": expression["literal_type"], "basis": "javac AST literal"}
    if kind in ("PARENTHESIZED", "UNARY_PLUS", "UNARY_MINUS"):
        result = literal_value(expression["operand"])
        if result["status"] == "resolved" and kind != "PARENTHESIZED":
            bits = 64 if result["java_type"] == "long" else 32
            value = result["value"] * (-1 if kind == "UNARY_MINUS" else 1)
            result.update(value=(value + 2 ** (bits - 1)) % 2 ** bits - 2 ** (bits - 1),
                          java_type="long" if bits == 64 else "int", basis="Java integral unary operation over AST literal")
        return result
    return {"status": "unresolved", "value": None, "reason": "unsupported expression kind: " + kind}


def resolve_constant(expression, file, constants):
    result = literal_value(expression)
    if result["status"] == "resolved" or expression is None:
        return result
    if expression["kind"] == "PARENTHESIZED":
        return resolve_constant(expression["operand"], file, constants)
    if expression["kind"] not in ("IDENTIFIER", "MEMBER_SELECT"):
        return result
    if expression.get("unattributed_inheritance"):
        return {"status": "unresolved", "value": None,
                "reason": "enclosing scope has unattributed supertypes; inherited constant binding is not guessed"}
    name = expression["name"]
    first = name.split(".")[0]
    if first in set(file["declared_type_names"]) | set(file["declared_variable_names"]):
        return {"status": "unresolved", "value": None, "reason": "source declares a matching name; constant shadowing is not guessed"}
    candidates = []
    if name.startswith("android.os.Build.VERSION_CODES.") or name.startswith("android.os.Build.VERSION_CODES_FULL."):
        candidates.append(name)
    for item in file["imports"]:
        imported = item["name"]
        if item["static"] and not imported.endswith(".*") and "." not in name and imported.rsplit(".", 1)[-1] == name:
            candidates.append(imported)
        elif not item["static"] and imported.rsplit(".", 1)[-1] == first:
            candidates.append(imported + name[len(first):])
    if not candidates and file["package"] == "android.os" and name.startswith("Build.VERSION_CODES"):
        candidates.append("android.os." + name)
    if len(candidates) != 1 or candidates[0] not in constants or len(constants[candidates[0]]) != 1:
        return {"status": "unresolved", "value": None, "reason": "no unique pinned Build constant binding", "candidate_bindings": candidates}
    constant = constants[candidates[0]][0]
    result = literal_value(constant["initializer"])
    if result["status"] == "resolved" and constant["source_parse_complete"]:
        result.update(basis="pinned Build source constant via lexical import/qualification", constant=candidates[0],
                      constant_source={k: constant[k] for k in ("path", "location", "source")})
    else:
        result = {"status": "unresolved", "value": None, "reason": "pinned constant initializer is unsupported/incomplete", "constant": candidates[0]}
    return result


def interpret_java(records, source_files, profile):
    file_pins = {f["path"]: f for f in source_files}
    file_rows = [r for r in records if r["record_type"] == "source_file"]
    expected = {p for p in file_pins if p.endswith(".java")}
    if len(file_rows) != len(expected) or {r["path"] for r in file_rows} != expected:
        raise InputError("AST file denominator differs from complete selected Java corpus")
    by_file = {r["path"]: r for r in file_rows}
    parsers = [r for r in records if r["record_type"] == "parser"]
    if len(parsers) != 1:
        raise InputError("Missing/duplicate parser metadata")
    constants = defaultdict(list)
    for original in records:
        if original["record_type"] == "version_constant":
            record = copy.deepcopy(original)
            record["source"] = file_pins[record["path"]]
            constants[record["declaring_field_identity"].replace("#", ".")].append(record)
    observations, occurrences = [], []
    unresolved_annotation_identities = []
    for original in records:
        if original["record_type"] not in ("annotated_variable", "non_variable_annotation"):
            continue
        record = copy.deepcopy(original)
        record.update(profile=profile, source=file_pins[record["path"]])
        file = by_file[record["path"]]
        annotations = record.get("annotations", [record.get("annotation")])
        known = defaultdict(list)
        for annotation in annotations:
            if annotation is None:
                continue
            resolved = annotation["resolved_name"]
            leaf = annotation["written_name"].rsplit(".", 1)[-1]
            if resolved in {PREFIX + a for a in ANNOTATIONS}:
                known[resolved.removeprefix(PREFIX)].append(annotation)
            elif resolved is None and leaf in ANNOTATIONS:
                unresolved_annotation_identities.append({"path": record["path"], "location": annotation["location"], "annotation": annotation})
        unknown_change_id = any(a is not None and a["resolved_name"] is None
                                and a["written_name"].rsplit(".", 1)[-1] == "ChangeId" for a in annotations)
        record["platform_change_id"] = True if known["ChangeId"] else None if unknown_change_id else False
        if known["ChangeId"] and record.get("field"):
            invalid, unresolved = [], []
            if record["primitive_type"] != "long":
                invalid.append("upstream processor requires primitive long")
            if not {"static", "final"}.issubset(record["effective_modifiers"]):
                invalid.append("upstream processor requires a static constant/final field")
            if not record["source_parse_complete"]:
                unresolved.append("source parse incomplete")
            if record["declaring_field_identity"] is None:
                unresolved.append("local/anonymous declaring type has no canonical field identity")
            value = resolve_constant(record["initializer"], file, constants)
            if value["status"] != "resolved":
                unresolved.append("change ID constant unresolved")
            record["change_id"] = value
            metadata = {name: bool(known[name]) for name in ("Disabled", "LoggingOnly", "NoLogging", "Overridable")}
            for name in ("EnabledAfter", "EnabledSince"):
                metadata[name] = None
                if known[name]:
                    args = known[name][0]["arguments"]
                    if len(args) != 1 or args[0]["name"] != "targetSdkVersion":
                        invalid.append(name + " requires one targetSdkVersion argument")
                    else:
                        target = resolve_constant(args[0]["expression"], file, constants)
                        metadata[name] = target
                        if target["status"] != "resolved":
                            unresolved.append(name + " threshold unresolved")
                        elif target["java_type"] == "long":
                            invalid.append(name + " targetSdkVersion requires int, not long")
            for name, items in known.items():
                if len(items) > 1:
                    invalid.append("duplicate non-repeatable annotation " + name)
                if name not in ("EnabledAfter", "EnabledSince") and any(a["arguments"] for a in items):
                    invalid.append("unexpected arguments on " + name)
            if known["Disabled"] and (known["EnabledAfter"] or known["EnabledSince"]):
                invalid.append("Disabled conflicts with target-SDK gating")
            if known["LoggingOnly"] and (known["Disabled"] or known["EnabledAfter"] or known["EnabledSince"]):
                invalid.append("LoggingOnly conflicts with Disabled/target-SDK gating")
            if known["EnabledAfter"] and known["EnabledSince"]:
                invalid.append("EnabledAfter conflicts with EnabledSince")
            if any(a["resolved_name"] is None and a["written_name"].rsplit(".", 1)[-1] in ANNOTATIONS for a in annotations):
                unresolved.append("companion annotation identity unresolved")
            record["processor_metadata"] = metadata
            record["validity"] = {"status": "invalid" if invalid else "unresolved" if unresolved else "valid_for_measured_source_rules",
                                  "invalid_reasons": invalid, "unresolved_reasons": unresolved,
                                  "compiler_attributed": False, "compiled_config_membership_measured": False}
            occurrences.append(record)
        observations.append(record)
    identities, ids = defaultdict(list), defaultdict(list)
    for record in occurrences:
        locator = {"path": record["path"], "line": record["location"]["line"], "field": record["declaring_field_identity"]}
        if record["declaring_field_identity"] is not None:
            identities[record["declaring_field_identity"]].append(locator)
        if record["change_id"]["status"] == "resolved":
            ids[str(record["change_id"]["value"])].append(locator)
    failures = [r for r in file_rows if not r["complete"]]
    complete = not failures and not unresolved_annotation_identities
    summary = {"java_files": len(file_rows), "parsed_without_errors": len(file_rows) - len(failures),
               "parse_complete": not failures, "parse_failures": failures, "diagnostics": [
                   {"path": r["path"], "diagnostics": r["diagnostics"]} for r in file_rows if r["diagnostics"]],
               "file_parse_inventory_sha256": digest(file_rows), "parser": parsers[0],
               "observed_changeid_field_occurrences": len(occurrences),
               "unresolved_changeid_field_candidates": sum(r.get("field", False) and r["platform_change_id"] is None
                                                           for r in observations),
               "complete_source_field_denominator": len(occurrences) if complete else None,
               "canonical_field_identities": len(identities), "resolved_unique_change_ids": len(ids),
               "validity_counts": dict(sorted(Counter(r["validity"]["status"] for r in occurrences).items())),
               "complete_resolved_metadata": complete and all(r["validity"]["status"] == "valid_for_measured_source_rules" for r in occurrences),
               "unresolved_annotation_identities": unresolved_annotation_identities,
               "non_field_changeid_occurrences": [r for r in observations if r["platform_change_id"] and not r.get("field")],
               "duplicate_declaring_fields": {k: v for k, v in sorted(identities.items()) if len(v) > 1},
               "duplicate_change_ids": {k: v for k, v in sorted(ids.items()) if len(v) > 1},
               "known_annotation_counts": dict(sorted(Counter(a["resolved_name"].removeprefix(PREFIX)
                   for r in occurrences for a in r["annotations"] if a["resolved_name"] in {PREFIX + a for a in ANNOTATIONS}).items())),
               "build_version_constants": [dict(record, literal_resolution=literal_value(record["initializer"]))
                   for key in sorted(constants) for record in constants[key]]}
    return summary, observations, occurrences


def parse_java(java, classes, core, directory):
    source_dir = directory / "source"
    for record in core["files"]:
        if record["path"].endswith(".java"):
            if record["mode"] not in ("100644", "100755"):
                raise InputError("Java source symlink is outside the selected parser contract")
            target = source_dir / record["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(core["contents"][record["path"]])
    process = subprocess.run([java, "-Xmx512m", "-cp", str(classes), "SourceCompatInventory", str(source_dir)],
                             text=True, encoding="utf-8", capture_output=True, check=False)
    if process.returncode:
        raise InputError("Java AST helper failed: " + process.stderr[-4000:])
    if process.stderr.strip():
        raise InputError("Uncaptured Java parser diagnostics: " + process.stderr[-4000:])
    return [json.loads(line) for line in process.stdout.splitlines()]


def project_selectors(data):
    tag, rows = None, []
    for line in data.decode().splitlines():
        parts = shlex.split(line, comments=True)
        if not parts:
            continue
        if parts[0] == "tag":
            if tag is not None or len(parts) != 2:
                raise InputError("Unsupported project tag declaration")
            tag = parts[1]
        else:
            path, separator, repo = parts[0].partition("=")
            rows.append({"path": path, "repository": repo if separator else "platform/" + path,
                         "sparse_selection": parts[1:]})
    if len({r["path"] for r in rows}) != len(rows) or tag is None:
        raise InputError("Duplicate/missing project selector")
    return tag, rows


def selected_references(profile, xml_bytes, selectors):
    root = ET.fromstring(xml_bytes)
    if root.findall("include") or len(root.findall("default")) != 1:
        raise InputError("Unsupported release manifest include/default structure")
    default = dict(root.find("default").attrib)
    projects = [dict(e.attrib) for e in root.findall("project")]
    if len(projects) != profile["release_manifest"]["expected_projects"]:
        raise InputError("Release manifest project denominator differs")
    by_repo = defaultdict(list)
    for project in projects:
        by_repo[project["name"]].append(project)
    refs = profile["repository_references"]
    if {r["path"] for r in selectors} != set(refs):
        raise InputError("Pinned reference selector denominator differs")
    rows = []
    for selector in selectors:
        ref = refs[selector["path"]]
        if any(ref[k] != selector[k] for k in ("path", "repository", "sparse_selection")):
            raise InputError("Pinned reference differs from project selector")
        if ref["tag"] != profile["tag"] or not re.fullmatch("[0-9a-f]{40}", ref["commit"]):
            raise InputError("Invalid selected release reference")
        matches = by_repo[ref["repository"]]
        if not matches or any(p.get("revision", default.get("revision")) != "refs/tags/" + profile["tag"] for p in matches):
            raise InputError("Selected repository/revision not bound by release manifest")
        rows.append({**selector, "tag": ref["tag"], "tag_object": ref["tag_object"], "commit": ref["commit"],
                     "annotated_tag": ref["annotated_tag"], "reference_receipt": ref["receipt"],
                     "manifest_matches": matches, "local_checkout_alias": not any(p.get("path", p["name"]) == selector["path"] for p in matches)})
    repos = defaultdict(set)
    for row in rows:
        repos[row["repository"]].add(row["commit"])
    if any(len(values) != 1 for values in repos.values()):
        raise InputError("Aliases disagree about repository commit")
    return {"manifest_projects": len(projects), "manifest_default": default,
            "selectors": len(rows), "unique_repositories": len(repos), "references": rows,
            "reference_mapping_is_not_module_equivalence": True}


def schema_metadata(data):
    root = ET.fromstring(data)
    ns = {"x": "http://www.w3.org/2001/XMLSchema"}
    attrs = [dict(a.attrib) for a in root.findall(".//x:complexType[@name='change']//x:attribute", ns)]
    expected = {"id", "name", "disabled", "loggingOnly", "noLogging", "enableAfterTargetSdk",
                "enableSinceTargetSdk", "description", "overridable"}
    if {a["name"] for a in attrs} != expected:
        raise InputError("Unsupported upstream compat config schema attributes")
    return {"sha256": sha256(data), "git_blob": git_object("blob", data), "attributes": attrs,
            "unique_constraints": [ET.tostring(e, encoding="unicode") for e in root.findall(".//x:unique", ns)],
            "compiled_config_read": False, "source_description_comments_measured": False}


def measured(record):
    metadata = copy.deepcopy(record["processor_metadata"])
    for key in ("EnabledAfter", "EnabledSince"):
        if metadata[key] is not None:
            metadata[key] = {k: v for k, v in metadata[key].items() if k in ("status", "value", "java_type", "reason")}
    return {"change_id": {k: v for k, v in record["change_id"].items() if k in ("status", "value", "reason")},
            "primitive_type": record["primitive_type"], "effective_modifiers": record["effective_modifiers"],
            "processor_metadata": metadata, "validity_status": record["validity"]["status"]}


def compare_sources(before, after, before_occurrences, after_occurrences):
    delta, counts = [], Counter()
    for category, key, a, b in [
        ("aidl_source_file", "path", before["aidl_files"], after["aidl_files"]),
        ("selected_repository_reference", "repository", before["source_references"]["references"], after["source_references"]["references"])]:
        left, right = defaultdict(list), defaultdict(list)
        for row in a: left[row[key]].append(row)
        for row in b: right[row[key]].append(row)
        for name in sorted(left.keys() | right.keys()):
            old, new = left.get(name), right.get(name)
            if category == "aidl_source_file":
                changed = old is None or new is None or old[0]["sha256"] != new[0]["sha256"] or old[0]["mode"] != new[0]["mode"]
            else:
                changed = old is None or new is None or {r["commit"] for r in old} != {r["commit"] for r in new}
            if changed:
                status = ("added" if old is None else "removed" if new is None else
                          ("bytes_changed" if old[0]["sha256"] != new[0]["sha256"] else "file_mode_changed")
                          if category == "aidl_source_file" else "commit_changed")
                delta.append({"category": category, "status": status, key: name, "before": old, "after": new})
                counts[category + ":" + status] += 1
    # Source occurrences are keyed by BOTH path and qualified field. Alternative files survive.
    left, right = defaultdict(list), defaultdict(list)
    for target, rows in [(left, before_occurrences), (right, after_occurrences)]:
        for row in rows:
            if row["declaring_field_identity"] is not None:
                target[row["path"], row["declaring_field_identity"]].append(row)
    comparable = (before["java"]["complete_source_field_denominator"] is not None
                  and after["java"]["complete_source_field_denominator"] is not None
                  and all(r["declaring_field_identity"] is not None for r in before_occurrences + after_occurrences))
    if comparable:
        for path, identity in sorted(left.keys() | right.keys()):
            old, new = left.get((path, identity)), right.get((path, identity))
            if old is None or new is None:
                status = "source_occurrence_added" if old is None else "source_occurrence_removed"
            elif [measured(r) for r in old] != [measured(r) for r in new]:
                status = "measured_metadata_changed"
            elif [[a["source"] for a in r["annotations"]] for r in old] != [[a["source"] for a in r["annotations"]] for r in new]:
                status = "annotation_syntax_or_order_changed"
            else:
                continue
            delta.append({"category": "compat_source_field", "status": status, "path": path,
                          "declaring_field_identity": identity, "before": old, "after": new})
            counts["compat_source_field:" + status] += 1
    old_ids = {r["change_id"]["value"] for r in before_occurrences if r["change_id"]["status"] == "resolved"}
    new_ids = {r["change_id"]["value"] for r in after_occurrences if r["change_id"]["status"] == "resolved"}
    complete_ids = before["java"]["complete_resolved_metadata"] and after["java"]["complete_resolved_metadata"]
    return delta, {"categories": dict(sorted(counts.items())), "compat_source_presence_comparable": comparable,
                   "complete_resolved_metadata_comparable": complete_ids,
                   "resolved_id_additions": sorted(new_ids - old_ids) if complete_ids else None,
                   "resolved_id_removals": sorted(old_ids - new_ids) if complete_ids else None,
                   "local_or_anonymous_occurrences_not_paired": sum(r["declaring_field_identity"] is None for r in before_occurrences + after_occurrences)}


UNMEASURED = {
    "source_selection": "core/java only; other framework directories/modules, build-selected membership and generated compat_config are unmeasured.",
    "java_semantics": "AST parsing and bounded lexical constant resolution are not full compiler attribution; inherited/external name resolution and unsupported expressions remain outside the measured rules.",
    "aidl_semantics": "Only complete source AIDL file bytes are compared. Imports, transactions, parcel layouts, Stable AIDL APIs and generated Binder ABI are unmeasured.",
    "runtime": "Per-app compatibility enable state, overrides, runtime providers, permissions, capabilities, SDK_INT behavior and device execution are unmeasured.",
    "module_equivalence": "Repository/tag/manifest reference matching is not APEX, built module, runtime image or SDK/NDK artifact equivalence.",
    "product_build": "Selected platform version fields are traced in build/release only; vendor/product overlays and aconfig flag selection are not evaluated.",
    "qualification": "CTS/CDD links and release names are reference metadata only. No CTS/CDD or API37 runtime qualification was performed.",
}


def render(summary):
    before, after = summary["profiles"]
    lines = [f'# {before["tag"]} to {after["tag"]} selected source comparison', "",
             "A bounded A07 source evidence increment: release reference profiles, core/java compatibility annotations and complete source-AIDL file bytes.", "",
             "## Measured source profiles", "",
             "| Profile | Selected platform SDK | Build ID | Java files parsed / total | Resolved ChangeId fields | Unresolved field candidates | Unique resolved IDs | Source AIDL files |",
             "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |"]
    for p in summary["profiles"]:
        lines.append(f'| {p["tag"]} | {p["sdk"]["full_spelling"]} | {p["build_id"]} | {p["parsed_java_files"]:,} / {p["java_files"]:,} | {p["changeid_occurrences"]} | {p["unresolved_field_candidates"]} | {p["unique_ids"]} | {p["aidl_files"]:,} |')
    lines += ["", "Counts are source occurrences and distinct resolved IDs, not compiled configuration membership or working runtime features.", "",
              "A parser error, unresolved annotation identity or unsupported value remains explicit. Observed counts are not a complete denominator when the corresponding JSON completeness field is null/false.", "",
              "## Changed observations", "", "| Category | Status | Rows |", "| --- | --- | ---: |"]
    for key, value in summary["comparison"]["categories"].items():
        category, status = key.split(":")
        lines.append(f"| {category} | {status} | {value} |")
    if not summary["comparison"]["compat_source_presence_comparable"]:
        lines += ["", "Whole-corpus compatibility field additions/removals and resolved-ID deltas are not computed: annotation identity or source-field completeness is unresolved. Raw candidates, inherited-scope reasons and the resolved subset remain in the inventory."]
    lines += ["", "The JSONL categories overlap; they are not additive API or defect counts. Duplicate declaring fields and IDs remain visible in profiles.json and compat-inventory.jsonl.", "",
              "The selected source configs are separate from the SDK36/SDK37 platform archives, NDK30 API slices, July API36.0 oracle and current CTS release. Earlier artifact properties are not rewritten.", "",
              "## Explicit limits", ""]
    lines += [f"- **{key.replace('_', ' ')}:** {value}" for key, value in UNMEASURED.items()]
    lines += ["", "See [the tool guide](../../source-release-diff.md) for pinned inputs, exact selection, parser rules, replay and validation.", ""]
    return "\n".join(lines)


def run(manifest, input_dir, project_lock, java, output):
    if manifest.get("schema_version") != 1:
        raise InputError("Unsupported manifest schema")
    lock_bytes = project_lock.read_bytes()
    if sha256(lock_bytes) != manifest["project_lock"]["sha256"]:
        raise InputError("Project lock differs from pinned selector profile")
    lock_tag, selectors = project_selectors(lock_bytes)
    if lock_tag != manifest["profiles"][0]["tag"]:
        raise InputError("Project source tag differs from before profile")
    profiles, inventory, occurrence_profiles = [], [], []
    with tempfile.TemporaryDirectory(prefix="gesher-source-diff-") as temporary:
        temp = Path(temporary); classes = temp / "classes"; classes.mkdir()
        subprocess.run([java, "-m", "jdk.compiler/com.sun.tools.javac.Main", "-encoding", "UTF-8", "-d", str(classes),
                        str(TOOL_DIR / "SourceCompatInventory.java")], check=True, capture_output=True, text=True)
        for selected in manifest["profiles"]:
            commits = defaultdict(set)
            for reference in selected["repository_references"].values():
                commits[reference["repository"]].add(reference["commit"])
            for pin in selected["archives"] + list(selected["blobs"].values()):
                if "repository" in pin and commits[pin["repository"]] != {pin["commit"]}:
                    raise InputError("Source input commit differs from selected release reference: " + pin["filename"])
            archives = {pin["role"]: read_archive(input_dir, pin) for pin in selected["archives"]}
            blobs = {role: checked_bytes(input_dir, pin) for role, pin in selected["blobs"].items()}
            references = selected_references(selected, checked_bytes(input_dir, selected["release_manifest"]), selectors)
            core = archives["core_java"]
            ast = parse_java(java, classes, core, temp / selected["id"])
            parsed, observations, occurrences = interpret_java(ast, core["files"], selected["id"])
            if parsed["parser"]["java_version"] != manifest["parser"]["java_version"]:
                raise InputError("JDK parser version differs from selected manifest")
            config = release_config_profile(archives["release_configs"]["contents"], archives["flag_declarations"]["contents"],
                                            archives["flag_values"]["contents"], blobs["release_map"], blobs["build_id"])
            schema = schema_metadata(archives["compat_schema"]["contents"]["config/platform-compat-config.xsd"])
            profile = {"id": selected["id"], "tag": selected["tag"], "source_references": references,
                       "release_config": config, "java": parsed, "compat_schema": schema,
                       "archives": [{**pin, "verification": {k: archives[pin["role"]][k] for k in
                                    ("git_tree", "file_inventory_sha256", "uncompressed_bytes")}} for pin in selected["archives"]],
                       "aidl_files": [f for f in core["files"] if f["path"].endswith(".aidl")],
                       "aidl_semantics_measured": False, "all_source_files": len(core["files"])}
            profiles.append(profile); inventory += observations; occurrence_profiles.append(occurrences)
    if len(profiles) != 2:
        raise InputError("Exactly two profiles are required")
    delta, comparison = compare_sources(*profiles, *occurrence_profiles)
    inventory.sort(key=lambda r: (r["profile"], r["path"], r["location"]["start"], r.get("name", "")))
    delta.sort(key=lambda r: (r["category"], r.get("path", r.get("repository", "")), r["status"]))
    inventory_bytes = b"".join(canonical(row) + b"\n" for row in inventory)
    delta_bytes = b"".join(canonical(row) + b"\n" for row in delta)
    summary = {"schema_version": 1, "comparison_id": manifest["comparison_id"], "manifest_sha256": digest(manifest),
               "generator_sha256": {name: sha256((TOOL_DIR / name).read_bytes()) for name in
                                    ("source_release_diff.py", "SourceCompatInventory.java")},
               "profile_data_sha256": digest(profiles), "compat_inventory_sha256": sha256(inventory_bytes),
               "delta_sha256": sha256(delta_bytes), "delta_records": len(delta), "comparison": comparison,
               "profiles": [{"id": p["id"], "tag": p["tag"], "sdk": p["release_config"]["sdk"],
                             "build_id": p["release_config"]["build_id"], "selectors": p["source_references"]["selectors"],
                             "repositories": p["source_references"]["unique_repositories"], "java_files": p["java"]["java_files"],
                             "parsed_java_files": p["java"]["parsed_without_errors"],
                             "complete_source_field_denominator": p["java"]["complete_source_field_denominator"],
                             "complete_resolved_metadata": p["java"]["complete_resolved_metadata"],
                             "changeid_occurrences": p["java"]["observed_changeid_field_occurrences"],
                             "unresolved_field_candidates": p["java"]["unresolved_changeid_field_candidates"],
                             "canonical_field_identities": p["java"]["canonical_field_identities"],
                             "unique_ids": p["java"]["resolved_unique_change_ids"], "validity_counts": p["java"]["validity_counts"],
                             "duplicate_declaring_fields": p["java"]["duplicate_declaring_fields"],
                             "duplicate_change_ids": p["java"]["duplicate_change_ids"], "aidl_files": len(p["aidl_files"])} for p in profiles],
               "unmeasured": UNMEASURED}
    output.mkdir(parents=True, exist_ok=True)
    for name, data in [("summary.json", summary), ("profiles.json", profiles)]:
        (output / name).write_text(json.dumps(data, indent=2, sort_keys=True, ensure_ascii=True) + "\n",
                                   encoding="utf-8", newline="\n")
    (output / "compat-inventory.jsonl").write_bytes(inventory_bytes)
    (output / "delta.jsonl").write_bytes(delta_bytes)
    (output / "README.md").write_text(render(summary), encoding="utf-8", newline="\n")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--project-lock", type=Path, default=TOOL_DIR.parent / "aosp.lock")
    parser.add_argument("--java", default="java", help="JDK21 java executable; jdk.compiler must be present")
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    result = run(json.loads(args.manifest.read_text(encoding="utf-8")), args.input_dir, args.project_lock, args.java, args.out_dir)
    print(f'Compared {result["comparison_id"]}: {result["delta_records"]} changed observations; no runtime or Binder ABI conclusion.')


if __name__ == "__main__":
    main()
