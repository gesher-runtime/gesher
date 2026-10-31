"""Explicit first production caller; fresh SDKs, no mutation of publisher cache."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import shlex
import subprocess
import sys
import tarfile
import first_link_context as focused

ROOT=Path(__file__).resolve().parents[2]
CORE=ROOT/'tools/host-pthread-authority'
sys.path.insert(0,str(CORE))
import minimum_receipt as minimum

NAME='llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64'
URL='https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/'+NAME+'.tar.xz'
FIELDS=('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_REPOSITORY','GITHUB_REPOSITORY_ID')
ROLES={'publisher-control':'publisher-reference-unmodified-v1','selected-minimum':minimum.ROLE}
CORE_FILES=['tools/host-pthread-authority/'+p for p in ('build-minimum-supplier.sh','minimum_receipt.py','minimum-inputs.json','minimum-detached-retirement.patch','minimum-noattach-join.patch')]


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def canonical(value):return json.dumps(value,sort_keys=True,separators=(',',':')).encode('utf-8')


def caller(env,selection):
    if env.get('GESHER_HOST_PTHREAD_CALLER') == 'coherent-selected-full/build':
        import coherent_selected_context as coherent
        return coherent.context(env,selection)
    if env.get('GESHER_HOST_PTHREAD_CALLER') == 'pthread-native-off-receiver/build':
        import native_off_receiver_context as native
        return native.context(env,selection)
    if env.get('GESHER_HOST_PTHREAD_CALLER') == focused.CALLER:
        return focused.context(env, selection)
    assert selection in ROLES and env.get('GESHER_HOST_PTHREAD_SELECTION')==selection
    assert env.get('GITHUB_ACTIONS')=='true'
    assert env['GITHUB_REF']=='refs/heads/wip/pthread-production-factory'
    assert env['GITHUB_WORKFLOW']=='ci' and env['GITHUB_JOB']=='build'
    assert env.get('GESHER_HOST_PTHREAD_CALLER')=='ci/build'
    result={k:env[k] for k in FIELDS}
    assert result['GITHUB_REPOSITORY']=='gesher-runtime/gesher' and result['GITHUB_REPOSITORY_ID']=='1426153186'
    assert re.fullmatch('[0-9a-f]{40}',result['GITHUB_SHA'])
    assert all(re.fullmatch('[1-9][0-9]*',result[k]) for k in ('GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT'))
    return result


def recipe(caller_key='ci/build'):
    assert caller_key in ('ci/build', focused.CALLER, 'pthread-native-off-receiver/build', 'coherent-selected-full/build')
    paths=CORE_FILES+['tools/toolchain/prepare_host_pthread.py']
    # Both exact callers import this closed tuple; bind the shared policy source
    # even when preserving the original production caller/default semantics.
    paths+=['tools/toolchain/first_link_context.py']
    if caller_key=='pthread-native-off-receiver/build':paths+=['tools/toolchain/native_off_receiver_context.py']
    if caller_key=='coherent-selected-full/build':paths+=['tools/toolchain/coherent_selected_context.py']
    result=dict(schema='gesher-original-profile-pthread-recipe-v1',caller=caller_key,pin=json.loads(json.dumps(minimum.PIN)),
                core_sources={p:sha(ROOT/p) for p in paths},
                selected_role=minimum.ROLE,publisher_role=ROLES['publisher-control'],
                supplier_cflags='-g -O2 -mguard=cf -Werror -Xclang -fdump-record-layouts',
                consumer_flags='unchanged product Win10; not supplier configure flags',
                authority_policy='fresh archive; typed SDK601 before make; actual480/allfields; original public headers/aliases/exports/imports and actual DLL source object')
    if caller_key == 'coherent-selected-full/build':
        import coherent_selected_context as coherent
        result['coherent_selected_caller']=coherent.TUPLE
    if caller_key == focused.CALLER:result['focused_caller']=focused.TUPLE
    if caller_key=='pthread-native-off-receiver/build':
        import native_off_receiver_context as native
        result['native_off_receiver_caller']=native.TUPLE
    return result


def fingerprint(value):return hashlib.sha256(canonical(value)).hexdigest()


def check_metadata(data,context):
    assert data['schema']=='gesher-host-toolchain-v1' and data['context']==context
    mode=data['selection'];assert mode in ROLES and data['role']==ROLES[mode]
    assert data['recipe']==recipe(data['recipe']['caller']) and data['recipe_fingerprint']==fingerprint(data['recipe'])
    if data['recipe']['caller']=='coherent-selected-full/build':
        import coherent_selected_context as coherent
        assert mode=='selected-minimum'
        coherent.check_descriptor(data['caller_authority'],context)
    elif data['recipe']['caller']==focused.CALLER:
        assert mode=='selected-minimum'
        focused.check_descriptor(data['caller_authority'],context)
    elif data['recipe']['caller']=='pthread-native-off-receiver/build':
        import native_off_receiver_context as native
        assert mode=='selected-minimum'
        native.check_descriptor(data['caller_authority'],context)
    else:assert 'caller_authority' not in data
    assert data['archive']==dict(url=URL,sha256=minimum.PIN['compiler_archive_sha256'],bytes=84003044)
    root=PurePosixPath(data['workspace']);assert root.is_absolute() and posixpath.normpath(str(root))==str(root)
    assert root.name=='gesher-host-pthread-'+mode+'-'+data['recipe_fingerprint'][:16]
    assert data['compiler_root']==str(root/NAME) and data['source_root']==str(root/'source')
    assert data['source_commit']==minimum.PIN['upstream_commit']
    before=data['source_before'];after=data['source_after'];assert set(before)==set(after)
    for name,value in minimum.PIN['source_sha256'].items():assert before[name]==value
    wanted=minimum.PIN['output_source_sha256'] if mode=='selected-minimum' else minimum.PIN['source_sha256']
    for name,value in wanted.items():assert after[name]==value
    assert {p for p in before if before[p]!=after[p]}==({'mingw-w64-libraries/winpthreads/src/thread.c'} if mode=='selected-minimum' else set())
    authority=data['authority'];assert hashlib.sha256(json.dumps(authority,sort_keys=True,indent=2).encode()).hexdigest()==data['authority_sha256']
    ctx={k:context[k] for k in FIELDS[:4]}
    if mode=='selected-minimum':
        assert authority['role']==minimum.ROLE and all(authority[k]==v for k,v in ctx.items())
        assert authority['pin']==minimum.PIN and authority['source_files']==minimum.PIN['output_source_sha256']
        minimum.validate_layout(authority['private_record_layout'])
        original=authority['original'];assert original['context']==ctx and original['role']==ROLES['publisher-control']
        assert original['compiler_archive_sha256']==minimum.PIN['compiler_archive_sha256']
        assert original['files']['lib/libpthread.a']==original['files']['lib/libwinpthread.a']==minimum.PIN['publisher_static_sha256']
        assert original['files']['lib/libpthread.dll.a']==original['files']['lib/libwinpthread.dll.a']
        assert authority['original_profile']['sdk']==authority['original_profile']['supplier']
        assert set(authority['original_profile']['sdk'])=={'_WIN32_WINNT','WINVER'}
        assert authority['original_profile']['sdk']['_WIN32_WINNT']==0x0601 and authority['original_profile']['sdk']['WINVER']>=0x0600
        assert authority['public_headers']==original['public_headers']
        assert minimum.exports(authority['dll_exports'])==original['dll_exports']
        assert minimum.exports(original['dll_exports_raw'])==original['dll_exports']
        assert minimum.imports(authority['dll_imports'])==original['dll_import_names'] and re.search(r'\bGetThreadId\b',authority['dll_imports'])
        assert minimum.imports(original['dll_imports_raw'])==original['dll_import_names']
        assert all(authority[k]==original[k] for k in ('compiler_driver_sha256','cxx_driver_sha256'))
        assert set(authority['dll_symbol_objects'])==set(minimum.CORE)
        assert len({r['actual_reading_path'] for r in authority['dll_symbol_objects'].values()})==1
        assert len({r['sha256'] for r in authority['dll_symbol_objects'].values()})==1
        object_paths=set()
        for r in authority['dll_symbol_objects'].values():
            assert PurePosixPath(r['actual_reading_path']).is_relative_to(root/'source/build')
            assert posixpath.normpath(r['actual_reading_path'])==r['actual_reading_path']
            assert r['actual_reading_path'].endswith('thread.o') and re.fullmatch('[0-9a-f]{64}',r['sha256'])
            assert re.fullmatch('[0-9a-fA-F]+',r['rva']);object_paths.add(r['actual_reading_path'])
            assert posixpath.normpath(posixpath.join(str(root/'source/build'),r['input']))==r['actual_reading_path']
        compiled=set();assert authority['dll_compile_commands']
        for command in authority['dll_compile_commands']:
            argv=shlex.split(command);assert str(root/NAME/'bin/x86_64-w64-mingw32-clang') in argv
            assert all(f in argv for f in ('-DHAVE_CONFIG_H','-DIN_WINPTHREAD','-DDLL_EXPORT','-DPIC','-g','-O2','-mguard=cf','-Werror','-Xclang','-fdump-record-layouts','-c','-o'))
            assert not any(re.match(r'-D(?:GESHER_|WINPTHREAD_ROUTING|_WIN32_WINNT|WINVER)',f) for f in argv)
            compiled.add(posixpath.normpath(posixpath.join(str(root/'source/build'),argv[argv.index('-o')+1])))
            assert posixpath.normpath(posixpath.join(str(root/'source/build'),argv[argv.index('-c')+1]))==str(root/'source/mingw-w64-libraries/winpthreads/src/thread.c')
        assert object_paths<=compiled
        assert authority['files']['lib/libwinpthread.a']!=original['files']['lib/libwinpthread.a']
        assert set(authority['library_symbols'])=={'lib/libwinpthread.a','lib/libwinpthread.dll.a'}
        for raw in authority['library_symbols'].values():assert 'gesher_' not in raw.lower() and 'pthread_join' in raw and '_pthread_tryjoin' in raw
        assert set(authority['build_logs'])=={'configure-command.log','build-commands.log','install-commands.log','config.log','supplier-dll.map','sdk-profile-macros.txt','supplier-profile-macros.txt','sdk-profile-machine.txt','supplier-profile-machine.txt','sdk-profile.c','sdk-profile.obj','supplier-profile.obj'}
        assert all(re.fullmatch('[0-9a-f]{64}',v) for v in authority['build_logs'].values())
    else:
        assert authority['context']==ctx and authority['role']==ROLES[mode]
        assert authority['files']['lib/libwinpthread.a']==minimum.PIN['publisher_static_sha256']
    assert authority['compiler_archive_sha256']==minimum.PIN['compiler_archive_sha256']
    files=authority['files'];assert set(files)=={'lib/libwinpthread.a','lib/libpthread.a','lib/libwinpthread.dll.a','lib/libpthread.dll.a','bin/libwinpthread-1.dll'}
    assert files['lib/libpthread.a']==files['lib/libwinpthread.a'] and files['lib/libpthread.dll.a']==files['lib/libwinpthread.dll.a']
    assert all(re.fullmatch('[0-9a-f]{64}',v) for v in files.values())
    assert set(authority['public_headers'])=={'pthread.h','pthread_compat.h','pthread_signal.h','pthread_time.h','pthread_unistd.h','sched.h','semaphore.h'}
    assert authority['public_headers']['pthread.h']==minimum.PIN['source_sha256']['mingw-w64-libraries/winpthreads/include/pthread.h']
    assert all(re.fullmatch('[0-9a-f]{64}',v) for v in authority['public_headers'].values())


def check_files(data):
    llvm=Path(data['compiler_root']);source=Path(data['source_root']);a=data['authority']
    assert sha(llvm.parent/'compiler.tar.xz')==data['archive']['sha256']
    for name,value in a['files'].items():assert sha(llvm/'x86_64-w64-mingw32'/name)==value
    for name,value in a['public_headers'].items():assert sha(llvm/'x86_64-w64-mingw32/include'/name)==value
    assert sha(llvm/'bin/x86_64-w64-mingw32-clang')==a['compiler_driver_sha256']
    assert sha(llvm/'bin/x86_64-w64-mingw32-clang++')==a['cxx_driver_sha256']
    for name,value in data['source_after'].items():assert sha(source/name)==value
    if data['selection']=='selected-minimum':
        assert json.loads((llvm/'gesher-winpthreads-authority.json').read_text())==a
        for row in a['dll_symbol_objects'].values():assert sha(row['actual_reading_path'])==row['sha256']
        for name,value in a['build_logs'].items():assert sha(source/'build'/name)==value
    else:assert not (llvm/'gesher-winpthreads-authority.json').exists()


def run(argv,cwd,log):
    print('PRODUCTION_TOOLCHAIN_COMMAND '+json.dumps(dict(argv=[str(x) for x in argv],cwd=str(cwd),log=str(log))),flush=True)
    with log.open('wb') as out:
        result=subprocess.run([str(x) for x in argv],cwd=cwd,stdout=out,stderr=subprocess.STDOUT)
    print(log.read_text(errors='replace'),end='',flush=True)
    result.check_returncode()


def prepare(selection):
    context=caller(os.environ,selection)
    assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()==context['GITHUB_SHA']
    subprocess.check_call(['git','diff','--exit-code','HEAD','--'],cwd=ROOT)
    definition=recipe(os.environ['GESHER_HOST_PTHREAD_CALLER']);fp=fingerprint(definition)
    temporary=Path(os.environ['RUNNER_TEMP']).resolve();workspace=temporary/('gesher-host-pthread-'+selection+'-'+fp[:16])
    assert workspace.is_relative_to(temporary) and not workspace.exists();workspace.mkdir()
    archive=workspace/'compiler.tar.xz'
    run(['curl','--fail','--location','--connect-timeout','20','--max-time','180',URL,'-o',archive],ROOT,workspace/'archive-download.log')
    assert archive.stat().st_size==84003044 and sha(archive)==minimum.PIN['compiler_archive_sha256']
    with tarfile.open(archive,'r:xz') as tar:
        assert {Path(m.name).parts[0] for m in tar.getmembers()}=={NAME}
        tar.extractall(workspace,filter='data')
    llvm=workspace/NAME;source=workspace/'source';source.mkdir()
    run(['git','init','-q'],source,workspace/'source-init.log')
    run(['git','fetch','--depth=1','https://github.com/mingw-w64/mingw-w64.git',minimum.PIN['upstream_commit']],source,workspace/'source-fetch.log')
    run(['git','checkout','--detach','FETCH_HEAD'],source,workspace/'source-checkout.log')
    names=subprocess.check_output(['git','ls-files','-z','mingw-w64-libraries/winpthreads'],cwd=source,text=True).rstrip('\0').split('\0')
    before={name:sha(source/name) for name in names}
    if selection=='selected-minimum':
        run(['bash',CORE/'build-minimum-supplier.sh',llvm,source],ROOT,workspace/'factory.log')
        authority=json.loads((llvm/'gesher-winpthreads-authority.json').read_text())
    else:
        run([sys.executable,CORE/'minimum_receipt.py','inputs',source],ROOT,workspace/'source-inputs.log')
        run([sys.executable,CORE/'minimum_receipt.py','original',llvm,source,source/'publisher-reference.json'],ROOT,workspace/'publisher-control.log')
        authority=json.loads((source/'publisher-reference.json').read_text())
    data=dict(schema='gesher-host-toolchain-v1',selection=selection,role=ROLES[selection],context=context,
              recipe=definition,recipe_fingerprint=fp,workspace=str(workspace),compiler_root=str(llvm),source_root=str(source),
              source_commit=minimum.PIN['upstream_commit'],source_before=before,source_after={n:sha(source/n) for n in names},
              archive=dict(url=URL,sha256=sha(archive),bytes=archive.stat().st_size),authority=authority,
              authority_sha256=hashlib.sha256(json.dumps(authority,sort_keys=True,indent=2).encode()).hexdigest())
    if definition['caller']=='coherent-selected-full/build':
        import coherent_selected_context as coherent
        data['caller_authority']=coherent.descriptor(os.environ)
    if definition['caller']==focused.CALLER:data['caller_authority']=focused.descriptor(os.environ)
    if definition['caller']=='pthread-native-off-receiver/build':
        import native_off_receiver_context as native
        data['caller_authority']=native.descriptor(os.environ)
    check_metadata(data,context);check_files(data)
    receipt=workspace/'production-toolchain-receipt.json';receipt.write_text(json.dumps(data,sort_keys=True,indent=2))
    summary=dict(source=context['GITHUB_SHA'],run=context['GITHUB_RUN_ID'],attempt=context['GITHUB_RUN_ATTEMPT'],selection=selection,
                 role=data['role'],recipe_fingerprint=fp,compiler_root=str(llvm),receipt=str(receipt),receipt_sha256=sha(receipt))
    print('PRODUCTION_TOOLCHAIN_RECEIPT '+json.dumps(summary,sort_keys=True),flush=True)
    with Path(os.environ['GITHUB_OUTPUT']).open('a') as out:
        for key,value in [('compiler_root',str(llvm)),('receipt',str(receipt)),('receipt_sha256',sha(receipt)),('recipe_fingerprint',fp)]:out.write(key+'='+value+'\n')


def main():
    parser=argparse.ArgumentParser();group=parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--selection',choices=tuple(ROLES));group.add_argument('--verify',type=Path);args=parser.parse_args()
    if args.selection:prepare(args.selection)
    else:
        data=json.loads(args.verify.read_text());ctx=caller(os.environ,data['selection'])
        check_metadata(data,ctx);check_files(data)
        print('Verified explicit current product-caller toolchain recipe/authority/files before configure; no full-product adoption inferred')


if __name__=='__main__':main()
