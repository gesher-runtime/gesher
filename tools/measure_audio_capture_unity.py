#!/usr/bin/env python3
"""Retain every complete capture component workload in fresh-process ABBA order.

Build both variants with test_audio_capture_unity.py --measure --build-only.
Actual source code, compiler options, output bytes/checksums and counters must
match the declared comparison. Synthetic source time advances by 10 ms; the
measured wall clock is processing time, not live-device capture latency.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time

NAMES = ('cold-stereo', 'small-streams', 'mono48-s16', 'stereo48-s16', 'eight48-s16',
         'stereo48-float', 'resample44-control', 'downmix-control', 'overflow-stereo')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True, type=Path)
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('--rounds', type=int, default=4)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.rounds < 1 or args.output.exists():
        parser.error('use positive rounds and a new output file')
    folders = {'baseline': args.baseline.resolve(), 'candidate': args.candidate.resolve()}
    result = {'schema': 1, 'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'platform': platform.platform(), 'rounds': args.rounds,
              'order': ['baseline', 'candidate', 'candidate', 'baseline'],
              'scope': 'Actual CaptureStream/PCM/paced-WAV sections; supplied engine attachment and source clock; '
                       'WAV loading, source Pull, Push, Read, stats and teardown included; ordinary allocators',
              'sources': {}, 'processes': [], 'summary': {}}
    for label, folder in folders.items():
        source = json.loads((folder / 'source.json').read_text())
        assert not source['channel_frame_observer'] and not source['sanitize'], (label, 'instrumented timing')
        executable = folder / 'capture-unity'
        assert hashlib.sha256(executable.read_bytes()).hexdigest() == source['executable_sha256'], label
        result['sources'][label] = source
    for key in ('fixture_sha256', 'header_sha256', 'template_sha256', 'compiler_version'):
        assert result['sources']['baseline'][key] == result['sources']['candidate'][key], key
    for source in result['sources'].values():
        assert source['command'][-2] == '-I', 'unexpected generated-header include position'
    assert (result['sources']['baseline']['command'][:-1] ==
            result['sources']['candidate']['command'][:-1]), 'different compiler options'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for block in range(args.rounds):
        for order, label in enumerate(result['order']):
            folder = folders[label]
            run = subprocess.run([str(folder / 'capture-unity'), 'measure', str(folder)],
                                 capture_output=True, text=True, timeout=120)
            records = []
            for match in re.finditer(r'^CAPTURE_STREAM (\S+) (.*)$', run.stdout, re.M):
                values = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', match[2])}
                records.append({'name': match[1], **values})
            process = {'block': block, 'order': order, 'variant': label,
                       'returncode': run.returncode, 'samples': records,
                       'stdout': run.stdout, 'stderr': run.stderr}
            result['processes'].append(process)
            args.output.write_text(json.dumps(result, indent=2) + '\n')
            assert run.returncode == 0 and not run.stderr, process
            assert tuple(record['name'] for record in records) == NAMES, process
            assert re.search(r'^CAPTURE_UNITY_PASS checks=\d+$', run.stdout, re.M), process
            assert all(record['channel_frames'] == 0 for record in records), 'timed source observer'
    for name in NAMES:
        by = {label: [sample for process in result['processes'] if process['variant'] == label
                      for sample in process['samples'] if sample['name'] == name] for label in folders}
        for field in ('instances', 'blocks', 'frames', 'lost', 'checksum'):
            assert len({sample[field] for rows in by.values() for sample in rows}) == 1, (name, field)
        summary = {}
        for metric in ('cpu_ns', 'wall_ns'):
            summary[metric] = {}
            for label in folders:
                values = [row[metric] for row in by[label]]
                summary[metric][label] = {'median': statistics.median(values),
                                         'min': min(values), 'max': max(values), 'samples': len(values)}
            a, b = (summary[metric][label]['median'] for label in folders)
            summary[metric]['change_percent'] = (b / a - 1) * 100
        result['summary'][name] = summary
        print(name, json.dumps(summary), flush=True)
    result['finished_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('CAPTURE_ABBA_PASS processes=' + str(len(result['processes'])) +
          ' samples=' + str(sum(len(p['samples']) for p in result['processes'])), flush=True)


if __name__ == '__main__':
    main()
