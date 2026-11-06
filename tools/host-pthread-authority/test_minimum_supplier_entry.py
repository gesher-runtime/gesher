"""Exercise the real shell authority gate without compiler, downloads or patches."""
import ast
import os
from pathlib import Path
import shutil
import subprocess
import sys
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


class PythonEntryTest(unittest.TestCase):
    def run_gate(self, name, job, reject=False, workflow='ci'):
        tree = ast.parse((ROOT / name).read_text())
        function = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                        and n.name == ('context' if name.endswith('minimum_receipt.py') else 'main'))
        body = function.body
        if name.endswith('prepare-supplier.py'):
            # Stop before the first actual git/compiler/source workspace access.
            end = next(i for i, n in enumerate(body) if isinstance(n, ast.Assert)
                       and 'output(' in ast.unparse(n.test))
            body = body[:end]
        elif name.endswith('selected_supplier.py'):
            begin = next(i for i, n in enumerate(body) if isinstance(n, ast.Assign)
                         and any(isinstance(t, ast.Name) and t.id == 'context' for t in n.targets))
            end = next(i for i, n in enumerate(body) if isinstance(n, ast.Assign)
                       and any(isinstance(t, ast.Name) and t.id == 'manifest' for t in n.targets))
            body = body[begin:end]
        else:
            # Keep the real function (including its return), but stub only git.
            body = [function]
        code = compile(ast.Module(body=body, type_ignores=[]), str(ROOT / name), 'exec')
        keys = ('GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_REPOSITORY', 'GITHUB_REPOSITORY_ID')
        env = dict(zip(keys, ('1' * 40, '123', '1', 'gesher-runtime/gesher', '1426153186')),
                   GITHUB_ACTIONS='true', GITHUB_JOB=job, GITHUB_REF='refs/pull/5/merge',
                   GITHUB_WORKFLOW=workflow, TCP_SUPPLIER_ROLE='selected-minimum')
        calls = []

        def validate(received, received_job):
            self.assertEqual(dict(received), env)
            self.assertEqual(received_job, job)
            calls.append(received_job)
            if reject:
                raise AssertionError('rejected shared CI authority')

        validator = types.ModuleType('coherent_tcp_context'); validator.context = validate
        namespace = dict(os=os, sys=sys, ROOT=ROOT, CONTEXT=keys,
                         command=lambda *args: env['GITHUB_SHA'] + '\n',
                         subprocess=types.SimpleNamespace(check_call=lambda *args: 0))
        with patch.dict(os.environ, env, clear=True), patch.dict(sys.modules, coherent_tcp_context=validator):
            exec(code, namespace)
            if name.endswith('minimum_receipt.py'):
                namespace['context']()
        self.assertEqual(calls, [job], 'shared CI entry did not delegate its authority')

    def test_all_shared_python_entries_delegate_and_propagate_denial(self):
        entries = (('tools/net-tcp-priority/prepare-supplier.py', 'compile'),
                   ('tools/net-tcp-priority/selected_supplier.py', 'windows'),
                   ('tools/host-pthread-authority/minimum_receipt.py', 'compile'))
        for path, job in entries:
            with self.subTest(path=path):
                self.run_gate(path, job)
                with self.assertRaisesRegex(AssertionError, 'rejected shared CI authority'):
                    self.run_gate(path, job, reject=True)
                with self.assertRaises(AssertionError):
                    self.run_gate(path, job, workflow='net-tcp-selected-component')


@unittest.skipUnless(shutil.which('bash'), 'bash is required')
class SupplierEntryTest(unittest.TestCase):
    def run_gate(self, workflow='ci', ref='refs/pull/5/merge', validator_exit=0):
        source = Path(__file__).with_name('build-minimum-supplier.sh').read_text()
        gate = source.split('llvm=$(realpath ', 1)[0]
        # Execute the actual entry gate; stub only its already separately tested
        # Python validator, and stop before any compiler/source filesystem work.
        stub = '''python3() {
  test "$*" = tools/toolchain/coherent_tcp_context.py || return 97
  echo VALIDATED_CI_CONTEXT
  return "$VALIDATOR_EXIT"
}
'''
        env = dict(os.environ, GITHUB_ACTIONS='true', GITHUB_JOB='compile',
                   GITHUB_WORKFLOW=workflow, GITHUB_REF=ref, TCP_SUPPLIER_ROLE='selected-minimum',
                   VALIDATOR_EXIT=str(validator_exit))
        return subprocess.run([shutil.which('bash'), '-c', stub + gate + '\necho ENTRY_ACCEPTED\n'],
                              env=env, capture_output=True, text=True, timeout=5)

    def test_pr_compiler_uses_the_same_guard_as_other_ci_roles(self):
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('VALIDATED_CI_CONTEXT', result.stdout)
        self.assertIn('ENTRY_ACCEPTED', result.stdout)

    def test_rejected_ci_context_cannot_enter_factory(self):
        result = self.run_gate(validator_exit=1)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('ENTRY_ACCEPTED', result.stdout)

    def test_dedicated_component_keeps_its_exact_branch(self):
        accepted = self.run_gate(workflow='net-tcp-selected-component', ref='refs/heads/wip/tcp-minimum-selection')
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertNotIn('VALIDATED_CI_CONTEXT', accepted.stdout)
        rejected = self.run_gate(workflow='net-tcp-selected-component')
        self.assertNotEqual(rejected.returncode, 0)
        self.assertNotIn('ENTRY_ACCEPTED', rejected.stdout)


if __name__ == '__main__':
    unittest.main()
