"""CI-only comparison assembly with explicit fresh EXE, old-data and translator owners."""
import argparse,hashlib,importlib.util,json,os,shutil,subprocess,xml.etree.ElementTree as ET
from pathlib import Path
from font_records import emit

QUALIFIED='1bf8fad9ec3b423ddef8c97c1d72b25790974364'
OLD='42076a8f28c0d23eee5b10ee0acf0844be0dc277'
EXECUTABLES=('dalvikvm.exe','gesher-exec.exe')
TIMEZONE={'tzdata/etc/tz/tzdata','tzdata/etc/tz/tz_version',
 'tzdata/etc/tz/versioned/9/tzdata','tzdata/etc/tz/versioned/9/tz_version',
 'tzdata/etc/tz/versioned/9/tzlookup.xml','tzdata/etc/tz/versioned/9/telephonylookup.xml',
 *('tzdata/etc/tz/versioned/9/icu/'+x+'.res' for x in ('zoneinfo64','metaZones','timezoneTypes','windowsZones'))}
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def load_module(name,file):
 spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(file));module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
def inventory(root):
 result={}
 for p in root.rglob('*'):
  assert not p.is_symlink(),'Linked comparison input'
  if p.is_file():result[p.relative_to(root).as_posix()]=sha(p)
 return result
def supplier_receipt(root,role):
 source=load_module('fonts_suppliers','baseline-supplier.py')
 spec=source.SUPPLIERS[role];receipt=json.loads((root.parent/(root.name+'.receipt.json')).read_text(encoding='utf-8'))
 assert receipt['role']==role and receipt['source']==spec.get('source',source.SOURCE)
 assert receipt['run']==spec.get('run',source.RUN) and receipt['attempt']==1
 assert receipt['repository']==source.REPO and receipt['repository_id']==source.REPO_ID
 assert receipt['artifact']['id']==spec['id'] and receipt['artifact']['digest']==spec['digest'] and not receipt['artifact']['expired']
 assert receipt['producer']['id']==spec['job'] and receipt['producer']['name']==spec['producer'] and receipt['producer']['conclusion']=='success'
 assert receipt['members']==inventory(root),'Extracted supplier differs from its verified archive receipt'
 return receipt
def data_files(root):
 # An explicit source-derived list: no old executable, DLL, image, nested binary,
 # or arbitrary path below a broad directory prefix can enter the fresh runtime.
 assert (root/'fonts/fonts.xml').read_bytes()==Path('config/fonts/fonts.xml').read_bytes()
 assert (root/'fonts/windows-fonts.xml').read_bytes()==Path('config/fonts/windows-fonts.xml').read_bytes()
 xml=ET.parse('config/fonts/fonts.xml');assert xml.getroot().tag=='familyset'
 fonts=set()
 for font in xml.getroot().iter('font'):
  name=(font.text or '').strip();assert name and Path(name).name==name and '/' not in name and '\\' not in name and ':' not in name
  assert Path(name).suffix.lower() in ('.ttf','.ttc','.otf');fonts.add('fonts/'+name)
 wanted={'core-oj.jar','core-libart.jar','core-icu4j.jar','icudt76l.dat',
         'fonts/fonts.xml','fonts/windows-fonts.xml','FONTS-NOTICES.txt','TZDATA-NOTICES.txt'}|TIMEZONE|fonts
 assert all((root/name).is_file() for name in wanted),'Missing explicit runtime data input'
 assert not any(Path(name).suffix.lower() in ('.exe','.dll','.art','.oat','.vdex') for name in wanted)
 return wanted
def compatibility_proof(current_source):
 def tree(commit):
  result={}
  for row in subprocess.check_output(['git','ls-tree','-r','-z',commit]).split(b'\0'):
   if row:
    metadata,path=row.split(b'\t',1);mode,kind,blob=metadata.decode().split();name=path.decode()
    if name.startswith(('compat/arm64/','compat/elf/include/')) or name in ('.github/workflows/arm64.yml','.github/actions/setup-build/action.yml','cmake/toolchain-llvm-mingw.cmake','compat/bionic/abi_types.h'):
     assert kind=='blob' and mode in ('100644','100755');result[name]=blob
  return result
 expected=tree(QUALIFIED)
 assert expected and tree(OLD)==expected and tree('00f96217616c5892dd8364067d007f92d541585b')==expected and tree(current_source)==expected
 return {'qualified_translator_source':QUALIFIED,'old_runtime_source':OLD,'native_font_baseline':'00f96217616c5892dd8364067d007f92d541585b','interface_and_translator_input_blobs':expected,
  'scope':'exact unchanged translator/build/dependency-version/ABI source, not aggregate source equivalence'}
def assemble(base,translator,fresh,destination,role):
 assert not destination.exists(),'Comparison destination must be fresh'
 base_role='qualified-data' if role=='current-utf8' else 'old-runtime'
 base_receipt=supplier_receipt(base,base_role);translator_receipt=supplier_receipt(translator,'qualified-translator')
 runtime=load_module('fresh_core_runtime','core-runtime-manifest.py');runtime.verify(fresh)
 fresh_record=json.loads((fresh/'core-runtime-provenance.json').read_text(encoding='utf-8'))
 source=fresh_record['context']['source'];compatible=compatibility_proof(source)
 selected=[(name,base/name,base_role,base_receipt['source']) for name in sorted(data_files(base))]
 for name in EXECUTABLES:
  origin=fresh/name if role=='current-utf8' else base/name
  selected.append((name,origin,'fresh-current-exe' if role=='current-utf8' else 'old-runtime-exe',source if role=='current-utf8' else OLD))
 selected.append(('gesher_arm64.dll',translator/'gesher_arm64.dll','qualified-translator',QUALIFIED))
 assert all(path.is_file() for _,path,_,_ in selected)
 assert len({name.casefold() for name,_,_,_ in selected})==len(selected)
 destination.mkdir(parents=True)
 owners={}
 for name,path,owner,origin_source in selected:
  target=destination/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
  owners[name]={'owner':owner,'source':origin_source,'sha256':sha(target)}
 assert {p.name for p in destination.iterdir() if p.suffix.lower() in ('.exe','.dll')}==set(EXECUTABLES)|{'gesher_arm64.dll'}
 assert not any(p.suffix.lower() in ('.art','.oat','.vdex') for p in destination.rglob('*'))
 if role=='current-utf8':assert all(owners[name]['owner']=='fresh-current-exe' and owners[name]['sha256']==fresh_record['outputs'][name] for name in EXECUTABLES)
 record={'role':role,'verifier_context':fresh_record['context'],'owners':owners,'compatibility':compatible,
  'base_supplier':base_receipt,
  'translator_supplier':translator_receipt,
  'fresh_runtime_proof':fresh_record,'full_qualification':False}
 (destination/'font-runtime-assembly.json').write_text(json.dumps(record,sort_keys=True)+'\n',encoding='utf-8')
 for name,owner in owners.items():emit('FONT_RUNTIME_ASSEMBLY_OWNER',{'path':name,**owner})
 for path,blob in compatible['interface_and_translator_input_blobs'].items():emit('FONT_RUNTIME_TRANSLATOR_ABI_INPUT',{'path':path,'blob':blob})
 emit('FONT_RUNTIME_COMPARISON_ASSEMBLY',{'role':role,'context':record['verifier_context'],'manifest_sha256':sha(destination/'font-runtime-assembly.json'),'owned_files':len(owners),'base_source':base_receipt['source'],'translator_source':translator_receipt['source'],'full_qualification':False})
def verify_assembly(root,role):
 record=json.loads((root/'font-runtime-assembly.json').read_text(encoding='utf-8'))
 runtime=load_module('fresh_core_runtime','core-runtime-manifest.py')
 assert record['role']==role and record['verifier_context']==runtime.context() and record['full_qualification'] is False
 assert record['compatibility']==compatibility_proof(record['verifier_context']['source'])
 proof=record['fresh_runtime_proof']
 assert proof['context']==runtime.context() and proof['inputs']==runtime.inputs() and proof['role']=='fresh-current-cli-runtime'
 assert proof['compiler']['sha256']==runtime.COMPILER_SHA and proof['web3_query_tests']=='OFF'
 assert {k:v['sha256'] for k,v in proof['canonical_pthread'].items()}=={name:runtime.PTHREAD_SHA for name in ('libpthread.a','libwinpthread.a')}
 supplier=load_module('fonts_suppliers','baseline-supplier.py')
 base_role='qualified-data' if role=='current-utf8' else 'old-runtime'
 for key,wanted in (('base_supplier',base_role),('translator_supplier','qualified-translator')):
  receipt=record[key];spec=supplier.SUPPLIERS[wanted]
  assert receipt['role']==wanted and receipt['source']==spec.get('source',supplier.SOURCE) and receipt['run']==spec.get('run',supplier.RUN) and receipt['attempt']==1
  assert receipt['artifact']['id']==spec['id'] and receipt['artifact']['digest']==spec['digest'] and receipt['producer']['id']==spec['job']
 expected={}
 base=record['base_supplier'];translator=record['translator_supplier']
 for name in data_files(root):expected[name]={'owner':base_role,'source':base['source'],'sha256':base['members'][name]}
 for name in EXECUTABLES:
  expected[name]={'owner':'fresh-current-exe' if role=='current-utf8' else 'old-runtime-exe','source':proof['context']['source'] if role=='current-utf8' else OLD,
   'sha256':proof['outputs'][name] if role=='current-utf8' else base['members'][name]}
 expected['gesher_arm64.dll']={'owner':'qualified-translator','source':QUALIFIED,'sha256':translator['members']['gesher_arm64.dll']}
 assert record['owners']==expected and inventory(root)=={**{p:r['sha256'] for p,r in expected.items()},'font-runtime-assembly.json':sha(root/'font-runtime-assembly.json')}
 emit('FONT_RUNTIME_ASSEMBLY_VERIFIED',{'role':role,'context':record['verifier_context'],'manifest_sha256':sha(root/'font-runtime-assembly.json'),'owned_files':len(expected),'old_executables_in_current':False if role=='current-utf8' else None,'images':0})
 return record

def main():
 p=argparse.ArgumentParser();p.add_argument('--base',type=Path);p.add_argument('--translator',type=Path);p.add_argument('--fresh',type=Path);p.add_argument('--verify',action='store_true');p.add_argument('--destination',type=Path,required=True);p.add_argument('--role',choices=('current-utf8','old42076'),required=True);a=p.parse_args()
 assert os.environ.get('GITHUB_ACTIONS')=='true','Runtime assembly is CI only'
 if a.verify:verify_assembly(a.destination,a.role)
 else:
  assert all((a.base,a.translator,a.fresh));assemble(a.base,a.translator,a.fresh,a.destination,a.role);verify_assembly(a.destination,a.role)
if __name__=='__main__':main()
