"""WIP CI-only collection of actual diagnostic DLL source-object provenance."""
import hashlib,json,os,re,shlex,shutil,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent;sys.path.insert(0,str(HERE))
import minimum_receipt as minimum

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def main(source,build,work,out):
 assert os.environ.get('GITHUB_ACTIONS')=='true' and os.environ.get('GITHUB_JOB')=='diagnostic-producer'
 assert os.environ.get('GESHER_MINIMUM_JOIN_RUNTIME_AUTHORIZED_SOURCE')==os.environ.get('GITHUB_SHA')
 source,build,work,out=(Path(x).resolve(strict=True) for x in (source,build,work,out))
 log=(work/'build.log').read_text(encoding='utf-8');install=(work/'install.log').read_text(encoding='utf-8')
 names=minimum.CORE+('gesher_minimum_join_diag_configure','gesher_minimum_join_diag_snapshot')
 definitions=minimum.map_symbols((work/'supplier-dll.map').read_text(encoding='utf-8'),names)
 reading=set(re.findall(r'\bReading ([^\r\n]+\.(?:o|obj))\r?$',log+'\n'+install,re.M));objects={}
 for name,(owner,rva) in definitions.items():
  matches={str((Path(p) if Path(p).is_absolute() else build/p).resolve()) for p in reading if Path(p).name==Path(owner).name}
  assert len(matches)==1;path=Path(next(iter(matches)))
  assert path.is_file() and path.is_relative_to(build) and path.name.endswith('thread.o')
  objects[name]={'input':owner,'rva':rva,'actual_reading_path':str(path),'sha256':sha(path)}
 assert len({v['actual_reading_path'] for v in objects.values()})==1
 compiled=set();commands=[]
 for line in log.splitlines():
  if 'src/thread.c' not in line or '-DDLL_EXPORT' not in line:continue
  assert '-DGESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC' in line;tokens=shlex.split(line)
  if '-c' not in tokens or '-o' not in tokens:continue
  input_path=Path(tokens[tokens.index('-c')+1]);input_path=(input_path if input_path.is_absolute() else build/input_path).resolve()
  expected=(source/'mingw-w64-libraries/winpthreads/src/thread.c').resolve()
  assert input_path==expected
  output=Path(tokens[tokens.index('-o')+1]);compiled.add(str((output if output.is_absolute() else build/output).resolve()));commands.append(line)
 assert commands and {v['actual_reading_path'] for v in objects.values()}<=compiled
 path=Path(next(iter(objects.values()))['actual_reading_path']);shutil.copyfile(path,out/'dll-source-thread.o')
 assert sha(out/'dll-source-thread.o')==sha(path)
 value={'source':os.environ['GITHUB_SHA'],'run':os.environ['GITHUB_RUN_ID'],'attempt':os.environ['GITHUB_RUN_ATTEMPT'],
        'role':'join-wait-diagnostic-only-v1','build':str(build),'objects':objects,'compile_commands':commands,'qualification':False}
 (out/'dll-source-observations.json').write_text(json.dumps(value,sort_keys=True,indent=2)+'\n',encoding='utf-8')
 print('JOIN_RUNTIME_DLL_SOURCE_OBSERVATION '+json.dumps(value,sort_keys=True),flush=True)
if __name__=='__main__':
 assert len(sys.argv)==5;main(*sys.argv[1:])
