"""CI-only Linux reference producer, no build invoked by import."""
import argparse,hashlib,json,os,platform,shutil,subprocess,tempfile
from pathlib import Path
from oracle import verify

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    assert os.environ.get('GITHUB_ACTIONS')=='true' and platform.system()=='Linux'
    assert not args.output.exists()
    root=Path(__file__).resolve().parents[2];source=root/'tests/file_mremap/shared_alias.c'
    compiler=Path(shutil.which('gcc')).resolve()
    version=subprocess.check_output([str(compiler),'--version'],text=True,timeout=30)
    with tempfile.TemporaryDirectory(prefix='file-mremap-reference-')as temp:
        output=Path(temp)/'reference'
        command=[str(compiler),'-std=c11','-Wall','-Wextra','-Werror',str(source),'-o',str(output)]
        subprocess.run(command,check=True,timeout=60)
        run=subprocess.run([str(output)],cwd=temp,check=True,capture_output=True,text=True,timeout=30)
        rows=verify(run.stdout)
    record=dict(schema=1,role='file-mremap-linux-reference',source=hashlib.sha256(source.read_bytes()).hexdigest(),
                commit=os.environ['GITHUB_SHA'],run=os.environ['GITHUB_RUN_ID'],attempt=os.environ['GITHUB_RUN_ATTEMPT'],
                compiler=str(compiler),compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(),
                version=version,kernel=platform.release(),command=command,stdout=run.stdout,rows=rows,
                scope='PARTIAL_A_ALIAS_EQUAL_ONLY',known_gap_families=['resize','private','fixed','dontunmap','beyond_eof','rollback_B'])
    args.output.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
if __name__=='__main__':main()
