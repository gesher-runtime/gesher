"""Pure ownership/inventory adversarial controls; synthetic text, never native execution."""
import contextlib,copy,importlib.util,io,json,subprocess,tempfile
from pathlib import Path

def load(name):
 spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(name+'.py'));module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def main():
 assembly=load('assemble-core-runtime');runtime=load('core-runtime-manifest');suppliers=load('baseline-supplier')
 source=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
 context={'source':source,'run':'metadata-test-run','attempt':'1','repository':'gesher-runtime/gesher','repository_id':'1426153186'}
 runtime.context=lambda:context
 original=assembly.load_module
 assembly.load_module=lambda name,file:runtime if file=='core-runtime-manifest.py' else original(name,file)
 compatible=assembly.compatibility_proof(source)
 negatives=0
 with tempfile.TemporaryDirectory(prefix='gesher-font-owner-metadata-') as temporary:
  root=Path(temporary)/'metadata-only';root.mkdir()
  fonts={'fonts/'+(font.text or '').strip() for font in assembly.ET.parse('config/fonts/fonts.xml').getroot().iter('font')}
  names=fonts|assembly.TIMEZONE|{'core-oj.jar','core-libart.jar','core-icu4j.jar','icudt76l.dat','FONTS-NOTICES.txt','TZDATA-NOTICES.txt','fonts/fonts.xml','fonts/windows-fonts.xml',*assembly.EXECUTABLES,'gesher_arm64.dll'}
  for name in names:
   file=root/name;file.parent.mkdir(parents=True,exist_ok=True)
   file.write_bytes(Path('config/'+name).read_bytes() if name in ('fonts/fonts.xml','fonts/windows-fonts.xml') else ('NONEXECUTABLE_METADATA_ONLY '+name).encode())
  members=assembly.inventory(root)
  def receipt(role):
   spec=suppliers.SUPPLIERS[role]
   return {'role':role,'source':spec.get('source',suppliers.SOURCE),'run':spec.get('run',suppliers.RUN),'attempt':1,'artifact':{'id':spec['id'],'digest':spec['digest']},'producer':{'id':spec['job']},'members':members}
  base=receipt('qualified-data');translator=receipt('qualified-translator')
  proof={'role':'fresh-current-cli-runtime','context':context,'inputs':runtime.inputs(),'compiler':{'sha256':runtime.COMPILER_SHA},'web3_query_tests':'OFF','canonical_pthread':{name:{'sha256':runtime.PTHREAD_SHA} for name in ('libpthread.a','libwinpthread.a')},'outputs':{name:members[name] for name in assembly.EXECUTABLES}}
  owners={name:{'owner':'qualified-data','source':assembly.QUALIFIED,'sha256':members[name]} for name in assembly.data_files(root)}
  owners.update({name:{'owner':'fresh-current-exe','source':source,'sha256':members[name]} for name in assembly.EXECUTABLES})
  owners['gesher_arm64.dll']={'owner':'qualified-translator','source':assembly.QUALIFIED,'sha256':members['gesher_arm64.dll']}
  good={'role':'current-utf8','verifier_context':context,'owners':owners,'compatibility':compatible,'base_supplier':base,'translator_supplier':translator,'fresh_runtime_proof':proof,'full_qualification':False}
  manifest=root/'font-runtime-assembly.json'
  def check(record):
   manifest.write_text(json.dumps(record),encoding='utf-8')
   with contextlib.redirect_stdout(io.StringIO()):assembly.verify_assembly(root,'current-utf8')
  check(good)
  mutations=[('source',lambda x:x['verifier_context'].update(source='0'*40)),('attempt',lambda x:x['verifier_context'].update(attempt='2')),('run',lambda x:x['verifier_context'].update(run='wrong')),
   ('old-executable-owner',lambda x:x['owners']['dalvikvm.exe'].update(owner='old-runtime-exe',source=assembly.OLD)),('translator-digest',lambda x:x['translator_supplier']['artifact'].update(digest='sha256:'+'0'*64)),
   ('old-data-role',lambda x:x['base_supplier'].update(role='old-runtime')),('foreign-runtime-input',lambda x:x['fresh_runtime_proof']['inputs'].update({'compat/art/dalvikvm_main.cc':'0'*40})),
   ('query-build',lambda x:x['fresh_runtime_proof'].update(web3_query_tests='ON')),('private-pthread',lambda x:x['fresh_runtime_proof']['canonical_pthread']['libpthread.a'].update(sha256='0'*64)),
   ('old-executable-hash',lambda x:x['owners']['dalvikvm.exe'].update(sha256='0'*64)),('wrong-role',lambda x:x.update(role='old42076'))]
  for label,mutate in mutations:
   bad=copy.deepcopy(good);mutate(bad)
   try:check(bad)
   except (AssertionError,KeyError):negatives+=1
   else:raise AssertionError('Accepted ownership negative '+label)
  for name in ('old-hidden.dll','boot.art','fonts/extra.ttf'):
   file=root/name;file.write_text('NONEXECUTABLE_METADATA_ONLY',encoding='utf-8')
   try:check(good)
   except (AssertionError,KeyError):negatives+=1
   else:raise AssertionError('Accepted unowned path '+name)
   file.unlink()
  file=root/'fonts'/next(iter(fonts)).split('/',1)[1];saved=file.read_bytes();file.write_bytes(saved+b'changed')
  try:check(good)
  except (AssertionError,KeyError):negatives+=1
  else:raise AssertionError('Accepted changed font bytes')
  file.write_bytes(saved);check(good)
 print('FONT_CORE_ASSEMBLY_METADATA_PASS exact_owned_files='+str(len(owners))+' adversarial_controls='+str(negatives)+' payloads=SYNTHETIC_TEXT native_processes_started=0')
if __name__=='__main__':main()
