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
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'native3-locks'))
from duplicate_oracle import OwnedJob
from trusted_binder import Bootstrap, Context

MAX_REQUEST = 65536


def main():
    if os.name != 'nt' or os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('SG fixture owner requires CI Windows')
    children = {}
    outputs = []
    if len(sys.argv) != 2:
        raise ValueError('owner log directory required')
    directory = Path(sys.argv[1])
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
                output = open(directory / (str(next_id) + '.log'), 'x+b')
                outputs.append(output)
                env = dict(os.environ, **request['environment'])
                fault = request.get('fixture_fault', '')
                inject = fault_callback(fault)
                before = inject if fault.startswith('before-publish-') else None
                child = owner.begin(request['broker'], request['endpoint'], output, env, before_resume=before)
                if fault.startswith('after-resume-'):
                    inject(child)
                started = True
            elif operation == 'spawn':
                if not started:
                    raise ValueError('broker not started')
                output = open(directory / (str(next_id) + '.log'), 'x+b')
                outputs.append(output)
                env = dict(os.environ, **request['environment'])
                role = request['role']
                if role not in ('@servicemanager', '@system_server'):
                    raise ValueError('unrecognized fixture role')
                env['GESHER_BINDER_UID'] = '1000'
                context = Context(magic=0x474c4358, version=3, uid=1000, server=owner.broker.pid)
                fault = request.get('fixture_fault', '')
                inject = fault_callback(fault)
                before = inject if fault.startswith('before-publish-') else None
                child = owner.spawn(request['args'], output, env, context, role=role, before_publish=before)
                if fault.startswith('after-resume-'):
                    inject(child)
            elif operation == 'read':
                child = children[request['id']]
                offset = request['offset']
                if not isinstance(offset, int) or offset < 0:
                    raise ValueError('invalid offset')
                child.stdout.flush()
                # A distinct file handle avoids moving the writer's file position.
                with open(child.stdout.name, 'rb') as reader:
                    reader.seek(offset)
                    data = reader.read(32768)
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
            print(json.dumps({'ok': True, 'id': child_id, 'pid': child.pid}), flush=True)
    finally:
        # Existing owner performs exact-handle termination, waits and closure.
        # The original job emits its audit to stderr so stdout stays framed.
        try:
            owner.close()
        finally:
            with contextlib.redirect_stdout(sys.stderr):
                job.__exit__(None, None, None)
            for output in outputs:
                output.close()


if __name__ == '__main__':
    main()
