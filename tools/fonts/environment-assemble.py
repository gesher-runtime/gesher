"""Assemble explicit reused diagnostic EXEs, qualified data, and qualified translator."""
import argparse,hashlib,importlib.util,json,os,shutil,subprocess
from pathlib import Path
from font_records import emit

def module(name,file):
 spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(file));value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def context():
 source=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip();assert source==os.environ['GITHUB_SHA']
 assert os.environ['GITHUB_REPOSITORY']=='gesher-runtime/gesher' and os.environ['GITHUB_REPOSITORY_ID']=='1426153186'
 return {'source':source,'run':os.environ['GITHUB_RUN_ID'],'attempt':os.environ['GITHUB_RUN_ATTEMPT'],'repository':os.environ['GITHUB_REPOSITORY'],'repository_id':os.environ['GITHUB_REPOSITORY_ID']}
def verify(root):
 record=json.loads((root/'font-env-assembly.json').read_text(encoding='utf-8'))
 reuse=module('bounded_f78','environment-supplier.py');assembly=module('qualified_data_selection','assemble-core-runtime.py')
 assert record['verifier_context']==context() and record['runtime_role']==reuse.ROLE and record['runtime_source']==reuse.SOURCE and record['runtime_run']==reuse.RUN and record['runtime_attempt']==1 and record['runtime_manifest_sha256']==reuse.MANIFEST_SHA and record['whole_runtime_run_green'] is False and record['fallback_qualified'] is False
 assert record['compatibility']==assembly.compatibility_proof(context()['source'])
 expected_files=assembly.data_files(root)|{'dalvikvm.exe','gesher-exec.exe','gesher_arm64.dll'}
 assert set(record['owners'])==expected_files and assembly.inventory(root)=={**{name:owner['sha256'] for name,owner in record['owners'].items()},'font-env-assembly.json':sha(root/'font-env-assembly.json')}
 for name,digest in reuse.EXE_SHA.items():assert record['owners'][name]=={'role':reuse.ROLE,'source':reuse.SOURCE,'sha256':digest}
 suppliers=assembly.load_module('qualified_environment_suppliers','baseline-supplier.py')
 for key,role in (('data_supplier','qualified-data'),('translator_supplier','qualified-translator')):
  receipt=record[key];spec=suppliers.SUPPLIERS[role]
  assert receipt['role']==role and receipt['source']==spec['source'] and receipt['run']==spec['run'] and receipt['attempt']==1
  assert receipt['repository']==suppliers.REPO and receipt['repository_id']==suppliers.REPO_ID
  assert receipt['artifact']['id']==spec['id'] and receipt['artifact']['digest']==spec['digest'] and not receipt['artifact']['expired']
  assert receipt['producer']['id']==spec['job'] and receipt['producer']['name']==spec['producer'] and receipt['producer']['conclusion']=='success'
 for name in assembly.data_files(root):assert record['owners'][name]=={'role':'QUALIFIED_1BF8_DATA_ONLY','source':record['data_supplier']['source'],'sha256':record['data_supplier']['members'][name]}
 assert record['owners']['gesher_arm64.dll']=={'role':'QUALIFIED_1BF8_TRANSLATOR','source':record['translator_supplier']['source'],'sha256':record['translator_supplier']['members']['gesher_arm64.dll']}
 emit('FONT_ENV_ASSEMBLY_VERIFIED',{'verifier_context':record['verifier_context'],'runtime_role':record['runtime_role'],'runtime_source':record['runtime_source'],'manifest_sha256':sha(root/'font-env-assembly.json'),'files':len(expected_files),'current_source_fresh_runtime':False,'fallback_qualified':False})
 return record

def main():
 p=argparse.ArgumentParser();p.add_argument('--runtime',type=Path);p.add_argument('--data',type=Path);p.add_argument('--translator',type=Path);p.add_argument('--destination',type=Path,required=True);p.add_argument('--verify',action='store_true');a=p.parse_args()
 assert os.environ.get('GITHUB_ACTIONS')=='true','Environment diagnostic archive handling is CI only'
 if a.verify:verify(a.destination);return
 assert all((a.runtime,a.data,a.translator)) and not a.destination.exists()
 reuse=module('bounded_f78','environment-supplier.py');proof=reuse.verify(a.runtime)
 assembly=module('qualified_data_selection','assemble-core-runtime.py');data=assembly.supplier_receipt(a.data,'qualified-data');translator=assembly.supplier_receipt(a.translator,'qualified-translator')
 compatible=assembly.compatibility_proof(context()['source']);owners={};selected=[]
 for name in sorted(assembly.data_files(a.data)):selected.append((name,a.data/name,'QUALIFIED_1BF8_DATA_ONLY',data['source'],data['members'][name]))
 for name in ('dalvikvm.exe','gesher-exec.exe'):selected.append((name,a.runtime/name,reuse.ROLE,reuse.SOURCE,proof['outputs'][name]))
 selected.append(('gesher_arm64.dll',a.translator/'gesher_arm64.dll','QUALIFIED_1BF8_TRANSLATOR',translator['source'],translator['members']['gesher_arm64.dll']))
 a.destination.mkdir(parents=True)
 for name,path,role,source,digest in selected:
  assert path.is_file() and sha(path)==digest and name not in owners
  target=a.destination/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target);assert sha(target)==digest
  owners[name]={'role':role,'source':source,'sha256':digest};emit('FONT_ENV_ASSEMBLY_OWNER',{'path':name,**owners[name]})
 record={'verifier_context':context(),'runtime_role':reuse.ROLE,'runtime_source':reuse.SOURCE,'runtime_run':reuse.RUN,'runtime_attempt':1,'runtime_manifest_sha256':reuse.MANIFEST_SHA,'whole_runtime_run_green':False,'owners':owners,'compatibility':compatible,'data_supplier':data,'translator_supplier':translator,'fallback_qualified':False}
 (a.destination/'font-env-assembly.json').write_text(json.dumps(record,sort_keys=True)+'\n',encoding='utf-8');verify(a.destination)
if __name__=='__main__':main()
