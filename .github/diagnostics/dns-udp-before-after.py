#!/usr/bin/env python3
"""Focused Windows-only QA: real UDP collision, prior policy, fixed full DNS suite.

This diagnostic restores only the old fixed-UDP bind error policy in an isolated
generated source; production DNS and the new real collision control stay exact.
It does not change the checkout or accept a failed fixed suite.
"""
from pathlib import Path
import argparse
import hashlib
import os
import re
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repo', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
if os.name != 'nt':
    parser.error('requires actual Windows and an initialized MSVC environment')
repo = args.repo.resolve()
work = args.output.resolve()
work.mkdir(parents=True, exist_ok=True)
fixture = repo / 'tests/native/dns_admission_host.cc.in'
fixture_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
assert fixture_hash == '3cf139369e8cd16d425261e7a71189d3ab5fe8807d34b41d69115e7e599eead3', fixture_hash
print('DNS_UDP_FIXTURE SHA256 ' + fixture_hash, flush=True)
subprocess.run([sys.executable, str(repo / 'tools/test_dns_admission.py'),
                '--output', str(work)], cwd=repo, check=True, timeout=150)
source = (work / 'dns_admission.cc').read_text()
fixed_policy = 'if (!PeerAddressInUse(last_error) && last_error != WSAEACCES)'
assert source.count(fixed_policy) == 1
baseline = work / 'dns_admission_old_udp_policy.cc'
baseline.write_text(source.replace(fixed_policy, 'if (!PeerAddressInUse(last_error))'))
baseline_exe = work / 'dns_admission_old_udp_policy.exe'
subprocess.run(['cl', '/nologo', '/std:c++17', '/EHsc', '/W4', '/WX',
                '/D_WIN32_WINNT=0x0a00', '/DWINVER=0x0a00', str(baseline),
                '/Fe:' + str(baseline_exe), '/Fo:' + str(work) + '/',
                'ws2_32.lib', 'bcrypt.lib'], cwd=repo, check=True, timeout=120)

def execute(executable: Path, log: str, timeout: int):
    result = subprocess.run([str(executable)], cwd=repo, capture_output=True,
                            text=True, errors='replace', timeout=timeout)
    output = result.stdout + result.stderr
    (work / log).write_text(output)
    print(output, end='', flush=True)
    return result.returncode, output

code, output = execute(baseline_exe, 'old-udp-policy.log', 15)
assert code == 1, ('old UDP policy did not fail as required', code)
assert 'FAIL DNS admission: exclusive peer UDP bind (native socket error 10013)' in output, output
assert 'DNS admission contracts passed:' not in output
print('DNS_UDP_NEGATIVE_CONTROL exit=1 exact_error=10013 expected_failure=1', flush=True)
code, output = execute(work / 'dns_admission.exe', 'fixed-full-dns.log', 75)
assert code == 0, ('fixed full DNS suite failed', code)
assert re.search(r'DNS_PEER_PORT kind=exclusive-udp occupied_udp_retries=[1-9][0-9]* '
                 r'first_error=10013 shared_port_ready=1', output), output
assert 'DNS_PEER_PORT kind=exclusive-wildcard ' in output
assert re.search(r'DNS admission contracts passed: [1-9][0-9]* \(bounded production\)', output), output
print('DNS_UDP_FIXED_CONTROL collision=10013 recovery=1 full_dns_contracts=passed', flush=True)
