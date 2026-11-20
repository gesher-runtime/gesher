"""Synthetic parser mutation controls; these are NOT native Windows evidence."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from validate_stdio_evidence import main as validate


def transcript(full_wct):
    lines = ['ACTUAL_PUBLIC_LOCK_PROBE pid=42 public-api=true',
             'ACTUAL_PUBLIC_LOCK_PROBE pid=43 public-api=true']
    phases = ['start', 'held-and-entered', 'sample-and-negatives', 'independent-close',
              'unlock', 'actual-thread-exit-and-join', 'control-cleanup']
    for cycle in range(1, 5):
        for tick, phase in enumerate(phases):
            lines.append(f'ACTUAL_STDIO_CLOCK cycle={cycle} phase={phase} tick={1000 + tick} deadline=6000')
        for phase, first, second, recursive in [('before', 0, 0, 0),
                                               ('held-sibling-unlocked', 16, 0, 1),
                                               ('waiting', 16, 16, 1)]:
            lines.append(f'ACTUAL_PUBLIC_FILE_LOCK phase={phase} FILE=0x1000 sibling=0x2000 owner=10 '
                         f'owner-birth=100 probe=30 probe-birth=300 target-result={first} '
                         f'sibling-result={second} recursive-owner={recursive} signaled-exit=true handle-closed=true')
        lines += [
            'ACTUAL_CLOSER_STACK pid=42 closer=20 birth=200 FILE=0x1000 fd=5 captured=1 error=0 frames=2 suspend-before=0 resume-before=1 closed=1',
            'ACTUAL_CLOSER_FRAME index=0 pc=0x3100 nearest-export=synthetic',
            'ACTUAL_CLOSER_FRAME index=1 pc=0x5100 nearest-export=synthetic',
            'ACTUAL_CLOSER_FILE_CALL index=1 pc=0x5100 unwound-r12=0x1000 FILE=0x1000 exact-r12-rcx-copy-and-public-fclose=true unsupported-code-copy=reject',
            'ACTUAL_CLOSER_ENTER_FRAME index=0 pc=0x3100 named-entry=0x3000 unwind-begin=0x3000 unwind-end=0x3200',
            'ACTUAL_CLOSER_FILE_CALL_SUMMARY matches=1 required=1 enter-frames=1 other-FILE=0x2000 other-matches=0',
            f'ACTUAL_WCT samples=3 success=1 error=0 count={3 if full_wct else 1} cycle=0 contradictory=0 closer=20 owner=10 FILE=0x1000 completed-before-unlock=0',
            'ACTUAL_WCT_NODE index=0 type=8 status=3 pid=42 tid=20',
        ]
        if full_wct:
            lines += ['ACTUAL_WCT_NODE index=1 type=1 status=3 pid=0 tid=0',
                      'ACTUAL_WCT_NODE index=2 type=8 status=2 pid=42 tid=10']
        lines += [
            'ACTUAL_STDIO_PHASE independent-fd-closed result=0 closer-completed-before-unlock=0',
            'ACTUAL_STDIO_PHASE stream-unlocked',
            'ACTUAL_PUBLIC_NEGATIVE_CONTROLS distinct-live-FILE-lock=reject wrong-live-owner=reject actual-other-birth=reject completed-real-closer=reject unsupported-code-copy=reject call-other-FILE-argument=reject',
            f'ACTUAL_PUBLIC_NEGATIVE_VALUES cycle={cycle} closer=20 closer-birth=200 wrong-owner=30 wrong-birth=100 completed-control-tid=40 completed-control-birth=400 completed-control-close=0 completed-control-signaled=true',
            'WCT_NEGATIVE_METADATA_COPIES error=reject cycle=reject wrong-live-thread=reject not-actual-OS-cycle=true',
            'STDIO_ROUTE_METADATA_COPIES single-running=reject sticky-contradiction=reject not-actual-OS-waits=true',
            f'ACTUAL_STDIO_ROUTE cycle={cycle} route={"full-wct" if full_wct else "public-file-lock"} full-wct={int(full_wct)} single-blocked={int(not full_wct)} strong-witness=true independent-close=0 fclose=0 preunlock-completed=false signaled-exit=true joined=true fd-retired=true',
        ]
    return '\n'.join(lines) + '\n'


class EvidenceControls(unittest.TestCase):
    def validate_text(self, text):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / 'synthetic.txt'
            output = Path(directory) / 'parsed.json'
            evidence.write_text(text)
            environment = dict(GITHUB_ACTIONS='true', GITHUB_SHA='synthetic-parser-control',
                               GITHUB_RUN_ID='synthetic', GITHUB_RUN_ATTEMPT='0')
            with patch.dict(os.environ, environment), patch.object(sys, 'argv',
                    ['validator', '--stderr', str(evidence), '--output', str(output)]), \
                    contextlib.redirect_stdout(io.StringIO()):
                validate()
            return json.loads(output.read_text())

    def test_complete_routes(self):
        for route in (False, True):
            with self.subTest(full_wct=route):
                parsed = self.validate_text(transcript(route))
                self.assertEqual(len(parsed['cycles']), 4)
                self.assertEqual(parsed['source'], 'synthetic-parser-control')

    def test_broken_evidence_is_rejected(self):
        mutations = [
            ('missing second worker', 'ACTUAL_PUBLIC_LOCK_PROBE pid=43 public-api=true\n', ''),
            ('zero owner birth', 'owner-birth=100', 'owner-birth=0'),
            ('nonowner acquired held target', 'target-result=16', 'target-result=0'),
            ('independent sibling unavailable', 'sibling-result=0 recursive-owner=1', 'sibling-result=16 recursive-owner=1'),
            ('owner cannot recursively acquire', 'recursive-owner=1', 'recursive-owner=0'),
            ('wrong public FILE argument', 'unwound-r12=0x1000', 'unwound-r12=0x2000'),
            ('wrong process', 'ACTUAL_CLOSER_STACK pid=42', 'ACTUAL_CLOSER_STACK pid=99'),
            ('wrong closer birth', 'closer=20 birth=200', 'closer=20 birth=201'),
            ('unchecked public call', 'exact-r12-rcx-copy-and-public-fclose=true', 'exact-r12-rcx-copy-and-public-fclose=false'),
            ('unsupported instruction accepted', 'unsupported-code-copy=reject', 'unsupported-code-copy=accept'),
            ('wrong enter boundary', 'unwind-end=0x3200', 'unwind-end=0x3001'),
            ('owned suspend not resumed', 'resume-before=1', 'resume-before=2'),
            ('running closer', 'index=0 type=8 status=3', 'index=0 type=8 status=2'),
            ('wrong wait thread', 'index=0 type=8 status=3 pid=42 tid=20', 'index=0 type=8 status=3 pid=42 tid=30'),
            ('wait query failed', 'samples=3 success=1', 'samples=3 success=0'),
            ('sticky contradictory wait', 'contradictory=0', 'contradictory=1'),
            ('sibling matched close argument', 'other-matches=0', 'other-matches=1'),
            ('early close', 'preunlock-completed=false', 'preunlock-completed=true'),
            ('independent close failed', 'independent-fd-closed result=0', 'independent-fd-closed result=-1'),
            ('descriptor retained', 'fd-retired=true', 'fd-retired=false'),
            ('thread handle leaked', 'handle-closed=true', 'handle-closed=false'),
            ('probe did not exit', 'signaled-exit=true handle-closed=true', 'signaled-exit=false handle-closed=true'),
            ('completed control did not exit', 'completed-control-signaled=true', 'completed-control-signaled=false'),
            ('deadline extended', 'deadline=6000', 'deadline=6001'),
        ]
        for route in (False, True):
            original = transcript(route)
            for name, old, new in mutations:
                with self.subTest(full_wct=route, mutation=name):
                    self.assertIn(old, original)
                    with self.assertRaises((AssertionError, KeyError)):
                        self.validate_text(original.replace(old, new, 1))


if __name__ == '__main__':
    unittest.main()
