"""CI-only current-source host-owner binary, selected-member and caller proof."""
import argparse,hashlib,importlib.util,json,os,re,shlex,shutil,subprocess,tempfile
from pathlib import Path
from font_records import emit
COMPILER_SHA='c9b86311ade81d53235c93fafabdf98328d094de344ea9a9038e0dab6695ee9f'
PTHREAD_SHA='dbbbb3a59f5a1077346b69d4dd8ea85eeca9574bfbd2d170eaf91b5fe5b2f88e'
ROLE='CURRENT_SOURCE_HOST_FONT_OWNER_ONLY'
INPUTS=('CMakeLists.txt','compat/posix/posix.cmake','compat/posix/include/gesher/font_environment.h','compat/posix/src/font_environment.cc','compat/posix/src/path.cc','compat/posix/src/unistd.cc','compat/bionic/shim.cc','compat/bionic/native_fonts.cc','tests/native/font_environment_test.cc','.github/workflows/fonts-owner-host-component.yml','tools/fonts/owner-host-manifest.py','tools/fonts/run-owner-host.ps1','tools/fonts/test-owner-host-arguments.ps1','tools/fonts/font_records.py','tools/fonts/cli-link-evidence.py','tools/fonts/verify-standalone-link.py')
FILES=('font_environment_test.exe','font_environment_test.map','font_environment_test.imports.txt','font-owner-host-build.log')
ROLES={'owner':'compat/posix/src/font_environment.cc','host_POSIX':'compat/posix/src/unistd.cc','font_paths':'compat/posix/src/path.cc','fixture':'tests/native/font_environment_test.cc'}
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def load(name):
 spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(name+'.py'));value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
def context():
 source=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
 assert source==os.environ['GITHUB_SHA'] and os.environ['GITHUB_REPOSITORY']=='gesher-runtime/gesher' and os.environ['GITHUB_REPOSITORY_ID']=='1426153186'
 return {'source':source,'run':os.environ['GITHUB_RUN_ID'],'attempt':os.environ['GITHUB_RUN_ATTEMPT'],'repository':os.environ['GITHUB_REPOSITORY'],'repository_id':os.environ['GITHUB_REPOSITORY_ID']}
def inputs():
 result={p:subprocess.check_output(['git','hash-object',p],text=True).strip() for p in INPUTS}
 assert all(subprocess.check_output(['git','rev-parse','HEAD:'+p],text=True).strip()==b for p,b in result.items()),'Compiled/source receipt checkout dirty'
 return result

def capture(build,root):
 assert not root.exists();root.mkdir(parents=True);evidence=load('cli-link-evidence')
 metadata=list((build/'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'));assert len(metadata)==1
 matches=re.findall(r'^\s*set\(CMAKE_CXX_COMPILER\s+"([^"]+)"\)\s*$',metadata[0].read_text(),re.M);assert len(matches)==1
 compiler=Path(matches[0]);emit('FONT_OWNER_HOST_COMPILER_RAW',{'path':str(compiler),'sha256':sha(compiler),'expected_sha256':COMPILER_SHA});assert compiler.is_absolute() and sha(compiler)==COMPILER_SHA;tools=compiler.parent
 cache={line.split('=',1)[0].split(':',1)[0]:line.split('=',1)[1] for line in (build/'CMakeCache.txt').read_text().splitlines() if '=' in line and not line.startswith(('#','//'))}
 assert cache.get('GESHER_WEB3_QUERY_TESTS')=='OFF'
 commands=json.loads((build/'compile_commands.json').read_text());compiled={}
 for role,path in ROLES.items():
  rows=[row for row in commands if Path(row['file']).resolve()==Path(path).resolve()];assert len(rows)==1
  row=rows[0];argv=row.get('arguments') or shlex.split(row['command']);index=argv.index(str(compiler));assert index in (0,1)
  if index:assert Path(argv[0]).name=='ccache'
  emit('FONT_OWNER_HOST_COMPILE_COMMAND_RAW',{'role':role,'argv':argv})
  assert [v for v in argv if v in ('-fexceptions','-fno-exceptions')][-1:] == ['-fno-exceptions']
  obj=Path(argv[argv.index('-o')+1]);obj=obj if obj.is_absolute() else Path(row['directory'])/obj;assert obj.is_file()
  compiled[role]={'path':path,'source_sha256':sha(path),'object':str(obj.resolve()),'object_sha256':sha(obj),'argv':argv}
  emit('FONT_OWNER_HOST_COMPILED_OBJECT',{'role':role,**compiled[role]})
 command=subprocess.check_output(['ninja','-t','commands','-s','font_environment_test.exe'],cwd=build,text=True).strip();argv=shlex.split(command);start=argv.index(str(compiler));end=argv.index('&&',start) if '&&' in argv[start:] else len(argv);link=argv[start:end]
 emit('FONT_OWNER_HOST_ACTUAL_LINK',{'command':command,'argv':link})
 assert '-static' in link and '-Wl,--verbose' in link and not any(v.startswith('@') for v in link) and Path(link[link.index('-o')+1]).name=='font_environment_test.exe'
 direct={(build/Path(v)).resolve() for v in link if not v.startswith('-') and v.endswith(('.obj','.o'))}
 fixture_object=Path(compiled['fixture']['object']).resolve();assert fixture_object in direct,'Compiled fixture object is not an actual final direct link input'
 trace=Path('font-owner-host-build.log').read_text();mapping=(build/'font_environment_test.map').read_text();emit('FONT_OWNER_HOST_MAP_RAW',{'first_lines':mapping.splitlines()[:8],'sha256':sha(build/'font_environment_test.map')});rows=evidence.map_records(mapping)
 fixture_entry=evidence.entry(rows,'main',fixture_object,build.resolve())
 canonical={}
 for alias in ('libpthread.a','libwinpthread.a'):
  path=Path(os.path.abspath(subprocess.check_output([str(compiler),'-print-file-name='+alias],text=True).strip()));emit('FONT_OWNER_HOST_PTHREAD_BYTES_RAW',{'path':str(path),'sha256':sha(path),'expected_sha256':PTHREAD_SHA});assert sha(path)==PTHREAD_SHA
  canonical[alias]={'path':str(path),'resolved_path':str(path.resolve()),'sha256':sha(path)}
  emit('FONT_OWNER_HOST_CANONICAL_PTHREAD',{'alias':alias,**canonical[alias]})
 pthread=evidence.prove_pthread(rows,trace,command,canonical,build.resolve(),tools,direct)
 archives={name:Path(path) for name,path in pthread['actual_read_archives'].items()};archive=(build/'libgesher_posix.a').resolve()
 assert archives.get('libgesher_posix.a') and archives['libgesher_posix.a'].resolve()==archive
 inventories={p:subprocess.check_output([str(tools/'llvm-ar'),'t',str(p)],text=True).splitlines() for p in set(archives.values())}
 scope=evidence.link_scope(trace,command)
 for line in scope.splitlines():
  if 'libgesher_posix.a' in line or 'font_environment.cc.obj' in line:emit('FONT_OWNER_HOST_ARCHIVE_RAW',line)
 loaded=re.findall(r'(?m)^(?:ld\.lld|lld-link): Loaded libgesher_posix\.a\(([^()]+)\) for (\S+)\s*$',scope)
 required={'owner':('gesher::ReadFontEnvironment(','gesher::WriteFontEnvironment('),'host_POSIX':('setenv','unsetenv'),'font_paths':('gesher::SystemFontFiles(',)};members={};symbols={}
 for role,prefixes in required.items():
  obj=Path(compiled[role]['object']);member=obj.name
  origins=[str(p) for p,items in inventories.items() for name in items if Path(name).name==member]
  emit('FONT_OWNER_HOST_MEMBER_ORIGINS',{'role':role,'member':member,'archive_origins':origins,'direct_collisions':[str(p) for p in direct if p.name==member]})
  assert origins==[str(archive)] and inventories[archive].count(member)==1 and not any(p.name==member for p in direct)
  data=subprocess.check_output([str(tools/'llvm-ar'),'p',str(archive),member]);assert hashlib.sha256(data).hexdigest()==compiled[role]['object_sha256']
  defined=set(subprocess.check_output([str(tools/'llvm-nm'),'-j','--demangle','--defined-only',str(obj)],text=True).splitlines());raw=set(subprocess.check_output([str(tools/'llvm-nm'),'-j','--defined-only',str(obj)],text=True).splitlines())
  live={}
  for prefix in prefixes:
   candidates=[symbol for symbol in defined if symbol==prefix or symbol.startswith(prefix)];assert len(candidates)==1
   symbol=candidates[0];found={(rva,name,owner) for rva,name,owner in rows if name==symbol};emit('FONT_OWNER_HOST_REQUIRED_LIVE_RAW',{'role':role,'symbol':symbol,'rows':sorted(found)})
   assert len(found)==1 and next(iter(found))[2]==member;live[symbol]=sorted(found)
  selected=[{'member':m,'required':s} for m,s in loaded if m==member];assert selected and all(x['required'] in raw for x in selected)
  members[role]={'archive':str(archive),'archive_sha256':sha(archive),'member':member,'member_sha256':hashlib.sha256(data).hexdigest(),'loaded':selected,'live_defined':live};symbols[role]=raw
  emit('FONT_OWNER_HOST_SELECTED_MEMBER',{'role':role,**members[role]})
 write=[symbol for symbol in symbols['owner'] if 'WriteFontEnvironment' in symbol];read=[symbol for symbol in symbols['owner'] if 'ReadFontEnvironment' in symbol];assert len(write)==len(read)==1
 relocations={}
 for role,symbol,count in (('host_POSIX',write[0],2),('font_paths',read[0],1)):
  raw=subprocess.check_output([str(tools/'llvm-readobj'),'--relocations',compiled[role]['object']],text=True);selected=[line for line in raw.splitlines() if symbol in line];emit('FONT_OWNER_HOST_CALLER_RELOCATIONS',{'role':role,'symbol':symbol,'rows':selected});assert len(selected)>=count and all('REL32' in line for line in selected);relocations[role]=selected
 imports=subprocess.check_output([str(tools/'llvm-readobj'),'--coff-imports',str(build/'font_environment_test.exe')],text=True)
 emit('FONT_OWNER_HOST_IMPORTS_RAW',imports)
 assert not re.search(r'(?i)(jvm|dalvik|libart|JNI_OnLoad|FontEnvironment)',imports)
 assert all(name in imports for name in ('GetThreadWaitChain','OpenThreadWaitChainSession','CloseThreadWaitChainSession','AdjustTokenPrivileges'))
 shutil.copy2(build/'font_environment_test.exe',root/FILES[0]);shutil.copy2(build/'font_environment_test.map',root/FILES[1]);(root/FILES[2]).write_text(imports,encoding='utf-8');shutil.copy2('font-owner-host-build.log',root/FILES[3])
 record={'role':ROLE,'context':context(),'inputs':inputs(),'outputs':{p:sha(root/p) for p in FILES},'compiler':{'path':str(compiler),'sha256':sha(compiler),'version':subprocess.check_output([str(compiler),'--version'],text=True)},'linker':{'sha256':sha(tools/'ld.lld'),'version':subprocess.check_output([str(tools/'ld.lld'),'--version'],text=True)},'compiled_objects':compiled,'fixture_entry':fixture_entry,'selected_owner_members':members,'actual_link_command':command,'caller_relocations':relocations,'canonical_pthread':canonical,'pthread_link_proof':pthread,'web3_query_tests':'OFF','exceptions':'OFF','runtime_shim_libcore_owner_qualified':False,'font_provider_OEM_caller_qualified':False,'font_fallback_qualified':False,'generic_getenv_qualified':False}
 (root/'font-owner-host-provenance.json').write_text(json.dumps(record,sort_keys=True)+'\n',encoding='utf-8');log(record,root)

def log(record,root):
 for key,value in record.items():emit('FONT_OWNER_HOST_'+key.upper(),value)
 emit('FONT_OWNER_HOST_MANIFEST',{'role':ROLE,'context':record['context'],'sha256':sha(root/'font-owner-host-provenance.json')})
def verify(root):
 assert all(not p.is_symlink() for p in root.rglob('*'))
 actual={p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()};assert actual==set(FILES)|{'font-owner-host-provenance.json'}
 record=json.loads((root/'font-owner-host-provenance.json').read_text(encoding='utf-8'));assert record['role']==ROLE and record['context']==context() and record['inputs']==inputs() and record['outputs']=={p:sha(root/p) for p in FILES} and record['compiler']['sha256']==COMPILER_SHA
 assert record['fixture_entry']['symbol']=='main' and record['fixture_entry']['object']==record['compiled_objects']['fixture']['object'] and record['fixture_entry']['rva']>0
 assert record['exceptions']=='OFF' and record['web3_query_tests']=='OFF' and all(record[key] is False for key in ('runtime_shim_libcore_owner_qualified','font_provider_OEM_caller_qualified','font_fallback_qualified','generic_getenv_qualified'))
 assert (root/FILES[0]).read_bytes()[:2]==b'MZ' and {x['sha256'] for x in record['canonical_pthread'].values()}=={PTHREAD_SHA}
 log(record,root);return record
if __name__=='__main__':
 parser=argparse.ArgumentParser();parser.add_argument('mode',choices=('capture','verify'));parser.add_argument('--root',type=Path,required=True);parser.add_argument('--build',type=Path,default=Path('out'));args=parser.parse_args();assert os.environ.get('GITHUB_ACTIONS')=='true','Native binary handling CI only'
 if args.mode=='capture':capture(args.build,args.root)
 else:verify(args.root)
