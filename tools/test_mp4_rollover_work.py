#!/usr/bin/env python3
"""Compare real MP4 writer recreation with rollover, preserving every output byte.

Counts actual C++ operator-new calls and requested bytes while production writer
operations run. This is cumulative allocation work, not peak RSS, application
speed, hardware encoder performance or a missing baseline API success claim.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/media/recorder-host'


def run(command, **kwargs):
    return subprocess.run([str(arg) for arg in command], check=True, timeout=120, **kwargs)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', help='Additionally compile that immutable Git source as the recreate control')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gesher-mp4-work-') as temporary:
        work = args.output_dir.resolve() if args.output_dir else Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        audio = work / 'packet.aac'
        run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', 'sine=frequency=750:sample_rate=48000',
             '-t', '0.1', '-c:a', 'aac', '-b:a', '96000', '-f', 'adts', audio])
        source = work / 'work-host.cc'
        host = (FIXTURE / 'lifecycle.cc').read_text().split('struct Events {')[0]
        source.write_text(host + (FIXTURE / 'rollover_work.cc').read_text())
        trees = [('candidate', ROOT)]
        if args.baseline:
            commit = subprocess.check_output(['git', 'rev-parse', '--verify', args.baseline + '^{commit}'],
                                             cwd=ROOT, text=True).strip()
            baseline = work / 'baseline'
            baseline.mkdir(exist_ok=True)
            archive = subprocess.check_output(['git', 'archive', '--format=tar', commit, 'compat/media'], cwd=ROOT)
            with tarfile.open(fileobj=io.BytesIO(archive)) as files:
                files.extractall(baseline, filter='data')
            trees.insert(0, (commit, baseline))
        results = []
        for name, tree in trees:
            media = tree / 'compat/media'
            supports = 'virtual bool SwitchFile(' in (media / 'mux/muxer.h').read_text()
            binary = work / ('work-' + name)
            run(shlex.split(args.cxx) + ['-std=c++20', '-O2', '-g', '-pthread', '-fno-exceptions',
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-mismatched-new-delete',
                '-DGESHER_MEDIA_STANDALONE', '-DHAS_SWITCH_FILE=' + str(int(supports)),
                '-I', tree, '-I', ROOT, '-I', FIXTURE / 'include', source,
                media / 'mux/mp4_muxer.cc', media / 'mux/ogg_muxer.cc', media / 'encode/codec_config.cc',
                '-o', binary])
            for mode in (('recreate', 'reuse') if supports else ('recreate',)):
                directory = work / (name + '-' + mode)
                directory.mkdir(exist_ok=True)
                result = json.loads(run([binary, mode, audio, directory], capture_output=True, text=True).stdout)
                result['source'] = name
                result['mp4_sha256'] = hashlib.sha256((media / 'mux/mp4_muxer.cc').read_bytes()).hexdigest()
                print(json.dumps(result, sort_keys=True), flush=True)
                results.append(result)
        control, reuse = results[-2:]
        assert control['mode'] == 'recreate' and reuse['mode'] == 'reuse'
        assert reuse['allocated_bytes'] < control['allocated_bytes']
        assert reuse['allocation_calls'] < control['allocation_calls']
        for result in results:
            assert (result['output_bytes'], result['checksum']) == (control['output_bytes'], control['checksum'])
        if args.baseline:
            assert (results[0]['allocated_bytes'], results[0]['allocation_calls']) == (
                control['allocated_bytes'], control['allocation_calls']), 'existing recreate path changed'
        # Compare all 32 actual files independently, rather than relying only
        # on the executable's rolling hash and self-reported byte count.
        digests = []
        for name, _ in trees:
            for mode in ('recreate', 'reuse') if name == 'candidate' else ('recreate',):
                digests.append([hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted((work / (name + '-' + mode)).glob('*.mp4'))])
        assert all(len(d) == 32 and d == digests[0] for d in digests)
        run(['ffmpeg', '-v', 'error', '-xerror', '-i', work / 'candidate-reuse/31.mp4', '-f', 'null', '-'])
        (work / 'measurements.json').write_text(json.dumps(results, indent=2) + '\n')
        print('32 output files byte-identical in every mode; final reused MP4 independently decoded')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
