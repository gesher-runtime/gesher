"""CI-only live multi-process controls; no expected result file is fabricated.

Linux observes actual kernel queued fcntl requests through /proc/locks.
Windows observes the registered conflict request, then requires actual result,
owned process exit and cleanup. Registration alone never counts as acquisition.
There is no sleep-based assertion of blocking. Every child has a hard lifetime.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import tempfile
import threading
import time


class Worker:
    def __init__(self, executable, role, path, command, mode):
        self.process = subprocess.Popen([str(executable), role, str(path), str(command), mode],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        text=True, bufsize=1)
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stdout:
            self.lines.put(line.strip())
        self.lines.put(None)

    def receive(self, prefix, timeout=6):
        line = self.lines.get(timeout=timeout)
        assert line is not None and line.startswith(prefix + ' '), (prefix, line, self.process.poll())
        return line.split()[1:]

    def send(self, command):
        self.process.stdin.write(command + '\n')
        self.process.stdin.flush()

    def finish(self, command=None, killed=False):
        if command:
            self.send(command)
        if killed:
            self.process.kill()
        result = self.process.wait(timeout=6)
        assert result != 0 if killed else result == 0, result
        self.reader.join(timeout=2)
        assert not self.reader.is_alive(), 'owned stdout reader must reach EOF'
        self.process.stdin.close()
        self.process.stdout.close()


def observe(waiter, windows):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if windows:
            waiter.send('observe')
            if waiter.receive('registered') == ['1']:
                return
        else:
            # Match the actual kernel pending arrow, PID and lock family.
            text = Path('/proc/locks').read_text()
            pattern = rf'^\d+:\s+->\s+(?:POSIX|OFDLCK)\s+ADVISORY\s+WRITE\s+{waiter.process.pid}\s'
            # Linux OFD entries deliberately expose pid -1. OFD reference
            # cases below therefore use holder POSIX + waiter POSIX for the
            # observed pending edge, and separate OFD holder lifetime.
            if re.search(pattern, text, re.MULTILINE):
                return
        assert waiter.process.poll() is None
    raise AssertionError('real conflict registration/kernel queue not observed')


def run(executable, windows):
    rows = []
    with tempfile.TemporaryDirectory(prefix='gesher-blocking-') as directory:
        path = Path(directory) / 'lock'
        path.write_bytes(b'x')
        cases = [('unlock', 7, 'normal'), ('close', 7, 'normal'),
                 ('dupclose', 7, 'normal'), ('crash', 7, 'normal'),
                 ('interrupt', 7, 'normal')]
        if windows:
            cases += [('timeout', 7, 'timeout')]
        else:
            cases += [('restart', 7, 'restart'), ('unlock', 38, 'normal'),
                      ('ofd-alias', 38, 'normal')]
        for action, holder_command, mode in cases:
            children = []
            try:
                holder = Worker(executable, 'holder', path, holder_command, 'normal')
                children.append(holder)
                assert holder.receive('held') == [str(holder.process.pid)]
                waiter = Worker(executable, 'waiter', path, 7, mode)
                children.append(waiter)
                assert waiter.receive('ready') == [str(waiter.process.pid)]
                assert waiter.process.pid != holder.process.pid
                observe(waiter, windows)
                assert waiter.lines.empty(), 'conflicting request cannot complete before release'
                if action in ('interrupt', 'restart'):
                    waiter.send('interrupt')
                    if action == 'restart':
                        # Confirm actual signal-handler delivery with SA_RESTART
                        # before checking the kernel request still conflicts.
                        deadline = time.monotonic() + 2
                        while True:
                            waiter.send('observe')
                            if waiter.receive('caught') == ['1']:
                                break
                            assert time.monotonic() < deadline
                        observe(waiter, False)
                        assert waiter.lines.empty()
                        holder.send('unlock')
                        assert holder.receive('released') == ['0']
                elif action == 'timeout':
                    pass  # engine's original absolute deadline must expire
                elif action == 'crash':
                    holder.finish(killed=True)
                    children.remove(holder)
                elif action == 'ofd-alias':
                    holder.send('dupclose')
                    assert holder.receive('released') == ['0']
                    observe(waiter, False)  # OFD lock survives nonfinal alias close
                    assert waiter.lines.empty()
                    holder.send('close')
                    assert holder.receive('released') == ['0']
                else:
                    holder.send(action)
                    assert holder.receive('released') == ['0']
                expected = ('canceled' if windows else 'interrupted') if action == 'interrupt' else \
                           'timeout' if action == 'timeout' else 'success'
                result = waiter.receive('result')
                assert result[0] == expected, (action, expected, result)
                if expected == 'success':
                    assert result[1] == '0'
                # Still-live acquisition must exclude a real third process;
                # canceled/timed-out requests must leave the holder intact.
                probe = Worker(executable, 'probe', path, 7, 'normal')
                children.append(probe)
                assert probe.receive('conflict') == ['1']
                probe.finish()
                children.remove(probe)
                if windows:
                    waiter.send('observe')
                    assert waiter.receive('registered') == ['0'], 'completed request must be unpublished'
                waiter.finish('join')
                children.remove(waiter)
                if holder in children:
                    holder.finish('exit')
                    children.remove(holder)
                row = dict(action=action, holder_command=holder_command, mode=mode,
                           outcome=result[0], actual_errno=int(result[1]))
                rows.append(row)
                print('BLOCKING_ROW ' + json.dumps(row, sort_keys=True), flush=True)
            finally:
                for child in children:
                    if child.process.poll() is None:
                        child.process.kill()
                    child.process.wait(timeout=6)
                    child.reader.join(timeout=2)
                    assert not child.reader.is_alive()
                    child.process.stdin.close()
                    child.process.stdout.close()
        if windows:
            for command in (7, 38):
                control = Worker(executable, 'public-gaps', path, command, 'normal')
                try:
                    assert control.receive('public-result') == ['-1']
                    assert control.receive('public-einval') == ['1']
                    control.finish()
                finally:
                    if control.process.poll() is None:
                        control.finish(killed=True)
                row = dict(public_command=command, result=-1, einval=1, outcome='common-EINVAL-open')
                rows.append(row)
                print('BLOCKING_PUBLIC_ROW ' + json.dumps(row, sort_keys=True), flush=True)
    return rows


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--worker', required=True, type=Path)
    parser.add_argument('--windows', action='store_true')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--reference', type=Path)
    args = parser.parse_args()
    assert args.windows == (os.name == 'nt')
    actual = run(args.worker.resolve(), args.windows)
    if args.windows:
        assert args.reference is not None
        reference = json.loads(args.reference.read_text())
        for action in ('unlock', 'close', 'dupclose', 'crash'):
            left = next(row for row in reference if row.get('action') == action and row['holder_command'] == 7)
            right = next(row for row in actual if row.get('action') == action)
            assert left == right, (left, right)
    args.output.write_text(json.dumps(actual, indent=2) + '\n')
    print(f'BLOCKING_SUMMARY rows={len(actual)} failures=0 public_blocking_closed=false', flush=True)
