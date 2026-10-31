"""CI-only owned actor controller. No native operation on import.

Source admission requires a future immutable authority's plan hash and typed
current receipts; this module does not manufacture/authenticate GitHub receipts.
"""
import argparse,datetime,hashlib,json,os,platform,queue,re,subprocess,tempfile,threading,time,uuid
from pathlib import Path
from source_contract import decode
CASES=('source_first','alias_first','partial','middle')
# Exact, source-owned adverse cells. Any unhit injection is a FAILED row.
# Destructive/final-release invariant failures remain FAILED_CONTAINMENT and
# prevent the suite PASS; diagnostic coverage never converts them to success.
FAULTS=(('fault_alias',1,1),('fault_alias',4,1),('fault_alias',4,5),
        ('fault_alias',9,1),('fault_middle',4,3),('fault_middle',5,2),
        ('fault_middle',6,1),('fault_middle',6,2),('fault_middle',7,1),
        ('fault_middle',8,1),('fault_middle',8,2),
        ('fault_prefix',5,1),('fault_suffix',5,1),('fault_partial',5,2),
        ('fault_sourcefirst',7,1),('fault_aliasfirst',10,1),
        ('fault_alias',11,1),('fault_finalclose',3,1))
PROTECTION_CASES=('protect_dead','protect_mixed','protect_partial','protect_surgery')
RETAINED=[]
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def atomic(path,value):
    temp=path.with_suffix(path.suffix+'.tmp');temp.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8');temp.replace(path)
class Actor:
    def __init__(self,argv,cwd,environment):
        self.record=dict(argv=argv,cwd=str(cwd),started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),started_monotonic_ns=time.monotonic_ns(),pid=None,exit=None,stdout='',stderr='',overflow=False,event_rows=[])
        self.events=queue.Queue();self.parts={'stdout':bytearray(),'stderr':bytearray()};self.threads=[];self.proc=None
        self.argv=argv;self.cwd=cwd;self.environment=environment
    def start(self):
        self.proc=subprocess.Popen(self.argv,cwd=self.cwd,env=self.environment,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,shell=False)
        self.record['pid']=self.proc.pid
        for name in ('stdout','stderr'):
            thread=threading.Thread(target=self.reader,args=(name,),daemon=True);self.threads.append(thread);thread.start()
    def reader(self,name):
        pending=b''
        try:
            while True:
                block=os.read(getattr(self.proc,name).fileno(),4096)
                if not block:break
                if len(self.parts[name])+len(block)>262144:
                    self.record['overflow']=True;break
                self.parts[name].extend(block)
                if name=='stdout':
                    pending+=block
                    while b'\n'in pending:
                        line,pending=pending.split(b'\n',1)
                        if len(line)>16384:self.record['overflow']=True;return
                        try:
                            row=decode(line)
                            if not isinstance(row,dict) or row.get('event') not in {'ready','peer_complete','query','ledger','failedstate_guard','failedstate_unchanged','fault_result','prefix_cleanup','containment','injection','complete','prerequisite','middle_result','protection_result'}:raise ValueError('unknown/nonobject actor event')
                            self.record['event_rows'].append(row);self.events.put_nowait(row)
                        except (ValueError,queue.Full):self.record['bad_json']=True
                    if len(pending)>16384:self.record['overflow']=True;return
            if name=='stdout' and pending:self.record['bad_json']=True
        except (OSError,ValueError)as error:self.record[name+'_io_error']=repr(error)
    def event(self,name,deadline):
        while time.monotonic()<deadline:
            try:row=self.events.get(timeout=min(.1,max(.001,deadline-time.monotonic())))
            except queue.Empty:
                if self.record['overflow']:raise AssertionError('output bound exceeded')
                if self.proc.poll()is not None and all(not t.is_alive()for t in self.threads):break
                continue
            if row.get('event')==name:return row
        raise TimeoutError('missing '+name)
    def close(self,deadline):
        error=None
        if self.proc is None:
            self.record.update(kernel_completed=True,io_completed=True,cleanup_error=None,not_started=True)
            return True
        try:
            if self.proc.stdin and not self.proc.stdin.closed:
                try:self.proc.stdin.close()
                except OSError as failure:error=repr(failure)
            try:self.proc.wait(timeout=max(.001,deadline-time.monotonic()))
            except subprocess.TimeoutExpired:
                self.record['killed_after_timeout']=True;self.proc.kill();self.proc.wait(timeout=5)
        except (OSError,subprocess.TimeoutExpired)as failure:error=repr(failure)
        kernel=self.proc.poll()is not None
        for thread in self.threads:
            if thread.ident is not None:thread.join(timeout=1)
        io=all(not t.is_alive()for t in self.threads)
        if io:
            for pipe in (self.proc.stdout,self.proc.stderr):
                try:pipe.close()
                except OSError as failure:error=repr(failure)
        # An outstanding reader retains its stream/buffer/process graph; never
        # block in stream.close or report cleanup success with borrowed IO live.
        io=io and not self.record.get('bad_json',False)and not any(k.endswith('_io_error')for k in self.record)
        self.record.update(exit=self.proc.returncode,stdout=self.parts['stdout'].decode('utf-8',errors='replace'),stderr=self.parts['stderr'].decode('utf-8',errors='replace'),kernel_completed=kernel,io_completed=io,cleanup_error=error)
        if not kernel or not io:RETAINED.append(self) # FAILED containment, never cleanup PASS
        return kernel and io and error is None

def expected_bytes(size,peer):
    data=bytearray((i*13+7)&255 for i in range(size));data[37]=0x5a
    if peer:data[43]=0x6b
    return bytes(data)
def run_lifetime(prefix,case,environment=None,fault=None):
    """Actual parent/peer handshake on Linux or Windows, one owned directory."""
    result=dict(case=case,verdict='FAILED',actors=[],cleanup=False)
    temporary=None;actors=[];deadline=time.monotonic()+45
    try:
        assert case in CASES+PROTECTION_CASES if fault is None else (case,*fault) in FAULTS
        temporary=tempfile.TemporaryDirectory(prefix='file-mremap-case-');cwd=Path(temporary.name);nonce=uuid.uuid4().hex
        environment=dict(environment or {k:v for k,v in os.environ.items()if k in ('SystemRoot','WINDIR','PATH','TEMP','TMP','GITHUB_ACTIONS')})
        environment['TMPDIR']=str(cwd).replace('\\','/')
        parent=Actor(prefix+[case,'backing.dat',nonce]+([]if fault is None else [str(v)for v in fault]),cwd,environment);actors.append(parent);parent.start()
        ready=parent.event('ready',deadline);assert ready['nonce']==nonce and ready['pid']==parent.proc.pid
        size=ready['file_size'];assert type(size)is int and size==ready['page']*5 and 20480<=size<=5242880
        backing=cwd/'backing.dat';before=backing.read_bytes();assert before==expected_bytes(size,False)
        result.update(page=ready['page'],file_size_before=len(before),file_sha256_before=hashlib.sha256(before).hexdigest())
        peer=Actor(prefix+['peer','backing.dat',nonce],cwd,environment);actors.append(peer);peer.start()
        peer_done=peer.event('peer_complete',deadline);assert peer_done['nonce']==nonce and peer_done['pid']==peer.proc.pid
        assert peer.close(deadline)and peer.record['exit']==0
        assert sum(r['event']=='peer_complete'for r in peer.record['event_rows'])==1
        assert peer_done['failed']==peer_done['cleanup_failures']==0 and peer_done['resources_before']==peer_done['resources_after']>=0
        result['peer']=peer_done
        parent.proc.stdin.write(b'PEER_DONE\n');parent.proc.stdin.flush();parent.proc.stdin.close()
        done=parent.event('complete',deadline);assert done['nonce']==nonce and done['case']==case
        assert parent.close(deadline)
        assert all(sum(r['event']==name for r in parent.record['event_rows'])==1 for name in ('ready','complete'))
        if fault is None:assert parent.record['exit']==0
        after=backing.read_bytes();assert after==expected_bytes(size,True)
        result.update(file_size_after=len(after),file_sha256_after=hashlib.sha256(after).hexdigest(),parent=done)
        assert done['file_size']==size
        if fault is None:
            assert done['failed']==done['cleanup_failures']==0
            assert done['resources_before']==done['resources_after']>=0
            if case in PROTECTION_CASES:result['protection_observation']=verify_protection(parent.record,case)
            result['verdict']='PASS'
        else:
            result['fault']=dict(mode=fault[0],nth=fault[1])
            result['observation']=verify_fault(parent.record,case,fault)
            # Observed failed containment is a real operation failure, not a
            # passing negative. Preserve every actual receipt and resource delta.
            if result['observation']['owner_failure']:
                assert parent.record['exit']!=0 and done['failed']!=0
                result['verdict']='FAILED_CONTAINMENT'
            else:
                assert parent.record['exit']==0 and done['failed']==done['cleanup_failures']==0
                assert done['resources_before']==done['resources_after']>=0
                result['verdict']='PASS_NONDESTRUCTIVE_REJECTION'
    except Exception as error:result['error']=repr(error)
    finally:
        complete=True
        for actor in actors:
            if 'kernel_completed'not in actor.record:complete=actor.close(min(deadline,time.monotonic()+5))and complete
            complete=complete and actor.record.get('kernel_completed',False)and actor.record.get('io_completed',False)and not actor.record.get('overflow',True)and not actor.record.get('killed_after_timeout',False)
            result['actors'].append(actor.record)
        if temporary:
            if complete:
                try:temporary.cleanup()
                except OSError as error:complete=False;result['directory_cleanup_error']=repr(error)
            else:
                RETAINED.append(temporary);result['retained_directory']=temporary.name
        result['cleanup']=complete
        if not complete:result['verdict']='FAILED'
    return result

def verify_fault(actor,case,fault):
    """Pure oracle of ACTUAL target-macro rows; synthetic controls never run OS."""
    rows=actor['event_rows'];mode,nth=fault
    assert (case,mode,nth)in FAULTS
    def one(event):
        selected=[r for r in rows if r.get('event')==event]
        assert len(selected)==1,('event cardinality',event)
        return selected[0]
    injection=one('injection');metrics=injection['reached']
    assert len(metrics)==20 and all(type(v)is int and v>=0 for v in metrics)
    reached={1:0,3:1,4:4,5:5,6:6,7:7,8:8,9:9,10:18,11:9}[mode]
    injected={1:2,3:3,4:11,5:12,6:13,7:14,8:15,9:16,10:19,11:16}[mode]
    assert metrics[reached]>=nth and metrics[injected]>=1
    assert metrics[injected]==1
    if mode==3:assert metrics[1]==metrics[3]==injection['outstanding_before_controller_cleanup']==1
    if mode==11:assert metrics[18]>=1 and metrics[19]==1
    result=one('fault_result');assert result['case']==case and result['operation']==-1
    ledger=[r for r in rows if r.get('event')=='ledger']
    phases={'after_fault','after_cleanup'}|({'after_first_prefix_cleanup'}if(case,mode,nth)==('fault_middle',5,2)else set())
    assert {r['phase']for r in ledger}==phases
    for phase in phases:
        pages=[r for r in ledger if r['phase']==phase]
        assert [r['page']for r in pages]==list(range(5))
        for r in pages:
            assert r['query_ok']==1
            if r['present']and r['kind']==1:
                assert r['owner_refs']>0 and r['owner_state']==1 and r['owner_refs']==r['owner_views']+r['owner_backing_pins']
                actual=[v for v in pages if v['present']and v['kind']==1 and v['page']-v['region_page']==r['page']-r['region_page']]
                assert sum(v['live']for v in actual)==r['live_pages']
                if r['live']:assert r['state']==0x1000 and r['type']==0x40000 and r['protection']!=1
                elif r['state']==0x1000:assert r['protection']==1
                if r['ledger_protection']:assert r['ledger_protection']==r['protection']
            if r['present']and r['kind']==2:
                assert r['state']==0x2000 and r['live']==r['live_pages']==r['owner_refs']==0
    failed=any(r['owner_failure']for r in ledger)
    if failed:
        assert one('containment')['classification']=='FAILED_CONTAINMENT'
        if mode in (3,11):
            assert all(r['failed_owners']==1 and r['failed_owner_state']==(3 if mode==3 else 4) and r['failed_owner_error']>0 for r in ledger)
            assert all(r['failed_handle_valid']==1 for r in ledger)
            if mode==3:assert all(r['failed_owner_refs']==0 for r in ledger)
            if mode==11:assert all(r['rollback_view']==r['rollback_query_ok']==1 and r['rollback_state']==0x1000 and r['rollback_type']==0x40000 and r['backing_refs']>0 for r in ledger)
        guards=[r for r in rows if r.get('event')=='failedstate_guard']
        assert [r['route']for r in guards]==list(range(11))
        assert all(r['rejected']==1 and r['errno']==5 for r in guards)
        unchanged=one('failedstate_unchanged')
        assert unchanged['ledger_and_backend_unchanged']==1 and unchanged['resources_before']==unchanged['resources_after']>=0
    else:assert not any(r.get('event')=='containment'for r in rows)
    if(case,mode,nth)==('fault_middle',5,2):assert one('prefix_cleanup')['second_readable']==1
    return dict(owner_failure=failed,injection_reached=metrics[reached],injected=metrics[injected],ledger_rows=len(ledger))

def verify_protection(actor,case):
    """Independent data oracle: never creates an actor or native resource."""
    assert case in PROTECTION_CASES
    rows=actor['event_rows'];ledger=[r for r in rows if r.get('event')=='ledger']
    expected_phases={'protect_retired','after_cleanup'}
    if case in ('protect_dead','protect_mixed'):expected_phases|={'protect_before','protect_after'}
    elif case=='protect_partial':expected_phases|={'protect_partial'}
    else:expected_phases|={'protect_changed','protect_surgery','protect_surgery_updated','protect_post_surgery_dead'}
    assert {r['phase']for r in ledger}==expected_phases
    byphase={phase:[r for r in ledger if r['phase']==phase]for phase in expected_phases}
    for phase,pages in byphase.items():
        assert [r['page']for r in pages]==list(range(5))
        assert all(r['query_ok']==1 and r['owner_failure']==r['failed_owners']==0 for r in pages)
        if phase in ('protect_retired','after_cleanup'):
            assert all(r['present']==0 and r['state']==0x10000 for r in pages)
            continue
        for r in pages:
            assert r['present']==1 and r['state']==0x1000
            if r['kind']==1:
                assert r['type']==0x40000 and r['owner_state']==1
                assert r['owner_refs']==r['owner_views']+r['owner_backing_pins']>0
                assert r['ledger_protection']==r['protection']
                siblings=[v for v in pages if v['kind']==1 and v['page']-v['region_page']==r['page']-r['region_page']]
                assert sum(v['live']for v in siblings)==r['live_pages']
            else:assert case=='protect_surgery'and r['page']==2 and r['kind']==0 and r['type']==0x20000 and r['live']==1
            want=4;live=1
            if case in ('protect_dead','protect_mixed')and r['page']==1:want=1;live=0
            if case=='protect_partial'and r['page']==0:want=2
            if case=='protect_surgery':
                want=2 if r['page']<3 else 4
                if phase!='protect_changed'and r['page']==2:want=4
                if phase in ('protect_surgery_updated','protect_post_surgery_dead')and r['page']in (2,3):want=2
                if phase=='protect_post_surgery_dead'and r['page']==0:want=1;live=0
            assert (r['protection'],r['live'])==(want,live)
    operations=[r for r in rows if r.get('event')=='protection_result']
    wanted={'protect_dead':[('unaligned',-1,22,0,0),('length_overflow',-1,22,0,0),('dead',-1,12,0,0)],'protect_mixed':[('mixed',-1,12,0,0)],
            'protect_partial':[('partial_failure',-1,13,2,1)],
            'protect_surgery':[('whole_read',0,None,5,0),('suffix_write',0,None,2,0),('post_surgery_live',0,None,2,0),('post_surgery_mixed',-1,12,0,0)]}[case]
    assert len(operations)==len(wanted)
    for row,(step,status,error,calls,injected)in zip(operations,wanted):
        assert row['step']==step and row['result']==status
        if error is not None:assert row['errno']==error
        assert row['protect_after']-row['protect_before']==calls
        assert row['injected_after']-row['injected_before']==injected
    return dict(case=case,ledger_rows=len(ledger),operations=operations,owner_failure=False)

def receipt(value,expected,role):
    assert value['role']==role and value['conclusion']=='success'
    assert value['context']==expected
    assert type(value['producer_id'])is int and value['producer_id']>0
    assert re.fullmatch('sha256:[0-9a-f]{64}',value['artifact_digest'])
    assert value['producer_name']and value['artifact_id']>0

def bound_file(value,context,role):
    path=Path(value['path']);assert path.is_absolute()and path.is_file()and not path.is_symlink()
    assert re.fullmatch('[0-9a-f]{64}',value['sha256'])and sha(path)==value['sha256']
    receipt(value['receipt'],context,role)
    return str(path)
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--plan',type=Path,required=True);parser.add_argument('--reference',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    assert not args.output.exists()
    record=dict(schema=1,role='file-mremap-windows-lifetime',verdict='PREREQUISITE_UNAVAILABLE',results=[])
    try:
        if __debug__ is False:raise RuntimeError('optimized Python is not an admitted controller')
        assert os.environ.get('GITHUB_ACTIONS')=='true'and platform.system()=='Windows'
        assert os.environ.get('GITHUB_ACTOR')==os.environ.get('GITHUB_TRIGGERING_ACTOR')=='palmoni5'
        assert os.environ.get('GITHUB_WORKFLOW_SHA')==os.environ['GITHUB_SHA']
        plan=json.loads(args.plan.read_bytes());assert sha(args.plan)==os.environ['GESHER_FILE_MREMAP_PLAN_SHA256']
        context=dict(source=os.environ['GITHUB_SHA'],run=os.environ['GITHUB_RUN_ID'],attempt=os.environ['GITHUB_RUN_ATTEMPT'],repository=os.environ['GITHUB_REPOSITORY'],repository_id=os.environ['GITHUB_REPOSITORY_ID'])
        assert context['source']==os.environ['GESHER_FILE_MREMAP_AUTHORIZED_SOURCE']and context==plan['context']
        assert plan['policy_ref']==os.environ['GESHER_FILE_MREMAP_POLICY_REF']and re.fullmatch('gesher-runtime/gesher/.github/workflows/file-mremap-trusted-source.yml@[0-9a-f]{40}',plan['policy_ref'])
        assert context['repository']=='gesher-runtime/gesher'and context['repository_id']=='1426153186'
        assert plan['runner']==os.environ['RUNNER_ENVIRONMENT_NAME']and plan['runner']in ('windows-2022','windows-2025')
        assert set(plan['roles'])=={'nativehost','x86_64','arm64-v8a'}
        reference=json.loads(args.reference.read_bytes());assert sha(args.reference)==plan['reference']['sha256']
        receipt(plan['reference']['receipt'],context,'file-mremap-linux-reference')
        assert reference['role']=='file-mremap-linux-reference'and reference['verdict']=='PASS'and reference['cleanup']['completed']
        assert (reference['commit'],reference['run'],reference['attempt'])==(context['source'],context['run'],context['attempt'])
        assert reference['source']==sha(Path(__file__).resolve().parents[2]/'tests/file_mremap/shared_alias.c')
        assert reference['lifetime_source']==sha(Path(__file__).resolve().parents[2]/'tests/file_mremap/lifetime_cases.c')
        assert [v['case']for v in reference['lifetime']]==list(CASES)and all(v['verdict']=='PASS'and v['cleanup']for v in reference['lifetime'])
        record.update(context=context,runner=plan['runner'],policy_ref=plan['policy_ref'],verdict='FAILED',authority_scope='STRUCTURAL_RECEIPTS_ONLY_IMMUTABLE_ADOPTION_PENDING')
        for role,entry in plan['roles'].items():
            fixture=bound_file(entry['fixture'],context,'file-mremap-fixture-'+role)
            assert entry['abi']==role
            compiler=entry['compiler'];receipt(compiler['receipt'],context,'file-mremap-compiler-current')
            assert re.fullmatch('[0-9a-f]{64}',compiler['driver_sha256'])and compiler['recipe_fingerprint']
            prefix=[fixture]
            if role!='nativehost':
                runtime=bound_file(entry['runtime'],context,'file-mremap-runtime-current')
                bound_file(entry['translator'],context,'file-mremap-translator-current')
                prefix=[runtime,'--elf',fixture,'file_mremap_lifetime'] # argv0: native2/arm64 existing literal grammar
                assert Path(entry['translator']['path']).resolve().parent==Path(runtime).resolve().parent
            environment={k:v for k,v in os.environ.items()if k in ('SystemRoot','WINDIR','PATH','TEMP','TMP','GITHUB_ACTIONS')}
            if role!='nativehost':
                library=Path(entry['library_directory']['path'])
                assert library.is_absolute()and library.is_dir()and not library.is_symlink()
                receipt(entry['library_directory']['receipt'],context,'file-mremap-guest-libraries-current')
                for name,digest in entry['library_directory']['members'].items():
                    assert Path(name).name==name and sha(library/name)==digest
                # Existing NoDrive LD_LIBRARY_PATH grammar; exact host root and
                # translator loading still need native CI, never guessed PASS.
                environment['LD_LIBRARY_PATH']=library.as_posix()[2:]
            for case in CASES:
                actual=run_lifetime(prefix,case,environment);actual['role']=role;actual['compiler']=compiler;record['results'].append(actual)
            if role=='nativehost':
                failure_fixture=bound_file(entry['failure_fixture'],context,'file-mremap-failure-fixture-nativehost')
                for case,mode,nth in FAULTS:
                    actual=run_lifetime([failure_fixture],case,environment,(mode,nth));actual['role']=role;record['results'].append(actual)
                for case in PROTECTION_CASES:
                    actual=run_lifetime([failure_fixture],case,environment);actual['role']=role;record['results'].append(actual)
        if len(record['results'])==len(CASES)*3+len(FAULTS)+len(PROTECTION_CASES) and all(v['verdict']=='PASS'and v['cleanup']for v in record['results']):record['verdict']='PASS'
    except Exception as error:record['error']=repr(error)
    finally:atomic(args.output,record)
    return 0 if record['verdict']=='PASS'else 1
if __name__=='__main__':raise SystemExit(main())
