"""Pure selected-link metadata controls. Never starts a compiler/process."""
import argparse
import copy
import json
from pathlib import Path, PurePosixPath
import re
import tempfile
import selected_link as ledger


def reject(fn):
    try:
        fn()
    except (AssertionError, KeyError, UnicodeError, ValueError):
        return
    raise AssertionError('Malformed proof accepted')


def projection(old):
    """Explicit SYNTHETIC current-wrapper projection, no selected build claim."""
    f = ledger.factory
    context = dict(GITHUB_SHA='1'*40, GITHUB_RUN_ID='123456', GITHUB_RUN_ATTEMPT='1',
                   GITHUB_REPOSITORY='gesher-runtime/gesher', GITHUB_REPOSITORY_ID='1426153186')
    recipe = f.recipe()
    fingerprint = f.fingerprint(recipe)
    root = '/SYNTHETIC/gesher-host-pthread-selected-minimum-' + fingerprint[:16]
    sdk = root + '/' + f.NAME
    source = root + '/source'
    old_sdk = old['compiler'].split('InstalledDir: ', 1)[1].split('/bin', 1)[0]
    old_source = next(iter(old['dll_symbol_objects'].values()))['actual_reading_path'].split('/build/')[0]
    def rewrite(value):
        if isinstance(value, dict):
            return {k: rewrite(v) for k, v in value.items()}
        if isinstance(value, list):
            return [rewrite(v) for v in value]
        if isinstance(value, str):
            return value.replace(old_sdk, sdk).replace(old_source, source)
        return value
    a = rewrite(old)
    a.update({k: context[k] for k in f.FIELDS[:4]})
    a['role'] = f.minimum.ROLE
    a['pin'] = copy.deepcopy(f.minimum.PIN)
    a['original']['context'] = {k: context[k] for k in f.FIELDS[:4]}
    data = dict(schema='gesher-host-toolchain-v1', context=context, selection='selected-minimum',
                role=f.minimum.ROLE, recipe=recipe, recipe_fingerprint=fingerprint,
                workspace=root, compiler_root=sdk, source_root=source,
                source_commit=f.minimum.PIN['upstream_commit'],
                source_before=copy.deepcopy(f.minimum.PIN['source_sha256']),
                source_after=copy.deepcopy(f.minimum.PIN['output_source_sha256']),
                archive=dict(url=f.URL, sha256=f.minimum.PIN['compiler_archive_sha256'], bytes=84003044),
                authority=a, authority_sha256=ledger.digest(json.dumps(a, sort_keys=True, indent=2).encode()))
    f.check_metadata(data, context)
    return data


def sealed_controls(old):
    data = projection(old)
    raw = json.dumps(data, sort_keys=True, indent=2).encode()
    compiler = dict(selected_toolchain=ledger.identity(data, raw))
    assert not ({'output', 'proof', 'argv', 'evidence'} & set(compiler['selected_toolchain']))
    driver = str(PurePosixPath(data['compiler_root'], 'bin/x86_64-w64-mingw32-clang++'))
    # Text-only synthetic artifact, not a generated or executed native binary.
    with tempfile.TemporaryDirectory(prefix='gesher-selected-ledger-metadata-') as temporary:
        directory = Path(temporary)
        (directory / 'payload.txt').write_bytes(b'SYNTHETIC_METADATA_ONLY_NOT_PE')
        (directory / 'selected-pthread-toolchain.json').write_bytes(raw)
        record = dict(schema=ledger.SCHEMA, selected_toolchain=compiler['selected_toolchain'], cwd='/SYNTHETIC/out',
                      argv=[driver, '-o', 'payload.txt', '-Wl,--verbose', '-Wl,-Map,payload.map'],
                      response_files=[], exit_code=0,
                      output=dict(path='/SYNTHETIC/out/payload.txt', sha256=ledger.factory.sha(directory / 'payload.txt')))
        record['expanded_argv'] = list(record['argv'])
        texts = dict(trace='ld.lld: Reading /SYNTHETIC/out/main.obj\n',
                     map='Address  Size     Align Out     In      Symbol\n00001000 00000020 4096 .text\n00001000 00000020 16         main.obj:(.text)\n00001000 00000000 0                 main\n',
                     imports='File: SYNTHETIC\nFormat: COFF-x86-64\nName: KERNEL32.dll\n')
        record['proof'] = ledger.prove(texts['trace'], texts['map'], texts['imports'], data, PurePosixPath(record['cwd']))
        record['evidence'] = {}
        for kind, text in texts.items():
            path = directory / (kind + '.txt')
            path.write_bytes(text.encode())
            record['evidence'][kind] = dict(file=path.name, sha256=ledger.factory.sha(path))
        path = directory / 'record.json'
        def write(value):
            path.write_text(json.dumps(value), encoding='utf-8')
            return dict(file=path.name, sha256=ledger.factory.sha(path), output_sha256=record['output']['sha256'],
                        status='NOT-LINKED', receipt_sha256=ledger.digest(raw))
        link = write(record)
        assert ledger.verify_sealed(directory, link, compiler, 'payload.txt', data['context']) == record
        mutations = [lambda r: r.update(exit_code=1), lambda r: r.update(schema='old'),
                     lambda r: r['argv'].__setitem__(0, '/SYNTHETIC/other/clang++'),
                     lambda r: r['expanded_argv'].append('hidden.obj'),
                     lambda r: r['output'].update(path='/SYNTHETIC/out/other.txt'),
                     lambda r: r['evidence']['trace'].update(file='../trace.txt'),
                     lambda r: r['evidence']['map'].update(sha256='0'*64),
                     lambda r: r['proof'].update(status='LINKED'),
                     lambda r: r['selected_toolchain'].update(receipt_sha256='0'*64)]
        for mutate in mutations:
            bad = copy.deepcopy(record)
            mutate(bad)
            link = write(bad)
            reject(lambda: ledger.verify_sealed(directory, link, compiler, 'payload.txt', data['context']))
        link = write(record)
        for key in ('sha256', 'receipt_sha256', 'output_sha256'):
            bad = dict(link)
            bad[key] = '0'*64
            reject(lambda: ledger.verify_sealed(directory, bad, compiler, 'payload.txt', data['context']))
        for field in data['context']:
            expected = dict(data['context'])
            expected[field] = 'wrong-outer-context'
            reject(lambda: ledger.verify_sealed(directory, link, compiler, 'payload.txt', expected))
        # A coherent older receipt + compiler + per-output sidecar still fails
        # against the independently supplied current outer producer context.
        for field, value in (('GITHUB_SHA', '2'*40), ('GITHUB_RUN_ID', '123455'), ('GITHUB_RUN_ATTEMPT', '2'),
                             ('GITHUB_REPOSITORY', 'elsewhere/gesher'), ('GITHUB_REPOSITORY_ID', '1')):
            older = copy.deepcopy(data)
            older['context'][field] = value
            if field in ledger.factory.FIELDS[:4]:
                older['authority'][field] = value
                older['authority']['original']['context'][field] = value
            older['authority_sha256'] = ledger.digest(json.dumps(older['authority'], sort_keys=True, indent=2).encode())
            ledger.factory.check_metadata(older, older['context'])
            old_raw = json.dumps(older, sort_keys=True, indent=2).encode()
            old_compiler = dict(selected_toolchain=ledger.identity(older, old_raw))
            old_record = copy.deepcopy(record)
            old_record['selected_toolchain'] = old_compiler['selected_toolchain']
            old_link = write(old_record)
            old_link['receipt_sha256'] = ledger.digest(old_raw)
            (directory / 'selected-pthread-toolchain.json').write_bytes(old_raw)
            reject(lambda: ledger.verify_sealed(directory, old_link, old_compiler, 'payload.txt', data['context']))
        (directory / 'selected-pthread-toolchain.json').write_bytes(raw)
        link = write(record)
        (directory / 'trace.txt').write_text('CHANGED', encoding='utf-8')
        reject(lambda: ledger.verify_sealed(directory, link, compiler, 'payload.txt', data['context']))
    print('PASS portable sealed metadata: 23 receipt/output/record/command/evidence/outer-context negatives; textual fixtures only, no native/process/network')


def controls(authority):
    # Historical actual metadata supplies only the symbol inventory; this is a
    # visibly SYNTHETIC map/command projection, never a new actual link receipt.
    data = dict(compiler_root='/SYNTHETIC/sdk', authority=authority)
    cwd = Path('/SYNTHETIC/out').resolve()
    proof_cwd = PurePosixPath('/SYNTHETIC/out')
    target = PurePosixPath(data['compiler_root'], 'x86_64-w64-mingw32/lib/libpthread.a')
    member = 'libwinpthread_la-thread.o'
    assert ledger.symbol_inventory(authority['library_symbols']['lib/libwinpthread.a'])[0]['pthread_self'] == {member}
    trace = f'ld.lld: Reading {target}\nld.lld: Loaded libpthread.a(thread.o) for pthread_self\n'.replace('thread.o', member)
    header = 'Address  Size     Align Out     In      Symbol\n'
    mapping = header + '00001000 00000020 4096 .text\n00001000 00000020 16         thread.o:(.text)\n00001000 00000000 0                 pthread_self\n'.replace('thread.o', member)
    imported = 'File: SYNTHETIC.exe\nFormat: COFF-x86-64\nImport {\n  Name: KERNEL32.dll\n}\n'
    linked = ledger.prove(trace, mapping, imported, data, proof_cwd)
    assert linked['status'] == 'LINKED' and set(linked['definitions']) == {'pthread_self'}
    assert ledger.prove(trace, mapping + mapping.splitlines()[-1]+'\n', imported, data, proof_cwd) == linked
    clock_trace = trace.replace(member, 'libwinpthread_la-clock.o').replace('pthread_self', 'clock_gettime64')
    clock_map = mapping.replace(member, 'libwinpthread_la-clock.o').replace('pthread_self', 'clock_gettime64')
    assert ledger.prove(clock_trace, clock_map, imported, data, proof_cwd)['status'] == 'LINKED'
    unused = ledger.prove('ld.lld: Reading /SYNTHETIC/out/ordinary.obj\n',
                          header + '00001000 00000020 4096 .text\n00001000 00000020 16         ordinary.obj:(.text)\n00001000 00000000 0                 main\n', imported, data, proof_cwd)
    assert unused == dict(status='NOT-LINKED', definitions={}, loaded_members=[])
    negatives = [
        ('', mapping, imported),
        (trace, '', imported),
        (trace, mapping.replace(header, ''), imported),
        (trace, header, imported),
        ('SYNTHETIC INCOMPLETE', header + '00001000 00000020 16 main.obj:(.text)\n', imported),
        (trace, mapping, ''),
        (trace, mapping, imported.replace('Format: COFF-x86-64', 'Format: unsupported')),
        (trace.replace(f'ld.lld: Reading {target}\n', ''), mapping, imported),
        (trace + 'ld.lld: Reading /SYNTHETIC/other/libpthread.a\n', mapping, imported),
        (trace.replace(str(target), '/SYNTHETIC/old/libpthread.a'), mapping, imported),
        (trace.replace(member, 'other.o'), mapping, imported),
        (trace, mapping.replace(member, 'ordinary.obj'), imported),
        (trace, mapping.replace(member, 'other.a('+member+')'), imported),
        (trace, mapping.replace(member, 'other.o'), imported),
        (trace.replace('Loaded libpthread.a(', 'Loaded /SYNTHETIC/other/libpthread.a('), mapping, imported),
        (trace, mapping.replace(member, '/SYNTHETIC/other/'+member), imported),
        (trace, mapping + mapping.replace('00001000', '00002000'), imported),
        (trace, mapping + mapping.replace(member, 'other.a('+member+')'), imported),
        (trace, mapping, imported.replace('KERNEL32.dll', 'libwinpthread-1.dll')),
        (trace, mapping, imported + 'Symbol: pthread_self (0)\n'),
        (trace, mapping, imported + 'Symbol: __imp_pthread_join (0)\n'),
        (trace, mapping, imported + 'Symbol: _pthread (0)\n'),
        (trace, mapping, imported + 'Symbol: __imp__pthread (0)\n'),
        (trace, '00001000 00000020 16 ordinary.obj:(.text)\n', imported),
        ('ld.lld: Reading /SYNTHETIC/out/ordinary.obj\n', mapping, imported),
        (trace.replace(member, 'libwinpthread_la-clock.o'), mapping.replace(member, 'libwinpthread_la-clock.o'), imported),
        (trace.replace(member, 'nonexistent.o'), mapping.replace(member, 'nonexistent.o'), imported),
        (trace+'ld.lld: Reading /SYNTHETIC/other/ordinary.a\nld.lld: Loaded ordinary.a('+member+') for unrelated\n', mapping, imported),
        (trace+'ld.lld: Reading /SYNTHETIC/direct/'+member+'\n', mapping, imported),
        (trace, mapping+'unknown writer row\n', imported),
        (trace, mapping.replace('16         '+member, '16    '+member), imported),
        (trace, mapping.replace('00001000 00000020 4096 .text\n', ''), imported),
        (trace, mapping.replace('00001000 00000020 16', '00003000 00000020 16'), imported),
        (trace, mapping.replace('00001000 00000000 0', '00003000 00000000 0'), imported),
        (trace, mapping.replace('00001000 00000000 0', '00001000 00000001 0'), imported),
        (trace, mapping.replace('00001000 00000000 0', '00001000 00000000 1'), imported),
        (trace, mapping.replace(':(.text)', 'no-input-section'), imported),
        (trace, mapping.replace(':(.text)', ':(.text'), imported),
        (trace, mapping.replace('                pthread_self', '                '), imported),
        (trace, mapping.replace('00001000 00000000 0', '00002000 00000020 4096 .other\n00001000 00000000 0'), imported),
    ]
    for t, m, i in negatives:
        reject(lambda: ledger.prove(t, m, i, data, proof_cwd))
    assert ledger.compiler_identity('not-even-an-existing-driver', {}) is None
    assert ledger.compiler_identity('not-even-an-existing-driver', {'GESHER_HOST_PTHREAD_SELECTION': 'publisher'}) is None
    reject(lambda: ledger.compiler_identity('unused', {'GESHER_HOST_PTHREAD_SELECTION': 'unknown'}))
    reject(lambda: ledger.compiler_identity('unused', {'GESHER_HOST_PTHREAD_SELECTION': 'selected-minimum'}))
    argv = [str(Path('/SYNTHETIC/sdk/bin/clang++').resolve()), '-target', 'x86_64-w64-mingw32', '-o', 'actual.exe',
            '-Wl,-Map,actual.map', '-Wl,--verbose', 'input.obj']
    assert ledger.link_paths(argv, cwd) == (cwd / 'actual.exe', cwd / 'actual.map')
    assert ledger.link_paths([a.replace('-Map,', '-Map=') for a in argv], cwd) == (cwd / 'actual.exe', cwd / 'actual.map')
    for bad in [argv + ['-o', 'second.exe'], argv + ['-Wl,-Map,second.map'],
                [a for a in argv if a != '-Wl,--verbose'], argv + ['-Wl,--verbose'],
                [a for a in argv if not a.startswith('-Wl,-Map')], ['relative-clang++'] + argv[1:]]:
        reject(lambda: ledger.link_paths(bad, cwd))
    rsp = cwd / 'quoted.rsp'
    raw = b'''-target x86_64-w64-mingw32 -o "actual file.exe" -Wl,-Map,"actual map.txt" "input space.obj" '' 'single\\ quote' '''
    command = [argv[0], '@quoted.rsp', '-Wl,--verbose']
    expanded, records = ledger.expanded(command, cwd, lambda p: raw if p == rsp else b'')
    assert expanded == [argv[0], '-target', 'x86_64-w64-mingw32', '-o', 'actual file.exe',
                        '-Wl,-Map,actual map.txt', 'input space.obj', '', 'single quote', '-Wl,--verbose']
    assert command == [argv[0], '@quoted.rsp', '-Wl,--verbose'], 'Audit changed execution argv'
    assert len(records) == 1 and records[0]['sha256'] == ledger.digest(raw)
    # Removed response files can be replayed exactly from captured bytes.
    saved = {r['path']: r['text'].encode() for r in records}
    assert ledger.expanded(command, cwd, lambda p: saved[str(p)]) == (expanded, records)
    for raw_bad in (b'@other.rsp', b'@quoted.rsp', b'\0', b'\xff\xfea', b'\xef\xbb\xbfa', b'\xff', b'"unfinished', b'escape\\'):
        reject(lambda: ledger.expanded(command, cwd, lambda p: raw_bad))
    for text, expected in [('a\\ b "c\\ d"', ['a b', 'c d']), ("'a\\\"b'", ['a"b']), ('a#b', ['a#b']), ('""\t\r\n\'\' ', ['', ''])]:
        assert ledger.gnu_tokens(text) == expected
    print('PASS selected-link pure: linked/not-linked + identical duplicates; '+str(len(negatives))+' proof, 2 mode, 6 command, 8 response negatives; no compiler/process/network')


def actual_replay(log, authority):
    raw = log.read_bytes()
    assert ledger.digest(raw) == 'f99bd657b3d8f6b6033d4e262492873654c5f0f74eabd707f8e0c6655ecec864'
    a_raw = authority.read_bytes()
    assert ledger.digest(a_raw) == 'aba3e32beef66ec5ca6919e65f5e49f8b40f587599a8d2f43205e60bab873701'
    a = json.loads(a_raw)
    assert a['GITHUB_RUN_ID'] == '41135044960' and a['GITHUB_RUN_ATTEMPT'] == '1'
    assert a['GITHUB_SHA'] == 'f0a8364afd3380eb0e25b8d82824dd1752f91e16'
    # GitHub's saved job stream contains one BOM before a transport timestamp
    # mid-map. Strip only that authenticated log prefix, not map input bytes.
    lines = [re.sub(r'^\ufeff?\d{4}-\d\d-\d\dT\S+Z ', '', l) for l in raw.decode('utf-8').splitlines()]
    first = lines.index('MINIMUM_CONTROL_RAW_MAP_BEGIN tag=static')
    last = lines.index('MINIMUM_CONTROL_RAW_MAP_END', first)
    mapping = '\n'.join(lines[first+1:last]) + '\n'
    before = max(i for i in range(first) if lines[i] == 'MINIMUM_CONTROL_RAW_SYMBOLS_END')
    trace = '\n'.join(l for l in lines[before+1:first] if l.startswith('ld.lld:')) + '\n'
    start = lines.index('MINIMUM_CONTROL_RAW_IMPORTS_BEGIN tag=static', last)
    end = lines.index('MINIMUM_CONTROL_RAW_IMPORTS_END', start)
    imported = '\n'.join(lines[start+1:end]) + '\n'
    assert ledger.digest(mapping.encode()) == '3e3a578a309fe72d78725e71eb59e3ce18db0061623fd3d0b46fca9d212800da'
    assert ledger.digest(trace.encode()) == '67c4315a459db9005c2877bd2e651cee7a321f8dd85a31fb6734fc0e57342805'
    assert ledger.digest(imported.encode()) == '21285f78bf2ac9f65e4d3a12e8684d283970a8a9a22ac1e63afd53a255daa3a8'
    root = a['compiler'].split('InstalledDir: ', 1)[1].split('/bin', 1)[0]
    data = dict(compiler_root=root, authority=a)
    cwd = PurePosixPath('/home/runner/work/gesher/gesher')
    result = ledger.prove(trace, mapping, imported, data, cwd)
    assert result['status'] == 'LINKED' and len(result['definitions']) == 142 and len(result['loaded_members']) == 7
    rows = {}
    for line in lines:
        match = re.fullmatch(r'MINIMUM_CONTROL_MEMBER tag=static symbol=(\S+) (.*)', line)
        if match:
            assert match[1] not in rows
            rows[match[1]] = json.loads(match[2])
    assert len(rows) == 18
    for symbol, old in rows.items():
        new = result['definitions'][symbol]
        assert all(new[k] == old[k] for k in ('archive', 'archive_sha256', 'member'))
        assert new['address'] == old['map_rva']
    member = 'libwinpthread_la-thread.o'
    archive = root + '/x86_64-w64-mingw32/lib/libwinpthread.a'
    reading = 'ld.lld: Reading ' + archive + '\n'
    assert trace.count(reading) == 1
    negatives = [
        (trace.replace(reading, ''), mapping),
        (trace + 'ld.lld: Reading /other/libwinpthread.a\n', mapping),
        (trace.replace(archive, '/other/libwinpthread.a'), mapping),
        (trace + 'ld.lld: Reading /other/ordinary.a\nld.lld: Loaded ordinary.a('+member+') for unrelated\n', mapping),
        (trace, mapping.replace(member+':(', 'libwinpthread.a('+member+'):( ')),
        (trace, mapping+'00001000 00000020 16 '+member+':(.text)\n00000001 00000000 0                 pthread_self\n'),
        (trace+'ld.lld: Reading /home/runner/work/gesher/gesher/direct/'+member+'\n', mapping),
    ]
    for t, m in negatives:
        reject(lambda: ledger.prove(t, m, imported, data, cwd))
    print('PASS actual411350/a1 full static map/trace/import replay: 142 live definitions/7 members; all18 prior bindings equal; 7 actual-derived ablations rejected; historical5c53 only, no new runtime claim')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--historical-authority', type=Path, required=True)
    parser.add_argument('--actual-producer-log', type=Path)
    parser.add_argument('--actual-authority', type=Path)
    args = parser.parse_args()
    raw = args.historical_authority.read_bytes()
    assert ledger.digest(raw) == '9fa310b69e9bc4a0c7081b8667c8dcb019481b07e644cb7875334d880a6886f8'
    data = json.loads(raw)
    assert data['GITHUB_RUN_ID'] == '41139659911' and data['GITHUB_RUN_ATTEMPT'] == '1'
    assert data['GITHUB_SHA'] == '695229f0c31939ef4a95dc0cb4eef7ba5172786a'
    controls(data)
    sealed_controls(data)
    assert bool(args.actual_producer_log) == bool(args.actual_authority)
    if args.actual_producer_log:
        actual_replay(args.actual_producer_log, args.actual_authority)


if __name__ == '__main__':
    main()
