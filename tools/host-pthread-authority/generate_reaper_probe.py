"""Generate opt-in process-owned callback-release reaper experiment."""
from pathlib import Path
import difflib,re,hashlib,json,subprocess,sys
repo=Path(__file__).resolve().parents[2]; src=Path(sys.argv[1])/'mingw-w64-libraries/winpthreads/src/thread.c'; p=repo/'tools/host-pthread-authority/supplier.patch'; base=subprocess.check_output(['git','show','dea48139:tools/host-pthread-authority/supplier.patch'],cwd=repo); assert p.read_bytes()==base
old=src.read_text(); new=old
needle='''struct gesher_key_generation {
  unsigned long long id;'''; repl='''struct gesher_key_generation {
#ifdef GESHER_REAPER_PROBE
  SLIST_ENTRY reaper_entry;
#endif
  unsigned long long id;'''; assert new.count(needle)==1; new=new.replace(needle,repl)
needle='''static struct gesher_key_generation *gesher_key_generations[PTHREAD_KEYS_MAX];'''
reaper='''#ifdef GESHER_REAPER_PROBE
/* Diagnostic only: bootstrap from normal host code before callback-key use. */
static SLIST_HEADER gesher_reaper_queue;
static HANDLE gesher_reaper_wake, gesher_reaper_thread, gesher_reaper_done;
static volatile LONG gesher_reaper_state, gesher_reaper_records, gesher_reaper_fail;
static DWORD WINAPI gesher_reaper_main(void *unused)
{
  (void)unused;
  for (;;) {
    if (WaitForSingleObject(gesher_reaper_wake,INFINITE)!=WAIT_OBJECT_0) abort();
    PSLIST_ENTRY entry;
    while ((entry=InterlockedPopEntrySList(&gesher_reaper_queue))!=NULL) {
      struct gesher_key_generation *record=CONTAINING_RECORD(entry,struct gesher_key_generation,reaper_entry);
      if (!record->module || !FreeLibrary(record->module)) abort();
      _aligned_free(record); InterlockedDecrement(&gesher_reaper_records);
      if (gesher_reaper_done && !SetEvent(gesher_reaper_done)) abort();
    }
    if (InterlockedCompareExchange(&gesher_reaper_state,0,0)==3) return 0;
  }
}
__declspec(dllexport) int __cdecl gesher_reaper_fault_v1(int stage)
{ if (stage<0 || stage>2 || InterlockedCompareExchange(&gesher_reaper_state,0,0)!=0) return EINVAL; gesher_reaper_fail=stage; return 0; }
__declspec(dllexport) int __cdecl gesher_reaper_start_v1(void)
{
  if (InterlockedCompareExchange(&gesher_reaper_state,1,0)!=0) return EBUSY;
  InitializeSListHead(&gesher_reaper_queue);
  gesher_reaper_wake=gesher_reaper_fail==1?NULL:CreateEventW(NULL,FALSE,FALSE,NULL);
  if (!gesher_reaper_wake) { InterlockedExchange(&gesher_reaper_state,0); return EAGAIN; }
  gesher_reaper_thread=gesher_reaper_fail==2?NULL:CreateThread(NULL,0,gesher_reaper_main,NULL,0,NULL);
  if (!gesher_reaper_thread) { if (!CloseHandle(gesher_reaper_wake)) abort(); gesher_reaper_wake=NULL; InterlockedExchange(&gesher_reaper_state,0); return EAGAIN; }
  InterlockedExchange(&gesher_reaper_state,2); return 0;
}
__declspec(dllexport) int __cdecl gesher_reaper_observe_v1(void *done, DWORD *tid, LONG *records)
{
  if (!tid || !records || InterlockedCompareExchange(&gesher_reaper_state,0,0)!=2) return EINVAL;
  gesher_reaper_done=(HANDLE)done; *tid=GetThreadId(gesher_reaper_thread);
  *records=InterlockedCompareExchange(&gesher_reaper_records,0,0); return *tid?0:EAGAIN;
}
__declspec(dllexport) int __cdecl gesher_reaper_stop_v1(void)
{
  /* Test contract: no concurrent producers or outstanding records. */
  if (InterlockedCompareExchange(&gesher_reaper_records,0,0)!=0) return EBUSY;
  if (InterlockedCompareExchange(&gesher_reaper_state,3,2)!=2) return EINVAL;
  if (!SetEvent(gesher_reaper_wake) || WaitForSingleObject(gesher_reaper_thread,5000)!=WAIT_OBJECT_0) abort();
  if (!CloseHandle(gesher_reaper_thread) || !CloseHandle(gesher_reaper_wake)) abort();
  gesher_reaper_thread=NULL; gesher_reaper_wake=NULL; gesher_reaper_done=NULL;
  InterlockedExchange(&gesher_reaper_state,4); return 0;
}
#endif
'''; assert new.count(needle)==1; new=new.replace(needle,reaper+needle)
needle='''  struct gesher_key_generation *record=calloc(1,sizeof(*record));
  if (!record) return NULL;'''; repl='''#ifdef GESHER_REAPER_PROBE
  struct gesher_key_generation *record=_aligned_malloc(sizeof(*record),MEMORY_ALLOCATION_ALIGNMENT);
  if (record) { memset(record,0,sizeof(*record)); InterlockedIncrement(&gesher_reaper_records); }
#else
  struct gesher_key_generation *record=calloc(1,sizeof(*record));
#endif
  if (!record) return NULL;'''; assert new.count(needle)==1; new=new.replace(needle,repl)
needle='''    HMODULE module=record->module;
    free(record);
    if (module && !FreeLibrary(module)) abort();'''; repl='''    HMODULE module=record->module;
#ifdef GESHER_REAPER_PROBE
    if (module) {
      InterlockedPushEntrySList(&gesher_reaper_queue,&record->reaper_entry);
      if (!SetEvent(gesher_reaper_wake)) abort();
    } else { _aligned_free(record); InterlockedDecrement(&gesher_reaper_records); }
#else
    free(record);
    if (module && !FreeLibrary(module)) abort();
#endif'''; assert new.count(needle)==1; new=new.replace(needle,repl)
needle='''\tif (!key)
\t\treturn EINVAL;
#ifdef GESHER_PTHREAD_ROUTING_PROBE
        struct gesher_key_generation *record=gesher_key_new(dest);'''; repl='''\tif (!key)
\t\treturn EINVAL;
#ifdef GESHER_REAPER_PROBE
        if (InterlockedCompareExchange(&gesher_reaper_state,0,0)!=2) return EAGAIN;
#endif
#ifdef GESHER_PTHREAD_ROUTING_PROBE
        struct gesher_key_generation *record=gesher_key_new(dest);'''; assert new.count(needle)==1; new=new.replace(needle,repl)
rel='mingw-w64-libraries/winpthreads/src/thread.c'; raw=list(difflib.unified_diff(old.splitlines(keepends=True),new.splitlines(keepends=True),fromfile='a/'+rel,tofile='b/'+rel,n=2)); out=raw[:2]; i=2
while i<len(raw):
 m=re.match(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)\n',raw[i]); assert m; a,ac,b,bc=int(m[1]),int(m[2] or 1),int(m[3]),int(m[4] or 1); tail=m[5]; i+=1; body=[]
 while i<len(raw) and not raw[i].startswith('@@ '): body.append(raw[i]); i+=1
 while body and body[0].startswith(' \t'): body.pop(0); a+=1; b+=1; ac-=1; bc-=1
 while body and body[-1].startswith(' \t'): body.pop(); ac-=1; bc-=1
 body=['\n' if x==' \n' else x for x in body]; out.append(f'@@ -{a},{ac} +{b},{bc} @@{tail}\n'); out.extend(body)
p.write_bytes(base+''.join(out).encode()); pin=repo/'tools/host-pthread-authority/pinned-inputs.json'; j=json.loads(pin.read_text()); j['patch_sha256']=hashlib.sha256(p.read_bytes()).hexdigest(); pin.write_bytes((json.dumps(j,indent=2)+'\n').encode()); print('generated opt-in callback reaper')
