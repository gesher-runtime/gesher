"""Pure source/AST/DAG controls. No compiler/native/network invocation."""
import ast,copy,fnmatch,subprocess
from pathlib import Path
import yaml
import native_off_receiver_policy as p

def normalize_factory(raw):
    class StripExactNativeRoute(ast.NodeTransformer):
        def visit_If(self,node):
            if "'pthread-native-off-receiver/build'"in ast.unparse(node.test):
                return [self.visit(n)for n in node.orelse]
            return self.generic_visit(node)
        def visit_Tuple(self,node):
            node.elts=[x for x in node.elts if not(isinstance(x,ast.Constant)and x.value=='pthread-native-off-receiver/build')]
            return self.generic_visit(node)
    return ast.dump(StripExactNativeRoute().visit(ast.parse(raw)),include_attributes=False)

def workflows(values,branch='wip/pthread-native-runtime161'):
    matched=[]
    for name,raw in values.items():
        data=yaml.safe_load(raw);events=data.get('on',data.get(True,{}))
        if isinstance(events,str):assert events!='push';continue
        if isinstance(events,list):assert 'push'not in events;continue
        assert not {'workflow_run','schedule','repository_dispatch'}&set(events)
        if 'push'not in events:continue
        push=events['push'];assert isinstance(push,dict)and isinstance(push.get('branches'),list)
        assert 'branches-ignore'not in push and all(type(x)is str and not x.startswith('!')for x in push['branches'])
        if any(fnmatch.fnmatchcase(branch,x)for x in push['branches']):matched.append(name)
    assert matched==[p.CALLER]

def main():
    root=p.ROOT
    actual={str(x.relative_to(root)).replace('\\','/'):x.read_text()for x in sorted((root/'.github/workflows').glob('*.yml'))}
    workflows(actual)
    d=yaml.load(actual[p.CALLER],Loader=yaml.BaseLoader);old=yaml.load(p.git('show',p.BASE+':.github/workflows/ci.yml'),Loader=yaml.BaseLoader)
    assert set(d['jobs'])=={'source_authority','build','native3-locks-reference','native3-locks-regression'}
    assert d['jobs']['native3-locks-regression']['strategy']['matrix']['os']==['windows-2022','windows-2025']
    assert set(d['jobs']['native3-locks-regression']['needs'])=={'source_authority','build','native3-locks-reference'}
    assert d['jobs']['build']['needs']==d['jobs']['native3-locks-reference']['needs']==['source_authority']
    copied=d['jobs']['build']['steps'][4:-1]
    assert copied==[old['jobs']['build']['steps'][i]for i in (3,4,6,7,8,9,10,11,12,13,15)]
    assert 'GESHER_WEB3_QUERY_TESTS=ON'not in actual[p.CALLER]
    assert '--mode=hook'not in actual[p.CALLER]
    assert d['jobs']['native3-locks-regression']['steps'][2:5]==old['jobs']['native3-locks-regression']['steps'][1:4]
    assert d['jobs']['native3-locks-regression']['steps'][-1]==old['jobs']['native3-locks-regression']['steps'][4]
    # Exact additive route, not a weakened old factory algorithm/default.
    oldfactory=p.git('show',p.BASE+':tools/toolchain/prepare_host_pthread.py').decode()
    newfactory=(root/'tools/toolchain/prepare_host_pthread.py').read_text()
    assert normalize_factory(oldfactory)==normalize_factory(newfactory)
    cmake=(root/'tools/toolchain/selected_link.cmake').read_text()
    start=cmake.index('elseif("$ENV{GESHER_HOST_PTHREAD_CALLER}" STREQUAL "pthread-native-off-receiver/build")')
    end=cmake.index('elseif(NOT "$ENV{GESHER_HOST_PTHREAD_CALLER}" STREQUAL "ci/build")',start)
    assert cmake[:start]+cmake[end:]==p.git('show',p.BASE+':tools/toolchain/selected_link.cmake').decode()
    opaque=['tools/web3/host_roles.py','tools/native3-locks/union_payload.py','tools/toolchain/selected_link.py',
            '.github/workflows/ci.yml','.github/actions/setup-build/action.yml',
            'tools/ci/artifact_receipt.py','tools/ci/receipt_identity.py','tools/surface/download-verified-artifact.py',
            'tools/web3/build-teardown-probe.sh','probes/web3/process_waits.cc','compat/input/thread_watch.cc',
            'CMakeLists.txt','cmake/webview.cmake','cmake/aosp/art_runtime.cmake','tools/package/gesher.iss',
            'tools/host-pthread-authority/build-minimum-supplier.sh','tools/host-pthread-authority/minimum-inputs.json',
            'tools/host-pthread-authority/minimum-detached-retirement.patch','tools/host-pthread-authority/minimum-noattach-join.patch']
    assert not p.git('diff',p.BASE,'--',*opaque)
    negatives=0
    for raw in ('name: bad\non: [push]\njobs: {}\n','name: bad\non:\n  push:\n    branches: [wip/pthread-native-runtime161]\njobs: {}\n'):
        bad=dict(actual);bad['.github/workflows/UNEXPECTED.yml']=raw
        try:workflows(bad)
        except (AssertionError,AttributeError):negatives+=1
        else:raise AssertionError('Unwanted producer trigger admitted')
    print('NATIVE_OFF_SOURCE_PURE_PASS original_factory_ASTexact_after_closed_route_removal=1 opaque_paths='+str(len(opaque))+' copied_ordinary_steps11_native_receiver_steps3=EXACT trigger_negatives='+str(negatives)+' native_network0')

def authority_controls():
    env=dict(GITHUB_ACTIONS='true',GITHUB_REPOSITORY=p.REPO,GITHUB_REPOSITORY_ID=p.RID,
             GITHUB_REF=p.REF,GITHUB_WORKFLOW=p.NAME,GITHUB_JOB='verify',
             GITHUB_WORKFLOW_REF=p.REPO+'/'+p.CALLER+'@'+p.REF,
             GITHUB_SHA='1'*40,GITHUB_WORKFLOW_SHA='1'*40,GITHUB_ACTOR='palmoni5',
             GITHUB_TRIGGERING_ACTOR='palmoni5',GITHUB_EVENT_NAME='push',
             GITHUB_RUN_ID='123',GITHUB_RUN_ATTEMPT='1',**{p.VARIABLE:'1'*40})
    policy=p.REPO+'/'+p.TRUST+'@'+'2'*40
    run=dict(id=123,run_attempt=1,head_sha='1'*40,head_branch=p.REF.removeprefix('refs/heads/'),
             path=p.CALLER,name=p.NAME,event='push',repository=dict(full_name=p.REPO,id=int(p.RID)),
             head_repository=dict(full_name=p.REPO,id=int(p.RID)),actor=dict(login='palmoni5'),
             triggering_actor=dict(login='palmoni5'),referenced_workflows=[dict(path=policy,sha='2'*40)])
    p.authority_run(env,run,policy);negatives=0
    for key in env:
        bad=dict(env);bad[key]='UNBOUND'
        try:p.authority_run(bad,run,policy)
        except (AssertionError,KeyError):negatives+=1
        else:raise AssertionError('Wrong gate env admitted: '+key)
    for key in ('id','run_attempt','head_sha','head_branch','path','name','event'):
        bad=copy.deepcopy(run);bad[key]='UNBOUND'
        try:p.authority_run(env,bad,policy)
        except (AssertionError,KeyError):negatives+=1
        else:raise AssertionError('Wrong gate live run admitted: '+key)
    for key,sub in [('repository','id'),('repository','full_name'),('head_repository','id'),
                    ('head_repository','full_name'),('actor','login'),('triggering_actor','login')]:
        bad=copy.deepcopy(run);bad[key][sub]='UNBOUND'
        try:p.authority_run(env,bad,policy)
        except (AssertionError,KeyError):negatives+=1
        else:raise AssertionError('Wrong gate live identity admitted')
    for refs in ([],run['referenced_workflows']*2,[dict(path='wrong',sha='2'*40)],[dict(path=policy,sha='3'*40)]):
        bad=copy.deepcopy(run);bad['referenced_workflows']=refs
        try:p.authority_run(env,bad,policy)
        except (AssertionError,KeyError):negatives+=1
        else:raise AssertionError('Wrong immutable reference admitted')
    print('NATIVE_OFF_POLICY_AUTHORITY_PURE_PASS negatives='+str(negatives)+' native_network0')

def immutable_controls():
    import textwrap
    raw=(p.ROOT/p.TRUST).read_text()
    code=textwrap.dedent(raw.split("python3 -I - <<'PY'\n",1)[1].rsplit('\n          PY',1)[0])
    tree=ast.parse(code);definitions=[n for n in tree.body if not(isinstance(n,ast.Expr)and isinstance(n.value,ast.Call)and isinstance(n.value.func,ast.Name)and n.value.func.id=='gate_main')]
    scope={};exec(compile(ast.Module(body=definitions,type_ignores=[]),'frozen-native-policy','exec'),scope)
    source=p.git('rev-parse','HEAD').decode().strip()
    scope['frozen_candidate'](scope['EXPECTED'],source)
    actual=p.inventory(source);full=scope['frozen_history']();count=0
    for path in actual:
        bad=dict(actual);bad[path]=('100644','0'*40)
        try:full['verify_tree'](bad,actual)
        except RuntimeError:count+=1
        else:raise AssertionError('Whole-tree mutation admitted: '+path)
    for bad in (dict(actual,**{'UNREVIEWED.source':('100644','0'*40)}),{k:v for k,v in actual.items()if k!=p.CALLER}):
        try:full['verify_tree'](bad,actual)
        except RuntimeError:count+=1
        else:raise AssertionError('Whole-tree addition/removal admitted')
    # Each candidate leaf is independently digest-bound, including the helpers
    # and this test. These are in-memory ablations, not self-approval of a run.
    for path in scope['EXPECTED']:
        wrong=dict(scope['EXPECTED']);wrong[path]='0'*64
        try:scope['frozen_candidate'](wrong,source)
        except AssertionError:count+=1
        else:raise AssertionError('Immutable leaf pin ablation admitted: '+path)
    print('NATIVE_OFF_IMMUTABLE_SOURCE_PURE_PASS whole_and_leaf_negatives='+str(count)+' independent_gate_definitions=true native_network0')

if __name__=='__main__':
    import sys
    main();authority_controls()
    if '--immutable'in sys.argv:immutable_controls()
