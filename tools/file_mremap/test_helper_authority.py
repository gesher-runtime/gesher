"""Pure metadata controls only. Every positive process/tool/budget row is SYNTHETIC.

No compiler, CMake, native process, network, artifact fetch or actual PE reader.
"""
import ast
import base64
import contextlib
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import stat
import tempfile
import types
import unittest
from unittest import mock
import zipfile

@contextlib.contextmanager
def pure_temp_directory():
    with tempfile.TemporaryDirectory(prefix='gesher-helper-pure-')as temp:
        target=Path(temp).resolve();parent=Path(tempfile.gettempdir()).resolve()
        A.require(target.is_absolute()and target.parent==parent and target.name.startswith('gesher-helper-pure-'),'owned pure fixture directory')
        yield str(target)
        A.require(target.resolve()==target and target.parent==parent,'pure fixture cleanup target')

def load(name):
    spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(name+'.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
A=load('helper_authority');P=load('helper_producer');ROOT=Path(__file__).resolve().parents[2]

def synthetic_output():
    lines=[]
    for family in ('alias','surgery'):
        for point in range(4):
            success=point==3;calls=point+1 if not success else point
            lines.append(json.dumps({'event':'allocation','family':family,'budget':point,'allocation_calls':calls,'injected':0 if success else 1,'status':'OK'if success else'NO_MEMORY','outstanding':0,'unchanged_owner_refs':True},separators=(',',':')))
            if family=='alias':lines.append(f'PREPARE_ALLOCATION point={point} calls={calls} status={0 if success else 1} empty={0 if success else 1} released=1')
            else:lines.append(f'SURGERY_ALLOCATION point={point} calls={calls} status={0 if success else 1} released=1 refs=3')
    for family in ('LEDGER','REQUEST'):
        for point in range(2):
            prefix='capacity=128 combined_pages=129 'if family=='LEDGER'else''
            lines.append(f'PUNCH_{family}_ALLOCATION {prefix}point={point} calls=1 injected={1 if point==0 else 0} status={1 if point==0 else 0} released=1')
    return ('\n'.join(lines+['PREPARE_RESULT failures=3 success=1 live=0 failed=0'])+'\n').encode()

def synthetic_context():
    return {'source':'a'*40,'run':'123','attempt':'2','repository':A.REPOSITORY,'repository_id':A.REPOSITORY_ID,'profile':A.PROFILE}

def synthetic_run():
    c=synthetic_context();return {'id':123,'run_attempt':2,'head_sha':c['source'],'name':A.WORKFLOW,'path':A.CALLER,'head_branch':A.BRANCH,'event':'push','actor':{'login':'palmoni5'},'triggering_actor':{'login':'palmoni5'},'repository':{'id':A.REPOSITORY_ID,'full_name':A.REPOSITORY},'head_repository':{'id':A.REPOSITORY_ID,'full_name':A.REPOSITORY},'referenced_workflows':[{'path':A.REPOSITORY+'/'+A.GUARD+'@'+'b'*40,'sha':'b'*40}]}

def synthetic_job():
    return {'id':42,'name':'helper-producer','head_sha':'a'*40,'run_id':123,'run_attempt':2,'status':'completed','conclusion':'success','started_at':'2026-10-31T00:00:00Z','completed_at':'2026-10-31T00:10:00Z'}

def synthetic_artifact():
    return {'id':55,'name':A.artifact_name(synthetic_context()),'expired':False,'size_in_bytes':1234,'digest':'sha256:'+'c'*64,'created_at':'2026-10-31T00:05:00Z','workflow_run':{'id':123,'head_sha':'a'*40,'repository_id':A.REPOSITORY_ID,'head_repository_id':A.REPOSITORY_ID}}

def synthetic_bundle():
    c=synthetic_context();auth={'context':c,'authority':{'guard':'b'*40,'guard_blob':'c'*40,'template':'d'*40,'caller_authority':'e'*40,'manifest':{'SYNTHETIC':True}},'roles':{'helper-producer':synthetic_job()}}
    files={name:b'SYNTHETIC_NOT_EXECUTABLE\n'for name in A.MEMBERS}
    objects={};commands=[];deps={}
    for obj,src in A.TUS.items():
        flags=list(A.COMMON)+(['-fexceptions']if obj!='caller.obj'else[])
        argv=['/publisher/bin/x86_64-w64-mingw32-clang++',*flags,'-MD','-MF','/owned/'+obj.replace('.obj','.d'),'-c','/repo/'+src,'-o','/owned/'+obj]
        cc=['/publisher/bin/clang','-cc1']+(['-fexceptions','-fcxx-exceptions']if obj!='caller.obj'else[])
        row={'source':src,'source_blob':A.SOURCE_BLOBS[obj],'source_sha256':A.digest(A.git(ROOT,'cat-file','blob',A.SOURCE_BLOBS[obj])),'header_blob':A.HEADER_BLOB,'header_sha256':A.digest(A.git(ROOT,'cat-file','blob',A.HEADER_BLOB)),'sha256':A.digest(files[obj]),'exceptions':obj!='caller.obj','argv':argv,'expanded':[cc]}
        objects[obj]=row;commands.append({'output':obj,'file':src,'argv':argv,'expanded':[cc]})
        deps[obj]=[{'origin':'toolchain','path':'/publisher/include/'+n,'relative':'include/'+n,'sha256':'f'*64,'bytes':10}for n in ('windows.h','map','vector','memory','new','array','utility')]+[{'origin':'source','path':'/repo/'+A.HEADER,'relative':A.HEADER,'sha256':row['header_sha256'],'bytes':100}]
    profile={'profile':A.PROFILE,'archive_sha256':P.ARCHIVE_SHA,'driver_sha256':P.DRIVER_SHA,'linker_sha256':P.LINKER_SHA,'target':'x86_64-w64-windows-gnu','common_flags':list(A.COMMON),'config_policy':'--no-default-config; no ambient HOME/include/library/config variables','objects':objects,'no_ambient_injection':True,'dependencies_verified':True,'link_dependencies_verified':True,'link_dependencies':{'archives':{'SYNTHETIC.a':{}},'loaded':[['SYNTHETIC.a','SYNTHETIC.obj','symbol']],'direct':[{'path':'/owned/'+x,'origin':'three-object'}for x in A.TUS]+[{'path':'/publisher/crt2.o','origin':'publisher-startup'}]},'link_expanded':[['/publisher/ld.lld']]}
    files['compiler-profile.json']=A.canonical(profile);files['source-manifest.json']=A.canonical(auth['authority']['manifest']);files['compile-commands.json']=A.canonical(commands);files['dependency-manifest.json']=A.canonical(deps)
    def seal():
        manifest={'schema':1,'context':auth['context'],'authority':{k:auth['authority'][k]for k in ('guard','guard_blob','template','caller_authority')},'producer_id':42,'role':A.PROFILE,'members':{p:{'bytes':len(v),'sha256':A.digest(v)}for p,v in files.items()if p!='producer-manifest.json'}}
        files['producer-manifest.json']=A.canonical(manifest)
    seal();return files,auth,seal

class Helper(unittest.TestCase):
    def setUp(self):
        # CI runs this ordinary suite too: no real API/tool/native entry point
        # can become armed merely because GITHUB_ACTIONS is true there.
        gate=mock.patch.dict(os.environ,{'GITHUB_ACTIONS':'false'})
        gate.start();self.addCleanup(gate.stop)

    def reject(self,fn):
        with self.assertRaises((A.Rejected,ValueError,KeyError,TypeError,AssertionError,zipfile.BadZipFile)):fn()

    def test_whole_frozen_source_and_all_five_pins(self):
        baseline=A.inventory(ROOT,A.BASE);self.assertEqual(A.digest(A.canonical(baseline)),A.BASE_INVENTORY);self.assertEqual(len(A.INPUT21),21)
        pins={p:'1'*40 for p in A.ADDITIONS};current=copy.deepcopy(baseline);current.update({p:{'mode':'100644','blob':b}for p,b in pins.items()});A.verify_inventory(baseline,current,pins)
        for path in (*A.ADDITIONS,'CMakeLists.txt','compat/posix/src/mman.cc','compat/posix/posix.cmake','.github/workflows/ci.yml'):
            bad=copy.deepcopy(current);bad[path]['blob']='2'*40;self.reject(lambda:A.verify_inventory(baseline,bad,pins))
        for path in A.ADDITIONS:
            bad=copy.deepcopy(current);del bad[path];self.reject(lambda:A.verify_inventory(baseline,bad,pins))
        bad=copy.deepcopy(current);bad['unknown.py']={'mode':'100644','blob':'1'*40};self.reject(lambda:A.verify_inventory(baseline,bad,pins))
        bad=copy.deepcopy(current);bad[A.CALLER]['mode']='120000';self.reject(lambda:A.verify_inventory(baseline,bad,pins))

    def test_whole_caller_guard_template_shadow_and_context(self):
        pins={p:'1'*40 for p in A.PYTHON_PATHS};guard=A.guard_bytes(pins,'2'*40);self.assertEqual(A.constants(guard),{'PINS':pins,'TEMPLATE':'2'*40})
        for before,after in [(b'5\n',b'50\n'),(b'actions: read',b'actions: write'),(b'python3 -I',b'python3 -B'),(b'helper_authority.py',b'unknown.py')]:
            mutant=guard.replace(before,after,1);self.assertNotEqual(mutant,guard);self.reject(lambda:A.constants(mutant))
        caller=A.caller_bytes('3'*40);self.assertEqual(caller.count(b'@'+b'3'*40),1);self.assertIn(b'name: helper-${{ matrix.os }}',caller);self.assertIn(b'timeout-minutes: 15',caller)
        source=(ROOT/'tools/file_mremap/helper_authority.py').read_text();tree=ast.parse(source)
        self.assertIn('sys.flags.isolated==1',source);self.assertIn('dirty/shadow checkout',source)
        imported={alias.name for node in ast.walk(tree) if isinstance(node,(ast.Import,ast.ImportFrom)) for alias in node.names}
        self.assertFalse({'source_contract','producer_receipt'} & imported)
        self.assertTrue(any(isinstance(x,ast.FunctionDef)and x.name=='guard_main'for x in tree.body))
        env={'GITHUB_ACTIONS':'true','GITHUB_REPOSITORY':A.REPOSITORY,'GITHUB_REPOSITORY_ID':str(A.REPOSITORY_ID),'GITHUB_REF':'refs/heads/'+A.BRANCH,'GITHUB_WORKFLOW':A.WORKFLOW,'GITHUB_WORKFLOW_REF':A.REPOSITORY+'/'+A.CALLER+'@refs/heads/'+A.BRANCH,'GITHUB_JOB':'helper-producer','GITHUB_ACTOR':'palmoni5','GITHUB_TRIGGERING_ACTOR':'palmoni5','GITHUB_EVENT_NAME':'push','GITHUB_SHA':'a'*40,'GITHUB_WORKFLOW_SHA':'a'*40,'GITHUB_RUN_ID':'123','GITHUB_RUN_ATTEMPT':'2'}
        self.assertEqual(A.context(env,'helper-producer'),synthetic_context())
        for key in env:
            bad=dict(env);bad[key]='wrong';self.reject(lambda:A.context(bad,'helper-producer'))

    def test_authenticated_roles_and_reference_not_producer_json(self):
        run=synthetic_run();A.verify_run(run,synthetic_context(),'b'*40)
        for key in ('id','run_attempt','head_sha','name','path','head_branch','event','referenced_workflows'):
            bad=copy.deepcopy(run);bad[key]=[]if key=='referenced_workflows'else'wrong';self.reject(lambda:A.verify_run(bad,synthetic_context(),'b'*40))
        for key in ('repository','head_repository','actor','triggering_actor'):
            bad=copy.deepcopy(run);bad[key]['login'if 'actor'in key else'id']='wrong';self.reject(lambda:A.verify_run(bad,synthetic_context(),'b'*40))
        row=synthetic_job();data={'total_count':1,'jobs':[row]};A.verify_jobs(data,synthetic_context(),['helper-producer'])
        for key,value in [('id',0),('name','old'),('head_sha','b'*40),('run_id',1),('run_attempt',1),('status','in_progress'),('conclusion','failure')]:
            bad=copy.deepcopy(data);bad['jobs'][0][key]=value;self.reject(lambda:A.verify_jobs(bad,synthetic_context(),['helper-producer']))
        self.reject(lambda:A.verify_jobs({'total_count':2,'jobs':[row]},synthetic_context(),['helper-producer']))

    def test_artifact_current_independent_role_digest_and_interval(self):
        artifact=synthetic_artifact();inventory={'total_count':1,'artifacts':[artifact]};A.verify_artifact(inventory,synthetic_context(),synthetic_job())
        for key,value in [('id',0),('name','stale'),('expired',True),('size_in_bytes',A.TOTAL_CAP+1),('digest','sha256:'+'x'*64),('created_at','2026-10-31T01:00:00Z')]:
            bad=copy.deepcopy(inventory);bad['artifacts'][0][key]=value;self.reject(lambda:A.verify_artifact(bad,synthetic_context(),synthetic_job()))
        for key in artifact['workflow_run']:
            bad=copy.deepcopy(inventory);bad['artifacts'][0]['workflow_run'][key]=0;self.reject(lambda:A.verify_artifact(bad,synthetic_context(),synthetic_job()))
        self.reject(lambda:A.verify_artifact({'total_count':2,'artifacts':[artifact,artifact]},synthetic_context(),synthetic_job()))

    def test_exact_bounded_extraction(self):
        def zipit(names,symlink=False):
            target=io.BytesIO()
            with zipfile.ZipFile(target,'w')as z:
                for name in names:
                    item=zipfile.ZipInfo(name)
                    if symlink:item.external_attr=(stat.S_IFLNK|0o777)<<16
                    z.writestr(item,b'SYNTHETIC')
            return target.getvalue()
        self.assertEqual(set(A.zip_members(zipit(A.MEMBERS))),set(A.MEMBERS))
        for names in (A.MEMBERS[:-1],(*A.MEMBERS,'extra'),(*A.MEMBERS[:-1],'../producer-manifest.json'),(*A.MEMBERS[:-1],'x/producer-manifest.json'),(*A.MEMBERS[:-1],'C:\\producer-manifest.json')):
            self.reject(lambda:A.zip_members(zipit(names)))
        self.reject(lambda:A.zip_members(zipit(A.MEMBERS,True)));self.reject(lambda:A.zip_members(b'notZIP'))
        self.reject(lambda:A.decode(b'{"x":1,"x":2}'))
        self.reject(lambda:A.decode(b'{"x":NaN}'))

    def test_bundle_source_object_header_flags_and_dependency_mutants(self):
        files,auth,seal=synthetic_bundle();A.verify_bundle(files,auth)
        for key in ('context','authority','producer_id','role','members'):
            bad=copy.deepcopy(files);value=A.decode(bad['producer-manifest.json']);value[key]={}if key in ('context','authority','members')else'wrong';bad['producer-manifest.json']=A.canonical(value);self.reject(lambda:A.verify_bundle(bad,auth))
        original=files['compiler-profile.json']
        for key,value in [('profile','FULL_A'),('driver_sha256','0'*64),('archive_sha256','0'*64),('target','arm64'),('common_flags',[]),('no_ambient_injection',False),('dependencies_verified',False),('config_policy','ambient')]:
            profile=A.decode(original);profile[key]=value;files['compiler-profile.json']=A.canonical(profile);seal();self.reject(lambda:A.verify_bundle(files,auth))
        for key,value in [('source_blob','0'*40),('header_blob','0'*40),('source_sha256','0'*64),('exceptions',False),('sha256','0'*64),('argv',['bad']),('expanded',[['cc','-cc1']])]:
            profile=A.decode(original);profile['objects']['helper.obj'][key]=value;files['compiler-profile.json']=A.canonical(profile);seal();self.reject(lambda:A.verify_bundle(files,auth))
        files['compiler-profile.json']=original;deps=A.decode(files['dependency-manifest.json']);deps['helper.obj'][0]['relative']='../escape';files['dependency-manifest.json']=A.canonical(deps);seal();self.reject(lambda:A.verify_bundle(files,auth))
        files,auth,seal=synthetic_bundle();files['helper.obj']+=b'foreign';seal();self.reject(lambda:A.verify_bundle(files,auth))

    def test_current_four_budget_grammars_and_negatives(self):
        raw=synthetic_output();value=A.parse_budgets(raw,b'',0);self.assertEqual(set(value['families']),{'alias','surgery','ledger','request'});self.assertFalse(value['production_registry_qualified'])
        mutations=[raw+b'extra\n',raw.replace(b'combined_pages=129',b'combined_pages=128'),raw.replace(b'injected=1',b'injected=0',1),raw.replace(b'point=0 calls=1',b'point=1 calls=1',1),raw.replace(b'unchanged_owner_refs":true',b'unchanged_owner_refs":false',1),raw.replace(b'outstanding":0',b'outstanding":1',1),raw.replace(b'failed=0',b'failed=1'),raw.replace(b'live=0',b'live=1'),raw.replace(b'capacity=128',b'capacity=127',1),raw.replace(b'PUNCH_REQUEST_ALLOCATION',b'UNKNOWN',1),raw[:-10],b'\n'.join(raw.splitlines()[1:])+b'\n']
        rows=raw.splitlines();mutations.append(b'\n'.join([rows[1],rows[0],*rows[2:]])+b'\n')
        for mutant in mutations:self.reject(lambda:A.parse_budgets(mutant,b'',0))
        for stderr,exit_code in ((b'error',0),(b'',1)):
            self.reject(lambda:A.parse_budgets(raw,stderr,exit_code))
        self.reject(lambda:A.parse_budgets(b'x'*(A.STREAM_CAP+1),b'',0))

    def test_process_containment_and_environment(self):
        good={'started':True,'pid':123,'creation_time':123456,'exe_verified':True,'kernel_completed':True,'io_completed':True,'handles_closed':True,'timed_out':False,'overflow':False,'exit':0};A.validate_completion(good)
        for key in good:
            bad=dict(good);bad[key]=1 if key in ('timed_out','overflow','exit')else 0;self.reject(lambda:A.validate_completion(bad))
        self.assertEqual(A.clean_environment({'SystemRoot':'Windows','GH_TOKEN':'secret','GITHUB_TOKEN':'secret','CPATH':'evil','LIBRARY_PATH':'evil','PATH':'system'}),{'SystemRoot':'Windows','PATH':'system'})
        self.reject(lambda:A.owned_helper('not-run.exe','0'*64,synthetic_context()))
        self.reject(lambda:P.tool(['not-run'],Path.cwd(),{},1))
        self.reject(lambda:P.download('https://invalid',1,'0'*64))
        self.reject(lambda:A.api('not-run'))

    def test_driver_and_dependency_metadata_only(self):
        with pure_temp_directory()as temp:
            root=Path(temp);tool=root/'publisher';build=root/'build';source=root/'source'
            for p in (tool/'bin',tool/'include',build,source):p.mkdir(parents=True,exist_ok=True)
            (tool/'bin/clang').write_text('SYNTHETIC')
            raw=(' "'+str(tool/'bin/clang')+'" "-cc1"\n').encode();self.assertEqual(len(P.parse_driver(raw,tool,source,build)),1)
            self.reject(lambda:P.parse_driver(b'no commands',tool,source,build))
            self.reject(lambda:P.parse_driver(b' "/foreign/clang" "-cc1"\n',tool,source,build))
            paths=[]
            for name in ('windows.h','map','vector','memory','new','array','utility'):
                p=tool/'include'/name;p.write_text('SYNTHETIC_HEADER');paths.append(p)
            dep=('x.obj: '+' '.join(shlex_quote(p)for p in paths)+'\n').encode();self.assertEqual(len(P.dependency_paths(dep,source,tool,build)),7)
            self.reject(lambda:P.dependency_paths(b'x.obj: /foreign/header.h\n',source,tool,build))
            self.reject(lambda:P.dependency_paths(b'x.obj: \n',source,tool,build))

    def test_owned_reader_quiescence_and_directory_retention(self):
        class Reader:
            def __init__(self,alive):self.alive=alive;self.joins=0
            def join(self,timeout):self.joins+=1
            def is_alive(self):return self.alive
        idle=Reader(False);live=Reader(True);unstarted=Reader(True)
        graph={'readers':[{'thread':idle,'started':True},{'thread':unstarted,'started':False}], 'errors':[]}
        self.assertTrue(A.settle_readers(graph,0));self.assertEqual(idle.joins,1);self.assertEqual(unstarted.joins,0)
        graph['readers'].append({'thread':idle,'started':False,'start_attempted':True});self.assertFalse(A.settle_readers(graph,0))
        graph['readers'].pop()
        graph['readers'].append({'thread':live,'started':True});self.assertFalse(A.settle_readers(graph,0))
        with pure_temp_directory()as temp:
            owned=Path(temp)/'mremap-helper-123-1-win22';owned.mkdir();(owned/'raw.txt').write_text('SYNTHETIC')
            P.A.LIVE_GRAPHS.append({'SYNTHETIC_RETAINED':True})
            try:
                self.reject(lambda:P.release_consumer_directory(owned,Path(temp)));self.assertTrue((owned/'raw.txt').exists())
            finally:P.A.LIVE_GRAPHS.pop()
            P.release_consumer_directory(owned,Path(temp));self.assertFalse(owned.exists())

    def test_driver_rejects_nonexistent_and_attached_foreign_paths(self):
        import shlex
        with pure_temp_directory()as temp:
            root=Path(temp);tc=root/'tc';source=root/'source';build=root/'build'
            for path in (tc/'bin',source,build):path.mkdir(parents=True,exist_ok=True)
            exe=tc/'bin/clang';exe.write_text('SYNTHETIC')
            def row(*args):return (' '+ ' '.join('"'+str(v).replace('\\','/')+'"'for v in (exe,*args))+'\n').encode()
            self.assertEqual(len(P.parse_driver(row('-cc1','-o',build/'notcreated.obj'),tc,source,build)),1)
            foreign=root/'foreign'/'notcreated.obj'
            for args in (('-o',foreign),('-I'+str(foreign),),('--sysroot='+str(foreign),),('-fdebug-compilation-dir='+str(foreign),),('@unseen.rsp',),('-I../../foreign',),('-o','../../foreign/new.obj')):
                self.reject(lambda:P.parse_driver(row(*args),tc,source,build))

    def test_every_base_blob_preserves_complete_history_without_body_inspection(self):
        baseline=A.inventory(ROOT,A.BASE);pins={p:'1'*40 for p in A.ADDITIONS}
        current=dict(baseline,**{p:{'mode':'100644','blob':v}for p,v in pins.items()})
        A.verify_inventory(baseline,current,pins)
        for path in baseline:
            bad=dict(current);bad[path]={'mode':baseline[path]['mode'],'blob':'0'*40}
            self.reject(lambda:A.verify_inventory(baseline,bad,pins))
        # No historical/selected/workflow leaf is exempted or projected away.
        self.assertGreater(len(baseline),1600)

    def test_lossless_chunked_plain_text_proofs(self):
        raw=('SYNTHETIC unicode \u05d0\n'*2000).encode()
        out=io.StringIO()
        with contextlib.redirect_stdout(out):A.emit_bytes('SYNTHETIC_21',raw)
        rows=out.getvalue().splitlines();joined=b''
        for index,row in enumerate(rows,1):
            match=re.fullmatch(r'FILE_MREMAP_HELPER_SYNTHETIC_21 part=(\d+)/(\d+) bytes=(\d+) sha256=([0-9a-f]{64}) base64=(.*)',row)
            self.assertIsNotNone(match);self.assertEqual(int(match[1]),index);self.assertEqual(int(match[2]),len(rows));self.assertEqual(int(match[3]),len(raw));self.assertEqual(match[4],A.digest(raw));self.assertLess(len(row),12500)
            joined+=base64.b64decode(match[5],validate=True)
        self.assertEqual(joined,raw)
        self.reject(lambda:A.emit_bytes('BAD-LABEL',b'x'))
        files={name:b'SYNTHETIC_TEXT'for name in A.MEMBERS};out=io.StringIO()
        with contextlib.redirect_stdout(out):P.emit_text_members(files,synthetic_context())
        self.assertEqual(len(out.getvalue().splitlines()),13)
        self.assertNotIn('MEMBER_HELPER_OBJ',out.getvalue());self.assertNotIn('MEMBER_HELPER_QUALIFICATION_EXE',out.getvalue())

    def test_failure_binary_opacity_and_complete_text_bound(self):
        raw=b'SYNTHETIC_OBJECT_EVEN_IF_PRINTABLE'
        opaque=P.failure_output(raw,True)
        self.assertEqual(opaque,{'bytes':len(raw),'sha256':A.digest(raw),'kind':'BINARY_NOT_PRINTED'})
        self.assertEqual(P.failure_output(b'complete error\n')['text'],'complete error\n')
        for raw in (b'\x00binary',b'\xff',b'x'*(A.STREAM_CAP+1)):
            value=P.failure_output(raw);self.assertNotIn('text',value);self.assertEqual(value['bytes'],len(raw));self.assertEqual(value['sha256'],A.digest(raw))

    def test_exact_approved_source_sha_before_candidate_git_reads(self):
        with mock.patch.object(A,'git')as fake:
            self.reject(lambda:A.verify_source(ROOT,'a'*40,'b'*40))
            fake.assert_not_called()

    def test_launch_boundary_and_no_child_proof_both_graph_modules(self):
        for module in (A,P.A):
            graph=synthetic_graph()
            module.prove_unattempted_failure(graph)
            self.assertEqual(graph['launch_state'],'PROVEN_FAILED_NO_CHILD')
            self.assertTrue(module.settle_graph(graph,0));self.assertFalse(graph['kernel_completed'])
            for error in (OSError('synthetic constructor failure'),KeyboardInterrupt()):
                graph=synthetic_graph()
                def acquire(*args,**kwargs):
                    self.assertEqual(graph['launch_state'],'STARTING_UNCERTAIN')
                    self.assertIsNone(graph['process']);raise error
                with mock.patch.object(module.subprocess,'Popen',side_effect=acquire):
                    with self.assertRaises(type(error)):module.launch_owned(graph,['SYNTHETIC_NEVER_EXECUTED'])
                module.prove_unattempted_failure(graph)
                self.assertEqual(graph['launch_state'],'STARTING_UNCERTAIN')
                self.assertFalse(module.settle_graph(graph,0));self.assertFalse(graph['handles_closed'])
            graph=synthetic_graph();proc=SyntheticProcess()
            with mock.patch.object(module.subprocess,'Popen',return_value=proc)as acquire:
                self.assertIs(module.launch_owned(graph,['SYNTHETIC_NEVER_EXECUTED']),proc)
                self.assertIs(graph['process'],proc);self.assertEqual(graph['launch_state'],'RETURNED_OWNED')
                with self.assertRaises(module.Rejected):module.launch_owned(graph,['NO_SECOND_LAUNCH'])
                self.assertEqual(acquire.call_count,1)

    def test_exact_owned_settlement_requires_kernel_readers_image_and_handles(self):
        for kind in ('TOOL','NATIVE'):
            graph=synthetic_graph(kind);proc=SyntheticProcess();graph.update(process=proc,launch_state='RETURNED_OWNED')
            self.assertFalse(A.settle_graph(graph,0));self.assertEqual(proc.closes,0)
            proc.done=True;reader=SyntheticReader(True)
            graph['readers']=[{'thread':reader,'started':True,'start_attempted':True}]
            self.assertFalse(A.settle_graph(graph,0));self.assertTrue(graph['kernel_completed']);self.assertFalse(proc.stdout.closed)
            reader.alive=False;graph['readers'].append({'thread':reader,'started':False,'start_attempted':True})
            self.assertFalse(A.settle_graph(graph,0));graph['readers'].pop()
            proc.stdout.fail=True;self.assertFalse(A.settle_graph(graph,0));self.assertFalse(graph['handles_closed'])
            proc.stdout.fail=False
            graph.update(image_state='STARTING_UNCERTAIN');self.assertFalse(A.settle_graph(graph,0))
            graph.update(image_state='RETURNED_OWNED',image_handle='SYNTHETIC_HANDLE',kernel=mock.Mock())
            graph['kernel'].CloseHandle.return_value=False
            self.assertFalse(A.settle_graph(graph,0));self.assertEqual(graph['image_handle'],'SYNTHETIC_HANDLE')
            graph['kernel'].CloseHandle.return_value=True
            self.assertTrue(A.settle_graph(graph,0));self.assertTrue(graph['handles_closed']);self.assertIsNone(graph['image_handle'])
            self.assertTrue(A.settle_graph(graph,0));self.assertEqual(proc.closes,1 if kind=='NATIVE'else 0)
            self.assertEqual(proc.kills,0);self.assertIn('settlement_error',graph)
        graph=synthetic_graph();proc=SyntheticProcess();proc.done=True
        graph.update(launch_state='STARTING_UNCERTAIN',process=proc)
        self.assertTrue(A.settle_graph(graph,0));self.assertEqual(graph['launch_state'],'RETURNED_OWNED')

    def test_effective_tool_and_native_failed_constructor_keep_original_graphs(self):
        path_type=type(Path.cwd())
        def unknown_tool(*args,**kwargs):
            graph=P.TOOL_GRAPHS[-1]
            self.assertEqual(graph['launch_state'],'STARTING_UNCERTAIN');raise KeyboardInterrupt()
        try:
            with mock.patch.dict(os.environ,{'GITHUB_ACTIONS':'true'}),mock.patch.object(P.os,'name','posix'),mock.patch.object(P,'Path',path_type),mock.patch.object(P,'sha',return_value='e'*64),mock.patch.object(P.subprocess,'Popen',side_effect=unknown_tool),contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(P.A.Rejected):P.tool(['SYNTHETIC_NEVER_EXECUTED'],Path.cwd(),{})
            graph=P.TOOL_GRAPHS[-1];self.assertEqual(graph['launch_state'],'STARTING_UNCERTAIN')
            self.assertFalse(graph['handles_closed']);self.assertFalse(P.settle_lifetime_once())
        finally:P.TOOL_GRAPHS.clear()
        # A fake ctypes module prevents EVERY Win32 acquisition/query. This
        # exercises the actual owned_helper→launch_owned failure flow as source.
        fake_kernel=mock.Mock();fake_kernel.CreateFileW.return_value=42
        wt=types.SimpleNamespace(**{n:object()for n in ('LPCWSTR','DWORD','LPVOID','HANDLE','BOOL')})
        ctypes=types.ModuleType('ctypes');ctypes.wintypes=wt
        ctypes.WinDLL=mock.Mock(return_value=fake_kernel);ctypes.c_void_p=lambda value:types.SimpleNamespace(value=value)
        def unknown_native(*args,**kwargs):
            graph=P.A.LIVE_GRAPHS[-1]
            self.assertEqual(graph['launch_state'],'STARTING_UNCERTAIN');self.assertEqual(graph['image_handle'],42)
            raise KeyboardInterrupt()
        with pure_temp_directory()as temp:
            image=Path(temp)/'SYNTHETIC_NOT_AN_EXE';image.write_bytes(b'SYNTHETIC')
            try:
                with mock.patch.dict(os.environ,{'GITHUB_ACTIONS':'true'}),mock.patch.object(P.A.os,'name','nt'),mock.patch.object(P.A,'Path',path_type),mock.patch.dict(P.sys.modules,{'ctypes':ctypes}),mock.patch.object(P.subprocess,'Popen',side_effect=unknown_native),contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaises(P.A.Rejected):P.A.owned_helper(image,P.A.digest(b'SYNTHETIC'),synthetic_context())
                graph=P.A.LIVE_GRAPHS[-1]
                self.assertEqual(graph['launch_state'],'STARTING_UNCERTAIN');self.assertEqual(graph['image_handle'],42)
                self.assertFalse(graph['kernel_completed']);self.assertFalse(graph['handles_closed']);fake_kernel.CloseHandle.assert_not_called()
                self.assertFalse(P.settle_lifetime_once());self.assertTrue(P.lifetime_unresolved())
            finally:P.A.LIVE_GRAPHS.clear()

    def test_lifetime_policy_unknown_launch_and_cleanup_failure_forbid_exit(self):
        for roots in (P.TOOL_GRAPHS,P.A.LIVE_GRAPHS):
            graph=synthetic_graph();graph['launch_state']='STARTING_UNCERTAIN';roots.append(graph)
            try:
                self.assertFalse(P.settle_lifetime_once());self.assertTrue(P.lifetime_unresolved());self.assertIn(graph,roots)
                with pure_temp_directory()as temp:
                    owned=Path(temp)/'mremap-helper-123-1-win22';owned.mkdir()
                    self.reject(lambda:P.release_consumer_directory(owned,Path(temp)));self.assertTrue(owned.exists())
            finally:roots.remove(graph)
        directory={'path':'SYNTHETIC_PATH','parent':'SYNTHETIC_PARENT'};P.DIRECTORY_ROOTS.append(directory)
        try:
            with mock.patch.object(P,'release_consumer_directory',side_effect=OSError('synthetic cleanup failure')):
                for _ in range(3):self.assertFalse(P.settle_lifetime_once())
            self.assertTrue(P.lifetime_unresolved());self.assertIn(directory,P.DIRECTORY_ROOTS)
            self.assertEqual(directory['settlement_error_count'],3)
            with mock.patch.object(P,'release_consumer_directory')as release:
                self.assertTrue(P.settle_lifetime_once());release.assert_called_once()
        finally:
            if directory in P.DIRECTORY_ROOTS:P.DIRECTORY_ROOTS.remove(directory)

    def test_cli_error_stays_failed_and_blocks_until_exact_owned_settlement(self):
        proc=SyntheticProcess();graph=synthetic_graph();graph.update(process=proc,launch_state='RETURNED_OWNED')
        reader=SyntheticReader(True);graph['readers']=[{'thread':reader,'started':True}]
        original=P.A.Rejected('SYNTHETIC_PERMANENT_FAILURE');pauses=[]
        def operation():P.TOOL_GRAPHS.append(graph);raise original
        def passive():
            self.assertTrue(P.lifetime_unresolved());self.assertIn(graph,P.TOOL_GRAPHS)
            self.assertFalse(proc.stdout.closed);pauses.append(1)
            if len(pauses)==1:proc.done=True
            else:reader.alive=False
        try:
            with mock.patch.dict(os.environ,{'GITHUB_ACTIONS':'true'}),mock.patch.object(P.sys,'argv',['helper','produce']),mock.patch.object(P,'produce',side_effect=operation),mock.patch.object(P,'quarantine_pause',side_effect=passive),mock.patch.object(P.subprocess,'Popen')as launch,contextlib.redirect_stdout(io.StringIO())as out:
                with self.assertRaises(P.A.Rejected)as failed:P.main()
                self.assertIs(failed.exception,original);launch.assert_not_called()
            self.assertEqual(len(pauses),2);self.assertFalse(P.lifetime_unresolved());self.assertTrue(proc.stdout.closed)
            self.assertNotIn('FILE_MREMAP_HELPER_RESULT',out.getvalue());self.assertEqual(proc.kills,0)
            # Already complete error exits as ERROR without waiting or converting.
            with mock.patch.object(P,'quarantine_pause')as pause,contextlib.redirect_stdout(io.StringIO()):P.finish_failure('produce',original)
            pause.assert_not_called()
        finally:
            if graph in P.TOOL_GRAPHS:P.TOOL_GRAPHS.remove(graph)

    def test_cli_barrier_survives_output_and_inprocess_interruption(self):
        graph=synthetic_graph();graph['launch_state']='STARTING_UNCERTAIN';P.TOOL_GRAPHS.append(graph)
        seen=[]
        original_settle=P.settle_lifetime_once
        def step():
            if len(seen)<2:
                self.assertTrue(P.lifetime_unresolved());seen.append(1)
                if len(seen)==1:raise KeyboardInterrupt()
                proc=SyntheticProcess();proc.done=True;graph.update(process=proc,launch_state='RETURNED_OWNED')
            return original_settle()
        try:
            with mock.patch.object(P,'emit',side_effect=OSError('synthetic output failure')),mock.patch.object(P,'settle_lifetime_once',side_effect=step),mock.patch.object(P,'quarantine_pause'):
                with self.assertRaises(OSError):P.finish_failure('produce',ValueError('permanent'))
            self.assertEqual(len(seen),2);self.assertFalse(P.lifetime_unresolved())
        finally:
            if graph in P.TOOL_GRAPHS:P.TOOL_GRAPHS.remove(graph)

    def test_actual_consume_unwind_retains_same_authority_graph_and_directory(self):
        owner=P.A;graph=synthetic_graph('NATIVE');proc=SyntheticProcess()
        graph.update(process=proc,launch_state='RETURNED_OWNED',image_handle='SYNTHETIC_IMAGE',kernel=mock.Mock())
        graph['kernel'].CloseHandle.return_value=True
        context=synthetic_context();auth={'context':context,'roles':{'helper-producer':{}},'authority':{}}
        manifest={'members':{'helper-qualification.exe':{'sha256':'e'*64}}}
        original=owner.Rejected('SYNTHETIC_CONSUME_FAILURE');pauses=[]
        def native(*args):
            self.assertIs(P.A,owner);owner.LIVE_GRAPHS.append(graph);raise original
        with pure_temp_directory()as temp:
            # Capture the real platform Path BEFORE the synthetic OS-role gate.
            path_type=type(Path(temp));dest=Path(temp)/'mremap-helper-123-2-win22'
            def passive():
                self.assertIs(P.A,owner);self.assertIn(graph,owner.LIVE_GRAPHS)
                self.assertTrue(P.DIRECTORY_ROOTS);self.assertTrue(dest.exists())
                self.assertFalse(proc.stdout.closed);self.assertEqual(graph['image_handle'],'SYNTHETIC_IMAGE')
                pauses.append(1);proc.done=True
            try:
                with mock.patch.dict(os.environ,{'GITHUB_ACTIONS':'true','RUNNER_TEMP':temp,'ImageOS':'win22','GH_TOKEN':'SYNTHETIC_NOT_REAL'}),mock.patch.object(P.os,'name','nt'),mock.patch.object(P,'Path',path_type),mock.patch.object(P.sys,'argv',['helper','consume']),mock.patch.object(owner,'authorize',return_value=auth),mock.patch.object(P,'current_job'),mock.patch.object(owner,'api'),mock.patch.object(owner,'verify_artifact',return_value={'id':1,'digest':'sha256:'+'d'*64}),mock.patch.object(P,'download',return_value=b'SYNTHETIC'),mock.patch.object(owner,'zip_members',return_value={'helper-qualification.exe':b'SYNTHETIC_NON_EXECUTABLE'}),mock.patch.object(owner,'verify_bundle',return_value=manifest),mock.patch.object(P,'emit_text_members'),mock.patch.object(owner,'owned_helper',side_effect=native),mock.patch.object(P,'quarantine_pause',side_effect=passive),mock.patch.object(P.subprocess,'Popen')as launch,contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaises(owner.Rejected)as failed:P.main()
                    self.assertIs(failed.exception,original);launch.assert_not_called()
                self.assertEqual(len(pauses),1);self.assertIs(P.A,owner)
                self.assertFalse(P.lifetime_unresolved());self.assertFalse(dest.exists());self.assertIsNone(graph['image_handle'])
            finally:
                if graph in owner.LIVE_GRAPHS:owner.LIVE_GRAPHS.remove(graph)
                P.DIRECTORY_ROOTS[:]=[d for d in P.DIRECTORY_ROOTS if d['path']!=dest]

    def test_batch_hashes_match_real_single_path_git_filters(self):
        names=tuple(dict.fromkeys((*A.ADDITIONS,*A.INPUT21)))
        batch=A.checked_out_hashes(ROOT,names)
        single={name:A.git(ROOT,'hash-object',name).decode().strip()for name in names}
        self.assertEqual(batch,single)
        for names in ((),('json.py\nhelper.py',),('"quoted',),('../escape',),('duplicate','duplicate')):
            with mock.patch.object(A.subprocess,'check_output')as fake:
                self.reject(lambda:A.checked_out_hashes(ROOT,names));fake.assert_not_called()

    def test_recipe_source_isolated_three_object_and_no_native_test(self):
        authority=(ROOT/'tools/file_mremap/helper_authority.py').read_text();producer=(ROOT/'tools/file_mremap/helper_producer.py').read_text()
        ast.parse(authority);ast.parse(producer)
        self.assertEqual(len(A.TUS),3);self.assertEqual(len(A.MEMBERS),16)
        self.assertEqual(P.COMMON,A.COMMON);self.assertIn('--no-default-config',A.COMMON);self.assertIn('-mno-ms-bitfields',A.COMMON);self.assertIn('-fsized-deallocation',A.COMMON)
        self.assertNotIn("['cmake'",producer);self.assertNotIn("['ninja'",producer)
        native=ast.get_source_segment(authority,next(x for x in ast.parse(authority).body if isinstance(x,ast.FunctionDef)and x.name=='owned_helper'))
        self.assertLess(native.index('LIVE_GRAPHS.append(graph)'),native.index('proc=launch_owned'))
        self.assertLess(native.index("graph['image_handle']=image_handle"),native.index('proc=launch_owned'))
        self.assertIn('proc=A.launch_owned(graph',producer);self.assertEqual(authority.count('subprocess.Popen('),1);self.assertNotIn('subprocess.Popen(',producer)
        self.assertIn("if not quiescent:return False",authority);self.assertIn("slot['started']",authority)
        self.assertIn('A.verify_artifact(A.api',producer);self.assertIn('A.verify_bundle(files,auth)',producer)
        self.assertIn("os.environ.pop('GH_TOKEN',None)",producer)

class SyntheticReader:
    def __init__(self,alive):self.alive=alive
    def join(self,timeout):pass
    def is_alive(self):return self.alive

class SyntheticStream:
    def __init__(self):self.closed=False;self.fail=False
    def close(self):
        if self.fail:raise OSError('SYNTHETIC_CLOSE_FAILURE')
        self.closed=True

class SyntheticProcess:
    def __init__(self):
        self.done=False;self.kills=0;self.closes=0;self.returncode=None
        self.stdin=SyntheticStream();self.stdout=SyntheticStream();self.stderr=SyntheticStream();self._handle=self
    def poll(self):return 1 if self.done else None
    def wait(self,timeout):
        if not self.done:raise RuntimeError('SYNTHETIC_LIVE_KERNEL')
        self.returncode=1;return 1
    def kill(self):self.kills+=1;raise AssertionError('passive policy must never kill')
    def Close(self):self.closes+=1

def synthetic_graph(kind='TOOL'):
    return {'kind':kind,'launch_state':'NOT_ATTEMPTED','process':None,'kernel_completed':False,'handles_closed':False,'readers':[],'errors':[]}

def shlex_quote(path):
    import shlex
    return shlex.quote(str(path))

if __name__=='__main__':
    unittest.main()
