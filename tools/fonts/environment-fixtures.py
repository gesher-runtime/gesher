"""Current-source/current-attempt tiny environment fixtures; CI-only binary handling."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
from font_records import emit
INPUTS=('tests/fonts-environment/probe.c','tests/fonts-environment/probe_jni.c','tests/fonts-environment/Native3FontEnvironmentTest.java','.github/workflows/fonts-environment-characterization.yml','tools/fonts/environment-fixtures.py','tools/fonts/font_records.py')
FILES=('font-env.dex','x86_64/font_env','arm64-v8a/font_env','x86_64/libfont_env.so','arm64-v8a/libfont_env.so')
COMPILER_SHA='a871130d810536f7bb924c8aeaff57c66de27bed9b13d0ccafba25fdcc8bd02d'
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def context():
 source=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip();assert source==os.environ['GITHUB_SHA']
 assert os.environ['GITHUB_REPOSITORY']=='gesher-runtime/gesher' and os.environ['GITHUB_REPOSITORY_ID']=='1426153186'
 return {'source':source,'run':os.environ['GITHUB_RUN_ID'],'attempt':os.environ['GITHUB_RUN_ATTEMPT'],'repository':os.environ['GITHUB_REPOSITORY'],'repository_id':os.environ['GITHUB_REPOSITORY_ID']}
def main():
 parser=argparse.ArgumentParser();parser.add_argument('mode',choices=('capture','verify'));parser.add_argument('--root',type=Path,required=True);a=parser.parse_args()
 assert os.environ.get('GITHUB_ACTIONS')=='true','Fixture binaries are CI only'
 assert a.root.is_dir() and all(not p.is_symlink() for p in a.root.rglob('*')),'Linked fixture input'
 actual={p.relative_to(a.root).as_posix() for p in a.root.rglob('*') if p.is_file()}
 assert actual==set(FILES)|({'font-environment-fixture.json'} if a.mode=='verify' else set()),'Unexpected/missing diagnostic fixture file'
 inputs={p:subprocess.check_output(['git','hash-object',p],text=True).strip() for p in INPUTS};outputs={p:sha(a.root/p) for p in FILES};manifest=a.root/'font-environment-fixture.json'
 if a.mode=='capture':
  tools=Path(os.environ['NDK_DIR'])/'toolchains/llvm/prebuilt/linux-x86_64/bin';compiler=tools/'clang';headers={};symbols={}
  assert sha(compiler)==COMPILER_SHA and sha('r8.jar')=='3502f087388ce95bbf5254555927f09385dc2636873964d0bd53f3eaf4fb49ad' and sha('dns-ndk.zip')=='59c2f6dc96743b5daf5d1626684640b20a6bd2b1d85b13156b90333741bad5cc'
  for abi,machine in (('x86_64',62),('arm64-v8a',183)):
   for name in ('font_env','libfont_env.so'):
    path=abi+'/'+name;raw=(a.root/path).read_bytes();assert raw[:6]==b'\x7fELF\x02\x01' and int.from_bytes(raw[18:20],'little')==machine
    headers[path]=subprocess.check_output([str(tools/'llvm-readelf'),'-h',str(a.root/path)],text=True)
    raw_symbols=subprocess.check_output([str(tools/'llvm-readelf'),'-Ws',str(a.root/path)],text=True)
    imports=[line for line in raw_symbols.splitlines() if ' UND ' in line and re.search(r'\b(?:getenv|environ)(?:@[^ ]+)?(?: \(.*\))?$',line)]
    assert any(re.search(r'\bgetenv(?:@|$)',line) for line in imports) and any(re.search(r'\benviron(?:@|$)',line) for line in imports),'Actual environment imports missing'
    symbols[path]=imports
  record={'role':'CURRENT_FONT_ENVIRONMENT_DIAGNOSTIC_FIXTURES','context':context(),'inputs':inputs,'outputs':outputs,'compiler_sha256':sha(compiler),'compiler_version':subprocess.check_output([str(compiler),'--version'],text=True),'r8_sha256':sha('r8.jar'),'ndk_archive_sha256':sha('dns-ndk.zip'),'java':subprocess.check_output(['javac','-version'],stderr=subprocess.STDOUT,text=True),'elf_headers':headers,'actual_environment_imports':symbols,'font_fallback_qualification':False}
  assert not manifest.exists();manifest.write_text(json.dumps(record,sort_keys=True)+'\n',encoding='utf-8')
 else:
  record=json.loads(manifest.read_text(encoding='utf-8'));assert record['role']=='CURRENT_FONT_ENVIRONMENT_DIAGNOSTIC_FIXTURES' and record['context']==context() and record['inputs']==inputs and record['outputs']==outputs and record['compiler_sha256']==COMPILER_SHA
 for key,value in record.items():emit('FONT_ENV_FIXTURE_'+key.upper(),value)
 emit('FONT_ENV_FIXTURE_MANIFEST',{'sha256':sha(manifest),'role':record['role'],'context':record['context']})
if __name__=='__main__':main()
