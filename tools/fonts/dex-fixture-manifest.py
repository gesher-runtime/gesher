"""CI-only provenance for the isolated font binaries emitted by native2."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path

INPUTS=('tests/native2/fonts_standalone.c','tests/native2/fonts_standalone_jni.c',
        'tests/native2/java/gesher/Native3StandaloneFontTest.java',
        'tests/native2/java/gesher/Native3DefaultFontTest.java',
        'tests/native2/java/gesher/Native3CliArgumentsTest.java','.github/workflows/native2.yml')
OUTPUTS=('native2-test.dex','x86_64/fonts_standalone','arm64-v8a/fonts_standalone',
         'x86_64/libfonts_standalone.so','arm64-v8a/libfonts_standalone.so')
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('mode',choices=('capture','verify'));p.add_argument('--root',type=Path,required=True);a=p.parse_args()
 assert os.environ.get('GITHUB_ACTIONS')=='true'
 source=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip();assert source==os.environ['GITHUB_SHA']
 context={'source':source,'run':os.environ['GITHUB_RUN_ID'],'attempt':os.environ['GITHUB_RUN_ATTEMPT'],
          'repository':os.environ['GITHUB_REPOSITORY'],'repository_id':os.environ['GITHUB_REPOSITORY_ID']}
 inputs={f:subprocess.check_output(['git','hash-object',f],text=True).strip() for f in INPUTS}
 outputs={f:sha(a.root/f) for f in OUTPUTS}
 manifest=a.root/'fonts-fixture-provenance.json'
 if a.mode=='capture':
  llvm=Path(os.environ['NDK_DIR'])/'toolchains/llvm/prebuilt/linux-x86_64/bin'
  clang=llvm/'clang';readelf=llvm/'llvm-readelf'
  headers={f:subprocess.check_output([str(readelf),'-h',str(a.root/f)],text=True) for f in OUTPUTS if f.endswith('.so')}
  for abi,machine in (('x86_64',62),('arm64-v8a',183)):
   raw=(a.root/abi/'libfonts_standalone.so').read_bytes()
   assert raw[:6]==b'\x7fELF\x02\x01' and int.from_bytes(raw[18:20],'little')==machine
  record={'role':'current-font-dex-fixtures','context':context,'inputs':inputs,'outputs':outputs,
    'compiler':{'path':str(clang.resolve()),'sha256':sha(clang),
                'version':subprocess.check_output([str(clang),'--version'],text=True)},
    'java':subprocess.check_output(['javac','-version'],stderr=subprocess.STDOUT,text=True),
    'r8_sha256':sha('r8.jar'),'authenticated_ndk_archive_sha256':sha('dns-ndk.zip'),'elf_headers':headers}
  assert record['r8_sha256']=='3502f087388ce95bbf5254555927f09385dc2636873964d0bd53f3eaf4fb49ad'
  assert not manifest.exists();manifest.write_text(json.dumps(record,sort_keys=True)+'\n',encoding='utf-8')
 else:
  record=json.loads(manifest.read_text(encoding='utf-8'))
  assert record['role']=='current-font-dex-fixtures' and record['context']==context
  assert record['inputs']==inputs and record['outputs']==outputs
 print('FONT_CURRENT_FIXTURE_MANIFEST '+json.dumps(record,sort_keys=True),flush=True)
if __name__=='__main__':main()
