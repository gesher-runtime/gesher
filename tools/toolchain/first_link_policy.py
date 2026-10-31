"""Local source-only freeze/trigger proof; never compile or publish anything."""
import argparse
import ast
import fnmatch
import hashlib
import json
from pathlib import Path
import re
import subprocess

import yaml
import first_link_context as focused

ROOT=Path(__file__).resolve().parents[2]
BASE='8e92d30b6b25faa3c1daf6014b7bd0ba2ebb6e2f'
CHANGED={focused.PATH,'tools/host-pthread-authority/minimum_receipt.py',
         'tools/toolchain/prepare_host_pthread.py','tools/toolchain/selected_link.cmake',
         'tools/toolchain/first_link_context.py','tools/toolchain/first_link.py','tools/toolchain/first_link_policy.py',
         'tools/toolchain/test_first_link_context.py','tools/toolchain/test_first_link_live.py',
         'tools/toolchain/test_first_link_manifest.py','docs/handoff/tracks/pthread-first-link-component.md'}


def sha(raw):return hashlib.sha256(raw).hexdigest()
def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT)


def normalized_workflow(raw):
    pattern=rb'(uses: '+re.escape((focused.REPOSITORY+'/'+focused.POLICY_PATH+'@').encode())+rb')([0-9a-f]{40})'
    assert len(re.findall(pattern,raw))==1
    return re.sub(pattern,lambda m:m[1]+b'IMMUTABLE_POLICY_PIN',raw)


def trigger_proof(workflows,branch=focused.BRANCH):
    report={};matches=[]
    for name,raw in workflows.items():
        value=yaml.safe_load(raw.decode('utf-8'));event=value.get('on',value.get(True,{}))
        if not isinstance(event,dict) or 'push' not in event:
            assert event!='push' and not(isinstance(event,list) and 'push' in event)
            report[name]=dict(push=False,branch_match=False);continue
        push=event['push'];assert isinstance(push,dict) and isinstance(push.get('branches'),list),'Unfiltered push workflow'
        assert 'branches-ignore' not in push and all(isinstance(p,str) and not p.startswith('!') for p in push['branches'])
        matched=any(fnmatch.fnmatchcase(branch,p) for p in push['branches'])
        report[name]=dict(push=True,branches=push['branches'],branch_match=matched,sha256=sha(raw))
        if matched:matches.append(name)
    assert matches==[focused.PATH],('Other automatic producers would run',matches)
    caller=yaml.safe_load(workflows[focused.PATH].decode('utf-8'));events=caller.get('on',caller.get(True))
    assert set(events)=={'push','workflow_dispatch'}
    assert events['push']==dict(branches=[focused.BRANCH],paths=[focused.PATH])
    assert set(caller['jobs'])=={'source_authority','build','portable'}
    assert caller['jobs']['portable']['strategy']['matrix']['os']==['windows-2022','windows-2025']
    return report


def source_proof():
    changes=set(git('diff','--name-only',BASE,'HEAD').decode().splitlines())
    assert changes==CHANGED,('Unexpected focused source change set',changes^CHANGED)
    assert not git('status','--porcelain'),'Source freeze must be clean'
    old=ast.parse(git('show',BASE+':tools/host-pthread-authority/minimum_receipt.py').decode())
    new=ast.parse((ROOT/'tools/host-pthread-authority/minimum_receipt.py').read_text(encoding='utf-8'))
    def native_parts(tree):return [ast.dump(n,include_attributes=False) for n in tree.body if not isinstance(n,ast.FunctionDef) or n.name!='context']
    assert native_parts(old)==native_parts(new),'Native profile/layout/supplier recipe changed beyond caller context'
    unchanged=['tools/host-pthread-authority/build-minimum-supplier.sh','tools/host-pthread-authority/minimum-inputs.json',
               'tools/host-pthread-authority/minimum-detached-retirement.patch','tools/host-pthread-authority/minimum-noattach-join.patch',
               'tools/fonts/verify-standalone-link.py','tools/toolchain/selected_link.py','tools/web3/host_roles.py',
               'tools/native3-locks/union_payload.py','tools/binder/verify-full-host.py','.github/workflows/ci.yml','.github/actions/setup-build/action.yml']
    assert not git('diff',BASE,'HEAD','--',*unchanged),'Full/default/provider/receipt/assembly code changed'
    workflows={str(p.relative_to(ROOT)).replace('\\','/'):p.read_bytes() for p in sorted((ROOT/'.github/workflows').glob('*.yml'))}
    triggers=trigger_proof(workflows)
    records={path:sha(normalized_workflow((ROOT/path).read_bytes()) if path==focused.PATH else (ROOT/path).read_bytes()) for path in sorted(CHANGED)}
    return dict(base=BASE,source=git('rev-parse','HEAD').decode().strip(),expected_changed_sha256=records,
                workflow_hash_normalization='only single exact immutable policy uses pin',trigger_exclusion=triggers,
                other_push_workflows=len(triggers)-1,planned_jobs=4,native_provider_source_unchanged=True,
                native_profile_recipe_unchanged_except_exact_caller=True,full_defaults_source_guards_unchanged=True)


def selftest():
    caller=(ROOT/focused.PATH).read_bytes();a=normalized_workflow(caller)
    pattern=rb'('+re.escape((focused.REPOSITORY+'/'+focused.POLICY_PATH+'@').encode())+rb')[0-9a-f]{40}'
    b=re.sub(pattern,lambda m:m[1]+b'1'*40,caller)
    assert a==normalized_workflow(b)
    bad=caller+caller
    try:normalized_workflow(bad)
    except AssertionError:pass
    else:raise AssertionError('Duplicate policy pin accepted')
    current={str(p.relative_to(ROOT)).replace('\\','/'):p.read_bytes() for p in sorted((ROOT/'.github/workflows').glob('*.yml'))}
    trigger_proof(current)
    for raw in (b'name: unexpected\non: [push]\njobs: {}\n',
                b'name: unexpected\non:\n  push:\n    branches: [wip/pthread-first-link]\njobs: {}\n'):
        bad=dict(current);bad['.github/workflows/UNEXPECTED.yml']=raw
        try:trigger_proof(bad)
        except AssertionError:pass
        else:raise AssertionError('An independent automatic producer was accepted')
    print('PASS focused source policy: unique normalized immutable pin; 2 other-producer trigger negatives; no compiler/network/native')


if __name__=='__main__':
    parser=argparse.ArgumentParser();group=parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--selftest',action='store_true');group.add_argument('--snapshot',type=Path);args=parser.parse_args()
    if args.selftest:selftest()
    else:
        data=source_proof();args.snapshot.write_text(json.dumps(data,sort_keys=True,indent=2)+'\n',encoding='utf-8')
        print('FOCUSED_SOURCE_FREEZE '+json.dumps({k:data[k] for k in ('source','base','other_push_workflows','planned_jobs')},sort_keys=True))
