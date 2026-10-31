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

def workflows(values,branch='wip/pthread-native-off-receiver'):
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
    assert 'runtime161_executed=false'in d['jobs']['native3-locks-regression']['steps'][-1]['run']
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
    for raw in ('name: bad\non: [push]\njobs: {}\n','name: bad\non:\n  push:\n    branches: [wip/pthread-native-off-receiver]\njobs: {}\n'):
        bad=dict(actual);bad['.github/workflows/UNEXPECTED.yml']=raw
        try:workflows(bad)
        except (AssertionError,AttributeError):negatives+=1
        else:raise AssertionError('Unwanted producer trigger admitted')
    print('NATIVE_OFF_SOURCE_PURE_PASS original_factory_ASTexact_after_closed_route_removal=1 opaque_paths='+str(len(opaque))+' copied_ordinary_steps11_native_receiver_steps3=EXACT trigger_negatives='+str(negatives)+' native_network0')

if __name__=='__main__':main()
