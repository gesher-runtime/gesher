#!/usr/bin/env python3
"""Build the production publisher overlay containing only self-cancellation repair.

The selected-minimum experiment and the original publisher cache are inputs to
neither mutation nor selection here. This factory extracts its own verified
publisher archive, patches pristine upstream source, and retains a separate
receipt for the actual static/DLL outputs and their original SDK/ABI surfaces.
Local build evidence does not claim Windows runtime qualification.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent / 'pthread-cancel'
PIN = json.loads((HERE / 'inputs.json').read_text())
NAME = 'llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64'
URL = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/' + NAME + '.tar.xz'
THREAD = 'mingw-w64-libraries/winpthreads/src/thread.c'
LIBRARIES = ('lib/libwinpthread.a', 'lib/libpthread.a', 'lib/libwinpthread.dll.a',
             'lib/libpthread.dll.a', 'bin/libwinpthread-1.dll')
CORE = ('pthread_setcancelstate', 'pthread_setcanceltype', 'pthread_self',
        'pthread_create', 'pthread_join', '_pthread_cleanup_dest')
# Reuse only pure SDK/ABI/PE/map validators. Their selected-role authority and
# two-patch build entry points are not called or altered by this production role.
sys.path.insert(0, str(ROOT / 'tools/host-pthread-authority'))
import minimum_receipt as original_profile
sys.path.insert(0, str(ROOT / 'tools/toolchain'))
import selected_link as link_evidence


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read(argv, cwd=ROOT):
    return subprocess.check_output([str(x) for x in argv], cwd=cwd, text=True, timeout=180)


def recipe():
    names = ('tools/toolchain/prepare_pthread_cancel.py', 'tools/toolchain/pthread_cancel_link.cmake',
             'tools/toolchain/pthread-cancel/inputs.json', 'tools/toolchain/pthread-cancel/self-cancellation.patch',
             'tools/host-pthread-authority/minimum_receipt.py', 'tools/host-pthread-authority/minimum-inputs.json',
             'tools/toolchain/selected_link.py', 'tools/toolchain/prepare_host_pthread.py',
             'tools/fonts/verify-standalone-link.py',
             'tools/toolchain/first_link_context.py', 'tools/web3/host_roles.py',
             'tools/web3/build-teardown-probe.sh',
             '.github/actions/setup-build/action.yml', '.github/workflows/ci.yml')
    return {name: sha(ROOT / name) for name in names}


def context():
    result = dict(commit=read(['git', 'rev-parse', 'HEAD']).strip(),
                  tree=read(['git', 'rev-parse', 'HEAD^{tree}']).strip(),
                  tracked_changes=bool(read(['git', 'diff', '--name-only', 'HEAD']).strip()))
    if os.environ.get('GITHUB_ACTIONS') == 'true':
        require(not result['tracked_changes'], 'CI overlay must use committed source')
        require(result['commit'] == os.environ['GITHUB_SHA'], 'CI checkout differs from GITHUB_SHA')
        result['actions'] = {key: os.environ[key] for key in
            ('GITHUB_SHA', 'GITHUB_REPOSITORY', 'GITHUB_REPOSITORY_ID', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_JOB')}
    return result


def inventory(llvm, target, header_names):
    headers = {name: sha(target / 'include' / name) for name in header_names}
    symbols = read([llvm / 'bin/llvm-nm', '--extern-only', '--defined-only', target / 'lib/libwinpthread.a'])
    exports = read([llvm / 'bin/llvm-readobj', '--coff-exports', target / 'bin/libwinpthread-1.dll'])
    imports = read([llvm / 'bin/llvm-readobj', '--coff-imports', target / 'bin/libwinpthread-1.dll'])
    return dict(files={name: sha(target / name) for name in LIBRARIES}, public_headers=headers,
                symbols=sorted(set(re.findall(r'^\S+\s+[A-Za-z]\s+(\S+)\s*$', symbols, re.M))),
                dll_exports=original_profile.exports(exports), dll_imports=original_profile.imports(imports))


def verify(path):
    record = json.loads(path.read_text())
    require(record['role'] == PIN['role'] and record['pin'] == PIN, 'Different runtime overlay role/pins')
    require(record['recipe'] == recipe(), 'Overlay recipe changed')
    llvm = Path(record['compiler_root'])
    target = llvm / 'x86_64-w64-mingw32'
    require(inventory(llvm, target, record['before']['public_headers']) == record['after'], 'Overlay libraries/headers/PE inventories changed')
    require(sha(llvm / 'bin/x86_64-w64-mingw32-clang') == record['compiler_sha256'], 'Compiler changed')
    require(sha(llvm / 'bin/x86_64-w64-mingw32-clang++') == record['cxx_sha256'], 'C++ compiler changed')
    require(record['source_after'][THREAD] == PIN['thread_output_sha256'], 'Different cancellation source')
    require(record['before']['files']['lib/libwinpthread.a'] == PIN['publisher_static_sha256'], 'Different original runtime')
    require(record['compiler_archive_sha256'] == PIN['compiler_archive_sha256'], 'Different original compiler archive')
    require(set(record['source_before']) == set(record['source_after']), 'Supplier source inventory changed')
    for name, expected in PIN['source_sha256'].items():
        require(record['source_before'][name] == expected, 'Different pristine supplier source: ' + name)
    original_profile.validate_layout(record['private_layout'])
    require(record['sdk_profile'] == record['supplier_profile'] == {'WINVER': 1537, '_WIN32_WINNT': 1537},
            'Different supplier SDK profile')
    require(record['after']['public_headers'] == record['before']['public_headers'] and
            record['after']['symbols'] == record['before']['symbols'] and
            record['after']['dll_exports'] == record['before']['dll_exports'] and
            record['after']['dll_imports'] == record['before']['dll_imports'], 'Runtime surface changed')
    require({name for name in record['source_before'] if record['source_before'][name] != record['source_after'][name]}
            == {THREAD}, 'Overlay includes unrelated source changes')
    return record


def compiler_identity(driver, env=os.environ):
    require(env.get('GESHER_HOST_PTHREAD_SELECTION') == 'publisher-cancel', 'Wrong production overlay selection')
    receipt = Path(env['GESHER_PTHREAD_CANCEL_RECEIPT'])
    require(sha(receipt) == env['GESHER_PTHREAD_CANCEL_RECEIPT_SHA256'], 'Production receipt hash mismatch')
    record = verify(receipt)
    require(record['context'] == context(), 'Production overlay belongs to another source/run')
    expected = Path(record['compiler_root']) / 'bin/x86_64-w64-mingw32-clang++'
    require(Path(driver).resolve() == expected.resolve(), 'Configured compiler bypasses production overlay')
    return dict(role=PIN['role'], receipt_sha256=sha(receipt), context=record['context'],
                recipe=record['recipe'],
                patch_sha256=PIN['patch_sha256'], thread_source_sha256=PIN['thread_output_sha256'],
                libraries=record['after']['files'], static_object_sha256=record['static_object_sha256'])


def build(args):
    work = args.work.resolve()
    require(not work.exists(), 'Overlay work directory must be fresh: ' + str(work))
    work.mkdir(parents=True)
    commands = []
    environment = dict(os.environ)
    for key in ('CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS', 'CPATH', 'C_INCLUDE_PATH',
                'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH', 'CONFIG_SITE', 'MAKEFLAGS'):
        environment.pop(key, None)

    def run(argv, cwd, name, timeout=600):
        argv = [str(x) for x in argv]
        commands.append(dict(argv=argv, cwd=str(cwd), log=name))
        with (work / name).open('w') as log:
            try:
                subprocess.run(argv, cwd=cwd, env=environment, stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=timeout)
            except (subprocess.SubprocessError, OSError):
                log.flush()
                print('\n'.join((work / name).read_text(errors='replace').splitlines()[-60:]), flush=True)
                raise

    archive = args.archive.resolve() if args.archive else work / 'compiler.tar.xz'
    if not args.archive:
        run(['curl', '--fail', '--location', '--retry', '3', '--connect-timeout', '20', '--max-time', '240',
             URL, '-o', archive], ROOT, 'download.log', 300)
    require(sha(archive) == PIN['compiler_archive_sha256'], 'Publisher archive hash mismatch')
    with tarfile.open(archive, 'r:xz') as tar:
        require({Path(item.name).parts[0] for item in tar.getmembers()} == {NAME}, 'Unexpected archive root')
        tar.extractall(work, filter='data')
    llvm = work / NAME
    target = llvm / 'x86_64-w64-mingw32'
    environment['PATH'] = str(llvm / 'bin') + os.pathsep + environment['PATH']
    source = work / 'source'
    source.mkdir()
    run(['git', 'init', '-q'], source, 'source-init.log')
    remote = args.source.resolve() if args.source else 'https://github.com/mingw-w64/mingw-w64.git'
    run(['git', 'fetch', '--depth=1', remote, PIN['upstream_commit']], source, 'source-fetch.log', 300)
    run(['git', 'checkout', '--detach', 'FETCH_HEAD'], source, 'source-checkout.log')
    require(read(['git', 'rev-parse', 'HEAD'], source).strip() == PIN['upstream_commit'], 'Different upstream source')
    tracked = read(['git', 'ls-files', '-z', 'mingw-w64-libraries/winpthreads'], source).strip('\0').split('\0')
    before_source = {name: sha(source / name) for name in tracked}
    for name, expected in PIN['source_sha256'].items():
        require(before_source[name] == expected, 'Pristine supplier source mismatch: ' + name)
    pthreads = source / 'mingw-w64-libraries/winpthreads'
    for header in (pthreads / 'include').glob('*.h'):
        require(sha(target / 'include' / header.name) == sha(header), 'Publisher public header differs: ' + header.name)
    require(sha(target / 'include/processthreadsapi.h') == PIN['sdk_processthreadsapi_sha256'], 'Publisher SDK changed')
    header_names = sorted(path.name for path in (pthreads / 'include').glob('*.h'))
    before = inventory(llvm, target, header_names)
    require(before['files']['lib/libwinpthread.a'] == before['files']['lib/libpthread.a'] == PIN['publisher_static_sha256'],
            'Not the original publisher runtime')
    patch = HERE / 'self-cancellation.patch'
    require(sha(patch) == PIN['patch_sha256'], 'Cancellation patch hash mismatch')
    run(['git', 'apply', '--check', patch], source, 'patch-check.log')
    run(['git', 'apply', patch], source, 'patch-apply.log')
    after_source = {name: sha(source / name) for name in tracked}
    require({name for name in tracked if before_source[name] != after_source[name]} == {THREAD}, 'Unrelated supplier edits')
    require(after_source[THREAD] == PIN['thread_output_sha256'], 'Patched cancellation source mismatch')
    compiler = llvm / 'bin/x86_64-w64-mingw32-clang'
    build_dir = work / 'build'
    build_dir.mkdir()
    flags = '-g -O2 -mguard=cf -Werror -Xclang -fdump-record-layouts'
    run([pthreads / 'configure', '--host=x86_64-w64-mingw32', '--prefix=' + str(target),
         '--libdir=' + str(target / 'lib'), '--enable-shared', '--enable-static', 'CFLAGS=' + flags,
         'CXXFLAGS=-g -O2 -mguard=cf -Werror', 'CC=' + str(compiler), 'AR=' + str(llvm / 'bin/llvm-ar'),
         'RANLIB=' + str(llvm / 'bin/llvm-ranlib'), 'RC=' + str(llvm / 'bin/x86_64-w64-mingw32-windres')],
        build_dir, 'configure.log')
    profile_source = build_dir / 'sdk-profile.c'
    profile_source.write_text('#define __LARGE_MBSTATE_T\n#ifdef HAVE_CONFIG_H\n#include "config.h"\n#endif\n'
        '#include <malloc.h>\n#include <signal.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <wchar.h>\n'
        '#define WIN32_LEAN_AND_MEAN\n#include <windows.h>\nDWORD (WINAPI *typed_getthreadid)(HANDLE)=GetThreadId;\n')
    profiles = {}
    for name, extra in (('sdk', []), ('supplier', ['-DHAVE_CONFIG_H', '-I.', '-I' + str(pthreads),
                        '-I' + str(pthreads / 'include'), '-DIN_WINPTHREAD', '-D__USE_MINGW_ANSI_STDIO=0'])):
        run([compiler, *extra, '-dM', '-E', profile_source], build_dir, name + '-macros.log')
        profiles[name] = original_profile.actual_macros((work / (name + '-macros.log')).read_text())
        run([compiler, *extra, '-Wall', '-Wextra', '-Werror', '-c', profile_source, '-o', name + '.obj'],
            build_dir, name + '-compile.log')
        machine = read([llvm / 'bin/llvm-readobj', '--file-headers', build_dir / (name + '.obj')])
        require('IMAGE_FILE_MACHINE_AMD64' in machine, 'Wrong supplier object architecture')
    require(profiles['sdk'] == profiles['supplier'], 'Configured supplier SDK changed')
    # Exact upstream variable check before removing its GCC-only -Wc wrapper.
    makefile = (pthreads / 'Makefile.am').read_text()
    require(re.findall(r'^libwinpthread_la_LDFLAGS (?:\+?=) (.+)$', makefile, re.M) ==
            ['-no-undefined -version-info 1:0:0', '-L$(builddir)/fakelib -Wc,-no-pthread'], 'Upstream link recipe changed')
    ldflags = 'libwinpthread_la_LDFLAGS=-no-undefined -version-info 1:0:0 -L$(builddir)/fakelib -Wl,-Map,' + str(build_dir / 'supplier.map') + ' -Wl,--verbose'
    run(['make', '-j2', 'V=1', ldflags], build_dir, 'build.log', 900)
    run(['make', 'install', ldflags], build_dir, 'install.log')
    after = inventory(llvm, target, header_names)
    for key in ('public_headers', 'symbols', 'dll_exports', 'dll_imports'):
        require(after[key] == before[key], 'Supplier surface changed: ' + key)
    require(after['files']['lib/libpthread.a'] == after['files']['lib/libwinpthread.a'] != PIN['publisher_static_sha256'], 'Static aliases missing repair')
    require(after['files']['lib/libpthread.dll.a'] == after['files']['lib/libwinpthread.dll.a'], 'Import aliases differ')
    log = (work / 'build.log').read_text()
    layouts = re.findall(r'^\s*0 \| struct _pthread_v\n(.*?\[sizeof=\d+, align=\d+\])', log, re.M | re.S)
    require(layouts, 'Actual supplier record layout missing')
    layout = layouts[-1]
    original_profile.validate_layout(layout)
    definitions = original_profile.map_symbols((build_dir / 'supplier.map').read_text(), CORE)
    owners = {row[0] for row in definitions.values()}
    require(len(owners) == 1, 'Cancellation and lifetime symbols have different DLL suppliers')
    dll_object = build_dir / next(iter(owners))
    require(dll_object.is_file() and dll_object.resolve().is_relative_to(build_dir), 'DLL object origin missing')
    member_names = read([llvm / 'bin/llvm-ar', 't', target / 'lib/libpthread.a']).splitlines()
    members = [name for name in member_names if name.endswith('thread.o')]
    require(len(members) == 1, 'Static thread object missing/ambiguous')
    member = members[0]
    member_bytes = subprocess.check_output([str(llvm / 'bin/llvm-ar'), 'p', str(target / 'lib/libpthread.a'), member])
    static_object = build_dir / 'src' / member
    require(static_object.is_file() and sha(static_object) == digest(member_bytes), 'Installed archive differs from actual source object')
    for obj, marker in ((static_object, ''), (dll_object, '-DDLL_EXPORT')):
        found = []
        for line in log.splitlines():
            if 'src/thread.c' not in line or (bool(marker) != ('-DDLL_EXPORT' in line)):
                continue
            tokens = shlex.split(line)
            if '-c' in tokens and '-o' in tokens:
                found.append((build_dir / tokens[tokens.index('-o') + 1]).resolve())
        require(obj.resolve() in found, 'Thread object not bound to actual source compile: ' + str(obj))
    require({name: sha(source / name) for name in tracked} == after_source, 'Supplier source changed during build')
    record = dict(role=PIN['role'], pin=PIN, recipe=recipe(), context=context(), compiler_root=str(llvm),
                  compiler_archive_sha256=sha(archive), compiler_sha256=sha(compiler),
                  cxx_sha256=sha(llvm / 'bin/x86_64-w64-mingw32-clang++'), before=before, after=after,
                  source_before=before_source, source_after=after_source, private_layout=layout,
                  sdk_profile=profiles['sdk'], supplier_profile=profiles['supplier'],
                  static_member=member, static_object_sha256=sha(static_object), dll_object_sha256=sha(dll_object),
                  library_symbols=read([llvm / 'bin/llvm-nm', '--defined-only', target / 'lib/libwinpthread.a']),
                  dll_symbol_owners=definitions, dll_map_sha256=sha(build_dir / 'supplier.map'), commands=commands,
                  logs={path.name: sha(path) for path in work.glob('*.log')})
    receipt = work / 'pthread-cancel-receipt.json'
    receipt.write_text(json.dumps(record, sort_keys=True, indent=2) + '\n')
    verify(receipt)
    print('PTHREAD_CANCEL_OVERLAY ' + json.dumps(dict(role=PIN['role'], receipt=str(receipt),
          receipt_sha256=sha(receipt), source_sha256=after_source[THREAD], static_sha256=after['files']['lib/libpthread.a'],
          dll_sha256=after['files']['bin/libwinpthread-1.dll'], private_bytes=PIN['calculated_private_size']), sort_keys=True), flush=True)
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
            output.write('compiler_root=' + str(llvm) + '\nreceipt=' + str(receipt) + '\nreceipt_sha256=' + sha(receipt) + '\n')


def prove_link(trace, mapping, imports, record, cwd):
    # The pure parser handles the pinned LLD's bare map-member names by binding
    # them against ALL actual Reading/Loaded archive observations. It does not
    # call the selected-minimum authority or assign its role to this overlay.
    inputs = dict(compiler_root=record['compiler_root'], authority=dict(files=record['after']['files'],
                  library_symbols={'lib/libwinpthread.a': record['library_symbols']}))
    proof = link_evidence.prove(trace, mapping, imports, inputs, cwd)
    require(proof['status'] == 'LINKED', 'Expected actual pthread implementation in product')
    for symbol in ('pthread_setcancelstate', 'pthread_setcanceltype'):
        require(symbol in proof['definitions'] and proof['definitions'][symbol]['member'] == record['static_member'],
                'Actual cancellation setter missing/different: ' + symbol)
    return proof


def capture_link(argv):
    require(os.environ.get('GESHER_HOST_PTHREAD_SELECTION') == 'publisher-cancel', 'Wrong product overlay link role')
    receipt = Path(os.environ['GESHER_PTHREAD_CANCEL_RECEIPT'])
    require(sha(receipt) == os.environ['GESHER_PTHREAD_CANCEL_RECEIPT_SHA256'], 'Link receipt hash mismatch')
    record = verify(receipt)
    require(record['context'] == context(), 'Product overlay belongs to another source/run')
    cwd = Path.cwd().resolve()
    args, responses = link_evidence.expanded(argv, cwd)
    llvm = Path(record['compiler_root'])
    require(Path(args[0]).resolve() in {(llvm / ('bin/x86_64-w64-mingw32-' + compiler)).resolve()
            for compiler in ('clang', 'clang++')}, 'Link bypasses verified production compiler')
    output, mapping = link_evidence.link_paths(args, cwd)
    trace = Path(str(output) + '.pthread-cancel-link.log')
    destination = Path(str(output) + '.pthread-cancel-link.json')
    destination.unlink(missing_ok=True)
    mapping.unlink(missing_ok=True)
    with trace.open('wb') as log:
        result = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print('\n'.join(trace.read_text(errors='replace').splitlines()[-80:]), flush=True)
        return result.returncode
    for item in responses:
        require(Path(item['path']).read_bytes() == item['text'].encode(), 'Link response file changed')
    imported = Path(str(output) + '.pthread-cancel-imports.txt')
    imported.write_text(read([llvm / 'bin/llvm-readobj', '--coff-imports', output]))
    proof = prove_link(trace.read_text(), mapping.read_text(), imported.read_text(), record, cwd)
    require(verify(receipt) == record and sha(receipt) == os.environ['GESHER_PTHREAD_CANCEL_RECEIPT_SHA256'],
            'Supplier changed during actual link')
    result = dict(role=PIN['role'], supplier_receipt_sha256=sha(receipt), cwd=str(cwd), argv=argv,
                  expanded_argv=args, response_files=responses, executable_sha256=sha(output), proof=proof,
                  evidence={kind: dict(path=str(path), sha256=sha(path)) for kind, path in
                            (('trace', trace), ('map', mapping), ('imports', imported))})
    destination.write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
    for line in trace.read_text(errors='replace').splitlines():
        if 'warning:' in line.lower():
            print(line, flush=True)
    print('PTHREAD_CANCEL_LINK ' + output.name + ' ' + result['executable_sha256'], flush=True)
    return 0


def check_links(args):
    record = verify(args.receipt)
    require(record['context'] == context(), 'Product evidence belongs to another source/run')
    results = {}
    args.output.mkdir(parents=True, exist_ok=True)
    for target in ('libbinder_probe', 'stdio_ofd_test', 'servicemanager', 'dalvikvm'):
        executable = (args.build / (target + '.exe')).resolve()
        capture = Path(str(executable) + '.pthread-cancel-link.json')
        linked = json.loads(capture.read_text())
        require(linked['role'] == PIN['role'] and linked['supplier_receipt_sha256'] == sha(args.receipt), 'Different product supplier receipt')
        require(linked['executable_sha256'] == sha(executable), 'Product changed after captured link')
        responses = {item['path']: item['text'].encode() for item in linked['response_files']}
        replay, response_files = link_evidence.expanded(linked['argv'], Path(linked['cwd']), lambda path: responses[str(path)])
        require(replay == linked['expanded_argv'] and response_files == linked['response_files'], 'Link invocation changed')
        actual_output, actual_map = link_evidence.link_paths(replay, Path(linked['cwd']))
        require(actual_output == executable and str(actual_map) == linked['evidence']['map']['path'], 'Wrong product/map link destination')
        evidence = {}
        for kind, item in linked['evidence'].items():
            raw = Path(item['path']).read_bytes()
            require(digest(raw) == item['sha256'], 'Actual link evidence changed: ' + target + '/' + kind)
            evidence[kind] = raw.decode()
            # Actual ART maps are large text. Preserve exact reconstructible
            # bytes in the review artifact without duplicating that raw volume.
            with gzip.open(args.output / (target + '-' + kind + '.gz'), 'wb') as compressed:
                compressed.write(raw)
        require(prove_link(evidence['trace'], evidence['map'], evidence['imports'], record, Path(linked['cwd'])) == linked['proof'],
                'Actual link ownership proof changed')
        shutil.copy2(capture, args.output / capture.name)
        results[target] = dict(executable_sha256=sha(executable), capture_sha256=sha(capture),
                               cancellation={name: linked['proof']['definitions'][name] for name in
                                             ('pthread_setcancelstate', 'pthread_setcanceltype')})
    shutil.copy2(args.receipt, args.output / args.receipt.name)
    evidence = args.receipt.parent
    for name in ('configure.log', 'build.log', 'install.log', 'sdk-macros.log', 'supplier-macros.log'):
        require(sha(evidence / name) == record['logs'][name], 'Supplier evidence changed: ' + name)
        shutil.copy2(evidence / name, args.output / name)
    require(sha(evidence / 'build/supplier.map') == record['dll_map_sha256'], 'Supplier DLL map changed')
    require(sha(evidence / 'source' / THREAD) == PIN['thread_output_sha256'], 'Supplier source evidence changed')
    shutil.copy2(evidence / 'build/supplier.map', args.output / 'supplier.map')
    shutil.copy2(evidence / 'source' / THREAD, args.output / 'patched-thread.c')
    shutil.copy2(evidence / 'source/mingw-w64-libraries/winpthreads/COPYING', args.output / 'COPYING.winpthreads.txt')
    shutil.copy2(HERE / 'self-cancellation.patch', args.output / 'self-cancellation.patch')
    linked = dict(role=PIN['role'], context=context(), supplier_receipt_sha256=sha(args.receipt), products=results)
    (args.output / 'pthread-cancel-links.json').write_text(json.dumps(linked, indent=2, sort_keys=True) + '\n')
    print('PTHREAD_CANCEL_LINKS ' + json.dumps(linked, sort_keys=True), flush=True)


def main():
    require(__debug__, 'Do not disable supplier validation assertions')
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    prepare = commands.add_parser('build')
    prepare.add_argument('--work', type=Path, default=ROOT / 'out-pthread-cancel')
    prepare.add_argument('--archive', type=Path, help='Existing exact publisher archive; always SHA256 verified')
    prepare.add_argument('--source', type=Path, help='Existing upstream Git clone; exact pinned commit is fetched into fresh work')
    validate = commands.add_parser('verify')
    validate.add_argument('receipt', type=Path)
    link = commands.add_parser('link')
    link.add_argument('argv', nargs=argparse.REMAINDER)
    links = commands.add_parser('links')
    links.add_argument('--receipt', type=Path, required=True)
    links.add_argument('--build', type=Path, default=ROOT / 'out')
    links.add_argument('--output', type=Path, default=ROOT / 'dist-pthread-cancel')
    args = parser.parse_args()
    if args.command == 'build':
        build(args)
    elif args.command == 'verify':
        verify(args.receipt)
    elif args.command == 'link':
        return capture_link(args.argv)
    else:
        check_links(args)


if __name__ == '__main__':
    sys.exit(main())
