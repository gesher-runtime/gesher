"""Generate a CI-only sync-family routing experiment from reviewed source.
Not a production ABI generator. Requires ctags and exact expanded v6 source.
"""
import difflib, hashlib, json, pathlib, re, subprocess, sys
repo=pathlib.Path(__file__).resolve().parents[2]; root=pathlib.Path(sys.argv[1]).resolve()
src=root/'mingw-w64-libraries/winpthreads/src'; files=['mutex.c','cond.c','rwlock.c','barrier.c','spinlock.c','sem.c']
tags=[json.loads(x) for x in subprocess.check_output(['ctags','--output-format=json','--fields=+nS','--kinds-C=+p','-o','-']+[str(src/n) for n in files],text=True).splitlines()]
tags=[t for t in tags if t.get('kind')=='function' and not t.get('file')]
tags.sort(key=lambda t:(pathlib.Path(t['path']).name,t['line'],t['name']))
assert len(tags)==75 and len({t['name'] for t in tags})==75
names=[t['name'] for t in tags]
assert 'do_sema_b_wait_intern' in names and 'sem_open' in names
header='''/* CI-only generated sync dispatch; no production enablement. */
#ifndef GESHER_SYNC_PROBE_H
#define GESHER_SYNC_PROBE_H
#ifdef GESHER_PTHREAD_ROUTING_PROBE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <windows.h>
int do_sema_b_wait_intern(HANDLE, int, DWORD);
struct gesher_sync_probe_v1 {
  unsigned size, version;
  int *(__cdecl *errno_ptr)(void);
'''+''.join('  __typeof__(&'+n+') '+n+';\n' for n in names)+''' };
__declspec(dllexport) const struct gesher_sync_probe_v1 * __cdecl gesher_sync_probe_v1(void);
#ifdef DLL_EXPORT
const struct gesher_sync_probe_v1 *gesher_sync_probe_owner(void);
#endif
#endif
#endif
'''
def block(t):
 name=t['name']; signature=t['signature']; args=[]
 for part in signature[1:-1].split(','):
  if part.strip()=='void': continue
  m=re.search(r'([A-Za-z_][A-Za-z_0-9]*)\s*$',part); assert m,(name,part); args.append(m.group(1))
 call='owner->'+name+'('+', '.join(args)+')'
 if name.startswith('sem_'):
  return '''#if defined(GESHER_PTHREAD_ROUTING_PROBE) && defined(DLL_EXPORT)
  const struct gesher_sync_probe_v1 *owner=gesher_sync_probe_owner();
  if (owner) {
    int *remote=owner->errno_ptr(), *local=&errno, saved=*remote;
    if (remote!=local) *remote=*local;
    __typeof__('''+call+''') result='''+call+''';
    if (remote!=local) { int value=*remote; *remote=saved; *local=value; }
    return result;
  }
#endif
'''
 return '''#if defined(GESHER_PTHREAD_ROUTING_PROBE) && defined(DLL_EXPORT)
  const struct gesher_sync_probe_v1 *owner=gesher_sync_probe_owner();
  if (owner) return '''+call+''';
#endif
'''
implementation='''#ifdef GESHER_PTHREAD_ROUTING_PROBE
static int * __cdecl gesher_sync_errno(void) { return &errno; }
__declspec(dllexport) const struct gesher_sync_probe_v1 * __cdecl gesher_sync_probe_v1(void)
{
  static const struct gesher_sync_probe_v1 table={sizeof(table),1,gesher_sync_errno,
'''+''.join('    '+n+',\n' for n in names)+'''  };
  return &table;
}
#ifdef DLL_EXPORT
const struct gesher_sync_probe_v1 *gesher_sync_probe_owner(void)
{
  typedef const struct gesher_sync_probe_v1 *(__cdecl *getter_fn)(void);
  HMODULE host=GetModuleHandleW(NULL), actual=NULL;
  getter_fn getter=host?(getter_fn)GetProcAddress(host,"gesher_sync_probe_v1"):NULL;
  const struct gesher_sync_probe_v1 *table; MEMORY_BASIC_INFORMATION info;
  if (!getter) {
    if (host && (GetProcAddress(host,"gesher_pthread_identity_probe_v6") || GetProcAddress(host,"gesher_clock_anchor_v1"))) abort();
    return NULL;
  }
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCWSTR)getter,&actual) || actual!=host) abort();
  table=getter();
  if (!table || !VirtualQuery(table,&info,sizeof(info)) || info.AllocationBase!=host || info.State!=MEM_COMMIT ||
      (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || (uintptr_t)table<(uintptr_t)info.BaseAddress ||
      (uintptr_t)table-(uintptr_t)info.BaseAddress>info.RegionSize ||
      sizeof(*table)>info.RegionSize-((uintptr_t)table-(uintptr_t)info.BaseAddress) ||
      table->size!=sizeof(*table) || table->version!=1) abort();
'''+''.join('  if (!table->'+n+' || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)table->'+n+',&actual) || actual!=host) abort();\n' for n in ['errno_ptr']+names)+'''  return table;
}
#endif
#endif
'''
def delta(old,new,rel):
 raw=list(difflib.unified_diff(old.splitlines(keepends=True),new.splitlines(keepends=True),fromfile='a/'+rel,tofile='b/'+rel,n=2)); out=raw[:2]; i=2
 while i<len(raw):
  m=re.match(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)\n',raw[i]); assert m,raw[i]
  a,ac,b,bc=int(m[1]),int(m[2] or 1),int(m[3]),int(m[4] or 1); tail=m[5]; i+=1; body=[]
  while i<len(raw) and not raw[i].startswith('@@ '): body.append(raw[i]); i+=1
  while body and body[0].startswith(' \t'): body.pop(0); a+=1; b+=1; ac-=1; bc-=1
  while body and body[-1].startswith(' \t'): body.pop(); ac-=1; bc-=1
  body=['\n' if line==' \n' else line for line in body]
  out.append(f'@@ -{a},{ac} +{b},{bc} @@{tail}\n'); out.extend(body)
 return ''.join(out)
patch=''; manifest=[]
for name in ['thread.c']+files:
 path=src/name; old=path.read_text(); new=old; lines=new.splitlines(keepends=True); indices=[i for i,l in enumerate(lines) if l.startswith('#include ')]; assert indices; lines.insert(indices[-1]+1,'#include "gesher_sync_probe.h"\n'); new=''.join(lines)
 if name=='thread.c':
  anchor='/* Hook for TLS-based deregistration/registration of thread.  */'; assert anchor in new; new=new.replace(anchor,implementation+'\n'+anchor,1)
 else:
  for t in [x for x in tags if pathlib.Path(x['path']).name==name]:
   sym=t['name']; pattern=re.compile(r'\b'+re.escape(sym)+r'\s*\([^;{}()]*\)\s*\{',re.M); matches=list(pattern.finditer(new)); assert len(matches)==1,(name,sym,len(matches)); m=matches[0]; new=new[:m.end()]+'\n'+block(t)+new[m.end():]
   manifest.append(name+'\t'+sym+'\t'+t['signature'])
 rel='mingw-w64-libraries/winpthreads/src/'+name; patch+=delta(old,new,rel)
rel='mingw-w64-libraries/winpthreads/src/gesher_sync_probe.h'; patch+='--- /dev/null\n+++ b/'+rel+'\n@@ -0,0 +1,'+str(len(header.splitlines()))+' @@\n'+''.join('+'+x for x in header.splitlines(keepends=True))
patch=patch.replace('\n \n','\n\n')
p=repo/'tools/host-pthread-authority/supplier.patch'; baseline=subprocess.check_output(['git','show','58dd958f:tools/host-pthread-authority/supplier.patch'],cwd=repo); assert p.read_bytes()==baseline, 'generate only from frozen v6 baseline'; p.write_bytes(baseline+patch.encode()); pin=repo/'tools/host-pthread-authority/pinned-inputs.json'; d=json.loads(pin.read_text());
for name in files: d['source_sha256']['mingw-w64-libraries/winpthreads/src/'+name]=hashlib.sha256((src/name).read_bytes()).hexdigest()
d['patch_sha256']=hashlib.sha256(p.read_bytes()).hexdigest(); pin.write_bytes((json.dumps(d,indent=2)+'\n').encode())
(repo/'docs/handoff/tracks/pthread-sync-probe-entries.tsv').write_bytes(('source\tsymbol\tsignature\n'+'\n'.join(manifest)+'\n').encode())
print('generated',len(tags),'sync entries')
