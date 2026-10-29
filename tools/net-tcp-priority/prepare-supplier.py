"""CI-only private static supplier experiment; never mutates the shared compiler.

The minimal patch changes detached spin-lock retirement, not public/private
layout. This is a scoped static experiment, not global supplier adoption.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
UPSTREAM = 'a3d93999ef0521681d45e445c39964a7af0f593f'
ARCHIVE_SHA256 = '5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4'
NAME = 'llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64'
URL = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/' + NAME + '.tar.xz'
INPUTS = {
    'src/thread.c': 'c2fcaa54269f0e773069aeeec8212cac71f417483bf65d5e75f100311df4080b',
    'src/thread.h': '3c105e10217cabae5a34d1e6e83352fb4eb8a9c2cf048de8053351eb6edf6a25',
    'include/pthread.h': 'bf3fd920306acb4144873d1099490cf332700c268a8ede0e484c7c0e721c7995',
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def output(*argv, cwd=ROOT):
    return subprocess.check_output([str(a) for a in argv], cwd=cwd, text=True, timeout=180)


def main():
    assert os.environ.get('GITHUB_ACTIONS') == 'true'
    context = {k: os.environ[k] for k in ('GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_REPOSITORY', 'GITHUB_REPOSITORY_ID')}
    assert context['GITHUB_REPOSITORY'] == 'gesher-runtime/gesher'
    assert os.environ['GITHUB_REF'] == 'refs/heads/wip/net-tcp-priority'
    assert os.environ['GITHUB_JOB'] == 'compile'
    assert output('git', 'rev-parse', 'HEAD').strip() == context['GITHUB_SHA']
    workspace = ROOT / 'tcp-private-supplier'
    assert not workspace.exists(), 'Private supplier workspace must be fresh'
    workspace.mkdir()
    commands = []

    def run(argv, cwd, name, seconds=600):
        argv = [str(a) for a in argv]
        commands.append(dict(argv=argv, cwd=str(cwd), log=name))
        with (workspace / name).open('wb') as log:
            try:
                subprocess.run(argv, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=seconds)
            except (subprocess.SubprocessError, OSError):
                log.flush()
                print('TCP_PRIORITY_SUPPLIER_BUILD_FAILURE', name, flush=True)
                print('\n'.join((workspace / name).read_text(errors='replace').splitlines()[-100:]), flush=True)
                raise

    archive = workspace / 'compiler.tar.xz'
    run(['curl', '--fail', '--location', '--retry', '3', '--connect-timeout', '20', '--max-time', '180',
         URL, '-o', archive], ROOT, 'download.log', 240)
    assert sha(archive) == ARCHIVE_SHA256
    with tarfile.open(archive, 'r:xz') as tar:
        assert {Path(m.name).parts[0] for m in tar.getmembers()} == {NAME}
        tar.extractall(workspace, filter='data')
    llvm = workspace / NAME
    target = llvm / 'x86_64-w64-mingw32'
    assert llvm.is_dir() and target.is_dir()
    compiler = llvm / 'bin/x86_64-w64-mingw32-clang'
    os.environ['PATH'] = str(llvm / 'bin') + os.pathsep + os.environ['PATH']
    source = workspace / 'source'
    source.mkdir()
    run(['git', 'init', '-q'], source, 'git-init.log')
    run(['git', 'fetch', '--depth=1', 'https://github.com/mingw-w64/mingw-w64.git', UPSTREAM], source, 'git-fetch.log', 240)
    run(['git', 'checkout', '--detach', 'FETCH_HEAD'], source, 'git-checkout.log')
    assert output('git', 'rev-parse', 'HEAD', cwd=source).strip() == UPSTREAM
    pthreads = source / 'mingw-w64-libraries/winpthreads'
    for name, digest in INPUTS.items():
        assert sha(pthreads / name) == digest, name
    tracked = output('git', 'ls-files', '-z', 'mingw-w64-libraries/winpthreads', cwd=source).split('\0')
    original_sources = {p: sha(source / p) for p in tracked if p}
    header_names = sorted(p.name for p in (pthreads / 'include').glob('*.h'))
    assert 'pthread.h' in header_names
    headers_before = {p: sha(target / 'include' / p) for p in header_names}
    libraries = ('lib/libwinpthread.a', 'lib/libpthread.a', 'lib/libwinpthread.dll.a',
                 'lib/libpthread.dll.a', 'bin/libwinpthread-1.dll')
    before = {p: sha(target / p) for p in libraries}
    nm = llvm / 'bin/llvm-nm'
    readobj = llvm / 'bin/llvm-readobj'

    def symbols():
        raw = output(nm, '--extern-only', '--defined-only', target / 'lib/libwinpthread.a')
        names = sorted(set(re.findall(r'^\S+\s+[A-Za-z]\s+(\S+)\s*$', raw, re.M)))
        assert names and 'pthread_create' in names and 'pthread_join' in names
        return names

    def exports():
        raw = output(readobj, '--coff-exports', target / 'bin/libwinpthread-1.dll')
        names = sorted(re.findall(r'^\s*Name:\s*(\S+)\s*$', raw, re.M))
        assert names and len(names) == len(set(names)) and 'pthread_create' in names
        return names

    original_symbols, original_exports = symbols(), exports()
    assert not any('gesher_winpthread' in name for name in original_symbols + original_exports)
    patch = HERE / 'supplier-retirement.patch'
    run(['git', 'apply', '--check', patch], source, 'patch-check.log')
    run(['git', 'apply', patch], source, 'patch-apply.log')
    assert output('git', 'diff', '--name-only', cwd=source).splitlines() == ['mingw-w64-libraries/winpthreads/src/thread.c']
    run(['git', 'diff', '--check'], source, 'patch-whitespace.log')
    makefile = (pthreads / 'Makefile.am').read_text()
    assert re.findall(r'^libwinpthread_la_LDFLAGS (?:\+?=) (.+)$', makefile, re.M) == [
        '-no-undefined -version-info 1:0:0', '-L$(builddir)/fakelib -Wc,-no-pthread']
    build = workspace / 'build'
    build.mkdir()
    run([pthreads / 'configure', '--host=x86_64-w64-mingw32', '--prefix=' + str(target),
         '--libdir=' + str(target / 'lib'), '--enable-shared', '--enable-static',
         'CFLAGS=-g -O2 -mguard=cf -Werror', 'CXXFLAGS=-g -O2 -mguard=cf -Werror',
         'CC=' + str(compiler), 'AR=' + str(llvm / 'bin/llvm-ar'),
         'RANLIB=' + str(llvm / 'bin/llvm-ranlib'), 'RC=' + str(llvm / 'bin/x86_64-w64-mingw32-windres')],
        build, 'configure.log')
    # Keep warnings fatal; override only upstream's GCC-specific link wrapper
    # option, exactly after checking the original Makefile variable above.
    flags = 'libwinpthread_la_LDFLAGS=-no-undefined -version-info 1:0:0 -L$(builddir)/fakelib'
    run(['make', '-j2', 'V=1', flags], build, 'build.log')
    run(['make', 'install', flags], build, 'install.log')
    headers_after = {p: sha(target / 'include' / p) for p in header_names}
    assert headers_before == headers_after, 'Public supplier headers changed'
    assert symbols() == original_symbols and exports() == original_exports, 'Supplier symbol inventory changed'
    after = {p: sha(target / p) for p in libraries}
    assert before['lib/libwinpthread.a'] != after['lib/libwinpthread.a']
    changed_sources = {p: sha(source / p) for p in tracked if p}
    assert {p for p in original_sources if original_sources[p] != changed_sources[p]} == {'mingw-w64-libraries/winpthreads/src/thread.c'}
    receipt = dict(context=context, role='static-detached-retirement-experiment-v1', upstream=UPSTREAM,
                   source_before=original_sources, source_after=changed_sources, patch_sha256=sha(patch),
                   compiler_archive=dict(url=URL, sha256=sha(archive), bytes=archive.stat().st_size),
                   private_compiler=str(llvm), producer_job='compile', compiler_driver_sha256=sha(compiler),
                   cxx_driver_sha256=sha(llvm / 'bin/x86_64-w64-mingw32-clang++'),
                   compiler_real_sha256=sha(compiler.resolve()), underlying_clang_sha256=sha((llvm / 'bin/clang').resolve()),
                   compiler_version=output(compiler, '--version'), libraries_before=before, libraries_after=after,
                   public_headers_before=headers_before, public_headers_after=headers_after,
                   symbols=original_symbols, dll_exports=original_exports,
                   notice=(pthreads / 'COPYING').read_text(), notice_sha256=sha(pthreads / 'COPYING'), commands=commands,
                   logs={**{p.name: sha(p) for p in workspace.glob('*.log')}, 'config.log': sha(build / 'config.log')})
    destination = ROOT / 'tcp-supplier-receipt.json'
    assert not destination.exists()
    destination.write_text(json.dumps(receipt, sort_keys=True, indent=2))
    print('TCP_PRIORITY_SUPPLIER_BUILD', json.dumps(receipt), flush=True)


if __name__ == '__main__':
    main()
