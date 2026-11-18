#!/usr/bin/env python3
"""Execute the production registry against both architecture-filtered Bionic maps.

The unchanged Android generate-version-script.py creates the arm64/x86_64 maps.
Actual Registry, owned/internal lookup and SymbolTable code compile unchanged.
Registration keys are extracted from actual registration statements; function
addresses and registration side effects are explicit inert host seams. No guest
compatibility function, Windows bootstrap or complete ABI/N10 matrix is claimed.
--source-ref reproduces historical registry behavior with the same current gate.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAPS = {"libc": "libc/libc.map.txt", "libm": "libm/libm.map.txt",
        "libdl": "libdl/libdl.map.txt", "libdl_android": "libdl/libdl_android.map.txt"}
TOKEN = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')


def section(text: str, begin: str, end: str) -> str:
    if text.count(begin) != 1 or text.count(end) != 1:
        raise ValueError("actual registry source boundary changed: " + begin)
    return text[text.index(begin):text.index(end)]


def body_end(text: str, begin: int) -> int:
    depth = 1
    for token in TOKEN.finditer(text, begin):
        if token[0] == "{":
            depth += 1
        elif token[0] == "}":
            depth -= 1
        if depth == 0:
            return token.start()
    raise ValueError("unterminated actual registration body")


def registration_events(body: str, parameter: str) -> list[tuple[int, str, str]]:
    """Read key statements only; unrelated string data never becomes exports."""
    body = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/', "", body)
    directives = re.findall(r'(?m)^\s*#\s*(\w+)', body)
    if any(name not in ("define", "undef") for name in directives):
        raise ValueError("conditional registration needs an explicit host boundary")
    body = re.sub(r'(?m)^[ \t]*#(?:[^\n]*\\\n)*[^\n]*(?:\n|$)', "", body)
    events = []
    table = re.escape(parameter)
    # The actual xattr batch is a range of literal names assigned to the table.
    loop = re.compile(r'for\s*\(\s*const\s+char\s*\*\s*(\w+)\s*:\s*\{([^}]+)\}\s*\)\s*\{'
                      r'\s*' + table + r'\[\s*\1\s*\]\s*=[^;]+;\s*\}')
    for match in loop.finditer(body):
        values = re.findall(r'"([A-Za-z_]\w*)"', match[2])
        if not values or re.sub(r'"[A-Za-z_]\w*"|[\s,]', "", match[2]):
            raise ValueError("nonliteral registration range needs an explicit host boundary")
        events.extend((match.start(), "key", name) for name in values)
    body = loop.sub(lambda match: " " * len(match[0]), body)
    literal = re.compile(r'\b' + table + r'\s*\[\s*"([A-Za-z_]\w*)"\s*\]\s*=')
    for match in literal.finditer(body):
        events.append((match.start(), "key", match[1]))
    if re.search(r'\b' + table + r'\s*\[', literal.sub("", body)):
        raise ValueError("unrecognized table key expression in registration")
    if re.search(r'\b' + table + r'\.(?:insert|emplace|try_emplace|insert_or_assign)\s*\(', body):
        raise ValueError("new table insertion form needs an explicit host boundary")
    for match in re.finditer(r'\b(?:E|ENTRY|L)\(\s*"([A-Za-z_]\w*)"\s*,', body):
        events.append((match.start(), "key", match[1]))
    for match in re.finditer(r'\b(?:SELF|MATH2F|MATH2|MATHF|MATH|P)\(\s*([A-Za-z_]\w*)\s*\)', body):
        events.append((match.start(), "key", match[1]))
    for match in re.finditer(r'\b([A-Za-z_]\w*)\(\s*' + table + r'\s*\)', body):
        if match[1].startswith("Register"):
            events.append((match.start(), "call", match[1]))
        elif match[1] != "WrapSystemCacerts":
            raise ValueError("unrecognized registration-table helper: " + match[1])
    if not events:
        raise ValueError("actual registration function has no recognized keys/calls")
    return sorted(events, key=lambda event: event[0])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bionic-source", type=Path, required=True,
                        help="actual pinned Bionic checkout fetched by aosp.lock")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-ref", help="historical production registry/registration commit")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source_hashes = {}
    reference = None
    if args.source_ref:
        reference = subprocess.check_output(["git", "rev-parse", "--verify", "--end-of-options",
            args.source_ref + "^{commit}"], cwd=ROOT, text=True).strip()

    def read(name: str) -> str:
        data = (subprocess.check_output(["git", "show", reference + ":" + name], cwd=ROOT)
                if reference else (ROOT / name).read_bytes())
        source_hashes[name] = hashlib.sha256(data).hexdigest()
        return data.decode()

    shim = read("compat/bionic/shim.cc")
    registry = section(shim, "struct BionicRegistry {", "struct ProviderDefinition {")
    public = section(shim, "void* LookupBionicSymbol(const char* name) {", "void InstallBionicShim() {")
    table_type = section(read("compat/bionic/shim_internal.h"), "// Symbol name ->", "// Each part of the shim")
    functions = {}
    paths = (subprocess.check_output(["git", "ls-tree", "-r", "--name-only", reference,
        "--", "compat/bionic"], cwd=ROOT, text=True).splitlines() if reference else
        [str(path.relative_to(ROOT)) for path in (ROOT / "compat/bionic").glob("*.cc")])
    for name in sorted(path for path in paths if path.endswith(".cc")):
        text = read(name)
        for match in re.finditer(r'void (Register\w+)\(SymbolTable&\s*(\w+)\)\s*\{', text):
            if match[1] in functions:
                raise ValueError("duplicate registration definition: " + match[1])
            body = text[match.end():body_end(text, match.end())]
            functions[match[1]] = registration_events(body, match[2])
    # This production helper only changes addresses of existing keys. Its
    # implementation is hashed, and any new insertion requires gate adaptation.
    wrappers = read("compat/bionic/system_cacerts.h")
    wrap_begin = wrappers.index("void WrapSystemCacerts(SymbolTable& t) {")
    wrap_body = wrappers[wrap_begin:body_end(wrappers, wrappers.index("{", wrap_begin) + 1)]
    if re.search(r'\bt\s*\[|\bt\.(?:insert|emplace|try_emplace|insert_or_assign)\s*\(', wrap_body):
        raise ValueError("system CA wrapper now inserts keys; update the explicit registration boundary")
    pending = re.findall(r'\b(Register\w+)\(', registry)
    selected = set()
    while pending:
        name = pending.pop()
        if name not in selected:
            if name not in functions:
                raise ValueError("missing actual registration definition: " + name)
            selected.add(name)
            pending.extend(value for _, kind, value in functions[name] if kind == "call")
    bootstrap = ["void " + name + "(SymbolTable&);" for name in sorted(selected)]
    for name in sorted(selected):
        bootstrap += ["void " + name + "(SymbolTable& t) {", "  static unsigned char address;"]
        for _, kind, value in functions[name]:
            bootstrap.append("  " + (f't["{value}"] = &address;' if kind == "key" else value + "(t);"))
        bootstrap.append("}")

    print("BOUNDARY unchanged actual Registry/owned/internal lookup; actual registration keys with inert addresses", flush=True)
    print("BOUNDARY unmodified Android map generator for arm64/x86_64; no complete ABI/N10 or compatibility-function execution", flush=True)
    print("SOURCE_REF " + (reference or "working-tree snapshot"), flush=True)
    template_path = ROOT / "tests/native/bionic_provider_ownership_host.cc.in"
    template = template_path.read_text()
    source_hashes[str(template_path.relative_to(ROOT))] = hashlib.sha256(template.encode()).hexdigest()
    source_hashes["tools/test_bionic_provider_ownership.py"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    source = (template.replace("@TABLE_TYPE@", table_type)
              .replace("@REGISTRATION_BOOTSTRAP@", "\n".join(bootstrap))
              .replace("@REGISTRY_IMPLEMENTATION@", registry)
              .replace("@PUBLIC_LOOKUP_IMPLEMENTATION@", public))
    header = read("compat/arm64/include/gesher/symbol_table.h")
    for name, digest in sorted(source_hashes.items()):
        print("SOURCE_SHA256 " + name + " " + digest, flush=True)
    print("REGISTRY_SHA256 " + hashlib.sha256(registry.encode()).hexdigest(), flush=True)
    print("COMPILED_SOURCE_SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    bionic = args.bionic_source.resolve()

    def bionic_input(name: str) -> bytes:
        path = bionic / name
        # Read omitted sparse inputs from the same local Git object, never
        # modify the authoritative checkout or download a different version.
        data = path.read_bytes() if path.exists() else subprocess.check_output(
            ["git", "show", "HEAD:" + name], cwd=bionic)
        print("BIONIC_INPUT_SHA256 " + name + " " + hashlib.sha256(data).hexdigest(), flush=True)
        return data

    with tempfile.TemporaryDirectory(prefix="gesher-provider-owners-") as temporary:
        folder = Path(temporary)
        generator = folder / "generate-version-script.py"
        generator.write_bytes(bionic_input("tools/generate-version-script.py"))
        for owner, name in MAPS.items():
            original = folder / (owner + ".unfiltered.map.txt")
            original.write_bytes(bionic_input(name))
            for architecture in ("arm64", "x86_64"):
                output = folder / (owner + "-" + architecture + ".map.txt")
                subprocess.run([sys.executable, str(generator), architecture, str(original), str(output)],
                               check=True, timeout=30)
                print("GENERATED_MAP_SHA256 " + output.name + " " +
                      hashlib.sha256(output.read_bytes()).hexdigest(), flush=True)
        (folder / "symbol_table.h").write_text(header)
        unit = folder / "registry.cc"
        unit.write_text(source)
        executable = folder / ("ownership.exe" if os.name == "nt" else "ownership")
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fno-exceptions", "-pthread", str(unit), "-o", str(executable)]
        environment = os.environ.copy()
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
            previous = environment.get("ASAN_OPTIONS", "")
            environment["ASAN_OPTIONS"] = previous + (":" if previous else "") + "detect_leaks=0"
            print("SANITIZERS address,undefined; fatal findings; LSan disabled under ptrace", flush=True)
        print("COMPILE " + json.dumps(command), flush=True)
        subprocess.run(command, check=True, timeout=120)
        print("EXECUTABLE_SHA256 " + hashlib.sha256(executable.read_bytes()).hexdigest(), flush=True)
        return subprocess.run([str(executable), str(folder)], env=environment, timeout=30).returncode


if __name__ == "__main__":
    raise SystemExit(main())
