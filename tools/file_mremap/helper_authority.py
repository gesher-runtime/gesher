"""Standalone helper authority. Pure functions never grant an execution lease.

CI entry points independently authenticate immutable caller, guard, current API
roles and downloaded bytes. This is not selected-production or full-A authority.
"""
import argparse
import base64
import ast
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import zipfile

BASE = '15dde556967b8a366477d3d7b3839abbcbfd42c6'
BASE_INVENTORY = 'c2bafd90c2783383db1ca6b1c73d0365ad87d91e7f35fe9c96b885a75f25225e'
REPOSITORY = 'gesher-runtime/gesher'
REPOSITORY_ID = 1426153186
BRANCH = 'wip/file-mremap-helper-qualification'
WORKFLOW = 'file-mremap-helper-component'
CALLER = '.github/workflows/file-mremap-helper-component.yml'
GUARD = '.github/workflows/file-mremap-helper-trusted-source.yml'
AUTHORITY_KEY = 'FILE_MREMAP_HELPER_CALLER_COMMIT'
PYTHON_PATHS = tuple('tools/file_mremap/' + x for x in
                     ('helper_authority.py', 'helper_producer.py', 'test_helper_authority.py'))
ADDITIONS = (CALLER, GUARD, *PYTHON_PATHS)
PROFILE = 'PRISTINE_PUBLISHER_STANDALONE_X64_UCRT'
MACROS = ('GESHER','USE_MINGW','WIN32_LEAN_AND_MEAN','NOMINMAX','NOGDI','_USE_MATH_DEFINES','__STDC_FORMAT_MACROS','__STDC_CONSTANT_MACROS','__STDC_LIMIT_MACROS','_WIN32_WINNT=0x0A00','WINVER=0x0A00','_FILE_OFFSET_BITS=64','_POSIX_THREAD_SAFE_FUNCTIONS')
COMMON = ('--no-default-config','-std=gnu++20','-mno-ms-bitfields','-fsized-deallocation','-O2','-g','-Wall','-Wextra','-Werror','-fno-exceptions',*('-D'+x for x in MACROS))
TUS = {'helper.obj': 'compat/posix/src/mman_alias_prepare.cc',
       'qualification.obj': 'tests/file_mremap/alias_prepare_test.cc',
       'caller.obj': 'tests/file_mremap/alias_prepare_noexcept_caller.cc'}
HEADER = 'compat/posix/src/mman_registry_internal.h'
SOURCE_BLOBS = {'helper.obj': 'd6765c8572c7fa4fcfd439a6be4450b7d998e34b',
                'qualification.obj': '215b460537b88827e64571e208f244e2d6f83755',
                'caller.obj': 'd4667a8c59e08ef875e7f9084fcfd2f033c76fe2'}
HEADER_BLOB = '90a729c51e7229a293df11f2902a1f16376d516b'
INPUT21 = tuple(sorted(('CMakeLists.txt', 'compat/posix/posix.cmake',
 'compat/posix/src/mman.cc', HEADER, *TUS.values(),
 'docs/handoff/tracks/file-mremap-shared-alias.md', 'tests/file_mremap/CMakeLists.txt',
 'tests/file_mremap/shared_alias.c', 'tests/file_mremap/alias_failure_adapter.cc',
 'tests/file_mremap/alias_failure_adapter.h', 'tests/file_mremap/alias_failure_test.c',
 'tests/file_mremap/lifetime_cases.c', 'tools/file_mremap/oracle.py',
 'tools/file_mremap/test_oracle.py', 'tools/file_mremap/reference.py',
 'tools/file_mremap/windows_controller.py', 'tools/file_mremap/source_contract.py',
 'tools/file_mremap/producer_receipt.py', 'tools/file_mremap/test_source_contract.py')))
MEMBERS = ('helper-qualification.exe', 'helper-qualification.map',
 'helper-qualification.imports.txt', 'helper.obj', 'qualification.obj', 'caller.obj',
 'helper.d', 'qualification.d', 'caller.d', 'compile-commands.json',
 'dependency-manifest.json', 'compiler-profile.json', 'link-transcript.txt',
 'object-symbols.json', 'source-manifest.json', 'producer-manifest.json')
FILE_CAP = 16 * 1024 * 1024
TOTAL_CAP = 64 * 1024 * 1024
ARCHIVE_CAP = 1024 * 1024 * 1024
STREAM_CAP = 256 * 1024
PROCESS_SECONDS = 60
POSTKILL_SECONDS = 5
CLEANUP_SECONDS = 10
LIVE_GRAPHS = []  # failed containment retains the complete owned graph

class Rejected(ValueError):
    pass

def require(condition, reason):
    if not condition:
        raise Rejected(reason)

def digest(raw):
    return hashlib.sha256(raw).hexdigest()

def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True) + '\n').encode()

def emit_bytes(label,raw):
    require(re.fullmatch('[A-Z_0-9]+',label) and type(raw)is bytes and len(raw)<=FILE_CAP,'proof record bound')
    parts=[raw[i:i+9000]for i in range(0,len(raw),9000)]or[b'']
    for index,part in enumerate(parts,1):
        print('FILE_MREMAP_HELPER_%s part=%d/%d bytes=%d sha256=%s base64=%s' %
              (label,index,len(parts),len(raw),digest(raw),base64.b64encode(part).decode('ascii')),flush=True)

def emit_record(label,value):
    emit_bytes(label,canonical(value))

def decode(raw, cap=FILE_CAP):
    require(type(raw) is bytes and len(raw) <= cap, 'JSON byte bound')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'duplicate JSON key')
            result[key] = value
        return result
    def finite(token):raise Rejected('nonfinite JSON number')
    return json.loads(raw, object_pairs_hook=unique,parse_constant=finite)

def hexid(value, length=40):
    require(type(value) is str and re.fullmatch('[0-9a-f]{%d}' % length, value), 'hash identity')

def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])

def inventory(root, ref):
    hexid(ref)
    result = {}
    for row in filter(None, git(root, 'ls-tree', '-r', '-z', ref).split(b'\0')):
        meta, name = row.split(b'\t', 1)
        mode, kind, blob = meta.decode().split()
        require(kind == 'blob' and mode in ('100644', '100755'), 'nonregular source')
        path = name.decode('utf-8')
        require(path not in result, 'duplicate source')
        result[path] = {'mode': mode, 'blob': blob}
    return result

def verify_inventory(base, current, allowed):
    require(digest(canonical(base)) == BASE_INVENTORY, 'frozen whole inventory')
    require(set(current) == set(base) | set(ADDITIONS), 'closed five additions')
    require(all(current[p] == v for p, v in base.items()), 'baseline/history/selected source changed')
    require(set(allowed) == set(ADDITIONS), 'five exact pins')
    require(all(current[p] == {'mode': '100644', 'blob': allowed[p]} for p in ADDITIONS), 'new source pin')

def source_manifest(root, source):
    base = inventory(root, BASE)
    require(digest(canonical(base)) == BASE_INVENTORY, 'base inventory')
    return {'source': source, 'base': BASE, 'base_inventory_sha256': BASE_INVENTORY,
            'profile': PROFILE, 'inputs21': {p: base[p] for p in INPUT21},
            'direct_sources': {p: base[p] for p in (*TUS.values(), HEADER)},
            'current_inventory_sha256': digest(canonical(inventory(root, source)))}

def caller_bytes(guard):
    hexid(guard)
    return ('''name: file-mremap-helper-component
on:
  workflow_dispatch:
  push:
    branches: [wip/file-mremap-helper-qualification]
    paths: [.github/workflows/file-mremap-helper-component.yml]
permissions:
  contents: read
  actions: read
concurrency:
  group: file-mremap-helper-${{ github.ref }}
  cancel-in-progress: false
jobs:
  source-authority:
    uses: gesher-runtime/gesher/.github/workflows/file-mremap-helper-trusted-source.yml@GUARD_SHA
  helper-producer:
    needs: source-authority
    runs-on: ubuntu-24.04
    timeout-minutes: 15
    env:
      GH_TOKEN: ${{ github.token }}
      FILE_MREMAP_HELPER_CALLER_COMMIT: ${{ vars.FILE_MREMAP_HELPER_CALLER_COMMIT }}
    steps:
      - uses: actions/checkout@v4
        with: {fetch-depth: 0, persist-credentials: false}
      - name: Authenticate source and build only three standalone helper objects
        run: python3 -I -B tools/file_mremap/helper_producer.py produce
      - uses: actions/upload-artifact@v4
        with:
          name: file-mremap-helper-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}
          path: helper-bundle/
          if-no-files-found: error
          retention-days: 3
  helper-windows:
    name: helper-${{ matrix.os }}
    needs: [source-authority, helper-producer]
    strategy:
      fail-fast: false
      matrix:
        os: [windows-2022, windows-2025]
    runs-on: ${{ matrix.os }}
    timeout-minutes: 5
    env:
      GH_TOKEN: ${{ github.token }}
      FILE_MREMAP_HELPER_CALLER_COMMIT: ${{ vars.FILE_MREMAP_HELPER_CALLER_COMMIT }}
    steps:
      - uses: actions/checkout@v4
        with: {fetch-depth: 0, persist-credentials: false}
      - name: Authenticate current producer and run only owned standalone helper
        shell: pwsh
        run: |
          python -I -B tools/file_mremap/helper_producer.py consume
          if ($LASTEXITCODE -ne 0) { throw 'Standalone helper qualification failed' }
'''.replace('GUARD_SHA', guard)).encode()

def guard_bytes(pins, template):
    require(set(pins) == set(PYTHON_PATHS), 'three Python pins')
    for value in (*pins.values(), template):
        hexid(value)
    bootstrap = """import hashlib,subprocess,sys
from pathlib import Path
PINS=PINS_LITERAL
TEMPLATE='TEMPLATE_LITERAL'
path='tools/file_mremap/helper_authority.py'
raw=subprocess.check_output(['git','cat-file','blob',PINS[path]])
assert hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\\0'+raw).hexdigest()==PINS[path]
assert sys.flags.isolated==1 and sys.dont_write_bytecode
scope={'__name__':'immutable_helper_authority','__file__':str(Path(path).resolve())}
exec(compile(raw,'immutable-helper-authority','exec'),scope)
scope['guard_main'](PINS,TEMPLATE)
""".replace('PINS_LITERAL', repr(dict(sorted(pins.items())))).replace('TEMPLATE_LITERAL', template)
    return ('''name: file-mremap-helper-trusted-source
on:
  workflow_call:
permissions:
  contents: read
  actions: read
jobs:
  verify:
    runs-on: ubuntu-24.04
    timeout-minutes: 5
    env:
      GH_TOKEN: ${{ github.token }}
      FILE_MREMAP_HELPER_CALLER_COMMIT: ${{ vars.FILE_MREMAP_HELPER_CALLER_COMMIT }}
    steps:
      - uses: actions/checkout@v4
        with: {fetch-depth: 0, persist-credentials: false}
      - name: Verify immutable standalone source and full preserved history
        shell: bash
        run: |
          python3 -I -B - <<'PY'
''' + ''.join('          ' + line + '\n' for line in bootstrap.splitlines()) + '''          PY
      - name: Run ordinary metadata controls only
        run: python3 -I -B tools/file_mremap/test_helper_authority.py
''').encode()

def constants(raw):
    import textwrap
    require(type(raw) is bytes and len(raw) <= FILE_CAP, 'guard byte bound')
    try:
        text = raw.decode().split("          python3 -I -B - <<'PY'\n", 1)[1].rsplit('\n          PY', 1)[0]
        tree = ast.parse(textwrap.dedent(text))
    except (UnicodeError, IndexError, SyntaxError) as error:
        raise Rejected('malformed immutable guard') from error
    values = {x.targets[0].id: ast.literal_eval(x.value) for x in tree.body
              if isinstance(x, ast.Assign) and isinstance(x.targets[0], ast.Name)
              and x.targets[0].id in ('PINS', 'TEMPLATE')}
    require(set(values) == {'PINS', 'TEMPLATE'}, 'guard constants')
    require(raw == guard_bytes(values['PINS'], values['TEMPLATE']), 'whole guard template')
    return values

def checked_out_hashes(root,names):
    """One real Git read for every path, retaining attributes/EOL filters."""
    require(0<len(names)<=10000 and len(set(names))==len(names),'checkout path count')
    require(all(type(name)is str and name and not name.startswith('"') and not Path(name).is_absolute() and '..'not in Path(name).parts and not any(char in name for char in '\r\n\0') for name in names),'line-delimited checkout paths')
    raw=subprocess.check_output(['git','-C',str(root),'hash-object','--stdin-paths'],input=('\n'.join(names)+'\n').encode('utf-8'))
    hashes=raw.decode('ascii').splitlines();require(len(hashes)==len(names),'complete checkout hashes')
    for value in hashes:hexid(value)
    return dict(zip(names,hashes))

def verify_source(root, source, reviewed, expected_guard=None):
    hexid(source); hexid(reviewed)
    require(source==reviewed,'exact independently reviewed source SHA')
    require(git(root, 'cat-file', '-t', reviewed).strip() == b'commit', 'authority is not a commit')
    current_caller = git(root, 'show', source + ':' + CALLER)
    require(current_caller == git(root, 'show', reviewed + ':' + CALLER), 'independent caller authority')
    matches = re.findall(rb'file-mremap-helper-trusted-source.yml@([0-9a-f]{40})', current_caller)
    require(len(matches) == 1, 'one immutable guard')
    guard = matches[0].decode(); hexid(guard)
    if expected_guard is not None:
        require(guard == expected_guard, 'wrong guard')
    raw = git(root, 'show', guard + ':' + GUARD)
    require(raw == git(root, 'show', source + ':' + GUARD) == git(root, 'show', reviewed + ':' + GUARD), 'guard selfpin')
    values = constants(raw)
    require(current_caller == caller_bytes(guard), 'whole role DAG')
    require(git(root, 'show', values['TEMPLATE'] + ':' + CALLER) == caller_bytes('0' * 40), 'zero-pin template')
    require(reviewed != values['TEMPLATE'], 'template cannot authorize caller')
    pins = dict(values['PINS']); pins[CALLER] = git(root, 'rev-parse', source + ':' + CALLER).decode().strip()
    pins[GUARD] = git(root, 'rev-parse', guard + ':' + GUARD).decode().strip()
    verify_inventory(inventory(root, BASE), inventory(root, source), pins)
    require(subprocess.run(['git', '-C', str(root), 'merge-base', '--is-ancestor', BASE, source], capture_output=True).returncode == 0, 'source ancestry')
    checked_inventory=inventory(root,source)
    # Actual Git hashing, not index/stat-cache trust. Protected bodies stay opaque.
    observed=checked_out_hashes(root,tuple(checked_inventory))
    require(all(observed[path]==row['blob']for path,row in checked_inventory.items()),'checkout/no-op changed')
    require(not git(root, 'status', '--porcelain', '--untracked-files=all').strip(), 'dirty/shadow checkout')
    return {'guard': guard, 'guard_blob': pins[GUARD], 'template': values['TEMPLATE'],
            'caller_authority': reviewed, 'pins': pins, 'manifest': source_manifest(root, source)}

def context(env, job):
    require(env.get('GITHUB_ACTIONS') == 'true', 'CI only')
    expected = {'GITHUB_REPOSITORY': REPOSITORY, 'GITHUB_REPOSITORY_ID': str(REPOSITORY_ID),
                'GITHUB_REF': 'refs/heads/' + BRANCH, 'GITHUB_WORKFLOW': WORKFLOW,
                'GITHUB_WORKFLOW_REF': REPOSITORY + '/' + CALLER + '@refs/heads/' + BRANCH,
                'GITHUB_JOB': job, 'GITHUB_ACTOR': 'palmoni5', 'GITHUB_TRIGGERING_ACTOR': 'palmoni5'}
    require(all(env.get(k) == v for k, v in expected.items()), 'dedicated caller context')
    require(env.get('GITHUB_EVENT_NAME') in ('push', 'workflow_dispatch'), 'event')
    source = env.get('GITHUB_SHA'); hexid(source)
    require(env.get('GITHUB_WORKFLOW_SHA') == source, 'workflow/source mismatch')
    for key in ('GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT'):
        require(re.fullmatch('[1-9][0-9]*', env.get(key, '')), 'run/attempt')
    return {'source': source, 'run': env['GITHUB_RUN_ID'], 'attempt': env['GITHUB_RUN_ATTEMPT'],
            'repository': REPOSITORY, 'repository_id': REPOSITORY_ID, 'profile': PROFILE}

def verify_run(run, current, guard):
    require(str(run['id']) == current['run'] and str(run['run_attempt']) == current['attempt'] and run['head_sha'] == current['source'], 'API source/run/attempt')
    require(run['name'] == WORKFLOW and run['path'] == CALLER and run['head_branch'] == BRANCH and run['event'] in ('push', 'workflow_dispatch'), 'API caller')
    require(run['actor']['login'] == run['triggering_actor']['login'] == 'palmoni5', 'API actors')
    for key in ('repository', 'head_repository'):
        require(run[key]['id'] == REPOSITORY_ID and run[key]['full_name'] == REPOSITORY, 'API repositories')
    refs = run.get('referenced_workflows')
    require(type(refs) is list and len(refs) == 1 and refs[0]['path'] == REPOSITORY + '/' + GUARD + '@' + guard and refs[0]['sha'] == guard, 'actual referenced immutable guard')

def verify_jobs(rows, current, names):
    require(rows['total_count'] == len(rows['jobs']) <= 100, 'complete job inventory')
    selected = {}
    for name in names:
        matches = [x for x in rows['jobs'] if x['name'] == name]
        require(len(matches) == 1, 'missing/ambiguous role ' + name)
        item = matches[0]
        require(item['head_sha'] == current['source'] and str(item['run_id']) == current['run'] and str(item['run_attempt']) == current['attempt'] and item['conclusion'] == 'success' and item['status'] == 'completed', 'current successful role')
        require(type(item['id']) is int and item['id'] > 0, 'job ID')
        selected[name] = item
    return selected

def api(path):
    require(os.environ.get('GITHUB_ACTIONS') == 'true', 'API CI only')
    request = urllib.request.Request('https://api.github.com/repos/' + REPOSITORY + '/' + path,
        headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN'], 'Accept': 'application/vnd.github+json'})
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self,*args,**kwargs):raise Rejected('unexpected JSON API redirect')
    with urllib.request.build_opener(NoRedirect()).open(request, timeout=30) as response:
        raw = response.read(FILE_CAP + 1)
    return decode(raw)

def authorize(job, completed=()):
    root = Path.cwd(); current = context(os.environ, job)
    require(git(root, 'rev-parse', 'HEAD').decode().strip() == current['source'], 'checkout head')
    source = verify_source(root, current['source'], os.environ.get(AUTHORITY_KEY, ''))
    verify_run(api('actions/runs/' + current['run']), current, source['guard'])
    jobs = api('actions/runs/' + current['run'] + '/attempts/' + current['attempt'] + '/jobs?per_page=100') if completed else None
    roles = verify_jobs(jobs, current, completed) if completed else {}
    return {'context': current, 'authority': source, 'roles': roles}

def artifact_name(current):
    return 'file-mremap-helper-' + current['source'] + '-' + current['run'] + '-' + current['attempt']

def verify_artifact(inventory_value, current, producer):
    require(inventory_value['total_count'] == len(inventory_value['artifacts']) <= 100, 'complete artifact inventory')
    found = [v for v in inventory_value['artifacts'] if v['name'] == artifact_name(current)]
    require(len(found) == 1, 'artifact missing/duplicate')
    value = found[0]; run = value['workflow_run']
    require(type(value['id']) is int and value['id'] > 0 and value['expired'] is False and type(value['size_in_bytes']) is int and 0 < value['size_in_bytes'] <= TOTAL_CAP, 'artifact bounds')
    require(re.fullmatch('sha256:[0-9a-f]{64}', value.get('digest', '')), 'artifact digest')
    require(str(run['id']) == current['run'] and run['head_sha'] == current['source'] and run['repository_id'] == run['head_repository_id'] == REPOSITORY_ID, 'artifact current source')
    def stamp(v):return datetime.datetime.fromisoformat(v.replace('Z', '+00:00'))
    require(stamp(producer['started_at']) <= stamp(value['created_at']) <= stamp(producer['completed_at']), 'artifact producer-attempt interval')
    return value

def zip_members(raw):
    require(type(raw) is bytes and len(raw) <= TOTAL_CAP, 'compressed byte cap')
    archive = zipfile.ZipFile(io.BytesIO(raw)); rows = archive.infolist()
    require(len(rows) == len(MEMBERS) and {x.filename for x in rows} == set(MEMBERS), 'exact sixteen ZIP members')
    total = 0; result = {}
    for item in rows:
        require(item.filename not in result and '/' not in item.filename and '\\' not in item.filename and not item.is_dir(), 'ZIP duplicate/path')
        mode = item.external_attr >> 16
        require(not item.flag_bits & 1 and not stat.S_ISLNK(mode) and (not stat.S_IFMT(mode) or stat.S_ISREG(mode)), 'ZIP encryption/link/type')
        require(0 < item.file_size <= FILE_CAP, 'member byte cap'); total += item.file_size
        require(total <= TOTAL_CAP, 'expanded cap')
        with archive.open(item) as stream:
            data = stream.read(FILE_CAP + 1)
        require(len(data) == item.file_size and len(data) <= FILE_CAP, 'member truncation')
        result[item.filename] = data
    archive.close(); return result

def verify_bundle(files, auth):
    require(set(files) == set(MEMBERS) and all(type(v) is bytes and 0 < len(v) <= FILE_CAP for v in files.values()) and sum(map(len, files.values())) <= TOTAL_CAP, 'bundle members/bounds')
    manifest = decode(files['producer-manifest.json'])
    require(set(manifest) == {'schema', 'context', 'authority', 'producer_id', 'members', 'role'}, 'producer schema')
    require(manifest['schema'] == 1 and manifest['context'] == auth['context'] and manifest['authority'] == {k: auth['authority'][k] for k in ('guard', 'guard_blob', 'template', 'caller_authority')}, 'manifest current authority')
    require(manifest['role'] == PROFILE and manifest['producer_id'] == auth['roles']['helper-producer']['id'], 'producer role binding')
    require(manifest['members'] == {p: {'bytes': len(v), 'sha256': digest(v)} for p, v in files.items() if p != 'producer-manifest.json'}, 'all fifteen hashes')
    require(decode(files['source-manifest.json']) == auth['authority']['manifest'], 'independent current source manifest')
    profile = decode(files['compiler-profile.json'])
    require(profile['profile'] == PROFILE and profile['archive_sha256'] == '5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4' and profile['driver_sha256'] == 'c9b86311ade81d53235c93fafabdf98328d094de344ea9a9038e0dab6695ee9f' and profile['linker_sha256'] == 'cbae3badcf29268220242e8875cb6c288336ab593238099746f0071c15bbf581', 'compiler profile pin')
    require(set(profile['objects']) == set(TUS), 'three object stamps')
    for obj, source in TUS.items():
        row = profile['objects'][obj]
        require(row['source'] == source and row['source_blob'] == SOURCE_BLOBS[obj] and row['header_blob'] == HEADER_BLOB and row['sha256'] == digest(files[obj]) and row['exceptions'] == (obj != 'caller.obj'), 'source/object/exception stamp')
    require(profile['no_ambient_injection'] is True and profile['target'] == 'x86_64-w64-windows-gnu', 'target/config profile')
    require(profile['dependencies_verified'] is True and profile['link_dependencies_verified'] is True, 'dependency receipts missing')
    require(profile['common_flags']==list(COMMON) and profile['config_policy']=='--no-default-config; no ambient HOME/include/library/config variables', 'visible ABI/config flags')
    commands=decode(files['compile-commands.json']);dependencies=decode(files['dependency-manifest.json'])
    require(type(commands)is list and len(commands)==3 and {v['output']for v in commands}==set(TUS) and set(dependencies)==set(TUS),'compile/dependency inventory')
    for obj,source in TUS.items():
        row=profile['objects'][obj];command=next(v for v in commands if v['output']==obj)
        argv=row['argv'];expected_flags=list(COMMON)+(['-fexceptions']if obj!='caller.obj'else[])
        require(argv[1:1+len(expected_flags)]==expected_flags and argv[1+len(expected_flags):][0:2]==['-MD','-MF'],'effective per-object flags')
        require(command=={'output':obj,'file':source,'argv':argv,'expanded':row['expanded']},'object command stamp')
        require(row['source_sha256']==digest(git(Path.cwd(),'cat-file','blob',SOURCE_BLOBS[obj])) and row['header_sha256']==digest(git(Path.cwd(),'cat-file','blob',HEADER_BLOB)),'actual immutable source/header')
        expanded=[r for r in row['expanded']if '-cc1'in r]
        require(len(expanded)==1 and ('-fexceptions'in expanded[0])==(obj!='caller.obj') and ('-fcxx-exceptions'in expanded[0])==(obj!='caller.obj'),'actual expanded exception mode')
        dep=dependencies[obj];require(type(dep)is list and 1<=len(dep)<=8192,'dependency bounds')
        require({'windows.h','map','vector','memory','new','array','utility'}<={Path(x['path']).name for x in dep},'positive SDK/STL headers')
        require(any(x['origin']=='source'and x['relative']==HEADER and x['sha256']==row['header_sha256']for x in dep),'bound dependency header')
        for x in dep:
            require(set(x)=={'origin','path','relative','sha256','bytes'}and x['origin']in ('source','toolchain')and type(x['bytes'])is int and x['bytes']>0,'dependency schema')
            hexid(x['sha256'],64);require(not Path(x['relative']).is_absolute()and '..'not in Path(x['relative']).parts,'dependency path')
    links=profile['link_dependencies'];require(links['archives']and links['loaded']and len(links['direct'])>=4,'actual link dependencies')
    require({Path(x['path']).name for x in links['direct']if x['origin']=='three-object'}==set(TUS),'three direct link inputs')
    combined=files['link-transcript.txt'].lower()+canonical(profile['link_expanded']).lower()
    require(not any(x in combined for x in (b'libgesher_posix',b'binder',b'gesher_crash_reporter_anchor',b'gesher_signal_mailbox_anchor',b'--wrap')),'foreign selected/product dependencies')
    return manifest

def parse_budgets(stdout, stderr, exit_code):
    require(type(stdout) is bytes and type(stderr) is bytes and len(stdout) <= STREAM_CAP and len(stderr) <= STREAM_CAP and not stderr and exit_code == 0, 'raw output/exit')
    lines = stdout.decode('ascii').splitlines(); order=[]; families = {x: [] for x in ('alias', 'surgery', 'ledger', 'request')}; supplemental = {'alias': [], 'surgery': []}; final = []
    for line in lines:
        if line.startswith('{'):
            value = decode(line.encode()); require(set(value) == {'event','family','budget','allocation_calls','injected','status','outstanding','unchanged_owner_refs'} and value['event'] == 'allocation' and value['family'] in supplemental, 'allocation JSON grammar')
            require(value['outstanding'] == 0 and value['unchanged_owner_refs'] is True, 'unreleased owner storage')
            order.append((value['family']+'_json',value['budget'])); families[value['family']].append((value['budget'], value['allocation_calls'], value['injected'], value['status']))
        elif line.startswith('PREPARE_ALLOCATION '):
            m=re.fullmatch(r'PREPARE_ALLOCATION point=(\d+) calls=(\d+) status=(\d+) empty=([01]) released=1',line); require(m, 'alias text grammar'); supplemental['alias'].append(tuple(map(int,m.groups()))); order.append(('alias_text',int(m[1])))
        elif line.startswith('SURGERY_ALLOCATION '):
            m=re.fullmatch(r'SURGERY_ALLOCATION point=(\d+) calls=(\d+) status=(\d+) released=1 refs=3',line); require(m, 'surgery text grammar'); supplemental['surgery'].append(tuple(map(int,m.groups()))); order.append(('surgery_text',int(m[1])))
        elif line.startswith('PUNCH_LEDGER_ALLOCATION '):
            m=re.fullmatch(r'PUNCH_LEDGER_ALLOCATION capacity=(\d+) combined_pages=(\d+) point=(\d+) calls=(\d+) injected=(\d+) status=([01]) released=1',line);require(m,'ledger grammar');cap,pages,point,calls,inj,status=map(int,m.groups());require(pages>cap>0,'actual local capacity witness');order.append(('ledger',point));families['ledger'].append((point,calls,inj,'OK' if status==0 else 'NO_MEMORY',cap,pages))
        elif line.startswith('PUNCH_REQUEST_ALLOCATION '):
            m=re.fullmatch(r'PUNCH_REQUEST_ALLOCATION point=(\d+) calls=(\d+) injected=(\d+) status=([01]) released=1',line);require(m,'request grammar');point,calls,inj,status=map(int,m.groups());order.append(('request',point));families['request'].append((point,calls,inj,'OK' if status==0 else 'NO_MEMORY'))
        else:
            m=re.fullmatch(r'PREPARE_RESULT failures=(\d+) success=1 live=0 failed=0',line);require(m,'unknown/truncated output');order.append(('result',0));final.append((len(lines)-1,int(m[1])));require(line==lines[-1],'final result trailing rows')
    require(len(final)==1, 'one final result')
    limits={'alias':16,'surgery':64,'ledger':32,'request':256}
    for name, rows in families.items():
        require(2<=len(rows)<=limits[name], 'missing budget family')
        require([r[0] for r in rows]==list(range(len(rows))), 'contiguous unique budgets')
        require(all(type(v)is int and v>=0 for row in rows for v in row[:3]), 'budget numeric types')
        require(all(r[1]>0 and r[2]==1 and r[3]=='NO_MEMORY' and r[1]==r[0]+1 for r in rows[:-1]), 'actual caught allocation failures')
        require(rows[-1][1]>0 and rows[-1][2]==0 and rows[-1][3]=='OK' and rows[-1][1]<=rows[-1][0], 'actual first success')
        if name in supplemental:
            text=supplemental[name];require(len(text)==len(rows), 'missing paired text')
            require(all(t[:3]==(r[0],r[1],0 if r[3]=='OK' else 1) for t,r in zip(text,rows)), 'conflicting paired budget')
            if name=='alias':require(all(t[3]==1 for t in text[:-1]) and text[-1][3]==0,'alias detached state')
    require(len(families['alias'])-1==final[0][1] and final[0][1]>=3 and len(families['surgery'])>=4, 'fixture failure counters')
    require(len({r[4:] for r in families['ledger']})==1, 'inconsistent ledger capacity')
    expected_order=[(family+suffix,i)for family in ('alias','surgery')for i in range(len(families[family]))for suffix in ('_json','_text')]+[(family,i)for family in ('ledger','request')for i in range(len(families[family]))]+[('result',0)]
    require(order==expected_order,'complete output order')
    return {'families':{k:len(v)for k,v in families.items()}, 'request_owner_refs':'FIXTURE_EXIT_ASSERTION', 'ledger_capacity_scope':'ACTUAL_LOCAL_VECTOR_NOT_PRODUCTION_REGISTRY', 'production_registry_qualified':False}

def clean_environment(env):
    # Native process receives only named OS essentials, never API credentials.
    keys=('SystemRoot','WINDIR','TEMP','TMP','USERPROFILE','PATH','COMSPEC','PATHEXT')
    return {k:v for k,v in env.items() if k in keys}

def validate_completion(record):
    require(record['started'] is True and type(record['pid'])is int and record['pid']>0 and record['creation_time']>0 and record['exe_verified'] is True, 'owned process identity')
    require(record['kernel_completed'] is True and record['io_completed'] is True and record['handles_closed'] is True and record['timed_out'] is False and record['overflow'] is False and record['exit']==0, 'process containment/completion')

def settle_readers(graph, deadline):
    """Join only started, rooted readers; false forbids stream disposal."""
    for slot in graph['readers']:
        if slot['started']:
            try:
                slot['thread'].join(timeout=max(0, deadline-time.monotonic()))
            except BaseException as error:
                containment_error(graph,error)
    return all((slot['started'] and not slot['thread'].is_alive()) or
               (not slot['started'] and not slot.get('start_attempted',False))
               for slot in graph['readers'])

def launch_owned(graph, argv, **kwargs):
    """Publish uncertainty before acquisition; no returned object proves nothing."""
    require(graph['launch_state']=='NOT_ATTEMPTED' and graph['process'] is None,'one owned launch')
    graph['launch_state']='STARTING_UNCERTAIN'
    proc=None
    try:
        proc=subprocess.Popen(argv,**kwargs)
    finally:
        # Also retain a returned local when interruption occurs before return.
        if proc is not None:
            graph['process']=proc
            graph['launch_state']='RETURNED_OWNED'
    return proc

def prove_unattempted_failure(graph):
    # No Popen exception classification claims absence. Only an operation which
    # never crossed the attempted-launch boundary can acquire this proof.
    if graph['launch_state']=='NOT_ATTEMPTED':
        require(graph['process'] is None,'unattempted process contradiction')
        graph['launch_state']='PROVEN_FAILED_NO_CHILD'

def launch_quiescent(graph):
    if graph['launch_state'] in ('NOT_ATTEMPTED','PROVEN_FAILED_NO_CHILD'):
        return graph['process'] is None
    return (graph['launch_state']=='RETURNED_OWNED' and graph['process'] is not None
            and graph['kernel_completed'])

def containment_error(graph,error):
    # Quarantine may be indefinite; storage/output must not grow each retry.
    graph['settlement_error_count']=graph.get('settlement_error_count',0)+1
    if 'settlement_error' not in graph:graph['settlement_error']=repr(error)[:4096]

def settle_graph(graph,deadline):
    """Passive exact-owned settlement only. Never launch, kill or do test work."""
    proc=graph['process']
    try:
        # A published object itself is positive ownership evidence even if an
        # interruption preceded the following state-label assignment.
        if graph['launch_state']=='STARTING_UNCERTAIN' and proc is not None:
            graph['launch_state']='RETURNED_OWNED'
        if graph.get('image_state')=='STARTING_UNCERTAIN' and graph.get('image_handle') is not None:
            graph['image_state']='RETURNED_OWNED'
        if graph.get('image_state')=='STARTING_UNCERTAIN':return False
        if graph['launch_state']=='RETURNED_OWNED' and proc is not None and not graph['kernel_completed']:
            if proc.poll() is not None:
                proc.wait(timeout=0);graph['kernel_completed']=True
        quiescent=launch_quiescent(graph) and settle_readers(graph,deadline)
        if not quiescent:return False
        if proc is not None:
            for stream in (getattr(proc,'stdin',None),proc.stdout,proc.stderr):
                if stream is not None and not stream.closed:stream.close()
            if graph['kind']=='NATIVE' and not graph.get('process_handle_closed',False):
                proc._handle.Close();graph['process_handle_closed']=True
        if graph.get('image_handle') is not None:
            require(graph['kernel'].CloseHandle(graph['image_handle']),'retained image close')
            graph['image_handle']=None;graph['image_state']='CLOSED'
        graph['handles_closed']=True
        return True
    except BaseException as error:
        containment_error(graph,error);return False

def owned_helper(executable, expected_sha, current):
    """CI-only Windows child. Never dispose IO while its readers remain live."""
    require(os.environ.get('GITHUB_ACTIONS')=='true' and os.name=='nt', 'owned helper Windows CI only')
    executable=Path(executable).resolve();require(digest(executable.read_bytes())==expected_sha,'launch bytes')
    graph={'kind':'NATIVE','launch_state':'NOT_ATTEMPTED','kernel_completed':False,'handles_closed':False,'image_state':'NOT_ATTEMPTED','image_handle':None,'kernel':None,'process':None,'readers':[],'buffers':[bytearray(),bytearray()],'errors':[],'record':{'context':current,'started':False,'kernel_completed':False,'io_completed':False,'handles_closed':False,'timed_out':False,'overflow':False,'exit':None}}
    LIVE_GRAPHS.append(graph);record=graph['record'];start=time.monotonic();kernel=None
    try:
        import ctypes
        from ctypes import wintypes
        kernel=ctypes.WinDLL('kernel32',use_last_error=True);graph['kernel']=kernel
        kernel.CreateFileW.argtypes=[wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,wintypes.LPVOID,wintypes.DWORD,wintypes.DWORD,wintypes.HANDLE];kernel.CreateFileW.restype=wintypes.HANDLE
        kernel.CloseHandle.argtypes=[wintypes.HANDLE];kernel.CloseHandle.restype=wintypes.BOOL
        image_handle=None;graph['image_state']='STARTING_UNCERTAIN'
        try:
            image_handle=kernel.CreateFileW(str(executable),0x80000000,1,None,3,0x80,None)
        finally:
            if image_handle not in (None,ctypes.c_void_p(-1).value):
                graph['image_handle']=image_handle;graph['image_state']='RETURNED_OWNED'
            elif image_handle is not None:graph['image_state']='PROVEN_FAILED_NO_HANDLE'
        require(graph['image_handle'] is not None,'retained image handle')
        require(digest(executable.read_bytes())==expected_sha,'retained image bytes')
        proc=launch_owned(graph,[str(executable)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=clean_environment(os.environ),cwd=executable.parent)
        record.update(started=True,pid=proc.pid)
        # Retain graph BEFORE fallible ctypes/query/reader acquisition.
        kernel.GetProcessTimes.argtypes=[wintypes.HANDLE]+[ctypes.POINTER(wintypes.FILETIME)]*4;kernel.GetProcessTimes.restype=wintypes.BOOL
        times=[wintypes.FILETIME()for _ in range(4)];require(kernel.GetProcessTimes(int(proc._handle),*(ctypes.byref(x)for x in times)), 'creation time query')
        record['creation_time']=(times[0].dwHighDateTime<<32)|times[0].dwLowDateTime
        kernel.QueryFullProcessImageNameW.argtypes=[wintypes.HANDLE,wintypes.DWORD,wintypes.LPWSTR,ctypes.POINTER(wintypes.DWORD)];kernel.QueryFullProcessImageNameW.restype=wintypes.BOOL
        size=wintypes.DWORD(32768);name=ctypes.create_unicode_buffer(size.value)
        require(kernel.QueryFullProcessImageNameW(int(proc._handle),0,name,ctypes.byref(size)), 'image identity query')
        record['exe_verified']=Path(name.value).resolve()==executable;record['exe_sha256']=expected_sha;record['argv']=[str(executable)]
        require(record['exe_verified'],'actual image path');proc.stdin.close()
        def drain(index,stream):
            try:
                while True:
                    chunk=stream.read(8192)
                    if not chunk:break
                    remaining=STREAM_CAP-len(graph['buffers'][index]);graph['buffers'][index].extend(chunk[:max(0,remaining)])
                    if len(chunk)>remaining:record['overflow']=True
            except BaseException as error:graph['errors'].append(repr(error))
        for i,stream in enumerate((proc.stdout,proc.stderr)):
            reader=threading.Thread(target=drain,args=(i,stream),daemon=True);slot={'thread':reader,'started':False,'start_attempted':False};graph['readers'].append(slot);slot['start_attempted']=True;reader.start();slot['started']=True
        deadline=start+PROCESS_SECONDS-CLEANUP_SECONDS
        while proc.poll()is None and time.monotonic()<deadline and not record['overflow']:time.sleep(.02)
        if proc.poll()is None:
            record['timed_out']=time.monotonic()>=deadline;proc.kill()
        proc.wait(timeout=POSTKILL_SECONDS)
        graph['kernel_completed']=True;record['kernel_completed']=True;record['exit']=proc.returncode
    except BaseException as error:
        graph['errors'].append(repr(error));prove_unattempted_failure(graph)
    finally:
        proc=graph['process']
        if proc is not None and not record['kernel_completed']:
            try:
                if proc.poll()is None:proc.kill()
                proc.wait(timeout=POSTKILL_SECONDS);graph['kernel_completed']=True;record['kernel_completed']=True;record['exit']=proc.returncode
            except BaseException as error:graph['errors'].append(repr(error))
        released=settle_graph(graph,start+PROCESS_SECONDS)
        record['launch_state']=graph['launch_state'];record['kernel_completed']=graph['kernel_completed']
        record['io_completed']=released;record['handles_closed']=graph['handles_closed']
        if 'settlement_error' in graph:graph['errors'].append(graph['settlement_error'])
        record['elapsed_seconds']=time.monotonic()-start;record['errors']=list(graph['errors'])
        record['stdout']=bytes(graph['buffers'][0]).decode('ascii',errors='backslashreplace');record['stderr']=bytes(graph['buffers'][1]).decode('ascii',errors='backslashreplace')
        record['stdout_sha256']=digest(bytes(graph['buffers'][0]));record['stderr_sha256']=digest(bytes(graph['buffers'][1]))
        record['root_retained']=not released
        emit_record('PROCESS',record)
        if not record['root_retained']:LIVE_GRAPHS.remove(graph)
    require(not graph['errors'],'process errors');validate_completion(record)
    result=parse_budgets(bytes(graph['buffers'][0]),bytes(graph['buffers'][1]),record['exit'])
    return record,result

def guard_main(pins,template):
    current=context(os.environ,'verify');root=Path.cwd()
    auth=authorize('verify');require(auth['authority']['template']==template and all(auth['authority']['pins'][p]==b for p,b in pins.items()), 'trusted bootstrap pins')
    # Candidate-root modules are never imported; no mutable no-op in real gate.
    require(sys.flags.isolated==1 and sys.dont_write_bytecode and all(Path(m.__file__).resolve().parent!=root for m in (json,hashlib)), 'isolated stdlib')
    emit_record('SOURCE',{'context':current,'authority':auth['authority'],'qualification':False})
