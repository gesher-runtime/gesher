"""CI-only pristine standalone helper recipe and current artifact consumer.

No root CMake/setup-build/global archive or production registry is executed.
Importing this file has no tool/network/native side effect.
Failed operations never become successful after containment. The existing CLI
is the lifetime owner: unresolved launch/kernel/IO/image/directory state enters
passive quarantine after the unchanged operation/finite cleanup budgets. Only
exact owned settlement allows error exit; forced cancellation remains unproved.
"""
import argparse
import ast
import hashlib
import types
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.parse
import urllib.request

A=types.ModuleType('standalone_helper_authority')
A.__file__=str(Path(__file__).with_name('helper_authority.py'))
exec(compile(Path(A.__file__).read_bytes(),A.__file__,'exec'),A.__dict__)
ARCHIVE_NAME='llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64'
ARCHIVE_SHA='5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4'
DRIVER_SHA='c9b86311ade81d53235c93fafabdf98328d094de344ea9a9038e0dab6695ee9f'
LINKER_SHA='cbae3badcf29268220242e8875cb6c288336ab593238099746f0071c15bbf581'
MACROS=A.MACROS
COMMON=A.COMMON
TOOL_GRAPHS=[]
DIRECTORY_ROOTS=[]

def emit(label,value):
    A.emit_record(label,value)

def rejection_evidence(auth,job,gate,details):
    """Best effort only: failure to format or emit never changes rejection."""
    try:
        value={'schema':'POST_LINK_REJECTION_V1','context':auth['context'],
               'guard':auth['authority']['guard'],'producer_id':job['id'],
               'gate':gate,'details':details(),'qualification':False}
        A.require(len(A.canonical(value))<=524288,'post-link diagnostic cap')
        emit('POST_LINK_REJECTION',value)
    except Exception:
        pass

def rejected_if_false(condition,diagnostic,gate,details):
    if not condition and diagnostic is not None:
        try:diagnostic(gate,details)
        except Exception:pass

def bounded_text(text):
    return A.diagnostic_text(text.encode('utf-8'),262144)

def emit_text_members(files,current):
    # Raw PE/object bytes are never dumped or downloaded for local review.
    # All twelve sealed text members are losslessly reviewable from plain logs.
    emit('TEXT_PROOF_INDEX',{'context':current,'members':{name:{'bytes':len(files[name]),'sha256':A.digest(files[name])}for name in A.MEMBERS if not name.endswith(('.exe','.obj'))}})
    for name in A.MEMBERS:
        if name.endswith(('.exe','.obj')):continue
        raw=files[name];raw.decode('utf-8')
        label='MEMBER_'+name.upper().replace('-','_').replace('.','_')
        A.emit_bytes(label,raw)

def save(path,value):
    raw=value if type(value)is bytes else A.canonical(value)
    A.require(len(raw)<=A.FILE_CAP,'output member cap');Path(path).write_bytes(raw)

def sha(path):return A.digest(Path(path).read_bytes())

def failure_output(raw,binary=False):
    value={'bytes':len(raw),'sha256':A.digest(raw),'kind':'BINARY_NOT_PRINTED'if binary else'OVERSIZE_NOT_PRINTED'}
    if binary or len(raw)>A.STREAM_CAP:return value
    try:
        text=raw.decode('utf-8')
        A.require(all(char.isprintable()or char in '\r\n\t'for char in text),'non-text tool failure')
    except (UnicodeError,A.Rejected):value['kind']='NON_TEXT_NOT_PRINTED';return value
    value.update(kind='COMPLETE_UTF8_TEXT',text=text);return value

def tool(argv,cwd,env,seconds=90,binary_stdout=False,receipt=None):
    """Bounded concurrent tool capture with a graph saved before readers start."""
    A.require(os.environ.get('GITHUB_ACTIONS')=='true' and os.name=='posix','compiler tools Linux CI only')
    graph={'kind':'TOOL','launch_state':'NOT_ATTEMPTED','kernel_completed':False,'handles_closed':False,'argv':list(map(str,argv)),'process':None,'readers':[],'buffers':[bytearray(),bytearray()],'overflow':False,'errors':[],'executable_sha256':None,'executable_realpath':None}
    TOOL_GRAPHS.append(graph);start=time.monotonic();proc=None;kernel_done=False;timed=False
    try:
        graph['executable_sha256']=sha(graph['argv'][0]);graph['executable_realpath']=str(Path(graph['argv'][0]).resolve())
        proc=A.launch_owned(graph,graph['argv'],cwd=cwd,env=env,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        def drain(index,stream):
            try:
                while True:
                    chunk=stream.read(65536)
                    if not chunk:break
                    remaining=A.FILE_CAP-len(graph['buffers'][index]);graph['buffers'][index].extend(chunk[:max(0,remaining)])
                    if len(chunk)>remaining:graph['overflow']=True
            except BaseException as error:graph['errors'].append(repr(error))
        for i,stream in enumerate((proc.stdout,proc.stderr)):
            reader=threading.Thread(target=drain,args=(i,stream),daemon=True);slot={'thread':reader,'started':False,'start_attempted':False};graph['readers'].append(slot);slot['start_attempted']=True;reader.start();slot['started']=True
        while proc.poll()is None and time.monotonic()<start+seconds and not graph['overflow']:time.sleep(.01)
        timed=proc.poll()is None and time.monotonic()>=start+seconds
        if proc.poll()is None:proc.kill()
        proc.wait(timeout=5);kernel_done=True;graph['kernel_completed']=True
    except BaseException as error:
        graph['errors'].append(repr(error));A.prove_unattempted_failure(graph)
    finally:
        proc=graph['process']
        if proc is not None and not kernel_done:
            try:
                if proc.poll()is None:proc.kill()
                proc.wait(timeout=5);kernel_done=True;graph['kernel_completed']=True
            except BaseException as error:graph['errors'].append(repr(error))
        released=A.settle_graph(graph,start+seconds+10)
        if 'settlement_error' in graph:graph['errors'].append(graph['settlement_error'])
        record={'argv':graph['argv'],'launch_state':graph['launch_state'],'pid':proc.pid if proc else None,'started':proc is not None,'elapsed_seconds':time.monotonic()-start,'exit':proc.returncode if proc else None,'kernel_completed':graph['kernel_completed'],'io_completed':released,'handles_closed':graph['handles_closed'],'overflow':graph['overflow'],'timed_out':timed,'root_retained':not released,'errors':list(graph['errors']),'executable_sha256':graph['executable_sha256'],'executable_realpath':graph['executable_realpath'],'stdout_sha256':A.digest(bytes(graph['buffers'][0])),'stderr_sha256':A.digest(bytes(graph['buffers'][1]))}
        emit('TOOL',record)
        if not record['root_retained']:TOOL_GRAPHS.remove(graph)
    if graph['errors']or timed or graph['overflow']or proc is None or proc.returncode!=0 or record['root_retained']:
        emit('TOOL_FAILURE',{'record':record,'stdout':failure_output(bytes(graph['buffers'][0]),binary_stdout),'stderr':failure_output(bytes(graph['buffers'][1]))})
        raise A.Rejected('tool failure or containment')
    if receipt is not None:receipt.append(record)
    return bytes(graph['buffers'][0]),bytes(graph['buffers'][1])

class Redirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,req,fp,code,msg,headers,newurl):
        parsed=urllib.parse.urlparse(newurl)
        A.require(parsed.scheme=='https' and parsed.hostname and parsed.port in (None,443) and parsed.username is None and parsed.password is None and (parsed.hostname in ('github.com','api.github.com','release-assets.githubusercontent.com','objects.githubusercontent.com') or parsed.hostname.endswith('.blob.core.windows.net')), 'untrusted download redirect')
        result=super().redirect_request(req,fp,code,msg,headers,newurl)
        if result is not None:
            result.remove_header('Authorization')
        return result

def download(url,cap,expected,token=None):
    A.require(os.environ.get('GITHUB_ACTIONS')=='true','download CI only')
    request=urllib.request.Request(url,headers={'Authorization':'Bearer '+token} if token else {})
    opener=urllib.request.build_opener(Redirect())
    with opener.open(request,timeout=60)as response:
        chunks=[];total=0
        while True:
            part=response.read(min(1024*1024,cap+1-total))
            if not part:break
            chunks.append(part);total+=len(part);A.require(total<=cap,'download cap')
    raw=b''.join(chunks);A.require(A.digest(raw)==expected,'download digest');return raw

def extract_toolchain(raw,destination):
    A.require(len(raw)<=A.ARCHIVE_CAP and not destination.exists(),'fresh archive extraction')
    destination.mkdir()
    with tarfile.open(fileobj=io.BytesIO(raw),mode='r:xz')as archive:
        rows=archive.getmembers();A.require(0<len(rows)<=100000,'toolchain member count')
        total=0;names=set()
        for row in rows:
            path=Path(row.name);A.require(not path.is_absolute() and '..'not in path.parts and path.parts[0]==ARCHIVE_NAME and row.name not in names,'toolchain path/duplicate')
            names.add(row.name);A.require(row.isfile()or row.isdir()or row.issym()or row.islnk(),'toolchain special file');total+=row.size
            A.require(total<=4*A.ARCHIVE_CAP,'toolchain expanded cap')
            if row.issym()or row.islnk():
                target=(destination/path.parent/row.linkname).resolve()if row.issym()else(destination/row.linkname).resolve()
                A.require(target.is_relative_to(destination.resolve()),'toolchain escaping link')
        archive.extractall(destination,filter='data')
    return destination/ARCHIVE_NAME

def dependency_paths(raw,source_root,toolroot,build,diagnostic=None,obj='helper.obj'):
    A.require(obj in A.SDK_STL_REQUIRED,'exact dependency object')
    text=raw.decode('utf-8').replace('\\\n',' ')
    A.require(': 'in text,'make dependency target')
    names=shlex.split(text.split(': ',1)[1]);A.require(1<=len(names)<=8192 and len(set(names))==len(names),'dependency count/duplicates')
    result=[]
    for name in names:
        path=Path(name);path=path if path.is_absolute()else build/path;path=path.resolve()
        source=path.is_relative_to(source_root.resolve());supplier=path.is_relative_to(toolroot.resolve())
        A.require(source or supplier,'foreign dependency')
        if source:A.require(path.relative_to(source_root).as_posix()in (*A.TUS.values(),A.HEADER),'unreviewed local include')
        result.append({'origin':'source'if source else'toolchain','path':str(path),'relative':path.relative_to(source_root if source else toolroot).as_posix(),'sha256':sha(path),'bytes':path.stat().st_size})
    required=A.SDK_STL_REQUIRED[obj]
    if not required<={Path(x['path']).name for x in result} and diagnostic is not None:
        try:
            diagnostic(raw,result,required)
        except Exception:
            pass # Diagnostic failure cannot change the original rejection.
    A.require(required<={Path(x['path']).name for x in result},'SDK/STL dependencies missing')
    return result

def emit_dependency_rejection(raw,result,required,auth,job,obj,controlled):
    observed=sorted({Path(x['path']).name for x in result})
    value={'schema':'DEPENDENCY_GATE_V1','context':auth['context'],
           'guard':auth['authority']['guard'],'producer_id':job['id'],'object':obj,
           'controlled_config_sha256':A.digest(A.canonical(controlled)),
           'raw':A.diagnostic_text(raw,262144),'required':sorted(required),
           'observed':observed,'missing':sorted(required-set(observed)),
           'qualification':False}
    A.require(len(A.canonical(value))<=524288,'dependency diagnostic cap')
    emit('DEPENDENCY_GATE',value)

def parse_driver(raw,toolroot,source_root,build):
    # Constrain even not-yet-created outputs and attached option paths.
    roots=tuple(p.resolve()for p in (toolroot,source_root,build))
    commands=[]
    for line in raw.decode().splitlines():
        if line.lstrip().startswith('"'):
            row=shlex.split(line);A.require(row,'empty expanded command')
            A.require(Path(row[0]).resolve().is_relative_to(roots[0]),'foreign implicit executable');commands.append(row)
            for value in row[1:]:
                A.require(not value.startswith('@'),'unrecorded response file')
                candidate=value;attached=False
                for prefix in ('-I','-L','-isystem','-iquote','-idirafter','--sysroot=','-fdebug-compilation-dir=','-resource-dir='):
                    if value.startswith(prefix)and len(value)>len(prefix):candidate=value[len(prefix):];attached=True;break
                if Path(candidate).is_absolute() or attached or (not candidate.startswith('-') and ('/'in candidate or '\\'in candidate or Path(candidate).suffix in ('.o','.obj','.a','.d','.cc','.cpp','.h','.rsp','.cfg'))):
                    path=Path(candidate);p=(path if path.is_absolute()else build/path).resolve()
                    A.require(any(p.is_relative_to(root)for root in roots),'ambient implicit path')
    A.require(commands,'missing expanded driver commands');return commands

def collect_crt_identity(auth,job,archives,loads,text,toolroot,build,env):
    selected=[v for v in archives.values()if Path(v['path']).name=='libmsvcrt.a']
    A.require(len(selected)==1,'collector exact CRT archive')
    archive=selected[0];rows=[list(x)for x in loads if Path(x[0]).name=='libmsvcrt.a']
    A.require(0<len(rows)<=256,'collector loaded CRT rows')
    for token,member,symbol in rows:
        origins=[value for key,value in archives.items()if token==key or (token==Path(token).name and Path(key).name==token)]
        A.require(len(origins)==1 and origins[0] is archive,'collector exact loaded archive origin')
    members=sorted({x[1]for x in rows});A.require(0<len(members)<=64,'collector selected member count')
    A.require(all(type(x)is str and 0<len(x)<=256 and '/'not in x and '\\'not in x and '\0'not in x for x in members),'collector member names')
    exe=build/'helper-qualification.exe';exe_size=exe.stat().st_size;A.require(0<exe_size<=A.FILE_CAP,'collector EXE cap')
    exe_hash=sha(exe);A.require(sha(archive['path'])==archive['sha256'],'collector pre archive hash')
    imports,_=tool([toolroot/'bin/llvm-readobj','--coff-imports',exe],build,env)
    A.require(imports,'collector empty imports')
    table,_=tool([toolroot/'bin/llvm-ar','t',archive['path']],build,env)
    table_text=A.diagnostic_text(table,262144);names=table.decode('utf-8').splitlines()
    A.require(0<len(names)<=65536 and all(0<len(n)<=256 and '/'not in n and '\\'not in n and '\0'not in n for n in names),'collector archive table')
    selected_members=[];aggregate=0
    for name in members:
        count=names.count(name);A.require(count>0,'collector loaded member absent from table')
        record={'name':name,'table_occurrences':count,'state':'AMBIGUOUS_NOT_EXTRACTED'}
        if count==1:
            raw,_=tool([toolroot/'bin/llvm-ar','p',archive['path'],name],build,env,binary_stdout=True)
            aggregate+=len(raw);A.require(0<len(raw)<=A.FILE_CAP and aggregate<=A.FILE_CAP,'collector member cap')
            record.update(state='UNIQUE_EXTRACTED',bytes=len(raw),sha256=A.digest(raw))
        selected_members.append(record)
    A.require(sha(archive['path'])==archive['sha256'] and exe.stat().st_size==exe_size and sha(exe)==exe_hash,'collector post archive/EXE identity')
    value={'schema':'CRT_IDENTITY_COLLECTOR_V1','context':auth['context'],'guard':auth['authority']['guard'],'publisher_archive_sha256_source_pin':ARCHIVE_SHA,
           'producer_id':job['id'],'trace_sha256':A.digest(text.encode()),'archive':archive,
           'exe_sha256':exe_hash,'exe_bytes':exe_size,'loaded_rows':rows,
           'member_table':table_text,'selected_members':selected_members,
           'imports':A.diagnostic_text(imports,262144),'qualification':False}
    A.require(len(A.canonical(value))<=524288,'collector packet cap');emit('CRT_IDENTITY_COLLECTOR',value)

def link_dependencies(text,expanded,toolroot,build,env,diagnostic=None,collector=None):
    reads=[];loads=[]
    for line in text.splitlines():
        m=re.fullmatch(r'(?:ld\.lld|lld-link): Reading (.+\.a)\s*',line)
        if m:reads.append(m[1]);continue
        m=re.fullmatch(r'(?:ld\.lld|lld-link): Loaded ([^()]+\.a)\(([^()]+)\) for (.+)',line)
        if m:loads.append(m.groups())
    rejected_if_false(bool(reads and loads),diagnostic,'reading_loaded',lambda:{'trace':bounded_text(text),'reads':reads,'loads':loads})
    A.require(reads and loads,'missing actual Reading/Loaded dependencies')
    archives={}
    for name in sorted(set(reads)):
        path=Path(name)
        if not path.is_absolute():
            candidates=[p.resolve()for p in toolroot.rglob(path.name)if p.is_file()]
            rejected_if_false(len(set(candidates))==1,diagnostic,'archive_ambiguity',lambda:{'name':name,'candidates':list(map(str,candidates))})
            A.require(len(set(candidates))==1,'ambiguous archive path');path=candidates[0]
        path=path.resolve();rejected_if_false(path.is_relative_to(toolroot.resolve()),diagnostic,'foreign_archive',lambda:{'path':str(path),'root':str(toolroot)})
        A.require(path.is_relative_to(toolroot.resolve()),'foreign archive')
        archives[name]={'path':str(path),'sha256':sha(path),'members':{}}
    names={Path(x['path']).name for x in archives.values()}
    ucrt_ok=any('ucrt'in n for n in names)or('libmsvcrt.a'in names and (archives.get('libmsvcrt.a',{}).get('sha256')=='4605a0d2f47c88f819dff01d19c5347cac10fdc618e5e1f505c6d094e5db0183'or any(x.get('sha256')=='4605a0d2f47c88f819dff01d19c5347cac10fdc618e5e1f505c6d094e5db0183'for x in archives.values()if Path(x.get('path','')).name=='libmsvcrt.a')))
    required_ok='libc++.a'in names and 'libunwind.a'in names and any('builtins'in n for n in names)and ucrt_ok and 'libkernel32.a'in names
    rejected_if_false(required_ok,diagnostic,'required_archives',lambda:{'trace':bounded_text(text),'names':sorted(names),'archives':archives,'predicates':{'libc++.a':'libc++.a'in names,'libunwind.a':'libunwind.a'in names,'builtins':any('builtins'in n for n in names),'ucrt':ucrt_ok,'libkernel32.a':'libkernel32.a'in names}})
    if not required_ok and collector is not None:
        try:collector(archives,loads,text)
        except Exception as error:
            if TOOL_GRAPHS or A.LIVE_GRAPHS:raise
            if diagnostic is not None:
                try:diagnostic('crt_collector_unavailable',lambda:{'error':repr(error)})
                except Exception:pass
    A.require(required_ok,'missing positive CRT/STL/unwind/builtins/system dependency')
    for archive,member,symbol in loads:
        candidates=[v for k,v in archives.items()if k==archive or Path(k).name==Path(archive).name]
        rejected_if_false(len(candidates)==1,diagnostic,'loaded_origin',lambda:{'archive':archive,'member':member,'symbol':symbol,'candidates':candidates})
        A.require(len(candidates)==1,'Loaded member has no unique Reading origin')
        raw,_=tool([toolroot/'bin/llvm-ar','p',candidates[0]['path'],member],build,env,binary_stdout=True)
        rejected_if_false(bool(raw),diagnostic,'empty_member',lambda:{'archive':archive,'member':member,'symbol':symbol,'bytes':len(raw),'sha256':A.digest(raw)})
        A.require(raw,'empty loaded member');old=candidates[0]['members'].get(member)
        row={'sha256':A.digest(raw),'bytes':len(raw)};rejected_if_false(old is None or old==row,diagnostic,'member_conflict',lambda:{'archive':archive,'member':member,'old':old,'current':row});A.require(old is None or old==row,'member conflict');candidates[0]['members'][member]=row
    direct=[]
    for row in expanded:
        if not Path(row[0]).name.startswith('ld.lld'):continue
        for name in row[1:]:
            if name.endswith(('.o','.obj')):
                p=Path(name);p=p if p.is_absolute()else build/p;p=p.resolve()
                if p.is_relative_to(build.resolve()):
                    rejected_if_false(p.name in A.TUS,diagnostic,'foreign_direct',lambda:{'path':str(p),'expected':list(A.TUS)})
                    A.require(p.name in A.TUS,'foreign direct object')
                else:
                    rejected_if_false(p.is_relative_to(toolroot.resolve()),diagnostic,'foreign_startup',lambda:{'path':str(p),'root':str(toolroot)})
                    A.require(p.is_relative_to(toolroot.resolve()),'foreign startup object')
                direct.append({'path':str(p),'sha256':sha(p),'origin':'three-object'if p.is_relative_to(build.resolve())else'publisher-startup'})
    rejected_if_false([Path(x['path']).name for x in direct if x['origin']=='three-object']==list(A.TUS) and all(Path(x['path'])==build/Path(x['path']).name for x in direct if x['origin']=='three-object'),diagnostic,'direct_objects',lambda:{'direct':direct,'expanded':expanded,'expected':list(A.TUS)})
    A.require([Path(x['path']).name for x in direct if x['origin']=='three-object']==list(A.TUS) and all(Path(x['path'])==build/Path(x['path']).name for x in direct if x['origin']=='three-object'),'exact actual three link objects')
    rejected_if_false(any(x['origin']=='publisher-startup'for x in direct),diagnostic,'startup_objects',lambda:{'direct':direct,'expanded':expanded})
    A.require(any(x['origin']=='publisher-startup'for x in direct),'missing actual startup objects')
    return {'archives':archives,'loaded':loads,'direct':direct}

def current_job(auth,name):
    current=auth['context'];data=A.api('actions/runs/'+current['run']+'/attempts/'+current['attempt']+'/jobs?per_page=100')
    A.require(data['total_count']==len(data['jobs'])<=100,'complete current roles')
    rows=[x for x in data['jobs']if x['name']==name]
    A.require(len(rows)==1,'current named role');row=rows[0]
    A.require(row['status']=='in_progress'and row['head_sha']==current['source']and str(row['run_id'])==current['run']and str(row['run_attempt'])==current['attempt'],'current active role');return row

def read_controlled_config(auth,job,tc,root,build):
    """Only two already-inventoried supplier text files, bounded before decode."""
    A.require(os.environ.get('GITHUB_ACTIONS')=='true'and os.name=='posix','supplier config reads Linux CI only')
    value={'schema':A.CONFIG_POLICY,'context':auth['context'],'guard':auth['authority']['guard'],'producer_id':job['id'],'roots':{'supplier':str(tc),'source':str(root),'build':str(build)},'files':{}}
    for role,(name,expected,pin)in A.CONFIG_FILES.items():
        path=tc/'bin'/name
        A.require(path.resolve().is_relative_to(tc)and path.resolve()==path,'exact supplier config path')
        with path.open('rb')as stream:raw=stream.read(len(expected)+1)
        A.require(raw==expected and A.digest(raw)==pin,'bounded exact supplier configuration')
        value['files'][role]={'path':str(path),'text':raw.decode('ascii'),'bytes':len(raw),'sha256':pin}
    return A.validate_controlled_config(value,auth['context'],auth['authority']['guard'],job['id'])

def emit_early_basis(auth,job,tc,root,build,configs,argv,expanded,streams,receipts,controlled):
    """Only already obtained metadata; no tool/file/API acquisition here."""
    context=auth['context'];authority=auth['authority']
    A.require(ARCHIVE_SHA==A.EARLY_ARCHIVE_PIN,'early reviewed archive pin')
    A.require(set(context)=={'source','run','attempt','repository','repository_id','profile'} and context['source']==authority['caller_authority'] and context['profile']==A.PROFILE and context['repository']==A.REPOSITORY and context['repository_id']==A.REPOSITORY_ID,'early current context')
    A.hexid(context['source']);A.hexid(authority['guard'])
    A.require(job['name']=='helper-producer'and job['status']=='in_progress'and job['head_sha']==context['source']and str(job['run_id'])==context['run']and str(job['run_attempt'])==context['attempt']and type(job['id'])is int and job['id']>0,'early current producer')
    tc,root,build=(p.resolve()for p in (tc,root,build))
    driver=str(tc/'bin/x86_64-w64-mingw32-clang++')
    A.validate_controlled_config(controlled,context,authority['guard'],job['id'])
    A.require(controlled['roots']=={'supplier':str(tc),'source':str(root),'build':str(build)},'early controlled current roots')
    expected=A.compile_recipe(controlled,'helper.obj')
    A.require(argv==expected,'early exact unchanged compile argv')
    A.require(set(streams)=={'target','version','expansion'}and set(receipts)==set(streams),'early exact stream schema')
    expected_calls={'target':[driver,*A.config_options(controlled),'-dumpmachine'],'version':[driver,*A.config_options(controlled),'--version'],'expansion':[driver,'-###',*argv[1:]]}
    text={}
    for name,cap in (('target',4096),('version',65536),('expansion',262144)):
        A.require(len(streams[name])==2 and len(receipts[name])==1,'early complete streams/receipt')
        record=receipts[name][0]
        A.require(set(record)=={'argv','launch_state','pid','started','elapsed_seconds','exit','kernel_completed','io_completed','handles_closed','overflow','timed_out','root_retained','errors','executable_sha256','executable_realpath','stdout_sha256','stderr_sha256'},'early exact receipt schema')
        A.require(record['argv']==expected_calls[name]and record['exit']==0 and record['executable_sha256']==DRIVER_SHA and Path(record['executable_realpath']).is_relative_to(tc),'early actual driver receipt')
        A.require(record['started']is True and record['launch_state']=='RETURNED_OWNED'and type(record['pid'])is int and record['pid']>0,'early returned owned tool')
        A.require(record['kernel_completed']and record['io_completed']and record['handles_closed']and not record['root_retained']and not record['timed_out']and not record['overflow']and not record['errors'],'early completed tool')
        text[name]={}
        for stream,raw in zip(('stdout','stderr'),streams[name]):
            value=A.diagnostic_text(raw,cap);A.require(value['sha256']==record[stream+'_sha256'],'early actual stream binding');text[name][stream]=value
    A.require(text['target']['stdout']['text'].strip()=='x86_64-w64-windows-gnu'and '23.1.3'in text['version']['stdout']['text'],'early required target/version')
    A.require(type(configs)is list and len(configs)<=4096,'early config count')
    seen=set()
    for row in configs:
        A.require(set(row)=={'path','bytes','sha256'}and type(row['path'])is str and len(row['path'])<=4096 and Path(row['path']).is_absolute()and Path(row['path']).resolve().is_relative_to(tc)and Path(row['path']).suffix=='.cfg'and row['path']not in seen,'early confined config')
        A.require(type(row['bytes'])is int and 0<=row['bytes']<=A.FILE_CAP and re.fullmatch('[0-9a-f]{64}',row['sha256']),'early config identity');seen.add(row['path'])
    for row in controlled['files'].values():
        A.require({'path':row['path'],'bytes':row['bytes'],'sha256':row['sha256']}in configs,'controlled files match actual inventory')
    A.require(0<len(expanded)<=4096 and all(type(r)is list and 0<len(r)<=4096 and all(type(x)is str and len(x)<=4096 for x in r)for r in expanded),'early parsed command bounds')
    cc1=A.validate_cpp_expansion(expanded,controlled,'helper.obj')
    # Reuse exactly the existing confinement parser on already obtained bytes.
    A.require(expanded==parse_driver(b''.join(streams['expansion']),tc,root,build),'early parsed/raw confinement')
    value={'schema':'EARLY_COMPILER_BASIS_V2','context':context,'guard':authority['guard'],'caller_authority':authority['caller_authority'],'producer_id':job['id'],'archive_sha256_source_pin':ARCHIVE_SHA,'linker_sha256_checked_pin':LINKER_SHA,'roots':{'supplier':str(tc),'source':str(root),'build':str(build)},'compile_argv':argv,'tool_receipts':receipts,'streams':text,'configs':configs,'config_selection':A.CONFIG_POLICY,'controlled_config':controlled,'parsed_commands':expanded,'unique_cc1':cc1,'confinement_validated':True,'effective_exceptions_validated':True,'qualification':False}
    A.require(len(A.canonical(value))<=A.FILE_CAP,'early combined record cap')
    emit('EARLY_COMPILER_BASIS',value)

def produce():
    A.require(os.name=='posix','producer Linux only')
    auth=A.authorize('helper-producer',('source-authority / verify',));job=current_job(auth,'helper-producer');emit('AUTHORITY',auth)
    root=Path.cwd().resolve();bundle=root/'helper-bundle';work=Path(os.environ['RUNNER_TEMP'])/('mremap-helper-'+auth['context']['run']+'-'+auth['context']['attempt'])
    A.require(not work.exists()and not bundle.exists(),'fresh producer directories');work.mkdir();bundle.mkdir()
    env={'PATH':'/usr/bin:/bin','HOME':str(work/'empty-home'),'LANG':'C.UTF-8','LC_ALL':'C.UTF-8','TMPDIR':str(work/'tmp')}
    Path(env['HOME']).mkdir();Path(env['TMPDIR']).mkdir()
    raw=download('https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/'+ARCHIVE_NAME+'.tar.xz',A.ARCHIVE_CAP,ARCHIVE_SHA)
    tc=extract_toolchain(raw,work/'publisher').resolve();driver=tc/'bin/x86_64-w64-mingw32-clang++';linker=tc/'bin/ld.lld'
    A.require(sha(driver)==DRIVER_SHA and sha(linker)==LINKER_SHA,'pinned actual driver/linker');build=work/'build';build.mkdir()
    configs=[{'path':str(p),'sha256':sha(p),'bytes':p.stat().st_size}for p in sorted(tc.rglob('*.cfg'))if p.is_file()]
    controlled=read_controlled_config(auth,job,tc,root,build)
    basis_receipts={name:[]for name in ('target','version','expansion')}
    target_raw,target_err=tool([driver,*A.config_options(controlled),'-dumpmachine'],build,env,receipt=basis_receipts['target']);target=target_raw.decode().strip();A.require(target=='x86_64-w64-windows-gnu','actual target')
    version,version_err=tool([driver,*A.config_options(controlled),'--version'],build,env,receipt=basis_receipts['version']);A.require('23.1.3'in version.decode(),'actual compiler version')
    commands=[];deps={};objects={};expanded={};transcripts=[]
    for obj,source in A.TUS.items():
        dep=obj.replace('.obj','.d');argv=A.compile_recipe(controlled,obj)
        diagnostic=[argv[0],'-###',*argv[1:]];out,err=tool(diagnostic,build,env,receipt=basis_receipts['expansion']if obj=='helper.obj'else None);expanded[obj]=parse_driver(out+err,tc,root,build)
        A.validate_cpp_expansion(expanded[obj],controlled,obj)
        if obj=='helper.obj':emit_early_basis(auth,job,tc,root,build,configs,argv,expanded[obj],{'target':(target_raw,target_err),'version':(version,version_err),'expansion':(out,err)},basis_receipts,controlled)
        stdout,stderr=tool(argv,build,env);transcripts.append({'object':obj,'argv':argv,'driver_expansion':(out+err).decode(),'stdout':stdout.decode(),'stderr':stderr.decode()})
        rawdep=(build/dep).read_bytes();deps[obj]=dependency_paths(rawdep,root,tc,build,
            lambda raw,rows,required:emit_dependency_rejection(raw,rows,required,auth,job,obj,controlled),obj=obj)
        A.require(any(x['relative']==A.HEADER and x['origin']=='source'for x in deps[obj]),'current private header dependency')
        save(bundle/dep,rawdep);shutil.copyfile(build/obj,bundle/obj)
        objects[obj]={'source':source,'source_blob':A.SOURCE_BLOBS[obj],'source_sha256':sha(root/source),'header_blob':A.HEADER_BLOB,'header_sha256':sha(root/A.HEADER),'sha256':sha(build/obj),'exceptions':obj!='caller.obj','argv':argv,'expanded':expanded[obj]}
        commands.append({'output':obj,'file':source,'argv':argv,'expanded':expanded[obj]})
    A.require_dependency_union(deps)
    link=A.link_recipe(controlled)
    out,err=tool([link[0],'-###',*link[1:]],build,env)
    try:exp=parse_driver(out+err,tc,root,build)
    except Exception as error:
        rejection_evidence(auth,job,'link_expansion',lambda:{'output':A.diagnostic_text(out+err,262144),'error':repr(error)})
        raise
    stdout,stderr=tool(link,build,env);trace=(stdout+stderr).decode();lower=trace.lower()+(out+err).decode().lower()
    diagnostic=lambda gate,details:rejection_evidence(auth,job,gate,details)
    rejected_if_false(not any(x in lower for x in ('libgesher_posix','binder','gesher_crash_reporter_anchor','gesher_signal_mailbox_anchor','--wrap','wrap=free','wrap=realloc')),diagnostic,'foreign_link_root',lambda:{'trace':bounded_text(trace),'expanded':bounded_text((out+err).decode())})
    A.require(not any(x in lower for x in ('libgesher_posix','binder','gesher_crash_reporter_anchor','gesher_signal_mailbox_anchor','--wrap','wrap=free','wrap=realloc')),'foreign implicit root dependencies')
    linked=link_dependencies(trace,exp,tc,build,env,diagnostic,
        lambda archives,loads,text:collect_crt_identity(auth,job,archives,loads,text,tc,build,env))
    imports,_=tool([tc/'bin/llvm-readobj','--coff-imports',build/'helper-qualification.exe'],build,env)
    rejected_if_false(bool(imports and not re.search(rb'(?i)jvm|dalvik|libart|binder|gesher_posix',imports)),diagnostic,'imports_role',lambda:{'imports':A.diagnostic_text(imports,262144)})
    A.require(imports and not re.search(rb'(?i)jvm|dalvik|libart|binder|gesher_posix',imports),'imports role')
    symbols={}
    for obj in A.TUS:
        value,_=tool([tc/'bin/llvm-nm','--defined-only','--demangle',build/obj],build,env);symbols[obj]=value.decode()
    rejected_if_false('operator new('in symbols['qualification.obj']and 'operator new('not in symbols['helper.obj']and 'operator new('not in symbols['caller.obj'],diagnostic,'new_ownership',lambda:{'symbols':{k:bounded_text(v)for k,v in symbols.items()}})
    A.require('operator new('in symbols['qualification.obj']and 'operator new('not in symbols['helper.obj']and 'operator new('not in symbols['caller.obj'],'test-only new ownership')
    rejected_if_false('PrepareAliasFromNoExceptions'in symbols['caller.obj']and 'PreparePunchRequestFromNoExceptions'in symbols['caller.obj'],diagnostic,'caller_symbols',lambda:{'symbols':bounded_text(symbols['caller.obj'])})
    A.require('PrepareAliasFromNoExceptions'in symbols['caller.obj']and 'PreparePunchRequestFromNoExceptions'in symbols['caller.obj'],'actual noexceptions caller functions')
    shutil.copyfile(build/'helper-qualification.exe',bundle/'helper-qualification.exe');shutil.copyfile(build/'helper-qualification.map',bundle/'helper-qualification.map')
    save(bundle/'helper-qualification.imports.txt',imports);save(bundle/'compile-commands.json',commands);save(bundle/'dependency-manifest.json',deps)
    profile={'profile':A.PROFILE,'archive_name':ARCHIVE_NAME,'archive_sha256':ARCHIVE_SHA,'driver':str(driver),'driver_realpath':str(driver.resolve()),'driver_sha256':DRIVER_SHA,'linker_sha256':LINKER_SHA,'target':target,'version':version.decode(),'common_flags':COMMON,'configs':configs,'config_policy':A.CONFIG_POLICY,'controlled_config':controlled,'objects':objects,'link_argv':link,'link_expanded':exp,'link_dependencies':linked,'no_ambient_injection':True,'dependencies_verified':True,'link_dependencies_verified':True}
    save(bundle/'compiler-profile.json',profile);save(bundle/'link-transcript.txt',A.canonical({'compile':transcripts,'link':{'argv':link,'expansion':(out+err).decode(),'stdout':stdout.decode(),'stderr':stderr.decode()}}));save(bundle/'object-symbols.json',symbols);save(bundle/'source-manifest.json',auth['authority']['manifest'])
    files={p.name:p.read_bytes()for p in bundle.iterdir()};rejected_if_false(set(files)==set(A.MEMBERS)-{'producer-manifest.json'},diagnostic,'payload_members',lambda:{'observed':sorted(files),'expected':sorted(set(A.MEMBERS)-{'producer-manifest.json'})});A.require(set(files)==set(A.MEMBERS)-{'producer-manifest.json'},'producer exact fifteen payloads')
    manifest={'schema':1,'context':auth['context'],'authority':{k:auth['authority'][k]for k in ('guard','guard_blob','template','caller_authority')},'producer_id':job['id'],'members':{p:{'bytes':len(v),'sha256':A.digest(v)}for p,v in files.items()},'role':A.PROFILE}
    save(bundle/'producer-manifest.json',manifest);total=sum(p.stat().st_size for p in bundle.iterdir());rejected_if_false(total<=A.TOTAL_CAP,diagnostic,'bundle_cap',lambda:{'bytes':total,'cap':A.TOTAL_CAP});A.require(total<=A.TOTAL_CAP,'bundle aggregate cap')
    # Verify using independently selected current role ID, not the manifest itself.
    local=dict(auth);local['roles']=dict(auth['roles'],**{'helper-producer':job})
    try:A.verify_bundle({p.name:p.read_bytes()for p in bundle.iterdir()},local)
    except Exception as error:
        diagnostic('bundle_verification',lambda:{'error':repr(error),'manifest':manifest})
        raise
    emit_text_members({p.name:p.read_bytes()for p in bundle.iterdir()},auth['context'])
    emit('PRODUCER_MANIFEST',{'sha256':sha(bundle/'producer-manifest.json'),'context':auth['context'],'producer_id':job['id'],'role':A.PROFILE,'qualification':False,'members':manifest['members']})

def release_consumer_directory(dest,parent):
    # No recursive deletion while a child/reader/image lease is retained.
    A.require(not A.LIVE_GRAPHS and not TOOL_GRAPHS,'FAILED_CONTAINMENT: retain owned directory')
    A.require(dest.is_absolute() and dest.resolve().parent==parent.resolve() and re.fullmatch(r'mremap-helper-[1-9][0-9]*-[1-9][0-9]*-win(?:22|25)',dest.name),'owned absolute cleanup target')
    if not dest.exists():
        A.require(not dest.is_symlink(),'cleanup absent reparse target');return
    A.require(not dest.is_symlink() and not(getattr(dest.lstat(),'st_file_attributes',0)&0x400),'cleanup reparse target')
    shutil.rmtree(dest);A.require(not dest.exists(),'owned directory cleanup')

def consume():
    A.require(os.name=='nt','consumer Windows only');auth=A.authorize('helper-windows',('source-authority / verify','helper-producer'))
    image=os.environ.get('ImageOS');A.require(image in ('win22','win25'),'host image role');role='helper-windows-'+('2022'if image=='win22'else'2025');current_job(auth,role)
    artifact=A.verify_artifact(A.api('actions/runs/'+auth['context']['run']+'/artifacts?per_page=100'),auth['context'],auth['roles']['helper-producer']);emit('DOWNLOAD_AUTHORITY',{'context':auth['context'],'roles':auth['roles'],'artifact':artifact,'source_authority':auth['authority'],'host_role':role})
    raw=download('https://api.github.com/repos/'+A.REPOSITORY+'/actions/artifacts/'+str(artifact['id'])+'/zip',A.TOTAL_CAP,artifact['digest'].split(':')[1],os.environ['GH_TOKEN'])
    files=A.zip_members(raw);manifest=A.verify_bundle(files,auth);emit_text_members(files,auth['context']);dest=Path(os.environ['RUNNER_TEMP'])/('mremap-helper-'+auth['context']['run']+'-'+auth['context']['attempt']+'-'+image)
    A.require(not dest.exists()and not dest.is_symlink(),'fresh consumer directory')
    directory={'path':dest,'parent':Path(os.environ['RUNNER_TEMP']).resolve()}
    DIRECTORY_ROOTS.append(directory)
    try:
        dest.mkdir()
        for name,value in files.items():(dest/name).write_bytes(value)
        os.environ.pop('GH_TOKEN',None);os.environ.pop('GITHUB_TOKEN',None)
        record,result=A.owned_helper(dest/'helper-qualification.exe',manifest['members']['helper-qualification.exe']['sha256'],auth['context'])
    finally:
        released=False
        if not A.LIVE_GRAPHS and not TOOL_GRAPHS:
            release_consumer_directory(dest,directory['parent']);DIRECTORY_ROOTS.remove(directory);released=True
        emit('DIRECTORY',{'path':str(dest),'released':released,'retained_graphs':len(A.LIVE_GRAPHS)})
    A.require(released,'retained containment failure')
    emit('RESULT',{'context':auth['context'],'host_role':role,'producer_id':manifest['producer_id'],'artifact_id':artifact['id'],'artifact_digest':artifact['digest'],'producer_manifest_sha256':A.digest(files['producer-manifest.json']),'process':record,'budgets':result,'directory_released':True,'full_A_qualified':False,'production_registry_qualified':False,'selected_supplier_qualified':False})

def lifetime_unresolved():
    return bool(TOOL_GRAPHS or A.LIVE_GRAPHS or DIRECTORY_ROOTS)

def settle_lifetime_once():
    # Past the finite cleanup budget this is ONLY a passive lifetime barrier:
    # zero-time exact-owned polls/joins and disposal after proven quiescence.
    # No kill retries, tool work, PID search, new process or success conversion.
    for roots in (TOOL_GRAPHS,A.LIVE_GRAPHS):
        for graph in tuple(roots):
            if A.settle_graph(graph,time.monotonic()):roots.remove(graph)
    if not TOOL_GRAPHS and not A.LIVE_GRAPHS:
        for directory in tuple(DIRECTORY_ROOTS):
            try:
                release_consumer_directory(directory['path'],directory['parent'])
                DIRECTORY_ROOTS.remove(directory)
            except BaseException as error:
                A.containment_error(directory,error)
    return not lifetime_unresolved()

def quarantine_pause():
    time.sleep(1)

def quarantine_failure():
    """Keep THIS CLI and original graphs alive; qualification is forever failed.

    Unknown launch cannot settle from process=None. An external forced cancel
    can destroy this owner and remains an UNPROVED boundary, not cleanup.
    """
    entered=False
    while True:
        try:
            # Entry and EVERY subsequent exit query belong to this guard too.
            # An interruption in the query is not proof that roots settled.
            if not lifetime_unresolved():return
            if not entered:
                entered=True
                emit('QUARANTINE',{'qualification':False,'state':'PASSIVE_RETAINED_OWNER','operation_budget_extended':False,'external_forced_cancellation':'UNPROVED','tool_graphs':len(TOOL_GRAPHS),'native_graphs':len(A.LIVE_GRAPHS),'directory_roots':len(DIRECTORY_ROOTS)})
            if settle_lifetime_once():break
            quarantine_pause()
        except BaseException:
            # In-process interruption/output failure cannot authorize CLI exit.
            # No repeated output or accumulating error log in this barrier.
            try:quarantine_pause()
            except BaseException:pass

def finish_failure(mode,error):
    try:
        emit('FAILURE',{'mode':mode,'error':repr(error)[:4096],'retained_tool_graphs':len(TOOL_GRAPHS),'retained_native_graphs':len(A.LIVE_GRAPHS),'retained_directory_roots':len(DIRECTORY_ROOTS),'qualification':False})
    finally:
        quarantine_failure()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('mode',choices=('produce','consume'));args=parser.parse_args()
    A.require(os.environ.get('GITHUB_ACTIONS')=='true' and sys.flags.isolated==1 and sys.dont_write_bytecode,'isolated no-cache CI execution only')
    try:
        (produce if args.mode=='produce'else consume)()
    except BaseException as error:
        finish_failure(args.mode,error);raise

if __name__=='__main__':main()
