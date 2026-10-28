"""Require all actual ordinary Binder cases and independent stable-page observations."""
import hashlib
from pathlib import Path
import re
import sys

lines = [re.sub(r'^\S+Z ', '', row) for row in Path(sys.argv[1]).read_text(encoding='utf-8-sig').splitlines()]
expected = {'host/host'} | {mode + '/' + abi for mode in ('elf', 'art', 'single', 'device') for abi in ('x86_64', 'arm64-v8a')}
scenarios = {'normal', 'malformed', 'oneway', 'smallmap', 'badread', 'prefixfault', 'adoptfail', 'readwrite', 'readonly'}
cases = {}
for row in lines:
    if not row.startswith('ACTUAL_SG_OPEN_AUDIT '):
        continue
    match = re.fullmatch(r'ACTUAL_SG_OPEN_AUDIT mode=(\S+) abi=(\S+) scenario=(\S+) broker=(\d+) server=(\d+) client=(\d+) opens=2 unexpected=0 scope=observed-run-only', row)
    assert match, row
    mode, abi, scenario, broker, server, client = match.groups()
    assert len({broker, server, client}) == 3
    key = mode + '/' + abi + '/' + scenario
    assert key not in cases, ('Duplicate case', key)
    cases[key] = server
assert set(cases) == {c + '/' + s for c in expected for s in scenarios}, 'Missing or extra ordinary case'
observed = {}
for row in lines:
    if not row.startswith('ACTUAL_BINDER_OUTPUT_PAGE '):
        continue
    match = re.fullmatch(r'ACTUAL_BINDER_OUTPUT_PAGE stage=(before|after) mode=(\S+) abi=(\S+) scenario=(readonly|readwrite) pid=(\d+) address=(\d+) bytes=4096 protection=(\d+) sha256=([a-f0-9]{64})', row)
    assert match, row
    stage, mode, abi, scenario, pid, address, protection, digest = match.groups()
    key = mode + '/' + abi + '/' + scenario
    assert pid == cases[key]
    target = observed.setdefault(key, {})
    assert stage not in target, ('Duplicate page observation', key, stage)
    target[stage] = (pid, address, protection, digest)
assert set(observed) == {c + '/' + s for c in expected for s in ('readonly', 'readwrite')}
sentinel = hashlib.sha256(b'\x5a' * 4096).hexdigest()
for key, values in observed.items():
    before, after = values['before'], values['after']
    assert before[:3] == after[:3] and before[3] == sentinel
    if key.endswith('/readonly'):
        assert before[2] == '2' and before[3] == after[3]
    else:
        assert before[2] == '4' and before[3] != after[3]
for label in (
        'stable readonly output returns EFAULT without publishing transaction bytes',
        'whole readonly output page remains unchanged after failure',
        'failed publication rolls back adopted owned descriptor slot',
        'rolled-back descriptor slot is actually reusable',
        'writable output receives actual owned descriptor and independent file bytes',
        'writable output actually frees receiver buffer and returns ordinary reply'):
    assert lines.count('PASS SG server: ' + label) == 9, ('Missing real FD/page assertion', label)
client_reports = [re.fullmatch(r'SG client: ([1-9]\d*) checks, 0 failed', x) for x in lines]
server_reports = [re.fullmatch(r'SG server: ([1-9]\d*) checks, 0 failed', x) for x in lines]
client_reports = [x for x in client_reports if x]
server_reports = [x for x in server_reports if x]
assert len(client_reports) == len(server_reports) == 81
assert sum(int(x.group(1)) for x in client_reports + server_reports) == 1944
assert len([x for x in lines if re.fullmatch(r'PASS SG mode: device/\S+ (server|client) actual app normal exit-code-zero result', x)]) == 36
assert len([x for x in lines if re.fullmatch(r'PASS SG mode: \S+ (server|client) actual actor PID exited', x)]) == 162
assert len([x for x in lines if re.fullmatch(r'PASS SG mode: \S+/oneway actual sender normal exit0 verified before receiver delivery', x)]) == 9
assert len([x for x in lines if re.fullmatch(r'SG modes: 963 checks, 0 failed;.*', x)]) == 1
assert not any(x.startswith('FAIL SG') for x in lines)
print(f'ACTUAL_BINDER_FULL_COUNTS contexts={len(expected)} cases={len(cases)} actor-reports=162 pages=36 RW=9 RO=9 device-normal0=36 failures=0', flush=True)
