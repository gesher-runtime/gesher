#!/usr/bin/env python3
"""Bind the original Notes scenario to an explicit native observation bridge."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('original_notes_diagnostic',
    ROOT / 'tools/qa-notes-main28b/diagnostic.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
NEW = {'.github/workflows/qa-notes-native-phase.yml', *(
    'tools/qa-notes-native-phase/' + name for name in
    ('build.py', 'diagnostic.py', 'overlay.py', 'phase_trace.h', 'run.ps1'))}
base.DIAGNOSTIC |= NEW
base.SCOPES += ['tools/qa-notes-native-phase', '.github/workflows/qa-notes-native-phase.yml',
                'cmake/webview.cmake', 'cmake/webview2_sdk.cmake',
                'cmake/toolchain-llvm-mingw.cmake', 'CMakeLists.txt', 'patches/libnativehelper', 'aosp.lock']
ORIGINAL_BRIDGE = '10ba637f8d360446e5d8908d986676b8bb0486f8d30b159731084b570532bc5d'

# The reused functions call the same strict proof, including the new source.
original_proof = base.source_proof
def source_proof():
    proof = original_proof()
    parent = 'a03920f3705c60c0f57a22f247d9c7e675a13c75'
    if set(base.git('diff', '--name-only', parent, 'HEAD').splitlines()) != NEW:
        raise ValueError('Only the six new phase diagnostic files may differ')
    for p in base.DIAGNOSTIC - NEW:
        if base.git('show', f'{parent}:{p}', raw=True) != (ROOT / p).read_bytes():
            raise ValueError('Original diagnostic changed: ' + p)
    return proof
base.source_proof = source_proof

def bridge_input(out):
    sys.path.insert(0, str(ROOT / 'tools/ci'))
    import artifact_receipt as r
    value = r.context()
    def selected():
        r.check_run(value)
        job = r.select_producer(value, r.read_jobs(value), 'build')
        items = r.read_artifacts(f"repos/{value['repo']}/actions/runs/{value['run_id']}/artifacts",
                                 name='notes-native-phase-bridges')
        item = r.select_supplier(value, items, 'notes-native-phase-bridges', job)
        return job, item
    before = selected()
    r.download(before[1], out / 'bridges')
    after = selected()
    if r.supplier_identity(before) != r.supplier_identity(after):
        raise ValueError('Bridge producer changed during download')
    directory = out / 'bridges'
    if {p.name for p in directory.iterdir()} != {'producer.json', 'rebuilt-libgesher_webview_bridge.so',
                                               'trace-libgesher_webview_bridge.so'}:
        raise ValueError('Missing or unexpected bridge payload')
    producer = json.loads((directory / 'producer.json').read_text())
    if (producer['source'] != source_proof() or producer['run'] != value['run_id']
            or producer['attempt'] != value['attempt'] or producer['producer_job'] != 'build'):
        raise ValueError('Bridge actual source/run/attempt producer mismatch')
    for name, digest in producer['outputs'].items():
        if base.sha(directory / name) != digest:
            raise ValueError('Bridge binary hash mismatch')
    if set(producer['outputs']) != {'rebuilt-libgesher_webview_bridge.so', 'trace-libgesher_webview_bridge.so'}:
        raise ValueError('Bridge output identity mismatch')
    import overlay
    for relative in (overlay.HOST, overlay.JNI):
        original = (ROOT / relative).read_text()
        transformed, additions = overlay.apply(relative, original)
        expected = dict(original_sha256=hashlib.sha256(original.encode()).hexdigest(),
                        overlay_sha256=hashlib.sha256(transformed.encode()).hexdigest(), insertions=additions)
        if producer['overlays'].get(relative) != expected:
            raise ValueError('Observation overlay differs from reviewed transformation')
    r.receipt(value, after[1], after[0])
    (out / 'results/bridge-receipt.json').write_text(json.dumps(dict(
        source=source_proof(), producer=producer, actual_producer=after[0], artifact=after[1]), indent=2) + '\n')

def summarize(out, mode, os_name):
    # Exact original scenario, validator and 157-family guard remain fatal.
    result = base.summarize(out, 'original', os_name)
    results = out / 'results'
    invocation = json.loads((results / 'invocation.json').read_text(encoding='utf-8-sig'))
    receipt = json.loads((results / 'bridge-receipt.json').read_text())
    expected = ORIGINAL_BRIDGE if mode == 'original' else receipt['producer']['outputs'][mode + '-libgesher_webview_bridge.so']
    if (invocation['native_phase_mode'] != mode or invocation['original_bridge_sha256'] != ORIGINAL_BRIDGE
            or invocation['installed_files']['libgesher_webview_bridge.so'] != expected):
        raise ValueError('Actual selected native bridge differs')
    phases = {}
    for path in sorted((results / 'native-phases').glob('native-phase-*.jsonl')):
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        if len(rows) > 1024 or any(set(row) != {'phase', 'tick', 'pid', 'tid', 'seq', 'object', 'a', 'b', 'c', 'd'} for row in rows):
            raise ValueError('Unexpected/unbounded native observation')
        phases[path.name] = rows
    if mode == 'trace' and not any(row['phase'] == 'jni_onload' for rows in phases.values() for row in rows):
        raise ValueError('The actual observed native library did not report JNI entry')
    if mode != 'trace' and phases:
        raise ValueError('An uninstrumented bridge emitted observation-only phases')
    report = dict(mode=mode, original_scenario_exit=invocation['original_exit'], original_result=result,
                  bridge_sha256=expected, source=source_proof(), phases=phases,
                  limits=['Observation-only; no product repair or rendered-content claim',
                          'Original unmodified scenario/deadlines/coverage/retirement remain required',
                          'Rebuilt mode separates compiler-output changes from phase instrumentation',
                          'A page event alone does not prove the preview contains rendered text'])
    (results / 'native-phase-summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(mode=mode, original_result=result, phase_files=len(phases), bridge_sha256=expected)))
    return result

def main():
    p = argparse.ArgumentParser()
    p.add_argument('operation', choices=['source-proof', 'input', 'summarize'])
    p.add_argument('--out', type=Path)
    p.add_argument('--mode', choices=['original', 'rebuilt', 'trace'])
    p.add_argument('--os', dest='os_name', choices=['windows-2022', 'windows-2025'])
    a = p.parse_args()
    if a.operation == 'source-proof':
        print(json.dumps(source_proof(), indent=2)); return 0
    if a.out is None: p.error('--out is required')
    out = a.out.resolve()
    if a.operation == 'input':
        base.input(out)
        bridge_input(out)
        path = out / 'results/source-and-supplier-receipt.json'
        receipt = json.loads(path.read_text())
        receipt['purpose'] = 'Original historical installer plus separately bound selected native bridge; default Notes unchanged'
        path.write_text(json.dumps(receipt, indent=2) + '\n')
        return 0
    if not a.mode or not a.os_name: p.error('--mode and --os are required')
    return summarize(out, a.mode, a.os_name)

if __name__ == '__main__':
    raise SystemExit(main())
