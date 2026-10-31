"""Pure source/history/trigger proof; never approve or run native outputs."""
import ast,copy,hashlib,json,re,subprocess,textwrap
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
BASE='f9288a647298adb90bdef82302d745cf806bb7ea'
FULL='1bf8fad9ec3b423ddef8c97c1d72b25790974364'
FULL_GUARD='340e30efafce7f232fb6e03197b775dc595c94e6'
FULL_TRUST='.github/workflows/net-combined-dns-trusted-source.yml'
FULL_BLOB='419783115876ababb9278e04d6b6092e02d2caf3'
CALLER='.github/workflows/pthread-native-off-receiver-component.yml'
TRUST='.github/workflows/pthread-native-off-receiver-trusted-source.yml'
CHANGED={CALLER,TRUST,'tools/toolchain/native_off_receiver_context.py',
 'tools/toolchain/native_off_receiver.py','tools/toolchain/native_off_receiver_policy.py',
 'tools/toolchain/test_native_off_receiver_context.py','tools/toolchain/test_native_off_receiver_live.py',
 'tools/toolchain/test_native_off_receiver_policy.py','tools/toolchain/prepare_host_pthread.py',
 'tools/host-pthread-authority/minimum_receipt.py','tools/toolchain/selected_link.cmake',
 'docs/handoff/tracks/pthread-native-off-receiver-component.md'}

def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT)
def digest(raw):return hashlib.sha256(raw).hexdigest()
def normalized_workflow(raw):
    pattern=rb'(uses: gesher-runtime/gesher/\.github/workflows/pthread-native-off-receiver-trusted-source\.yml@)[0-9a-f]{40}'
    assert len(re.findall(pattern,raw))==1
    return re.sub(pattern,lambda m:m[1]+b'IMMUTABLE_POLICY_PIN',raw)

def frozen_history():
    raw=git('show',FULL_GUARD+':'+FULL_TRUST)
    assert hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()==FULL_BLOB
    assert raw==git('show',FULL+':'+FULL_TRUST)
    code=textwrap.dedent(raw.decode().split("python3 -I - <<'PY'\n",1)[1].rsplit('\n          PY',1)[0]);tree=ast.parse(code)
    definitions=[n for n in tree.body if not(isinstance(n,ast.Expr)and isinstance(n.value,ast.Call)and isinstance(n.value.func,ast.Name)and n.value.func.id=='main')]
    scope={};exec(compile(ast.Module(body=definitions,type_ignores=[]),'immutable-full-definitions','exec'),scope)
    main=next(n for n in tree.body if isinstance(n,ast.FunctionDef)and n.name=='main')
    start=next(i for i,n in enumerate(main.body)if isinstance(n,ast.Expr)and "'--is-ancestor', BASE, sha"in ast.unparse(n))
    end=next(i for i,n in enumerate(main.body)if isinstance(n,ast.Assign)and any(isinstance(t,ast.Name)and t.id=='reviewed'for t in n.targets))
    scope.update(sha=FULL,reviewed=FULL,guard_commit=FULL_GUARD,approved_caller=git('show',FULL+':'+scope['CALLER']))
    exec(compile(ast.Module(body=main.body[start:end],type_ignores=[]),'historical-source-only','exec'),scope)
    start=next(i for i,n in enumerate(main.body)if isinstance(n,ast.Expr)and "'--is-ancestor', COMBINED_BASE, sha"in ast.unparse(n))
    end=next(i for i,n in enumerate(main.body)if isinstance(n,ast.For)and "compat/posix/src/record_lock_registry.cc"in ast.unparse(n))
    exec(compile(ast.Module(body=main.body[start:end],type_ignores=[]),'qualified-full-source-only','exec'),scope)
    # Declarative selected pin metadata only, never a fixture/preflight main.
    raw_inner=git('show',FULL+':tools/native3-locks/union_payload.py').decode();inner=ast.parse(raw_inner);nodes=[]
    for n in inner.body:
        if isinstance(n,ast.Assign)and any(isinstance(t,ast.Name)and t.id=='TARGETS'for t in n.targets):break
        if isinstance(n,ast.Assign)or(isinstance(n,ast.Expr)and isinstance(n.value,ast.Call)and isinstance(n.value.func,ast.Attribute)and isinstance(n.value.func.value,ast.Name)and n.value.func.value.id=='PINNED_SOURCE'):nodes.append(n)
    selected={'Path':Path,'__file__':str(ROOT/'tools/native3-locks/union_payload.py')}
    exec(compile(ast.Module(body=nodes,type_ignores=[]),'immutable-pin-data','exec'),selected)
    assert len(selected['PINNED_SOURCE'])==104
    full=scope['inventory'](FULL)
    assert all(full[p][1]==b for p,b in selected['PINNED_SOURCE'].items())
    historical='00f96217616c5892dd8364067d007f92d541585b'
    legacy=ast.parse(git('show',historical+':tools/native3-locks/union_payload.py').decode());nodes=[]
    for n in legacy.body:
        if isinstance(n,ast.Assign)and any(isinstance(t,ast.Name)and t.id=='TARGETS'for t in n.targets):break
        if isinstance(n,ast.Assign)or(isinstance(n,ast.Expr)and isinstance(n.value,ast.Call)and isinstance(n.value.func,ast.Attribute)and isinstance(n.value.func.value,ast.Name)and n.value.func.value.id=='PINNED_SOURCE'):nodes.append(n)
    legacy_scope={'Path':Path,'__file__':str(ROOT/'tools/native3-locks/union_payload.py')}
    exec(compile(ast.Module(body=nodes,type_ignores=[]),'historical-pin-data','exec'),legacy_scope)
    prior=scope['inventory'](historical)
    assert len(legacy_scope['PINNED_SOURCE'])==98
    assert all(prior[p][1]==b for p,b in legacy_scope['PINNED_SOURCE'].items())
    assert set(legacy_scope['PINNED_SOURCE'])<=set(selected['PINNED_SOURCE'])
    print('NATIVE_OFF_FROZEN_HISTORY qualified_full='+FULL+' historical_inner98/current_inner104 selected_outer_history=UNCHANGED native_executed=false',flush=True)
    return scope

def source_proof():
    assert not git('status','--porcelain','--untracked-files=all')
    assert set(git('diff','--name-only',BASE,'HEAD').decode().splitlines())==CHANGED
    full=frozen_history();expected=full['inventory'](BASE);current=full['inventory'](git('rev-parse','HEAD').decode().strip())
    for path in CHANGED:expected[path]=current[path]
    full['verify_tree'](current,expected);full['verify_checkout'](expected)
    for path in CHANGED:
        bad=copy.deepcopy(current);bad[path]=('100644','0'*40)
        try:full['verify_tree'](bad,expected)
        except RuntimeError:pass
        else:raise AssertionError('Changed-source mutant accepted: '+path)
    old=ast.parse(git('show',BASE+':tools/host-pthread-authority/minimum_receipt.py').decode())
    new=ast.parse((ROOT/'tools/host-pthread-authority/minimum_receipt.py').read_text())
    native=lambda t:[ast.dump(n,include_attributes=False)for n in t.body if not isinstance(n,ast.FunctionDef)or n.name!='context']
    assert native(old)==native(new)
    print('NATIVE_OFF_SOURCE_PROOF exact_paths='+str(len(CHANGED))+' whole_tree='+str(len(current))+' scope=source_only',flush=True)
    return dict(expected=expected,current=current,full=full)
