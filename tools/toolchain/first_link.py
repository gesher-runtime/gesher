"""CI-only FIRST_LINK_ONLY actual small outputs; no ART/production role fiction."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import subprocess
import sys

import first_link_context as focused
import prepare_host_pthread as factory
import selected_link as link

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/web3'))
import host_roles

ROLE='FIRST_LINK_ONLY'
SCOPE='cross-link + portable evidence ONLY; no runtime or full product qualification'
RECEIPT='selected-pthread-FIRST_LINK_ONLY-toolchain.json'
TARGETS={'binderd':'compat/binder/binderd.cc',
         'native3_lock_command_worker':'tests/native3-locks/command_worker.cc',
         'native3_lock_duplicate_test':'tests/native3-locks/duplicate_windows_test.cc'}
KINDS={name+'.exe':'CMAKE_SMALL_OUTPUT' for name in TARGETS}
KINDS.update({'first-link-response-diagnostic.exe':'RESPONSE_DIAGNOSTIC',
              'first-link-notlinked-diagnostic.exe':'NOTLINKED_DIAGNOSTIC'})


def emit(label,value,raw=None):
    raw=json.dumps(value,sort_keys=True).encode() if raw is None else raw
    assert json.loads(raw)==value
    for line in host_roles.ledger_lines(raw,label):print(line,flush=True)


def policy_ref():
    text=(ROOT/focused.PATH).read_text(encoding='utf-8')
    pins=re.findall(r'^\s*uses: ('+re.escape(focused.REPOSITORY+'/'+focused.POLICY_PATH)+r'@[0-9a-f]{40})\s*$',text,re.MULTILINE)
    assert len(pins)==1,'Missing/ambiguous immutable policy reference'
    return pins[0]


def current(consumer=False):
    ctx=focused.consumer_context(os.environ,'selected-minimum') if consumer else focused.context(os.environ,'selected-minimum')
    assert os.environ['GESHER_FIRST_LINK_POLICY_REF']==policy_ref()
    assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()==ctx['GITHUB_SHA']
    subprocess.check_call(['git','diff','--exit-code','HEAD','--'],cwd=ROOT)
    run=json.loads(subprocess.check_output(['gh','api','repos/'+focused.REPOSITORY+'/actions/runs/'+ctx['GITHUB_RUN_ID']],text=True))
    observed={k:run.get(k) for k in ('id','run_attempt','head_sha','head_branch','path','name','event','actor','triggering_actor','referenced_workflows')}
    emit('FIRST_LINK_LIVE_RUN',observed)
    assert str(run['id'])==ctx['GITHUB_RUN_ID'] and str(run['run_attempt'])==ctx['GITHUB_RUN_ATTEMPT']
    assert run['head_sha']==ctx['GITHUB_SHA'] and run['head_branch']==focused.BRANCH
    assert run['path']==focused.PATH and run['name']==focused.WORKFLOW and run['event']==os.environ['GITHUB_EVENT_NAME']
    assert run['repository']['full_name']==focused.REPOSITORY
    assert str(run['repository']['id'])==str(run['head_repository']['id'])==focused.REPOSITORY_ID
    assert run['actor']['login']==run['triggering_actor']['login']=='palmoni5'
    refs=[r for r in run['referenced_workflows'] if r['path']==policy_ref()]
    assert len(refs)==1 and refs[0]['sha']==policy_ref().rsplit('@',1)[1]
    jobs=json.loads(subprocess.check_output(['gh','api','repos/'+focused.REPOSITORY+'/actions/runs/'+ctx['GITHUB_RUN_ID']+'/attempts/'+ctx['GITHUB_RUN_ATTEMPT']+'/jobs?per_page=100'],text=True))
    rows=jobs['jobs'];assert jobs['total_count']==len(rows)<=100
    emit('FIRST_LINK_LIVE_JOBS',dict(context=ctx,jobs=[{k:r.get(k) for k in ('id','name','head_sha','run_id','run_attempt','status','conclusion')} for r in rows]))
    assert {r['name'] for r in rows}<={'source_authority / verify','build','portable-windows-2022','portable-windows-2025'}
    for name in ('source_authority / verify','build') if consumer else ('source_authority / verify',):
        selected=[r for r in rows if r['name']==name]
        assert len(selected)==1
        job=selected[0]
        assert job['status']=='completed' and job['conclusion']=='success'
        assert job['head_sha']==ctx['GITHUB_SHA'] and str(job['run_id'])==ctx['GITHUB_RUN_ID'] and str(job['run_attempt'])==ctx['GITHUB_RUN_ATTEMPT']
    return ctx


def run(argv,cwd,log):
    emit('FIRST_LINK_COMMAND',dict(argv=[str(a) for a in argv],cwd=str(cwd),log=str(log)))
    with log.open('wb') as output:
        child=subprocess.Popen([str(a) for a in argv],cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        for body in iter(lambda:child.stdout.read(65536),b''):
            output.write(body);sys.stdout.buffer.write(body);sys.stdout.buffer.flush()
        result=child.wait()
    assert result==0,('Actual first-link command failed',result)


def compiler_record(build,out,data,raw):
    actual={};metadata={}
    for language,suffix in (('C','clang'),('CXX','clang++')):
        files=list((build/'CMakeFiles').glob('*/CMake'+language+'Compiler.cmake'))
        assert len(files)==1
        content=files[0].read_text(encoding='utf-8')
        matches=re.findall(r'^\s*set\(CMAKE_'+language+r'_COMPILER\s+"([^"]+)"\)\s*$',content,re.MULTILINE)
        assert matches==[data['compiler_root']+'/bin/x86_64-w64-mingw32-'+suffix]
        destination=out/('FIRST_LINK_ONLY_'+language+'Compiler.cmake');shutil.copyfile(files[0],destination)
        actual[language]=matches[0];metadata[language]=dict(path=str(files[0]),file=destination.name,sha256=factory.sha(destination))
    return dict(role='FIRST_LINK_ONLY_COMPILER',configured_drivers=actual,generated_metadata=metadata,
                selected_toolchain=link.identity(data,raw),scope='actual small outputs; no query/ART compile or runtime claim')


def response_args(original,output,map_path,implib,cwd,portable=False):
    result=list(original);assert result.count('-o')==1
    changed={result.index('-o')+1};result[result.index('-o')+1]=str(output)
    maps=0
    for index,arg in enumerate(result):
        if not arg.startswith('-Wl,'):continue
        parts=arg[4:].split(',')
        for n,part in enumerate(parts):
            if part=='-Map':parts[n+1]=str(map_path);maps+=1;changed.add(index)
            elif part.startswith(('-Map=','--Map=')):parts[n]=part.split('=',1)[0]+'='+str(map_path);maps+=1;changed.add(index)
            elif part=='--out-implib':parts[n+1]=str(implib);changed.add(index)
            elif part.startswith('--out-implib='):parts[n]='--out-implib='+str(implib);changed.add(index)
            elif part.startswith(('--pdb','--output-def','--dependency-file','--reproduce')):
                raise AssertionError('Unsupported additional output in diagnostic relink')
        result[index]='-Wl,'+','.join(parts)
    assert maps==1 and all(a==b for n,(a,b) in enumerate(zip(original,result)) if n not in changed)
    assert link.link_paths(result,cwd,portable=portable)[0]==output
    return result,sorted(changed)


def gnu_response(args):
    return ('\n'.join('"'+a.replace('\\','\\\\').replace('"','\\"')+'"' for a in args)+'\n').encode('utf-8')


def completed_commands(log,compiler,source_root=ROOT):
    result={}
    for line in log.splitlines():
        match=re.fullmatch(r'\[[0-9]+/[0-9]+\] (.*)',line)
        if not match:continue
        args=shlex.split(match[1])
        if '-c' not in args or '-o' not in args:continue
        source=args[args.index('-c')+1]
        for target,relative in TARGETS.items():
            if source==str(source_root/relative):
                assert target not in result
                assert compiler['configured_drivers']['CXX'] in args
                assert 'CMakeFiles/'+target+'.dir/' in args[args.index('-o')+1]
                result[target]=args
    assert set(result)==set(TARGETS),'Missing/ambiguous actual completed target compile commands'
    return result


def build():
    context=current();data,raw=link.authority()
    assert data['context']==context and data['recipe']['caller']==focused.CALLER
    build=ROOT/'out';out=ROOT/'dist-first-link-only'
    assert build.is_dir() and not out.exists();out.mkdir()
    compiler=compiler_record(build,out,data,raw)
    log=out/'actual-small-build.log'
    run(['ninja','-C',build,'-v',*TARGETS],ROOT,log)
    actual=completed_commands(log.read_text(encoding='utf-8'),compiler)
    outputs={}
    for target in TARGETS:
        original=build/(target+'.exe');published=out/original.name;shutil.copyfile(original,published)
        outputs[published.name]=dict(kind='CMAKE_SMALL_OUTPUT',target=target,
            source=dict(file=TARGETS[target],sha256=factory.sha(ROOT/TARGETS[target])),
            link=link.seal(original,published,compiler,RECEIPT))
        assert outputs[published.name]['link']['status']=='LINKED'
    diagnostic=build/'first-link-diagnostics';assert not diagnostic.exists();diagnostic.mkdir()
    original=json.loads((build/'binderd.exe.selected-pthread-link.json').read_text())
    protected=[build/'binderd.exe',build/'binderd.exe.selected-pthread-link.json']+[Path(r['path']) for r in original['evidence'].values()]
    for argument in original['expanded_argv']:
        if not argument.startswith('-Wl,'):continue
        parts=argument[4:].split(',')
        for n,part in enumerate(parts):
            value=parts[n+1] if part=='--out-implib' else part.split('=',1)[1] if part.startswith('--out-implib=') else None
            if value is not None:
                path=(build/value).resolve();assert path.is_relative_to(build)
                if path.exists():protected.append(path)
    protected_hashes={str(p):factory.sha(p) for p in protected}
    response_output=diagnostic/'first-link-response-diagnostic.exe'
    response,changed=response_args(original['expanded_argv'],response_output,diagnostic/'response.map',diagnostic/'response.dll.a',build)
    rsp=diagnostic/'response.rsp';rsp.write_bytes(gnu_response(response[1:]))
    expanded,_=link.expanded([response[0],'@'+str(rsp)],build)
    assert expanded==response
    run([sys.executable,ROOT/'tools/toolchain/selected_link.py','launch',response[0],'@'+str(rsp)],build,out/'actual-response-diagnostic.log')
    assert all(factory.sha(p)==h for p,h in protected_hashes.items()),'Response diagnostic changed original final evidence'
    published=out/response_output.name;shutil.copyfile(response_output,published)
    outputs[published.name]=dict(kind='RESPONSE_DIAGNOSTIC',source_target='binderd',changed_output_arg_indices=changed,
        original_expanded_argv=original['expanded_argv'],diagnostic_expanded_argv=response,
        link=link.seal(response_output,published,compiler,RECEIPT))
    assert outputs[published.name]['link']['status']=='LINKED'
    native=diagnostic/'first-link-notlinked-diagnostic.exe'
    argv=[compiler['configured_drivers']['C'],'-std=gnu17','-static','-mno-ms-bitfields','-D_WIN32_WINNT=0x0A00','-DWINVER=0x0A00',
          '-Wall','-Wextra','-Werror',str(ROOT/'probes/platform/platform_probe.c'),'-lsynchronization','-Wl,--verbose',
          '-Wl,-Map,'+str(diagnostic/'notlinked.map'),'-o',str(native)]
    run([sys.executable,ROOT/'tools/toolchain/selected_link.py','launch',*argv],ROOT,out/'actual-notlinked-diagnostic.log')
    published=out/native.name;shutil.copyfile(native,published)
    outputs[published.name]=dict(kind='NOTLINKED_DIAGNOSTIC',source=dict(file='probes/platform/platform_probe.c',sha256=factory.sha(ROOT/'probes/platform/platform_probe.c')),
        link=link.seal(native,published,compiler,RECEIPT))
    assert outputs[published.name]['link']['status']=='NOT-LINKED'
    manifest=dict(schema='gesher-first-link-only-v1',role=ROLE,scope=SCOPE,source_root=str(ROOT),
                  context=context,caller_authority=focused.descriptor(os.environ),producer_job='build',compiler=compiler,
                  actual_completed_compile_commands=actual,outputs=outputs,
                  files={p.name:factory.sha(p) for p in out.iterdir()})
    path=out/'first-link-only.json';path.write_text(json.dumps(manifest,sort_keys=True,indent=2)+'\n',encoding='utf-8')
    verify_files(out,context,os.environ['GESHER_FIRST_LINK_POLICY_REF'])
    emit('FIRST_LINK_ONLY_MANIFEST',manifest,path.read_bytes())
    for name,row in outputs.items():
        record=(out/row['link']['file']).read_bytes();emit('FIRST_LINK_ONLY_TARGET',json.loads(record),record)


def verify_files(root,expected,expected_policy):
    assert root.is_dir() and not root.is_symlink()
    manifest=json.loads((root/'first-link-only.json').read_text(encoding='utf-8'))
    assert manifest['schema']=='gesher-first-link-only-v1' and manifest['role']==ROLE and manifest['scope']==SCOPE
    assert manifest['context']==expected and manifest['producer_job']=='build'
    focused.check_descriptor(manifest['caller_authority'],expected)
    assert manifest['caller_authority']['policy_ref']==expected_policy
    assert set(manifest['outputs'])==set(KINDS) and set(manifest['actual_completed_compile_commands'])==set(TARGETS)
    required=set(KINDS)|{name+'.selected-pthread-'+tail for name in KINDS for tail in ('link.json','trace.txt','map.txt','imports.txt')}
    required|={RECEIPT,'actual-small-build.log','actual-response-diagnostic.log','actual-notlinked-diagnostic.log',
               'FIRST_LINK_ONLY_CCompiler.cmake','FIRST_LINK_ONLY_CXXCompiler.cmake'}
    assert set(manifest['files'])==required
    assert {p.name for p in root.iterdir()}==set(manifest['files'])|{'first-link-only.json'}
    for name,digest in manifest['files'].items():
        assert Path(name).name==name and (root/name).is_file() and not (root/name).is_symlink()
        assert factory.sha(root/name)==digest
    receipt=(root/RECEIPT).read_bytes();data=json.loads(receipt)
    factory.check_metadata(data,expected)
    assert data['recipe']['caller']==focused.CALLER and data['caller_authority']==manifest['caller_authority']
    compiler=manifest['compiler'];assert compiler['role']=='FIRST_LINK_ONLY_COMPILER'
    assert compiler['selected_toolchain']==link.identity(data,receipt)
    assert compiler['configured_drivers']=={lang:data['compiler_root']+'/bin/x86_64-w64-mingw32-'+suffix for lang,suffix in (('C','clang'),('CXX','clang++'))}
    assert set(compiler['generated_metadata'])=={'C','CXX'}
    source=PurePosixPath(manifest['source_root']);assert source.is_absolute() and '..' not in source.parts and str(source)==manifest['source_root']
    for language,metadata in compiler['generated_metadata'].items():
        assert metadata['file']=='FIRST_LINK_ONLY_'+language+'Compiler.cmake'
        original=PurePosixPath(metadata['path'])
        assert original.is_relative_to(source/'out/CMakeFiles') and '..' not in original.parts
        assert original.name=='CMake'+language+'Compiler.cmake'
        path=root/metadata['file'];assert factory.sha(path)==metadata['sha256']
        matches=re.findall(r'^\s*set\(CMAKE_'+language+r'_COMPILER\s+"([^"]+)"\)\s*$',path.read_text(encoding='utf-8'),re.MULTILINE)
        assert matches==[compiler['configured_drivers'][language]]
    for target,args in manifest['actual_completed_compile_commands'].items():
        assert isinstance(args,list) and compiler['configured_drivers']['CXX'] in args and '-c' in args and '-o' in args
        assert all(flag in args for flag in ('-Wall','-Wextra','-Werror','-mno-ms-bitfields','-D_WIN32_WINNT=0x0A00','-DWINVER=0x0A00'))
        assert args[args.index('-c')+1]==str(source/TARGETS[target])
        assert 'CMakeFiles/'+target+'.dir/' in args[args.index('-o')+1]
    assert completed_commands((root/'actual-small-build.log').read_text(encoding='utf-8'),compiler,source)==manifest['actual_completed_compile_commands']
    records={}
    for name,row in manifest['outputs'].items():
        assert row['kind']==KINDS[name]
        record=link.verify_sealed(root,row['link'],compiler,name,expected,RECEIPT)
        records[name]=record
        assert record['proof']['status']==('NOT-LINKED' if row['kind']=='NOTLINKED_DIAGNOSTIC' else 'LINKED')
        if row['kind']=='CMAKE_SMALL_OUTPUT':
            assert record['cwd']==str(source/'out')
            assert row['target'] in TARGETS and name==row['target']+'.exe'
            assert row['source']==dict(file=TARGETS[row['target']],sha256=factory.sha(ROOT/TARGETS[row['target']]))
        elif row['kind']=='NOTLINKED_DIAGNOSTIC':
            assert row['source']==dict(file='probes/platform/platform_probe.c',sha256=factory.sha(ROOT/'probes/platform/platform_probe.c'))
        else:
            assert row['source_target']=='binderd'
    response=manifest['outputs']['first-link-response-diagnostic.exe'];record=records['first-link-response-diagnostic.exe']
    before=records['binderd.exe']['expanded_argv'];diagnostic=source/'out/first-link-diagnostics'
    after,changed=response_args(before,diagnostic/'first-link-response-diagnostic.exe',diagnostic/'response.map',diagnostic/'response.dll.a',source/'out',portable=True)
    assert response['original_expanded_argv']==before and response['diagnostic_expanded_argv']==after==record['expanded_argv']
    assert response['changed_output_arg_indices']==changed
    assert record['cwd']==str(source/'out') and record['argv']==[after[0],'@'+str(diagnostic/'response.rsp')]
    assert len(record['response_files'])==1 and record['response_files'][0]['path']==str(diagnostic/'response.rsp')
    assert record['response_files'][0]['text'].encode('utf-8')==gnu_response(after[1:])
    return manifest


def main():
    parser=argparse.ArgumentParser();parser.add_argument('mode',choices=('authority','build','verify'));parser.add_argument('--root',type=Path,default=ROOT/'ci-suppliers/first-link-only');args=parser.parse_args()
    if args.mode=='authority':current()
    elif args.mode=='build':build()
    else:
        expected=current(consumer=True);manifest=verify_files(args.root,expected,os.environ['GESHER_FIRST_LINK_POLICY_REF'])
        emit('FIRST_LINK_ONLY_PORTABLE_VERIFIED',dict(context=expected,role=ROLE,outputs=sorted(manifest['outputs']),runtime_executed=False))
        for row in manifest['outputs'].values():
            raw=(args.root/row['link']['file']).read_bytes();emit('FIRST_LINK_ONLY_PORTABLE_TARGET',json.loads(raw),raw)


if __name__=='__main__':main()
