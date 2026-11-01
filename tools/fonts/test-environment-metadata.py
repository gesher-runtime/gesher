"""Pure actual API identity and synthetic owned-text controls. Zero native execution."""
import contextlib,copy,importlib.util,io,json,subprocess,tempfile,types
from pathlib import Path

def load(name):
 spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(name+'.py'));value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def main():
 reuse=load('environment-supplier');assembly=load('environment-assemble');original=load('assemble-core-runtime');suppliers=load('baseline-supplier')
 golden=json.loads(Path('tests/fonts-environment/f78-metadata.json').read_text(encoding='utf-8'))
 def identity(value):
  with contextlib.redirect_stdout(io.StringIO()):reuse.validate(value['run'],value['jobs'],value['artifacts'])
 identity(golden);negatives=0
 mutations=[('source',lambda v:v['run'].update(head_sha='0'*40)),('run',lambda v:v['run'].update(id=-1)),('attempt',lambda v:v['run'].update(run_attempt=2)),('whole-run-green',lambda v:v['run'].update(conclusion='success')),('wrong-workflow',lambda v:v['run'].update(path='ci.yml')),('head-repository',lambda v:v['run']['head_repository'].update(id=-1)),('missing-job',lambda v:v['jobs']['jobs'].pop()),('wrong-producer-role',lambda v:v['jobs']['jobs'][0].update(name='other-role')),('failed-current-consumer',lambda v:next(x for x in v['jobs']['jobs'] if x['id']==124261932573).update(conclusion='failure')),('OLD-runtime-executed',lambda v:next(x for x in v['jobs']['jobs'] if x['id']==124261932732).update(conclusion='success')),('job-source',lambda v:v['jobs']['jobs'][0].update(head_sha='0'*40)),('job-attempt',lambda v:v['jobs']['jobs'][0].update(run_attempt=2)),('artifact-id',lambda v:v['artifacts']['artifacts'][0].update(id=-1)),('artifact-digest',lambda v:v['artifacts']['artifacts'][0].update(digest='sha256:'+'0'*64)),('artifact-expired',lambda v:v['artifacts']['artifacts'][0].update(expired=True)),('artifact-source',lambda v:v['artifacts']['artifacts'][0]['workflow_run'].update(head_sha='0'*40)),('artifact-outside-producer',lambda v:v['artifacts']['artifacts'][0].update(created_at='2020-01-01T00:00:00Z')),('duplicate-role',lambda v:v['artifacts']['artifacts'][1].update(name='fonts-core-runtime'))]
 for label,change in mutations:
  bad=copy.deepcopy(golden);change(bad)
  try:identity(bad)
  except (RuntimeError,KeyError,AssertionError):negatives+=1
  else:raise AssertionError('Accepted diagnostic supplier negative '+label)
 source=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
 context={'source':source,'run':'metadata-only','attempt':'1','repository':'gesher-runtime/gesher','repository_id':'1426153186'}
 assembly.context=lambda:context
 # The actual fixed executable hash constants are tested by identity above and
 # live archive verification. For ownership mutations use explicitly nonbinary
 # text payloads; do not manufacture a matching executable artifact locally.
 fake=types.SimpleNamespace(**vars(reuse));fake.EXE_SHA={name:None for name in reuse.EXE_SHA}
 real_module=assembly.module;assembly.module=lambda name,file:fake if file=='environment-supplier.py' else real_module(name,file)
 with tempfile.TemporaryDirectory(prefix='font-env-ownership-metadata-') as temporary:
  root=Path(temporary);fonts={'fonts/'+(e.text or '').strip() for e in original.ET.parse('config/fonts/fonts.xml').getroot().iter('font')}
  names=fonts|original.TIMEZONE|{'core-oj.jar','core-libart.jar','core-icu4j.jar','icudt76l.dat','FONTS-NOTICES.txt','TZDATA-NOTICES.txt','fonts/fonts.xml','fonts/windows-fonts.xml','dalvikvm.exe','gesher-exec.exe','gesher_arm64.dll'}
  for name in names:
   p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(Path('config/'+name).read_bytes() if name in ('fonts/fonts.xml','fonts/windows-fonts.xml') else ('NONEXECUTABLE_METADATA_ONLY '+name).encode())
  members=original.inventory(root);fake.EXE_SHA={name:members[name] for name in reuse.EXE_SHA}
  def receipt(role):
   spec=suppliers.SUPPLIERS[role];return {'role':role,'source':spec['source'],'run':spec['run'],'attempt':1,'repository':suppliers.REPO,'repository_id':suppliers.REPO_ID,'artifact':{'id':spec['id'],'digest':spec['digest'],'expired':False},'producer':{'id':spec['job'],'name':spec['producer'],'conclusion':'success'},'members':members}
  data=receipt('qualified-data');translator=receipt('qualified-translator')
  owners={p:{'role':'QUALIFIED_1BF8_DATA_ONLY','source':data['source'],'sha256':members[p]} for p in original.data_files(root)}
  owners.update({p:{'role':reuse.ROLE,'source':reuse.SOURCE,'sha256':members[p]} for p in reuse.EXE_SHA})
  owners['gesher_arm64.dll']={'role':'QUALIFIED_1BF8_TRANSLATOR','source':translator['source'],'sha256':members['gesher_arm64.dll']}
  good={'verifier_context':context,'runtime_role':reuse.ROLE,'runtime_source':reuse.SOURCE,'runtime_run':reuse.RUN,'runtime_attempt':1,'runtime_manifest_sha256':reuse.MANIFEST_SHA,'whole_runtime_run_green':False,'fallback_qualified':False,'compatibility':original.compatibility_proof(source),'owners':owners,'data_supplier':data,'translator_supplier':translator}
  def check(record):
   (root/'font-env-assembly.json').write_text(json.dumps(record),encoding='utf-8')
   with contextlib.redirect_stdout(io.StringIO()):assembly.verify(root)
  check(good)
  changes=[('wrong-source-label',lambda v:v.update(runtime_source='0'*40)),('fresh-role-label',lambda v:v.update(runtime_role='FRESH_CURRENT_RUNTIME')),('wrong-run',lambda v:v.update(runtime_run=0)),('wrong-attempt',lambda v:v.update(runtime_attempt=2)),('wrong-manifest',lambda v:v.update(runtime_manifest_sha256='0'*64)),('whole-green',lambda v:v.update(whole_runtime_run_green=True)),('fallback-claim',lambda v:v.update(fallback_qualified=True)),('wrong-verifier-run',lambda v:v['verifier_context'].update(run='wrong')),('OLD-exe',lambda v:v['owners']['dalvikvm.exe'].update(source=original.OLD)),('wrong-exe-hash',lambda v:v['owners']['dalvikvm.exe'].update(sha256='0'*64)),('data-role',lambda v:v['data_supplier'].update(role='old-runtime')),('translator-digest',lambda v:v['translator_supplier']['artifact'].update(digest='sha256:'+'0'*64)),('translator-job',lambda v:v['translator_supplier']['producer'].update(id=-1))]
  for label,change in changes:
   bad=copy.deepcopy(good);change(bad)
   try:check(bad)
   except (AssertionError,KeyError,RuntimeError):negatives+=1
   else:raise AssertionError('Accepted ownership negative '+label)
  for name in ('hidden-old.dll','boot.art','fonts/unknown.ttf'):
   p=root/name;p.write_text('NONEXECUTABLE_METADATA_ONLY',encoding='utf-8')
   try:check(good)
   except (AssertionError,KeyError,RuntimeError):negatives+=1
   else:raise AssertionError('Accepted unowned path '+name)
   p.unlink()
  check(good)
 print('FONT_ENV_METADATA_PASS actual_terminal_supplier_identity=1 synthetic_owned_files='+str(len(owners))+' negatives='+str(negatives)+' native_processes_started=0')
if __name__=='__main__':main()
