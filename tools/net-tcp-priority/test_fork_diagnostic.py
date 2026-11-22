# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 The Gesher Authors
"""Isolated synthetic metadata and actual source checks; never build a supplier."""
import ast
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import fork_supplier as fork
import selected_supplier as selected

identity = fork.identity
minimum = selected.minimum
ROOT = selected.ROOT
BASE = '0da2b7f18fa8013932a6cb77dac32423df9ef8ab'


def old_source(path):
    return subprocess.check_output(['git', 'show', BASE + ':' + path], cwd=ROOT, text=True)


def functions(source):
    return {node.name: ast.get_source_segment(source, node) for node in ast.parse(source).body
            if isinstance(node, ast.FunctionDef)}


def fact_body(source):
    # Every new fact function has exactly one one-line documentation statement.
    node = ast.parse(source).body[0]
    assert isinstance(node.body[0], ast.Expr) and isinstance(node.body[0].value, ast.Constant)
    return '\n'.join(source.splitlines()[2:])


def fixture():
    sha = 'a' * 40
    repository = dict(id=1460486951, full_name='yom-chadash/gesher', fork=True,
                      source=dict(id=1426153186, full_name='gesher-runtime/gesher'))
    env = dict(GITHUB_ACTIONS='true', GITHUB_JOB='compile', GITHUB_SHA=sha,
               GITHUB_WORKFLOW_SHA=sha, GITHUB_REPOSITORY=repository['full_name'],
               GITHUB_REPOSITORY_ID=str(repository['id']), GITHUB_RUN_ID='45054400000',
               GITHUB_RUN_ATTEMPT='1', GITHUB_EVENT_NAME='workflow_dispatch',
               GITHUB_REF='refs/heads/diagnostic/tcp', GITHUB_WORKFLOW='ci',
               GITHUB_WORKFLOW_REF=repository['full_name'] + '/.github/workflows/ci.yml@refs/heads/diagnostic/tcp',
               GESHER_COHERENT_TCP_AUTHORIZED_SOURCE=sha,
               GESHER_COHERENT_TCP_POLICY_REF=repository['full_name'] + '/.github/workflows/ci.yml@' + sha,
               TCP_SUPPLIER_ROLE='selected-minimum')
    event = dict(repository=copy.deepcopy(repository))
    run = dict(id=45054400000, run_attempt=1, head_sha=sha, event='workflow_dispatch',
               repository=copy.deepcopy(repository), head_repository=copy.deepcopy(repository),
               name='ci', path='.github/workflows/ci.yml', head_branch='diagnostic/tcp')
    return env, event, repository, run


def descriptor():
    env, event, repository, run = fixture()
    return identity.validate(env, event, repository, run, env['GITHUB_SHA'], 'compile')


def projection():
    # Reuse the existing explicitly synthetic historical-log metadata projection.
    path = ROOT / 'tools/net-tcp-priority/test-selected-supplier.py'
    spec = importlib.util.spec_from_file_location('selected_projection', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    receipt = json.loads((ROOT / 'tests/net-tcp-priority/minimum-receipt-411350.json').read_text())
    return module.project(receipt)


def diagnostic_projection():
    canonical, _ = projection()
    value = descriptor()
    replacements = [('tcp-selected-supplier', fork.WORKSPACE),
                    ('build-minimum-supplier.sh', fork.FACTORY),
                    ('selected-minimum-factory.log', fork.FACTORY_LOG)]

    def rewrite(item):
        if isinstance(item, str):
            for before, after in replacements:
                item = item.replace(before, after)
            return item
        if isinstance(item, dict):
            return {rewrite(k): rewrite(v) for k, v in item.items()}
        if isinstance(item, list):
            return [rewrite(v) for v in item]
        return item

    supplier = rewrite(canonical)
    supplier.update(context=copy.deepcopy(value['context']), role=fork.ROLE,
                    evidence_schema=fork.SCHEMA, diagnostic=copy.deepcopy(value))
    authority = supplier['minimum_authority']
    authority.update({k: value['context'][k] for k in selected.AUTHORITY_CONTEXT})
    authority.update(role=fork.minimum.ROLE, evidence_schema=fork.minimum.SCHEMA,
                     diagnostic=copy.deepcopy(value))
    authority['original'].update(context={k: value['context'][k] for k in selected.AUTHORITY_CONTEXT},
        role=fork.minimum.PUBLISHER_ROLE, evidence_schema=fork.minimum.SCHEMA, diagnostic=copy.deepcopy(value))
    supplier['minimum_authority_sha256'] = selected.authority_digest(authority)
    return supplier, value


def manifest_projection():
    supplier, value = diagnostic_projection()
    ctx = value['context']
    manifest = dict(evidence_schema=fork.SCHEMA, diagnostic=value, supplier=supplier,
                    source=ctx['GITHUB_SHA'], repository=ctx['GITHUB_REPOSITORY'],
                    repository_id=ctx['GITHUB_REPOSITORY_ID'], run=ctx['GITHUB_RUN_ID'],
                    attempt=ctx['GITHUB_RUN_ATTEMPT'], producer_job='compile',
                    supplier_selection='selected-minimum', scope='noninline-only')
    return manifest, value


class ContextTest(unittest.TestCase):
    def test_all_roles_retain_one_actual_fork_identity(self):
        env, event, repo, run = fixture()
        wanted = descriptor()
        for job in identity.JOBS:
            received = identity.validate(dict(env, GITHUB_JOB=job), event, repo, run, env['GITHUB_SHA'], job)
            self.assertEqual(received, wanted)
        identity.check_descriptor(wanted)
        self.assertFalse(wanted['canonical_authority'])
        self.assertEqual(wanted['context']['GITHUB_REPOSITORY_ID'], '1460486951')

    def test_every_missing_or_wrong_environment_identity_rejected(self):
        env, event, repo, run = fixture()
        for key in env:
            for value in (None, '', 'wrong'):
                changed = dict(env)
                if value is None:
                    del changed[key]
                else:
                    changed[key] = value
                with self.subTest(key=key, value=value), self.assertRaises((RuntimeError, KeyError)):
                    identity.validate(changed, event, repo, run, env['GITHUB_SHA'], 'compile')

    def test_canonical_flag_event_ref_and_job_substitution_rejected(self):
        env, event, repo, run = fixture()
        changes = [('GITHUB_REPOSITORY', identity.CANONICAL),
                   ('GITHUB_REPOSITORY_ID', identity.CANONICAL_ID),
                   ('GITHUB_REPOSITORY', '../gesher'), ('GITHUB_REPOSITORY', 'a/b/c'),
                   ('GITHUB_REPOSITORY_ID', '01'), ('GITHUB_RUN_ID', '0'), ('GITHUB_RUN_ATTEMPT', '-1'),
                   ('GITHUB_EVENT_NAME', 'push'), ('GITHUB_EVENT_NAME', 'pull_request'),
                   ('GITHUB_EVENT_NAME', 'pull_request_target'), ('GITHUB_EVENT_NAME', 'merge_group'),
                   ('GITHUB_REF', 'refs/tags/test'), ('GITHUB_REF', 'refs/heads/'),
                   ('GITHUB_JOB', 'build'), ('TCP_SUPPLIER_ROLE', 'canonical')]
        for key, value in changes:
            with self.subTest(key=key), self.assertRaises((RuntimeError, KeyError)):
                identity.validate(dict(env, **{key: value}), event, repo, run, env['GITHUB_SHA'], 'compile')
        with self.assertRaises(RuntimeError):
            identity.validate(env, event, repo, run, 'f' * 40, 'compile')
        for authority in (True, 0, 1, 'false', None):
            with self.subTest(authority=authority), self.assertRaises(RuntimeError):
                identity.check_descriptor(descriptor() | {'canonical_authority': authority})

    def test_api_run_repository_and_upstream_mismatch_fail_closed(self):
        env, event, repo, run = fixture()
        for target in ('event', 'repo', 'run'):
            if target == 'run':
                mutations = [lambda x: x.update(id=7), lambda x: x.update(run_attempt=2),
                    lambda x: x.update(head_sha='f' * 40), lambda x: x.update(event='push'),
                    lambda x: x.update(path='.github/workflows/native2.yml'),
                    lambda x: x.update(name='native2'), lambda x: x.update(head_branch='main'),
                    lambda x: x['repository'].update(id=7), lambda x: x['head_repository'].update(id=7),
                    lambda x: x['head_repository'].update(full_name='someone/else')]
            else:
                mutations = [lambda x: x.update(id=7), lambda x: x.update(id='1460486951'),
                    lambda x: x.update(full_name='someone/else'), lambda x: x.update(fork=False)]
                if target == 'repo':
                    mutations += [lambda x: x['source'].update(id=7),
                                  lambda x: x['source'].update(full_name='someone/else')]
            for index, mutate in enumerate(mutations):
                e, p, r = copy.deepcopy((event, repo, run))
                mutate(e['repository'] if target == 'event' else p if target == 'repo' else r)
                with self.subTest(target=target, index=index), self.assertRaises(RuntimeError):
                    identity.validate(env, e, p, r, env['GITHUB_SHA'], 'compile')

    def test_dynamic_fork_owner_and_live_api_bridge_without_identity_rewrite(self):
        env, event, repo, run = fixture()
        for record in (event['repository'], repo, run['repository'], run['head_repository']):
            record.update(id=17, full_name='another-owner/renamed-repository')
        env.update(GITHUB_REPOSITORY=repo['full_name'], GITHUB_REPOSITORY_ID='17')
        env['GITHUB_WORKFLOW_REF'] = repo['full_name'] + '/.github/workflows/ci.yml@' + env['GITHUB_REF']
        env['GESHER_COHERENT_TCP_POLICY_REF'] = repo['full_name'] + '/.github/workflows/ci.yml@' + env['GITHUB_SHA']
        with tempfile.TemporaryDirectory() as temp:
            event_path = Path(temp) / 'event.json'
            event_path.write_text(json.dumps(event))
            env['GITHUB_EVENT_PATH'] = str(event_path)
            with patch.dict(os.environ, env, clear=True), patch.object(identity, 'checkout', return_value=env['GITHUB_SHA']), \
                    patch.object(identity, 'get_json', side_effect=[repo, run]) as api:
                before = dict(os.environ)
                actual = identity.current('compile')
                self.assertEqual(dict(os.environ), before)
                self.assertEqual([args[0][0] for args in api.call_args_list],
                    ['repos/' + repo['full_name'], 'repos/' + repo['full_name'] + '/actions/runs/' + env['GITHUB_RUN_ID']])
                identity.assert_current(actual, 'compile')
                os.environ['GITHUB_RUN_ATTEMPT'] = '2'
                with self.assertRaises(RuntimeError):
                    identity.assert_current(actual, 'compile')


class SupplierTest(unittest.TestCase):
    def test_separate_types_reject_both_cross_scope_directions(self):
        canonical, context = projection()
        diagnostic, value = diagnostic_projection()
        selected.check_selected(canonical, context)
        fork.check_supplier(diagnostic, value)
        with self.assertRaises((AssertionError, KeyError, RuntimeError)):
            selected.check_selected(diagnostic, value['context'])
        with self.assertRaises((AssertionError, KeyError, RuntimeError)):
            fork.check_supplier(canonical, value)
        with self.assertRaises((AssertionError, KeyError, RuntimeError)):
            selected.check_selected(diagnostic, context)

    def test_each_diagnostic_envelope_and_baseline_remains_typed(self):
        good, value = diagnostic_projection()
        for path in [(), ('minimum_authority',), ('minimum_authority', 'original')]:
            for key, replacement in [('role', selected.ROLE), ('evidence_schema', 1),
                                      ('diagnostic', descriptor() | {'canonical_authority': True}),
                                      ('diagnostic', descriptor() | {'canonical_authority': 0})]:
                bad = copy.deepcopy(good)
                record = bad
                for part in path:
                    record = record[part]
                record[key] = replacement
                bad['minimum_authority_sha256'] = selected.authority_digest(bad['minimum_authority'])
                with self.subTest(path=path, key=key), self.assertRaises((AssertionError, RuntimeError)):
                    fork.check_supplier(bad, value)

    def test_resealed_diagnostic_does_not_bypass_actual_fact_predicates(self):
        good, value = diagnostic_projection()
        changes = [lambda x: x['compiler_archive'].update(bytes=1),
                   lambda x: x['minimum_authority'].update(private_record_layout='[sizeof=496, align=16]'),
                   lambda x: x['minimum_authority']['original_profile']['supplier'].update(_WIN32_WINNT=0xa00),
                   lambda x: x['minimum_authority']['pin'].update(role='foreign'),
                   lambda x: x['minimum_authority']['source_files'].clear(),
                   lambda x: x['minimum_authority']['files'].update({'lib/libpthread.a': '0' * 64}),
                   lambda x: x['minimum_authority']['dll_symbol_objects'].pop('pthread_join'),
                   lambda x: x['minimum_authority'].update(dll_compile_commands=[]),
                   lambda x: x['minimum_authority']['build_logs'].pop('supplier-profile.obj'),
                   lambda x: x.update(commands=[]), lambda x: x.update(compiler_root='/other/path'),
                   lambda x: x['context'].update(GITHUB_RUN_ATTEMPT='2')]
        for index, mutate in enumerate(changes):
            bad = copy.deepcopy(good)
            mutate(bad)
            bad['minimum_authority_sha256'] = selected.authority_digest(bad['minimum_authority'])
            with self.subTest(index=index), self.assertRaises((AssertionError, KeyError, RuntimeError)):
                fork.check_supplier(bad, value)

    def test_tcp_manifest_context_scope_and_members_are_not_selected_by_flag(self):
        good, value = manifest_projection()
        fork.check_manifest(good, value)
        for key in ('evidence_schema', 'source', 'repository', 'repository_id', 'run', 'attempt',
                    'producer_job', 'supplier_selection', 'scope'):
            bad = copy.deepcopy(good)
            bad[key] = 'wrong'
            with self.subTest(key=key), self.assertRaises((AssertionError, KeyError, RuntimeError)):
                fork.check_manifest(bad, value)
        lines = ['FORK_DIAGNOSTIC_' + x for x in minimum.receipt_lines(json.dumps(good['supplier']).encode())]
        with self.assertRaises(AssertionError):
            minimum.reconstruct_receipt(lines)

    def test_actual_whole_artifact_verifier_rejects_member_role_source_and_byte_changes(self):
        good, value = manifest_projection()
        sources = {'SYNTHETIC_METADATA_SOURCE_ONLY': 'a' * 64}
        good['sources'] = dict(sources)
        with tempfile.TemporaryDirectory(prefix='tcp-nonexecutable-metadata-only-') as temp:
            root = Path(temp)
            for name in fork.ROLES:
                (root / name).write_bytes(b'SYNTHETIC METADATA ONLY - NOT AN EXECUTABLE: ' + name.encode())
            good['outputs'] = {name: dict(role=role, sha256=hashlib.sha256((root / name).read_bytes()).hexdigest())
                               for name, role in fork.ROLES.items()}
            manifest_path = root / 'provenance.json'
            manifest_path.write_text(json.dumps(good))
            self.assertEqual(fork.check_artifact(root, value, sources), good)
            mutations = [lambda x: x['sources'].clear(), lambda x: x['outputs'].pop('gesher_arm64.dll'),
                         lambda x: x['outputs']['gesher-exec.exe'].update(role='foreign'),
                         lambda x: x['outputs']['tcp-priority-x86_64'].update(sha256='f' * 64)]
            for index, mutate in enumerate(mutations):
                bad = copy.deepcopy(good)
                mutate(bad)
                manifest_path.write_text(json.dumps(bad))
                with self.subTest(index=index), self.assertRaises((AssertionError, KeyError)):
                    fork.check_artifact(root, value, sources)
            manifest_path.write_text(json.dumps(good))
            extra = root / 'extra'
            extra.write_text('SYNTHETIC METADATA ONLY')
            with self.assertRaises(AssertionError):
                fork.check_artifact(root, value, sources)
            extra.unlink()
            target = root / 'gesher_arm64.dll'
            original = target.read_bytes()
            target.unlink()
            with self.assertRaises(AssertionError):
                fork.check_artifact(root, value, sources)
            target.symlink_to(root / 'gesher-exec.exe')
            with self.assertRaises(AssertionError):
                fork.check_artifact(root, value, sources)
            target.unlink()
            target.write_bytes(original + b'changed')
            with self.assertRaises(AssertionError):
                fork.check_artifact(root, value, sources)

    def test_canonical_validator_matches_immutable_baseline_on_leaf_mutations(self):
        baseline = types.ModuleType('immutable_selected_supplier')
        baseline.__file__ = str(ROOT / 'tools/net-tcp-priority/selected_supplier.py')
        exec(compile(old_source('tools/net-tcp-priority/selected_supplier.py'), baseline.__file__, 'exec'), baseline.__dict__)
        good, context = projection()

        def leaves(value, path=()):
            if isinstance(value, dict):
                for key, item in value.items():
                    yield from leaves(item, path + (key,))
            elif isinstance(value, list):
                for key, item in enumerate(value):
                    yield from leaves(item, path + (key,))
            else:
                yield path

        def accepted(checker, value):
            try:
                checker(value, context)
                return True
            except (AssertionError, KeyError, TypeError, IndexError, ValueError, AttributeError):
                return False

        paths = list(leaves(good))
        for path in paths:
            bad = copy.deepcopy(good)
            parent = bad
            for key in path[:-1]:
                parent = parent[key]
            parent[path[-1]] = 'SYNTHETIC_WRONG_METADATA'
            if path != ('minimum_authority_sha256',):
                bad['minimum_authority_sha256'] = selected.authority_digest(bad['minimum_authority'])
            with self.subTest(path=path):
                self.assertEqual(accepted(selected.check_selected, bad), accepted(baseline.check_selected, bad))
        print('CANONICAL_SUPPLIER_BASELINE_PARITY leaf_mutants=' + str(len(paths)), flush=True)


class CorrelationTest(unittest.TestCase):
    def test_real_diagnostic_correlation_checks_types_before_any_comparison_claim(self):
        import coherent_supplier_correlation as correlation
        import prepare_pthread_cancel as publisher
        manifest, value = manifest_projection()
        sources = {'SYNTHETIC_METADATA_SOURCE_ONLY': 'b' * 64}
        manifest['sources'] = dict(sources)
        production = dict(role=publisher.PIN['role'], recipe=publisher.recipe(),
            patch_sha256=publisher.PIN['patch_sha256'], thread_source_sha256=publisher.PIN['thread_output_sha256'],
            context=dict(commit=value['context']['GITHUB_SHA'], tracked_changes=False,
                         actions=dict(value['context'], GITHUB_JOB='build')))
        host_record = dict(compiler=dict(production_pthread=production))
        env, event, repo, run = fixture()
        env['GITHUB_JOB'] = 'supplier-correlation'
        with tempfile.TemporaryDirectory(prefix='tcp-correlation-metadata-only-') as temp:
            root = Path(temp)
            tcp = root / 'ci-suppliers/tcp_selected'
            host = root / 'ci-suppliers/native3_host'
            tcp.mkdir(parents=True)
            host.mkdir(parents=True)
            for name in fork.ROLES:
                (tcp / name).write_bytes(b'SYNTHETIC METADATA ONLY - NOT AN EXECUTABLE: ' + name.encode())
            manifest['outputs'] = {name: dict(role=role, sha256=hashlib.sha256((tcp / name).read_bytes()).hexdigest())
                                   for name, role in fork.ROLES.items()}
            (tcp / 'provenance.json').write_text(json.dumps(manifest))
            event_path = root / 'event.json'
            event_path.write_text(json.dumps(event))
            env['GITHUB_EVENT_PATH'] = str(event_path)

            def execute(record):
                # Existing downloaded-host authentication and git/API reads are
                # explicit test boundaries. The new typed correlation and whole
                # TCP member verifier execute unchanged, with nonexecutable data.
                with patch.dict(os.environ, env, clear=True), \
                        patch.object(identity, 'checkout', return_value=env['GITHUB_SHA']), \
                        patch.object(identity, 'get_json', side_effect=[repo, run]), \
                        patch.object(correlation.host_roles, 'context', return_value={'synthetic_host_boundary': True}), \
                        patch.object(correlation.union_payload, 'source_inventory', return_value=sources), \
                        patch.object(correlation.union_payload, 'check_manifest', return_value=record), \
                        contextlib.redirect_stdout(io.StringIO()) as output:
                    previous = Path.cwd()
                    try:
                        os.chdir(root)
                        correlation.diagnostic_main()
                    finally:
                        os.chdir(previous)
                    return output.getvalue()

            result = execute(host_record)
            self.assertIn('FORK_TCP_DIAGNOSTIC_CORRELATION ', result)
            self.assertIn('not-applicable-publisher-host', result)
            changes = [lambda x: x['compiler'].update(selected_toolchain={}),
                lambda x: x['compiler']['production_pthread'].update(role='foreign'),
                lambda x: x['compiler']['production_pthread'].update(recipe={}),
                lambda x: x['compiler']['production_pthread'].update(patch_sha256='0' * 64),
                lambda x: x['compiler']['production_pthread'].update(thread_source_sha256='0' * 64),
                lambda x: x['compiler']['production_pthread']['context'].update(commit='f' * 40),
                lambda x: x['compiler']['production_pthread']['context'].update(tracked_changes=True),
                lambda x: x['compiler']['production_pthread']['context']['actions'].update(GITHUB_JOB='compile'),
                lambda x: x['compiler']['production_pthread']['context']['actions'].update(GITHUB_REPOSITORY_ID='7'),
                lambda x: x['compiler']['production_pthread']['context']['actions'].update(GITHUB_RUN_ATTEMPT='2')]
            for index, mutate in enumerate(changes):
                bad = copy.deepcopy(host_record)
                mutate(bad)
                with self.subTest(index=index), self.assertRaises((AssertionError, KeyError, RuntimeError)):
                    execute(bad)
            (host / correlation.host_roles.receipt_name('native3-common-host-fixture')).write_text('SYNTHETIC')
            with self.assertRaises(AssertionError):
                execute(host_record)


class SourcePreservationTest(unittest.TestCase):
    def test_canonical_entries_pins_and_shell_are_byte_exact(self):
        for path in ('tools/toolchain/coherent_tcp_context.py',
                     'tools/host-pthread-authority/build-minimum-supplier.sh',
                     'tools/host-pthread-authority/minimum-inputs.json',
                     'tools/host-pthread-authority/minimum-detached-retirement.patch',
                     'tools/host-pthread-authority/minimum-noattach-join.patch'):
            self.assertEqual((ROOT / path).read_text(), old_source(path), path)
        path = 'tools/host-pthread-authority/minimum_receipt.py'
        old, new = functions(old_source(path)), functions((ROOT / path).read_text())
        for name in ('context', 'receipt_lines', 'reconstruct_receipt', 'exports', 'imports',
                     'validate_layout', 'actual_macros', 'map_symbols', 'selftest'):
            self.assertEqual(old[name], new[name], name)
        path = 'tools/net-tcp-priority/selected_supplier.py'
        old, new = functions(old_source(path)), functions((ROOT / path).read_text())
        for name in ('main', 'authority_digest', 'digest', 'normalized'):
            self.assertEqual(old[name], new[name], name)

    def test_actual_minimum_source_compiler_sdk_and_abi_facts_are_exact(self):
        path = 'tools/host-pthread-authority/minimum_receipt.py'
        old, new = functions(old_source(path)), functions((ROOT / path).read_text())
        self.assertEqual('\n'.join(old['inputs'].splitlines()[2:]), fact_body(new['input_facts']))
        self.assertEqual('\n'.join(old['profile'].splitlines()[3:]), fact_body(new['profile_facts']))
        original = '\n'.join(old['original'].splitlines()[3:])
        original = original.replace("    record = dict(context=ctx, role='publisher-reference-unmodified-v1',\n                  compiler_archive_sha256=", '    record = dict(compiler_archive_sha256=', 1)
        original = original.replace('    output.write_text(json.dumps(record, sort_keys=True, indent=2))', '    return record', 1)
        self.assertEqual(original, fact_body(new['original_facts']))
        supplier = old['supplier'][old['supplier'].index("    build = source / 'build'\n"):]
        supplier = supplier[:supplier.index("    payload = json.dumps(record,")]
        supplier = supplier.replace('    record = dict(ctx, role=ROLE, pin=PIN, files=files,',
                                    '    record = dict(pin=PIN, files=files,', 1) + '    return record'
        self.assertEqual(supplier, fact_body(new['supplier_facts']))

    def test_selected_factual_validator_is_exact_after_fixed_role_parameters(self):
        path = 'tools/net-tcp-priority/selected_supplier.py'
        old, new = functions(old_source(path)), functions((ROOT / path).read_text())
        original = '\n'.join(old['check_selected'].splitlines()[2:])
        original = original.replace("    assert context['GITHUB_REPOSITORY']=='gesher-runtime/gesher' and context['GITHUB_REPOSITORY_ID']=='1426153186'\n", '', 1)
        replacements = {
            "supplier['role']==ROLE and minimum.ROLE==ROLE": "supplier['role']==receipt_role and minimum.ROLE==ROLE",
            "suffix='/tcp-selected-supplier/'+NAME": "suffix='/'+workspace_name+'/'+NAME",
            "authority['role']==ROLE and authority['pin']==minimum.PIN": "authority['role']==authority_role and authority['pin']==minimum.PIN",
            "original['role']=='publisher-reference-unmodified-v1'": "original['role']==publisher_role",
            "assert 'selected-minimum-factory.log' in supplier['logs']": "assert factory_log in supplier['logs']",
            "workspace.rsplit('/',1)[0]+'/tools/host-pthread-authority/build-minimum-supplier.sh'": "workspace.rsplit('/',1)[0]+'/tools/host-pthread-authority/'+factory_name",
            "row['log']=='selected-minimum-factory.log'": "row['log']==factory_log",
        }
        for before, after in replacements.items():
            self.assertEqual(original.count(before), 1)
            original = original.replace(before, after, 1)
        # The fact helper signature spans two lines; take its body from the
        # first actual assertion rather than assuming a one-line signature.
        actual = new['_check_selected_facts']
        actual = actual[actual.index('    assert set(context)==set(CONTEXT)'):]
        self.assertEqual(original, actual)

    def test_diagnostic_factory_executes_the_identical_mechanical_recipe(self):
        canonical = (ROOT / 'tools/host-pthread-authority/build-minimum-supplier.sh').read_text()
        diagnostic = (ROOT / 'tools/host-pthread-authority/build-minimum-diagnostic.sh').read_text()
        canonical = canonical[canonical.index('llvm=$(realpath '):]
        diagnostic = diagnostic[diagnostic.index('llvm=$(realpath '):]
        diagnostic = diagnostic.replace('tools/host-pthread-authority/minimum_diagnostic.py',
                                        'tools/host-pthread-authority/minimum_receipt.py')
        self.assertEqual(canonical, diagnostic)

    def test_complete_member_roles_equal_the_original_producer(self):
        tree = ast.parse(old_source('tools/net-tcp-priority/manifest.py'))
        roles = {}
        for node in tree.body:
            if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'roles' for t in node.targets):
                roles = ast.literal_eval(node.value)
            if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
                call = node.value
                if isinstance(call.func, ast.Attribute) and isinstance(call.func.value, ast.Name) and \
                        call.func.value.id == 'roles' and call.func.attr == 'update':
                    roles.update(ast.literal_eval(call.args[0]))
        self.assertEqual(len(roles), 13)
        self.assertEqual(fork.ROLES, roles)

    def test_original_windows_runtime_and_correlation_remain_identical(self):
        path = 'tools/net-tcp-priority/run-component.ps1'
        old, new = old_source(path), (ROOT / path).read_text()
        marker = 'function RequireDiagnosticRetirement'
        self.assertEqual(old[old.index(marker):], new[new.index(marker):])
        path = 'tools/toolchain/coherent_supplier_correlation.py'
        old, new = functions(old_source(path)), functions((ROOT / path).read_text())
        self.assertEqual(old['main'], new['main'])
        self.assertEqual(old['compare'], new['compare'])


if __name__ == '__main__':
    unittest.main()
