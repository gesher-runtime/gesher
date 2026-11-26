#!/usr/bin/env python3
"""Offline public/protected SDK member descriptors versus boot DEX definitions.

No app launches, execution, private string dumps, JNI or behavioral claims.
Archive limits and class/header validation reuse java_api_inventory.py.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import zipfile

import java_api_inventory as base

PUBLIC, PRIVATE, PROTECTED, STATIC, FINAL = 1, 2, 4, 8, 16
INTERFACE, ABSTRACT, NATIVE = 0x200, 0x400, 0x100
InvalidInput = base.InvalidInput
fail, checked, u16, u32, read_uleb = base.fail, base.checked, base.u16, base.u32, base.read_uleb


@dataclass(frozen=True)
class Limits(base.Limits):
    max_member_descriptor_bytes: int = 65535
    max_proto_descriptor_bytes_per_dex: int = 64 << 20
    max_type_list_entries: int = 1024


@dataclass(frozen=True, order=True)
class Member:
    kind: str
    name: str
    descriptor: str
    flags: int

    @property
    def key(self):
        return self.kind, self.name, self.descriptor


@dataclass
class Class:
    name: str
    flags: int
    superclass: str | None
    interfaces: tuple[str, ...]
    members: dict[tuple[str, str, str], Member]
    dex_source: tuple[str, str] | None = None


def modified_utf8(raw: bytes, expected_units: int | None = None) -> str:
    """Decode bounded Java/DEX modified UTF-8 (including UTF-16 surrogates)."""
    units, at = [], 0
    while at < len(raw):
        first = raw[at]
        if 0 < first < 0x80:
            value, size = first, 1
        elif 0xc0 <= first <= 0xdf:
            if at + 1 >= len(raw) or raw[at + 1] & 0xc0 != 0x80:
                fail("invalid_modified_utf8")
            value, size = ((first & 31) << 6) | (raw[at + 1] & 63), 2
            if value < 0x80 and value != 0:
                fail("invalid_modified_utf8")
        elif 0xe0 <= first <= 0xef:
            if at + 2 >= len(raw) or any(byte & 0xc0 != 0x80 for byte in raw[at + 1:at + 3]):
                fail("invalid_modified_utf8")
            value, size = ((first & 15) << 12) | ((raw[at + 1] & 63) << 6) | (raw[at + 2] & 63), 3
            if value < 0x800:
                fail("invalid_modified_utf8")
        else:
            fail("invalid_modified_utf8")
        units.append(value)
        at += size
    if expected_units is not None and len(units) != expected_units:
        fail("modified_utf8_length_mismatch")
    return b"".join(struct.pack("<H", unit) for unit in units).decode("utf-16-le", errors="surrogatepass")


def member_name(name: str, kind: str):
    if (not name or any(char in name for char in ".;/[\0") or
            (any(char in name for char in "<>") and (kind != "method" or name not in ("<init>", "<clinit>")))):
        fail("invalid_member_name")


def descriptor(raw: str, kind: str) -> str:
    """Validate the JVM descriptor grammar; void only as a method return."""
    try:
        data = raw.encode("ascii")
    except UnicodeError:
        fail("unsupported_descriptor_encoding")
    def one(at, allow_void=False):
        start = at
        while at < len(data) and data[at] == 91:
            at += 1
        if at - start > 255 or at == len(data):
            fail("invalid_member_descriptor")
        if data[at:at + 1] == b"L":
            end = data.find(b";", at)
            if end < 0:
                fail("invalid_member_descriptor")
            base.class_descriptor(data[at:end + 1])
            return end + 1
        if data[at:at + 1] in (b"B", b"C", b"D", b"F", b"I", b"J", b"S", b"Z"):
            return at + 1
        if allow_void and at == start and data[at:at + 1] == b"V":
            return at + 1
        fail("invalid_member_descriptor")
    if kind == "field":
        end = one(0)
    else:
        if not data.startswith(b"("):
            fail("invalid_member_descriptor")
        end = 1
        while end < len(data) and data[end] != 41:
            end = one(end)
        if end == len(data):
            fail("invalid_member_descriptor")
        end = one(end + 1, True)
    if end != len(data):
        fail("invalid_member_descriptor")
    return raw


def add_member(members, item):
    if item.key in members:
        fail("duplicate_member_definition")
    member_name(item.name, item.kind)
    descriptor(item.descriptor, item.kind)
    if item.name in ("<init>", "<clinit>") and not item.descriptor.endswith(")V"):
        fail("invalid_constructor_descriptor")
    members[item.key] = item


def class_members(data: bytes, limits=Limits()) -> Class:
    name, _, _ = base.class_file(data, limits)
    def be16(at):
        checked(data, at, 2)
        return struct.unpack_from(">H", data, at)[0]
    def be32(at):
        checked(data, at, 4)
        return struct.unpack_from(">I", data, at)[0]
    pool, at, index, count = {}, 10, 1, be16(8)
    while index < count:
        checked(data, at, 1)
        tag, at = data[at], at + 1
        if tag == 1:
            size = be16(at)
            checked(data, at + 2, size)
            pool[index] = (tag, data[at + 2:at + 2 + size])
            at += 2 + size
        elif tag == 7:
            pool[index], at = (tag, be16(at)), at + 2
        else:
            size = {3: 4, 4: 4, 5: 8, 6: 8, 8: 2, 9: 4, 10: 4, 11: 4, 12: 4,
                    15: 3, 16: 2, 17: 4, 18: 4, 19: 2, 20: 2}.get(tag)
            if size is None:
                fail("invalid_class_pool_tag")
            checked(data, at, size)
            at += size
            index += tag in (5, 6)
        index += 1
    def text(index, max_bytes=None):
        item = pool.get(index)
        if not item or item[0] != 1 or len(item[1]) > (max_bytes or limits.max_descriptor_bytes):
            fail("invalid_class_member_string")
        return modified_utf8(item[1])
    def cname(index):
        item = pool.get(index)
        if not item or item[0] != 7:
            fail("invalid_class_parent_index")
        return base.class_descriptor(("L" + text(item[1]) + ";").encode("ascii"))
    flags, parent = be16(at), be16(at + 4)
    at += 6
    interfaces = tuple(cname(be16(at + 2 + 2*i)) for i in range(be16(at)))
    at += 2 + 2*len(interfaces)
    def attributes(position):
        size = be16(position)
        position += 2
        for _ in range(size):
            text(be16(position))
            length = be32(position + 2)
            checked(data, position + 6, length)
            position += 6 + length
        return position
    members = {}
    for kind in ("field", "method"):
        count = be16(at)
        at += 2
        for _ in range(count):
            item = Member(kind, text(be16(at + 2)), text(be16(at + 4), limits.max_member_descriptor_bytes), be16(at))
            at = attributes(at + 6)
            add_member(members, item)
    at = attributes(at)
    if at != len(data):
        fail("class_trailing_data")
    return Class(name, flags, cname(parent) if parent else None, interfaces, members)


def dex_members(data: bytes, limits=Limits()) -> dict[str, Class]:
    base.dex_classes(data, limits)  # Header/table bounds, hashes, descriptors and definitions.
    data_at = u32(data, 108)
    strings_count, strings_at = u32(data, 56), u32(data, 60)
    type_count, type_at = u32(data, 64), u32(data, 68)
    proto_count, proto_at = u32(data, 72), u32(data, 76)
    field_count, field_at = u32(data, 80), u32(data, 84)
    method_count, method_at = u32(data, 88), u32(data, 92)
    class_count, class_at = u32(data, 96), u32(data, 100)
    if max(proto_count, field_count, method_count) > 65536:
        fail("dex_member_table_count_limit")
    @lru_cache(maxsize=None)
    def text(index):
        if index >= strings_count:
            fail("invalid_dex_member_string_index")
        offset = u32(data, strings_at + 4*index)
        if not data_at <= offset < len(data):
            fail("invalid_dex_string_offset")
        length, start = read_uleb(data, offset)
        if length > limits.max_descriptor_bytes:
            fail("member_string_length_limit")
        end = data.find(b"\0", start, min(len(data), start + limits.max_descriptor_bytes + 1))
        if end < 0:
            fail("unterminated_dex_member_string")
        return modified_utf8(data[start:end], length)
    types = [text(u32(data, type_at + 4*i)) for i in range(type_count)]
    def type_name(index):
        if index >= type_count:
            fail("invalid_dex_member_type_index")
        return types[index]
    def class_name(index):
        return base.class_descriptor(type_name(index).encode("ascii"))
    def type_list(offset):
        if not offset:
            return ()
        if offset < data_at or offset % 4:
            fail("invalid_dex_type_list_offset")
        count = u32(data, offset)
        if count > limits.max_type_list_entries:
            fail("dex_type_list_count_limit")
        checked(data, offset + 4, count*2)
        return tuple(u16(data, offset + 4 + i*2) for i in range(count))
    protos, descriptor_bytes = [], 0
    for i in range(proto_count):
        at = proto_at + i*12
        shorty = text(u32(data, at))
        result = type_name(u32(data, at + 4))
        args = [type_name(index) for index in type_list(u32(data, at + 8))]
        value = descriptor("(" + "".join(args) + ")" + result, "method")
        descriptor_bytes += len(value)
        if len(value) > limits.max_member_descriptor_bytes or descriptor_bytes > limits.max_proto_descriptor_bytes_per_dex:
            fail("member_descriptor_length_limit")
        expected_shorty = "".join("L" if value.startswith(("L", "[")) else value for value in [result, *args])
        if shorty != expected_shorty:
            fail("dex_proto_shorty_mismatch")
        protos.append(value)
    fields, methods = [], []
    for kind, count, table, result in (("field", field_count, field_at, fields),
                                       ("method", method_count, method_at, methods)):
        for i in range(count):
            at = table + 8*i
            owner_type = type_name(u16(data, at))
            # method_ids may name an array owner (for example clone references);
            # it is a reference, never an array class definition.
            if kind == "method" and owner_type.startswith("["):
                owner = descriptor(owner_type, "field")
            else:
                owner = base.class_descriptor(owner_type.encode("ascii"))
            type_index = u16(data, at + 2)
            if kind == "method" and type_index >= proto_count:
                fail("invalid_dex_proto_index")
            signature = protos[type_index] if kind == "method" else descriptor(type_name(type_index), "field")
            name = text(u32(data, at + 4))
            member_name(name, kind)
            result.append((owner, name, signature))
    classes = {}
    for i in range(class_count):
        at = class_at + 32*i
        name = class_name(u32(data, at))
        parent = u32(data, at + 8)
        interfaces = tuple(class_name(index) for index in type_list(u32(data, at + 12)))
        members = {}
        position = u32(data, at + 24)
        if position:
            counts = []
            for _ in range(4):
                count, position = read_uleb(data, position)
                counts.append(count)
            if counts[0] + counts[1] > field_count or counts[2] + counts[3] > method_count:
                fail("dex_class_member_count_limit")
            for group, count in enumerate(counts):
                ids, kind, index = (fields, "field", 0) if group < 2 else (methods, "method", 0)
                for j in range(count):
                    delta, position = read_uleb(data, position)
                    if j and not delta:
                        fail("duplicate_or_unsorted_dex_member_index")
                    index += delta
                    if index >= len(ids):
                        fail("invalid_dex_encoded_member_index")
                    flags, position = read_uleb(data, position)
                    owner, member, signature = ids[index]
                    if owner != name:
                        fail("dex_member_owner_mismatch")
                    if kind == "field" and bool(flags & STATIC) != (group == 0):
                        fail("dex_field_storage_flag_mismatch")
                    if kind == "method":
                        code, position = read_uleb(data, position)
                        if code and (code < data_at or code % 4):
                            fail("invalid_dex_code_offset")
                        if code:
                            checked(data, code, 16)
                        if bool(code) == bool(flags & (NATIVE | ABSTRACT)):
                            fail("dex_code_access_flag_mismatch")
                        direct = bool(flags & (PRIVATE | STATIC)) or member in ("<init>", "<clinit>")
                        if direct != (group == 2):
                            fail("dex_method_storage_flag_mismatch")
                    add_member(members, Member(kind, member, signature, flags))
        classes[name] = Class(name, u32(data, at + 4), class_name(parent) if parent != 0xffffffff else None,
                              interfaces, members)
    return classes


def archive_members(path: Path, role: str, source_id: str, limits=Limits()):
    names, _, manifest = base.archive_inventory(path, role, source_id, limits)
    classes = {}
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            if role == "sdk" and info.filename.endswith(".class") and info.filename != "module-info.class" and not info.filename.startswith("META-INF/"):
                cls = class_members(archive.read(info), limits)
                parsed = {cls.name: cls}
            elif role == "boot" and base.DEX_ENTRY.fullmatch(info.filename):
                parsed = dex_members(archive.read(info), limits)
                for cls in parsed.values():
                    cls.dex_source = source_id, info.filename
            else:
                continue
            if classes.keys() & parsed.keys():
                fail("duplicate_class_in_archive")
            classes.update(parsed)
    if classes.keys() != names:
        fail("member_class_inventory_mismatch")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1 << 20):
            digest.update(chunk)
    if digest.hexdigest() != manifest["sha256"]:
        fail("input_changed_during_inventory")
    manifest["encoded_member_definitions"] = sum(len(cls.members) for cls in classes.values())
    return classes, manifest


class Resolver:
    """Conservative descriptor lookup, with unknown or ambiguous resolution retained."""
    def __init__(self, classes, boot_classes=None):
        self.classes = classes
        self.boot_classes = classes if boot_classes is None else boot_classes

    def definition(self, name, referring_class):
        # A boot-defined class can resolve its own references through boot only.
        visible = self.boot_classes if referring_class in self.boot_classes else self.classes
        return visible.get(name)

    @staticmethod
    def parents(cls):
        return (*cls.interfaces, cls.superclass) if cls.superclass else cls.interfaces

    @lru_cache(maxsize=None)
    def ancestry(self, name):
        seen, unknown, stack = set(), set(), [(name, name, frozenset())]
        while stack:
            current, referring_class, active = stack.pop()
            cls = self.definition(current, referring_class)
            if cls is None:
                unknown.add(current)
                continue
            if current in active:
                fail("cyclic_class_hierarchy")
            if current in seen:
                continue
            seen.add(current)
            stack.extend((parent, current, active | {current}) for parent in self.parents(cls))
        return frozenset(seen), frozenset(unknown)

    def field(self, name, key, active=frozenset(), referring_class=None):
        cls = self.definition(name, referring_class)
        if cls is None:
            return [], True
        if name in active:
            fail("cyclic_class_hierarchy")
        if key in cls.members:
            return [(name, cls.members[key])], False
        unknown = False
        for parent in self.parents(cls):
            found, incomplete = self.field(parent, key, active | {name}, name)
            unknown |= incomplete
            if found:
                return found, unknown
        return [], unknown

    def method(self, name, key):
        cls, lineage, unknown = self.classes[name], [], False
        if key[1] in ("<init>", "<clinit>"):
            item = cls.members.get(key)
            return ([(name, item)] if item else []), False
        if cls.flags & INTERFACE:
            if key in cls.members:
                return [(name, cls.members[key])], False
            obj = self.definition("java.lang.Object", cls.name)
            if obj is None:
                unknown = True
            elif key in obj.members and obj.members[key].flags & PUBLIC and not obj.members[key].flags & STATIC:
                return [(obj.name, obj.members[key])], False
            lineage = [cls]
        else:
            seen, current = set(), cls
            while current:
                if current.name in seen:
                    fail("cyclic_class_hierarchy")
                seen.add(current.name)
                lineage.append(current)
                if key in current.members:
                    return [(current.name, current.members[key])], unknown
                # Signature-polymorphic call-site descriptors are not in SDK declarations;
                # exact native declarations remain ordinary rows in this inventory.
                if not current.superclass:
                    break
                current = self.definition(current.superclass, current.name)
                if current is None:
                    unknown = True
        interfaces = set()
        stack = [(parent, item.name) for item in lineage for parent in item.interfaces]
        while stack:
            parent, referring_class = stack.pop()
            target = self.definition(parent, referring_class)
            if target is None:
                unknown = True
            elif parent not in interfaces:
                interfaces.add(parent)
                stack.extend((dependency, parent) for dependency in target.interfaces)
        matches = [(parent, self.classes[parent].members[key]) for parent in sorted(interfaces)
                   if parent in self.classes and key in self.classes[parent].members
                   and not self.classes[parent].members[key].flags & (PRIVATE | STATIC)]
        maximal = [(parent, item) for parent, item in matches
                   if not any(parent != other and parent in self.ancestry(other)[0] for other, _ in matches)]
        concrete = [(parent, item) for parent, item in maximal if not item.flags & ABSTRACT]
        return (concrete if len(concrete) == 1 else maximal), unknown

    def lookup(self, name, key):
        return self.field(name, key) if key[0] == "field" else self.method(name, key)


def flag_issues(expected: Member, actual: Member):
    issues = []
    if bool(expected.flags & STATIC) != bool(actual.flags & STATIC):
        issues.append("static_instance_mismatch")
    if expected.flags & PUBLIC and not actual.flags & PUBLIC:
        issues.append("public_visibility_reduced")
    elif expected.flags & PROTECTED and not actual.flags & (PUBLIC | PROTECTED):
        issues.append("protected_visibility_reduced")
    if actual.flags & FINAL and not expected.flags & FINAL:
        issues.append("new_final_restriction")
    if expected.kind == "method" and actual.flags & ABSTRACT and not expected.flags & ABSTRACT:
        issues.append("concrete_became_abstract")
    return issues


def descriptor_classes(signature):
    # Descriptors have already been validated. Never scan arbitrary pool strings.
    return {value.replace("/", ".") for value in re.findall(r"L([^;]+);", signature)}


def compare_member(sdk_class, item, available, resolver, sdk_names, lookup=None):
    if sdk_class.name not in available:
        return {"status": "declaring_class_absent"}
    found, unknown = resolver.lookup(sdk_class.name, item.key) if lookup is None else lookup
    if not found:
        lineage, unresolved = resolver.ancestry(sdk_class.name)
        count = sum(1 for name in lineage if name in available for other in available[name].members.values()
                    if other.kind == item.kind and other.name == item.name and other.descriptor != item.descriptor
                    and (item.name != "<init>" or name == sdk_class.name))
        result = {"status": "unresolved_hierarchy" if unknown else "absent_from_present_class",
                  "other_descriptor_declarations_with_same_name": count}
        if unknown:
            result["unresolved_parent_count"] = len(unresolved)
        return result
    flags = sorted({issue for _, actual in found for issue in flag_issues(item, actual)})
    actual_class = available[sdk_class.name]
    if bool(sdk_class.flags & INTERFACE) != bool(actual_class.flags & INTERFACE):
        flags.append("class_interface_mismatch")
    if sdk_class.flags & PUBLIC and not actual_class.flags & PUBLIC:
        flags.append("declaring_class_public_visibility_reduced")
    if not sdk_class.flags & FINAL and actual_class.flags & FINAL:
        flags.append("declaring_class_new_final_restriction")
    missing_types = sorted(name for name in descriptor_classes(item.descriptor)
                           if any(resolver.definition(name, owner) is None for owner, _ in found))
    status = "exact_descriptor_present"
    if flags:
        status = "exact_descriptor_possible_flag_mismatch"
    if unknown or len(found) > 1:
        status = "exact_descriptor_resolution_uncertain"
    result = {"status": status, "location": "declared" if any(owner == sdk_class.name for owner, _ in found) else "inherited",
              "actual_access_flags": sorted({actual.flags for _, actual in found})}
    if flags:
        result["possible_mismatches"] = flags
    if missing_types:
        key = ("descriptor_types_absent_from_boot" if resolver.boot_classes is available
               else "descriptor_types_absent_from_defining_loader")
        result[key] = missing_types
    if unknown:
        result["unresolved_parent_count"] = len(resolver.ancestry(sdk_class.name)[1])
    if len(found) > 1:
        result["interface_declaration_candidates"] = len(found)
    result["resolved_owners"] = sorted({owner if owner in sdk_names else "<non-sdk declaration>" for owner, _ in found})
    return result


def dex_origin(source):
    source_id, dex_entry = source
    return {"source_id": source_id, "dex_entry": dex_entry}


def inventory(sdk: Path, boots: list[Path], replacements=None, limits=Limits(), sdk_api=36,
              loader_jars=(), loader_name=None):
    replacements = replacements or {}
    if not boots or any(value <= 0 for value in asdict(limits).values()):
        fail("invalid_inventory_bounds_or_boot_list")
    if bool(loader_jars) != (loader_name is not None) or (
            loader_name is not None and not re.fullmatch(r"[a-z][a-z0-9_.-]{0,63}", loader_name)):
        fail("invalid_loader_profile")
    inputs = [sdk, *boots, *replacements.values(), *loader_jars]
    if len(inputs) > limits.max_archives or len({path.name for path in boots}) != len(boots):
        fail("archive_count_or_duplicate_boot_name")
    if set(replacements) - {path.name for path in boots}:
        fail("candidate_replacement_has_no_base_input")
    if sum(base.safe_file(path, limits).stat().st_size for path in inputs) > limits.max_total_archive_bytes:
        fail("total_archive_bytes_limit")
    sdk_classes, manifest = archive_members(sdk, "sdk", "sdk", limits)
    manifests, installed, candidate = [manifest], {}, {}
    duplicates = {"installed": 0, "candidate": 0}
    duplicate_providers = {"installed": Counter(), "candidate": Counter()}
    def merge(target, classes, role, source_id):
        duplicates[role] += len(target.keys() & classes.keys())
        # The supplied boot order is deliberate: the first definition wins.
        for name, item in classes.items():
            if name in target:
                selected_source_id, _ = target[name].dex_source
                duplicate_providers[role][(selected_source_id, source_id)] += 1
            else:
                target[name] = item
    for index, path in enumerate(boots, 1):
        classes, manifest = archive_members(path, "boot", f"boot-{index:02d}", limits)
        manifests.append(manifest)
        merge(installed, classes, "installed", manifest["source_id"])
        if path.name in replacements:
            classes, manifest = archive_members(replacements[path.name], "boot", f"candidate-{index:02d}", limits)
            manifest["replaces_source_id"] = f"boot-{index:02d}"
            manifests.append(manifest)
        merge(candidate, classes, "candidate", manifest["source_id"])
    targets = {"installed": installed}
    if replacements:
        targets["candidate"] = candidate
    resolvers = {name: Resolver(classes) for name, classes in targets.items()}
    loader_profiles = {}
    if loader_jars:
        libraries = []
        for index, path in enumerate(loader_jars, 1):
            source_id = f"loader-{index:02d}"
            classes, manifest = archive_members(path, "boot", source_id, limits)
            manifest["role"] = "loader"
            manifests.append(manifest)
            libraries.append((classes, source_id))
        for name, parent in tuple(targets.items()):
            profile = name + "_child"
            selected = dict(parent)
            duplicates[profile] = 0
            duplicate_providers[profile] = Counter()
            for classes, source_id in libraries:
                merge(selected, classes, profile, source_id)
            targets[profile] = selected
            resolvers[profile] = Resolver(selected, parent)
            loader_profiles[profile] = {"name": loader_name, "parent": name,
                                        "ordered_source_ids": [source_id for _, source_id in libraries]}
    summaries = {name: {"sdk_classes_present": len(sdk_classes.keys() & classes.keys()),
                        "sdk_classes_absent": len(sdk_classes.keys() - classes.keys()),
                        "duplicate_class_definitions_first_wins": duplicates[name],
                        "duplicate_provider_groups": [{"selected_source_id": selected, "skipped_source_id": skipped,
                                                       "class_definitions": count}
                                                      for (selected, skipped), count in sorted(duplicate_providers[name].items())],
                        "status_counts": Counter(), "kind_status_counts": defaultdict(Counter),
                        "families": defaultdict(Counter), "possible_mismatch_counts": Counter(),
                        "descriptor_present_with_absent_boot_types": 0,
                        "descriptor_present_with_absent_defining_loader_types": 0,
                        "inherited_exact_descriptor_rows": 0} for name, classes in targets.items()}
    rows, kinds, sdk_visibility = [], Counter(), Counter()
    for cname, cls in sorted(sdk_classes.items()):
        sdk_visibility["public" if cls.flags & PUBLIC else "other_classfile_visibility"] += 1
        for item in sorted(cls.members.values()):
            if not item.flags & (PUBLIC | PROTECTED) or item.name == "<clinit>":
                continue
            kind = "constructor" if item.name == "<init>" else item.kind
            kinds[kind] += 1
            row = {"class": cname, "kind": kind, "name": item.name, "descriptor": item.descriptor,
                   "sdk_access_flags": item.flags, "family": base.family(cname)[0]}
            for name, classes in targets.items():
                resolver = resolvers[name]
                lookup = None
                if cname in classes:
                    lookup = resolver.lookup(cname, item.key)
                    origins = sorted({(owner if owner in sdk_classes else "<non-sdk declaration>",
                                       classes[owner].dex_source) for owner, _ in lookup[0]})
                    provenance = {"declaring": dex_origin(classes[cname].dex_source),
                                  "resolved": [{"owner": owner, **dex_origin(origin)}
                                               for owner, origin in origins]}
                    row.setdefault("definition_sources", {})[name] = provenance
                result = compare_member(cls, item, classes, resolver, sdk_classes.keys(), lookup)
                row[name] = result
                summary = summaries[name]
                summary["status_counts"][result["status"]] += 1
                summary["kind_status_counts"][kind][result["status"]] += 1
                summary["families"][row["family"]][result["status"]] += 1
                summary["possible_mismatch_counts"].update(result.get("possible_mismatches", []))
                summary["descriptor_present_with_absent_boot_types"] += bool(result.get("descriptor_types_absent_from_boot"))
                summary["descriptor_present_with_absent_defining_loader_types"] += bool(
                    result.get("descriptor_types_absent_from_defining_loader"))
                summary["inherited_exact_descriptor_rows"] += result.get("location") == "inherited"
            rows.append(row)
    def plain(value):
        if isinstance(value, dict):
            return {key: plain(item) for key, item in sorted(value.items())}
        return value
    report = {"schema_version": 1, "scope": "SDK public/protected field, constructor and method declarations; exact JVM descriptors on selected boot path",
              "declared_sdk_api": sdk_api, "sdk_api_claim": "caller_declared; exact input hash recorded",
              "not_a_defect_or_behavior_count": True, "limits": asdict(limits), "inputs": manifests,
              "sdk_class_definitions": len(sdk_classes), "sdk_classfile_visibility": dict(sdk_visibility),
              "sdk_public_protected_member_declarations": len(rows), "sdk_member_kinds": dict(kinds),
              "comparisons": plain(summaries),
              "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "base_parser_sha256": hashlib.sha256(Path(base.__file__).read_bytes()).hexdigest(),
              "references": ["https://source.android.com/docs/core/runtime/dex-format",
                             "https://docs.oracle.com/javase/specs/jvms/se21/html/jvms-4.html",
                             "https://docs.oracle.com/javase/specs/jvms/se21/html/jvms-5.html#jvms-5.4.3"],
              "limitations": ["SDK stubs omit hidden APIs and do not establish all Android contracts or required feature providers.",
                              "Each SDK declaration is counted once; inherited SDK members are not expanded for every subclass.",
                              "Runtime superclass/interface lookup avoids false declaration-only absences. Constructors are never inherited; private/static superinterface methods are excluded.",
                              "An unresolved parent yields unresolved_hierarchy rather than definitive absence. Ambiguous interface resolution remains uncertain.",
                              "Exact descriptors include field type, method parameters and return type. Covariant returns require an exact bridge/inherited descriptor; generic signatures are not compared.",
                              "Visibility/static/final/abstract/class-kind checks are possible mismatches. Protected receiver rules, class-loader constraints, module exports, inner-class source access, default method execution and polymorphic call sites are not fully evaluated.",
                              "Presence does not assess code bodies, native registration, values/constants, exceptions, annotations, permissions, services or behavior.",
                              "Boot descriptor type absence concerns the selected boot inputs. The child model checks descriptor types from each member owner. Desugaring is not assessed.",
                              "Supplied boot order uses first definition. Runtime class-loader ordering was not executed or independently proved.",
                              "No runtime private member names, arbitrary strings, absolute input paths or bytecode bodies are written to the report."]}
    if loader_profiles:
        report["scope"] = "SDK public/protected member descriptors on selected boot paths and an explicit parent-first child"
        report["loader_profiles"] = loader_profiles
        report["limitations"].append(
            "The child profile models one ordered DEX path delegating to boot. Separate LoadedApk "
            "shared-library graphs, delegate-last loaders and app isolation need runtime evidence.")
    if replacements:
        changes = [row for row in rows if row["installed"] != row["candidate"]]
        report["candidate_delta"] = {"changed_member_rows": len(changes),
                                     "status_transitions": dict(Counter(row["installed"]["status"] + " -> " + row["candidate"]["status"] for row in changes))}
    return report, rows


def markdown(report):
    lines = ["# Static Java member availability", "", report["scope"], "",
             f"Reference: {report['sdk_class_definitions']:,} SDK class definitions; {report['sdk_public_protected_member_declarations']:,} public/protected member declarations.",
             "Each declaration is counted once. Members on absent classes are counted separately from absences on present classes.",
             "Presence is structural evidence, with no claim about behavior. Full per-declaration results are in `java-member-availability.jsonl`.", "",
             "| Status | Installed | Candidate |", "| --- | ---: | ---: |"]
    comparisons = report["comparisons"]
    if report.get("loader_profiles"):
        lines[-2:] = ["| Status | " + " | ".join(comparisons) + " |",
                      "| --- | " + " | ".join("---:" for _ in comparisons) + " |"]
    names = comparisons if report.get("loader_profiles") else ("installed", "candidate")
    statuses = sorted({status for summary in comparisons.values() for status in summary["status_counts"]})
    for status in statuses:
        counts = (f"{comparisons.get(name, {}).get('status_counts', {}).get(status, 0):,}" for name in names)
        lines.append(f"| {status} | " + " | ".join(counts) + " |")
    if report.get("loader_profiles"):
        lines += ["", "## Supplied child class paths", ""]
        for name, profile in report["loader_profiles"].items():
            ordered = ", ".join(profile["ordered_source_ids"])
            lines.append(f"- `{name}` models `{profile['name']}` with parent `{profile['parent']}` and ordered inputs {ordered}.")
    lines += ["", "## Definitively absent boot descriptors on present classes by family", "",
              "| Family | Installed | Candidate |", "| --- | ---: | ---: |"]
    for family, counts in comparisons["installed"]["families"].items():
        lines.append(f"| {family} | {counts.get('absent_from_present_class', 0):,} | {comparisons.get('candidate', {}).get('families', {}).get(family, {}).get('absent_from_present_class', 0):,} |")
    lines += ["", "## Interpretation and limits", ""]
    lines += [f"- {limit}" for limit in report["limitations"]]
    lines += ["", "## Reference formats and resolution", ""]
    lines += [f"- [{url.split('/')[2]}]({url})" for url in report["references"]]
    lines += ["", "## Input hashes", "", "| Input | SHA-256 |", "| --- | --- |"]
    lines += [f"| {item['source_id']} / {item['name']} | `{item['sha256']}` |" for item in report["inputs"]]
    lines += ["", f"Generator SHA-256: `{report['generator_sha256']}`; base parser SHA-256: `{report['base_parser_sha256']}`."]
    return "\n".join(lines) + "\n"


def safe_output_targets(outdir: Path, inputs):
    """Check parents and dangling links before creating directories or files."""
    import stat
    targets = [outdir / name for name in ("java-member-availability.jsonl", "java-member-summary.json", "java-member-availability.md")]
    normalized_inputs = {path.absolute() for path in inputs}
    if any(path.absolute() in normalized_inputs for path in targets):
        fail("output_would_overwrite_input")
    for path in [outdir, *outdir.absolute().parents, *targets]:
        try:
            info = path.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            fail("reparse_or_symbolic_output_path")
    return targets


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-jar", required=True, type=Path)
    parser.add_argument("--sdk-api-level", default=36, type=int)
    parser.add_argument("--boot-jar", required=True, action="append", type=Path)
    parser.add_argument("--candidate-replace", action="append", default=[], metavar="BOOT_NAME=JAR")
    parser.add_argument("--loader-name", help="Label for one hypothetical parent-first child of boot.")
    parser.add_argument("--loader-jar", action="append", default=[], type=Path,
                        help="DEX archive in the child's search order. Repeat for additional archives.")
    parser.add_argument("--outdir", type=Path, default=Path("out/compat-audit/java-members"))
    args = parser.parse_args(argv)
    try:
        replacements = {}
        for value in args.candidate_replace:
            name, sep, path = value.partition("=")
            if not sep or name in replacements:
                fail("invalid_candidate_replacement")
            replacements[name] = Path(path)
        report, rows = inventory(args.sdk_jar, args.boot_jar, replacements, sdk_api=args.sdk_api_level,
                                 loader_jars=args.loader_jar, loader_name=args.loader_name)
        targets = safe_output_targets(args.outdir, [args.sdk_jar, *args.boot_jar, *replacements.values(),
                                                    *args.loader_jar])
        args.outdir.mkdir(parents=True, exist_ok=True)
        with targets[0].open("w", encoding="utf-8", newline="\n") as stream:
            digest = hashlib.sha256()
            for row in rows:
                line = json.dumps(row, sort_keys=True, ensure_ascii=True, separators=(",", ":")) + "\n"
                stream.write(line)
                digest.update(line.encode("utf-8"))
        report["member_inventory"] = {"name": targets[0].name, "rows": len(rows), "sha256": digest.hexdigest()}
        targets[1].write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        targets[2].write_text(markdown(report), encoding="utf-8")
        print(json.dumps({"sdk_members": len(rows), "comparisons": {name: item["status_counts"] for name, item in report["comparisons"].items()},
                          "candidate_delta": report.get("candidate_delta")}))
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile, RecursionError) as error:
        code = str(error) if isinstance(error, InvalidInput) else type(error).__name__
        print(f"Java member inventory failed: {code}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
