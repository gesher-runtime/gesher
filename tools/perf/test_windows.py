"""Real Windows job ownership/accounting; no APK or synthetic app capability."""
from __future__ import annotations
import ctypes as c
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

import benchmark
from contract import read_json, write_json
from windows import WindowsJob, U32, U64

HERE = Path(__file__).resolve().parent
WORKER = r'''
import json, pathlib, subprocess, sys, time
folder = pathlib.Path(sys.argv[1])
if len(sys.argv) == 2:
    child = subprocess.Popen([sys.executable, __file__, str(folder), 'branch'])
    (folder / 'root.json').write_text(json.dumps({'pid': __import__('os').getpid(), 'child': child.pid}))
    deadline = time.monotonic() + .2
    while time.monotonic() < deadline: sum(range(10000))
    (folder / 'io.bin').write_bytes(b'x' * (2 * 1024 * 1024))
else:
    leaf = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
    (folder / 'branch.json').write_text(json.dumps({'pid': __import__('os').getpid(), 'child': leaf.pid}))
    time.sleep(60)
'''


@unittest.skipUnless(os.name == 'nt', 'Requires real Windows Job Objects')
class WindowsOwnershipTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='gesher-perf-job-')
        self.root = Path(self.temporary.name)
        self.worker = self.root / 'worker.py'
        self.worker.write_text(WORKER)
        self.job = None
        self.processes = []

    def tearDown(self):
        if self.job is not None:
            self.job.close()
        for process in self.processes:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)
        self.temporary.cleanup()

    def start(self, command=None):
        self.job = WindowsJob(command or [sys.executable, str(self.worker), str(self.root)],
                              self.root, os.environ.copy(), self.root)
        return self.job

    def wait_json(self, name):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                return json.loads((self.root / name).read_text())
            except (OSError, ValueError):
                time.sleep(.01)
        self.fail(f'{name} was not produced')

    def open_identity(self, pid):
        handle = self.job.kernel.OpenProcess(0x100000 | 0x1000, False, pid)
        self.assertTrue(handle)
        created, exited, kernel, user = U64(), U64(), U64(), U64()
        self.assertTrue(self.job.kernel.GetProcessTimes(handle, c.byref(created), c.byref(exited),
                                                        c.byref(kernel), c.byref(user)))
        return handle, {'pid': pid, 'created_100ns': created.value}

    def test_exited_root_accounting_and_live_descendants_cleanup(self):
        job = self.start()
        branch = self.wait_json('branch.json')
        deadline = time.monotonic() + 10
        while job.poll() is None and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertEqual(job.poll(), 0)
        sample = job.sample()
        self.assertGreaterEqual(sample['job']['total_processes'], 3)
        self.assertGreater(sample['job']['user_cpu_ms'] + sample['job']['kernel_cpu_ms'], 0)
        self.assertGreaterEqual(sample['job']['write_bytes'], 2 * 1024 * 1024)
        self.assertGreater(sample['totals']['private_commit_bytes'], 0)
        self.assertIsNone(sample['totals']['unique_total_resident_bytes'])
        witnesses = [self.open_identity(pid)[0] for pid in (branch['pid'], branch['child'])]
        try:
            job.close(); self.job = None
            for handle in witnesses:
                self.assertEqual(job.kernel.WaitForSingleObject(handle, 10000), 0)
        finally:
            for handle in witnesses: job.kernel.CloseHandle(handle)

    def test_unrelated_process_not_admitted_or_terminated(self):
        sentinel = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        self.processes.append(sentinel)
        job = self.start([sys.executable, '-c', 'import time; time.sleep(60)'])
        handle, identity = self.open_identity(sentinel.pid)
        try:
            with self.assertRaisesRegex(ValueError, 'outside'):
                job.verify_participants([identity])
            job.close(); self.job = None
            self.assertIsNone(sentinel.poll())
        finally:
            job.kernel.CloseHandle(handle)

    def test_wrong_creation_identity_and_duplicate_are_rejected(self):
        job = self.start([sys.executable, '-c', 'import time; time.sleep(60)'])
        handle, identity = self.open_identity(job.pid)
        try:
            job.verify_participants([identity])
            with self.assertRaisesRegex(ValueError, 'Duplicate'):
                job.verify_participants([identity, identity])
            wrong = dict(identity, created_100ns=identity['created_100ns'] + 1)
            with self.assertRaisesRegex(ValueError, 'reused'):
                job.verify_participants([wrong])
        finally:
            job.kernel.CloseHandle(handle)

    def test_controller_death_does_not_leave_inherited_job_handle(self):
        supervisor = self.root / 'supervisor.py'
        supervisor.write_text("import os, pathlib, sys, time\n"
            + f"sys.path.insert(0, {str(HERE)!r})\nfrom windows import WindowsJob\n"
            + "folder=pathlib.Path(sys.argv[1])\n"
            + "job=WindowsJob([sys.executable,str(folder/'worker.py'),str(folder)],folder,os.environ.copy(),folder)\n"
            + "time.sleep(60)\n")
        controller = subprocess.Popen([sys.executable, str(supervisor), str(self.root)])
        self.processes.append(controller)
        branch = self.wait_json('branch.json')
        # Create a separate empty owned job only to expose documented typed APIs.
        separate = self.root / 'separate'; separate.mkdir()
        self.job = WindowsJob([sys.executable, '-c', 'import time; time.sleep(60)'], self.root,
                              os.environ.copy(), separate)
        witnesses = [self.open_identity(pid)[0] for pid in (branch['pid'], branch['child'])]
        try:
            controller.kill(); controller.communicate(timeout=10)
            for handle in witnesses:
                self.assertEqual(self.job.kernel.WaitForSingleObject(handle, 10000), 0)
        finally:
            for handle in witnesses: self.job.kernel.CloseHandle(handle)

    def test_startup_failure_closes_created_handles(self):
        job = self.start([sys.executable, '-c', 'import time; time.sleep(60)'])
        count = U32()
        current = c.c_void_p(-1)
        def handles():
            self.assertTrue(job.kernel.GetProcessHandleCount(current, c.byref(count)))
            return count.value
        before = handles()
        for index in range(12):
            output = self.root / str(index); output.mkdir()
            with self.assertRaises(FileNotFoundError):
                WindowsJob(['certainly-not-a-real-executable-perf178'], self.root, os.environ.copy(), output)
        self.assertLessEqual(handles(), before + 1)

    def test_collection_off_registers_child_before_normal_exit(self):
        plan = {'schema_version': 1, 'workload': {'id': 'registered-exit-fixture', 'kind': 'command',
                'command': ['{python}', '{artifact:fixture}', '{attempt}', '{variant}', 'register_exit']},
                'variants': [{'id': name, 'source_revision': character * 40,
                              'artifacts': {'fixture': str(HERE / 'fixture_workload.py')}}
                             for name, character in [('baseline', 'a'), ('candidate', 'b')]],
                'measurement': {'rounds': 1, 'warmups': 0, 'timeout_seconds': 30,
                                'collect_resources': False, 'required_metrics': ['workload_ms']}}
        plan_path = self.root / 'plan.json'; output = self.root / 'output'
        write_json(plan_path, plan)
        self.assertEqual(benchmark.run(plan_path, output), 2)  # Success, insufficient sample count.
        rows = read_json(output / 'runs.json')['attempts']
        self.assertEqual([row['status'] for row in rows], ['passed', 'passed'])
        for row in rows:
            folder = output / row['directory']
            raw = read_json(folder / 'attempt.json')
            self.assertEqual(raw['collection']['samples'], [])
            self.assertEqual(len(raw['collection']['verified_registrations']), 1)
            self.assertIsNone(raw['metrics']['cpu_ms'])
            self.assertTrue(read_json(folder / 'child-completed.json')['normal_exit'])


if __name__ == '__main__':
    unittest.main()
