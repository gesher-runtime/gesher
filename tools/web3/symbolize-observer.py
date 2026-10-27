#!/usr/bin/env python3
"""Analysis only in CI: original failed native PCs and their exact producer map."""
import argparse
import bisect
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

SOURCE_RUN = 40512344714
SOURCE_SHA = '96b12ab6f9b5146870c7a373e337f762010eb6f1'
OBSERVATION_RUN = 40550620230
OBSERVATION_SHA = 'f4b5bdf54a655c0bc6675d6916e64f2b54115ff2'
OBSERVATION_JOB = 122353154161
REPOSITORY = 'gesher-runtime/gesher'


def read_json(name):
    return json.loads(Path(name).read_text(encoding='utf-8'))


def pin():
    producer = read_json('producer.json')
    observation = read_json('observation.json')
    observed_job = read_json('observation-job.json')
    assert producer['id'] == SOURCE_RUN and producer['head_sha'] == SOURCE_SHA
    assert producer['run_attempt'] == 1 and producer['status'] == 'completed'
    assert producer['name'] == 'ci' and producer['conclusion'] == 'failure'
    assert producer['repository']['full_name'] == REPOSITORY
    assert producer['head_repository']['id'] == producer['repository']['id']
    assert observation['id'] == OBSERVATION_RUN and observation['head_sha'] == OBSERVATION_SHA
    assert observation['status'] == 'completed' and observation['conclusion'] == 'failure'
    assert observation['run_attempt'] == 1 and observation['name'] == 'web3-probes'
    assert observation['repository']['id'] == producer['repository']['id']
    assert observed_job['id'] == OBSERVATION_JOB and observed_job['run_id'] == OBSERVATION_RUN
    assert observed_job['head_sha'] == OBSERVATION_SHA and observed_job['run_attempt'] == 1
    assert observed_job['name'] == 'runtime (windows-2025)' and observed_job['conclusion'] == 'failure'
    jobs = read_json('producer-jobs.json')
    assert jobs['total_count'] <= 100
    build = [row for row in jobs['jobs'] if row['name'] == 'build']
    assert len(build) == 1 and build[0]['conclusion'] == 'success'
    assert build[0]['run_id'] == SOURCE_RUN and build[0]['head_sha'] == SOURCE_SHA
    assert build[0]['run_attempt'] == 1
    inventory = read_json('producer-artifacts.json')
    assert inventory['total_count'] <= 100
    selected = [row for row in inventory['artifacts'] if row['name'] == 'windows-x64']
    assert len(selected) == 1
    artifact = selected[0]
    assert not artifact['expired'] and artifact['id'] > 0 and artifact['size_in_bytes'] > 0
    assert re.fullmatch(r'sha256:[0-9a-f]{64}', artifact['digest'])
    assert artifact['workflow_run']['id'] == SOURCE_RUN
    assert artifact['workflow_run']['head_sha'] == SOURCE_SHA
    assert artifact['workflow_run']['repository_id'] == producer['repository']['id']
    log = Path('observation.log').read_text(encoding='utf-8')
    identity = f'WEB3_SOURCE artifact=windows-x64 id={artifact["id"]} digest={artifact["digest"]} run={SOURCE_RUN} SHA={SOURCE_SHA} attempt=1'
    assert identity in log, 'Observed runtime did not identify this exact immutable artifact'
    original = log.index('INFO  web3: original teardown deadline FAILED for the Gesher device: web3app.apk\'s A11yActivity')
    assert original < log.index('WEB3_OBSERVER_BEGIN pid=7400')
    with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
        output.write(f'runtime={artifact["id"]}\n')
    print(f'WEB3_SYMBOL_SOURCE artifact={artifact["id"]} digest={artifact["digest"]} source={SOURCE_RUN}/{SOURCE_SHA} observation={OBSERVATION_RUN}/{OBSERVATION_JOB}/{OBSERVATION_SHA}')


def analyze():
    runtime = Path('original-runtime')
    binary = runtime / 'dalvikvm.exe'
    linker_map = runtime / 'dalvikvm.map'
    for member in (binary, linker_map):
        assert member.is_file() and member.stat().st_size > 0, f'Missing original member {member}'
        print(f'WEB3_SYMBOL_MEMBER name={member.name} sha256={hashlib.sha256(member.read_bytes()).hexdigest()} size={member.stat().st_size} exact-original-artifact=true')
    # No rebuilt module or generated map is allowed. Both members are selected
    # from the exact artifact identity the failed job actually downloaded.
    spec = importlib.util.spec_from_file_location('gesher_symbolize', 'tools/symbolize.py')
    symbols = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(symbols)
    base, addresses, names = symbols.load_map(linker_map)
    assert addresses, 'Original linker map has no symbols'
    log = Path('observation.log').read_text(encoding='utf-8')
    block = log.split('WEB3_OBSERVER_BEGIN pid=7400', 1)[1].split('WEB3_OBSERVER_END pid=7400', 1)[0]
    selected = []
    keep = False
    for line in block.splitlines():
        match = re.search(r'waits \(thread (\d+)\):', line)
        if match:
            keep = int(match.group(1)) in (2304, 6092, 8796)
        if keep:
            selected.append(line)
    assert all(any(f'waits (thread {tid}):' in line for line in selected) for tid in (2304, 6092, 8796))
    resolved = []
    mangled = []
    for line in selected:
        match = re.search(r'dalvikvm!\+0x0 \(\+0x([0-9a-fA-F]+)\)', line)
        if match:
            rva = int(match.group(1), 16)
            index = bisect.bisect_right(addresses, base + rva) - 1
            assert index >= 0, f'PC below original map: {rva:x}'
            symbol = names[index]
            mangled.append(symbol)
            line += f' ORIGINAL_MAP {symbol}+0x{base + rva - addresses[index]:x}'
        resolved.append(line)
    demangler = shutil.which('c++filt') or shutil.which('llvm-cxxfilt')
    assert demangler, 'No symbol demangler available'
    demangled = subprocess.run([demangler], input='\n'.join(mangled), text=True,
                               capture_output=True, timeout=10, check=True).stdout.splitlines()
    text = '\n'.join(resolved)
    for original, readable in zip(mangled, demangled):
        text = text.replace(original, readable)
    print('WEB3_SYMBOL_CONFIDENCE exact-original-artifact binary/map; nearest preceding linker symbol+offset; no PDB source-line claim')
    print(text)


if __name__ == '__main__':
    assert os.environ.get('GITHUB_ACTIONS') == 'true', 'This analysis consumes artifacts only inside CI'
    parser = argparse.ArgumentParser()
    parser.add_argument('--pin', action='store_true')
    parser.add_argument('--analyze', action='store_true')
    options = parser.parse_args()
    assert options.pin != options.analyze
    pin() if options.pin else analyze()
