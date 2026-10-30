"""Actual GNU -Map and LLD Reading/Loaded proof; parsers are pure metadata."""
import hashlib,importlib.util,os,re,shlex,subprocess,tempfile
from pathlib import Path
from font_records import emit

def qualified_map():
 spec=importlib.util.spec_from_file_location('qualified_font_link_map',Path(__file__).with_name('verify-standalone-link.py'))
 module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def map_records(raw):
 # The qualified parser validates the actual writer/header/ranges first.
 expected=qualified_map().map_rows(raw);records=[];owner=None
 for line in raw.splitlines()[1:]:
  if not line.strip():continue
  match=re.fullmatch(r'([0-9a-fA-F]{8,16}) ([0-9a-fA-F]{8,16})\s+([0-9]+) (.*)',line)
  body=match[4];indent=len(body)-len(body.lstrip(' '));text=body[indent:]
  if indent==0:owner=None
  elif indent==8:owner=text.rpartition(':(')[0]
  elif indent==16:records.append((int(match[1],16),text,owner))
 assert [(symbol,origin) for _,symbol,origin in records]==expected
 return records

def entry(records,symbol,object_file,build):
 found=[(rva,name,origin) for rva,name,origin in records if name==symbol]
 for row in found:emit('FONT_CLI_ENTRY_RAW',{'symbol':symbol,'rva':row[0],'owner':row[2]})
 emit('FONT_CLI_ENTRY_EXPECTED',{'symbol':symbol,'object':str(object_file.resolve()),'rows':len(found)})
 normalized={(rva,name,str((build/Path(origin)).resolve())) for rva,name,origin in found}
 assert len(normalized)==1 and next(iter(normalized))[2]==str(object_file.resolve()),('Missing/conflicting live entry RVA/owner',symbol,sorted(normalized))
 result=next(iter(normalized));return {'symbol':symbol,'rva':result[0],'object':result[2],'identical_rows_coalesced':len(found)-1}

def link_scope(trace,command):
 statuses=[(i,m[1]) for i,line in enumerate(trace.splitlines()) if (m:=re.fullmatch(r'\[[0-9]+/[0-9]+\] (.*)',line))]
 matches=[i for i,row in statuses if row==command.strip()]
 emit('FONT_CLI_LINK_SCOPE_RAW',{'link_command_sha256':hashlib.sha256(command.strip().encode()).hexdigest(),'exact_status_matches':matches,'status_lines':len(statuses)})
 assert len(matches)==1 and not any(i>matches[0] for i,_ in statuses),'Target link trace is absent/ambiguous/not the final requested command'
 return '\n'.join(trace.splitlines()[matches[0]+1:])

def selected_archives(scope,canonical,build):
 reading=[];loaded=[]
 for line in scope.splitlines():
  if re.search(r'\b(?:Reading|Loaded) .*lib(?:win)?pthread',line):emit('FONT_CLI_PTHREAD_RAW',line)
  match=re.fullmatch(r'(?:ld\.lld|lld-link): Reading (.+\.a)\s*',line)
  if match:reading.append(match[1].rstrip())
  match=re.fullmatch(r'(?:ld\.lld|lld-link): Loaded ([^()]+\.a)\(([^()]+)\) for (\S+)\s*',line)
  if match:loaded.append((match[1],match[2],match[3]))
 archives={}
 for value in reading:
  path=Path(os.path.abspath(build/value));assert path.is_file(),('Opened archive unavailable',value)
  archives.setdefault(path.name,set()).add(path)
 assert all(len(paths)==1 for paths in archives.values()),'Duplicate opened archive basenames'
 archives={name:next(iter(paths)) for name,paths in archives.items()}
 selected=[row for row in loaded if row[0] in canonical]
 assert selected,'No actual static pthread member loaded'
 for alias in set(archives)&set(canonical):
  assert str(archives[alias])==canonical[alias]['path'] and str(archives[alias].resolve())==canonical[alias]['resolved_path'],'Selected noncanonical/private archive origin'
  assert hashlib.sha256(archives[alias].read_bytes()).hexdigest()==canonical[alias]['sha256'],'Selected archive bytes differ'
 assert {row[0] for row in selected}<=set(archives),'Loaded pthread alias was never actually opened'
 return archives,sorted(set(selected))

def prove_pthread(records,trace,command,canonical,build,tools,objects):
 scope=link_scope(trace,command);archives,selected=selected_archives(scope,canonical,build)
 inventories={path:subprocess.check_output([str(tools/'llvm-ar'),'t',str(path)],text=True).splitlines() for path in set(archives.values())}
 canonical_resolved={Path(item['resolved_path']) for item in canonical.values()}
 proof=[]
 for alias,member,required in selected:
  archive=archives[alias];names=inventories[archive]
  emit('FONT_CLI_PTHREAD_MEMBER_INVENTORY_RAW',{'archive':str(archive),'member':member,'exact_member_count':names.count(member)})
  assert names.count(member)==1,'Ambiguous selected member'
  candidates=[path for path,items in inventories.items() for name in items if Path(name).name==Path(member).name]
  emit('FONT_CLI_PTHREAD_MEMBER_ORIGINS_RAW',{'member':member,'actual_archive_origins':[str(path) for path in candidates],'canonical_origins':sorted(str(path) for path in canonical_resolved),'direct_object_collisions':[str(path) for path in objects if path.name==Path(member).name]})
  assert candidates and all(path.resolve() in canonical_resolved for path in candidates),'Selected member basename collides with a foreign archive'
  assert not any(path.name==Path(member).name for path in objects),'Selected member basename collides with a direct object'
  data=subprocess.check_output([str(tools/'llvm-ar'),'p',str(archive),member]);assert data
  with tempfile.TemporaryDirectory(prefix='font-cli-selected-member-') as temporary:
   file=Path(temporary)/'member.o';file.write_bytes(data)
   raw=set(subprocess.check_output([str(tools/'llvm-nm'),'-j','--defined-only',str(file)],text=True).splitlines())
   defined=set(subprocess.check_output([str(tools/'llvm-nm'),'-j','--demangle','--defined-only',str(file)],text=True).splitlines())
  mapped=sorted(set((rva,symbol,origin) for rva,symbol,origin in records if origin==member and symbol in defined))
  emit('FONT_CLI_PTHREAD_MEMBER_RAW',{'archive':str(archive),'member':member,'required':required,'required_defined':required in raw,'mapped_definition_count':len(mapped)})
  assert required in raw and mapped,'Selected member lacks actual defined/live-map evidence'
  record={'archive':str(archive),'archive_sha256':canonical[alias]['sha256'],'member':member,'member_sha256':hashlib.sha256(data).hexdigest(),'loaded_for':required,'live_defined_map_rows':mapped}
  proof.append(record)
 return {'actual_read_archives':{name:str(path) for name,path in archives.items()},'selected_members':proof}
