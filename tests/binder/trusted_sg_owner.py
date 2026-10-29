"""CI-only retained-handle SG fixture owner; protocol over inherited stdin/stdout.

Only the trusted test parent holds the input pipe. Guest processes receive no
pipe or private broker endpoint. This controller is not a production authority.
"""
import base64
import contextlib
import json
import os
from pathlib import Path
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'native3-locks'))
from duplicate_oracle import OwnedJob
from trusted_binder import Bootstrap, Context

MAX_REQUEST = 65536
MAX_OUTPUT = 16 * 1024 * 1024


class Capture:
    def __init__(self):
        read_fd, write_fd = os.pipe()
        try:
            self.reader = os.fdopen(read_fd, 'rb', buffering=0)
        except BaseException:
            os.close(read_fd)
            os.close(write_fd)
            raise
        try:
            self.writer = os.fdopen(write_fd, 'wb', buffering=0)
        except BaseException:
            self.reader.close()
            os.close(write_fd)
            raise
        self.data = bytearray()
        self.lock = threading.Lock()
        self.error = None
        self.thread = threading.Thread(target=self.drain, daemon=True)
        try:
            self.thread.start()
        except BaseException:
            self.reader.close()
            self.writer.close()
            raise

    def drain(self):
        try:
            while True:
                chunk = self.reader.read(32768)
                if not chunk:
                    break
                with self.lock:
                    if len(self.data) + len(chunk) > MAX_OUTPUT:
                        raise ValueError('SG output exceeds bound')
                    self.data.extend(chunk)
        except Exception as error:
            self.error = error
        finally:
            self.reader.close()

    def read(self, offset, complete):
        if complete:
            self.thread.join(5)
            if self.thread.is_alive():
                raise RuntimeError('SG output pipe retained after child exit')
        if self.error:
            raise RuntimeError('SG output drain failed') from self.error
        with self.lock:
            return bytes(self.data[offset:offset + 32768])


def main():
    if os.name != 'nt' or os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('SG fixture owner requires CI Windows')
    children = {}
    captures = {}
    all_captures = []
    def make_capture():
        capture = Capture()
        try:
            all_captures.append(capture)
        except BaseException:
            capture.writer.close()
            capture.thread.join(5)
            if capture.thread.is_alive():
                raise RuntimeError('unregistered capture did not terminate')
            raise
        return capture
    if len(sys.argv) != 1:
        raise ValueError('owner takes no arguments')
    next_id = 1
    job = OwnedJob().__enter__()
    owner = Bootstrap(job)
    started = False
    def fault_callback(fault):
        if fault not in ('', 'before-publish-crash', 'before-publish-hang',
                         'after-resume-crash', 'after-resume-hang'):
            raise ValueError('unknown fixture fault')
        def inject(child):
            print(f'SG_OWNER_FAULT phase={fault} pid={child.pid}', file=sys.stderr, flush=True)
            if fault.endswith('crash'):
                os._exit(97)
            time.sleep(30)
        return inject
    try:
        while True:
            line = sys.stdin.buffer.readline(MAX_REQUEST + 1)
            if not line:
                break
            if len(line) > MAX_REQUEST or not line.endswith(b'\n'):
                raise ValueError('oversized or truncated request')
            request = json.loads(line)
            operation = request['op']
            if operation == 'close':
                response = {'ok': True}
                print(json.dumps(response), flush=True)
                break
            if operation == 'begin':
                if started:
                    raise ValueError('duplicate broker')
                env = dict(os.environ, **request['environment'])
                fault = request.get('fixture_fault', '')
                inject = fault_callback(fault)
                before = inject if fault.startswith('before-publish-') else None
                capture = make_capture()
                if request.get('capture_fault') == 'after-start':
                    capture.writer.close()
                    print('SG_CAPTURE_FAULT after-start=1', file=sys.stderr, flush=True)
                    raise RuntimeError('injected capture setup failure')
                try:
                    child = owner.begin(request['broker'], request['endpoint'], capture.writer, env, before_resume=before)
                finally:
                    capture.writer.close()
                if fault.startswith('after-resume-'):
                    inject(child)
                started = True
            elif operation == 'spawn':
                if not started:
                    raise ValueError('broker not started')
                if next_id > 3:
                    raise ValueError('fixture permits exactly two actors')
                env = dict(os.environ, **request['environment'])
                role = request['role']
                if role not in ('@servicemanager', '@system_server'):
                    raise ValueError('unrecognized fixture role')
                env['GESHER_BINDER_UID'] = '1000'
                context = Context(magic=0x474c4358, version=3, uid=1000, server=owner.broker.pid)
                fault = request.get('fixture_fault', '')
                inject = fault_callback(fault)
                before = inject if fault.startswith('before-publish-') else None
                capture = make_capture()
                try:
                    child = owner.spawn(request['args'], capture.writer, env, context, role=role, before_publish=before)
                finally:
                    capture.writer.close()
                if fault.startswith('after-resume-'):
                    inject(child)
            elif operation == 'read':
                child = children[request['id']]
                offset = request['offset']
                if not isinstance(offset, int) or offset < 0:
                    raise ValueError('invalid offset')
                data = captures[request['id']].read(offset, child.poll() is not None)
                print(json.dumps({'ok': True, 'data': base64.b64encode(data).decode('ascii'),
                                  'next': offset + len(data)}), flush=True)
                continue
            elif operation in ('status', 'terminate'):
                child = children[request['id']]
                if operation == 'terminate' and child.poll() is None:
                    child.kill()
                code = child.poll()
                response = {'ok': True, 'pid': child.pid, 'exit': code}
                print(json.dumps(response), flush=True)
                continue
            else:
                raise ValueError('unknown operation')
            child_id = next_id
            next_id += 1
            children[child_id] = child
            captures[child_id] = capture
            print(json.dumps({'ok': True, 'id': child_id, 'pid': child.pid}), flush=True)
    finally:
        # Existing owner performs exact-handle termination, waits and closure.
        # The original job emits its audit to stderr so stdout stays framed.
        try:
            owner.close()
        finally:
            with contextlib.redirect_stdout(sys.stderr):
                job.__exit__(None, None, None)
            for capture in all_captures:
                capture.thread.join(5)
                if capture.thread.is_alive() or capture.error:
                    raise RuntimeError('SG output cleanup incomplete')
            if all_captures:
                print(f'SG_CAPTURE_CLEANUP drains={len(all_captures)} joined=1', file=sys.stderr, flush=True)


if __name__ == '__main__':
    main()
