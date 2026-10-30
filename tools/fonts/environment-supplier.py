"""Narrow diagnostic reuse exception, never generic full/current-source qualification."""
import argparse,datetime,hashlib,json,os,subprocess
from pathlib import Path
from font_records import emit
REPO='gesher-runtime/gesher';REPO_ID=1426153186
ROLE='BOUNDED_QUALIFIED_F78_DIAGNOSTIC';SOURCE='f78bcbbb10050267374fa5b94318174f384a0b7e';RUN=41108241255
ARTIFACT=13005515313;DIGEST='sha256:b86178bd50a3d6dae7b4c5221c3544995e7ceea0486fe59b0f6c497333bac312'
MANIFEST_SHA='ce1d3bfe8b1469b229664b32576ddee5d1dffeae178933c8286e535379fa6d54'
EXE_SHA={'dalvikvm.exe':'9de0fb10c5dbd7f5caacd2f338aed8f426653f9fe761cc74aec2900f9dd86595','gesher-exec.exe':'9e5c0d4f58842f276670e0e5e16cc2053ef970f383e9768f47ee319279f0485a'}
JOBS={124243600680:('trusted-source / verify','success'),124243711875:('runtime-producer','success'),124243712498:('fixture-producer / native2','success'),124261932573:('compare (windows-2022, current-utf8)','success'),124261932679:('compare (windows-2025, current-utf8)','success'),124261932591:('compare (windows-2025, old42076)','success'),124261932732:('compare (windows-2022, old42076)','failure')}
ROUTES=('compat/bionic/shim.cc','compat/bionic/process_bionic.cc','compat/elf/exec/gesher_exec.cc','compat/posix/src/path.cc')
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def require(value,message):
 if not value:raise RuntimeError(message)
def validate(run,jobs,artifacts):
 emit('FONT_ENV_REUSE_EXPECTED',{'role':ROLE,'source':SOURCE,'run':RUN,'attempt':1,'workflow_name':'fonts-core-component','workflow_path':'.github/workflows/fonts-core-component.yml','whole_conclusion':'failure','runtime_producer_success':124243711875,'current_consumers_success':[124261932573,124261932679],'only_failure':124261932732,'artifact':ARTIFACT,'digest':DIGEST,'dedicated_diagnostic_exception':True,'generic_full_success_exception':False})
 emit('FONT_ENV_REUSE_RECEIVED_RUN',{key:{'present':key in run,'value':run.get(key)} for key in ('id','run_attempt','head_sha','name','path','status','conclusion','head_branch','repository','head_repository')})
 require(run['id']==RUN and run['run_attempt']==1 and run['head_sha']==SOURCE and run['name']=='fonts-core-component' and run['path']=='.github/workflows/fonts-core-component.yml' and run['head_branch']=='wip/fonts-cli-utf8' and run['status']=='completed' and run['conclusion']=='failure','Wrong diagnostic source/run/attempt/workflow/conclusion')
 require(run['repository']['full_name']==run['head_repository']['full_name']==REPO and run['repository']['id']==REPO_ID and run['head_repository']['id']==REPO_ID,'Wrong diagnostic repositories')
 emit('FONT_ENV_REUSE_JOB_INVENTORY',{'expected_ids':sorted(JOBS),'total_count':jobs.get('total_count'),'received_count':len(jobs.get('jobs',[])),'received':[{'id':x.get('id'),'name':x.get('name'),'workflow_name':x.get('workflow_name'),'head_sha':x.get('head_sha'),'run_id':x.get('run_id'),'run_attempt':x.get('run_attempt'),'status':x.get('status'),'conclusion':x.get('conclusion')} for x in jobs.get('jobs',[])]})
 require(jobs['total_count']==len(jobs['jobs'])==len(JOBS) and {x['id'] for x in jobs['jobs']}==set(JOBS),'Incomplete/wrong diagnostic producer/consumer inventory')
 for row in jobs['jobs']:
  wanted=JOBS[row['id']];emit('FONT_ENV_REUSE_RECEIVED_JOB',{key:{'present':key in row,'value':row.get(key)} for key in ('id','name','workflow_name','head_sha','run_id','run_attempt','status','conclusion')})
  require((row['name'],row['conclusion'])==wanted and row['workflow_name']=='fonts-core-component' and row['head_sha']==SOURCE and row['run_id']==RUN and row['run_attempt']==1 and row['status']=='completed','Diagnostic producer/current consumer/OLD failure differs')
 require(artifacts['total_count']==len(artifacts['artifacts']) and artifacts['total_count']<=100,'Incomplete artifact inventory')
 rows=[x for x in artifacts['artifacts'] if x['name']=='fonts-core-runtime'];emit('FONT_ENV_REUSE_ARTIFACT_INVENTORY',{'count':artifacts['total_count'],'names':[x.get('name') for x in artifacts['artifacts']],'selected_count':len(rows),'selected':rows});require(len(rows)==1,'Ambiguous diagnostic runtime role');item=rows[0]
 emit('FONT_ENV_REUSE_RECEIVED_ARTIFACT',item)
 require(item['id']==ARTIFACT and item['digest']==DIGEST and not item['expired'] and item['size_in_bytes']>0,'Wrong diagnostic artifact/digest')
 identity=item['workflow_run'];require(identity['id']==RUN and identity['head_sha']==SOURCE and identity['repository_id']==identity['head_repository_id']==REPO_ID,'Wrong artifact source/repository')
 producer=next(x for x in jobs['jobs'] if x['id']==124243711875);time=lambda value:datetime.datetime.fromisoformat(value.replace('Z','+00:00'))
 require(time(producer['started_at'])<=time(item['created_at'])<=time(producer['completed_at']),'Artifact outside exact successful runtime attempt')
 return producer,item

def verify(root):
 from importlib.util import spec_from_file_location,module_from_spec
 spec=spec_from_file_location('frozen_runtime_definition',Path(__file__).with_name('core-runtime-manifest.py'));runtime=module_from_spec(spec);spec.loader.exec_module(runtime)
 manifest=root/'core-runtime-provenance.json';require(sha(manifest)==MANIFEST_SHA,'Diagnostic producer manifest bytes differ')
 record=json.loads(manifest.read_text(encoding='utf-8'))
 require(record['context']=={'source':SOURCE,'run':str(RUN),'attempt':'1','repository':REPO,'repository_id':str(REPO_ID)} and record['role']=='fresh-current-cli-runtime','Wrong original producer manifest context')
 require({p.name for p in root.iterdir()}==set(runtime.FILES)|{'core-runtime-provenance.json'} and all(p.is_file() and not p.is_symlink() for p in root.iterdir()),'Diagnostic runtime inventory changed')
 require(record['outputs']=={name:sha(root/name) for name in runtime.FILES} and all(record['outputs'][name]==digest for name,digest in EXE_SHA.items()),'Diagnostic runtime output bytes differ')
 for path,blob in record['inputs'].items():require(subprocess.check_output(['git','rev-parse',SOURCE+':'+path],text=True).strip()==blob,'Frozen runtime input differs')
 for path in ROUTES:require(subprocess.check_output(['git','rev-parse',SOURCE+':'+path],text=True).strip()==subprocess.check_output(['git','hash-object',path],text=True).strip(),'Environment getter/array route changed')
 emit('FONT_ENV_REUSE_VERIFIED',{'role':ROLE,'producer_source':SOURCE,'producer_run':RUN,'producer_attempt':1,'verifier_source':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'manifest_sha256':MANIFEST_SHA,'outputs':record['outputs'],'full_run_green':False,'current_source_fresh_runtime':False,'fallback_qualified':False})
 return record

def main():
 p=argparse.ArgumentParser();p.add_argument('--destination',type=Path,required=True);p.add_argument('--verify',action='store_true');a=p.parse_args()
 require(os.environ.get('GITHUB_ACTIONS')=='true' and os.environ['GITHUB_REPOSITORY']==REPO,'Diagnostic runtime archive is CI only')
 if a.verify:verify(a.destination);return
 require(not a.destination.exists(),'Diagnostic destination must be fresh')
 def api(path):return json.loads(subprocess.check_output(['gh','api','repos/'+REPO+'/'+path]))
 producer,item=validate(api('actions/runs/'+str(RUN)),api('actions/runs/'+str(RUN)+'/attempts/1/jobs?per_page=100'),api('actions/runs/'+str(RUN)+'/artifacts?per_page=100'))
 subprocess.run(['python','tools/surface/download-verified-artifact.py','--id',str(ARTIFACT),'--digest',DIGEST,'--destination',str(a.destination)],check=True)
 verify(a.destination)
 receipt={'role':ROLE,'source':SOURCE,'run':RUN,'attempt':1,'producer':producer,'artifact':item,'manifest_sha256':MANIFEST_SHA,'whole_run_green':False,'current_source_fresh':False}
 target=a.destination.parent/(a.destination.name+'.diagnostic-receipt.json');require(not target.exists(),'Diagnostic receipt exists');target.write_text(json.dumps(receipt,sort_keys=True)+'\n',encoding='utf-8')
 emit('FONT_ENV_REUSE_RECEIPT',receipt)
if __name__=='__main__':main()
