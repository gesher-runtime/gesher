"""CI-only bounded passive reads; parent retains admitted stage read locks."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import io

def inventory(stream, module):
    """Pure framed-byte decoder; tests supply only synthetic memory."""
    raw = stream.readline(16385)
    assert len(raw) <= 16384, 'bounded inventory request'
    request = json.loads(raw)
    assert set(request) == {'entries'} and len(request['entries']) == 3
    assert {row['name'] for row in request['entries']} == module.ADMITTED_NAMES
    for row in request['entries']:
        assert type(row['bytes']) is int and 64 <= row['bytes'] <= module.MAX_BYTES
        # Parent reads the ACTUAL retained stage FileStream from offset zero,
        # verifies its EOF sentinel, and frames those bytes. No path reopening.
        data = stream.read(row['bytes'])
        assert len(data) == row['bytes']
        assert stream.read(22) == b'\nFONT_RESOURCE_PE_END\n'
        result = module.inspect_admitted_stream(row['name'], io.BytesIO(data), row)
        yield {'name': row['name'], 'manifest': row,
                          'directories': result['directories'],
                          'descriptor_slots': result['descriptor_slots'],
                          'dependencies': [vars(item) for item in result['dependencies']],
                          'loader_closure': False}
    assert stream.read(1) == b'', 'unexpected trailing frame bytes'

def main():
    assert os.environ.get('GITHUB_ACTIONS') == 'true' and os.name == 'nt', 'Windows CI only'
    spec = importlib.util.spec_from_file_location('resource_pe', Path(__file__).with_name('resource-pe-imports.py'))
    module = importlib.util.module_from_spec(spec);sys.modules[spec.name] = module;spec.loader.exec_module(module)
    for record in inventory(sys.stdin.buffer, module):
        print(json.dumps(record, sort_keys=True), flush=True)

if __name__ == '__main__':
    main()
