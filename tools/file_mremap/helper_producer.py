"""CI-only pristine standalone helper recipe and current artifact consumer.

No root CMake/setup-build/global archive or production registry is executed.
Importing this file has no tool/network/native side effect.
"""
import argparse
import ast
import hashlib
import importlib.util
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

_spec=importlib.util.spec_from_file_location('standalone_helper_authority',Path(__file__).with_name('helper_authority.py'))
A=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(A)
ARCHIVE_NAME='llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64'
ARCHIVE_SHA='5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4'
DRIVER_SHA='c9b86311ade81d53235c93fafabdf98328d094de344ea9a9038e0dab6695ee9f'
LINKER_SHA='cbae3badcf29268220242e8875cb6c288336ab593238099746f0071c15bbf581'
MACROS=A.MACROS
COMMON=A.COMMON
TOOL_GRAPHS=[]

def emit(label,value):
    print('FILE_MREMAP_HELPER_'+label+' '+A.canonical(value).decode().rstrip(),flush=True)

def save(path,value):
    raw=value if type(value)is bytes else A.canonical(value)
    A.require(len(raw)<=A.FILE_CAP,'output member cap');Path(path).write_bytes(raw)

def sha(path):return A.digest(Path(path).read_bytes())

def tool(argv,cwd,env,seconds=90):
    """Bounded concurrent tool capture with a graph saved before readers start."""
    A.require(os.environ.get('GITHUB_ACTIONS')=='true' and os.name=='posix','compiler tools Linux CI only')
    graph={'argv':list(map(str,argv)),'process':None,'readers':[],'buffers':[bytearray(),bytearray()],'overflow':False,'errors':[],'executable_sha256':None,'executable_realpath':None}
    TOOL_GRAPHS.append(graph);start=time.monotonic();proc=None;kernel_done=False;timed=False;closed=False
    try:
        graph['executable_sha256']=sha(graph['argv'][0]);graph['executable_realpath']=str(Path(graph['argv'][0]).resolve())
        proc=subprocess.Popen(graph['argv'],cwd=cwd,env=env,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE);graph['process']=proc
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
        proc.wait(timeout=5);kernel_done=True
    except BaseException as error:
        graph['errors'].append(repr(error))
    finally:
        if proc is not None and not kernel_done:
            try:
                if proc.poll()is None:proc.kill()
                proc.wait(timeout=5);kernel_done=True
            except BaseException as error:graph['errors'].append(repr(error))
        quiescent=(proc is None or kernel_done)and A.settle_readers(graph,start+seconds+10)
        if quiescent:
            try:
                if proc is not None:
                    proc.stdout.close();proc.stderr.close()
                closed=True
            except BaseException as error:graph['errors'].append(repr(error))
        record={'argv':graph['argv'],'pid':proc.pid if proc else None,'started':proc is not None,'elapsed_seconds':time.monotonic()-start,'exit':proc.returncode if proc else None,'kernel_completed':kernel_done,'io_completed':quiescent,'handles_closed':closed,'overflow':graph['overflow'],'timed_out':timed,'root_retained':not(quiescent and closed),'errors':list(graph['errors']),'executable_sha256':graph['executable_sha256'],'executable_realpath':graph['executable_realpath'],'stdout_sha256':A.digest(bytes(graph['buffers'][0])),'stderr_sha256':A.digest(bytes(graph['buffers'][1]))}
        emit('TOOL',record)
        if not record['root_retained']:TOOL_GRAPHS.remove(graph)
    if graph['errors']or timed or graph['overflow']or proc is None or proc.returncode!=0 or record['root_retained']:
        emit('TOOL_FAILURE',{'record':record,'stdout':bytes(graph['buffers'][0]).decode(errors='backslashreplace'),'stderr':bytes(graph['buffers'][1]).decode(errors='backslashreplace')})
        raise A.Rejected('tool failure or containment')
    return bytes(graph['buffers'][0]),bytes(graph['buffers'][1])

class Redirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,req,fp,code,msg,headers,newurl):
        parsed=urllib.parse.urlparse(newurl)
        A.require(parsed.scheme=='https' and parsed.hostname and (parsed.hostname in ('github.com','api.github.com','release-assets.githubusercontent.com','objects.githubusercontent.com') or parsed.hostname.endswith('.blob.core.windows.net')), 'untrusted download redirect')
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

def dependency_paths(raw,source_root,toolroot,build):
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
    required={'windows.h','map','vector','memory','new','array','utility'}
    A.require(required<={Path(x['path']).name for x in result},'SDK/STL dependencies missing')
    return result

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
                candidate=value
                for prefix in ('-I','-L','-isystem','-iquote','-idirafter','--sysroot=','-fdebug-compilation-dir=','-resource-dir='):
                    if value.startswith(prefix)and len(value)>len(prefix):candidate=value[len(prefix):];break
                if Path(candidate).is_absolute():
                    p=Path(candidate).resolve()
                    A.require(any(p.is_relative_to(root)for root in roots),'ambient implicit path')
    A.require(commands,'missing expanded driver commands');return commands

def link_dependencies(text,expanded,toolroot,build,env):
    reads=[];loads=[]
    for line in text.splitlines():
        m=re.fullmatch(r'(?:ld\.lld|lld-link): Reading (.+\.a)\s*',line)
        if m:reads.append(m[1]);continue
        m=re.fullmatch(r'(?:ld\.lld|lld-link): Loaded ([^()]+\.a)\(([^()]+)\) for (.+)',line)
        if m:loads.append(m.groups())
    A.require(reads and loads,'missing actual Reading/Loaded dependencies')
    archives={}
    for name in sorted(set(reads)):
        path=Path(name)
        if not path.is_absolute():
            candidates=[p.resolve()for p in toolroot.rglob(path.name)if p.is_file()]
            A.require(len(set(candidates))==1,'ambiguous archive path');path=candidates[0]
        path=path.resolve();A.require(path.is_relative_to(toolroot.resolve()),'foreign archive')
        archives[name]={'path':str(path),'sha256':sha(path),'members':{}}
    names={Path(x['path']).name for x in archives.values()}
    A.require('libc++.a'in names and 'libunwind.a'in names and any('builtins'in n for n in names)and any('ucrt'in n for n in names)and 'libkernel32.a'in names,'missing positive CRT/STL/unwind/builtins/system dependency')
    for archive,member,symbol in loads:
        candidates=[v for k,v in archives.items()if k==archive or Path(k).name==Path(archive).name]
        A.require(len(candidates)==1,'Loaded member has no unique Reading origin')
        raw,_=tool([toolroot/'bin/llvm-ar','p',candidates[0]['path'],member],build,env)
        A.require(raw,'empty loaded member');old=candidates[0]['members'].get(member)
        row={'sha256':A.digest(raw),'bytes':len(raw)};A.require(old is None or old==row,'member conflict');candidates[0]['members'][member]=row
    direct=[]
    for row in expanded:
        if not Path(row[0]).name.startswith('ld.lld'):continue
        for name in row[1:]:
            if name.endswith(('.o','.obj')):
                p=Path(name);p=p if p.is_absolute()else build/p;p=p.resolve()
                if p.is_relative_to(build.resolve()):A.require(p.name in A.TUS,'foreign direct object')
                else:A.require(p.is_relative_to(toolroot.resolve()),'foreign startup object')
                direct.append({'path':str(p),'sha256':sha(p),'origin':'three-object'if p.is_relative_to(build.resolve())else'publisher-startup'})
    A.require([Path(x['path']).name for x in direct if x['origin']=='three-object']==list(A.TUS) and all(Path(x['path'])==build/Path(x['path']).name for x in direct if x['origin']=='three-object'),'exact actual three link objects')
    A.require(any(x['origin']=='publisher-startup'for x in direct),'missing actual startup objects')
    return {'archives':archives,'loaded':loads,'direct':direct}

def current_job(auth,name):
    current=auth['context'];data=A.api('actions/runs/'+current['run']+'/attempts/'+current['attempt']+'/jobs?per_page=100')
    A.require(data['total_count']==len(data['jobs'])<=100,'complete current roles')
    rows=[x for x in data['jobs']if x['name']==name]
    A.require(len(rows)==1,'current named role');row=rows[0]
    A.require(row['status']=='in_progress'and row['head_sha']==current['source']and str(row['run_id'])==current['run']and str(row['run_attempt'])==current['attempt'],'current active role');return row

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
    target,_=tool([driver,'--no-default-config','-dumpmachine'],build,env);target=target.decode().strip();A.require(target=='x86_64-w64-windows-gnu','actual target')
    version,_=tool([driver,'--version'],build,env);A.require('23.1.3'in version.decode(),'actual compiler version')
    commands=[];deps={};objects={};expanded={};transcripts=[]
    for obj,source in A.TUS.items():
        dep=obj.replace('.obj','.d');argv=[str(driver),*COMMON]
        if obj!='caller.obj':argv.append('-fexceptions')
        argv+=['-MD','-MF',str(build/dep),'-c',str(root/source),'-o',str(build/obj)]
        diagnostic=[argv[0],'-###',*argv[1:]];out,err=tool(diagnostic,build,env);expanded[obj]=parse_driver(out+err,tc,root,build)
        cc1=[r for r in expanded[obj]if '-cc1'in r];A.require(len(cc1)==1 and ('-fexceptions'in cc1[0])==(obj!='caller.obj') and ('-fcxx-exceptions'in cc1[0])==(obj!='caller.obj'),'actual effective exception mode')
        stdout,stderr=tool(argv,build,env);transcripts.append({'object':obj,'argv':argv,'driver_expansion':(out+err).decode(),'stdout':stdout.decode(),'stderr':stderr.decode()})
        rawdep=(build/dep).read_bytes();deps[obj]=dependency_paths(rawdep,root,tc,build)
        A.require(any(x['relative']==A.HEADER and x['origin']=='source'for x in deps[obj]),'current private header dependency')
        save(bundle/dep,rawdep);shutil.copyfile(build/obj,bundle/obj)
        objects[obj]={'source':source,'source_blob':A.SOURCE_BLOBS[obj],'source_sha256':sha(root/source),'header_blob':A.HEADER_BLOB,'header_sha256':sha(root/A.HEADER),'sha256':sha(build/obj),'exceptions':obj!='caller.obj','argv':argv,'expanded':expanded[obj]}
        commands.append({'output':obj,'file':source,'argv':argv,'expanded':expanded[obj]})
    link=[str(driver),'--no-default-config',*(str(build/obj)for obj in A.TUS),'-static','-Wl,--verbose','-Wl,-Map,'+str(build/'helper-qualification.map'),'-o',str(build/'helper-qualification.exe')]
    out,err=tool([link[0],'-###',*link[1:]],build,env);exp=parse_driver(out+err,tc,root,build)
    stdout,stderr=tool(link,build,env);trace=(stdout+stderr).decode();lower=trace.lower()+(out+err).decode().lower()
    A.require(not any(x in lower for x in ('libgesher_posix','binder','gesher_crash_reporter_anchor','gesher_signal_mailbox_anchor','--wrap','wrap=free','wrap=realloc')),'foreign implicit root dependencies')
    linked=link_dependencies(trace,exp,tc,build,env)
    imports,_=tool([tc/'bin/llvm-readobj','--coff-imports',build/'helper-qualification.exe'],build,env)
    A.require(imports and not re.search(rb'(?i)jvm|dalvik|libart|binder|gesher_posix',imports),'imports role')
    symbols={}
    for obj in A.TUS:
        value,_=tool([tc/'bin/llvm-nm','--defined-only','--demangle',build/obj],build,env);symbols[obj]=value.decode()
    A.require('operator new('in symbols['qualification.obj']and 'operator new('not in symbols['helper.obj']and 'operator new('not in symbols['caller.obj'],'test-only new ownership')
    A.require('PrepareAliasFromNoExceptions'in symbols['caller.obj']and 'PreparePunchRequestFromNoExceptions'in symbols['caller.obj'],'actual noexceptions caller functions')
    shutil.copyfile(build/'helper-qualification.exe',bundle/'helper-qualification.exe');shutil.copyfile(build/'helper-qualification.map',bundle/'helper-qualification.map')
    save(bundle/'helper-qualification.imports.txt',imports);save(bundle/'compile-commands.json',commands);save(bundle/'dependency-manifest.json',deps)
    profile={'profile':A.PROFILE,'archive_name':ARCHIVE_NAME,'archive_sha256':ARCHIVE_SHA,'driver':str(driver),'driver_realpath':str(driver.resolve()),'driver_sha256':DRIVER_SHA,'linker_sha256':LINKER_SHA,'target':target,'version':version.decode(),'common_flags':COMMON,'configs':configs,'config_policy':'--no-default-config; no ambient HOME/include/library/config variables','objects':objects,'link_argv':link,'link_expanded':exp,'link_dependencies':linked,'no_ambient_injection':True,'dependencies_verified':True,'link_dependencies_verified':True}
    save(bundle/'compiler-profile.json',profile);save(bundle/'link-transcript.txt',A.canonical({'compile':transcripts,'link':{'argv':link,'expansion':(out+err).decode(),'stdout':stdout.decode(),'stderr':stderr.decode()}}));save(bundle/'object-symbols.json',symbols);save(bundle/'source-manifest.json',auth['authority']['manifest'])
    files={p.name:p.read_bytes()for p in bundle.iterdir()};A.require(set(files)==set(A.MEMBERS)-{'producer-manifest.json'},'producer exact fifteen payloads')
    manifest={'schema':1,'context':auth['context'],'authority':{k:auth['authority'][k]for k in ('guard','guard_blob','template','caller_authority')},'producer_id':job['id'],'members':{p:{'bytes':len(v),'sha256':A.digest(v)}for p,v in files.items()},'role':A.PROFILE}
    save(bundle/'producer-manifest.json',manifest);A.require(sum(p.stat().st_size for p in bundle.iterdir())<=A.TOTAL_CAP,'bundle aggregate cap')
    # Verify using independently selected current role ID, not the manifest itself.
    local=dict(auth);local['roles']=dict(auth['roles'],**{'helper-producer':job});A.verify_bundle({p.name:p.read_bytes()for p in bundle.iterdir()},local)
    emit('PRODUCER_MANIFEST',{'sha256':sha(bundle/'producer-manifest.json'),'context':auth['context'],'producer_id':job['id'],'role':A.PROFILE,'qualification':False,'members':manifest['members']})

def release_consumer_directory(dest,parent):
    # No recursive deletion while a child/reader/image lease is retained.
    A.require(not A.LIVE_GRAPHS,'FAILED_CONTAINMENT: retain owned directory')
    A.require(dest.is_absolute() and dest.resolve().parent==parent.resolve() and re.fullmatch(r'mremap-helper-[1-9][0-9]*-[1-9][0-9]*-win(?:22|25)',dest.name),'owned absolute cleanup target')
    A.require(not dest.is_symlink() and not(getattr(dest.lstat(),'st_file_attributes',0)&0x400),'cleanup reparse target')
    shutil.rmtree(dest);A.require(not dest.exists(),'owned directory cleanup')

def consume():
    A.require(os.name=='nt','consumer Windows only');auth=A.authorize('helper-windows',('source-authority / verify','helper-producer'))
    image=os.environ.get('ImageOS');A.require(image in ('win22','win25'),'host image role');role='helper-windows-'+('2022'if image=='win22'else'2025');current_job(auth,role)
    artifact=A.verify_artifact(A.api('actions/runs/'+auth['context']['run']+'/artifacts?per_page=100'),auth['context'],auth['roles']['helper-producer']);emit('DOWNLOAD_AUTHORITY',{'context':auth['context'],'roles':auth['roles'],'artifact':artifact,'source_authority':auth['authority'],'host_role':role})
    raw=download('https://api.github.com/repos/'+A.REPOSITORY+'/actions/artifacts/'+str(artifact['id'])+'/zip',A.TOTAL_CAP,artifact['digest'].split(':')[1],os.environ['GH_TOKEN'])
    files=A.zip_members(raw);manifest=A.verify_bundle(files,auth);dest=Path(os.environ['RUNNER_TEMP'])/('mremap-helper-'+auth['context']['run']+'-'+auth['context']['attempt']+'-'+image)
    A.require(not dest.exists(),'fresh consumer directory');dest.mkdir()
    try:
        for name,value in files.items():(dest/name).write_bytes(value)
        os.environ.pop('GH_TOKEN',None);os.environ.pop('GITHUB_TOKEN',None)
        record,result=A.owned_helper(dest/'helper-qualification.exe',manifest['members']['helper-qualification.exe']['sha256'])
    finally:
        released=False
        if not A.LIVE_GRAPHS:
            release_consumer_directory(dest,Path(os.environ['RUNNER_TEMP']).resolve());released=True
        emit('DIRECTORY',{'path':str(dest),'released':released,'retained_graphs':len(A.LIVE_GRAPHS)})
    A.require(released,'retained containment failure')
    emit('RESULT',{'context':auth['context'],'host_role':role,'producer_id':manifest['producer_id'],'artifact_id':artifact['id'],'artifact_digest':artifact['digest'],'producer_manifest_sha256':A.digest(files['producer-manifest.json']),'process':record,'budgets':result,'directory_released':True,'full_A_qualified':False,'production_registry_qualified':False,'selected_supplier_qualified':False})

def main():
    parser=argparse.ArgumentParser();parser.add_argument('mode',choices=('produce','consume'));args=parser.parse_args()
    A.require(os.environ.get('GITHUB_ACTIONS')=='true','CI execution only')
    try:
        (produce if args.mode=='produce'else consume)()
    except BaseException as error:
        emit('FAILURE',{'mode':args.mode,'error':repr(error),'retained_tool_graphs':len(TOOL_GRAPHS),'retained_native_graphs':len(A.LIVE_GRAPHS),'qualification':False});raise

if __name__=='__main__':main()
