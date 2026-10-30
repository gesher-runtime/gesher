"""CI-only fresh CLI runtime proof; old data/translator never supply current EXEs."""
import argparse,hashlib,importlib.util,json,os,re,shlex,shutil,subprocess
from pathlib import Path

COMPILER_SHA='c9b86311ade81d53235c93fafabdf98328d094de344ea9a9038e0dab6695ee9f'
PTHREAD_SHA='dbbbb3a59f5a1077346b69d4dd8ea85eeca9574bfbd2d170eaf91b5fe5b2f88e'
INPUTS=('cmake/aosp/art_runtime.cmake','compat/art/dalvikvm_main.cc','compat/elf/exec/exec_spawn.cc',
 'compat/elf/exec/exec_spawn.h','cmake/toolchain-llvm-mingw.cmake','aosp.lock','tools/fonts/core-runtime-manifest.py')
FILES=('dalvikvm.exe','gesher-exec.exe','dalvikvm.map','gesher-exec.fonts.map','dalvikvm.imports.txt',
 'gesher-exec.imports.txt','dalvikvm-main-relocations.txt','dalvikvm-main-disassembly.txt')
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def context():
 source=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
 assert source==os.environ['GITHUB_SHA'] and os.environ['GITHUB_REPOSITORY']=='gesher-runtime/gesher'
 return {'source':source,'run':os.environ['GITHUB_RUN_ID'],'attempt':os.environ['GITHUB_RUN_ATTEMPT'],
         'repository':os.environ['GITHUB_REPOSITORY'],'repository_id':os.environ['GITHUB_REPOSITORY_ID']}
def inputs():return {p:subprocess.check_output(['git','hash-object',p],text=True).strip() for p in INPUTS}
def capture(build,root):
 assert not root.exists();root.mkdir(parents=True)
 records=list((build/'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'));assert len(records)==1
 matches=re.findall(r'^\s*set\(CMAKE_CXX_COMPILER\s+"([^"]+)"\)\s*$',records[0].read_text(),re.M);assert len(matches)==1
 compiler=Path(matches[0]);assert compiler.is_absolute() and sha(compiler)==COMPILER_SHA
 tools=compiler.parent
 cache={row.split('=',1)[0].split(':',1)[0]:row.split('=',1)[1] for row in (build/'CMakeCache.txt').read_text().splitlines() if '=' in row and not row.startswith(('#','//'))}
 assert cache.get('GESHER_WEB3_QUERY_TESTS')=='OFF','Fresh runtime must retain ordinary default-OFF role'
 commands=json.loads((build/'compile_commands.json').read_text())
 targets={'upstream':Path('third_party/aosp/art/dalvikvm/dalvikvm.cc').resolve(),
          'adapter':Path('compat/art/dalvikvm_main.cc').resolve()}
 proof={};objects={}
 for role,source in targets.items():
  rows=[r for r in commands if Path(r['file']).resolve()==source];assert len(rows)==1
  row=rows[0];argv=row.get('arguments') or shlex.split(row['command'])
  index=argv.index(str(compiler));assert index in (0,1)
  if index==1:assert Path(argv[0]).name=='ccache'
  definitions=[a for a in argv if a.startswith('-Dmain=')]
  assert definitions==(['-Dmain=gesher_dalvikvm_main'] if role=='upstream' else [])
  output=Path(argv[argv.index('-o')+1]);output=output if output.is_absolute() else Path(row['directory'])/output
  assert output.is_file() and 'CMakeFiles/dalvikvm.dir/' in output.as_posix()
  proof[role]={'source':str(source),'source_sha256':sha(source),'command':argv,'object':str(output.resolve()),'object_sha256':sha(output)}
  objects[role]=output.resolve()
 # Reuse the already qualified actual GNU -Map hierarchy parser, not a new writer guess.
 path=Path('tools/fonts/verify-standalone-link.py');spec=importlib.util.spec_from_file_location('font_link_map',path);map_parser=importlib.util.module_from_spec(spec);spec.loader.exec_module(map_parser)
 raw=(build/'dalvikvm.map').read_text();rows=map_parser.map_rows(raw)
 relative={role:obj.relative_to(build.resolve()).as_posix() for role,obj in objects.items()}
 required={'adapter':'wmain','upstream':'gesher_dalvikvm_main(int, char**)'}
 for role,symbol in required.items():
  found=[(name,owner) for name,owner in rows if name==symbol]
  print('FONT_CLI_MAP_RAW '+json.dumps({'role':role,'wanted_symbol':symbol,'actual':found}),flush=True)
  assert found==[(symbol,relative[role])],('Wrong/missing live entry object',role,found)
 crt=Path(subprocess.check_output([str(compiler),'-print-file-name=crt2u.o'],text=True).strip()).resolve(strict=True)
 crt_origins={str(Path(owner).resolve()) for _,owner in rows if Path(owner).name=='crt2u.o'}
 print('FONT_CLI_CRT_RAW '+json.dumps({'compiler_selected':str(crt),'map_origins':sorted(crt_origins)}),flush=True)
 assert crt_origins=={str(crt)} and not any(Path(owner).name=='crt2.o' for _,owner in rows)
 names=subprocess.check_output([str(tools/'llvm-nm'),'-j','--defined-only',str(objects['upstream'])],text=True).splitlines()
 candidates=[name for name in names if 'gesher_dalvikvm_main' in name];assert len(candidates)==1
 symbol=candidates[0]
 relocations=subprocess.check_output([str(tools/'llvm-readobj'),'--relocations',str(objects['adapter'])],text=True)
 disassembly=subprocess.check_output([str(tools/'llvm-objdump'),'-dr',str(objects['adapter'])],text=True)
 raw_relocations=[line for line in relocations.splitlines() if symbol in line]
 print('FONT_CLI_RELOCATION_RAW '+json.dumps({'symbol':symbol,'rows':raw_relocations}),flush=True)
 assert len(raw_relocations)==1 and 'REL32' in raw_relocations[0],'Adapter lacks actual relative call relocation to upstream entry'
 pthread={}
 for name in ('libpthread.a','libwinpthread.a'):
  file=Path(subprocess.check_output([str(compiler),'-print-file-name='+name],text=True).strip()).resolve(strict=True)
  assert sha(file)==PTHREAD_SHA,('Noncanonical/private pthread archive',str(file))
  pthread[name]={'path':str(file),'sha256':sha(file),'role':'published-20261006-static-archive'}
 for target in ('dalvikvm','gesher-exec'):
  shutil.copy2(build/(target+'.exe'),root/(target+'.exe'))
  map_name='dalvikvm.map' if target=='dalvikvm' else 'gesher-exec.fonts.map'
  shutil.copy2(build/map_name,root/map_name)
  text=subprocess.check_output([str(tools/'llvm-readobj'),'--coff-imports',str(build/(target+'.exe'))],text=True)
  (root/(target+'.imports.txt')).write_text(text,encoding='utf-8')
  if target=='gesher-exec':assert not re.search(r'(?i)(jvm|dalvik|libart_runtime)',text)
 (root/'dalvikvm-main-relocations.txt').write_text(relocations,encoding='utf-8')
 (root/'dalvikvm-main-disassembly.txt').write_text(disassembly,encoding='utf-8')
 record={'role':'fresh-current-cli-runtime','context':context(),'inputs':inputs(),
  'outputs':{p:sha(root/p) for p in FILES},'compiler':{'path':str(compiler),'sha256':sha(compiler),
    'version':subprocess.check_output([str(compiler),'--version'],text=True)},
  'linker':{'sha256':sha(tools/'ld.lld'),'version':subprocess.check_output([str(tools/'ld.lld'),'--version'],text=True)},
  'main_objects':proof,'crt2u':{'path':str(crt),'sha256':sha(crt),'map_origins':sorted(crt_origins)},
  'adapter_upstream_relocation':raw_relocations,'canonical_pthread':pthread,'web3_query_tests':'OFF',
  'dalvikvm_link_command':subprocess.check_output(['ninja','-C',str(build),'-t','commands','-s','dalvikvm.exe'],text=True)}
 assert '-municode' in shlex.split(record['dalvikvm_link_command'])
 (root/'core-runtime-provenance.json').write_text(json.dumps(record,sort_keys=True)+'\n',encoding='utf-8')
 print('FONT_FRESH_RUNTIME_PROOF '+json.dumps(record,sort_keys=True),flush=True)
def verify(root):
 record=json.loads((root/'core-runtime-provenance.json').read_text(encoding='utf-8'))
 assert {p.name for p in root.iterdir()}==set(FILES)|{'core-runtime-provenance.json'}
 assert all(p.is_file() and not p.is_symlink() for p in root.iterdir())
 assert record['role']=='fresh-current-cli-runtime' and record['context']==context() and record['inputs']==inputs()
 assert record['outputs']=={p:sha(root/p) for p in FILES}
 assert record['compiler']['sha256']==COMPILER_SHA and record['web3_query_tests']=='OFF'
 assert all(r['sha256']==PTHREAD_SHA for r in record['canonical_pthread'].values()) and len(record['canonical_pthread'])==2
 print('FONT_FRESH_RUNTIME_VERIFIED '+json.dumps(record,sort_keys=True),flush=True)
def main():
 p=argparse.ArgumentParser();p.add_argument('mode',choices=('capture','verify'));p.add_argument('--build',type=Path,default=Path('out'));p.add_argument('--root',type=Path,required=True);a=p.parse_args()
 assert os.environ.get('GITHUB_ACTIONS')=='true','Runtime artifact handling is CI only'
 if a.mode=='capture':capture(a.build,a.root)
 else:verify(a.root)
if __name__=='__main__':main()
