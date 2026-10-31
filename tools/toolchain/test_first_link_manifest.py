"""FIRST_LINK_ONLY adversarial TEXT metadata fixtures, never native execution."""
import argparse
import copy
import json
from pathlib import Path, PurePosixPath
import shlex
import tempfile

import first_link as component
import first_link_context as focused
import prepare_host_pthread as factory
import selected_link as helper
from test_selected_link import projection


def historical_data(raw):
    assert helper.digest(raw)=='9fa310b69e9bc4a0c7081b8667c8dcb019481b07e644cb7875334d880a6886f8'
    data=projection(json.loads(raw));before=data['workspace']
    recipe=factory.recipe(focused.CALLER);fp=factory.fingerprint(recipe)
    after='/SYNTHETIC/gesher-host-pthread-selected-minimum-'+fp[:16]
    data=json.loads(json.dumps(data).replace(before,after))
    data['recipe']=recipe;data['recipe_fingerprint']=fp
    data['authority_sha256']=helper.digest(json.dumps(data['authority'],sort_keys=True,indent=2).encode())
    data['caller_authority']=dict(focused.TUPLE,source=data['context']['GITHUB_SHA'],
        policy_ref=focused.REPOSITORY+'/'+focused.POLICY_PATH+'@'+'2'*40)
    factory.check_metadata(data,data['context']);return data


def record(root,name,args,cwd,compiler,data,response=False):
    linked=name!='first-link-notlinked-diagnostic.exe'
    (root/name).write_bytes(('SYNTHETIC_TEXT_NOT_PE '+name).encode())
    member='libwinpthread_la-thread.o';library=data['compiler_root']+'/x86_64-w64-mingw32/lib/libwinpthread.a'
    trace='ld.lld: Reading '+library+'\nld.lld: Loaded libwinpthread.a('+member+') for pthread_self\n' if linked else 'ld.lld: Reading /SYNTHETIC/main.obj\n'
    owner=member if linked else '/SYNTHETIC/main.obj';symbol='pthread_self' if linked else 'main'
    mapping='Address Size Align Out In Symbol\n00001000 00000020 4096 .text\n00001000 00000020 16         '+owner+':(.text)\n00001000 00000000 0                 '+symbol+'\n'
    imports='File: SYNTHETIC_TEXT_NOT_PE\nFormat: COFF-x86-64\nName: KERNEL32.dll\n'
    value=dict(schema=helper.SCHEMA,selected_toolchain=compiler['selected_toolchain'],cwd=str(cwd),argv=args[:],expanded_argv=args[:],response_files=[],exit_code=0,
               output=dict(path=str(helper.link_paths(args,cwd,portable=True)[0]),sha256=factory.sha(root/name)),evidence={})
    if response:
        rsp=str(cwd/'first-link-diagnostics/response.rsp');body=component.gnu_response(args[1:])
        value['argv']=[args[0],'@'+rsp]
        value['response_files']=[dict(path=rsp,sha256=helper.digest(body),bytes=len(body),text=body.decode())]
    for kind,body in (('trace',trace),('map',mapping),('imports',imports)):
        path=root/(name+'.selected-pthread-'+kind+'.txt');path.write_bytes(body.encode())
        value['evidence'][kind]=dict(file=path.name,sha256=factory.sha(path))
    value['proof']=helper.prove(trace,mapping,imports,data,cwd)
    path=root/(name+'.selected-pthread-link.json');path.write_text(json.dumps(value,sort_keys=True,indent=2)+'\n',encoding='utf8')
    return dict(file=path.name,sha256=factory.sha(path),output_sha256=value['output']['sha256'],status=value['proof']['status'],
                receipt_file=component.RECEIPT,receipt_sha256=helper.digest((root/component.RECEIPT).read_bytes()))


def fixture(root,data):
    source=PurePosixPath('/SYNTHETIC/source');cwd=source/'out'
    raw=json.dumps(data,sort_keys=True,indent=2).encode();(root/component.RECEIPT).write_bytes(raw)
    compiler=dict(role='FIRST_LINK_ONLY_COMPILER',configured_drivers={language:data['compiler_root']+'/bin/x86_64-w64-mingw32-'+suffix for language,suffix in (('C','clang'),('CXX','clang++'))},
        generated_metadata={},selected_toolchain=helper.identity(data,raw),scope='SYNTHETIC_METADATA_ONLY')
    for language in ('C','CXX'):
        file='FIRST_LINK_ONLY_'+language+'Compiler.cmake';path=root/file
        path.write_text('set(CMAKE_'+language+'_COMPILER "'+compiler['configured_drivers'][language]+'")\n',encoding='utf8')
        compiler['generated_metadata'][language]=dict(file=file,path=str(cwd/'CMakeFiles/metadata-only'/('CMake'+language+'Compiler.cmake')),sha256=factory.sha(path))
    outputs={};commands={};logs=[];driver=compiler['configured_drivers']['CXX']
    for n,(target,path) in enumerate(component.TARGETS.items(),1):
        args=[driver,'-Wall','-Wextra','-Werror','-D_WIN32_WINNT=0x0A00','-DWINVER=0x0A00','-mno-ms-bitfields','-o','CMakeFiles/'+target+'.dir/'+path+'.obj','-c',str(source/path)]
        commands[target]=args;logs.append('['+str(n)+'/3] '+shlex.join(args))
        args=[driver,'-static','CMakeFiles/'+target+'.dir/'+path+'.obj','-o',target+'.exe','-Wl,--verbose','-Wl,-Map,'+str(cwd/(target+'.map')),'-Wl,--out-implib,lib'+target+'.dll.a']
        name=target+'.exe';outputs[name]=dict(kind='CMAKE_SMALL_OUTPUT',target=target,source=dict(file=path,sha256=factory.sha(component.ROOT/path)),link=record(root,name,args,cwd,compiler,data))
    original=json.loads((root/outputs['binderd.exe']['link']['file']).read_text())['expanded_argv'];diagnostic=cwd/'first-link-diagnostics'
    after,changed=component.response_args(original,diagnostic/'first-link-response-diagnostic.exe',diagnostic/'response.map',diagnostic/'response.dll.a',cwd,portable=True)
    name='first-link-response-diagnostic.exe';outputs[name]=dict(kind='RESPONSE_DIAGNOSTIC',source_target='binderd',changed_output_arg_indices=changed,
        original_expanded_argv=original,diagnostic_expanded_argv=after,link=record(root,name,after,cwd,compiler,data,response=True))
    name='first-link-notlinked-diagnostic.exe';args=[compiler['configured_drivers']['C'],'-o',str(diagnostic/name),'-Wl,--verbose','-Wl,-Map,'+str(diagnostic/'notlinked.map')]
    outputs[name]=dict(kind='NOTLINKED_DIAGNOSTIC',source=dict(file='probes/platform/platform_probe.c',sha256=factory.sha(component.ROOT/'probes/platform/platform_probe.c')),link=record(root,name,args,source,compiler,data))
    (root/'actual-small-build.log').write_text('\n'.join(logs)+'\n',encoding='utf8')
    for name in ('actual-response-diagnostic.log','actual-notlinked-diagnostic.log'):(root/name).write_text('SYNTHETIC_METADATA_LOG_NOT_ACTUAL_EXECUTION\n',encoding='utf8')
    manifest=dict(schema='gesher-first-link-only-v1',role=component.ROLE,scope=component.SCOPE,source_root=str(source),context=data['context'],
                  caller_authority=data['caller_authority'],producer_job='build',compiler=compiler,actual_completed_compile_commands=commands,outputs=outputs,
                  files={p.name:factory.sha(p) for p in root.iterdir()})
    (root/'first-link-only.json').write_text(json.dumps(manifest),encoding='utf8')
    return manifest


def tests(data):
    expected=data['context'];policy=data['caller_authority']['policy_ref']
    with tempfile.TemporaryDirectory(prefix='first-link-SYNTHETIC-text-') as temporary:
        root=Path(temporary);good=fixture(root,data)
        assert component.verify_files(root,expected,policy)==good
        mutations=[lambda x:x.update(role='production'),lambda x:x.update(scope='full runtime qualified'),
            lambda x:x.update(producer_job='compile'),lambda x:x['caller_authority'].update(policy_ref=policy[:-40]+'3'*40),
            lambda x:x['compiler']['configured_drivers'].update(CXX='/wrong/clang++'),
            lambda x:x['compiler']['generated_metadata'].pop('C'),
            lambda x:x['compiler']['generated_metadata']['CXX'].update(path='/wrong/CMakeCXXCompiler.cmake'),
            lambda x:x['actual_completed_compile_commands'].pop('binderd'),
            lambda x:x['actual_completed_compile_commands']['binderd'].append('UNOBSERVED_FLAG'),
            lambda x:x['outputs'].pop('binderd.exe'),lambda x:x['outputs']['binderd.exe'].update(kind='ART_RUNTIME'),
            lambda x:x['outputs']['binderd.exe']['source'].update(sha256='0'*64),
            lambda x:x['outputs']['first-link-response-diagnostic.exe'].update(changed_output_arg_indices=list(range(99))),
            lambda x:x['outputs']['first-link-response-diagnostic.exe']['diagnostic_expanded_argv'].append('-shared'),
            lambda x:x['outputs']['first-link-notlinked-diagnostic.exe']['link'].update(status='LINKED'),
            lambda x:x['files'].update({'missing-extra.txt':'0'*64})]
        for key in expected:
            mutations.append(lambda x,k=key:x['context'].update({k:'wrong-current-source-or-attempt'}))
        for mutate in mutations:
            bad=copy.deepcopy(good);mutate(bad);(root/'first-link-only.json').write_text(json.dumps(bad),encoding='utf8')
            try:component.verify_files(root,expected,policy)
            except (AssertionError,KeyError,ValueError,FileNotFoundError):pass
            else:raise AssertionError('Malformed FIRST_LINK_ONLY metadata accepted')
        (root/'first-link-only.json').write_text(json.dumps(good),encoding='utf8')
        receipt=root/component.RECEIPT;body=receipt.read_bytes();receipt.write_text('CORRUPTED_SYNTHETIC_RECEIPT')
        try:component.verify_files(root,expected,policy)
        except (AssertionError,ValueError):pass
        else:raise AssertionError('Wrong receipt bytes accepted')
        receipt.write_bytes(body)
        assert component.verify_files(root,expected,policy)==good
    print('PASS FIRST_LINK_ONLY exact5-output/31-file portable TEXT fixture; '+str(len(mutations)+1)+' source/run/policy/driver/role/response/receipt negatives; no compiler/network/native process')


if __name__=='__main__':
    parser=argparse.ArgumentParser();group=parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--historical-authority',type=Path);group.add_argument('--receipt',type=Path);args=parser.parse_args()
    if args.historical_authority:data=historical_data(args.historical_authority.read_bytes())
    else:
        data=json.loads(args.receipt.read_bytes());factory.check_metadata(data,data['context'])
        assert data['recipe']['caller']==focused.CALLER
    tests(data)
