from pathlib import Path
import hashlib
import json
import subprocess

controls = Path(__file__).resolve().parent
root = Path.cwd()
source = root / 'source'
baseline = root / 'baseline'
manifest = json.loads((controls / 'manifest.json').read_text(encoding='utf-8'))
for directory, expected in [(source, manifest['fixed_source']),
                            (baseline, manifest['old_source_ref'])]:
    actual = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'],
                                     text=True).strip()
    assert actual == expected, (directory, actual, expected)
    print(f'SOURCE_IDENTITY path={directory.name} source={actual}', flush=True)

def read(path):
    return path.read_text(encoding='utf-8')

def digest(text):
    return hashlib.sha256(text.encode('utf-8')).hexdigest()

for relative, expected in manifest['fixed_files'].items():
    assert digest(read(source / relative)) == expected, relative
    print('SOURCE_FILE', relative, expected, flush=True)
for name, expected in manifest['files'].items():
    assert digest(read(controls / name)) == expected, name

original = read(baseline / 'compat/devices/hello_key_win.cc')
assert digest(original) == manifest['old_native_sha256']
assert original.count('void Release(Operation* o) {') == 1
adapted = read(controls / 'hello_key_win_before.cc')
marker = '\n// Injection-only adapter for the new regression;'
assert adapted.count(marker) == 1
assert adapted.split(marker)[0] == original.replace(
    'void Release(Operation* o) {', 'void Release(Operation* o, bool) {', 1)
fixture = read(source / 'tests/biometrics/hello_key_create_windows_test.cc')
assert read(controls / 'late_create_regression.cc').startswith(
    fixture[:fixture.index('void StartupFailures()')])
print('CONTROL_VERIFIED exact prior native bodies, explicit injection/signature adapters, '
      'same current scripted COM classes; no interactive proof', flush=True)
