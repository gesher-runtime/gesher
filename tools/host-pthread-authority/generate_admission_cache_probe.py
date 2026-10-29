"""Generate one attach-time probe admission cache; no lazy loader calls."""
from pathlib import Path
import difflib,re,hashlib,json,subprocess,sys
repo=Path(__file__).resolve().parents[2]; src=Path(sys.argv[1])/'mingw-w64-libraries/winpthreads/src/thread.c'; p=repo/'tools/host-pthread-authority/supplier.patch'; base=subprocess.check_output(['git','show','eec80c5c:tools/host-pthread-authority/supplier.patch'],cwd=repo); assert p.read_bytes()==base
old=src.read_text(); new=old
needle='#if 1\nstatic const struct gesher_identity_probe_v6 *identity_probe_owner(void)\n'; repl='''#if 1
/* Single shared admission state for identity and sync wrappers. */
static volatile LONG gesher_admission_state;
static volatile LONG gesher_discovery_count;
static const struct gesher_identity_probe_v6 *gesher_identity_cached;
static const struct gesher_sync_probe_v1 *gesher_sync_cached;
__declspec(dllexport) int __cdecl gesher_admission_probe_v1(LONG *mode, LONG *discoveries)
{
  if (!mode || !discoveries) return EINVAL;
  *mode=InterlockedCompareExchange(&gesher_admission_state,0,0);
  *discoveries=InterlockedCompareExchange(&gesher_discovery_count,0,0);
  return 0;
}
static const struct gesher_identity_probe_v6 *identity_probe_owner(void)
{
  LONG state=InterlockedCompareExchange(&gesher_admission_state,0,0);
  if (state!=1 && state!=2) abort();
  return gesher_identity_cached;
}
static const struct gesher_identity_probe_v6 *identity_probe_discover(void)
'''; assert new.count(needle)==1; new=new.replace(needle,repl); new=new.replace('(LPCWSTR)identity_probe_owner,&own)', '(LPCWSTR)identity_probe_discover,&own)')
needle='''static const struct gesher_identity_probe_v6 *identity_probe_discover(void)
{
'''; assert new.count(needle)==1; new=new.replace(needle,needle+'  InterlockedIncrement(&gesher_discovery_count);\n')
needle='#if 1\nconst struct gesher_sync_probe_v1 *gesher_sync_probe_owner(void)\n'; repl='#if 1\nstatic const struct gesher_sync_probe_v1 *gesher_sync_probe_discover(void)\n'; assert new.count(needle)==1; new=new.replace(needle,repl); new=new.replace('(LPCWSTR)gesher_sync_probe_owner,&own)', '(LPCWSTR)gesher_sync_probe_discover,&own)')
needle='''static const struct gesher_sync_probe_v1 *gesher_sync_probe_discover(void)
{
'''; assert new.count(needle)==1; new=new.replace(needle,needle+'  InterlockedIncrement(&gesher_discovery_count);\n')
needle='''  return table;
}
#endif
#endif

/* Hook for TLS-based deregistration/registration of thread.  */'''
repl='''  return table;
}
const struct gesher_sync_probe_v1 *gesher_sync_probe_owner(void)
{
  LONG state=InterlockedCompareExchange(&gesher_admission_state,0,0);
  if (state!=1 && state!=2) abort();
  return gesher_sync_cached;
}
/* No Win32 loader lookup after the alias publication lock is taken. */
static int gesher_owner_admit(void)
{
  const struct gesher_identity_probe_v6 *identity=identity_probe_discover();
  const struct gesher_sync_probe_v1 *sync=gesher_sync_probe_discover();
  if (InterlockedCompareExchange(&gesher_admission_state,0,0)!=0 || (!!identity != !!sync)) abort();
  if (identity && identity->register_alias(&gesher_dll_alias_node,&_pthread_key_dest)) return EINVAL;
  gesher_identity_cached=identity; gesher_sync_cached=sync; gesher_participating_owner=identity;
  InterlockedExchange(&gesher_admission_state,identity?2:1);
  return 0;
}
#endif
#endif

/* Hook for TLS-based deregistration/registration of thread.  */'''; assert new.count(needle)==1; new=new.replace(needle,repl)
needle='''  if (dwReason == DLL_PROCESS_ATTACH) {
    const struct gesher_identity_probe_v6 *candidate=identity_probe_owner();
    if (candidate && candidate->register_alias(&gesher_dll_alias_node,&_pthread_key_dest)) abort();
    gesher_participating_owner=candidate;
  }'''; repl='''  if (dwReason == DLL_PROCESS_ATTACH && gesher_owner_admit()) abort();'''; assert new.count(needle)==1; new=new.replace(needle,repl)
needle='''  if (dwReason == DLL_PROCESS_ATTACH) {
    const struct gesher_identity_probe_v6 *candidate=identity_probe_owner();
    if (candidate && candidate->register_alias(&gesher_dll_alias_node, &_pthread_key_dest)) return FALSE;
    gesher_participating_owner=candidate;
  }'''; repl='''  if (dwReason == DLL_PROCESS_ATTACH && gesher_owner_admit()) return FALSE;'''; assert new.count(needle)==1; new=new.replace(needle,repl)
needle='''      gesher_participating_owner=NULL;
'''; assert new.count(needle)==2; new=new.replace(needle,needle+'      InterlockedExchange(&gesher_admission_state,-1);\n')
rel='mingw-w64-libraries/winpthreads/src/thread.c'; raw=list(difflib.unified_diff(old.splitlines(keepends=True),new.splitlines(keepends=True),fromfile='a/'+rel,tofile='b/'+rel,n=2)); out=raw[:2]; i=2
while i<len(raw):
 m=re.match(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)\n',raw[i]); assert m; a,ac,b,bc=int(m[1]),int(m[2] or 1),int(m[3]),int(m[4] or 1); tail=m[5]; i+=1; body=[]
 while i<len(raw) and not raw[i].startswith('@@ '): body.append(raw[i]); i+=1
 while body and body[0].startswith(' \t'): body.pop(0); a+=1; b+=1; ac-=1; bc-=1
 while body and body[-1].startswith(' \t'): body.pop(); ac-=1; bc-=1
 body=['\n' if x==' \n' else x for x in body]; out.append(f'@@ -{a},{ac} +{b},{bc} @@{tail}\n'); out.extend(body)
p.write_bytes(base+''.join(out).encode()); pin=repo/'tools/host-pthread-authority/pinned-inputs.json'; j=json.loads(pin.read_text()); j['patch_sha256']=hashlib.sha256(p.read_bytes()).hexdigest(); pin.write_bytes((json.dumps(j,indent=2)+'\n').encode()); print('generated one shared attach-time admission cache')
