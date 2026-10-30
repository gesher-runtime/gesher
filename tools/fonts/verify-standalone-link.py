"""CI-only font input proof from actual GNU -Map hierarchy and exact object bytes."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

NATIVE = 'CMakeFiles/gesher_native_fonts.dir/compat/bionic/native_fonts.cc.obj'
REQUIRED = {NATIVE: 'gesher::bionic::(anonymous namespace)::OpenIterator(',
            'FontCollection.cpp.obj': 'minikin::FontCollection::create(',
            'MinikinSkia.cpp.obj': 'android::MinikinFontSkia::MinikinFontSkia('}
HEADER = 'Address Size Align Out In Symbol'


def map_rows(mapping):
    # Actual410563 GNU -Map output; lld/COFF/LLDMapFile.cpp. The archive
    # parent is absent. Child symbols are demangled and inherit input sections.
    lines = mapping.splitlines()
    assert lines and ' '.join(lines[0].split()) == HEADER, 'Unsupported map writer/schema'
    owner = None
    output = None
    rows = []
    for line in lines[1:]:
        if not line.strip():
            continue
        match = re.fullmatch(r'([0-9a-fA-F]{8,16}) ([0-9a-fA-F]{8,16})\s+([0-9]+) (.*)', line)
        assert match, ('Unknown map row', line)
        address, size, align = int(match[1], 16), int(match[2], 16), int(match[3])
        body = match[4]
        indent = len(body) - len(body.lstrip(' '))
        text = body[indent:]
        if indent == 0:
            owner = None  # New output section must not inherit an earlier owner.
            output = (address, address + size)
        elif indent == 8:
            origin, separator, section = text.rpartition(':(')
            assert separator and origin and section.endswith(')'), ('Missing input owner', line)
            assert output is not None and output[0] <= address <= address + size <= output[1], (
                'Input outside output section', line)
            owner = (origin, address, address + size)
        elif indent == 16:
            assert owner is not None and size == 0 and align == 0, ('Orphan/invalid symbol', line)
            assert owner[1] <= address <= owner[2], ('Symbol outside input section', line, owner)
            assert text, 'Empty symbol'
            rows.append((text, owner[0]))
        else:
            raise AssertionError(('Unknown map indentation', line))
    return rows


def check_map(mapping):
    rows = map_rows(mapping)
    for origin, prefix in REQUIRED.items():
        assert any(source == origin and symbol.startswith(prefix) for symbol, source in rows), (
            'Missing precise live font symbol/owner', origin, prefix)
    for symbol, origin in rows:
        assert not any(name in origin.lower() for name in
                       ('libart_runtime', 'framework_runtime.dir/', 'dalvikvm.dir/')), origin
        assert 'JNI_CreateJavaVM' not in symbol and 'JNI_GetCreatedJavaVMs' not in symbol, symbol
    return rows


def link_inputs(command, compiler, base):
    assert len(command.strip().splitlines()) == 1, 'Ambiguous target commands'
    tokens = shlex.split(command)
    index = tokens.index(str(compiler))
    end = tokens.index('&&', index) if '&&' in tokens[index:] else len(tokens)
    argv = tokens[index:end]
    assert not any(token.startswith('@') for token in argv), 'Unresolved response file'
    for token in argv:
        normalized = token.replace('\\', '/').lower()
        assert not any(name in normalized for name in
                       ('libart_runtime', 'framework_runtime.dir/', 'dalvikvm.dir/')), token
    archives, objects = {}, {}
    for token in argv[1:]:
        if token.startswith('-'):
            continue
        path = (base / token).resolve()
        if token.endswith(('.a', '.lib')):
            archives.setdefault(path.name.lower(), set()).add(path)
        elif token.endswith(('.obj', '.o')):
            objects.setdefault(path.name, set()).add(path)
    assert all(len(paths) == 1 for paths in archives.values()), 'Colliding archive basenames'
    for name in ('libminikin.a', 'libhwui.a'):
        assert name in archives, ('Missing actual archive input', name)
    return argv, {name: next(iter(paths)) for name, paths in archives.items()}, objects


def inventory_members(archives, tools):
    return {path: subprocess.check_output([str(tools / 'llvm-ar'), 't', str(path)], text=True).splitlines()
            for path in archives.values()}


def require_unique_member(inventories, objects, archive, member):
    matches = [(path, name) for path, names in inventories.items() for name in names
               if Path(name.replace('\\', '/')).name == member]
    assert len(matches) == 1 and matches[0][0] == archive and not objects.get(member), (
        'Missing/ambiguous actual member origin', member, matches, objects.get(member))
    return matches[0][1]


def defined_symbols(object_file, tools):
    # The observed map uses LLVM-demangled symbols; preserve their complete names.
    output = subprocess.check_output([str(tools / 'llvm-nm'), '-j', '--demangle',
                                      '--defined-only', str(object_file)], text=True)
    return set(output.splitlines())


def mapped_definitions(rows, origin, defined):
    return sorted(set(symbol for symbol, source in rows if source == origin
                      and symbol.startswith(REQUIRED[origin]) and symbol in defined))


def member_proof(archive, member, inventories, objects, rows, tools):
    name = require_unique_member(inventories, objects, archive, member)
    data = subprocess.check_output([str(tools / 'llvm-ar'), 'p', str(archive), name])
    assert data, ('Empty actual archive member', archive, member)
    with tempfile.TemporaryDirectory(prefix='fonts-link-') as temporary:
        object_file = Path(temporary) / member
        object_file.write_bytes(data)
        defined = defined_symbols(object_file, tools)
    matched = mapped_definitions(rows, member, defined)
    assert matched, ('Actual member does not define mapped class symbols', member)
    return {'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
            'member': name, 'member_sha256': hashlib.sha256(data).hexdigest(),
            'matching_symbols': len(matched), 'sample_symbol': matched[0]}


def main():
    assert os.environ.get('GITHUB_ACTIONS') == 'true', 'Native input inspection is CI only'
    exe, map_file, compiler = map(Path, sys.argv[1:])
    raw = map_file.read_bytes()
    mapping = raw.decode('utf-8', errors='strict')
    raw_lines = mapping.splitlines()
    print('FONT_LINK_MAP_DIAGNOSTIC ' + json.dumps({'sha256': hashlib.sha256(raw).hexdigest(),
          'header': raw_lines[:8], 'raw_font_inputs': [line for line in raw_lines if any(word in line
          for word in ('native_fonts.cc.obj', 'FontCollection.cpp.obj', 'MinikinSkia.cpp.obj'))][:12]}), flush=True)
    rows = check_map(mapping)
    command = subprocess.check_output(['ninja', '-t', 'commands', '-s', exe.name], text=True)
    argv, archives, objects = link_inputs(command, compiler, Path.cwd())
    tools = compiler.parent
    inventories = inventory_members(archives, tools)
    proofs = {name: member_proof(archives[name], member, inventories, objects, rows, tools)
              for name, member in (('libminikin.a', 'FontCollection.cpp.obj'),
                                   ('libhwui.a', 'MinikinSkia.cpp.obj'))}
    native = objects.get('native_fonts.cc.obj', set())
    assert len(native) == 1 and next(iter(native)) == (Path.cwd() / NATIVE).resolve(), 'Ambiguous native provider object'
    assert not any(Path(name.replace('\\', '/')).name == 'native_fonts.cc.obj'
                   for names in inventories.values() for name in names), 'Native provider basename collision'
    native = next(iter(native))
    defined = defined_symbols(native, tools)
    assert mapped_definitions(rows, NATIVE, defined), 'Native map/object symbol mismatch'
    imports = subprocess.check_output([str(tools / 'llvm-readobj'), '--coff-imports', str(exe)], text=True)
    assert not re.search(r'(?i)(jvm|dalvik|libart_runtime)', imports), imports
    versions = {name: subprocess.check_output([str(tool), '--version'], text=True).splitlines()[0]
                for name, tool in (('compiler', compiler), ('linker', tools / 'ld.lld'))}
    print('FONT_STANDALONE_LINK ' + json.dumps({
        'executable_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
        'map_sha256': hashlib.sha256(raw).hexdigest(),
        'proof_script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'compiled_source': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
        'workflow_source': os.environ['GITHUB_SHA'],
        'compiler_sha256': hashlib.sha256(compiler.read_bytes()).hexdigest(),
        'linker_sha256': hashlib.sha256((tools / 'ld.lld').read_bytes()).hexdigest(),
        'native_object_sha256': hashlib.sha256(native.read_bytes()).hexdigest(),
        'link_command_sha256': hashlib.sha256(command.encode()).hexdigest(),
        'link_argv': argv, 'archive_members': proofs, 'tool_versions': versions,
        'checked_archive_inputs': sorted(str(path) for path in inventories),
        'origins': len({origin for _, origin in rows}),
        'import_dlls': re.findall(r'Name: (.+)', imports), 'vm_inputs': False,
    }, sort_keys=True), flush=True)


if __name__ == '__main__':
    main()
