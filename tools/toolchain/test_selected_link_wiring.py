"""Source/metadata-only receiver projection controls; no native execution."""
import ast
import copy
from contextlib import redirect_stdout
import importlib.util
import json
import io
import os
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

import yaml

ROOT = Path(__file__).resolve().parents[2]
BASE = '4216a2999f0755faa42993fe4ea38fe66efc4957'
SELECTED_CASE = '''  selected-minimum)
    test -n "$SELECTED_LLVM" && test -n "$SELECTED_RECEIPT" && test -n "$SELECTED_RECEIPT_SHA256"
    llvm="$SELECTED_LLVM"
    printf 'GESHER_SELECTED_PTHREAD_RECEIPT=%s\\nGESHER_SELECTED_PTHREAD_RECEIPT_SHA256=%s\\nGESHER_HOST_PTHREAD_CALLER=ci/build\\n' "$SELECTED_RECEIPT" "$SELECTED_RECEIPT_SHA256" >> "$GITHUB_ENV"
    ;;
'''
OLD_CASE = '  selected-minimum) test -n "$SELECTED_LLVM"; llvm="$SELECTED_LLVM" ;;\n'
ACTOR = '''selected_links=host_roles.seal_selected([(Path('out/binder_sg_actor.exe'),root/'binder_sg_actor.exe')],production['compiler'],'binder-ordinary-actor')
if selected_links is not None:
    manifest['selected_link']=selected_links
'''


def old(path):
    return subprocess.check_output(['git', 'show', BASE + ':' + path], cwd=ROOT).decode('utf-8')


def exact_replace(text, before, after):
    assert text.count(before) == 1, 'Missing/ambiguous reviewed selected source boundary'
    return text.replace(before, after)


def workflow_projection(current, prior):
    result = copy.deepcopy(current)
    build = result['jobs']['build']
    selector = next(s for s in build['steps'] if s.get('name') == 'Bind the verified compiler root for this product build')
    assert selector['env'].pop('SELECTED_RECEIPT') == '${{ steps.host_toolchain.outputs.selected_receipt }}'
    assert selector['env'].pop('SELECTED_RECEIPT_SHA256') == '${{ steps.host_toolchain.outputs.selected_receipt_sha256 }}'
    selector['run'] = exact_replace(selector['run'], SELECTED_CASE, OLD_CASE)
    actor = next(s for s in build['steps'] if s.get('name') == 'Bind isolated ordinary Binder actor to exact production compiler and source')
    actor['run'] = exact_replace(actor['run'], ACTOR, '')
    assert result == prior, 'Changes outside exact first-job opt-in ledger projection'
    return result


def source_controls():
    workflow = '.github/workflows/ci.yml'
    current = yaml.safe_load((ROOT / workflow).read_text(encoding='utf-8'))
    prior = yaml.safe_load(old(workflow))
    workflow_projection(current, prior)
    mutations = []
    for before, after in [('test -n "$SELECTED_RECEIPT" && ', ''),
                          ('test -n "$SELECTED_RECEIPT_SHA256"', 'true'),
                          ('llvm="$SELECTED_LLVM"', 'llvm="$HOME/toolchain/$LLVM_MINGW_NAME"'),
                          ('publisher) llvm="$HOME/toolchain/$LLVM_MINGW_NAME"', 'publisher) llvm="$SELECTED_LLVM"'),
                          ('GESHER_HOST_PTHREAD_CALLER=ci/build', 'GESHER_HOST_PTHREAD_CALLER=other')]:
        bad = copy.deepcopy(current)
        selector = next(s for s in bad['jobs']['build']['steps'] if s.get('name') == 'Bind the verified compiler root for this product build')
        assert before in selector['run']
        selector['run'] = selector['run'].replace(before, after)
        mutations.append(bad)
    bad = copy.deepcopy(current)
    bad['jobs']['build']['steps'].append(dict(name='Unreviewed build step', run='true'))
    mutations.append(bad)
    bad = copy.deepcopy(current)
    bad['jobs']['native3-locks-reference']['runs-on'] = 'other'
    mutations.append(bad)
    for bad in mutations:
        try:
            workflow_projection(bad, prior)
        except AssertionError:
            pass
        else:
            raise AssertionError('Invalid default/other-job/root projection accepted')
    action = '.github/actions/setup-build/action.yml'
    current = yaml.safe_load((ROOT / action).read_text(encoding='utf-8'))
    step = current['runs']['steps'][-1]
    include = '    -DCMAKE_PROJECT_INCLUDE="$GITHUB_WORKSPACE/tools/toolchain/selected_link.cmake" \\\n'
    step['run'] = exact_replace(step['run'], include, '')
    assert current == yaml.safe_load(old(action)), 'Default setup action changed'
    shell = 'tools/web3/build-teardown-probe.sh'
    current = (ROOT / shell).read_text(encoding='utf-8')
    start = current.index('link_launcher=()\n')
    end = current.index('"$toolchain/bin/x86_64-w64-mingw32-clang++" --version\n', start)
    selected = current[start:end]
    assert selected == '''link_launcher=()
diagnostics=()
case "${GESHER_HOST_PTHREAD_SELECTION:-publisher}" in
  selected-minimum)
    link_launcher=(python3 tools/toolchain/selected_link.py launch)
    diagnostics=(-Wl,--verbose "-Wl,-Map,$output/web3_process_waits.map")
    ;;
  publisher) ;;
  *) exit 1 ;;
esac
'''
    current = current[:start] + current[end:]
    current = exact_replace(current, '"${link_launcher[@]}" ', '')
    current = exact_replace(current, ' "${diagnostics[@]}"', '')
    assert current == old(shell).replace('\r\n', '\n'), 'Default observer argv/source changed'
    # No source-authority allow-list or existing pinned blob is updated here.
    native = 'tools/native3-locks/union_payload.py'
    a, b = ast.parse((ROOT / native).read_text(encoding='utf-8')), ast.parse(old(native))
    def immutable(tree):
        return [ast.dump(n, include_attributes=False) for n in tree.body if not isinstance(n, ast.FunctionDef)]
    assert immutable(a) == immutable(b), 'Native source pins/constants/imports changed'
    print('PASS wiring source: 7 root/context/default/other-job negatives; exact publisher configure/observer argv and unchanged Native source pins/constants')


def compiler_controls():
    path = ROOT / 'tools/web3/host_roles.py'
    spec = importlib.util.spec_from_file_location('selected_wiring_host_roles', path)
    current = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(current)
    original = next(n for n in ast.parse(old('tools/web3/host_roles.py')).body if isinstance(n, ast.FunctionDef) and n.name == 'compiler')
    namespace = dict(vars(current))
    exec(compile(ast.fix_missing_locations(ast.Module(body=[original], type_ignores=[])), '<original compiler metadata function>', 'exec'), namespace)
    with tempfile.TemporaryDirectory(prefix='gesher-compiler-metadata-only-') as temporary:
        build = Path(temporary)
        driver = build / 'SYNTHETIC-driver-metadata.txt'
        driver.write_text('SYNTHETIC_METADATA_ONLY_NOT_EXECUTABLE', encoding='utf-8')
        directory = build / 'CMakeFiles' / 'metadata-only'
        directory.mkdir(parents=True)
        (directory / 'CMakeCXXCompiler.cmake').write_text('set(CMAKE_CXX_COMPILER "' + str(driver) + '")\n')
        (build / 'CMakeCache.txt').write_text('CMAKE_CXX_COMPILER:FILEPATH=' + str(driver) + '\n')
        rows = [dict(directory=str(ROOT), file=str(ROOT / name), arguments=[str(driver), '-o', name+'.metadata-only']) for name in current.CHANGED]
        (build / 'compile_commands.json').write_text(json.dumps(rows))
        with patch.dict(os.environ, {'GESHER_HOST_PTHREAD_SELECTION': 'publisher'}):
            before, after = namespace['compiler'](build), current.compiler(build)
        assert json.dumps(before, sort_keys=True).encode() == json.dumps(after, sort_keys=True).encode(), 'Publisher compiler bytes changed'
        assert current.selected_members({'actual.exe'}, after, 'production') == set()
        assert current.seal_selected([], after, 'production') is None
        assert current.verify_selected(build, None, after, {}, set(), 'production') is None
        try:
            current.verify_selected(build, {}, after, {}, set(), 'production')
        except SystemExit:
            pass
        else:
            raise AssertionError('Selected evidence accepted in publisher record')
        selected = dict(after, selected_toolchain={'context': {}})
        expected = {'selected-pthread-production-toolchain.json'} | {'actual.exe.selected-pthread-'+s for s in ('link.json', 'map.txt', 'trace.txt', 'imports.txt')}
        assert current.selected_members({'actual.exe'}, selected, 'production') == expected
        info = dict(source='1'*40, run='123456', attempt='1', repository='gesher-runtime/gesher', repository_id='1426153186')
        fields = dict(zip(('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_REPOSITORY','GITHUB_REPOSITORY_ID'),
                          (info['source'],info['run'],info['attempt'],info['repository'],info['repository_id'])))
        selected['selected_toolchain']['context'] = fields
        targets = {'native3_lock_command_worker.exe', 'native3_lock_duplicate_test.exe', 'binderd.exe'}
        assert len(current.selected_members(targets, selected, 'native3-common-host-fixture')) == 13
        assert len({n+s for n in ('native3_lock_command_worker','native3_lock_duplicate_test','binderd') for s in ('.exe','.imports.txt')}
                   | current.selected_members(targets, selected, 'native3-common-host-fixture')) == 19
        (build / 'synthetic-record.json').write_text('{"scope":"SYNTHETIC_METADATA_ONLY"}', encoding='utf-8')
        links = {name: {'file':'synthetic-record.json','index':'SYNTHETIC'} for name in targets}
        class Receiver:
            calls = []
            def verify_sealed(self, directory, link, compiler, executable, expected_context, receipt_name):
                self.calls.append((directory, link, compiler, executable, expected_context, receipt_name))
        receiver = Receiver()
        with patch.object(current, 'selected_ledger', return_value=receiver), patch.object(current, 'ledger_lines', return_value=['SYNTHETIC_TRANSPORT_ONLY']), redirect_stdout(io.StringIO()):
            current.verify_selected(build, links, selected, info, targets, 'native3-common-host-fixture')
            assert len(receiver.calls) == 3 and {row[3] for row in receiver.calls} == targets
            assert all(row[4] == fields for row in receiver.calls), 'Mandatory context was not passed from actual outer producer'
            assert all(row[5] == 'selected-pthread-native3-common-host-fixture-toolchain.json' for row in receiver.calls), 'Role-scoped receipt name was not passed'
            for field in fields:
                bad = copy.deepcopy(selected)
                bad['selected_toolchain']['context'][field] = 'older-or-other'
                try:
                    current.verify_selected(build, links, bad, info, targets, 'native3-common-host-fixture')
                except SystemExit:
                    pass
                else:
                    raise AssertionError('Receiver accepted coherent different outer producer context')
            for bad_links in ({}, dict(links, extra={})):
                try:
                    current.verify_selected(build, bad_links, selected, info, targets, 'native3-common-host-fixture')
                except SystemExit:
                    pass
                else:
                    raise AssertionError('Receiver accepted missing/extra selected target proof')
    print('PASS compiler publisher JSON byte equality against original function; selected evidence injection rejected; exact per-target inventory; metadata text only, native_processes=0')
    print('PASS Native exact6default/6+13selected; mandatory outer context forwarded to three real API calls; 5 context +2 target-set negatives')
    payload = dict(compiler={'selected_toolchain': {'context': 'SYNTHETIC'}}, proof='x'*18000, unicode='\u05d0')
    raw = json.dumps(payload, sort_keys=True, ensure_ascii=False).encode('utf-8')
    rows = current.ledger_lines(raw, 'TEST_SELECTED_LEDGER')
    assert all(row.isascii() and len(row) <= 4096 for row in rows)
    assert current.reconstruct_ledger(rows, 'TEST_SELECTED_LEDGER') == raw
    bad_part = rows[1].split(' ',1)
    item = json.loads(bad_part[1]);item['sha256'] = '0'*64
    wrong_hash = rows[:1] + [bad_part[0]+' '+json.dumps(item,sort_keys=True)] + rows[2:]
    transport_bad = [rows[1:], rows[:-1], rows[:1]+rows[2:], rows[:1]+[rows[2],rows[1]]+rows[3:],
                     rows[:1]+[rows[1]]+rows[1:], wrong_hash,
                     [rows[0].replace('SELECTED_LEDGER_BEGIN','OTHER_BEGIN')]+rows[1:],
                     [row.replace('TEST_SELECTED_LEDGER','OTHER_LABEL') for row in rows],
                     rows[:1]+['x'*4097]+rows[2:]]
    for bad in transport_bad:
        try:
            current.reconstruct_ledger(bad, 'TEST_SELECTED_LEDGER')
        except (AssertionError,SystemExit,KeyError,ValueError):
            pass
        else:
            raise AssertionError('Malformed ledger transport accepted')
    publisher = dict(compiler={'sha256':'SYNTHETIC'}, role='publisher')
    output = io.StringIO()
    with redirect_stdout(output):
        current.emit_record('ORIGINAL_LABEL', publisher, flush=True)
    assert output.getvalue() == 'ORIGINAL_LABEL '+json.dumps(publisher,sort_keys=True)+'\n'
    output = io.StringIO()
    with redirect_stdout(output):
        current.emit_record('TEST_SELECTED_LEDGER', payload, raw=raw)
    assert current.reconstruct_ledger(output.getvalue().splitlines(),'TEST_SELECTED_LEDGER') == raw
    print('PASS exact publisher record output/flush and bounded selected whole-byte transport roundtrip; 9 missing/order/label/hash/long-line negatives')


if __name__ == '__main__':
    source_controls()
    compiler_controls()
