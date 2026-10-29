"""CI-only observer coupling experiment; never attributes the historical overrun."""
import argparse
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import traceback
from validate_stdio_evidence import main as validate_evidence

from command_oracle import Client, run


def experiment(worker, directory, blocked, protocol):
    clients = []

    class SinkClient(Client):
        def __init__(self, executable, paths):
            self.evidence = bytearray()
            self.withheld = False
            self.cleaned = False
            self.sink_error = None
            self.drain = None
            self.process = None
            self.read_pipe = None
            self.sink = None
            self.pipe_writer = None
            self.raw_read = self.raw_write = None
            clients.append(self)  # Ownership precedes every partial setup operation.
            try:
                self.start(executable, paths)
                request = protocol / (str(self.process.pid) + '.request')
                pending = request.with_suffix('.pending')
                pending.write_text(str(self.process.pid))
                pending.replace(request)
                limit = time.monotonic() + 5
                while not (protocol / (str(self.process.pid) + '.go')).exists():
                    assert time.monotonic() < limit, 'retained supervisor did not capture child'
                    time.sleep(0.005)
            except BaseException:
                self.cleanup()
                raise

        def start(self, executable, paths):
            if blocked:
                import msvcrt
                self.raw_read, self.raw_write = os.pipe()
                read_fd, write_fd = self.raw_read, self.raw_write
                self.read_pipe = os.fdopen(read_fd, 'rb', buffering=0)
                self.raw_read = None
                sink = os.fdopen(write_fd, 'wb', buffering=0)
                self.raw_write = None
                self.pipe_writer = sink
                api = ctypes.WinDLL('kernel32', use_last_error=True).GetNamedPipeInfo
                api.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD),
                                ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
                                ctypes.POINTER(wintypes.DWORD)]
                api.restype = wintypes.BOOL
                output, capacity = wintypes.DWORD(), wintypes.DWORD()
                assert api(msvcrt.get_osfhandle(read_fd), None, ctypes.byref(output),
                           ctypes.byref(capacity), None), ctypes.get_last_error()
                actual_capacity = max(output.value, capacity.value)
                assert 0 < actual_capacity <= 4096, ('uncontrolled pipe capacity', output.value, capacity.value)
                print('STDIO_SINK_PIPE capacity=' + str(actual_capacity), flush=True)

                def drain():
                    try:
                        while True:
                            line = self.read_pipe.readline()
                            if not line:
                                break
                            self.evidence.extend(line)
                            if b'ACTUAL_STDIO_CLOCK cycle=1 phase=start ' in line:
                                self.withheld = True
                                # This adverse sink deliberately outlasts the SAME
                                # native5s deadline. Client.receive remains10s.
                                time.sleep(6)
                    except BaseException as error:
                        self.sink_error = error

                self.drain = threading.Thread(target=drain, daemon=True)
                self.drain.start()
                try:
                    super().__init__(executable, paths, stderr=sink)
                finally:
                    sink.close()  # Only the child owns the remaining writer.
                    self.pipe_writer = None
            else:
                self.sink = tempfile.TemporaryFile(mode='w+b')
                super().__init__(executable, paths, stderr=self.sink)

        def cleanup(self):
            if self.cleaned:
                return
            self.cleaned = True
            failures = []
            try:
                if self.process is not None:
                    super().cleanup()
            except BaseException as error:
                failures.append(repr(error))
            try:
                for name in ('raw_read', 'raw_write'):
                    descriptor = getattr(self, name)
                    if descriptor is not None:
                        os.close(descriptor)
                        setattr(self, name, None)
                if self.pipe_writer is not None:
                    self.pipe_writer.close()
                    self.pipe_writer = None
                if self.drain:
                    self.drain.join(timeout=8)
                    assert not self.drain.is_alive(), 'owned diagnostic pipe reader did not reach EOF'
                    assert self.sink_error is None, repr(self.sink_error)
                    self.read_pipe.close()
                elif self.sink is not None:
                    self.sink.seek(0)
                    self.evidence.extend(self.sink.read())
                    self.sink.close()
                elif self.read_pipe is not None:
                    self.read_pipe.close()
            except BaseException as error:
                failures.append('diagnostic sink cleanup: ' + repr(error))
            if failures:
                raise AssertionError('; '.join(failures))

    failure = None
    try:
        rows = run(worker, [], client_factory=SinkClient)
        assert len(rows) == 161, len(rows)
    except BaseException as error:
        failure = error
        traceback.print_exc()
    finally:
        for client in clients:
            # run() must have performed actual bounded owned cleanup itself.
            assert client.cleaned
            if client.process is not None:
                assert client.process.returncode is not None and not client.reader.is_alive()
    evidence = b''.join(client.evidence for client in clients)
    route = 'blocked-pipe' if blocked else 'direct-file'
    path = directory / (route + '.stderr.txt')
    path.write_bytes(evidence)
    # Preserve every actual native line for direct completed-log audit.
    sys.stderr.write(evidence.decode('utf-8', errors='strict'))
    saved_args = sys.argv
    accepted = True
    try:
        sys.argv = ['validate_stdio_evidence.py', '--stderr', str(path), '--output', str(directory / (route + '.json'))]
        validate_evidence()
    except AssertionError:
        accepted = False
        traceback.print_exc()
    finally:
        sys.argv = saved_args
    if blocked:
        assert failure is not None, 'deliberate blocked observer unexpectedly passed'
        assert any(client.withheld for client in clients), 'sink was never withheld within stdio budget'
        assert [client.process.returncode for client in clients] == [94, 0]
        assert b'phase=fatal94-' in evidence, 'exact worker fatal source evidence missing'
        assert not accepted, 'strict witness validator accepted the adverse case'
    else:
        assert failure is None, repr(failure)
        assert [client.process.returncode for client in clients] == [23, 0]
        assert accepted, 'direct-file witness failed unchanged strict validator'
    print('STDIO_SINK_RESULT route=' + route + ' expected=' + ('strict-reject' if blocked else 'strict-pass') +
          ' actual=true owned_reaped=true reader_eof=true historical_cause_proved=false', flush=True)


def main():
    assert os.environ.get('GITHUB_ACTIONS') == 'true' and os.name == 'nt', 'Windows CI-only native fixture'
    parser = argparse.ArgumentParser()
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--job-protocol', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(exist_ok=True)
    experiment(args.worker.resolve(strict=True), args.output, blocked=False, protocol=args.job_protocol)
    experiment(args.worker.resolve(strict=True), args.output, blocked=True, protocol=args.job_protocol)


if __name__ == '__main__':
    main()
