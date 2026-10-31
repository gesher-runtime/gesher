"""Pure metadata controls only. Every positive process/tool/budget row is SYNTHETIC.

No compiler, CMake, native process, network, artifact fetch or actual PE reader.
"""
import ast
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import stat
import tempfile
import unittest
import zipfile

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
        self.assertNotIn('source_contract',source);self.assertNotIn('producer_receipt',source)
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
        self.reject(lambda:A.owned_helper('not-run.exe','0'*64))
        self.reject(lambda:P.tool(['not-run'],Path.cwd(),{},1))
        self.reject(lambda:P.download('https://invalid',1,'0'*64))
        self.reject(lambda:A.api('not-run'))

    def test_driver_and_dependency_metadata_only(self):
        with tempfile.TemporaryDirectory()as temp:
            root=Path(temp);tool=root/'publisher';build=root/'build';source=root/'source'
            for p in (tool/'bin',tool/'include',build,source):p.mkdir(parents=True,exist_ok=True)
            (tool/'bin/clang').write_text('SYNTHETIC')
            raw=(' "'+str(tool/'bin/clang')+'" "-cc1"\n').encode();self.assertEqual(len(P.parse_driver(raw,tool)),1)
            self.reject(lambda:P.parse_driver(b'no commands',tool))
            self.reject(lambda:P.parse_driver(b' "/foreign/clang" "-cc1"\n',tool))
            paths=[]
            for name in ('windows.h','map','vector','memory','new','array','utility'):
                p=tool/'include'/name;p.write_text('SYNTHETIC_HEADER');paths.append(p)
            dep=('x.obj: '+' '.join(shlex_quote(p)for p in paths)+'\n').encode();self.assertEqual(len(P.dependency_paths(dep,source,tool,build)),7)
            self.reject(lambda:P.dependency_paths(b'x.obj: /foreign/header.h\n',source,tool,build))
            self.reject(lambda:P.dependency_paths(b'x.obj: \n',source,tool,build))

    def test_recipe_source_isolated_three_object_and_no_native_test(self):
        authority=(ROOT/'tools/file_mremap/helper_authority.py').read_text();producer=(ROOT/'tools/file_mremap/helper_producer.py').read_text()
        ast.parse(authority);ast.parse(producer)
        self.assertEqual(len(A.TUS),3);self.assertEqual(len(A.MEMBERS),16)
        self.assertEqual(P.COMMON,A.COMMON);self.assertIn('--no-default-config',A.COMMON);self.assertIn('-mno-ms-bitfields',A.COMMON);self.assertIn('-fsized-deallocation',A.COMMON)
        self.assertNotIn("['cmake'",producer);self.assertNotIn("['ninja'",producer)
        self.assertLess(authority.index('LIVE_GRAPHS.append(graph)'),authority.index('proc=subprocess.Popen'))
        self.assertLess(authority.index("graph['image_handle']=image_handle"),authority.index('proc=subprocess.Popen'))
        self.assertIn("if record['io_completed']:",authority);self.assertIn("slot['started']",authority)
        self.assertIn('A.verify_artifact(A.api',producer);self.assertIn('A.verify_bundle(files,auth)',producer)
        self.assertIn("os.environ.pop('GH_TOKEN',None)",producer)

def shlex_quote(path):
    import shlex
    return shlex.quote(str(path))

if __name__=='__main__':
    unittest.main()
