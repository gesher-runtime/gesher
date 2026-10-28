#!/usr/bin/env python3
"""CI-only controller for actual Windows resources and internal failure dependencies.

This suite does not qualify atomic spawn, blocking waits or inherited OFD locks.
Every mode runs in a fresh owned process, including intentional quarantine.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import uuid


class OwnedJob:
    """Unnamed noninherited Windows job; closing the parent handle kills owned children."""
    def __enter__(self):
        assert os.name == 'nt' and ctypes.sizeof(ctypes.c_void_p) == 8, 'Actual Win64 fixture job'
        class Limits(ctypes.Structure):
            _fields_ = [('process_time', ctypes.c_int64), ('job_time', ctypes.c_int64),
                        ('flags', wintypes.DWORD), ('minimum', ctypes.c_size_t), ('maximum', ctypes.c_size_t),
                        ('active', wintypes.DWORD), ('affinity', ctypes.c_size_t),
                        ('priority', wintypes.DWORD), ('scheduling', wintypes.DWORD)]
        class Extended(ctypes.Structure):
            _fields_ = [('basic', Limits), ('io', ctypes.c_uint64 * 6),
                        ('process_memory', ctypes.c_size_t), ('job_memory', ctypes.c_size_t),
                        ('peak_process', ctypes.c_size_t), ('peak_job', ctypes.c_size_t)]
        assert ctypes.sizeof(Extended) == 144
        self.api = ctypes.WinDLL('kernel32', use_last_error=True)
        self.api.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        self.api.CreateJobObjectW.restype = wintypes.HANDLE
        self.api.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        self.api.SetInformationJobObject.restype = wintypes.BOOL
        self.api.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        self.api.AssignProcessToJobObject.restype = wintypes.BOOL
        self.api.CloseHandle.argtypes = [wintypes.HANDLE]
        self.api.CloseHandle.restype = wintypes.BOOL
        self.handle = self.api.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = Extended()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.api.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            error = ctypes.get_last_error()
            if not self.api.CloseHandle(self.handle):
                raise ctypes.WinError(ctypes.get_last_error())
            raise ctypes.WinError(error)
        self.children = []
        return self

    def own(self, process):
        # Popen's existing Windows kernel handle identifies this exact child;
        # never reopen by a possibly reused PID. These fixtures spawn no children
        # before assignment; atomic general-purpose spawn is not claimed here.
        if not self.api.AssignProcessToJobObject(self.handle, wintypes.HANDLE(int(process._handle))):
            raise ctypes.WinError(ctypes.get_last_error())
        self.children.append(process)

    def __exit__(self, kind, value, traceback):
        if not self.api.CloseHandle(self.handle):
            raise ctypes.WinError(ctypes.get_last_error())
        deadline = time.monotonic() + 5
        for child in self.children:
            child.wait(timeout=max(0, deadline - time.monotonic()))
        print(f'ACTUAL_DUPLICATE_JOB_CLEANUP children={len(self.children)} reaped=true', flush=True)


def required_cases(mode):
    if mode == 'raw-race':
        return {
            'held-reservation': 'actual occupied NUL target survives simultaneous raw CRT sentinel allocation',
            'raw-witness-contention': '1000 actual minimum duplicates preserve concurrent raw publisher witnesses and sentinel bytes',
            'raw-cleanup': 'raw publisher race leaves no private fd or native handle growth',
        }
    if mode == 'capacity':
        return {
            'actual-emfile': 'actual CRT allocator reaches real EMFILE',
            'actual-8192-slots': 'all8192 actual low-I/O descriptor slots are occupied',
            'duplicate-emfile-cleanup': 'actual full-table duplicate fails EMFILE with native reservation cleanup',
            'source-retained': 'capacity failure preserves original source fd',
            'capacity-cleanup': 'capacity failure returns native handles and private ownership to baseline',
        }
    if mode == 'retained-nul':
        return {
            'retained-nul-owned': 'every failed-close private NUL remains owned and counted',
            'real-fd-reuse': 'actual CRT publisher reuses retained fd number',
            'foreign-reuse-survives': 'reused foreign FILE_OBJECT survives private cleanup',
            'locks-preserved': 'retained NUL cleanup preserves actual POSIX/OFD source ownership',
        }
    if mode.startswith('quarantine-'):
        cases = {
            'quarantine-owned': 'uncertain source alias is explicitly retained and counted',
            'quarantine-fails-closed': 'public close fails closed before changing original ownership',
            'quarantine-real-cleanup': 'fixture retires its actual identified uncertain alias',
            'quarantine-foreign-reuse': 'quarantine never retries/recloses foreign reused fd',
        }
        live = {
            'quarantine-socket': 'actual retained duplicate SOCKET remains live and explicitly table-owned',
            'quarantine-event': 'actual retained event reference remains live and explicitly table-owned',
            'quarantine-binder': 'actual retained Binder reference completes real broker roundtrip',
        }
        if mode in live:
            cases['quarantine-resource-live'] = live[mode]
        return cases
    cases = {
        'rollback-error': 'explicit flag-failure dependency returns actual failure',
        'rollback-retired': 'all unpublished duplicate ownership retired',
        'rollback-handle-parity': 'actual native handle count returns to warm baseline',
        'rollback-foreign-reuse': 'reused fd reads actual sentinel, never stale virtual redirection',
    }
    if mode == 'regular':
        cases.update({
            'posix-preserved': 'internal rollback preserved source POSIX any-close locks',
            'ofd-preserved': 'internal rollback preserved source OFD lock',
            'actual-final-close': 'actual final user close releases POSIX and OFD ownership',
        })
    elif mode == 'device':
        cases['device-live'] = 'original device dispatch remains live'
    elif mode == 'event':
        cases['event-live'] = 'original event object remains live'
    elif mode == 'socket':
        cases['socket-peer'] = 'original actual SOCKET peer receives after rollback'
        cases['socket-noinherit'] = 'socket plain duplicate preserves existing never-inherit policy'
    elif mode == 'binder':
        cases['binder-roundtrip'] = 'original Binder source completes actual broker roundtrip'
    return cases


def run(worker, broker):
    assert os.environ.get('GITHUB_ACTIONS') == 'true', 'CI-only native fixture'
    results = []
    with tempfile.TemporaryDirectory(prefix='gesher-duplicate-') as directory, OwnedJob() as job:
        root = Path(directory)
        endpoint = 'native3-' + uuid.uuid4().hex
        environment = dict(os.environ, GESHER_BINDER=endpoint, GESHER_BINDER_TRACE='1')
        broker_log = root / 'broker.log'
        broker_output = broker_log.open('wb')
        service = None
        children = []
        binder_pids = []
        qualified = False
        try:
            service = subprocess.Popen([str(broker), endpoint], stdin=subprocess.DEVNULL,
                                       stdout=broker_output, stderr=subprocess.STDOUT, env=environment)
            job.own(service)
            deadline = time.monotonic() + 5
            while True:
                assert service.poll() is None, ('owned broker exited before readiness', service.returncode)
                if 'binderd: serving device ' + endpoint in broker_log.read_text(errors='replace'):
                    break
                assert time.monotonic() < deadline, 'owned broker readiness exceeded five seconds'
                time.sleep(0.01)
            print(f'ACTUAL_DUPLICATE_BROKER pid={service.pid} endpoint={endpoint} ready=actual-pipe-created', flush=True)
            results = run_modes(worker, root, environment, children, binder_pids, job)
            assert len(results) == 13 and all(child.poll() == 0 for child in children), 'actual client normal exit/PID closure audit'
            text = broker_log.read_text(errors='replace')
            observed = [int(pid) for pid in re.findall(r'proc \d+: pid (\d+) opened the device', text)]
            assert sorted(observed) == sorted(binder_pids) and len(observed) == 2, ('actual isolated broker opens', observed, binder_pids)
            assert service.poll() is None, 'owned broker died before completed client audit'
            qualified = True
            print(f'ACTUAL_DUPLICATE_BROKER_AUDIT client-pids={binder_pids} normal-exits=13 opens=2 unexpected=0', flush=True)
        finally:
            # binderd deliberately serves until killed; this is explicit owned
            # server teardown, never fabricated proof of a normal broker exit0.
            killed = False
            try:
                if service is not None:
                    if service.poll() is None:
                        service.terminate()
                        killed = True
                    service.wait(timeout=5)
                    if qualified and not killed:
                        raise AssertionError('Healthy owned broker exited before its controlled stop')
            finally:
                broker_output.close()
                print(broker_log.read_text(errors='replace'), end='', flush=True)
            if service is not None:
                print(f'ACTUAL_DUPLICATE_BROKER_CLEANUP pid={service.pid} exit={service.returncode} reaped=true intentional-owned-kill={killed} clients-qualified={qualified}', flush=True)
    print(f'ACTUAL_DUPLICATE_ORACLE modes={len(results)} failures=0 semantics=bounded-resource-controls', flush=True)
    return results


def run_modes(worker, root, environment, children, binder_pids, job):
    kinds = ('regular', 'device', 'event', 'socket', 'binder')
    modes = (*kinds, 'retained-nul', *('quarantine-' + kind for kind in kinds), 'raw-race', 'capacity')
    results = []
    source, sentinel = root / 'source', root / 'sentinel'
    source.write_bytes(b'x' * 1024)
    sentinel.write_bytes(b'S')
    for mode in modes:
        process = subprocess.Popen([str(worker), mode, str(source), str(sentinel)],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, env=environment)
        children.append(process)
        if mode in ('binder', 'quarantine-binder'):
            binder_pids.append(process.pid)
        try:
            job.own(process)
            output, unused = process.communicate(timeout=60)
            print(output, end='', flush=True)
            assert process.returncode == 0, (mode, process.returncode)
            summaries = re.findall(r'^ACTUAL_DUPLICATE_UNIT assertions=(\d+) failures=(\d+) '
                                   r'scope=actual-resource-dependency-controls atomic-spawn-open$',
                                   output, re.MULTILINE)
            assert len(summaries) == 1, (mode, 'missing/duplicate actual completion')
            assertions, failures = map(int, summaries[0])
            assert assertions > 0 and failures == 0, (mode, summaries)
            cases = required_cases(mode)
            for identifier, label in cases.items():
                matches = re.findall(r'^PASS duplicate ownership: ' + re.escape(label) + r'$', output, re.MULTILINE)
                assert len(matches) == 1, (mode, identifier, 'missing/duplicate required actual case')
            if mode == 'raw-race':
                assert re.search(r'^ACTUAL_RAW_PUBLISHER duplicates=1000 '
                                 r'raw-publications=[1-9][0-9]* errors=0$', output, re.MULTILINE), output
            results.append(dict(mode=mode, assertions=assertions, failures=failures, cases=list(cases)))
            print('ACTUAL_DUPLICATE_ROW ' + json.dumps(results[-1], sort_keys=True), flush=True)
        finally:
            if process.poll() is None:
                process.kill()
                try:
                    tail, unused = process.communicate(timeout=5)
                    print(tail, end='', flush=True)
                except subprocess.TimeoutExpired as error:
                    raise AssertionError('owned duplicate fixture did not terminate within five seconds') from error
            process.stdout.close()
    return results

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--worker', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--broker', required=True, type=Path)
    arguments = parser.parse_args()
    rows = run(arguments.worker.resolve(strict=True), arguments.broker.resolve(strict=True))
    arguments.output.write_text(json.dumps(rows, indent=2) + '\n')
