"""Failure accounting, frozen identities, bounded subprocess and statistical tests."""
from __future__ import annotations
from copy import deepcopy
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

import benchmark
import contract
import failure_diagnostics
from failure_diagnostics import runner_failure_diagnostics

HERE = Path(__file__).resolve().parent


def result():
    return {'schema_version': 1, 'invocation_id': 'a' * 32, 'variant': 'candidate', 'workload_id': 'test',
            'completed': True, 'success': True, 'coverage_complete': True, 'problems': [],
            'workload_elapsed_ms': 20, 'events': [{'name': 'start', 'at_ms': 0}, {'name': 'done', 'at_ms': 20}],
            'metrics': {'workload_ms': 20, 'gpu_memory_bytes': None}}


def validate(value):
    return contract.validate_result(value, invocation='a' * 32, variant='candidate', workload='test',
                                    measured_ms=30, required_metrics=['workload_ms'])


class EvidenceTests(unittest.TestCase):
    def test_success_retains_unavailable_not_zero(self):
        self.assertIsNone(validate(result())['gpu_memory_bytes'])

    def test_stale_failure_incomplete_and_coverage_fail(self):
        for key, value in [('invocation_id', 'b' * 32), ('variant', 'baseline'), ('workload_id', 'other'),
                           ('success', False), ('completed', False), ('coverage_complete', False),
                           ('problems', ['frame missing']), ('workload_elapsed_ms', 100)]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                changed = result(); changed[key] = value; validate(changed)

    def test_metrics_and_event_numbers_are_strict(self):
        for value in [-1, math.nan, math.inf, -math.inf, True, '0', 10**1000]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                changed = result(); changed['metrics']['workload_ms'] = value; validate(changed)
        for events in [[{'name': 'done', 'at_ms': 21}],
                       [{'name': 'a', 'at_ms': 2}, {'name': 'b', 'at_ms': 1}],
                       [{'name': 'a', 'at_ms': 0}, {'name': 'a', 'at_ms': 0}]]:
            with self.subTest(events=events), self.assertRaises(ValueError):
                changed = result(); changed['events'] = events; validate(changed)

    def test_missing_required_metric_fails(self):
        changed = result(); changed['metrics']['workload_ms'] = None
        with self.assertRaisesRegex(ValueError, 'unavailable'): validate(changed)

    def test_duplicate_json_and_nonfinite_constants_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'evidence.json'
            for text in ['{"completed":false,"completed":true}', '{"value":NaN}', '{"value":Infinity}']:
                path.write_text(text)
                with self.assertRaises(ValueError): contract.read_json(path)

    def test_frozen_artifacts_detect_same_size_changes_and_new_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); path = root / 'image.art'; path.write_bytes(b'abcd')
            frozen = {'runtime': contract.artifact(root)}
            contract.verify_artifacts(frozen)
            path.write_bytes(b'abce')
            with self.assertRaisesRegex(ValueError, 'changed'): contract.verify_artifacts(frozen)
            path.write_bytes(b'abcd'); (root / 'new.dll').write_bytes(b'new')
            with self.assertRaisesRegex(ValueError, 'changed'): contract.verify_artifacts(frozen)

    def test_e2e_binds_hashes_boundaries_and_real_process_roles(self):
        roles = ['application', 'binderd', 'servicemanager', 'system_server']
        request = {'workload': {'package': 'test.package', 'scenario': 'warm-device', 'abi': 'x86_64'},
                   'artifact_identities': {name: {'sha256': name + '-hash'} for name in ['manifest', 'apk', 'runtime']}}
        payload = result()
        names = ['command_start', 'first_frame_observed', 'first_input', 'first_input_change_observed', 'completed']
        payload['events'] = [{'name': name, 'at_ms': index * 5} for index, name in enumerate(names)]
        payload['metrics'].update(command_to_first_frame_observed_ms=5, first_input_to_change_observed_ms=5)
        payload['participants'] = [{'pid': i + 1, 'created_100ns': i + 100, 'role': role} for i, role in enumerate(roles)]
        payload['evidence'] = {**request['workload'], **{name + '_sha256': name + '-hash' for name in ['manifest', 'apk', 'runtime']},
            **{name: True for name in ['first_frame_verified', 'nonblank_verified', 'input_change_verified',
                                      'lifecycle_verified', 'fresh_profile', 'owned_device_verified']}}
        registered = deepcopy(payload['participants'])
        validate(payload); benchmark.validate_e2e(payload, request, registered_participants=registered)
        for change in ['hash', 'flag', 'event', 'metric', 'participants', 'unregistered']:
            changed = deepcopy(payload)
            if change == 'hash': changed['evidence']['apk_sha256'] = 'other-bytes'
            if change == 'flag': changed['evidence']['nonblank_verified'] = False
            if change == 'event': changed['events'].pop(1)
            if change == 'metric': changed['metrics']['first_input_to_change_observed_ms'] = 0
            if change == 'participants': changed['participants'] = []
            if change == 'unregistered': changed['participants'][0]['pid'] = 500
            with self.subTest(change=change), self.assertRaises(ValueError):
                validate(changed); benchmark.validate_e2e(changed, request, registered_participants=registered)


class AccountingTests(unittest.TestCase):
    def process(self, pid, commit, ws=500, private=100):
        return {'pid': pid, 'created_100ns': pid + 1000, 'private_commit_bytes': commit,
                'working_set_bytes': ws, 'private_resident_bytes': private, 'handles': 5, 'threads': 1}

    def test_shared_pages_not_claimed_as_unique_ram(self):
        total = contract.aggregate_snapshot([self.process(1, 10), self.process(2, 20)])
        self.assertEqual(total['private_commit_bytes'], 30)
        self.assertEqual(total['private_resident_bytes'], 200)
        self.assertEqual(total['working_set_sum_including_shared_duplicates_bytes'], 1000)
        self.assertIsNone(total['unique_shared_resident_bytes'])
        self.assertIsNone(total['unique_total_resident_bytes'])

    def test_missing_counts_propagate_and_duplicate_pid_is_rejected(self):
        process = self.process(1, 20); process['threads'] = None
        self.assertIsNone(contract.aggregate_snapshot([process])['threads'])
        duplicate = deepcopy(process); duplicate['created_100ns'] += 1
        with self.assertRaises(ValueError): contract.aggregate_snapshot([process, duplicate])

    def test_peak_is_concurrent_not_sum_of_process_peaks(self):
        samples = []
        for i, values in enumerate([(100, 10), (10, 200)]):
            samples.append({'job': {'user_cpu_ms': 5 + i, 'kernel_cpu_ms': 3 + i,
                                    'read_bytes': 20 + i, 'write_bytes': 30 + i,
                                    'other_bytes': 40 + i, 'page_faults': 50 + i},
                            'live_snapshot_complete': True,
                            'totals': contract.aggregate_snapshot([self.process(1, values[0]), self.process(2, values[1])])})
        total = benchmark.resource_summary(samples)
        self.assertEqual(total['private_commit_sampled_peak_bytes'], 210)
        self.assertNotEqual(total['private_commit_sampled_peak_bytes'], 300)
        self.assertEqual(total['cpu_ms'], 2)
        self.assertEqual(total['read_bytes'], 1)
        self.assertIsNone(total['context_switches'])
        samples[1]['live_snapshot_complete'] = False
        self.assertEqual(benchmark.resource_summary(samples)['private_commit_sampled_peak_bytes'], 110)

    def test_preparation_is_excluded_and_observer_subtraction_retains_bounds(self):
        fields = ['user_cpu_ms', 'kernel_cpu_ms', 'read_bytes', 'write_bytes', 'other_bytes', 'page_faults']
        samples = []
        for inclusive, before, after in [(100, 40, 42), (140, 70, 73)]:
            samples.append({'job': dict.fromkeys(fields, inclusive), 'observer_before': dict.fromkeys(fields, before),
                            'observer_after': dict.fromkeys(fields, after), 'exclude_root_observer': True,
                            'live_snapshot_complete': True, 'totals': contract.aggregate_snapshot([self.process(1, 10)])})
        metrics = benchmark.resource_summary(samples)
        # Per counter: job delta 40; observer delta [28,33]; application delta [7,12].
        self.assertEqual(metrics['cpu_ms'], 24)
        self.assertEqual(metrics['cpu_ms_lower_bound'], 14)
        self.assertEqual(metrics['observer_subtraction_cpu_uncertainty_ms'], 10)
        self.assertEqual(metrics['read_bytes'], 12)
        samples[1]['observer_after'] = None
        with self.assertRaisesRegex(ValueError, 'Missing E2E observer'): benchmark.resource_summary(samples)

    def test_unavailable_or_decreasing_counters_cannot_pass(self):
        self.assertIsNone(benchmark.resource_summary([])['cpu_ms'])
        fields = ['user_cpu_ms', 'kernel_cpu_ms', 'read_bytes', 'write_bytes', 'other_bytes', 'page_faults']
        samples = [{'job': dict.fromkeys(fields, value)} for value in [2, 1]]
        with self.assertRaisesRegex(ValueError, 'decreased'): benchmark.resource_summary(samples)


class RegistrationTests(unittest.TestCase):
    def test_live_registration_is_ordered_authenticated_and_not_sampling(self):
        process = Mock()
        message = {'invocation_id': 'a' * 32, 'sequence': 1,
                   'participants': [{'pid': 10, 'created_100ns': 20, 'role': 'application'}]}
        with patch.object(benchmark.os, 'name', 'nt'):
            accepted = benchmark.register_participants(message, invocation='a' * 32, previous=None,
                                                       phase='start', process=process)
            self.assertIs(accepted, message)
            self.assertIs(benchmark.register_participants(deepcopy(message), invocation='a' * 32,
                previous=accepted, phase='stop', process=process), accepted)
        process.verify_participants.assert_called_once_with(message['participants'])
        process.sample.assert_not_called()

    def test_missing_stale_replaced_or_unowned_registration_cannot_acknowledge(self):
        process = Mock()
        original = {'invocation_id': 'a' * 32, 'sequence': 1, 'participants': [{'pid': 10, 'created_100ns': 20}]}
        for change in ['stale', 'sequence_gap', 'empty', 'wrong_phase', 'changed_replay', 'outside_job']:
            message = deepcopy(original); previous = None; phase = 'start'
            process.reset_mock(side_effect=True)
            if change == 'stale': message['invocation_id'] = 'b' * 32
            if change == 'sequence_gap': message['sequence'] = 2
            if change == 'empty': message['participants'] = []
            if change == 'wrong_phase': phase = None
            if change == 'changed_replay':
                previous = original; message['participants'][0]['created_100ns'] = 21
            if change == 'outside_job': process.verify_participants.side_effect = ValueError('outside job')
            with self.subTest(change=change), patch.object(benchmark.os, 'name', 'nt'), self.assertRaises(ValueError):
                benchmark.register_participants(message, invocation='a' * 32, previous=previous,
                                                phase=phase, process=process)


class StatisticsTests(unittest.TestCase):
    def rows(self, candidate=100, count=100):
        rows = contract.schedule(['baseline', 'candidate'], count, 0)
        for row in rows:
            row.update(status='passed', metrics={'latency_ms': 100 if row['variant'] == 'baseline' else candidate})
        return rows

    def test_schedule_alternates_and_keeps_warmups_distinct(self):
        rows = contract.schedule(['A', 'B'], 3, 1)
        self.assertEqual([r['variant'] for r in rows], ['A', 'B', 'A', 'B', 'B', 'A', 'A', 'B'])
        self.assertEqual([r['phase'] for r in rows[:2]], ['warmup', 'warmup'])
        three = contract.schedule(['A', 'B', 'C'], 6, 0)
        self.assertEqual([sum(three[i]['variant'] == name for i in range(0, 18, 3)) for name in ['A', 'B', 'C']], [2, 2, 2])

    def test_equal_and_regression_and_small_sample(self):
        self.assertTrue(contract.summarize(self.rows(), ['baseline', 'candidate'], ['latency_ms'])['gate_passed'])
        report = contract.summarize(self.rows(120), ['baseline', 'candidate'], ['latency_ms'])
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(report['comparisons'][0]['status'], 'regression')
        report = contract.summarize(self.rows(count=10), ['baseline', 'candidate'], ['latency_ms'])
        self.assertFalse(report['gate_passed']); self.assertEqual(report['status'], 'insufficient_data')

    def test_failure_is_not_a_fast_sample_or_removed_denominator(self):
        rows = self.rows()
        rows[1].update(status='timed_out', metrics={'latency_ms': 0})
        report = contract.summarize(rows, ['baseline', 'candidate'], ['latency_ms'])
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(report['variants']['candidate']['planned'], 100)
        self.assertEqual(report['variants']['candidate']['successful'], 99)
        self.assertEqual(report['variants']['candidate']['metrics']['latency_ms']['p50'], 100)

    def test_failed_warmup_cannot_hide_behind_successful_measured_runs(self):
        rows = self.rows(); rows.append({'phase': 'warmup', 'variant': 'candidate', 'status': 'failed'})
        self.assertEqual(contract.summarize(rows, ['baseline', 'candidate'], ['latency_ms'])['status'], 'failed')


class DiagnosticsTests(unittest.TestCase):
    def test_windows_root_normalization_keeps_the_exact_canonical_parent(self):
        parent = r'\\?\Volume{01234567-89ab-cdef-0123-456789abcdef}\Users'
        requested, normalized = parent + r'\RUNNER~1', parent + r'\runneradmin'
        self.assertNotEqual(requested.casefold(), normalized.casefold())  # Former guard rejected this alias.
        self.assertEqual(failure_diagnostics._windows_root_child(parent, normalized), normalized)
        for outside in [parent, parent + '\\', parent + r'\..', parent + r'\subdirectory\runneradmin',
                        parent + r'-other\runneradmin', normalized.replace('01234567', '87654321', 1),
                        r'C:\Users\runneradmin']:
            with self.subTest(outside=outside), self.assertRaisesRegex(OSError, 'location changed'):
                failure_diagnostics._windows_root_child(parent, outside)

    @unittest.skipUnless(os.name == 'nt', 'Actual Windows short-name normalization')
    def test_windows_short_root_alias_reads_owned_data_and_rejects_links(self):
        import ctypes as c
        with tempfile.TemporaryDirectory() as folder:
            parent = Path(folder); output = parent / 'diagnostic owned long root'; output.mkdir()
            (output / 'summary.json').write_text('OWNED_SHORT_ROOT_POSITIVE')
            outside = parent / 'outside.txt'; outside.write_text('OUTSIDE_SHORT_ROOT_SENTINEL')
            (output / 'runs.json').symlink_to(outside)
            k = c.WinDLL('kernel32', use_last_error=True)
            values = []
            for name in ('GetShortPathNameW', 'GetLongPathNameW'):
                call = getattr(k, name)
                call.argtypes, call.restype = [c.c_wchar_p, c.c_wchar_p, c.c_uint32], c.c_uint32
                buffer = c.create_unicode_buffer(32768)
                count = call(str(output), buffer, len(buffer))
                self.assertTrue(0 < count < len(buffer), f'{name}: {c.get_last_error()}')
                values.append(buffer.value)
            short, long = values
            if short.casefold() == long.casefold():
                self.skipTest('This volume has no 8.3 alias for the owned test path')
            with failure_diagnostics._WindowsDirectory.root(Path(short)) as held:
                self.assertEqual(held.read('summary.json'), 'OWNED_SHORT_ROOT_POSITIVE')
                with self.assertRaisesRegex(OSError, 'Unsafe evidence'):
                    held.read('runs.json')
            details = runner_failure_diagnostics(Path(short))
            self.assertIn('OWNED_SHORT_ROOT_POSITIVE', details)
            self.assertNotIn('OUTSIDE_SHORT_ROOT_SENTINEL', details)
            print('DIAGNOSTIC_SHORT_ROOT alias_observed=true owned_read=true unsafe_link_rejected=true', flush=True)

    def test_file_directory_and_root_links_do_not_expose_sibling_content(self):
        with tempfile.TemporaryDirectory() as folder:
            parent = Path(folder); output = parent / 'output'; attempt = output / '00000-fixture'
            attempt.mkdir(parents=True)
            outside = parent / 'outside'; outside.mkdir()
            (outside / 'stderr.log').write_text('OUTSIDE_DIAGNOSTIC_SENTINEL')
            (output / 'summary.json').write_text('OWNED_POSITIVE')
            (output / 'runs.json').symlink_to(outside / 'stderr.log')
            (attempt / 'stderr.log').symlink_to(outside / 'stderr.log')
            (output / '00001-linked').symlink_to(outside, target_is_directory=True)
            details = runner_failure_diagnostics(output)
            self.assertIn('OWNED_POSITIVE', details)
            self.assertNotIn('OUTSIDE_DIAGNOSTIC_SENTINEL', details)
            self.assertIn('Unsafe evidence', details)
            self.assertIn('attempt directory omitted', details)
            root_link = parent / 'linked-output'; root_link.symlink_to(output, target_is_directory=True)
            self.assertIn('diagnostics unavailable', runner_failure_diagnostics(root_link))

    def test_hard_link_is_omitted_but_owned_regular_file_is_read(self):
        with tempfile.TemporaryDirectory() as folder:
            parent = Path(folder); output = parent / 'output'; output.mkdir()
            outside = parent / 'outside'; outside.write_text('HARD_LINK_SENTINEL')
            os.link(outside, output / 'runs.json')
            (output / 'summary.json').write_text('OWNED_POSITIVE')
            details = runner_failure_diagnostics(output)
            self.assertIn('OWNED_POSITIVE', details)
            self.assertNotIn('HARD_LINK_SENTINEL', details)
            self.assertIn('Unsafe evidence', details)

    @unittest.skipUnless(os.name == 'posix', 'POSIX FIFO boundary')
    def test_fifo_is_rejected_without_opening_for_data(self):
        with tempfile.TemporaryDirectory() as folder:
            os.mkfifo(Path(folder) / 'runs.json')
            # One disposable child contains any regression; run kills/reaps it
            # on timeout. Call the actual collector, without its outer worker.
            code = 'import sys; sys.path.insert(0,sys.argv[1]); from failure_diagnostics import _collect; print(_collect(sys.argv[2]))'
            child = subprocess.run([sys.executable, '-c', code, str(HERE), folder],
                                   capture_output=True, text=True, timeout=1, check=True)
            self.assertIn('Unsafe evidence', child.stdout)

    def test_directory_scan_stops_before_entry_65(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            for number in range(80):
                (output / f'unrelated-{number}').touch()
            original = os.scandir
            visited = []
            class Counted:
                def __init__(self, path): self.entries = original(path)
                def __enter__(self): return self
                def __exit__(self, *_): self.entries.close()
                def __iter__(self): return self
                def __next__(self):
                    if len(visited) == 64: raise AssertionError('Unbounded directory scan')
                    entry = next(self.entries); visited.append(entry.name); return entry
            with patch.object(failure_diagnostics.os, 'scandir', side_effect=Counted):
                failure_diagnostics._collect(output)
            self.assertEqual(len(visited), 64)

    def test_held_attempt_cannot_be_redirected_to_another_directory(self):
        with tempfile.TemporaryDirectory() as folder:
            parent = Path(folder); output = parent / 'output'; attempt = output / '00000-fixture'
            attempt.mkdir(parents=True); (attempt / 'stderr.log').write_text('OWNED_POSITIVE')
            outside = parent / 'outside'; outside.mkdir(); (outside / 'stderr.log').write_text('OUTSIDE_SENTINEL')
            backend = failure_diagnostics._WindowsDirectory if os.name == 'nt' else failure_diagnostics._PosixDirectory
            with backend.root(output) as root, root.directory(attempt.name) as held:
                if os.name == 'nt':
                    with self.assertRaises(OSError): attempt.rename(output / 'moved')
                    with self.assertRaises(OSError): output.rename(parent / 'moved-output')
                    c, k, _, _ = held.api
                    writer = k.CreateFileW(held.path, 0x40000000, 7, None, 3, 0x02200000, None)
                    error = c.get_last_error()
                    if writer != c.c_void_p(-1).value: k.CloseHandle(writer)
                    self.assertEqual(writer, c.c_void_p(-1).value)
                    self.assertEqual(error, 32)  # Sharing violation prevents reparse mutation access.
                else:
                    attempt.rename(output / 'moved'); attempt.symlink_to(outside, target_is_directory=True)
                    with self.assertRaises(OSError): root.directory(attempt.name)
                self.assertEqual(held.read('stderr.log'), 'OWNED_POSITIVE')

    @unittest.skipUnless(os.name == 'nt', 'Actual NTFS junction boundary')
    def test_windows_junctions_are_not_traversed(self):
        with tempfile.TemporaryDirectory() as folder:
            parent = Path(folder); output = parent / 'output'; output.mkdir()
            outside = parent / 'outside'; outside.mkdir(); (outside / 'runs.json').write_text('JUNCTION_SENTINEL')
            (outside / 'stderr.log').write_text('JUNCTION_SENTINEL')
            (output / 'summary.json').write_text('OWNED_POSITIVE')
            for link, target in [(output / '00000-junction', outside), (parent / 'junction-output', outside)]:
                subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(link), str(target)],
                               capture_output=True, timeout=5, check=True)
            details = runner_failure_diagnostics(output)
            self.assertIn('OWNED_POSITIVE', details)
            self.assertNotIn('JUNCTION_SENTINEL', details)
            self.assertIn('attempt directory omitted', details)
            self.assertIn('diagnostics unavailable', runner_failure_diagnostics(parent / 'junction-output'))

    def test_stalled_worker_is_reaped_and_original_assertion_survives(self):
        original = subprocess.Popen
        children = []
        def started(*args, **kwargs):
            child = original(*args, **kwargs); children.append(child); return child
        with patch.object(failure_diagnostics, '_WORKER_SOURCE', 'import time; time.sleep(60)'), \
                patch.object(failure_diagnostics, 'TIMEOUT_SECONDS', .1), \
                patch.object(failure_diagnostics.subprocess, 'Popen', side_effect=started), \
                patch.object(benchmark, 'run', return_value=1), self.assertRaises(AssertionError) as failed:
            RunnerTests().assert_run_code(None, None, 2)
        self.assertIn('1 != 2', str(failed.exception))
        self.assertIn('TimeoutExpired', str(failed.exception))
        self.assertEqual(len(children), 1)
        self.assertIsNotNone(children[0].poll())

    def test_matching_return_does_not_read_diagnostic_files_or_launch_worker(self):
        with patch.object(benchmark, 'run', return_value=2), patch.object(
                failure_diagnostics.subprocess, 'run', side_effect=AssertionError('unexpected worker')):
            RunnerTests().assert_run_code(None, None, 2)
            RunnerTests().assert_attempt_status({'status': 'passed'}, 'passed', None)


class RunnerTests(unittest.TestCase):
    def assert_run_code(self, plan, output, expected):
        actual = benchmark.run(plan, output)
        self.assertEqual(actual, expected, runner_failure_diagnostics(output) if actual != expected else None)

    def assert_attempt_status(self, row, expected, output, *, require_problem=False):
        try:
            self.assertEqual(row['status'], expected)
            if require_problem:
                self.assertTrue(row['problems'])
        except self.failureException as error:
            raise self.failureException(f'{error}\n{runner_failure_diagnostics(output)}') from error

    def plan(self, root, mode='success'):
        fixture = HERE / 'fixture_workload.py'
        value = {'schema_version': 1, 'workload': {'id': 'fixture', 'kind': 'command',
                 'command': ['{python}', '{artifact:fixture}', '{attempt}', '{variant}', mode]},
                 'variants': [{'id': name, 'source_revision': character * 40, 'artifacts': {'fixture': str(fixture)}}
                              for name, character in [('baseline', 'a'), ('candidate', 'b')]],
                 'measurement': {'rounds': 1, 'warmups': 0, 'timeout_seconds': 10,
                                 'required_metrics': ['workload_ms']}}
        path = root / 'plan.json'; contract.write_json(path, value)
        return path

    def test_actual_successful_subprocess_and_directory_ownership(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan = self.plan(root); out = root / 'out'
            self.assert_run_code(plan, out, 2)
            report = contract.read_json(out / 'summary.json')
            self.assertEqual(report['failed_or_incomplete_attempts_including_warmups'], 0)
            self.assertFalse(report['gate_passed'])
            ledger = contract.read_json(out / 'runs.json')
            self.assertEqual(len(ledger['attempts']), 2)
            self.assertEqual(len({row['invocation_id'] for row in ledger['attempts']}), 2)
            with self.assertRaises(FileExistsError): benchmark.run(plan, out)

    def test_real_failure_protocols_never_pass(self):
        for mode in ['exit', 'failure', 'missing', 'stale', 'nan', 'missing_metric', 'outside_window']:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as folder:
                root = Path(folder); out = root / 'out'
                self.assert_run_code(self.plan(root, mode), out, 1)
                rows = contract.read_json(out / 'runs.json')['attempts']
                self.assert_attempt_status(rows[0], 'passed', out)
                self.assert_attempt_status(rows[1], 'failed', out, require_problem=True)

    def test_timeout_preserves_raw_denominator(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan = self.plan(root, 'timeout')
            value = contract.read_json(plan); value['measurement']['timeout_seconds'] = 1
            contract.write_json(plan, value)
            self.assert_run_code(plan, root / 'out', 1)
            rows = contract.read_json(root / 'out/runs.json')['attempts']
            self.assert_attempt_status(rows[1], 'timed_out', root / 'out')
            self.assertTrue((root / 'out' / rows[1]['directory'] / 'stdout.log').exists())

    def test_unexpected_return_preserves_real_failure_evidence_after_cleanup(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); out = root / 'out'
            with self.assertRaises(AssertionError) as failure:
                self.assert_run_code(self.plan(root, 'exit'), out, 2)
            details = str(failure.exception)
            self.assertIn('1 != 2', details)
            self.assertIn('Workload exited with code 17', details)
            self.assertIn('"status": "passed"', details)
            self.assertIn('"status": "failed"', details)
            self.assertIn('attempt.json', details)
            self.assertIn('isolated fixture candidate', details)
        self.assertFalse(out.exists())
        self.assertIn('Workload exited with code 17', str(failure.exception))

    def test_expected_failure_code_still_preserves_unexpected_baseline_evidence(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); out = root / 'out'; plan = self.plan(root, 'exit')
            value = contract.read_json(plan)
            # Force the existing exit-17 child behavior for both attempts. The
            # aggregate failure is expected, but the failed baseline is not.
            value['workload']['command'][3] = 'candidate'
            contract.write_json(plan, value)
            self.assert_run_code(plan, out, 1)
            rows = contract.read_json(out / 'runs.json')['attempts']
            self.assertEqual([row['status'] for row in rows], ['failed', 'failed'])
            with self.assertRaises(AssertionError) as failure:
                self.assert_attempt_status(rows[0], 'passed', out)
            self.assertIn("'failed' != 'passed'", str(failure.exception))
            self.assertIn('Workload exited with code 17', str(failure.exception))
            self.assertIn('isolated fixture candidate', str(failure.exception))
        self.assertFalse(out.exists())
        self.assertIn('Workload exited with code 17', str(failure.exception))

    def test_failure_diagnostics_bound_output_and_tolerate_missing_or_malformed_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'runs.json').write_bytes(b'{invalid-json-\xff')
            for number in range(5):
                attempt = root / f'{number:05d}-fixture'
                attempt.mkdir()
                for name in ('attempt.json', 'stderr.log', 'stdout.log', 'phase.json', 'ack.json', 'result.json'):
                    (attempt / name).write_bytes(b'BEGIN-' + b'x' * 65536 + b'-END')
            details = runner_failure_diagnostics(root)
            self.assertLessEqual(len(details), 32768)
            self.assertIn('{invalid-json-', details)
            self.assertIn('FileNotFoundError', details)  # Missing summary cannot mask the assertion.
            self.assertIn('bytes omitted', details)
            self.assertIn('BEGIN-', details); self.assertIn('-END', details)
            self.assertIn('remaining diagnostic output omitted', details)
            self.assertNotIn('00004-fixture', details)

    def test_plan_rejects_bool_numbers_duplicate_variants_empty_metrics(self):
        with tempfile.TemporaryDirectory() as folder:
            original = contract.read_json(self.plan(Path(folder)))
            for change in ['bool_rounds', 'duplicate', 'no_metrics', 'metrics_object', 'variant_scalar', 'measurement_list', 'path_null']:
                plan = deepcopy(original)
                if change == 'bool_rounds': plan['measurement']['rounds'] = True
                if change == 'duplicate': plan['variants'][1]['id'] = 'baseline'
                if change == 'no_metrics': plan['measurement']['required_metrics'] = []
                if change == 'metrics_object': plan['measurement']['required_metrics'] = [{}]
                if change == 'variant_scalar': plan['variants'][0] = False
                if change == 'measurement_list': plan['measurement'] = []
                if change == 'path_null': plan['variants'][0]['artifacts']['fixture'] = None
                with self.subTest(change=change), self.assertRaises(ValueError): benchmark.validate_plan(plan)

    def test_post_attempt_artifact_drift_preserves_raw_collection(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan_path = self.plan(root); receipt = root / 'receipt.json'
            receipt.write_text('original')
            plan = contract.read_json(plan_path); plan['workload']['artifacts'] = {'receipt': str(receipt)}
            contract.write_json(plan_path, plan)
            def actual_attempt(plan, variant, output, frozen, invocation):
                raw = {'status': 'passed', 'metrics': {'workload_ms': 10}, 'problems': [],
                       'collection': {'samples': [{'retained': True}]}}
                contract.write_json(output / 'attempt.json', raw)
                receipt.write_text('changed!')
                return raw
            with patch.object(benchmark, 'run_attempt', side_effect=actual_attempt):
                self.assert_run_code(plan_path, root / 'out', 1)
            rows = contract.read_json(root / 'out/runs.json')['attempts']
            self.assertEqual([row['status'] for row in rows], ['failed', 'failed'])
            attempt = root / 'out' / rows[0]['directory']
            self.assertTrue(contract.read_json(attempt / 'attempt.json')['collection']['samples'][0]['retained'])
            self.assertTrue((attempt / 'validation-error.json').exists())


if __name__ == '__main__':
    unittest.main()
