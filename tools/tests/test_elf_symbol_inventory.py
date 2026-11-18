"""Normal ELF metadata fixtures; shared libraries are compiled but never loaded."""
from collections import Counter
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import elf_symbol_inventory as elf


def compiled_library(directory, name, source, *, bits=64, language="c", version_script=None,
                     libraries=(), extra_flags=()):
    """Build an isolated, dependency-free shared fixture; never execute its code.

    Other inventory tests can reuse this helper. A failed supported compiler
    invocation is an assertion failure, rather than silently skipping a test.
    """
    compiler = shutil.which("c++" if language == "c++" else "cc")
    if compiler is None:
        raise unittest.SkipTest("A host C/C++ compiler is required for compiled ELF fixtures")
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    output = directory / name
    source_path = directory / (name + (".cc" if language == "c++" else ".c"))
    source_path.write_text(source, encoding="ascii")
    command = [compiler, f"-m{bits}", "-shared", "-fPIC", "-nostdlib", "-fno-stack-protector",
               "-Wl,--build-id=none", f"-Wl,-soname,{name}", str(source_path), "-o", str(output)]
    if version_script is not None:
        script_path = directory / (name + ".map")
        script_path.write_text(version_script, encoding="ascii")
        command.append(f"-Wl,--version-script={script_path}")
    command.extend(extra_flags)
    if libraries:
        command.append("-Wl,--no-as-needed")
        command.extend(str(path) for path in libraries)
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(f"Fixture compilation failed ({result.returncode}):\n{result.stderr}")
    return output


PROVIDER_SOURCE = r'''
int previous_implementation(void) { return 1; }
int current_implementation(void) { return 2; }
__asm__(".symver previous_implementation,versioned_api@V1");
__asm__(".symver current_implementation,versioned_api@@V2");
__asm__(".globl absolute_limit\n.set absolute_limit,17");
int public_data[3];
'''
PROVIDER_VERSIONS = "V1 { global: versioned_api; }; V2 { global: versioned_api; public_data; absolute_limit; local: *; } V1;"


def static_elf_fixture(*, bits=64, machine=None, symbols=()):
    """Deterministic unversioned ELF table fixture; never a runtime provider.

    Symbol dictionaries use name plus optional numeric binding/type/visibility
    and size; defaults are GLOBAL/FUNC/DEFAULT/0. Empty symbols gives a null-only
    dynamic table. The exact machine may be supplied for ABI/ZIP profile tests.
    """
    if bits not in (32, 64):
        raise ValueError("Fixture bits must be 32 or 64")
    machine = (3 if bits == 32 else 62) if machine is None else machine
    entries = sorted(symbols, key=lambda item: item.get("binding", 1) != 0)
    strings, offsets = bytearray(b"\0"), {}
    for name in (item["name"] for item in entries):
        offsets[name] = len(strings)
        strings.extend(name.encode("ascii") + b"\0")
    symbol_width, header_width, section_width = (16, 52, 40) if bits == 32 else (24, 64, 64)
    encoded_symbols = bytearray(symbol_width)  # STN_UNDEF.
    for item in entries:
        name = offsets[item["name"]]
        info = item.get("binding", 1) << 4 | item.get("type", 2)
        visibility, size = item.get("visibility", 0), item.get("size", 0)
        encoded_symbols.extend(struct.pack("<IIIBBH", name, 0, size, info, visibility, 1) if bits == 32
                               else struct.pack("<IBBHQQ", name, info, visibility, 1, 0, size))
    section_strings = b"\0.data\0.dynstr\0.dynsym\0.shstrtab\0"
    data = bytearray(header_width)
    sections = [(0,) * 10]
    for name, kind, flags, payload, link, info, alignment, entry_size in (
        (".data", 1, 3, bytes(max([8, *(item.get("size", 0) for item in entries)])), 0, 0, 8, 0),
        (".dynstr", 3, 2, strings, 0, 0, 1, 0),
        (".dynsym", 11, 2, encoded_symbols, 2, 1 + sum(item.get("binding", 1) == 0 for item in entries), bits // 8, symbol_width),
        (".shstrtab", 3, 0, section_strings, 0, 0, 1, 0),
    ):
        data.extend(bytes((-len(data)) % alignment))
        position = len(data)
        data.extend(payload)
        sections.append((section_strings.index(name.encode()), kind, flags, 0,
                         position, len(payload), link, info, alignment, entry_size))
    data.extend(bytes((-len(data)) % 8))
    table_offset = len(data)
    for section in sections:
        data.extend(struct.pack("<IIIIIIIIII" if bits == 32 else "<IIQQQQIIQQ", *section))
    data[:16] = b"\x7fELF" + bytes([1 if bits == 32 else 2, 1, 1]) + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH" if bits == 32 else "<HHIQQQIHHHHHH", data, 16,
                     3, machine, 1, 0, 0, table_offset, 0, header_width, 0, 0,
                     section_width, len(sections), 4)
    return bytes(data)


def readelf_records(path):
    """Independent GNU readelf oracle for the compared dynamic-symbol fields."""
    readelf = shutil.which("readelf")
    if readelf is None:
        raise unittest.SkipTest("GNU readelf is required for independent ELF fixture checks")
    versions = subprocess.run([readelf, "--version-info", "--wide", str(path)],
                              capture_output=True, text=True, check=True).stdout
    import re
    definition_names, in_definitions, in_symbols = set(), False, False
    symbol_versions = {}
    for line in versions.splitlines():
        if line.startswith("Version symbols section"):
            in_symbols, in_definitions = True, False
        elif line.startswith("Version definition section"):
            in_definitions, in_symbols = True, False
        elif line.startswith("Version needs section"):
            in_definitions, in_symbols = False, False
        if in_definitions and "Name:" in line:
            definition_names.add(line.split("Name:", 1)[1].strip().split()[0])
        if in_symbols and re.match(r"^\s*[0-9a-f]+:", line):
            offset, fields = line.split(":", 1)
            start = int(offset, 16)
            for number, (index, hidden, name) in enumerate(re.findall(r"([0-9a-f]+)(h?)\s*\(([^)]*)\)", fields)):
                symbol_versions[start + number] = (int(index, 16), bool(hidden), name)
    text = subprocess.run([readelf, "--dyn-syms", "--wide", str(path)],
                          capture_output=True, text=True, check=True).stdout
    rows = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 7 or not parts[0].endswith(":") or not parts[0][:-1].isdigit():
            continue
        index, value, size, kind, binding, visibility, section = parts[:7]
        name = parts[7] if len(parts) > 7 else ""
        index, value, size = int(index[:-1]), int(value, 16), int(size)
        symbol, separator, version = name.partition("@@" if "@@" in name else "@")
        version = version if separator else None
        version_index, version_hidden, table_name = symbol_versions.get(index, (1, False, "*global*"))
        if version_index > 1:
            version = table_name
        binding = "GNU_UNIQUE" if binding == "UNIQUE" else binding
        kind = "GNU_IFUNC" if kind == "IFUNC" else kind
        defined = section != "UND"
        if index == 0:
            category = "null_symbol"
        elif not defined:
            category = "undefined_reference"
        elif section == "ABS" and kind == "OBJECT" and value == size == 0 and symbol in definition_names:
            category = "absolute_version_label"
        elif binding == "LOCAL" or version_index == 0:
            category = "local_defined"
        elif visibility in ("HIDDEN", "INTERNAL"):
            category = "nonvisible_defined"
        elif not symbol or kind in ("SECTION", "FILE"):
            category = "non_api_metadata"
        else:
            category = "defined_export"
        rows.append({
            "symbol": symbol, "category": category, "binding": binding, "type": kind,
            "visibility": visibility,
            "definition_kind": "undefined" if not defined else "absolute" if section == "ABS" else "common" if section == "COM" else "section",
            "object_size": size if kind in ("OBJECT", "TLS") else None,
            "version": version,
            "version_kind": ("definition" if defined else "requirement") if version else None,
            "default_version": not version_hidden if version and defined else None,
            "version_hidden_bit": version_hidden,
            "version_local": version_index == 0,
        })
    return rows


class ElfSymbolInventoryTests(unittest.TestCase):
    def checked_inventory(self, path):
        actual = elf.inventory(Path(path).read_bytes())
        expected = readelf_records(path)
        self.assertEqual(len(actual["symbols"]), len(expected))
        self.assertEqual([{key: row[key] for key in expected[index]} for index, row in enumerate(actual["symbols"])], expected)
        self.assertEqual(actual["category_counts"], dict(Counter(row["category"] for row in expected)))
        return actual

    def test_compiled_elf32_and_elf64_symbols_objects_tls_and_undefined_refs(self):
        source = r'''
long public_object[3];
__thread int thread_object;
__attribute__((weak)) int weak_object;
__attribute__((weak)) int weak_api(void) { return 1; }
__attribute__((visibility("protected"))) int protected_api(void) { return 2; }
__attribute__((visibility("hidden"))) int hidden_api(void) { return 3; }
extern int required_api(void);
extern int optional_api(void) __attribute__((weak));
int public_api(void) { return required_api() + (optional_api ? optional_api() : 0); }
'''
        with tempfile.TemporaryDirectory() as temporary:
            for bits in (32, 64):
                with self.subTest(bits=bits):
                    path = compiled_library(temporary, f"libnormal{bits}.so", source, bits=bits)
                    result = self.checked_inventory(path)
                    rows = {row["symbol"]: row for row in result["symbols"]}
                    self.assertEqual(result["header"]["elf_class"], bits)
                    self.assertEqual(result["header"]["machine"], 3 if bits == 32 else 62)
                    self.assertEqual(result["dynamic"], {"soname": path.name, "needed": []})
                    self.assertEqual(rows["public_object"]["object_size"], 3 * bits // 8)
                    self.assertEqual((rows["thread_object"]["type"], rows["thread_object"]["object_size"]), ("TLS", 4))
                    self.assertEqual((rows["weak_object"]["binding"], rows["weak_object"]["object_size"]), ("WEAK", 4))
                    self.assertEqual(rows["weak_api"]["binding"], "WEAK")
                    self.assertEqual(rows["protected_api"]["visibility"], "PROTECTED")
                    self.assertEqual(rows["protected_api"]["category"], "defined_export")
                    self.assertNotIn("hidden_api", rows)
                    self.assertEqual((rows["required_api"]["category"], rows["required_api"]["binding"]), ("undefined_reference", "GLOBAL"))
                    self.assertEqual((rows["optional_api"]["category"], rows["optional_api"]["binding"]), ("undefined_reference", "WEAK"))

    def test_version_aliases_parent_definitions_and_absolute_symbols(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = compiled_library(temporary, "libprovider.so", PROVIDER_SOURCE, version_script=PROVIDER_VERSIONS)
            result = self.checked_inventory(path)
            aliases = {row["version"]: row for row in result["symbols"] if row["symbol"] == "versioned_api"}
            self.assertEqual(set(aliases), {"V1", "V2"})
            self.assertEqual((aliases["V1"]["default_version"], aliases["V1"]["version_hidden_bit"]), (False, True))
            self.assertEqual((aliases["V2"]["default_version"], aliases["V2"]["version_hidden_bit"]), (True, False))
            self.assertTrue(all(row["category"] == "defined_export" and row["visibility"] == "DEFAULT" for row in aliases.values()))
            definitions = {item["name"]: item for item in result["version_definitions"]}
            self.assertEqual(definitions["V1"]["parents"], [])
            self.assertEqual(definitions["V2"]["parents"], ["V1"])
            rows = {row["symbol"]: row for row in result["symbols"] if row["symbol"] != "versioned_api"}
            self.assertEqual({rows[name]["category"] for name in ("V1", "V2")}, {"absolute_version_label"})
            self.assertEqual((rows["absolute_limit"]["category"], rows["absolute_limit"]["definition_kind"]), ("defined_export", "absolute"))

    def test_versioned_undefined_reference_retains_provider_and_needed_library(self):
        with tempfile.TemporaryDirectory() as temporary:
            provider = compiled_library(temporary, "libprovider.so", PROVIDER_SOURCE, version_script=PROVIDER_VERSIONS)
            consumer = compiled_library(temporary, "libconsumer.so", r'''
extern int versioned_api(void);
extern int optional_api(void) __attribute__((weak));
int consumer(void) { return versioned_api() + (optional_api ? optional_api() : 0); }
''', libraries=(provider,))
            result = self.checked_inventory(consumer)
            rows = {row["symbol"]: row for row in result["symbols"]}
            self.assertEqual(result["dynamic"], {"soname": "libconsumer.so", "needed": ["libprovider.so"]})
            self.assertEqual((rows["versioned_api"]["category"], rows["versioned_api"]["version_kind"], rows["versioned_api"]["version"]), ("undefined_reference", "requirement", "V2"))
            self.assertEqual(rows["versioned_api"]["version_provider"], "libprovider.so")
            self.assertIsNone(rows["versioned_api"]["default_version"])
            self.assertEqual(rows["optional_api"]["binding"], "WEAK")
            self.assertIsNone(rows["optional_api"]["version"])
            self.assertEqual([(item["name"], item["provider"]) for item in result["version_requirements"]], [("V2", "libprovider.so")])

    def test_gnu_unique_object_is_retained(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = compiled_library(temporary, "libunique.so", r'''
template <class T> struct Holder { static int value; };
template <class T> int Holder<T>::value = 7;
extern "C" int* unique_address(void) { return &Holder<int>::value; }
''', language="c++")
            result = self.checked_inventory(path)
            unique = [row for row in result["symbols"] if row["binding"] == "GNU_UNIQUE"]
            self.assertEqual(len(unique), 1)
            self.assertEqual((unique[0]["category"], unique[0]["type"], unique[0]["object_size"]), ("defined_export", "OBJECT", 4))

    def test_static_local_hidden_internal_and_protected_symbols(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "visibility.so"
            # Linkers normally remove/localize hidden definitions. This valid
            # section-table fixture retains those explicit dynamic rows.
            path.write_bytes(static_elf_fixture(symbols=[
                {"name": name, "binding": binding, "type": 1, "visibility": visibility, "size": 8}
                for name, binding, visibility in (
                    ("local_private", 0, 0), ("hidden_private", 1, 2), ("internal_private", 1, 1),
                    ("protected_public", 1, 3), ("strong_public", 1, 0),
                )
            ]))
            result = self.checked_inventory(path)
            rows = {row["symbol"]: row for row in result["symbols"]}
            self.assertEqual(rows["local_private"]["category"], "local_defined")
            self.assertEqual(rows["hidden_private"]["category"], "nonvisible_defined")
            self.assertEqual(rows["internal_private"]["category"], "nonvisible_defined")
            self.assertEqual(rows["protected_public"]["category"], "defined_export")
            self.assertEqual(rows["strong_public"]["category"], "defined_export")
            self.assertEqual([rows[name]["other_raw"] for name in ("hidden_private", "internal_private", "protected_public")], [2, 1, 3])
            self.assertEqual(result["category_counts"], {"null_symbol": 1, "local_defined": 1, "nonvisible_defined": 2, "defined_export": 2})


if __name__ == "__main__":
    unittest.main()
