"""Pure selected-link metadata controls. Never starts a compiler/process."""
import argparse
import copy
import json
from pathlib import Path, PurePosixPath
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
                     map='Address  Size     Align Out     In      Symbol\n00001000 00000020 16 main.obj:(.text)\n00001000 00000000 0                 main\n',
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
        assert ledger.verify_sealed(directory, link, compiler, 'payload.txt') == record
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
            reject(lambda: ledger.verify_sealed(directory, link, compiler, 'payload.txt'))
        link = write(record)
        for key in ('sha256', 'receipt_sha256', 'output_sha256'):
            bad = dict(link)
            bad[key] = '0'*64
            reject(lambda: ledger.verify_sealed(directory, bad, compiler, 'payload.txt'))
        (directory / 'trace.txt').write_text('CHANGED', encoding='utf-8')
        reject(lambda: ledger.verify_sealed(directory, link, compiler, 'payload.txt'))
    print('PASS portable sealed metadata: 13 receipt/output/record/command/evidence negatives; textual fixtures only, no native/process/network')


def controls(authority):
    # Historical actual metadata supplies only the symbol inventory; this is a
    # visibly SYNTHETIC map/command projection, never a new actual link receipt.
    data = dict(compiler_root='/SYNTHETIC/sdk', authority=authority)
    cwd = Path('/SYNTHETIC/out').resolve()
    proof_cwd = PurePosixPath('/SYNTHETIC/out')
    target = PurePosixPath(data['compiler_root'], 'x86_64-w64-mingw32/lib/libpthread.a')
    trace = f'ld.lld: Reading {target}\nld.lld: Loaded libpthread.a(thread.o) for pthread_self\n'
    header = 'Address  Size     Align Out     In      Symbol\n'
    mapping = header + '00001000 00000020 16 libpthread.a(thread.o):(.text)\n00001000 00000000 0                 pthread_self\n'
    imported = 'File: SYNTHETIC.exe\nFormat: COFF-x86-64\nImport {\n  Name: KERNEL32.dll\n}\n'
    linked = ledger.prove(trace, mapping, imported, data, proof_cwd)
    assert linked['status'] == 'LINKED' and set(linked['definitions']) == {'pthread_self'}
    assert ledger.prove(trace, mapping + mapping, imported, data, proof_cwd) == linked
    unused = ledger.prove('ld.lld: Reading /SYNTHETIC/out/ordinary.obj\n',
                          header + '00001000 00000020 16 ordinary.obj:(.text)\n00001000 00000000 0                 main\n', imported, data, proof_cwd)
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
        (trace.replace('Loaded libpthread.a(thread.o)', 'Loaded libpthread.a(other.o)'), mapping, imported),
        (trace, mapping.replace('libpthread.a(thread.o)', 'ordinary.obj'), imported),
        (trace, mapping.replace('libpthread.a(thread.o)', 'other.a(thread.o)'), imported),
        (trace, mapping.replace('libpthread.a(thread.o)', 'libpthread.a(other.o)'), imported),
        (trace.replace('Loaded libpthread.a(', 'Loaded /SYNTHETIC/other/libpthread.a('), mapping, imported),
        (trace, mapping.replace('libpthread.a(thread.o)', '/SYNTHETIC/other/libpthread.a(thread.o)'), imported),
        (trace, mapping + mapping.replace('00001000', '00002000'), imported),
        (trace, mapping + mapping.replace('libpthread.a(thread.o)', 'other.a(thread.o)'), imported),
        (trace, mapping, imported.replace('KERNEL32.dll', 'libwinpthread-1.dll')),
        (trace, mapping, imported + 'Symbol: pthread_self (0)\n'),
        (trace, mapping, imported + 'Symbol: __imp_pthread_join (0)\n'),
        (trace, '00001000 00000020 16 ordinary.obj:(.text)\n', imported),
        ('ld.lld: Reading /SYNTHETIC/out/ordinary.obj\n', mapping, imported),
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--historical-authority', type=Path, required=True)
    args = parser.parse_args()
    raw = args.historical_authority.read_bytes()
    assert ledger.digest(raw) == '9fa310b69e9bc4a0c7081b8667c8dcb019481b07e644cb7875334d880a6886f8'
    data = json.loads(raw)
    assert data['GITHUB_RUN_ID'] == '41139659911' and data['GITHUB_RUN_ATTEMPT'] == '1'
    assert data['GITHUB_SHA'] == '695229f0c31939ef4a95dc0cb4eef7ba5172786a'
    controls(data)
    sealed_controls(data)


if __name__ == '__main__':
    main()
