"""Bind an explicitly insecure, CI-only Web4 jar to its production base and patch."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import zipfile

FILES = ["app/java/com/gesher/app/webview/EngineListenerCoordinator.java",
         "app/java/com/gesher/app/webview/GesherWebViewProvider.java",
         "app/java/com/gesher/app/webview/PageScripts.java",
         "tests/windows/tracks/54-web3.ps1",
         "tests/windows/web3/Web3Servers.ps1",
         "tests/apps/web3app/java/com/gesher/web3app/FrameActivity.java"]
MARKER = b"web4 CDP experiment active"
CLASS = b"EngineListenerCoordinator"

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def source_digest(path):
    return hashlib.sha256(Path(path).read_bytes().replace(b"\r\n", b"\n")).hexdigest()

def markers(path):
    with zipfile.ZipFile(path) as jar:
        dex = b''.join(jar.read(n) for n in jar.namelist() if n.endswith('.dex'))
    return MARKER in dex, CLASS in dex

def git(*args):
    return subprocess.check_output(['git', *args], text=True).strip()

p = argparse.ArgumentParser()
p.add_argument('mode', choices=['capture', 'verify', 'production-only'])
p.add_argument('--production', required=True)
for name in ['experimental', 'patch', 'manifest', 'apk']:
    p.add_argument('--' + name)
a = p.parse_args()
if a.mode == 'production-only':
    if any(markers(a.production)):
        raise SystemExit('FAIL insecure Web4 class or marker in production')
    print('PASS production excludes insecure Web4 class and marker sha256=' + digest(a.production))
    raise SystemExit(0)
if not all((a.experimental, a.patch, a.manifest, a.apk)):
    p.error('capture/verify require all roles')
if any(markers(a.production)) or not all(markers(a.experimental)):
    raise SystemExit('FAIL Web4 role marker or role swap')
record = dict(role='insecure-characterization-only', head=git('rev-parse', 'HEAD'),
              patch=source_digest(a.patch), production=digest(a.production),
              experimental=digest(a.experimental), apk=digest(a.apk), sources={f: source_digest(f) for f in FILES})
if record['production'] == record['experimental']:
    raise SystemExit('FAIL Web4 roles identical')
if a.mode == 'capture':
    record['compiler_binary_path'] = str(Path(shutil.which('javac')).resolve())
    record['compiler_binary_sha256'] = digest(record['compiler_binary_path'])
    record['build_script'] = source_digest('app/build.sh')
    record['invocation'] = 'app/build.sh out/web4-characterization --framework-classes out/framework/classes --core-classes out/coremod'
    record['compiler'] = subprocess.check_output(['javac', '-version'], stderr=subprocess.STDOUT, text=True).strip()
    record['run'] = os.environ['GITHUB_RUN_ID']
    record['attempt'] = os.environ['GITHUB_RUN_ATTEMPT']
    Path(a.manifest).write_text(json.dumps(record, sort_keys=True, indent=2) + '\n')
    print('CAPTURED Web4 isolated producer:', json.dumps(record, sort_keys=True))
else:
    expected = json.loads(Path(a.manifest).read_text())
    if expected.get('build_script') != source_digest('app/build.sh') or expected.get('invocation') != 'app/build.sh out/web4-characterization --framework-classes out/framework/classes --core-classes out/coremod' or len(expected.get('compiler_binary_sha256', '')) != 64:
        raise SystemExit('FAIL Web4 compiler/build identity')
    for key, value in record.items():
        if expected.get(key) != value:
            raise SystemExit('FAIL Web4 role identity: ' + key)
    if expected['run'] != os.environ['GITHUB_RUN_ID'] or expected['attempt'] != os.environ['GITHUB_RUN_ATTEMPT']:
        raise SystemExit('FAIL Web4 run/attempt mismatch')
    print('PASS Web4 isolated insecure role:', json.dumps(expected, sort_keys=True))
