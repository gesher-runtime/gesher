"""CI-only static-DLL registration versus alias publication contention gate."""
from pathlib import Path
import difflib,re,hashlib,json,subprocess,sys
repo=Path(__file__).resolve().parents[2]; src=Path(sys.argv[1])/'mingw-w64-libraries/winpthreads/src/thread.c'; p=repo/'tools/host-pthread-authority/supplier.patch'; base=subprocess.check_output(['git','show','58196b2e:tools/host-pthread-authority/supplier.patch'],cwd=repo); assert p.read_bytes()==base
old=src.read_text(); new=old; needle='static struct gesher_alias_probe_node *gesher_alias_probe_nodes;\n'; hook='''static HANDLE gesher_alias_gate_entered, gesher_alias_gate_release, gesher_alias_gate_contended;
__declspec(dllexport) void __cdecl gesher_alias_gate_v1(void *entered, void *release, void *contended)
{ gesher_alias_gate_entered=entered; gesher_alias_gate_release=release; gesher_alias_gate_contended=contended; }
'''; assert new.count(needle)==1; new=new.replace(needle,needle+hook)
needle='''  AcquireSRWLockExclusive(&gesher_alias_probe_lock);
  struct gesher_alias_probe_node *it;'''; repl='''  if (gesher_alias_gate_contended && !TryAcquireSRWLockExclusive(&gesher_alias_probe_lock)) {
    if (!SetEvent(gesher_alias_gate_contended)) abort();
    AcquireSRWLockExclusive(&gesher_alias_probe_lock);
  } else if (!gesher_alias_gate_contended) AcquireSRWLockExclusive(&gesher_alias_probe_lock);
  struct gesher_alias_probe_node *it;'''; assert new.count(needle)==1; new=new.replace(needle,repl)
needle='''  AcquireSRWLockExclusive(&gesher_alias_probe_lock);
  _pthread_key_dest=table;'''; repl='''  AcquireSRWLockExclusive(&gesher_alias_probe_lock);
  if (gesher_alias_gate_entered && (!SetEvent(gesher_alias_gate_entered) || WaitForSingleObject(gesher_alias_gate_release,5000)!=WAIT_OBJECT_0)) abort();
  _pthread_key_dest=table;'''; assert new.count(needle)==1; new=new.replace(needle,repl)
rel='mingw-w64-libraries/winpthreads/src/thread.c'; raw=list(difflib.unified_diff(old.splitlines(keepends=True),new.splitlines(keepends=True),fromfile='a/'+rel,tofile='b/'+rel,n=2)); out=raw[:2]; i=2
while i<len(raw):
 m=re.match(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)\n',raw[i]); assert m; a,ac,b,bc=int(m[1]),int(m[2] or 1),int(m[3]),int(m[4] or 1); tail=m[5]; i+=1; body=[]
 while i<len(raw) and not raw[i].startswith('@@ '): body.append(raw[i]); i+=1
 while body and body[0].startswith(' \t'): body.pop(0); a+=1; b+=1; ac-=1; bc-=1
 while body and body[-1].startswith(' \t'): body.pop(); ac-=1; bc-=1
 body=['\n' if x==' \n' else x for x in body]; out.append(f'@@ -{a},{ac} +{b},{bc} @@{tail}\n'); out.extend(body)
p.write_bytes(base+''.join(out).encode()); pin=repo/'tools/host-pthread-authority/pinned-inputs.json'; j=json.loads(pin.read_text()); j['patch_sha256']=hashlib.sha256(p.read_bytes()).hexdigest(); pin.write_bytes((json.dumps(j,indent=2)+'\n').encode()); print('generated alias load contention gate')
