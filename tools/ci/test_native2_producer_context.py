"""Synthetic native2 payloads through real consumer, archive and identity checks.

Compiler/ELF/reference metadata here is deliberately synthetic, never evidence of
executed DNS/UDP behavior. Remote transport and the checkout identity/inventory
are the test boundaries; the actual payload validators run unchanged below.
"""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import runpy
import sys
import tempfile
import unittest
from unittest.mock import patch

import test_binder_modes as binder
import producer_context as proof

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/net-dns'))
import payload_shared
spec = importlib.util.spec_from_file_location('rerun_dns_payload', ROOT / 'tools/net-dns/payload.py')
dns = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dns)
SOURCES = {'SYNTHETIC_SOURCE_INVENTORY': {'mode': '100644', 'git_blob': 'b' * 40}}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, sort_keys=True) + '\n')


class Native2Scenario(binder.Scenario):
    def __init__(self, kind, old=1, now=3):
        self.temp = tempfile.TemporaryDirectory(prefix='native2-rerun-metadata-')
        self.root = Path(self.temp.name)
        self.fixture = self.root / 'fixtures'
        self.fixture.mkdir()
        self.old, self.now, self.workflow, self.kind = old, now, 'ci', kind
        self.consumer = dict(source='1' * 40, repository='gesher-runtime/gesher', repository_id='7',
                             run='9', attempt=str(now), event='pull_request',
                             supplier_head='2' * 40, head_repository_id='8')
        self.original = dict(self.consumer, attempt=str(old))
        self.producer = 'native2 / native2'
        self.old_job = dict(id=10, name=self.producer, run_id=9, run_attempt=old, head_sha='2' * 40,
                            status='completed', conclusion='success', started_at='2026-11-20T12:01:13Z',
                            completed_at='2026-11-20T12:07:27Z')
        self.new_job = dict(self.old_job, id=20, run_attempt=now)
        self.old_jobs, self.new_jobs = [self.old_job], [self.new_job]
        self.old_run = dict(id=9, run_attempt=old, head_sha='2' * 40, name='ci', event='pull_request',
                            repository=dict(id=7, full_name=self.consumer['repository']), head_repository=dict(id=8))
        self.new_run = dict(self.old_run, run_attempt=now)
        self.out = self.fixture / ('net-' + kind)
        self.out.mkdir()
        self.manifest_path = self.out / 'manifest.json'
        self.compiler_path = self.out / 'compilers.json'
        for abi, machine in (('x86_64', 62), ('arm64-v8a', 183)):
            folder = self.fixture / abi
            folder.mkdir()
            elf = bytearray(64)
            elf[:6] = b'\x7fELF\x02\x01'
            elf[18:20] = machine.to_bytes(2, 'little')
            elf += b'SYNTHETIC METADATA ONLY, NOT AN EXECUTABLE'
            for name in (('net_dns_wire',) if kind == 'dns' else ('net_udp_trunc', 'net_udp_fault')):
                (folder / name).write_bytes(elf)
        if kind == 'dns':
            self.make_dns()
        else:
            self.make_udp()
        self.archive = self.root / 'native2.zip'
        self.make_archive()
        self.item = dict(id=30, name='native2', digest='sha256:' + sha(self.archive),
                         size_in_bytes=self.archive.stat().st_size, expired=False,
                         created_at='2026-11-20T12:06:40Z', workflow_run=dict(id=9, head_sha='2' * 40,
                         repository_id=7, head_repository_id=8))
        self.items, self.downloads, self.runs, self.post_download = [self.item], 0, 0, None

    def metadata(self, context):
        return dict(context=copy.deepcopy(context), archive=dict(
            publisher_sha1='090e8083a715fdb1a3e402d0763c388abb03fb4e', size=663987688,
            sha256='c' * 64), guests={})

    def make_dns(self):
        common = '\n'.join(['DNS synthetic metadata row ' + str(i) for i in range(96)] +
                           ['DNS all done rows=96 failures=0']) + '\n'
        (self.out / 'linux-reference.log').write_text(common)
        for abi in ('x86_64', 'arm64'):
            (self.out / f'aosp-reference-{abi}.log').write_text(common)
            (self.out / f'aosp-extended-{abi}.log').write_text('DNS all done rows=174 failures=0\n')
        records = self.metadata(self.original)
        records.update(linux=[{}], aosp=[{}, {}])
        for abi in ('x86_64', 'arm64-v8a'):
            records['guests'][abi] = dict(command=['-Wall', '-Wextra', '-Werror', '-static-libstdc++'],
                version='clang version SYNTHETIC METADATA ONLY', clang_sha256='d' * 64,
                private_abi={'guest': {'sha256': sha(self.fixture / abi / 'net_dns_wire')}})
        write_json(self.compiler_path, records)
        with patch.object(proof, 'current_context', return_value=self.original), \
                patch.object(dns, 'source_inventory', return_value=SOURCES):
            write_json(self.manifest_path, dns.expected(self.fixture))
            # This is exactly the build-side local validator, before upload.
            with contextlib.redirect_stdout(io.StringIO()):
                dns.validate_payload(self.fixture, dns.expected(self.fixture))

    def make_udp(self):
        reference = ['UDP synthetic api=' + str(i) for i in range(192)]
        reference += ['UDP synthetic metadata'] * 7 + ['UDP all passed; rows=192; alias=128; stream=1']
        (self.out / 'linux-reference.txt').write_text('\n'.join(reference) + '\n')
        (self.out / 'linux-fault-reference.txt').write_text('UDPFAULT synthetic metadata\n' * 176)
        (self.out / 'ndk-compiler.txt').write_text('clang version SYNTHETIC METADATA ONLY\n')
        (self.out / 'linux-compiler.txt').write_text('SYNTHETIC METADATA ONLY\n')
        info = dict(self.original, sources=SOURCES,
                    fixture_sha256=sha(ROOT / 'tests/native2/net_udp_trunc.cpp'),
                    fault_fixture_sha256=sha(ROOT / 'tests/native2/net_udp_fault_observer.c'))
        records = self.metadata(info)
        for abi in ('x86_64', 'arm64-v8a'):
            records['guests'][abi] = [dict(output=abi + '/' + name,
                command=['SYNTHETIC', '-Wall', '-Wextra', '-Werror'],
                version='clang version SYNTHETIC METADATA ONLY', clang_sha256='d' * 64,
                sha256=sha(self.fixture / abi / name)) for name in ('net_udp_trunc', 'net_udp_fault')]
        write_json(self.compiler_path, records)
        with patch.object(proof, 'current_context', return_value=self.original), \
                patch.object(payload_shared, 'source_inventory', return_value=SOURCES), \
                contextlib.redirect_stdout(io.StringIO()):
            self.run_udp('create')

    def run_udp(self, mode):
        with patch.object(sys, 'argv', ['payload.py', mode, str(self.fixture)]):
            runpy.run_path(str(ROOT / 'tools/net-udp/payload.py'), run_name='__main__')

    def check(self):
        with patch.object(payload_shared, 'source_inventory', return_value=SOURCES), \
                patch.object(dns, 'source_inventory', return_value=SOURCES):
            if self.kind == 'dns':
                dns.verify(self.fixture)
            else:
                self.run_udp('verify')

    def update_manifest(self, mutation):
        manifest = json.loads(self.manifest_path.read_bytes())
        mutation(manifest)
        write_json(self.manifest_path, manifest)

    def republish(self):
        """Build an authenticated synthetic archive containing deliberately bad metadata."""
        self.make_archive()
        self.item.update(digest='sha256:' + sha(self.archive), size_in_bytes=self.archive.stat().st_size)


class Native2ConsumerProvenance(unittest.TestCase):
    def scenario(self, kind, **kwargs):
        value = Native2Scenario(kind, **kwargs)
        self.addCleanup(value.close)
        return value

    def rejected(self, value):
        with self.assertRaises((RuntimeError, ValueError, KeyError, TypeError, FileNotFoundError)):
            value.verify()

    def test_same_retained_and_rebuilt_producers_keep_the_consumer_attempt(self):
        for kind in ('udp', 'dns'):
            for old, now in ((1, 1), (1, 2), (1, 3), (2, 3), (3, 3)):
                with self.subTest(kind=kind, producer=old, consumer=now):
                    value = self.scenario(kind, old=old, now=now)
                    value.verify()
                    self.assertEqual(value.downloads, 1)

    def test_nested_member_cannot_be_relabelled_as_current_or_another_role(self):
        for kind in ('udp', 'dns'):
            for mutation in ('attempt', 'role', 'path'):
                with self.subTest(kind=kind, mutation=mutation):
                    value = self.scenario(kind)
                    if mutation == 'path':
                        alias = value.fixture / 'wrong-subtree'
                        value.out.rename(alias)
                        value.out.symlink_to(alias, target_is_directory=True)
                    elif mutation == 'role':
                        value.update_manifest(lambda x: x.update(role='native2-' + ('dns' if kind == 'udp' else 'udp')))
                    else:
                        value.update_manifest(lambda x: (x if kind == 'udp' else x['context']).update(attempt='3'))
                    self.rejected(value)

    def test_source_inventory_and_member_hashes_remain_bound_to_current_checkout(self):
        for kind in ('udp', 'dns'):
            for mutation in ('source', 'sources', 'hash', 'member'):
                with self.subTest(kind=kind, mutation=mutation):
                    value = self.scenario(kind)
                    if mutation == 'member':
                        (value.fixture / 'x86_64' / ('net_udp_trunc' if kind == 'udp' else 'net_dns_wire')).write_bytes(b'changed')
                    elif mutation == 'sources':
                        value.update_manifest(lambda x: x.update(sources={}))
                    elif mutation == 'hash':
                        value.update_manifest(lambda x: next(iter(x['members'].values())).update(sha256='f' * 64))
                    else:
                        value.update_manifest(lambda x: (x if kind == 'udp' else x['context']).update(source='f' * 40))
                    self.rejected(value)

    def test_compiler_context_cannot_follow_consumer_instead_of_producer(self):
        for kind in ('udp', 'dns'):
            for mutation in ('attempt', 'archive', 'abi', 'command'):
                with self.subTest(kind=kind, mutation=mutation):
                    value = self.scenario(kind)
                    records = json.loads(value.compiler_path.read_bytes())
                    if mutation == 'attempt':
                        records['context']['attempt'] = '3'
                    elif mutation == 'archive':
                        records['archive']['size'] = 1
                    elif mutation == 'abi':
                        del records['guests']['arm64-v8a']
                    else:
                        command = records['guests']['x86_64']
                        if kind == 'udp':
                            command = command[0]
                        command['command'] = ['SYNTHETIC']
                    write_json(value.compiler_path, records)
                    value.update_manifest(lambda x: x['members']['net-' + kind + '/compilers.json'].update(
                        sha256=sha(value.compiler_path)))
                    # Even authenticated producer output must satisfy the exact
                    # compiler/archive/ABI contracts after the archive proof.
                    value.republish()
                    self.rejected(value)

    def test_current_producer_reexecution_and_ambiguous_artifacts_reject_old_payload(self):
        for kind in ('udp', 'dns'):
            for mutation in ('reexecuted', 'missing-job', 'duplicate-artifact', 'digest'):
                with self.subTest(kind=kind, mutation=mutation):
                    value = self.scenario(kind)
                    if mutation == 'reexecuted':
                        value.new_job.update(started_at='2026-11-20T13:01:13Z', completed_at='2026-11-20T13:07:27Z')
                    elif mutation == 'missing-job':
                        value.new_jobs.clear()
                    elif mutation == 'duplicate-artifact':
                        value.items.append(dict(value.item, id=31))
                    else:
                        value.archive.write_bytes(value.archive.read_bytes() + b'changed')
                    self.rejected(value)

    def test_manual_workflow_cannot_reuse_old_attempt_without_proof(self):
        for kind in ('udp', 'dns'):
            with self.subTest(kind=kind):
                value = self.scenario(kind)
                value.workflow = 'native2'
                self.rejected(value)
                self.assertEqual(value.downloads, 0)


if __name__ == '__main__':
    unittest.main()
