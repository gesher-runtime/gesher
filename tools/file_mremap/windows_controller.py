"""CI-only owned actor controller. No native operation on import.

Source admission requires a future immutable authority's plan hash and typed
current receipts; this module does not manufacture/authenticate GitHub receipts.
"""
import argparse,datetime,hashlib,json,os,platform,queue,re,subprocess,tempfile,threading,time,uuid
from pathlib import Path
CASES=('source_first','alias_first','partial','middle')
RETAINED=[]
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def atomic(path,value):
    temp=path.with_suffix(path.suffix+'.tmp');temp.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8');temp.replace(path)
class Actor:
    def __init__(self,argv,cwd,environment):
        self.record=dict(argv=argv,cwd=str(cwd),started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),started_monotonic_ns=time.monotonic_ns(),pid=None,exit=None,stdout='',stderr='',overflow=False)
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
                        try:self.events.put_nowait(json.loads(line))
                        except (ValueError,queue.Full):self.record['bad_json']=True
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
def run_lifetime(prefix,case,environment=None):
    """Actual parent/peer handshake on Linux or Windows, one owned directory."""
    result=dict(case=case,verdict='FAILED',actors=[],cleanup=False)
    temporary=None;actors=[];deadline=time.monotonic()+45
    try:
        assert case in CASES
        temporary=tempfile.TemporaryDirectory(prefix='file-mremap-case-');cwd=Path(temporary.name);nonce=uuid.uuid4().hex
        environment=dict(environment or {k:v for k,v in os.environ.items()if k in ('SystemRoot','WINDIR','PATH','TEMP','TMP','GITHUB_ACTIONS')})
        environment['TMPDIR']=str(cwd).replace('\\','/')
        parent=Actor(prefix+[case,'backing.dat',nonce],cwd,environment);actors.append(parent);parent.start()
        ready=parent.event('ready',deadline);assert ready['nonce']==nonce and ready['pid']==parent.proc.pid
        size=ready['file_size'];assert type(size)is int and size==ready['page']*5 and 20480<=size<=5242880
        backing=cwd/'backing.dat';before=backing.read_bytes();assert before==expected_bytes(size,False)
        result.update(page=ready['page'],file_size_before=len(before),file_sha256_before=hashlib.sha256(before).hexdigest())
        peer=Actor(prefix+['peer','backing.dat',nonce],cwd,environment);actors.append(peer);peer.start()
        peer_done=peer.event('peer_complete',deadline);assert peer_done['nonce']==nonce and peer_done['pid']==peer.proc.pid
        assert peer.close(deadline)and peer.record['exit']==0
        assert peer_done['failed']==peer_done['cleanup_failures']==0 and peer_done['resources_before']==peer_done['resources_after']>=0
        result['peer']=peer_done
        parent.proc.stdin.write(b'PEER_DONE\n');parent.proc.stdin.flush();parent.proc.stdin.close()
        done=parent.event('complete',deadline);assert done['nonce']==nonce and done['case']==case
        assert parent.close(deadline)and parent.record['exit']==0
        after=backing.read_bytes();assert after==expected_bytes(size,True)
        result.update(file_size_after=len(after),file_sha256_after=hashlib.sha256(after).hexdigest(),parent=done)
        assert done['failed']==done['cleanup_failures']==0 and done['file_size']==size
        assert done['resources_before']==done['resources_after']>=0
        result['verdict']='PASS'
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
        record.update(context=context,runner=plan['runner'],policy_ref=plan['policy_ref'],verdict='FAILED')
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
        if all(v['verdict']=='PASS'and v['cleanup']for v in record['results']):record['verdict']='PASS'
    except Exception as error:record['error']=repr(error)
    finally:atomic(args.output,record)
    return 0 if record['verdict']=='PASS'else 1
if __name__=='__main__':raise SystemExit(main())
