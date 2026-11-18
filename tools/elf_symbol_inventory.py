#!/usr/bin/env python3
"""Static ELF dynamic-symbol metadata, with version aliases kept separately.

This bounded reader uses section tables in ELF32/ELF64 little-endian ET_DYN
files. It neither loads libraries nor evaluates relocations, code, signatures,
or runtime resolution. Unsupported forms fail instead of losing observations.
"""
from __future__ import annotations

from collections import Counter
import struct


SHT_DYNSYM, SHT_STRTAB, SHT_DYNAMIC = 11, 3, 6
SHT_VERSYM, SHT_VERDEF, SHT_VERNEED = 0x6fffffff, 0x6ffffffd, 0x6ffffffe
BINDINGS = {0: "LOCAL", 1: "GLOBAL", 2: "WEAK", 10: "GNU_UNIQUE"}
TYPES = {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 3: "SECTION", 4: "FILE", 5: "COMMON", 6: "TLS", 10: "GNU_IFUNC"}
VISIBILITIES = {0: "DEFAULT", 1: "INTERNAL", 2: "HIDDEN", 3: "PROTECTED"}
MAX_FILE_BYTES, MAX_SECTIONS, MAX_SYMBOLS = 32 << 20, 8192, 200_000


class UnsupportedElf(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise UnsupportedElf(message)


class Reader:
    def __init__(self, data):
        require(16 <= len(data) <= MAX_FILE_BYTES, "elf_file_size")
        require(data[:4] == b"\x7fELF" and data[4] in (1, 2), "elf_magic_or_class")
        require(data[5:7] == b"\x01\x01", "only_little_endian_elf_version_1_supported")
        self.data, self.bits = data, 32 if data[4] == 1 else 64
        fields = self.unpack("HHIIIIIHHHHHH" if self.bits == 32 else "HHIQQQIHHHHHH", 16)
        kind, machine, version, _, _, shoff, flags, ehsize, _, _, shentsize, shnum, shstrndx = fields
        require(kind == 3 and version == 1, "only_et_dyn_supported")
        require(ehsize == (52 if self.bits == 32 else 64), "unsupported_elf_header_size")
        require(0 < shnum <= MAX_SECTIONS and shstrndx < shnum, "extended_or_missing_section_indices_unsupported")
        fmt = "IIIIIIIIII" if self.bits == 32 else "IIQQQQIIQQ"
        require(shentsize == struct.calcsize("<" + fmt), "unsupported_section_header_size")
        self.span(shoff, shnum * shentsize)
        self.sections = []
        for index in range(shnum):
            name, typ, section_flags, _, offset, size, link, info, align, entsize = self.unpack(fmt, shoff + index * shentsize)
            self.sections.append({"index": index, "name_offset": name, "type": typ,
                                  "flags": section_flags, "offset": offset, "size": size,
                                  "link": link, "info": info, "align": align, "entsize": entsize})
            if typ != 8:  # SHT_NOBITS has no corresponding file byte range.
                self.span(offset, size)
        names = self.section(shstrndx, SHT_STRTAB)
        for section in self.sections:
            section["name"] = self.string(names, section["name_offset"])
        self.header = {"elf_class": self.bits, "endianness": "little", "elf_type": kind,
                       "machine": machine, "flags": flags, "osabi": data[7], "abi_version": data[8]}

    def span(self, offset, size):
        require(0 <= offset <= len(self.data) and 0 <= size <= len(self.data) - offset, "elf_range_outside_file")
        return self.data[offset:offset + size]

    def unpack(self, fmt, offset):
        size = struct.calcsize("<" + fmt)
        self.span(offset, size)
        return struct.unpack_from("<" + fmt, self.data, offset)

    def section(self, index, typ=None):
        require(0 <= index < len(self.sections), "invalid_linked_section")
        section = self.sections[index]
        require(typ is None or section["type"] == typ, "unexpected_linked_section_type")
        return section

    def of_type(self, typ):
        found = [section for section in self.sections if section["type"] == typ]
        require(len(found) <= 1, "multiple_sections_of_supported_type")
        return found[0] if found else None

    def string(self, section, offset):
        require(0 <= offset < section["size"], "string_offset_outside_table")
        start, limit = section["offset"] + offset, section["offset"] + section["size"]
        end = self.data.find(b"\0", start, limit)
        require(end >= 0, "unterminated_elf_string")
        return self.data[start:end].decode("utf-8", errors="strict")

    def local_unpack(self, section, fmt, relative):
        size = struct.calcsize("<" + fmt)
        require(0 <= relative and size <= section["size"] - relative, "version_record_outside_section")
        return self.unpack(fmt, section["offset"] + relative)

    def version_definitions(self):
        section = self.of_type(SHT_VERDEF)
        if section is None:
            return {}
        strings = self.section(section["link"], SHT_STRTAB)
        result, offset = {}, 0
        require(0 < section["info"] <= MAX_SYMBOLS, "unsupported_version_definition_count")
        for number in range(section["info"]):
            version, flags, index, count, _, auxiliary, following = self.local_unpack(section, "HHHHIII", offset)
            require(version == 1 and 0 < index < 0x8000 and count > 0, "unsupported_version_definition")
            require(index not in result, "duplicate_version_definition_index")
            names, at = [], offset + auxiliary
            require(count <= MAX_SYMBOLS and auxiliary >= 20, "unsupported_version_definition_auxiliary")
            for child in range(count):
                name, next_aux = self.local_unpack(section, "II", at)
                names.append(self.string(strings, name))
                if child + 1 < count:
                    require(next_aux >= 8, "invalid_version_definition_auxiliary_link")
                    at += next_aux
                else:
                    require(next_aux == 0, "extra_version_definition_auxiliary")
            result[index] = {"name": names[0], "parents": names[1:], "flags": flags}
            if number + 1 < section["info"]:
                require(following >= 20, "invalid_version_definition_link")
                offset += following
            else:
                require(following == 0, "extra_version_definition_record")
        return result

    def version_requirements(self):
        section = self.of_type(SHT_VERNEED)
        if section is None:
            return {}
        strings = self.section(section["link"], SHT_STRTAB)
        result, offset = {}, 0
        require(0 < section["info"] <= MAX_SYMBOLS, "unsupported_version_requirement_count")
        for number in range(section["info"]):
            version, count, filename, auxiliary, following = self.local_unpack(section, "HHIII", offset)
            require(version == 1 and 0 < count <= MAX_SYMBOLS and auxiliary >= 16, "unsupported_version_requirement")
            provider, at = self.string(strings, filename), offset + auxiliary
            for child in range(count):
                _, flags, index, name, next_aux = self.local_unpack(section, "IHHII", at)
                require(1 < index < 0x8000 and index not in result, "invalid_version_requirement_index")
                result[index] = {"name": self.string(strings, name), "provider": provider, "flags": flags}
                if child + 1 < count:
                    require(next_aux >= 16, "invalid_version_requirement_auxiliary_link")
                    at += next_aux
                else:
                    require(next_aux == 0, "extra_version_requirement_auxiliary")
            if number + 1 < section["info"]:
                require(following >= 16, "invalid_version_requirement_link")
                offset += following
            else:
                require(following == 0, "extra_version_requirement_record")
        return result

    def dynamic_names(self):
        section = self.of_type(SHT_DYNAMIC)
        if section is None:
            return {"soname": None, "needed": []}
        strings = self.section(section["link"], SHT_STRTAB)
        fmt, width = ("II", 8) if self.bits == 32 else ("QQ", 16)
        require(section["entsize"] == width and section["size"] % width == 0, "unsupported_dynamic_table_size")
        sonames, needed, terminated = [], [], False
        for offset in range(0, section["size"], width):
            tag, value = self.local_unpack(section, fmt, offset)
            if tag == 0:
                terminated = True
                break
            if tag == 1:
                needed.append(self.string(strings, value))
            elif tag == 14:
                sonames.append(self.string(strings, value))
        require(terminated and len(sonames) <= 1, "unsupported_dynamic_name_records")
        return {"soname": sonames[0] if sonames else None, "needed": needed}

    def inventory(self):
        symbols = self.of_type(SHT_DYNSYM)
        require(symbols is not None, "dynamic_symbol_table_required")
        strings = self.section(symbols["link"], SHT_STRTAB)
        fmt, width = ("IIIBBH", 16) if self.bits == 32 else ("IBBHQQ", 24)
        require(symbols["entsize"] == width and symbols["size"] % width == 0, "unsupported_dynamic_symbol_size")
        count = symbols["size"] // width
        require(0 < count <= MAX_SYMBOLS, "unsupported_dynamic_symbol_count")
        definitions, requirements = self.version_definitions(), self.version_requirements()
        require(not definitions.keys() & requirements.keys(), "overlapping_version_indices")
        version_names = {item["name"] for item in definitions.values()}
        versions = self.of_type(SHT_VERSYM)
        if versions is not None:
            require(versions["link"] == symbols["index"] and versions["size"] == count * 2
                    and versions["entsize"] in (0, 2), "unsupported_symbol_version_table")
        rows = []
        for index in range(count):
            values = self.unpack(fmt, symbols["offset"] + index * width)
            if self.bits == 32:
                name, value, size, info, other, section_index = values
            else:
                name, info, other, section_index, value, size = values
            require(section_index != 0xffff, "extended_symbol_section_index_unsupported")
            require(section_index < len(self.sections) or section_index in (0xfff1, 0xfff2), "unsupported_symbol_section_index")
            binding, typ = info >> 4, info & 15
            require(binding in BINDINGS and typ in TYPES, "unsupported_symbol_binding_or_type")
            symbol = self.string(strings, name)
            raw_version = self.unpack("H", versions["offset"] + index * 2)[0] if versions else 1
            version_index, version_hidden = raw_version & 0x7fff, bool(raw_version & 0x8000)
            version = definitions.get(version_index) or requirements.get(version_index)
            require(version_index <= 1 or version is not None, "unresolved_symbol_version_index")
            defined = section_index != 0
            version_kind = "definition" if version_index > 1 and version_index in definitions else "requirement" if version_index > 1 else None
            require(not defined or version_kind != "requirement", "defined_symbol_has_requirement_version")
            version_name = version["name"] if version_index > 1 else None
            visibility = other & 7
            require(visibility in VISIBILITIES, "extended_symbol_visibility_unsupported")
            # GNU ld may emit absolute zero-size objects named after version nodes.
            # Keep them as explicit metadata rather than API functions/data.
            version_label = (defined and section_index == 0xfff1 and value == 0 and size == 0
                             and typ == 1 and symbol in version_names)
            if index == 0:
                require(not symbol and not value and not size and not info and not other and not section_index,
                        "unsupported_null_symbol")
                category = "null_symbol"
            elif not defined:
                category = "undefined_reference"
            elif version_label:
                category = "absolute_version_label"
            elif binding == 0 or version_index == 0:
                category = "local_defined"
            elif visibility in (1, 2):
                category = "nonvisible_defined"
            elif not symbol or typ in (3, 4):
                category = "non_api_metadata"
            else:
                category = "defined_export"
            rows.append({
                "symbol": symbol, "category": category, "binding": BINDINGS[binding],
                "type": TYPES[typ], "visibility": VISIBILITIES[visibility], "other_raw": other,
                "definition_kind": "undefined" if not defined else "absolute" if section_index == 0xfff1 else "common" if section_index == 0xfff2 else "section",
                "object_size": size if typ in (1, 6) else None,
                "version": version_name, "version_kind": version_kind,
                "version_provider": version.get("provider") if version else None,
                "version_local": version_index == 0,
                "version_hidden_bit": version_hidden,
                "default_version": (not version_hidden) if version_kind == "definition" else None,
            })
        return {"header": self.header, "dynamic": self.dynamic_names(),
                "version_definitions": [{"index": index, **item} for index, item in sorted(definitions.items())],
                "version_requirements": [{"index": index, **item} for index, item in sorted(requirements.items())],
                "dynamic_symbol_count": count, "category_counts": dict(sorted(Counter(row["category"] for row in rows).items())),
                "symbols": rows}


def inventory(data):
    return Reader(data).inventory()
