"""Passive AMD64 PE32+ dependency-name inventory. No file discovery or loading.

The stream adapter accepts an ALREADY admitted, read-locked stage handle; it
does not establish admission/locking itself. Only CI may supply real tool bytes.
Local controls use synthetic memory only. Imports are not loader closure.
"""
from dataclasses import dataclass
import hashlib
import re
import struct

MAX_BYTES = 128 * 1024 * 1024
MAX_SECTIONS = 96
MAX_DESCRIPTORS = 512  # COMBINED visited regular/delay slots, including sentinels.
MAX_NAME_BYTES = 128  # Excludes the mandatory terminating NUL.
ADMITTED_NAMES = frozenset(("cdb.exe", "dbgeng.dll", "dbghelp.dll"))
U32_END = 1 << 32


class InvalidPE(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise InvalidPE(message)


@dataclass(frozen=True)
class Dependency:
    directory: str
    descriptor_rva: int
    name_rva: int
    raw_name: str
    normalized_name: str
    delay_attributes: int | None = None


class Image:
    def __init__(self, data):
        require(type(data) is bytes, "byte input required")
        require(64 <= len(data) <= MAX_BYTES, "PE input size")
        self.data = data
        require(data[:2] == b"MZ", "DOS signature")
        pe = self.u32(60)
        require(pe >= 64, "PE offset overlaps DOS header")
        self.span(pe, 24)
        require(data[pe:pe + 4] == b"PE\0\0", "PE signature")
        require(self.u16(pe + 4) == 0x8664, "only AMD64 supported")
        count = self.u16(pe + 6)
        require(1 <= count <= MAX_SECTIONS, "section count")
        optional_size = self.u16(pe + 20)
        optional = pe + 24
        self.span(optional, optional_size)
        require(optional_size >= 112, "truncated optional header")
        require(self.u16(optional) == 0x20B, "only PE32+ supported")
        image_size = self.u32(optional + 56)
        self.headers = self.u32(optional + 60)
        directories = self.u32(optional + 108)
        require(directories <= 16, "unsupported data-directory count")
        require(112 + directories * 8 <= optional_size, "truncated directories")
        table = optional + optional_size
        self.span(table, count * 40)
        require(table + count * 40 <= self.headers <= len(data), "header extent")
        require(self.headers <= image_size, "image/header size")
        self.directories = []
        for index in range(directories):
            self.directories.append((self.u32(optional + 112 + index * 8),
                                     self.u32(optional + 116 + index * 8)))
        self.sections = []
        virtual_ranges = [(0, self.headers)]
        file_ranges = [(0, self.headers)]
        for index in range(count):
            at = table + index * 40
            virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, at + 8)
            extent = max(virtual_size, raw_size)
            require(extent > 0 and rva > 0, "empty section extent")
            require(rva + extent <= U32_END and rva + extent <= image_size,
                    "section RVA overflow/image extent")
            self.disjoint(virtual_ranges, rva, rva + extent, "ambiguous section RVA")
            if raw_size:
                self.span(raw, raw_size)
                self.disjoint(file_ranges, raw, raw + raw_size, "overlapping file ranges")
            self.sections.append((rva, raw_size, raw))

    def span(self, offset, size):
        require(0 <= offset <= len(self.data) and 0 <= size <= len(self.data) - offset,
                "truncated file range")

    def u16(self, offset):
        self.span(offset, 2)
        return struct.unpack_from("<H", self.data, offset)[0]

    def u32(self, offset):
        self.span(offset, 4)
        return struct.unpack_from("<I", self.data, offset)[0]

    @staticmethod
    def disjoint(ranges, start, end, message):
        require(all(end <= lo or start >= hi for lo, hi in ranges), message)
        ranges.append((start, end))

    def mapped(self, rva, size):
        require(0 <= rva < U32_END and 0 < size <= U32_END - rva,
                "RVA range overflow")
        candidates = []
        if rva < self.headers and size <= self.headers - rva:
            candidates.append(rva)
        for start, raw_size, raw in self.sections:
            if start <= rva and rva - start < raw_size and size <= raw_size - (rva - start):
                candidates.append(raw + rva - start)
        require(len(candidates) == 1, "RVA is not uniquely file-backed")
        self.span(candidates[0], size)
        return candidates[0]

    def name(self, rva):
        require(rva != 0, "missing dependency name RVA")
        value = bytearray()
        for index in range(MAX_NAME_BYTES + 1):
            octet = self.data[self.mapped(rva + index, 1)]
            if octet == 0:
                require(value, "empty dependency name")
                return value.decode("ascii")
            require(index < MAX_NAME_BYTES, "dependency name exceeds cap")
            # Passive text ONLY: never treat returned names as filesystem paths.
            require(0x20 <= octet <= 0x7E, "dependency name is not printable ASCII")
            value.append(octet)
        raise InvalidPE("unterminated dependency name")

    def dependencies(self):
        result = []
        slots = 0
        facts = []
        for label, index, width in (("regular", 1, 20), ("delay", 13, 32)):
            rva, size = self.directories[index] if index < len(self.directories) else (0, 0)
            require((rva == 0) == (size == 0), "incomplete " + label + " directory")
            if not rva:
                facts.append((label, "absent", 0, 0))
                continue
            require(size >= width, "truncated " + label + " directory")
            offset = self.mapped(rva, size)
            terminated = False
            for relative in range(0, size - width + 1, width):
                slots += 1
                require(slots <= MAX_DESCRIPTORS, "combined descriptor cap")
                fields = struct.unpack_from("<" + "I" * (width // 4), self.data, offset + relative)
                if not any(fields):
                    terminated = True
                    break
                attributes = None
                if label == "delay":
                    attributes = fields[0]
                    require(attributes == 1,
                            "unsupported delay attributes (requires RVA mode=1): " + str(attributes))
                    name_rva = fields[1]
                else:
                    name_rva = fields[3]
                name = self.name(name_rva)
                result.append(Dependency(label, rva + relative, name_rva, name,
                                         name.lower(), attributes))
            require(terminated, "missing " + label + " directory terminator")
            facts.append((label, "present", rva, size))
        return {"dependencies": tuple(result), "directories": tuple(facts),
                "descriptor_slots": slots, "loader_closure": False}


def inspect_bytes(data):
    return Image(data).dependencies()


def inspect_admitted_stream(name, stream, entry):
    """Read once, at most cap+one sentinel, from caller's retained locked handle.

    entry is the exact admitted stage manifest row (name/bytes/sha256). No path
    opens, other manifests, discovery, trust checks or real local-tool reads.
    Caller must independently prove handle identity and lock lifetime in CI.
    """
    require(name in ADMITTED_NAMES, "only three admitted tool names")
    require(type(entry) is dict and set(entry) == {"name", "bytes", "sha256"},
            "manifest row shape")
    require(entry["name"] == name, "manifest name mismatch")
    require(type(entry["bytes"]) is int and 64 <= entry["bytes"] <= MAX_BYTES,
            "manifest byte count")
    require(type(entry["sha256"]) is str and re.fullmatch("[0-9a-f]{64}", entry["sha256"]),
            "manifest SHA256")
    data = stream.read(MAX_BYTES + 1)
    require(type(data) is bytes and len(data) <= MAX_BYTES, "bounded stream bytes")
    require(len(data) == entry["bytes"], "manifest size mismatch")
    require(hashlib.sha256(data).hexdigest() == entry["sha256"], "manifest hash mismatch")
    return inspect_bytes(data)
