"""Read-only lexical inventory; input must already be independently verified.

Usage: python source_coverage_inventory.py C:/path/to/expanded-supplier
Requires Universal Ctags with JSON support. This is evidence for review, not an
export list or proof of preprocessed call-graph coverage.
"""
import csv
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
ROOT = Path(sys.argv[1]).resolve()
WIN = ROOT / 'mingw-w64-libraries/winpthreads'
DEST = REPO / 'docs/handoff/tracks'

def tags_for(paths):
    output = subprocess.check_output(['ctags', '--output-format=json', '--fields=+ne',
                                      '--kinds-C=+p', '-o', '-'] + [str(p) for p in paths], text=True)
    return [json.loads(line) for line in output.splitlines()]

functions = [t for t in tags_for(sorted((WIN / 'src').glob('*.c'))) if t.get('kind') == 'function']
names = {t['name'] for t in functions}
# Manually reviewed bodies: only caller-provided attribute storage and memcpy/
# scalar operations; cross-image structure layout still needs qualification.
LOCAL_VALUE_ONLY = {'pthread_attr_init', 'pthread_attr_destroy', 'pthread_attr_getstack',
                    'pthread_attr_setstack', 'pthread_attr_getstackaddr', 'pthread_attr_setstackaddr',
                    'pthread_attr_getstacksize', 'pthread_attr_setstacksize'}
rows = []
for tag in functions:
    path = Path(tag['path'])
    lines = path.read_text(errors='replace').splitlines()
    body = '\n'.join(lines[tag['line'] - 1:tag.get('end', tag['line'])])
    calls = sorted(set(re.findall(r'\b([A-Za-z_][A-Za-z_0-9]*)\s*\(', body)) - {tag['name']})
    calls = [x for x in calls if x in names or x.startswith(('pthread_', '_pthread_', '__pthread_', 'gesher_'))]
    status = ('LOCAL_VALUE_ONLY' if path.name == 'thread.c' and tag['name'] in LOCAL_VALUE_ONLY else
              'PROBE_OWNER_DISPATCH' if ('identity_probe_owner()' in body and 'owner->' in body)
              or 'gesher_probe_route_' in body else
              'CLOCK_AUTHORITY' if any(x in body for x in ('clock_anchor()', 'cpu_clock_read(')) else 'REVIEW')
    flags = []
    for name, pattern in [('TLS', r'_pthread_tls|Tls(Get|Set)Value|__pthread_self_lite|pthread_self\s*\('),
                          ('CLEANUP', r'pthread_cleanup_(push|pop)|pthread_getclean|_pthread_cleanup_dest'),
                          ('CANCEL', r'pthread_testcancel|__pthread_shallcancel|_pthread_invoke_cancel'),
                          ('DIRECT_KEY', r'_pthread_key_dest|_pthread_key_lock|gesher_key_'),
                          ('LOADER', r'LoadLibrary|FreeLibrary|GetModuleHandle|DLL_(PROCESS|THREAD)')]:
        if re.search(pattern, body):
            flags.append(name)
    rows.append({'file': path.name, 'line': tag['line'], 'end': tag.get('end', ''),
                 'symbol': tag['name'], 'linkage': 'internal' if tag.get('file') else 'external-or-conditional',
                 'status': status, 'direct_flags': ','.join(flags) or '-', 'direct_calls': ','.join(calls) or '-'})
rows.sort(key=lambda x: (x['file'], x['line'], x['symbol']))
with (DEST / 'pthread-v6-source-function-matrix.tsv').open('w', encoding='utf-8', newline='') as f:
    writer = csv.DictWriter(f, fieldnames=['file', 'line', 'end', 'symbol', 'linkage', 'status', 'direct_flags', 'direct_calls'],
                            delimiter='\t', lineterminator='\n')
    writer.writeheader()
    writer.writerows(rows)

headers = []
for tag in tags_for(sorted(WIN.rglob('*.h'))):
    name, kind = tag.get('name', ''), tag.get('kind', '')
    if kind not in ('prototype', 'function', 'variable', 'externvar', 'macro'):
        continue
    if 'pthread' not in name.lower() and not name.lower().startswith(('clock_', 'nanosleep', 'sem_', 'sched_')):
        continue
    headers.append([Path(tag['path']).relative_to(WIN).as_posix(), tag['line'], kind, name,
                    tag.get('typeref', ''), tag.get('pattern', '')])
headers.sort(key=lambda x: (x[0], x[1], x[3]))
with (DEST / 'pthread-v6-header-symbol-matrix.tsv').open('w', encoding='utf-8', newline='') as f:
    writer = csv.writer(f, delimiter='\t', lineterminator='\n')
    writer.writerow(['file', 'line', 'kind', 'symbol', 'type_hint', 'declaration_pattern'])
    writer.writerows(headers)
entries = [hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.relative_to(ROOT).as_posix()
           for p in sorted(WIN.rglob('*')) if p.is_file() and p.suffix in ('.c', '.h', '.S')]
(DEST / 'pthread-v6-source-inputs.sha256').write_bytes(('\n'.join(entries) + '\n').encode())
print('function definitions:', len(rows), 'header tags:', len(headers), 'source inputs:', len(entries))
