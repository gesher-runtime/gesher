"""Explicit isolated harness fixture; never used as application evidence."""
from pathlib import Path
import gc
import json
import os
import subprocess
import sys
import time

import protocol_io


def write(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value), encoding='utf-8')
    temporary.replace(path)


def main():
    output, variant, mode = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    request = json.loads((output / 'request.json').read_text())
    invocation = request['invocation_id']

    def phase(name):
        deadline = time.monotonic() + 10
        protocol_io.write_json(output / 'phase.json', {'invocation_id': invocation, 'phase': name, 'participants': []},
                               deadline=deadline)
        while time.monotonic() < deadline:
            ack = output / 'ack.json'
            if ack.exists():
                message = protocol_io.read_json(ack, deadline=deadline)
                if message.get('invocation_id') != invocation:
                    raise ValueError('fixture acknowledgement invocation mismatch')
                if message['phase'] == name:
                    return
            time.sleep(.005)
        raise RuntimeError('fixture acknowledgement timeout')

    phase('start')
    start = time.monotonic()
    participants = []
    if mode == 'register_exit':
        # Real owned Windows child, registered while alive and then naturally
        # exited before final validation. No optional resource sample is needed.
        worker = output / 'registered-worker.py'
        worker.write_text(r'''
import ctypes as c, json, os, pathlib, sys, time
folder=pathlib.Path(sys.argv[1])
k=c.WinDLL('kernel32', use_last_error=True)
k.GetProcessTimes.argtypes=[c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p]
k.GetProcessTimes.restype=c.c_int
created,exited,kernel,user=c.c_uint64(),c.c_uint64(),c.c_uint64(),c.c_uint64()
if not k.GetProcessTimes(c.c_void_p(-1),c.byref(created),c.byref(exited),c.byref(kernel),c.byref(user)):
    raise OSError(c.get_last_error())
temporary=folder/'child-identity.tmp'
temporary.write_text(json.dumps({'pid':os.getpid(),'created_100ns':created.value,'role':'fixture-application'}))
temporary.replace(folder/'child-identity.json')
deadline=time.monotonic()+20
while not (folder/'child-exit').exists():
    if time.monotonic()>deadline: raise RuntimeError('parent never registered child')
    time.sleep(.005)
''')
        child = subprocess.Popen([sys.executable, str(worker), str(output)])
        deadline = time.monotonic() + 10
        while not (output / 'child-identity.json').exists():
            if time.monotonic() > deadline: raise RuntimeError('child identity timeout')
            time.sleep(.005)
        participants = [protocol_io.read_json(output / 'child-identity.json', deadline=deadline)]
        protocol_io.write_json(output / 'participants.json', {'invocation_id': invocation, 'sequence': 1,
                               'participants': participants}, deadline=deadline)
        while True:
            ack_path = output / 'participants-ack.json'
            if ack_path.exists():
                ack = protocol_io.read_json(ack_path, deadline=deadline)
                if ack == {'invocation_id': invocation, 'sequence': 1, 'verified': True}: break
            if time.monotonic() > deadline: raise RuntimeError('child registration timeout')
            time.sleep(.005)
        (output / 'child-exit').touch()
        if child.wait(timeout=10) != 0: raise RuntimeError('registered child failed')
        del child
        gc.collect()  # Release the workload's Popen handle before final evidence.
        write(output / 'child-completed.json', {'normal_exit': True})
    # The fixture can deliberately leave a descendant for ownership/cleanup tests.
    if mode == 'descendant':
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        write(output / 'descendant.json', {'pid': child.pid})
    if variant == 'candidate' and mode == 'timeout':
        time.sleep(60)
    # Real file I/O and CPU make Windows job accounting observable.
    data = bytearray(2 * 1024 * 1024)
    data[:] = b'x' * len(data)
    (output / 'owned-payload.bin').write_bytes(data)
    total = 0
    deadline = time.monotonic() + .08
    while time.monotonic() < deadline:
        total += sum(range(1000))
    elapsed = (time.monotonic() - start) * 1000
    phase('stop')
    if variant == 'candidate' and mode == 'missing':
        return 0
    result = {'schema_version': 1, 'invocation_id': invocation, 'variant': variant,
              'workload_id': request['workload']['id'], 'completed': True, 'success': True,
              'coverage_complete': True, 'problems': [], 'events': [],
              'workload_elapsed_ms': elapsed, 'metrics': {'workload_ms': elapsed}, 'participants': participants}
    if variant == 'candidate':
        if mode == 'stale': result['invocation_id'] = '0' * 32
        if mode == 'failure': result['success'] = False
        if mode == 'nan': result['metrics']['workload_ms'] = float('nan')
        if mode == 'missing_metric': result['metrics'] = {}
        if mode == 'outside_window': result['workload_elapsed_ms'] = 1e9
    write(output / 'result.json', result)
    print(f'isolated fixture {variant} {total}', flush=True)
    return 17 if variant == 'candidate' and mode == 'exit' else 0


if __name__ == '__main__':
    raise SystemExit(main())
