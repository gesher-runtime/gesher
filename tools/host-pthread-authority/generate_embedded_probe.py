"""CI-only static-archive-in-DLL routing experiment; not product admission."""
from pathlib import Path
import difflib,hashlib,json,re,subprocess,sys
repo=Path(__file__).resolve().parents[2]; src=Path(sys.argv[1])/'mingw-w64-libraries/winpthreads/src'; patchfile=repo/'tools/host-pthread-authority/supplier.patch'; base=subprocess.check_output(['git','show','22842be6:tools/host-pthread-authority/supplier.patch'],cwd=repo); assert patchfile.read_bytes()==base
files=['thread.c','sched.c','mutex.c','cond.c','rwlock.c','barrier.c','spinlock.c','sem.c','gesher_sync_probe.h']; patch=''
def delta(old,new,rel):
 raw=list(difflib.unified_diff(old.splitlines(keepends=True),new.splitlines(keepends=True),fromfile='a/'+rel,tofile='b/'+rel,n=2)); out=raw[:2]; i=2
 while i<len(raw):
  m=re.match(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)\n',raw[i]); assert m; a,ac,b,bc=int(m[1]),int(m[2] or 1),int(m[3]),int(m[4] or 1); tail=m[5]; i+=1; body=[]
  while i<len(raw) and not raw[i].startswith('@@ '): body.append(raw[i]); i+=1
  while body and body[0].startswith(' \t'): body.pop(0); a+=1; b+=1; ac-=1; bc-=1
  while body and body[-1].startswith(' \t'): body.pop(); ac-=1; bc-=1
  body=['\n' if x==' \n' else x for x in body]; out.append(f'@@ -{a},{ac} +{b},{bc} @@{tail}\n'); out.extend(body)
 return ''.join(out)
for name in files:
 old=(src/name).read_text(); new=old.replace('#if defined(GESHER_PTHREAD_ROUTING_PROBE) && defined(DLL_EXPORT)','#ifdef GESHER_PTHREAD_ROUTING_PROBE')
 if name=='gesher_sync_probe.h':
  new=new.replace('#ifdef DLL_EXPORT\nconst struct gesher_sync_probe_v1 *gesher_sync_probe_owner(void);\n#endif','const struct gesher_sync_probe_v1 *gesher_sync_probe_owner(void);')
 if name=='thread.c':
  anchor='void (**_pthread_key_dest)(void *) = NULL;'
  assert new.count(anchor)==1
  new=new.replace(anchor,'#ifdef GESHER_PTHREAD_ROUTING_PROBE\n__declspec(dllexport)\n#endif\n'+anchor)
  for marker in ('static const struct gesher_identity_probe_v6 *identity_probe_owner(void)','const struct gesher_sync_probe_v1 *gesher_sync_probe_owner(void)'):
   anchor='#ifdef DLL_EXPORT\n'+marker; assert new.count(anchor)==1,(name,marker); new=new.replace(anchor,'#if 1\n'+marker)
  anchor='''  HMODULE host=GetModuleHandleW(NULL), actual=NULL;
  get_fn get=host ? (get_fn)GetProcAddress(host,"gesher_pthread_identity_probe_v6") : NULL;'''
  repl='''  HMODULE host=GetModuleHandleW(NULL), actual=NULL, own=NULL;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCWSTR)identity_probe_owner,&own)) abort();
  if (own==host) return NULL;
  get_fn get=host ? (get_fn)GetProcAddress(host,"gesher_pthread_identity_probe_v6") : NULL;'''; assert new.count(anchor)==1; new=new.replace(anchor,repl)
  anchor='''  HMODULE host=GetModuleHandleW(NULL), actual=NULL;
  getter_fn getter=host?(getter_fn)GetProcAddress(host,"gesher_sync_probe_v1"):NULL;'''
  repl='''  HMODULE host=GetModuleHandleW(NULL), actual=NULL, own=NULL;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCWSTR)gesher_sync_probe_owner,&own)) abort();
  if (own==host) return NULL;
  getter_fn getter=host?(getter_fn)GetProcAddress(host,"gesher_sync_probe_v1"):NULL;'''; assert new.count(anchor)==1; new=new.replace(anchor,repl)
  anchor='''  pthread_spinlock_t new_spin_keys = PTHREAD_SPINLOCK_INITIALIZER;

  if (dwReason == DLL_PROCESS_DETACH)'''
  repl='''  pthread_spinlock_t new_spin_keys = PTHREAD_SPINLOCK_INITIALIZER;

#if defined(GESHER_PTHREAD_ROUTING_PROBE) && !defined(DLL_EXPORT)
  /* The archive may live in a DLL, not only in the process EXE. */
  if (dwReason == DLL_PROCESS_ATTACH) {
    const struct gesher_identity_probe_v6 *candidate=identity_probe_owner();
    if (candidate && candidate->register_alias(&gesher_dll_alias_node,&_pthread_key_dest)) abort();
    gesher_participating_owner=candidate;
  }
  if (gesher_participating_owner) {
    if (dwReason == DLL_PROCESS_DETACH) {
      if (gesher_participating_owner->unregister_alias(&gesher_dll_alias_node)) abort();
      gesher_participating_owner=NULL;
    }
    return;
  }
#endif
  if (dwReason == DLL_PROCESS_DETACH)'''; assert new.count(anchor)==1; new=new.replace(anchor,repl)
 if old!=new: patch+=delta(old,new,'mingw-w64-libraries/winpthreads/src/'+name)
header=Path(sys.argv[1])/'mingw-w64-libraries/winpthreads/include/pthread.h'; old=header.read_text(); needle='WINPTHREAD_API extern void (**_pthread_key_dest)(void *);'; assert old.count(needle)==1
new=old.replace(needle,'#ifdef GESHER_PTHREAD_ROUTING_PROBE\n__declspec(dllexport)\n#endif\n'+needle)
patch+=delta(old,new,'mingw-w64-libraries/winpthreads/include/pthread.h')
patchfile.write_bytes(base+patch.encode()); pin=repo/'tools/host-pthread-authority/pinned-inputs.json'; j=json.loads(pin.read_text()); j['patch_sha256']=hashlib.sha256(patchfile.read_bytes()).hexdigest(); pin.write_bytes((json.dumps(j,indent=2)+'\n').encode()); print('generated embedded-archive routing delta')
