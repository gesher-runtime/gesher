"""Real assembly/consumer function harness with TEXT markers, zero native work."""
from contextlib import redirect_stdout
import copy
import importlib.util
import io
import json
from pathlib import Path, PurePosixPath
import sys
import tempfile

import selected_link as helper
from test_selected_link import projection

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools/web3'))
import host_roles


def module(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT/relative)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def rejected(action):
    try:
        action()
    except (AssertionError, SystemExit, RuntimeError, ValueError, KeyError, FileNotFoundError):
        return
    raise AssertionError('Invalid assembled role accepted')


def snapshot(root):
    return {p.relative_to(root).as_posix():helper.factory.sha(p) for p in root.rglob('*') if p.is_file()}


def artifacts(root, role, executable, data, raw, compiler):
    root.mkdir()
    # These .exe-named files are plain ASCII metadata markers, not PE binaries.
    (root/executable).write_bytes(('SYNTHETIC_TEXT_NOT_PE '+role).encode())
    (root/host_roles.receipt_name(role)).write_bytes(raw)
    directory = '/SYNTHETIC/out/'+role
    original = 'dalvikvm.exe' if role=='query-hook' else executable
    argv = [data['compiler_root']+'/bin/x86_64-w64-mingw32-clang++', '-o', original,
            '-Wl,--verbose', '-Wl,-Map,'+original+'.map']
    texts = dict(trace='ld.lld: Reading '+directory+'/main.obj\n',
                 map='Address Size Align Out In Symbol\n00001000 00000020 4096 .text\n00001000 00000020 16         main.obj:(.text)\n00001000 00000000 0                 main\n',
                 imports='File: SYNTHETIC_TEXT_NOT_PE\nFormat: COFF-x86-64\nName: KERNEL32.dll\n')
    record = dict(schema=helper.SCHEMA, selected_toolchain=compiler['selected_toolchain'], cwd=directory,
                  argv=argv, expanded_argv=argv[:], response_files=[], exit_code=0,
                  output=dict(path=directory+'/'+original,sha256=helper.factory.sha(root/executable)), evidence={})
    for kind, text in texts.items():
        path=root/(executable+'.selected-pthread-'+kind+'.txt');path.write_bytes(text.encode())
        record['evidence'][kind]=dict(file=path.name,sha256=helper.factory.sha(path))
    record['proof']=helper.prove(texts['trace'],texts['map'],texts['imports'],data,PurePosixPath(directory))
    path=root/(executable+'.selected-pthread-link.json')
    path.write_text(json.dumps(record,sort_keys=True,indent=2)+'\n',encoding='utf-8')
    return {executable:dict(file=path.name,sha256=helper.factory.sha(path),output_sha256=record['output']['sha256'],
                           status='NOT-LINKED',receipt_sha256=helper.digest(raw),receipt_file=host_roles.receipt_name(role))}


def controls():
    historical=ROOT.parent/'gesher-ci-review/tcp-selected-411396-current-authority.json'
    raw=historical.read_bytes()
    assert helper.digest(raw)=='9fa310b69e9bc4a0c7081b8667c8dcb019481b07e644cb7875334d880a6886f8'
    data=projection(json.loads(raw));receipt=json.dumps(data,sort_keys=True,indent=2).encode()
    info=dict(source=data['context']['GITHUB_SHA'],run=data['context']['GITHUB_RUN_ID'],attempt=data['context']['GITHUB_RUN_ATTEMPT'],
              repository=data['context']['GITHUB_REPOSITORY'],repository_id=data['context']['GITHUB_REPOSITORY_ID'])
    compiler=dict(configured_driver=data['compiler_root']+'/bin/x86_64-w64-mingw32-clang++',launcher=None,
                  selected_toolchain=helper.identity(data,receipt))
    assembler=module('selected_actual_surface_assembler','tools/surface/assemble-artifacts.py')
    binder=module('selected_actual_binder_verifier','tools/binder/verify-full-host.py')
    with tempfile.TemporaryDirectory(prefix='gesher-role-assembly-text-') as temporary, redirect_stdout(io.StringIO()):
        temporary=Path(temporary)
        prod=temporary/'production';hook=temporary/'hook';actor=temporary/'actor'
        production=dict(info,role='production',test_hook_compiled=False,compiler=compiler,
                        selected_link=artifacts(prod,'production','dalvikvm.exe',data,receipt,compiler),
                        executable_sha256=None,selected_executables=['dalvikvm.exe'])
        production['executable_sha256']=helper.factory.sha(prod/'dalvikvm.exe')
        (prod/'web3-production-role.json').write_text(json.dumps(production),encoding='utf-8')
        hook_links=artifacts(hook,'query-hook',host_roles.HOOK,data,receipt,compiler)
        (hook/'web3-query-hook-role.json').write_text(json.dumps(dict(info,role='query-hook',compiler=compiler,selected_link=hook_links)),encoding='utf-8')
        actor_links=artifacts(actor,'binder-ordinary-actor','binder_sg_actor.exe',data,receipt,compiler)
        (actor/'binder-sg-runtime-source.txt').write_text(info['source']+'\n')
        source_names={'compat/posix/src/binder.cc','compat/posix/src/binder_sg.h','compat/posix/src/binder_wire.h',
                      'compat/binder/binderd.cc','cmake/aosp/servicemanager.cmake','tests/binder/sg-actor.cc',
                      'tests/binder/sg-actor.h','tests/binder/run-modes.ps1','tests/binder/OutputPageObserver.cs'}
        manifest=dict(info,role='binder-ordinary-actor',compiler=compiler,production_runtime_sha256=production['executable_sha256'],
                      actor_compile_command=[compiler['configured_driver'],'SYNTHETIC_SOURCE_METADATA'],actor_launcher=None,
                      files={p.name:helper.factory.sha(p) for p in actor.iterdir()},sources={p:helper.factory.sha(ROOT/p) for p in source_names},
                      selected_link=actor_links)
        path=actor/'binder-host-source.json';path.write_text(json.dumps(manifest),encoding='utf-8')
        # Actual current consumer order: isolated actor verification -> strict
        # assembler -> production-role inventory + portable verification.
        assert binder.verify_actor(actor,production,info)==manifest
        for secondary, owner in ((hook,'web3_query_hook'),(actor,'binder2_sg_host')):
            destination=temporary/('merged-'+owner);ledger=temporary/(owner+'.json')
            assembler.assemble(prod,destination,ledger,'base_windows_x64')
            assembler.assemble(secondary,destination,ledger,owner)
            targets=host_roles.production_targets(destination,production,info,hook=secondary==hook)
            assert targets=={'dalvikvm.exe'}, 'Secondary executable absorbed into production'
            host_roles.verify_selected(destination,production['selected_link'],compiler,info,targets,'production')
            if secondary==hook:
                host_roles.verify_selected(destination,hook_links,compiler,info,{host_roles.HOOK},'query-hook')
                wrong=copy.deepcopy(hook_links);wrong[host_roles.HOOK]['receipt_file']=host_roles.receipt_name('production')
                rejected(lambda:host_roles.verify_selected(destination,wrong,compiler,info,{host_roles.HOOK},'query-hook'))
                receipt_path=destination/host_roles.receipt_name('query-hook');saved=receipt_path.read_bytes()
                receipt_path.write_text('CORRUPT_SYNTHETIC_HOOK_RECEIPT')
                rejected(lambda:host_roles.verify_selected(destination,hook_links,compiler,info,{host_roles.HOOK},'query-hook'))
                receipt_path.write_bytes(saved)
            else:
                assert binder.verify_actor(destination,production,info,strict_payload=False)==manifest
            ownership=json.loads(ledger.read_text())
            assert ownership[host_roles.receipt_name('production')]['owner']=='base_windows_x64'
            assert ownership[host_roles.receipt_name('query-hook' if secondary==hook else 'binder-ordinary-actor')]['owner']==owner
            # Even identical generic duplicate bytes must still be rejected by
            # the unchanged assembler, before any destination mutation.
            collision=secondary/host_roles.receipt_name('production');collision.write_bytes(receipt)
            before=snapshot(destination);ledger_before=ledger.read_bytes()
            rejected(lambda:assembler.assemble(secondary,destination,ledger,owner))
            assert snapshot(destination)==before and ledger.read_bytes()==ledger_before
            collision.unlink()
        # Complete producer inventory survives the actor's actual merged order.
        merged=temporary/'merged-binder2_sg_host'
        bad=copy.deepcopy(production);bad['selected_executables'].append('binder_sg_actor.exe');bad['selected_executables'].sort()
        rejected(lambda:host_roles.production_targets(merged,bad,info))
        bad=copy.deepcopy(production);bad['selected_executables']=['missing.exe']
        rejected(lambda:host_roles.production_targets(merged,bad,info))
        (merged/'unverified.exe').write_text('SYNTHETIC_TEXT_NOT_PE')
        rejected(lambda:host_roles.production_targets(merged,production,info))
        (merged/'unverified.exe').unlink()
        # Wrong role receipt is rejected even if its factory bytes would match.
        bad=copy.deepcopy(manifest);bad['selected_link']['binder_sg_actor.exe']['receipt_file']=host_roles.receipt_name('production')
        path.write_text(json.dumps(bad),encoding='utf-8')
        rejected(lambda:binder.verify_actor(actor,production,info))
        path.write_text(json.dumps(manifest),encoding='utf-8')
        original=(actor/host_roles.receipt_name('binder-ordinary-actor')).read_bytes()
        (actor/host_roles.receipt_name('binder-ordinary-actor')).write_text('CORRUPT_SYNTHETIC_RECEIPT')
        rejected(lambda:binder.verify_actor(actor,production,info))
        (actor/host_roles.receipt_name('binder-ordinary-actor')).write_bytes(original)
        old=copy.deepcopy(info);old['run']='older'
        rejected(lambda:binder.verify_actor(actor,production,old))
        bad=copy.deepcopy(manifest);bad.pop('selected_link');path.write_text(json.dumps(bad),encoding='utf-8')
        rejected(lambda:binder.verify_actor(actor,production,info))
        path.write_text(json.dumps(manifest),encoding='utf-8')
        # Default publisher still uses the exact two-file actor payload and log.
        pub=temporary/'publisher';pub.mkdir()
        for name in ('binder_sg_actor.exe','binder-sg-runtime-source.txt'):(pub/name).write_bytes((actor/name).read_bytes())
        pub_manifest=copy.deepcopy(manifest);pub_manifest.pop('selected_link');pub_manifest['compiler'].pop('selected_toolchain')
        pub_manifest['files']={p.name:helper.factory.sha(p) for p in pub.iterdir()}
        pub_production=copy.deepcopy(production);pub_production['compiler']=pub_manifest['compiler'];pub_production.pop('selected_link');pub_production.pop('selected_executables')
        (pub/'binder-host-source.json').write_text(json.dumps(pub_manifest),encoding='utf-8')
        stream=io.StringIO()
        with redirect_stdout(stream):assert binder.verify_actor(pub,pub_production,info)==pub_manifest
        assert stream.getvalue()=='ACTUAL_BINDER_FULL_HOST '+json.dumps(pub_manifest,sort_keys=True)+'\n'
        assert set(pub_manifest['files'])=={'binder_sg_actor.exe','binder-sg-runtime-source.txt'}
    print('PASS actual assembler production->hook/Binder + real Binder pre/post verification; role receipts distinct; 2 atomic collision +3 inventory +6 actor/hook/context/receipt negatives; publisher exact2-file/log; TEXT ONLY native_processes=0')


if __name__=='__main__':controls()
