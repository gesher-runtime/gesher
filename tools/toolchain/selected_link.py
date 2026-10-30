"""Selected-only first-caller link ledger. No compiler or supplier implementation.

The launcher executes the exact received argv; response expansion is audit-only.
Missing/failed capture is never evidence that a target does not use pthread.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import subprocess
import sys

import prepare_host_pthread as factory

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = 'gesher-selected-pthread-link-v1'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def mode(env):
    value = env.get('GESHER_HOST_PTHREAD_SELECTION', 'publisher')
    assert value in ('publisher', 'selected-minimum'), 'Unknown product supplier selection'
    return value


def authority(env=os.environ):
    assert mode(env) == 'selected-minimum'
    context = factory.caller(env, 'selected-minimum')
    receipt = Path(env['GESHER_SELECTED_PTHREAD_RECEIPT'])
    raw = receipt.read_bytes()
    assert digest(raw) == env['GESHER_SELECTED_PTHREAD_RECEIPT_SHA256']
    data = json.loads(raw)
    factory.check_metadata(data, context)
    factory.check_files(data)
    return data, raw


def identity(data, raw):
    """Stable within this producer, including OFF -> ON; no final-output fields."""
    a = data['authority']
    return dict(schema='gesher-selected-pthread-compiler-v1', context=data['context'],
                role=data['role'], recipe_fingerprint=data['recipe_fingerprint'],
                receipt_sha256=digest(raw), authority_sha256=data['authority_sha256'],
                source_commit=data['source_commit'], source_files=a['source_files'],
                aliases=a['files'], public_headers=a['public_headers'],
                original_profile=a['original_profile'],
                ledger_sources={p: factory.sha(ROOT / p) for p in (
                    'tools/toolchain/selected_link.py',
                    'tools/toolchain/selected_link.cmake')})


def compiler_identity(driver, env=os.environ):
    if mode(env) == 'publisher':
        return None
    data, raw = authority(env)
    assert Path(driver).resolve() == Path(data['compiler_root'], 'bin/x86_64-w64-mingw32-clang++').resolve()
    return identity(data, raw)


def gnu_tokens(text):
    """LLVM GNU response grammar, not shell execution or POSIX shlex.

    LLVM23.1.3 CommandLine.cpp TokenizeGNUCommandLine escapes the next
    character even inside single quotes. Reject unfinished quotes/escapes.
    """
    result, token, started = [], [], False
    index = 0
    while index < len(text):
        char = text[index]
        if char in ' \t\r\n':
            if started:
                result.append(''.join(token))
                token, started = [], False
        elif char == '\\':
            index += 1
            assert index < len(text), 'Unfinished response escape'
            token.append(text[index])
            started = True
        elif char in "\"'":
            quote = char
            started = True
            index += 1
            while index < len(text) and text[index] != quote:
                if text[index] == '\\':
                    index += 1
                    assert index < len(text), 'Unfinished quoted response escape'
                token.append(text[index])
                index += 1
            assert index < len(text), 'Unfinished response quote'
        else:
            token.append(char)
            started = True
        index += 1
    if started:
        result.append(''.join(token))
    return result


def expanded(argv, cwd, read=lambda p: p.read_bytes(), portable=False):
    """Linux-host Clang/GNU response grammar; retain raw files and exact argv.

    Reject unsupported nesting/cycles/encoding instead of guessing. Execution
    receives the unexpanded argv, so quoting and compiler options are preserved.
    """
    records = {}

    def expand(args, stack=()):
        result = []
        for arg in args:
            if not arg.startswith('@'):
                result.append(arg)
                continue
            path = PurePosixPath(posixpath.normpath(str(cwd / arg[1:]))) if portable else (cwd / arg[1:]).resolve()
            assert arg != '@' and path not in stack and len(stack) < 16
            raw = read(path)
            assert b'\0' not in raw and not raw.startswith((b'\xff\xfe', b'\xfe\xff', b'\xef\xbb\xbf'))
            text = raw.decode('utf-8', errors='strict')
            record = dict(path=str(path), sha256=digest(raw), bytes=len(raw), text=text)
            assert str(path) not in records or records[str(path)] == record
            records[str(path)] = record
            # Nested @files have tool-specific relative resolution. This first
            # caller supports the ordinary generated flat Ninja response only.
            tokens = gnu_tokens(text)
            assert not any(t.startswith('@') for t in tokens), 'Nested response is not qualified'
            result.extend(expand(tokens, stack + (path,)))
        return result
    return expand(argv), list(records.values())


def link_paths(argv, cwd, portable=False):
    path_class = PurePosixPath if portable else Path
    def resolve(p):
        return PurePosixPath(posixpath.normpath(str(cwd / p))) if portable else (cwd / p).resolve()
    assert argv and path_class(argv[0]).is_absolute()
    assert argv.count('-o') == 1 and argv.index('-o') + 1 < len(argv)
    output = resolve(argv[argv.index('-o') + 1])
    maps = []
    verbose = 0
    for arg in argv:
        if arg.startswith('-Wl,'):
            tokens = arg[4:].split(',')
            verbose += tokens.count('--verbose')
            for i, token in enumerate(tokens):
                if token == '-Map':
                    assert i + 1 < len(tokens)
                    maps.append(resolve(tokens[i + 1]))
                elif token.startswith(('-Map=', '--Map=')):
                    maps.append(resolve(token.split('=', 1)[1]))
    assert len(maps) == 1 and verbose == 1, 'One explicit actual map and verbose option required'
    assert output != maps[0]
    return output, maps[0]


def provider_names(raw):
    names = set()
    for line in raw.splitlines():
        match = re.fullmatch(r'[0-9a-fA-F]+\s+[TDBRWV]\s+(\S+)', line)
        if match:
            name = match[1]
            if name.startswith(('pthread_', '_pthread_', '__pthread_', '__pth_')) or name in ('__dyn_tls_pthread', '__xl_f'):
                names.add(name)
    assert {'pthread_self', 'pthread_join', 'pthread_create', 'pthread_exit'} <= names
    return names


def prove(trace, mapping, imports, data, cwd):
    """Pure actual LLD proof; exact symbol/address/owner duplicates coalesce."""
    assert trace.strip() and mapping.strip() and imports.strip()
    assert re.search(r'^\s*Address\s+Size\s+Align\s+Out\s+In\s+Symbol\s*$', mapping, re.MULTILINE), 'Missing actual LLD map header'
    assert re.search(r'\bReading [^\r\n]+\r?$', trace, re.MULTILINE), 'Missing actual linker Reading observations'
    assert 'Format: COFF-x86-64' in imports, 'Missing actual target PE import capture'
    names = provider_names(data['authority']['library_symbols']['lib/libwinpthread.a'])
    # Pure POSIX producer paths: consumers may validate on Windows without
    # resolving Linux paths through the local drive or touching a compiler.
    cwd = PurePosixPath(str(cwd))
    assert cwd.is_absolute()
    def normalize_origin(p):
        return posixpath.normpath(str(cwd / p))
    target = PurePosixPath(data['compiler_root']) / 'x86_64-w64-mingw32'
    assert target.is_absolute()
    aliases = {str(target / n): h for n, h in data['authority']['files'].items() if n.endswith('.a')}
    reading = set()
    for p in re.findall(r'\bReading ([^\r\n]+)\r?$', trace, re.MULTILINE):
        # Archive member Reading records are not archive origins.
        if p.endswith('.a'):
            reading.add(normalize_origin(p))
    loaded = set()
    for archive, member in re.findall(r'\bLoaded (.+?\.a)\(([^()]+)\) for [^\r\n]+', trace):
        if 'pthread' in PurePosixPath(archive).name.lower() or member.startswith('libwinpthread_'):
            origins = {p for p in reading if PurePosixPath(p).name == PurePosixPath(archive).name}
            assert len(origins) == 1, 'Missing/ambiguous actual pthread archive Reading origin'
            origin = origins.pop()
            assert origin in aliases and not origin.endswith('.dll.a'), 'Unexpected provider archive'
            if '/' in archive:
                assert normalize_origin(archive) == origin, 'Loaded fullpath contradicts Reading origin'
            loaded.add((PurePosixPath(origin).name, member, origin))
    definitions = {}
    owner = None
    input_sections = 0
    for line in mapping.splitlines():
        row = re.fullmatch(r'([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+[0-9]+ (.*)', line)
        if not row:
            continue
        tail = row[2]
        if ':(' in tail:
            owner = tail.strip().rsplit(':(', 1)[0]
            assert owner
            input_sections += 1
        elif tail.startswith('                '):
            symbol = tail.strip()
            if symbol in names:
                value = (owner, row[1].lower())
                assert symbol not in definitions or definitions[symbol] == value, 'Conflicting provider definition'
                definitions[symbol] = value
        else:
            owner = None
    assert input_sections > 0, 'No actual mapped input sections'
    bindings = {}
    for symbol, (owner, address) in definitions.items():
        assert owner is not None
        match = re.fullmatch(r'(.+\.a)\(([^()]+)\)', owner)
        assert match, 'Provider symbol came from an unreceipted object: ' + symbol
        candidates = [(p, m) for alias, m, p in loaded if alias == PurePosixPath(match[1]).name and m == match[2]]
        assert len(candidates) == 1, 'Map provider has no unique actual loaded member'
        origin, member = candidates[0]
        if '/' in match[1]:
            assert normalize_origin(match[1]) == origin, 'Map fullpath contradicts loaded origin'
        bindings[symbol] = dict(archive=origin, archive_sha256=aliases[origin], member=member, address=address)
    assert not re.search(r'Name:\s*lib(?:win)?pthread[^\s]*\.dll\b', imports, re.IGNORECASE), 'Unexpected dynamic pthread dependency'
    assert not re.search(r'Symbol:\s*(?:__imp_)?(?:pthread_|_pthread_|__pthread_|__pth_)', imports), 'Imported pthread provider symbol'
    assert bool(loaded) == bool(bindings), 'Loaded provider without live definition, or missing loaded proof'
    return dict(status='LINKED' if bindings else 'NOT-LINKED', definitions=bindings,
                loaded_members=[dict(archive=p, sha256=aliases[p], member=m) for _, m, p in sorted(loaded)])


def capture(argv):
    data, raw = authority()
    cwd = Path.cwd().resolve()
    args, responses = expanded(argv, cwd)
    assert Path(args[0]).resolve() in {Path(data['compiler_root'], 'bin/x86_64-w64-mingw32-' + n).resolve() for n in ('clang', 'clang++')}
    output, map_path = link_paths(args, cwd)
    # Exclusive per-output log. A later deliberate relink replaces this capture;
    # consumers seal production evidence before the hook build.
    log = Path(str(output) + '.selected-pthread-link.log')
    record_path = Path(str(output) + '.selected-pthread-link.json')
    record_path.unlink(missing_ok=True)
    map_path.unlink(missing_ok=True)
    with log.open('wb') as stream:
        child = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for chunk in iter(lambda: child.stdout.read(65536), b''):
            stream.write(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
        code = child.wait()
    if code:
        return code
    for item in responses:
        assert Path(item['path']).read_bytes() == item['text'].encode('utf-8'), 'Response changed during actual link'
    readobj = Path(data['compiler_root'], 'bin/llvm-readobj')
    imported = subprocess.check_output([str(readobj), '--coff-imports', str(output)])
    import_path = Path(str(output) + '.selected-pthread-imports.txt')
    import_path.write_bytes(imported)
    trace = log.read_bytes()
    mapping = map_path.read_bytes()
    proof = prove(trace.decode('utf-8', errors='strict'), mapping.decode('utf-8', errors='strict'),
                  imported.decode('utf-8', errors='strict'), data, cwd)
    after, after_raw = authority()
    assert identity(data, raw) == identity(after, after_raw), 'Authority changed during link'
    record = dict(schema=SCHEMA, selected_toolchain=identity(data, raw), cwd=str(cwd),
                  argv=argv, expanded_argv=args, response_files=responses, exit_code=code,
                  output=dict(path=str(output), sha256=factory.sha(output)),
                  evidence={kind: dict(path=str(path), sha256=factory.sha(path)) for kind, path in (
                      ('trace', log), ('map', map_path), ('imports', import_path))}, proof=proof)
    record_path.write_text(json.dumps(record, sort_keys=True, indent=2) + '\n', encoding='utf-8')
    return 0


def seal(original, published, compiler):
    """Copy reconstructible evidence beside an already copied/moved final PE."""
    data, raw = authority()
    assert compiler['selected_toolchain'] == identity(data, raw)
    record_path = Path(str(original.resolve()) + '.selected-pthread-link.json')
    record = json.loads(record_path.read_text())
    assert record['schema'] == SCHEMA and record['selected_toolchain'] == compiler['selected_toolchain']
    assert record['exit_code'] == 0 and record['output']['path'] == str(original.resolve())
    assert record['output']['sha256'] == factory.sha(published)
    # Ninja may already have removed response files after the launcher exited.
    # Reconstruct from the raw text captured and checked during the actual link.
    saved_responses = {r['path']: r['text'].encode('utf-8') for r in record['response_files']}
    replay, responses = expanded(record['argv'], Path(record['cwd']), lambda p: saved_responses[str(p)])
    assert replay == record['expanded_argv'] and responses == record['response_files']
    out, mapping = link_paths(replay, Path(record['cwd']))
    assert str(out) == record['output']['path'] and str(mapping) == record['evidence']['map']['path']
    evidence = {}
    for kind, item in record['evidence'].items():
        content = Path(item['path']).read_bytes()
        assert digest(content) == item['sha256']
        path = published.with_name(published.name + '.selected-pthread-' + kind + '.txt')
        path.write_bytes(content)
        evidence[kind] = dict(file=path.name, sha256=digest(content))
    assert prove(Path(record['evidence']['trace']['path']).read_text(), mapping.read_text(),
                 Path(record['evidence']['imports']['path']).read_text(), data, Path(record['cwd'])) == record['proof']
    sealed = dict(record, evidence=evidence)
    receipt = published.with_name('selected-pthread-toolchain.json')
    if receipt.exists():
        assert receipt.read_bytes() == raw
    else:
        receipt.write_bytes(raw)
    path = published.with_name(published.name + '.selected-pthread-link.json')
    path.write_text(json.dumps(sealed, sort_keys=True, indent=2) + '\n', encoding='utf-8')
    return dict(file=path.name, sha256=factory.sha(path), output_sha256=record['output']['sha256'],
                status=record['proof']['status'], receipt_sha256=digest(raw))


def verify_sealed(directory, link, compiler, executable):
    """Portable receiver proof: actual archived evidence, no native processes."""
    def member(name):
        assert isinstance(name, str) and Path(name).name == name and name not in ('', '.', '..')
        path = directory / name
        assert path.is_file() and not path.is_symlink()
        return path
    raw = member('selected-pthread-toolchain.json').read_bytes()
    data = json.loads(raw)
    factory.check_metadata(data, data['context'])
    assert identity(data, raw) == compiler['selected_toolchain']
    assert link['receipt_sha256'] == digest(raw)
    record_path = member(link['file'])
    assert factory.sha(record_path) == link['sha256']
    record = json.loads(record_path.read_text())
    assert record['schema'] == SCHEMA and record['exit_code'] == 0
    assert record['selected_toolchain'] == compiler['selected_toolchain']
    assert record['output']['sha256'] == link['output_sha256'] == factory.sha(member(executable))
    assert link['status'] == record['proof']['status']
    cwd = PurePosixPath(record['cwd'])
    assert cwd.is_absolute() and posixpath.normpath(str(cwd)) == str(cwd)
    saved_responses = {r['path']: r['text'].encode('utf-8') for r in record['response_files']}
    argv, responses = expanded(record['argv'], cwd, lambda p: saved_responses[str(p)], portable=True)
    assert argv == record['expanded_argv'] and responses == record['response_files']
    assert PurePosixPath(argv[0]) in {PurePosixPath(data['compiler_root'], 'bin/x86_64-w64-mingw32-' + n) for n in ('clang', 'clang++')}
    output, _ = link_paths(argv, cwd, portable=True)
    assert str(output) == record['output']['path']
    assert set(record['evidence']) == {'trace', 'map', 'imports'}
    evidence = {}
    for kind, item in record['evidence'].items():
        path = member(item['file'])
        assert factory.sha(path) == item['sha256']
        evidence[kind] = path.read_text(encoding='utf-8')
    assert prove(evidence['trace'], evidence['map'], evidence['imports'], data,
                 PurePosixPath(record['cwd'])) == record['proof']
    return record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=('launch',))
    parser.add_argument('argv', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    sys.exit(capture(args.argv))


if __name__ == '__main__':
    main()
