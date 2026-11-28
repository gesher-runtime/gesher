#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Compare unchanged and candidate HTTP modules using fresh JVMs in ABBA order.

The loopback peer uses one selector. Every accepted request must complete with
all 4096 expected bytes. The report includes the deliberate 200 ms backend hold
in whole-work elapsed time and reports post-release drain time separately.
Process CPU/RSS include the peer, fixture, provider and VM; unavailable counters
remain null. This is a component workload, not Windows/app qualification.
"""
import argparse
import hashlib
import json
import math
import platform
import statistics
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--okhttp-source', type=Path, required=True)
    parser.add_argument('--baseline-ref', required=True)
    parser.add_argument('--rounds', type=int, default=6)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.rounds < 2:
        parser.error('Use at least two ABBA rounds')
    report = {'baseline_ref': args.baseline_ref,
              'candidate_base': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'candidate_sources_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                  for name in ['app/java/android/net/http/JavaHttpEngine.java',
                               'app/java/android/net/http/HttpRequestScheduler.java',
                               'tests/net/HttpEngineAdmissionTest.java']},
              'platform': platform.platform(), 'abba_rounds': args.rounds,
              'fixed_backend_hold_ms': 200, 'body_bytes_per_request': 4096,
              'process_counter_scope': 'whole JVM including one selector peer, test callbacks and production HTTP provider',
              'samples': [], 'summary': []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='gesher-http-admission-work-') as directory:
        work = Path(directory)
        for variant in ['baseline', 'candidate']:
            command = ['python3', str(ROOT / 'tools/test_http_engine_policy.py'),
                       '--java-home', str(args.java_home), '--okhttp-source', str(args.okhttp_source),
                       '--compile-output', str(work / variant)]
            if variant == 'baseline':
                command += ['--source-ref', args.baseline_ref]
            subprocess.run(command, cwd=ROOT, check=True, timeout=90)
        for count in [1, 32, 256, 1024]:
            for round_number in range(args.rounds):
                for position, variant in enumerate(['baseline', 'candidate', 'candidate', 'baseline']):
                    command = [str(args.java_home / 'bin/java'), '-cp', str(work / variant),
                               'HttpEngineAdmissionTest', '--host', '--burst', str(count)]
                    if variant == 'baseline':
                        command += ['--baseline']
                    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True,
                                            check=True, timeout=90)
                    lines = [line.removeprefix('HTTP_ADMISSION_WORK ') for line in result.stdout.splitlines()
                             if line.startswith('HTTP_ADMISSION_WORK ')]
                    if len(lines) != 1:
                        raise RuntimeError(f'Missing complete work receipt: {result.stdout}\n{result.stderr}')
                    sample = json.loads(lines[0])
                    if sample['requests'] != count or sample['decoded_bytes'] != count * 4096:
                        raise RuntimeError(f'Incomplete workload: {sample}')
                    sample.update(round=round_number, position=position, variant=variant)
                    report['samples'].append(sample)
                    args.output.write_text(json.dumps(report, indent=2) + '\n')
            for variant in ['baseline', 'candidate']:
                samples = [s for s in report['samples'] if s['requests'] == count and s['variant'] == variant]
                summary = {'requests': count, 'variant': variant, 'samples': len(samples)}
                for metric in ['blocked_connections', 'client_threads', 'drain_elapsed_ns',
                               'whole_elapsed_ns_including_fixed_hold', 'whole_process_cpu_ns',
                               'drain_process_cpu_ns', 'blocked_process_rss_bytes']:
                    values = sorted(s[metric] for s in samples if s[metric] is not None)
                    summary[metric] = {'median': statistics.median(values),
                                       'p95': values[math.ceil(len(values) * .95) - 1],
                                       'min': values[0], 'max': values[-1]} if values else None
                report['summary'].append(summary)
                print(json.dumps(summary), flush=True)
            args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
