"""Pure actual-map/verbose-format controls; tool calls are mocked, no native execution."""
import contextlib,hashlib,importlib.util,io,json,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
from font_records import emit

def load(name):
 spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(name+'.py'));module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
proof=load('cli-link-evidence');golden=load('test-standalone-link')
SYMBOL='gesher::bionic::(anonymous namespace)::OpenIterator()'
OWNER='CMakeFiles/gesher_native_fonts.dir/compat/bionic/native_fonts.cc.obj'
COMMAND=': && clang++ object.obj -static -Wl,--verbose -o dalvikvm.exe && :'

class MapEvidence(unittest.TestCase):
 def test_actual_golden_and_identical_triples(self):
  self.assertEqual(hashlib.sha256(golden.GOLDEN.encode()).hexdigest(),golden.GOLDEN_SHA256)
  rows=proof.map_records(golden.GOLDEN);self.assertEqual(len(rows),27)
  symbol=next(name for _,name,owner in rows if owner=='MinikinSkia.cpp.obj')
  actual=proof.entry(rows,symbol,Path.cwd()/'MinikinSkia.cpp.obj',Path.cwd())
  self.assertEqual(actual['rva'],0xaa620);self.assertEqual(actual['identical_rows_coalesced'],2)
  original=next(row for row in rows if row[1]==SYMBOL)
  result=proof.entry(rows+[original],SYMBOL,Path.cwd()/OWNER,Path.cwd())
  self.assertEqual(result['identical_rows_coalesced'],1)
  normalized=[(rva,name,owner.replace('CMakeFiles/','CMakeFiles/../CMakeFiles/',1)) for rva,name,owner in rows]
  self.assertEqual(proof.entry(normalized,SYMBOL,Path.cwd()/OWNER,Path.cwd())['rva'],0x1470)
 def test_missing_owner_address_and_basename_conflicts(self):
  rows=proof.map_records(golden.GOLDEN);original=next(row for row in rows if row[1]==SYMBOL)
  variants=[[],rows+[(original[0]+1,SYMBOL,OWNER)],rows+[(original[0],SYMBOL,'other/'+OWNER)],[(rva,name,'native_fonts.cc.obj' if name==SYMBOL else owner) for rva,name,owner in rows]]
  for variant in variants:
   with self.subTest(variant=variant[-1:] if variant else []),self.assertRaises(AssertionError):proof.entry(variant,SYMBOL,Path.cwd()/OWNER,Path.cwd())
 def test_real_writer_schema_and_ranges_retained(self):
  for text in (golden.GOLDEN.replace('00001470 00004416','00001470 00000001'),'Address Publics by Value Rva+Base Lib:Object\n'):
   with self.assertRaises(AssertionError):proof.map_records(text)

class VerboseEvidence(unittest.TestCase):
 def setUp(self):
  self.temporary=tempfile.TemporaryDirectory(prefix='font-cli-link-metadata-');self.addCleanup(self.temporary.cleanup)
  self.root=Path(self.temporary.name);self.build=self.root/'build';self.build.mkdir()
  self.canonical={}
  for alias in ('libpthread.a','libwinpthread.a'):
   file=self.root/'canonical'/alias;file.parent.mkdir(exist_ok=True);file.write_bytes(b'NONEXECUTABLE_ARCHIVE_METADATA_ONLY')
   self.canonical[alias]={'path':str(file),'resolved_path':str(file.resolve()),'sha256':hashlib.sha256(file.read_bytes()).hexdigest()}
  # Reading/Loaded spelling and member name are actual410884 publisher rows;
  # only absolute paths are replaced with these synthetic metadata fixtures.
  self.scope='\n'.join(['ld.lld: Reading '+self.canonical['libwinpthread.a']['path'],'ld.lld: Reading '+self.canonical['libpthread.a']['path'],'ld.lld: Reading libwinpthread.a(libwinpthread_la-thread.o)','ld.lld: Loaded libwinpthread.a(libwinpthread_la-thread.o) for pthread_create'])
  self.trace='[1/2] compile-metadata-only\n[2/2] '+COMMAND+'\n'+self.scope
 def test_exact_command_scope_and_selected_origin(self):
  self.assertEqual(proof.link_scope(self.trace,COMMAND),self.scope)
  opened,loaded=proof.selected_archives(self.scope,self.canonical,self.build)
  self.assertEqual(len(opened),2);self.assertEqual(loaded,[('libwinpthread.a','libwinpthread_la-thread.o','pthread_create')])
 def test_missing_duplicate_wrong_scope_and_private_archive(self):
  for trace in ('missing',self.trace+'\n[3/3] other-link',self.trace+'\n[3/3] '+COMMAND):
   with self.assertRaises(AssertionError):proof.link_scope(trace,COMMAND)
  private=self.root/'private'/'libwinpthread.a';private.parent.mkdir();private.write_bytes(b'PRIVATE_METADATA_ONLY')
  for scope in (self.scope.replace('Loaded','NotLoaded'),self.scope.replace(self.canonical['libwinpthread.a']['path'],str(private)),self.scope+'\nld.lld: Reading '+str(private),self.scope.replace('ld.lld: Reading '+self.canonical['libwinpthread.a']['path']+'\n','')):
   with self.assertRaises(AssertionError):proof.selected_archives(scope,self.canonical,self.build)
 def fake_tools(self,args,**kwargs):
  if Path(args[0]).name=='llvm-ar' and args[1]=='t':return 'libwinpthread_la-thread.o\n'
  if Path(args[0]).name=='llvm-ar' and args[1]=='p':return b'NONEXECUTABLE_MEMBER_METADATA_ONLY'
  if Path(args[0]).name=='llvm-nm':return 'pthread_create\npthread_mutex_lock\n'
  raise AssertionError('Unexpected metadata tool request '+repr(args))
 def test_member_bytes_definition_and_live_map_binding(self):
  rows=[(0x1000,'pthread_create','libwinpthread_la-thread.o')]
  with patch.object(proof.subprocess,'check_output',side_effect=self.fake_tools):
   result=proof.prove_pthread(rows,self.trace,COMMAND,self.canonical,self.build,self.root/'NEVER_EXECUTED_TOOLS',set())
   self.assertEqual(result['selected_members'][0]['loaded_for'],'pthread_create')
   self.assertEqual(result['selected_members'][0]['member_sha256'],hashlib.sha256(b'NONEXECUTABLE_MEMBER_METADATA_ONLY').hexdigest())
   for invalid in ([],[(0x1000,'foreign_symbol','libwinpthread_la-thread.o')],[(0x1000,'pthread_create','foreign.o')]):
    with self.assertRaises(AssertionError):proof.prove_pthread(invalid,self.trace,COMMAND,self.canonical,self.build,self.root,set())
   with self.assertRaises(AssertionError):proof.prove_pthread(rows,self.trace.replace('for pthread_create','for missing_definition'),COMMAND,self.canonical,self.build,self.root,set())
   with self.assertRaises(AssertionError):proof.prove_pthread(rows,self.trace,COMMAND,self.canonical,self.build,self.root,{self.root/'libwinpthread_la-thread.o'})
   other=self.build/'libforeign.a';other.write_bytes(b'FOREIGN_METADATA_ONLY')
   with self.assertRaises(AssertionError):proof.prove_pthread(rows,self.trace+'\nld.lld: Reading '+str(other),COMMAND,self.canonical,self.build,self.root,set())
 def test_bounded_reconstructible_record(self):
  value={'command':'\\"'+('אב😀"\\'*3000)};capture=io.StringIO()
  with contextlib.redirect_stdout(capture):emit('FONT_METADATA_LOG',value)
  lines=capture.getvalue().splitlines();self.assertTrue(all(len(line)<=1000 for line in lines))
  rows=[json.loads(line.split(' ',1)[1]) for line in lines];self.assertEqual([row['part'] for row in rows],list(range(1,len(rows)+1)))
  payload=''.join(row['payload'] for row in rows);self.assertEqual(json.loads(payload),value)
  self.assertTrue(all(row['record_sha256']==hashlib.sha256(payload.encode()).hexdigest() for row in rows))

if __name__=='__main__':
 with contextlib.redirect_stdout(io.StringIO()):result=unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromModule(__import__(__name__)))
 assert result.wasSuccessful()
 print('FONT_CLI_LINK_METADATA_PASS tests='+str(result.testsRun)+' actual_golden=RETAINED identical_triples_only=COALESCED private_missing_conflicting_evidence=REJECTED native_processes_started=0')
