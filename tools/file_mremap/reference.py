"""CI-only Linux reference; persists actual failure receipts before verdict."""
import argparse,hashlib,json,os,platform,shutil,subprocess,tempfile
from pathlib import Path
from oracle import verify
from windows_controller import run_lifetime,CASES

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    assert not args.output.exists()
    root=Path(__file__).resolve().parents[2];source=root/'tests/file_mremap/shared_alias.c'
    record=dict(schema=2,role='file-mremap-linux-reference',source=hashlib.sha256(source.read_bytes()).hexdigest(),
                commit=os.environ.get('GITHUB_SHA'),run=os.environ.get('GITHUB_RUN_ID'),attempt=os.environ.get('GITHUB_RUN_ATTEMPT'),
                kernel=platform.release(),lifetime_source=hashlib.sha256((root/'tests/file_mremap/lifetime_cases.c').read_bytes()).hexdigest(),lifetime=[],steps=[],verdict='FAILED',cleanup=dict(attempted=False,completed=False,error=None),
                scope='PARTIAL_A_ALIAS_EQUAL_ONLY',known_gap_families=['resize','private','fixed','dontunmap','beyond_eof','rollback_B'])
    temporary=None
    def invoke(name,command,timeout,cwd=None):
        step=dict(name=name,command=command,stdout='',stderr='',exit=None,timeout=False)
        record['steps'].append(step)
        try:
            result=subprocess.run(command,cwd=cwd,capture_output=True,text=True,timeout=timeout)
            step.update(stdout=result.stdout,stderr=result.stderr,exit=result.returncode)
        except subprocess.TimeoutExpired as error:
            def decode(value):return value.decode('utf-8',errors='replace')if isinstance(value,bytes)else(value or'')
            step.update(stdout=decode(error.stdout),stderr=decode(error.stderr),timeout=True)
        except OSError as error:step['launch_error']=str(error)
        return step
    try:
        if os.environ.get('GITHUB_ACTIONS')!='true' or platform.system()!='Linux':
            record['prerequisite']='UNAVAILABLE_NOT_LINUX_CI'
        elif not shutil.which('gcc'):record['prerequisite']='UNAVAILABLE_GCC'
        else:
            compiler=Path(shutil.which('gcc')).resolve()
            record.update(compiler=str(compiler),compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest())
            version=invoke('version',[str(compiler),'--version'],30)
            if version['exit']!=0:record['prerequisite']='COMPILER_VERSION_FAILED'
            else:
                temporary=tempfile.TemporaryDirectory(prefix='file-mremap-reference-');output=Path(temporary.name)/'reference'
                build=invoke('compile',[str(compiler),'-std=c11','-Wall','-Wextra','-Werror',str(source),'-o',str(output)],60)
                if build['exit']==0:
                    native=invoke('native',[str(output)],30,temporary.name)
                    try:
                        record['rows']=verify(native['stdout'])
                        record['oracle']='PASS'
                    except (AssertionError,KeyError,TypeError,ValueError)as error:
                        record['oracle']='FAIL';record['oracle_error']=repr(error)
                    if native['exit']==0 and record['oracle']=='PASS':
                        lifetime=Path(temporary.name)/'lifetime'
                        lifetime_build=invoke('compile_lifetime',[str(compiler),'-std=c11','-Wall','-Wextra','-Werror',str(root/'tests/file_mremap/lifetime_cases.c'),'-o',str(lifetime)],60)
                        if lifetime_build['exit']==0:
                            record['lifetime']=[run_lifetime([str(lifetime)],case)for case in CASES]
                            if all(row['verdict']=='PASS'and row['cleanup']for row in record['lifetime']):record['verdict']='PASS'
    except Exception as error:record['controller_error']=repr(error)
    finally:
        if temporary is not None:
            record['cleanup']['attempted']=True
            try:temporary.cleanup();record['cleanup']['completed']=True
            except OSError as error:record['cleanup']['error']=str(error);record['verdict']='FAILED'
        else:record['cleanup']['completed']=True # no native resources were acquired
        q=args.output.with_suffix(args.output.suffix+'.tmp')
        q.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8');q.replace(args.output)
    return 0 if record['verdict']=='PASS'else 1
if __name__=='__main__':raise SystemExit(main())
