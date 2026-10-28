#!/usr/bin/env python3
"""Run the SAME command worker against Linux fcntl or the Windows common layer.

The reference is an actual Linux result file, never a second interval model.
This first slice deliberately does not claim blocking, inherited OFD or spawn.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import traceback


class Client:
    def __init__(self, executable, paths):
        self.process = subprocess.Popen([str(executable), *paths], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True, bufsize=1)
        self.expected_exit = 0
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        try:
            ready = self.receive()
            assert ready.get('ready') and ready.get('pointer_high'), ready
            assert ready['pid'] == self.process.pid, ready
        except BaseException:
            self.cleanup()
            raise

    def _read(self):
        try:
            for line in self.process.stdout:
                self.lines.put(line)
        finally:
            self.lines.put(None)

    def receive(self):
        try:
            line = self.lines.get(timeout=10)
        except queue.Empty as error:
            raise AssertionError('actual worker command exceeded 10 seconds') from error
        assert line is not None, ('worker EOF', self.process.poll())
        return json.loads(line)

    def command(self, text):
        assert self.process.poll() is None, ('worker already exited', self.process.returncode)
        self.process.stdin.write(text + '\n')
        self.process.stdin.flush()
        return self.receive()

    def cleanup(self):
        failures = []
        if self.process.poll() is None:
            try:
                self.process.stdin.write('exit\n')
                self.process.stdin.flush()
                response = self.receive()
                assert response == {'bye': True}, response
                assert self.process.wait(timeout=5) == 0, self.process.returncode
            except BaseException as error:
                failures.append(str(error))
        if self.process.poll() is None:
            self.process.kill()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                failures.append('owned worker did not terminate after kill')
        self.reader.join(timeout=2)
        if self.reader.is_alive():
            failures.append('owned stdout reader did not reach EOF')
        else:
            self.process.stdout.close()
        self.process.stdin.close()
        if self.process.returncode not in (self.expected_exit, None):
            failures.append('worker exit ' + str(self.process.returncode))
        if failures:
            raise AssertionError('; '.join(failures))


def run(executable, contention_failures):
    rows, clients = [], []
    with tempfile.TemporaryDirectory(prefix='gesher-lock-command-') as directory:
        root = Path(directory)
        main, separate, hardlink = (root / name for name in ('main-\u05ea\u05d9\u05e7-\u6587\u4ef6', 'separate', 'hardlink'))
        main.write_bytes(b'x' * 1024)
        separate.write_bytes(b'y' * 1024)
        os.link(main, hardlink)
        paths = [str(main), str(separate), str(hardlink), str(main / 'child'), '',
                 '/dev/gesher-record-lock-absent', str(root / 'missing-directory' / 'child')]
        try:
            for unused in range(2):
                clients.append(Client(executable, paths))
            assert clients[0].process.pid != clients[1].process.pid

            def command(actor, text, expected=0, error=None):
                actual = clients[actor].command(text)
                assert actual['result'] == expected, (actor, text, actual)
                if error is not None:
                    assert actual['errno'] == error, (actor, text, actual)
                if 'canary' in actual:
                    assert actual['canary'], (actor, text, actual)
                    # Linux copy_to_user may copy a prefix before EFAULT. The
                    # exact partial struct is unspecified, not a result value.
                    if actual['result'] < 0 and actual['errno'] == 14:
                        for key in ('type', 'whence', 'start', 'length', 'pid'):
                            actual.pop(key)
                    elif actual['result'] == 0 and text.split()[2] in ('5', '36') and actual['type'] != 2:
                        if actual['pid'] > 0:
                            owners = {client.process.pid: index for index, client in enumerate(clients)}
                            assert actual['pid'] in owners, ('unknown conflict owner', actual)
                            actual['pid'] = 'actor' + str(owners[actual['pid']])
                if 'minimum' in actual:
                    assert actual['minimum'], (actor, text, actual)
                if 'contended' in actual:
                    # Retain failure while collecting all four actual cycles.
                    # The SAME Linux comparator still rejects every false row;
                    # a missing WCT chain never becomes accepted contention.
                    if not actual['contended'] or actual['stream_result'] != 0:
                        contention_failures.append(dict(actor=actor, command=text, actual=actual.copy()))
                        print('ACTUAL_CONTENTION_FAILURE ' + json.dumps(contention_failures[-1], sort_keys=True), flush=True)
                rows.append(dict(actor=actor, command=text, actual=actual))
                print('ACTUAL_COMMAND_ROW ' + json.dumps(rows[-1], sort_keys=True), flush=True)
                return actual

            def lock(actor, slot=0, cmd=6, kind=1, start=10, length=20, whence=0, pid=0,
                     pointer=0, expected=0, error=None):
                return command(actor, f'lock {slot} {cmd} {kind} {whence} {start} {length} {pid} {pointer}', expected, error)

            def query(locked, cmd=5):
                result = lock(1, cmd=cmd)
                assert result['type'] == (1 if locked else 2), result
                return result

            command(0, 'open 0 0 2')
            command(1, 'open 0 0 2')
            # Compare actual native handle inheritance, not only logical flags.
            # This checks final state; overlapping spawn atomicity is separate.
            for source_flags in (0, 1):
                command(0, f'setfd 0 {source_flags}')
                for operation, expected_flags in (('dupfd', 0), ('dupfdce', 1)):
                    assert command(0, f'{operation} 0 1 40')['minimum_offset'] == 0
                    flags = command(0, 'getfd 1 0')
                    assert flags['flags'] == expected_flags and flags['inherited'] == (expected_flags == 0)
                    command(0, 'close 1')
                    assert command(0, 'getfd 0 0')['flags'] == source_flags
            command(0, 'dupfd 0 1 8192', -1, 22)
            command(0, 'dupfd 31 1 8192', -1, 9)
            assert command(0, 'dupfd 0 1 8191')['minimum_offset'] == 0
            command(0, 'close 1')
            lock(0)
            query(True)
            lock(1, expected=-1, error=11)
            command(1, 'io 0')  # Advisory exclusion never forbids ordinary I/O.

            # dup2 closes a DIFFERENT fd even if both reference one description.
            command(0, 'dup 0 1 0')
            command(0, 'dup2 0 1')
            query(False)
            lock(0, cmd=37)
            command(0, 'dup2 0 1')
            query(True)
            command(0, 'close 0')
            query(True)
            command(0, 'close 1')
            query(False)
            command(0, 'open 0 0 2')

            # F_DUPFD housekeeping must not erase POSIX locks. The Windows
            # implementation avoids a temporary alias/close altogether.
            lock(0)
            command(0, 'dupfd 0 1 -1', -1, 22)
            query(True)
            command(0, 'dupfd 0 1 40')
            query(True)
            command(0, 'dup2 31 1', -1, 9)
            query(True)
            command(0, 'close 1')
            query(False)

            # Hardlinks share file identity; independent descriptions do not
            # share OFD ownership. Same-process POSIX/OFD locks still conflict.
            command(0, 'open 2 2 2')
            lock(0, cmd=37)
            assert lock(0, cmd=36)['type'] == 2  # Same-description exclusion.
            other_description = lock(0, slot=2, cmd=36)
            assert other_description['type'] == 1 and other_description['pid'] == -1
            lock(0, slot=2, expected=-1, error=11)
            command(0, 'close 0')
            query(False)
            lock(0, slot=2)
            command(0, 'open 3 1 2')
            command(0, 'close 3')
            query(True)
            command(0, 'open 0 0 2')
            command(0, 'close 0')
            query(False)
            command(0, 'close 2')

            # Access and validation errors preserve actual user input fields.
            command(0, 'open 0 0 0')
            lock(0, expected=-1, error=9)
            lock(0, kind=0)
            lock(0, kind=2)
            command(0, 'close 0')
            command(0, 'open 0 0 1')
            lock(0, kind=0, expected=-1, error=9)
            lock(0, cmd=5)
            command(0, 'close 0')
            command(0, 'open 0 0 2')
            lock(0, cmd=37, pid=1, expected=-1, error=22)
            lock(0, whence=99, expected=-1, error=22)
            lock(0, start=0, length=-1, expected=-1, error=22)
            lock(0, start=9223372036854775807, length=2, expected=-1, error=75)
            lock(0, start=30, length=-20)
            query(True)
            lock(0, kind=2, start=0, length=0)
            lock(0, start=10, length=0)
            query(True)
            lock(0, kind=2, start=0, length=0)

            # A high pointer is real, not a casted low-address canary. Stable
            # READONLY/cross-page/no-access output failures must not mutate the
            # registry or change an existing lock. Protection races stay open.
            lock(0)
            for pointer in (1, 2, 3, 4, 5):
                lock(1, cmd=5, pointer=pointer, expected=-1, error=14)
                query(True)
            for pointer in (2, 3):
                lock(1, start=100, pointer=pointer)
                lock(1, kind=2, start=100)
            for pointer in (1, 4, 5):
                lock(1, start=100, pointer=pointer, expected=-1, error=14)
            lock(0, kind=2, start=0, length=0)
            query(False)
            lock(1, cmd=5, pointer=2, expected=-1, error=14)
            unchanged = lock(1, cmd=5, start=123, length=456, whence=0, pid=789)
            assert unchanged['type'] == 2 and unchanged['start'] == 123 and unchanged['length'] == 456 and unchanged['pid'] == 789
            command(0, 'close 0')

            # Actual failed freopen closes the original regular stream, even
            # when an empty/unsupported destination fails before a CRT reopen.
            # An actual successful Unicode reopen retains the mapped path and
            # performs POSIX any-close cleanup before a fresh stream is used.
            command(0, 'stream 0 0 2')
            lock(0)
            query(True)
            command(0, 'freopen 0 0')
            query(False)
            command(0, 'fclose 0')
            for destination in (3, 4, 5, 6):
                command(0, 'stream 0 0 2')
                lock(0)
                query(True)
                command(0, f'freopen 0 {destination}', -1)
                query(False)

            # Force a real stream lock contention, then close a different fd
            # while holding that stream lock. Externally bounded by receive.
            for unused in range(4):
                command(0, 'stream 0 0 2')
                command(0, 'open 1 0 2')
                lock(0)
                command(0, 'contended_stdio 0 1')
                query(False)
            # A real owner process exits without any library/stdio cleanup.
            # Distinct ranges cover process locks and private-anchor OFD locks.
            command(0, 'open 0 0 2')
            lock(0)
            lock(0, cmd=37, start=100)
            query(True)
            assert lock(1, cmd=36, start=100)['type'] == 1
            clients[0].process.stdin.write('crash\n')
            clients[0].process.stdin.flush()
            clients[0].expected_exit = 23
            assert clients[0].process.wait(timeout=5) == 23
            rows.append(dict(actor=0, command='crash', actual={'exit': 23}))
            print('ACTUAL_COMMAND_ROW ' + json.dumps(rows[-1], sort_keys=True), flush=True)
            query(False)
            assert lock(1, cmd=36, start=100)['type'] == 2
            command(1, 'close 0')
        finally:
            failures = []
            for client in clients:
                try:
                    client.cleanup()
                except BaseException as error:
                    failures.append(str(error))
            if failures:
                raise AssertionError('all owned worker cleanup attempted: ' + '; '.join(failures))
    return rows


def main():
    assert os.environ.get('GITHUB_ACTIONS') == 'true', 'CI-only fixture execution'
    parser = argparse.ArgumentParser()
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--duplicate-worker', type=Path)
    parser.add_argument('--broker', type=Path)
    args = parser.parse_args()
    failures = []
    try:
        contention_failures = []
        rows = run(args.worker.resolve(), contention_failures)
        assert len(rows) == 161, ('missing required operation rows', len(rows))
        actual = dict(schema=1, scope='nonblocking common-layer command worker', rows=rows)
        args.output.write_text(json.dumps(actual, indent=2) + '\n')
        mismatches = []
        if args.reference:
            reference = json.loads(args.reference.read_text())
            assert reference['schema'] == actual['schema']
            assert len(reference['rows']) == len(rows), 'reference row count differs'
            for index, (expected, observed) in enumerate(zip(reference['rows'], rows)):
                if expected != observed:
                    mismatches.append((index, expected, observed))
                    print('ACTUAL_COMMAND_MISMATCH ' + json.dumps(dict(index=index, expected=expected, observed=observed), sort_keys=True), flush=True)
        if mismatches or contention_failures:
            print('ACTUAL_COMMAND_ORACLE_FAILURE rows=' + str(len(rows)) + ' mismatches=' + str(len(mismatches)) +
                  ' contention_failures=' + str(len(contention_failures)), flush=True)
            raise AssertionError(('strict operation/stdio evidence failed', mismatches, contention_failures))
        print('ACTUAL_COMMAND_ORACLE rows=' + str(len(rows)) + ' mismatches=0', flush=True)
    except Exception as error:
        traceback.print_exc()
        failures.append('actual commands: ' + str(error))
    if args.duplicate_worker:
        try:
            assert args.broker, 'Actual owned broker executable is required'
            from duplicate_oracle import run as run_duplicates
            duplicates = run_duplicates(args.duplicate_worker.resolve(strict=True), args.broker.resolve(strict=True))
            args.output.with_name('windows-duplicates.json').write_text(json.dumps(duplicates, indent=2) + '\n')
        except Exception as error:
            traceback.print_exc()
            failures.append('actual duplicate resources: ' + str(error))
    if failures:
        raise AssertionError('; '.join(failures))


if __name__ == '__main__':
    main()
