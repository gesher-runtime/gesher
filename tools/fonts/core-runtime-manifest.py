"""CI-only fresh CLI runtime proof; old data/translator never supply current EXEs."""
import argparse,hashlib,importlib.util,json,os,re,shlex,shutil,subprocess
from font_records import emit
from pathlib import Path

COMPILER_SHA='c9b86311ade81d53235c93fafabdf98328d094de344ea9a9038e0dab6695ee9f'
UPSTREAM_SHA='840583bf5bc9b55e5b1ddb57ff3642af5d4ecaf9040a2c84fa374be3fb6b9259'
PTHREAD_SHA='dbbbb3a59f5a1077346b69d4dd8ea85eeca9574bfbd2d170eaf91b5fe5b2f88e'
INPUTS=('cmake/aosp/art_runtime.cmake','compat/art/dalvikvm_main.cc','compat/elf/exec/exec_spawn.cc',
 'compat/elf/exec/exec_spawn.h','cmake/toolchain-llvm-mingw.cmake','aosp.lock','tools/fonts/core-runtime-manifest.py',
 'tools/fonts/cli-link-evidence.py','tools/fonts/font_records.py','tools/fonts/verify-standalone-link.py')
FILES=('dalvikvm.exe','gesher-exec.exe','dalvikvm.map','gesher-exec.fonts.map','dalvikvm.imports.txt',
 'gesher-exec.imports.txt','dalvikvm-main-relocations.txt','dalvikvm-main-disassembly.txt',
 'fonts-dalvikvm-build.log','fonts-gesher-exec-build.log')
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
  actual_sha=sha(source)
  emit('FONT_CLI_COMPILED_SOURCE_RAW',{'role':role,'source':str(source),'actual_sha256':actual_sha,'expected_upstream_sha256':UPSTREAM_SHA if role=='upstream' else None})
  if role=='upstream':assert actual_sha==UPSTREAM_SHA,'Pinned LF upstream dalvikvm source differs before main rename proof'
  row=rows[0];argv=row.get('arguments') or shlex.split(row['command'])
  index=argv.index(str(compiler));assert index in (0,1)
  if index==1:assert Path(argv[0]).name=='ccache'
  definitions=[a for a in argv if a.startswith('-Dmain=')]
  assert definitions==(['-Dmain=gesher_dalvikvm_main'] if role=='upstream' else [])
  output=Path(argv[argv.index('-o')+1]);output=output if output.is_absolute() else Path(row['directory'])/output
  assert output.is_file() and 'CMakeFiles/dalvikvm.dir/' in output.as_posix()
  proof[role]={'source':str(source),'source_sha256':sha(source),'command':argv,'object':str(output.resolve()),'object_sha256':sha(output)}
  objects[role]=output.resolve()
 # Preserve RVA and owner, validating through the already qualified actual writer schema.
 path=Path('tools/fonts/cli-link-evidence.py');spec=importlib.util.spec_from_file_location('font_cli_link',path);evidence=importlib.util.module_from_spec(spec);spec.loader.exec_module(evidence)
 raw=(build/'dalvikvm.map').read_text()
 emit('FONT_CLI_MAP_HEADER_RAW',{'map_sha256':sha(build/'dalvikvm.map'),'first_lines':raw.splitlines()[:8]})
 records=evidence.map_records(raw)
 required={'adapter':'wmain','upstream':'gesher_dalvikvm_main(int, char**)'}
 entries={role:evidence.entry(records,symbol,objects[role],build.resolve()) for role,symbol in required.items()}
 crt=Path(os.path.abspath(subprocess.check_output([str(compiler),'-print-file-name=crt2u.o'],text=True).strip()))
 crt_origins={str((build/Path(owner)).resolve()) for _,_,owner in records if Path(owner).name=='crt2u.o'}
 emit('FONT_CLI_CRT_RAW',{'compiler_selected':str(crt),'map_origins':sorted(crt_origins)})
 assert crt_origins=={str(crt.resolve(strict=True))} and not any(Path(owner).name=='crt2.o' for _,_,owner in records)
 names=subprocess.check_output([str(tools/'llvm-nm'),'-j','--defined-only',str(objects['upstream'])],text=True).splitlines()
 candidates=[name for name in names if 'gesher_dalvikvm_main' in name];assert len(candidates)==1
 symbol=candidates[0]
 relocations=subprocess.check_output([str(tools/'llvm-readobj'),'--relocations',str(objects['adapter'])],text=True)
 disassembly=subprocess.check_output([str(tools/'llvm-objdump'),'-dr',str(objects['adapter'])],text=True)
 raw_relocations=[line for line in relocations.splitlines() if symbol in line]
 emit('FONT_CLI_RELOCATION_RAW',{'symbol':symbol,'rows':raw_relocations})
 assert len(raw_relocations)==1 and 'REL32' in raw_relocations[0],'Adapter lacks actual relative call relocation to upstream entry'
 pthread={}
 for name in ('libpthread.a','libwinpthread.a'):
  file=Path(os.path.abspath(subprocess.check_output([str(compiler),'-print-file-name='+name],text=True).strip()))
  emit('FONT_CLI_CANONICAL_ARCHIVE_RAW',{'alias':name,'path':str(file),'resolved_path':str(file.resolve(strict=True)),'sha256':sha(file),'expected_sha256':PTHREAD_SHA})
  assert sha(file)==PTHREAD_SHA,('Noncanonical/private pthread archive',str(file))
  pthread[name]={'path':str(file),'resolved_path':str(file.resolve()),'sha256':sha(file),'role':'published-20261006-static-archive'}
 link_proofs={};link_commands={}
 for target in ('dalvikvm','gesher-exec'):
  command=subprocess.check_output(['ninja','-t','commands','-s',target+'.exe'],cwd=build,text=True).strip()
  argv=shlex.split(command);index=argv.index(str(compiler));end=argv.index('&&',index) if '&&' in argv[index:] else len(argv);link_argv=argv[index:end]
  emit('FONT_CLI_ACTUAL_LINK_COMMAND',{'target':target,'argv':link_argv})
  assert '-static' in link_argv and '-Wl,--verbose' in link_argv and not any(arg.startswith('@') for arg in link_argv)
  assert Path(link_argv[link_argv.index('-o')+1]).name==target+'.exe'
  if target=='dalvikvm':assert '-municode' in link_argv
  direct={(build/Path(arg)).resolve() for arg in link_argv if not arg.startswith('-') and arg.endswith(('.obj','.o'))}
  trace=Path('fonts-'+target+'-build.log').read_text()
  map_name='dalvikvm.map' if target=='dalvikvm' else 'gesher-exec.fonts.map'
  map_text=(build/map_name).read_text()
  emit('FONT_CLI_TARGET_MAP_RAW',{'target':target,'path':str(build/map_name),'sha256':sha(build/map_name),'first_lines':map_text.splitlines()[:8]})
  mapped=evidence.map_records(map_text)
  link_proofs[target]=evidence.prove_pthread(mapped,trace,command,pthread,build.resolve(),tools,direct)
  link_commands[target]=command
  shutil.copy2('fonts-'+target+'-build.log',root/('fonts-'+target+'-build.log'))
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
  'main_objects':proof,'live_entries':entries,'pinned_upstream_sha256':UPSTREAM_SHA,'crt2u':{'path':str(crt),'sha256':sha(crt),'map_origins':sorted(crt_origins)},
  'adapter_upstream_relocation':raw_relocations,'canonical_pthread':pthread,'web3_query_tests':'OFF',
  'actual_link_commands':link_commands,'pthread_link_proofs':link_proofs}
 (root/'core-runtime-provenance.json').write_text(json.dumps(record,sort_keys=True)+'\n',encoding='utf-8')
 log_record(root,record,'CAPTURED')
def log_record(root,record,stage):
 for path,blob in record['inputs'].items():emit('FONT_FRESH_RUNTIME_INPUT',{'path':path,'blob':blob})
 for path,digest in record['outputs'].items():emit('FONT_FRESH_RUNTIME_OUTPUT',{'path':path,'sha256':digest})
 for role,value in record['main_objects'].items():emit('FONT_FRESH_RUNTIME_COMPILED_OBJECT',{'role':role,**value})
 for role,value in record['live_entries'].items():emit('FONT_FRESH_RUNTIME_LIVE_ENTRY',{'role':role,**value})
 for target,command in record['actual_link_commands'].items():emit('FONT_FRESH_RUNTIME_LINK_COMMAND',{'target':target,'command':command})
 for target,value in record['pthread_link_proofs'].items():
  for name,path in value['actual_read_archives'].items():emit('FONT_FRESH_RUNTIME_OPENED_ARCHIVE',{'target':target,'alias':name,'path':path})
  for member in value['selected_members']:emit('FONT_FRESH_RUNTIME_SELECTED_MEMBER',{'target':target,**member})
 for name in ('compiler','linker','crt2u','adapter_upstream_relocation','canonical_pthread'):emit('FONT_FRESH_RUNTIME_'+name.upper(),record[name])
 emit('FONT_FRESH_RUNTIME_'+stage,{'context':record['context'],'role':record['role'],'outputs':len(record['outputs']),'manifest_sha256':sha(root/'core-runtime-provenance.json'),'pinned_upstream_sha256':record['pinned_upstream_sha256']})

def verify(root):
 record=json.loads((root/'core-runtime-provenance.json').read_text(encoding='utf-8'))
 assert {p.name for p in root.iterdir()}==set(FILES)|{'core-runtime-provenance.json'}
 assert all(p.is_file() and not p.is_symlink() for p in root.iterdir())
 assert record['role']=='fresh-current-cli-runtime' and record['context']==context() and record['inputs']==inputs()
 assert record['outputs']=={p:sha(root/p) for p in FILES}
 assert record['compiler']['sha256']==COMPILER_SHA and record['web3_query_tests']=='OFF'
 assert set(record['canonical_pthread'])=={'libpthread.a','libwinpthread.a'} and all(r['sha256']==PTHREAD_SHA for r in record['canonical_pthread'].values())
 assert record['pinned_upstream_sha256']==UPSTREAM_SHA and record['main_objects']['upstream']['source_sha256']==UPSTREAM_SHA
 assert set(record['pthread_link_proofs'])=={'dalvikvm','gesher-exec'} and all(value['selected_members'] for value in record['pthread_link_proofs'].values())
 for value in record['pthread_link_proofs'].values():
  for item in value['selected_members']:
   assert item['archive_sha256']==PTHREAD_SHA and item['live_defined_map_rows']
   assert item['archive'] in {item['path'] for item in record['canonical_pthread'].values()}
 log_record(root,record,'VERIFIED')
def main():
 p=argparse.ArgumentParser();p.add_argument('mode',choices=('capture','verify'));p.add_argument('--build',type=Path,default=Path('out'));p.add_argument('--root',type=Path,required=True);a=p.parse_args()
 assert os.environ.get('GITHUB_ACTIONS')=='true','Runtime artifact handling is CI only'
 if a.mode=='capture':capture(a.build,a.root)
 else:verify(a.root)
if __name__=='__main__':main()
