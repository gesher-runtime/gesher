"""Generate probe-only, bounded rwlock lazy-initialization contention gate."""
from pathlib import Path
import difflib,re,json,hashlib,subprocess,sys
repo=Path(__file__).resolve().parents[2]; src=Path(sys.argv[1])/'mingw-w64-libraries/winpthreads/src'; p=repo/'tools/host-pthread-authority/supplier.patch'; base=subprocess.check_output(['git','show','fd25b3d9:tools/host-pthread-authority/supplier.patch'],cwd=repo); assert p.read_bytes()==base
patch=''
for kind in ('rwlock',):
 old=(src/(kind+'.c')).read_text(); new=old
 marker='static pthread_spinlock_t cond_locked = PTHREAD_SPINLOCK_INITIALIZER;'
 assert new.count(marker)==1
 hook='''
#ifdef GESHER_PTHREAD_ROUTING_PROBE
static HANDLE gesher_'''+kind+'''_entered, gesher_'''+kind+'''_release, gesher_'''+kind+'''_contended;
__declspec(dllexport) void __cdecl gesher_'''+kind+'''_init_gate_v1(void *entered, void *release, void *contended)
{ gesher_'''+kind+'''_entered=entered; gesher_'''+kind+'''_release=release; gesher_'''+kind+'''_contended=contended; }
static void gesher_'''+kind+'''_init_lock(void)
{
  if (!gesher_'''+kind+'''_entered) { pthread_spin_lock(&cond_locked); return; }
  int r=pthread_spin_trylock(&cond_locked);
  if (r==EBUSY) {
    if (!SetEvent(gesher_'''+kind+'''_contended)) abort();
    pthread_spin_lock(&cond_locked);
  } else if (!r) {
    if (!SetEvent(gesher_'''+kind+'''_entered) || WaitForSingleObject(gesher_'''+kind+'''_release,5000)!=WAIT_OBJECT_0) abort();
  } else abort();
}
#endif
'''
 new=new.replace(marker,marker+hook)
 if kind=='cond':
  a='cond_static_init (pthread_cond_t *c)\n{\n  int r = 0;\n\n  pthread_spin_lock (&cond_locked);'; b='cond_static_init (pthread_cond_t *c)\n{\n  int r = 0;\n\n#ifdef GESHER_PTHREAD_ROUTING_PROBE\n  gesher_cond_init_lock();\n#else\n  pthread_spin_lock (&cond_locked);\n#endif'
 else:
  a='rwlock_static_init(pthread_rwlock_t *rw)\n{\n  int r;\n  pthread_spin_lock(&cond_locked);'; b='rwlock_static_init(pthread_rwlock_t *rw)\n{\n  int r;\n#ifdef GESHER_PTHREAD_ROUTING_PROBE\n  gesher_rwlock_init_lock();\n#else\n  pthread_spin_lock(&cond_locked);\n#endif'
 assert new.count(a)==1; new=new.replace(a,b)
 if kind=='rwlock':
  a='''  if (*rw != PTHREAD_RWLOCK_INITIALIZER)
  {
    pthread_spin_unlock(&cond_locked);
    return EINVAL;
  }'''; b='''  if (*rw != PTHREAD_RWLOCK_INITIALIZER)
  {
    pthread_spin_unlock(&cond_locked);
#ifdef GESHER_PTHREAD_ROUTING_PROBE
    /* Another caller completed initialization after our sentinel check. */
    return EBUSY;
#else
    return EINVAL;
#endif
  }'''; assert new.count(a)==1; new=new.replace(a,b)
 rel='mingw-w64-libraries/winpthreads/src/'+kind+'.c'; raw=list(difflib.unified_diff(old.splitlines(keepends=True),new.splitlines(keepends=True),fromfile='a/'+rel,tofile='b/'+rel,n=2)); out=raw[:2]; i=2
 while i<len(raw):
  m=re.match(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)\n',raw[i]); assert m; a,ac,b,bc=int(m[1]),int(m[2] or 1),int(m[3]),int(m[4] or 1); tail=m[5]; i+=1; body=[]
  while i<len(raw) and not raw[i].startswith('@@ '): body.append(raw[i]); i+=1
  while body and body[0].startswith(' \t'): body.pop(0); a+=1; b+=1; ac-=1; bc-=1
  while body and body[-1].startswith(' \t'): body.pop(); ac-=1; bc-=1
  body=['\n' if x==' \n' else x for x in body]; out.append(f'@@ -{a},{ac} +{b},{bc} @@{tail}\n'); out.extend(body)
 patch+=''.join(out)
p.write_bytes(base+patch.encode()); pin=repo/'tools/host-pthread-authority/pinned-inputs.json'; j=json.loads(pin.read_text()); j['patch_sha256']=hashlib.sha256(p.read_bytes()).hexdigest(); pin.write_bytes((json.dumps(j,indent=2)+'\n').encode()); print('generated rwlock initialization contention gate')
