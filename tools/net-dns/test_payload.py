# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 The Gesher Authors
"""Synthetic manifest controls only; these files are never executable suppliers."""
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import payload


class DnsEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.canonical = dict(source='a' * 40, run='9', attempt='1',
                              repository='gesher-runtime/gesher', repository_id='1426153186',
                              event='push', supplier_head='a' * 40, head_repository_id='1426153186')
        self.diagnostic = dict(self.canonical, repository='yom-chadash/gesher', repository_id='1460486951',
                               evidence_scope='fork-diagnostic', canonical_authority=False,
                               event='workflow_dispatch', ref='refs/heads/diagnostic/test', workflow='ci',
                               workflow_sha='a' * 40,
                               workflow_ref='yom-chadash/gesher/.github/workflows/ci.yml@refs/heads/diagnostic/test',
                               upstream_repository='gesher-runtime/gesher', upstream_repository_id='1426153186')
        # Fork diagnostics retain their independent schema and identity fields.
        del self.diagnostic['supplier_head'], self.diagnostic['head_repository_id']
        self.temp = tempfile.TemporaryDirectory(prefix='dns-metadata-only-')
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.out = self.folder / 'net-dns'
        self.out.mkdir()
        self.addCleanup(patch.stopall)
        # Identity/API and the tracked checkout are the explicit test boundaries.
        # The actual manifest/member/compiler/ABI/reference verifier runs below.
        patch.object(payload, 'execution_context', side_effect=lambda value:
                     self.canonical if value is None else self.diagnostic).start()
        patch.object(payload.producer_context, 'authenticate', return_value=self.canonical).start()
        patch.object(payload, 'source_inventory', return_value={
            'synthetic-metadata-only': {'mode': '100644', 'git_blob': 'b' * 40}}).start()
        for abi, machine in (('x86_64', 62), ('arm64-v8a', 183)):
            directory = self.folder / abi
            directory.mkdir()
            data = bytearray(64)
            data[:6] = b'\x7fELF\x02\x01'
            data[18:20] = machine.to_bytes(2, 'little')
            data += b'SYNTHETIC METADATA ONLY - NOT AN EXECUTABLE'
            (directory / 'net_dns_wire').write_bytes(data)
        common = '\n'.join(['DNS synthetic metadata row ' + str(i) for i in range(96)] +
                           ['DNS all done rows=96 failures=0']) + '\n'
        (self.out / 'linux-reference.log').write_text(common)
        for abi in ('x86_64', 'arm64'):
            (self.out / ('aosp-reference-' + abi + '.log')).write_text(common)
            (self.out / ('aosp-extended-' + abi + '.log')).write_text('DNS all done rows=174 failures=0\n')

    def write(self, diagnostic):
        context = self.diagnostic if diagnostic is not None else self.canonical
        records = dict(context=copy.deepcopy(context), archive=dict(publisher_sha1='090e8083a715fdb1a3e402d0763c388abb03fb4e',
                       size=663987688, sha256='c' * 64), linux=[{}], aosp=[{}, {}], guests={})
        for abi in ('x86_64', 'arm64-v8a'):
            records['guests'][abi] = dict(command=['-Wall', '-Wextra', '-Werror', '-static-libstdc++'],
                version='clang version SYNTHETIC METADATA ONLY', clang_sha256='d' * 64,
                private_abi={'guest': {'sha256': payload.digest(self.folder / abi / 'net_dns_wire')}})
        self.write_json('compilers.json', records)
        self.seal(diagnostic)
        return records

    def write_json(self, name, value):
        (self.out / name).write_text(json.dumps(value))

    def seal(self, diagnostic):
        self.write_json('manifest.json', payload.expected(self.folder, diagnostic))

    def verify(self, diagnostic):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            payload.verify(self.folder, diagnostic)
        return output.getvalue()

    def test_each_evidence_type_passes_only_its_own_schema_and_label(self):
        for context, schema, label in ((None, 1, 'DNS_FULL_VERIFIED'),
                (self.diagnostic, 'gesher-dns-fork-diagnostic-v1', 'DNS_FORK_DIAGNOSTIC_VERIFIED')):
            with self.subTest(schema=schema):
                self.write(context)
                manifest = payload.read(self.out / 'manifest.json')
                self.assertEqual(manifest['schema'], schema)
                self.assertIn(label, self.verify(context))
                other = self.diagnostic if context is None else None
                with self.assertRaises(RuntimeError):
                    self.verify(other)

    def test_identity_scope_schema_source_and_typed_members_cannot_change(self):
        self.write(self.diagnostic)
        wanted = payload.read(self.out / 'manifest.json')
        mutants = []
        for key, value in [('schema', 1), ('producer', 'native2 / native2')]:
            bad = copy.deepcopy(wanted); bad[key] = value; mutants.append(bad)
        for key, value in [('repository', 'gesher-runtime/gesher'), ('repository_id', '1426153186'),
                           ('source', 'f' * 40), ('run', '10'), ('attempt', '2'), ('event', 'push'),
                           ('evidence_scope', 'canonical'), ('canonical_authority', True)]:
            bad = copy.deepcopy(wanted); bad['context'][key] = value; mutants.append(bad)
        bad = copy.deepcopy(wanted); bad['sources'] = {}; mutants.append(bad)
        for key in ('role', 'sha256'):
            bad = copy.deepcopy(wanted)
            bad['members']['x86_64/net_dns_wire'][key] = 'forged'
            mutants.append(bad)
        bad = copy.deepcopy(wanted); del bad['members']['arm64-v8a/net_dns_wire']; mutants.append(bad)
        for index, mutant in enumerate(mutants):
            self.write_json('manifest.json', mutant)
            with self.subTest(index=index), self.assertRaises(RuntimeError):
                self.verify(self.diagnostic)

    def test_resealing_does_not_bypass_existing_compiler_and_abi_checks(self):
        changes = [lambda x: x['context'].update(attempt='2'),
                   lambda x: x['archive'].update(size=1),
                   lambda x: x['archive'].update(publisher_sha1='f' * 40),
                   lambda x: x['guests'].pop('arm64-v8a'),
                   lambda x: x['linux'].clear(),
                   lambda x: x['aosp'].pop(),
                   lambda x: x['guests']['x86_64'].update(command=['-O2']),
                   lambda x: x['guests']['x86_64'].update(version='wrong compiler'),
                   lambda x: x['guests']['x86_64']['private_abi']['guest'].update(sha256='f' * 64)]
        for context in (None, self.diagnostic):
            for index, change in enumerate(changes):
                records = self.write(context)
                change(records)
                self.write_json('compilers.json', records)
                self.seal(context)
                with self.subTest(diagnostic=context is not None, index=index), self.assertRaises(RuntimeError):
                    self.verify(context)

    def test_reference_content_and_member_inventory_stay_mandatory(self):
        self.write(self.diagnostic)
        for name in ('aosp-reference-x86_64.log', 'aosp-extended-arm64.log'):
            path = self.out / name
            original = path.read_bytes()
            path.write_text('DNS corrupt reference\n')
            self.seal(self.diagnostic)
            with self.subTest(name=name), self.assertRaises(RuntimeError):
                self.verify(self.diagnostic)
            path.write_bytes(original)
        self.seal(self.diagnostic)
        (self.out / 'extra').write_text('unexpected member')
        with self.assertRaises(RuntimeError):
            self.verify(self.diagnostic)

    def test_wrong_elf_role_fails_even_with_consistent_member_hashes(self):
        for context in (None, self.diagnostic):
            records = self.write(context)
            elf = self.folder / 'x86_64/net_dns_wire'
            original = elf.read_bytes()
            wrong = bytearray(original)
            wrong[18:20] = (183).to_bytes(2, 'little')
            elf.write_bytes(wrong)
            records['guests']['x86_64']['private_abi']['guest']['sha256'] = payload.digest(elf)
            self.write_json('compilers.json', records)
            self.seal(context)
            with self.subTest(diagnostic=context is not None), self.assertRaisesRegex(RuntimeError, 'Wrong DNS ELF role'):
                self.verify(context)
            elf.write_bytes(original)

    def test_linked_asset_is_not_a_valid_member(self):
        self.write(self.diagnostic)
        path = self.out / 'aosp-reference-arm64.log'
        path.unlink()
        path.symlink_to(self.out / 'linux-reference.log')
        with self.assertRaisesRegex(RuntimeError, 'Unexpected DNS asset members'):
            self.verify(self.diagnostic)

    def test_default_context_itself_remains_canonical_only(self):
        env = dict(GITHUB_ACTIONS='true', GITHUB_SHA='a' * 40, GITHUB_RUN_ID='9', GITHUB_RUN_ATTEMPT='1',
                   GITHUB_REPOSITORY='gesher-runtime/gesher', GITHUB_REPOSITORY_ID='1426153186',
                   GITHUB_WORKFLOW='ci', GITHUB_EVENT_NAME='push', GITHUB_EVENT_PATH=str(self.folder / 'event.json'))
        (self.folder / 'event.json').write_text('{}')
        with patch.dict(os.environ, env, clear=True), \
                patch.object(payload.subprocess, 'check_output', return_value='a' * 40 + '\n'):
            self.assertEqual(payload.context(), self.canonical)
            os.environ['GITHUB_REPOSITORY'] = 'yom-chadash/gesher'
            os.environ['GITHUB_REPOSITORY_ID'] = '1460486951'
            with self.assertRaisesRegex(RuntimeError, 'Wrong repository'):
                payload.context()


if __name__ == '__main__':
    unittest.main()
