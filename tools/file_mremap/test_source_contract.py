"""Pure synthetic contract mutants. No process/PE/compiler/native execution."""
import copy,unittest
from source_contract import (INPUTS,BASE,SCOPE,REPO,RID,CALLER,POLICY,BRANCH,WORKFLOW,
    canonical,digest,decode,validate_freeze,validate_plan,ContractError)
from producer_receipt import OUTPUT_ROLES,OBJECTS,validate_producer
from windows_controller import verify_fault
H='1'*40;G='2'*40;D='3'*64

def data():
    freeze=dict(schema=1,scope=SCOPE,source=H,tree='4'*40,base=BASE,inventory_sha256=D,
      inputs=[dict(path=p,mode='100644',blob='5'*40,sha256=D,size=10)for p in INPUTS],
      anonymous_suffix_sha256=D,native2_blob='6'*40)
    context=dict(source=H,run='701',attempt='2',repository=REPO,repository_id=RID)
    expected=dict(context=context,policy_commit=G,policy_blob='7'*40,source_manifest_sha256=digest(canonical(freeze)),whole_peer_sha256=D)
    plan=dict(schema=1,scope=SCOPE,context=context,policy_ref=REPO+'/'+POLICY+'@'+G,
      policy_blob=expected['policy_blob'],whole_peer_sha256=D,source_manifest_sha256=expected['source_manifest_sha256'],producer_job='produce',reference_job='linux-reference',consumers=['windows-2022','windows-2025'])
    run=dict(id=701,run_attempt=2,head_sha=H,head_branch=BRANCH,path=CALLER,name=WORKFLOW,event='workflow_dispatch',
      repository=dict(full_name=REPO,id=int(RID)),head_repository=dict(full_name=REPO,id=int(RID)),
      actor=dict(login='palmoni5'),triggering_actor=dict(login='palmoni5'),referenced_workflows=[dict(path=plan['policy_ref'],sha=G)])
    jobs=dict(total_count=1,jobs=[dict(id=801,name='produce',run_id=701,run_attempt=2,head_sha=H,status='completed',conclusion='success',started_at='2026-10-31T01:00:00Z',completed_at='2026-10-31T02:00:00Z')])
    jobs['jobs'].insert(0,dict(jobs['jobs'][0],id=800,name='source_authority / verify'))
    jobs['total_count']=2
    artifact=dict(id=901,name='file-mremap-produced-'+H+'-701-2',digest='sha256:'+D,expired=False,size_in_bytes=100,workflow_run=dict(id=701,head_sha=H,repository_id=int(RID),head_repository_id=int(RID)))
    compiler=dict(driver_sha256=D,recipe_fingerprint=D,receipt_sha256=D,supplier_role='production-current-selected',stl_abi='synthetic-STL',allocator_abi='synthetic-allocator')
    stamps={}
    for role,(source,exceptions,backend)in OBJECTS.items():
        argv=['synthetic-driver','-c',source,'-o','synthetic.obj','-fexceptions'if exceptions else '-fno-exceptions']
        if backend:argv+=['-DGESHER_FILE_MREMAP_FAILURE_TEST=1']
        stamps[role]=dict(source=source,source_sha256=D,object_sha256=D,driver_sha256=D,arguments=argv,stl_abi=compiler['stl_abi'],allocator_abi=compiler['allocator_abi'])
    allocation=[]
    for family in ('alias','surgery'):
        for budget in (0,1):allocation.append(dict(family=family,budget=budget,allocation_calls=1,injected=1-budget,status='NO_MEMORY'if budget==0 else 'OK',outstanding=0,unchanged_owner_refs=True))
    produced=dict(schema=1,scope=SCOPE,context=context,role='file-mremap-producer',source_manifest_sha256=expected['source_manifest_sha256'],producer_id=801,artifact_id=901,artifact_digest=artifact['digest'],compiler=compiler,
      outputs=[dict(role=role,member=role+'.bin',sha256=D,size=10,source=H,producer_id=801)for role in OUTPUT_ROLES],object_stamps=stamps,
      helper_qualification=dict(exit=0,stdout_sha256=D,stderr_sha256=D,source_role='nativehost-prepare',allocation_rows=allocation,cleanup_completed=True))
    return dict(freeze=freeze,expected=expected,plan=plan,context=context,run=run,jobs=jobs,artifact=artifact,produced=produced)
def union(d):return validate_producer(d['produced'],d['freeze'],d['context'],G,d['run'],d['jobs'],d['artifact'])
def page(i,**change):
    row=dict(event='ledger',phase='after_fault',page=i,present=1,region_page=i,region_size=20480,
      kind=1,live=1,live_pages=5,owner_refs=3,owner_state=1,owner_failure=0,failed_owners=0,
      failed_owner_state=-1,failed_owner_error=0,failed_owner_refs=0,rollback_view=0,
      failed_handle_valid=0,backing_refs=0,rollback_query_ok=0,rollback_state=0,rollback_protection=0,rollback_type=0,
      ledger_protection=0,operation_error=0,query_ok=1,state=0x1000,protection=4,type=0x40000,group=1)
    row.update(change);return row
def fault_data():
    counts=[0]*20;counts[0]=counts[2]=1
    return dict(event_rows=[dict(event='fault_result',case='fault_alias',operation=-1,errno=12),
      *[page(i)for i in range(5)],*[page(i,phase='after_cleanup',present=0,live=0,live_pages=0,owner_refs=0,state=0x10000,protection=0,type=0)for i in range(5)],dict(event='injection',reached=counts,outstanding_before_controller_cleanup=0)])
def prefix_data():
    rows=[]
    for phase in ('after_fault','after_first_prefix_cleanup'):
        for i in range(5):
            r=page(i,phase=phase,owner_failure=1,owner_refs=4)
            if i<2:r.update(region_size=8192,live_pages=2)
            elif i==2:r.update(kind=2,region_page=0,region_size=4096,live=0,live_pages=0,owner_refs=0,owner_state=-1,state=0x2000,protection=0,type=0)
            else:r.update(region_page=i-3,region_size=8192,live_pages=2)
            if phase=='after_first_prefix_cleanup'and i<2:
                r['live_pages']=1
                if i==0:r.update(live=0,protection=1)
            rows.append(r)
    rows += [page(i,phase='after_cleanup',owner_failure=1,present=0,live=0,live_pages=0,owner_refs=0,state=0x10000,protection=0,type=0)for i in range(5)]
    counts=[0]*20;counts[5]=4;counts[12]=1
    return dict(event_rows=[dict(event='fault_result',case='fault_middle',operation=-1,errno=5),*rows,
      dict(event='prefix_cleanup',second_readable=1),dict(event='containment',classification='FAILED_CONTAINMENT'),
      *[dict(event='failedstate_guard',route=i,rejected=1,errno=5)for i in range(11)],
      dict(event='failedstate_unchanged',ledger_and_backend_unchanged=1,resources_before=10,resources_after=10),
      dict(event='injection',reached=counts,outstanding_before_controller_cleanup=0)])

class SourceContracts(unittest.TestCase):
    def test_structural_baselines_are_synthetic_only(self):
        d=data();validate_freeze(d['freeze']);validate_plan(d['plan'],d['expected'],d['freeze']);union(d)
    def test_duplicate_json_key(self):
        with self.assertRaises(ContractError):decode(b'{"source":1,"source":2}')
    def test_missing_or_extra_inputs(self):
        for field in ('missing','duplicate','order','linked'):
            with self.subTest(field=field):
                d=data();rows=d['freeze']['inputs']
                if field=='missing':rows.pop()
                elif field=='duplicate':rows.append(rows[0])
                elif field=='order':rows.reverse()
                else:rows[0]['mode']='120000'
                with self.assertRaises(ContractError):validate_freeze(d['freeze'])
    def test_plan_authority_mutants(self):
        changes=[('scope','FULL'),('policy_ref',REPO+'/'+POLICY+'@'+H),('policy_blob','8'*40),('whole_peer_sha256','8'*64),('source_manifest_sha256','8'*64),('producer_job','build'),('reference_job','produce'),('consumers',['windows-2022'])]
        for field,value in changes:
            with self.subTest(field=field):
                d=data();d['plan'][field]=value
                with self.assertRaises(ContractError):validate_plan(d['plan'],d['expected'],d['freeze'])
        d=data();d['plan']['context']=dict(d['context'],attempt='1')
        with self.assertRaises(ContractError):validate_plan(d['plan'],d['expected'],d['freeze'])
    def test_current_role_attempt_artifact_mutants(self):
        mutations=[lambda d:d['run'].update(run_attempt=1),lambda d:d['run'].update(head_sha=G),
          lambda d:d['run']['head_repository'].update(id=1),lambda d:d['run']['triggering_actor'].update(login='other'),
          lambda d:d['run'].update(referenced_workflows=[]),lambda d:d['jobs'].update(total_count=3),
          lambda d:d['jobs']['jobs'].append(copy.deepcopy(d['jobs']['jobs'][0])),
          lambda d:d['jobs']['jobs'][1].update(name='build'),lambda d:d['jobs']['jobs'][0].update(run_attempt=1),
          lambda d:d['jobs']['jobs'][0].update(conclusion='failure'),lambda d:d['artifact'].update(expired=True),
          lambda d:d['artifact']['workflow_run'].update(head_sha=G),lambda d:d['produced'].update(producer_id=802),
          lambda d:d['produced']['outputs'].pop(),lambda d:d['produced']['outputs'][0].update(member='../escape'),
          lambda d:d['produced']['outputs'][0].update(source=G),lambda d:d['produced']['compiler'].update(supplier_role='FIRST_LINK')]
        for number,change in enumerate(mutations):
            with self.subTest(number=number):
                d=data();change(d)
                with self.assertRaises((ContractError,KeyError)):union(d)
    def test_actual_compile_mode_not_flag_presence(self):
        for role in OBJECTS:
            d=data();d['produced']['object_stamps'][role]['arguments'].append('-fno-exceptions'if OBJECTS[role][1]else '-fexceptions')
            with self.subTest(role=role),self.assertRaises(ContractError):union(d)
        for role in ('mman-production-object','helper-production-object'):
            d=data();d['produced']['object_stamps'][role]['arguments'].append('-DGESHER_FILE_MREMAP_FAILURE_TEST=1')
            with self.subTest(role=role),self.assertRaises(ContractError):union(d)
    def test_actual_allocator_injection_required(self):
        for key,value in [('injected',0),('allocation_calls',0),('outstanding',1),('unchanged_owner_refs',False)]:
            d=data();d['produced']['helper_qualification']['allocation_rows'][0][key]=value
            with self.subTest(key=key),self.assertRaises(ContractError):union(d)
    def test_fault_oracle_is_data_only(self):
        verify_fault(fault_data(),'fault_alias',(1,1))
        self.assertTrue(verify_fault(prefix_data(),'fault_middle',(5,2))['owner_failure'])
    def test_fault_coverage_ledger_and_containment_mutants(self):
        changes=[lambda d:d['event_rows'][-1]['reached'].__setitem__(12,0),
          lambda d:d['event_rows'][2].update(live_pages=1),
          lambda d:d['event_rows'][2].update(protection=1),
          lambda d:d['event_rows'].pop(3),
          lambda d:next(r for r in d['event_rows']if r['event']=='prefix_cleanup').update(second_readable=0),
          lambda d:next(r for r in d['event_rows']if r['event']=='failedstate_guard').update(rejected=0),
          lambda d:next(r for r in d['event_rows']if r['event']=='failedstate_unchanged').update(ledger_and_backend_unchanged=0)]
        for number,change in enumerate(changes):
            with self.subTest(number=number):
                d=prefix_data();change(d)
                with self.assertRaises(AssertionError):verify_fault(d,'fault_middle',(5,2))
if __name__=='__main__':unittest.main()
