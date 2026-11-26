#!/usr/bin/env python3
"""Run complete production identity and shortcut publication contracts with declared host boundaries."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    'app/java/com/gesher/app/shell/AppShell.java',
    'app/java/com/gesher/server/NotificationManagerService.java',
    'app/java/com/gesher/server/AppOpsModeStore.java',
)
MUTATIONS = {
    'key-uppercase': (SOURCES[0], 'encodeToString(bytes, false)', 'shortcut exact UTF-8/separator/lowercase identity'),
    'notification-uppercase': (SOURCES[1], 'encodeToString(digest.digest(), false)', 'notification complete signer digest'),
    'appops-uppercase': (SOURCES[2], 'encodeToString(digest, false)', 'AppOps complete signer digest'),
}


def prepare(args, work: Path) -> dict:
    work.mkdir(parents=True, exist_ok=True)
    classes = work / 'classes'
    if classes.exists(): shutil.rmtree(classes)
    classes.mkdir()
    library = work / 'libidentity_digest_host.so'
    subprocess.run(['cc', '-std=c11', '-shared', '-fPIC',
                    '-I' + str(args.java_home / 'include'),
                    '-I' + str(args.java_home / 'include/linux'),
                    str(ROOT / 'tests/zygote/path_host.c'), '-o', str(library)], check=True)
    classpath = os.pathsep.join(str(p.resolve()) for p in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig_jar))
    sources = []; hashes = {}
    for relative in SOURCES:
        source = ROOT / relative
        if args.source_ref:
            source = work / 'source' / relative; source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(subprocess.check_output(['git', 'show', args.source_ref + ':' + relative], cwd=ROOT))
        if args.mutation and relative == MUTATIONS[args.mutation][0]:
            text = source.read_text(); target = MUTATIONS[args.mutation][1]
            if text.count(target) != 1: raise ValueError('Expected exactly one mutation input: ' + target)
            source = work / 'source' / relative; source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(text.replace(target, target.replace('false', 'true')))
        hashes[relative] = hashlib.sha256(source.read_bytes()).hexdigest()
        sources.append(str(source))
    for relative in ('tests/dex-services/gesher/services/IdentityDigestContractTest.java',
                     'tests/java-performance/IdentityDigestHost.java',
                     'tests/java-performance/com/gesher/server/IdentitySignerHost.java'):
        source = ROOT / relative
        hashes[relative] = hashlib.sha256(source.read_bytes()).hexdigest(); sources.append(str(source))
    subprocess.run([str(args.java_home / 'bin/javac'), '-source', '17', '-target', '17',
                    '-XDstringConcat=inline', '-encoding', 'UTF-8', '-cp', classpath,
                    '-d', str(classes), *sources], check=True)
    record = {'source_ref': args.source_ref, 'mutation': args.mutation, 'source_sha256': hashes,
              'app_classes_sha256': hashlib.sha256(args.app_classes.read_bytes()).hexdigest(),
              'libcore_hex_sha256': hashlib.sha256((args.core_classes / 'libcore/util/HexEncoding.class').read_bytes()).hexdigest(),
              'preparation_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'benchmark_sha256': hashlib.sha256((ROOT / 'tools/benchmark_identity_digests.py').read_bytes()).hexdigest(),
              'classes_sha256': {str(p.relative_to(classes)): hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in sorted(classes.rglob('*.class'))},
              'java_version': subprocess.run([str(args.java_home / 'bin/java'), '-version'],
                                             capture_output=True, text=True, check=True).stderr,
              'command': [str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xms64m', '-Xmx256m',
                          '-cp', str(classes) + os.pathsep + classpath, 'IdentityDigestHost', str(library)],
              'environment': {'GESHER_SHORTCUTS': '0', 'GESHER_LOCALES': 'en-US'}}
    (work / 'compiled.json').write_text(json.dumps(record, indent=2) + '\n')
    print('IDENTITY_DIGEST_SOURCE ' + json.dumps(record['source_sha256']), flush=True)
    return record


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes', 'apksig-jar'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--source-ref', help='Compile all changed production classes from this exact reference')
    parser.add_argument('--mutation', choices=MUTATIONS, help='Require rejection of one actual producer encoding mutation')
    parser.add_argument('--work-dir', type=Path, help='Retain compiled inputs and manifest for controlled comparisons')
    parser.add_argument('--compile-only', action='store_true')
    args = parser.parse_args()
    if not shutil.which('cc'): parser.error('A C compiler is required for the existing executable-path host boundary')
    if args.compile_only and not args.work_dir: parser.error('--compile-only requires --work-dir')
    def execute(work):
        record = prepare(args, work)
        if args.compile_only: return
        environment = os.environ.copy(); environment.update(record['environment'])
        environment['ANDROID_DATA'] = str(work / 'data')
        environment.pop('GESHER_BINDER_UID', None)
        if not args.mutation:
            subprocess.run(record['command'] + ['contracts', str(work / 'inputs')],
                           env=environment, check=True, timeout=90)
        else:
            result = subprocess.run(record['command'] + ['contracts', str(work / 'inputs')],
                                    env=environment, capture_output=True, text=True, timeout=90)
            text = result.stdout + result.stderr
            (work / 'mutation.log').write_text(text)
            if result.returncode == 0 or MUTATIONS[args.mutation][2] not in text:
                raise AssertionError('Mutation did not fail its intended output assertion: ' + text)
            if 'IDENTITY_DIGEST_COMPLETE' in text: raise AssertionError('Failed producer claimed completion')
            print('IDENTITY_DIGEST_MUTATION_REJECTED ' + args.mutation, flush=True)
    if args.work_dir: execute(args.work_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='gesher-identity-digest-') as directory:
            execute(Path(directory))


if __name__ == '__main__': main()
