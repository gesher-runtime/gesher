"""CI-only proof from actual COFF map, generated link inputs and archive members."""
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

REQUIRED = {'native_fonts.cc.obj', 'libminikin:FontCollection.cpp.obj',
            'libhwui:MinikinSkia.cpp.obj'}


def map_rows(mapping):
    # LLD COFF MapFile.cpp: address, raw symbol, RVA+base, Lib:Object.
    # The archive extension is deliberately stripped, e.g. libminikin:member.obj.
    rows = []
    for line in mapping.splitlines():
        parts = line.split()
        if parts and re.fullmatch(r'[0-9a-fA-F]{4}:[0-9a-fA-F]{8,16}', parts[0]):
            # Section table rows have lengths ending in H rather than an RVA.
            if len(parts) >= 4 and re.fullmatch(r'[0-9a-fA-F]{8,16}', parts[2]):
                rows.append((parts[1], parts[-1]))
    return rows


def check_map(mapping):
    rows = map_rows(mapping)
    origins = {origin for _, origin in rows}
    assert REQUIRED <= origins, ('Missing precise font origins', sorted(REQUIRED - origins))
    for symbol, origin in rows:
        archive = origin.split(':', 1)[0].lower()
        assert not archive.startswith(('libart_runtime', 'libdalvik', 'framework_runtime')), origin
        assert 'JNI_CreateJavaVM' not in symbol and 'JNI_GetCreatedJavaVMs' not in symbol, symbol
    return rows


def link_inputs(command, compiler, base):
    # Read-only `ninja -t commands -s` emits the one target command, not a build.
    assert len(command.strip().splitlines()) == 1, 'Ambiguous target commands'
    tokens = shlex.split(command)
    index = tokens.index(str(compiler))
    end = tokens.index('&&', index) if '&&' in tokens[index:] else len(tokens)
    argv = tokens[index:end]
    assert not any(token.startswith('@') for token in argv), 'Unresolved response file'
    normalized = [token.replace('\\', '/').lower() for token in argv]
    for token in normalized:
        assert not any(name in token for name in
                       ('libart_runtime', 'framework_runtime.dir/', 'dalvikvm.dir/')), token
    archives = {}
    for token in argv[1:]:
        if token.endswith(('.a', '.lib')) and not token.startswith('-'):
            path = (base / token).resolve()
            archives.setdefault(path.name, set()).add(path)
    assert all(len(paths) == 1 for paths in archives.values()), 'Colliding archive basenames'
    for name in ('libminikin.a', 'libhwui.a'):
        assert name in archives, ('Missing actual archive input', name)
    return argv, {name: next(iter(paths)) for name, paths in archives.items()}


def member_proof(archive, member, rows, tools):
    inventory = subprocess.check_output([str(tools / 'llvm-ar'), 't', str(archive)], text=True).splitlines()
    candidates = [name for name in inventory if Path(name).name == member]
    assert len(candidates) == 1, ('Missing or ambiguous archive member', archive, member)
    data = subprocess.check_output([str(tools / 'llvm-ar'), 'p', str(archive), candidates[0]])
    assert data, ('Empty selected member', archive, member)
    with tempfile.TemporaryDirectory(prefix='fonts-link-') as temporary:
        object_file = Path(temporary) / member
        object_file.write_bytes(data)
        output = subprocess.check_output([str(tools / 'llvm-nm'), '--format=posix',
                                          '--defined-only', str(object_file)], text=True)
    defined = {line.split()[0] for line in output.splitlines() if line.split()}
    origin = archive.stem + ':' + member
    matched = sorted(symbol for symbol, source in rows if source == origin and symbol in defined)
    assert matched, ('Map symbols do not belong to exact member', origin)
    return {'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
            'member': candidates[0], 'member_sha256': hashlib.sha256(data).hexdigest(),
            'map_origin': origin, 'matching_symbols': len(matched), 'sample_symbol': matched[0]}


def main():
    exe, map_file, compiler = map(Path, sys.argv[1:])
    raw = map_file.read_bytes()
    mapping = raw.decode('utf-8', errors='strict')
    observed = map_rows(mapping)
    print('FONT_LINK_MAP_DIAGNOSTIC ' + json.dumps({
        'sha256': hashlib.sha256(raw).hexdigest(), 'header': mapping.splitlines()[:8],
        'rows': len(observed), 'font_rows': [(symbol, origin) for symbol, origin in observed
                    if any(word in origin for word in ('minikin', 'MinikinSkia', 'native_fonts'))][:12]
    }), flush=True)
    rows = check_map(mapping)
    command = subprocess.check_output(['ninja', '-t', 'commands', '-s', exe.name], text=True)
    argv, archives = link_inputs(command, compiler, Path.cwd())
    tools = compiler.parent
    proofs = {name: member_proof(archives[name], member, rows, tools)
              for name, member in (('libminikin.a', 'FontCollection.cpp.obj'),
                                   ('libhwui.a', 'MinikinSkia.cpp.obj'))}
    imports = subprocess.check_output([str(tools / 'llvm-readobj'), '--coff-imports', str(exe)], text=True)
    assert not re.search(r'(?i)(jvm|dalvik|libart_runtime)', imports), imports
    versions = {name: subprocess.check_output([str(tool), '--version'], text=True).splitlines()[0]
                for name, tool in (('compiler', compiler), ('linker', tools / 'ld.lld'))}
    print('FONT_STANDALONE_LINK ' + json.dumps({
        'executable_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
        'map_sha256': hashlib.sha256(raw).hexdigest(),
        'link_command_sha256': hashlib.sha256(command.encode()).hexdigest(),
        'link_argv': argv, 'archive_members': proofs, 'tool_versions': versions,
        'origins': len({origin for _, origin in rows}),
        'import_dlls': re.findall(r'Name: (.+)', imports), 'vm_inputs': False,
    }, sort_keys=True), flush=True)


if __name__ == '__main__':
    main()