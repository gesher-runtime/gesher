#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Reject the old callback/retirement race and real DNS ownership regressions.

Compiles the complete production resolver and the same explicit platform/AOSP
adapters as test_dns_resolver_admission.py. The controlled case invokes actual
timeout transitions; the unchanged real 30-second deadline runs separately.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

import test_dns_resolver_admission as shared

ROOT = Path(__file__).resolve().parents[1]
WAIT = '''        until(() -> {
            Snapshot state = snapshot();
            boolean retired = state.requests == 32 && state.lookupQueue == 0;
            if (!retired && observationWaiting != null) observationWaiting.countDown();
            return retired;
        }, "queued timeout unlinks; timed-out uncancelable lookups keep their slots", end);'''
IMMEDIATE = '''        check(snapshot().requests == 32 && snapshot().lookupQueue == 0,
                "queued timeout unlinks; timed-out uncancelable lookups keep their slots");'''
UNLINK = '''            if (sLookups.remove(this)) lookupOwned = false;
            error = systemError("resNetworkResult", OsConstants.ETIMEDOUT);'''
RETAIN = '''            if (false && sLookups.remove(this)) lookupOwned = false;
            error = systemError("resNetworkResult", OsConstants.ETIMEDOUT);'''
OWNER = 'if (retired || preparing || lookupOwned || dispatchOwned || callbackOwned) return;'
EARLY = 'if (retired || preparing || dispatchOwned || callbackOwned) return;'


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise AssertionError('ambiguous current source boundary')
    return source.replace(before, after, 1)


def run(work, java_home):
    original = {p.name: p.read_text() for p in (ROOT / 'tests/dns-resolver').glob('*.java')}
    original['DnsResolver.java'] = (ROOT / 'app/java/android/net/DnsResolver.java').read_text()
    for path, content in shared.STUBS.items():
        assert Path(path).name not in original
        original[Path(path).name] = content
    variants = [
        ('current', None, None, None, None),
        ('original_immediate_observation', 'DnsResolverHost.java', WAIT, IMMEDIATE,
         'callback notification is not Executor retirement'),
        ('queued_lookup_not_unlinked', 'DnsResolver.java', UNLINK, RETAIN,
         'controlled queued timeout actually unlinks its lookup and completion tasks'),
        ('active_lookup_retired_early', 'DnsResolver.java', OWNER, EARLY,
         'returned callback still charges its blocked Executor and 32 native owners'),
    ]
    receipts = []
    for label, path, before, after, assertion in variants:
        directory = work / label
        directory.mkdir()
        sources = dict(original)
        if path:
            sources[path] = replace_once(sources[path], before, after)
        for name, content in sources.items():
            (directory / name).write_text(content)
        command = [str(java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                   '-d', str(directory), *map(str, sorted(directory.glob('*.java')))]
        build = subprocess.run(command, text=True, capture_output=True, timeout=60)
        (directory / 'compile.log').write_text(build.stdout + build.stderr)
        if build.returncode:
            raise AssertionError(label + ' compile failed: ' + build.stderr)
        invocation = [str(java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx256m',
                      '-cp', str(directory), 'android.net.DnsResolverHost', 'deadline-retirement']
        result = subprocess.run(invocation, text=True, capture_output=True, timeout=75)
        (directory / 'run.log').write_text(result.stdout + result.stderr)
        receipt = {'label': label, 'compile': command, 'invocation': invocation,
                   'returncode': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr,
                   'required_assertion': assertion,
                   'source_sha256': {name: hashlib.sha256(content.encode()).hexdigest()
                                     for name, content in sources.items()}}
        receipts.append(receipt)
        (work / 'receipts.json').write_text(json.dumps(receipts, indent=2) + '\n')
        if assertion:
            assert result.returncode == 1 and 'java.lang.AssertionError: ' + assertion in result.stderr, receipt
            if label == 'original_immediate_observation':
                assert 'queued timeout unlinks; timed-out uncancelable lookups keep their slots' in result.stderr
        else:
            assert result.returncode == 0 and not result.stderr, receipt
            assert 'DnsResolver production deadline-retirement contracts passed:' in result.stdout
        print('DNS_DEADLINE_RETIREMENT_CONTROL_PASS', label, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--build-output', type=Path)
    args = parser.parse_args()
    if args.build_output:
        args.build_output.mkdir(parents=True, exist_ok=True)
        if any(args.build_output.iterdir()):
            parser.error('build output must be empty')
        run(args.build_output, args.java_home)
    else:
        with tempfile.TemporaryDirectory(prefix='gesher-dns-retirement-') as directory:
            run(Path(directory), args.java_home)


if __name__ == '__main__':
    main()
