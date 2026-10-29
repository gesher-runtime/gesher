/* Isolated CI characterization: same-key deletion from an actual destructor. */
#include <pthread.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
static HANDLE entered, returned;
static pthread_key_t key;
static void destructor(void *value) {
  if (value!=(void *)(uintptr_t)1 || !SetEvent(entered)) abort();
  if (pthread_key_delete(key) || !SetEvent(returned)) abort();
}
static void *worker(void *unused) {
  (void)unused;
  if (pthread_setspecific(key,(void *)(uintptr_t)1)) abort();
  return NULL;
}
static int child(void) {
  pthread_t thread;
  if (pthread_key_create(&key,destructor) || pthread_create(&thread,NULL,worker,NULL) || pthread_join(thread,NULL)) return 4;
  return 0;
}
int main(int argc,char **argv) {
  if (argc==4 && strcmp(argv[1],"--child")==0) {
    entered=(HANDLE)(uintptr_t)_strtoui64(argv[2],NULL,10);
    returned=(HANDLE)(uintptr_t)_strtoui64(argv[3],NULL,10);
    return child();
  }
  if (argc!=1) return 1;
  int status=2, assigned=0, created=0, observed=0, attrs_ready=0;
  SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
  entered=CreateEventW(&sa,TRUE,FALSE,NULL); returned=CreateEventW(&sa,TRUE,FALSE,NULL);
  HANDLE job=CreateJobObjectW(NULL,NULL);
  PROCESS_INFORMATION pi={0}; STARTUPINFOEXW si={0}; SIZE_T bytes=0;
  LPPROC_THREAD_ATTRIBUTE_LIST attrs=NULL;
  if (!entered || !returned || !job) goto cleanup;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) goto cleanup;
  InitializeProcThreadAttributeList(NULL,1,0,&bytes);
  attrs=HeapAlloc(GetProcessHeap(),0,bytes);
  if (!attrs || !InitializeProcThreadAttributeList(attrs,1,0,&bytes)) goto cleanup;
  attrs_ready=1;
  HANDLE inherited[2]={entered,returned};
  if (!UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),NULL,NULL)) goto cleanup;
  wchar_t path[32768], command[32768]; DWORD n=GetModuleFileNameW(NULL,path,32768);
  if (!n || n>=32768 || _snwprintf(command,32768,L"\"%ls\" --child %llu %llu",path,
      (unsigned long long)(uintptr_t)entered,(unsigned long long)(uintptr_t)returned)<0) goto cleanup;
  si.StartupInfo.cb=sizeof(si); si.lpAttributeList=attrs;
  if (!CreateProcessW(path,command,NULL,NULL,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&si.StartupInfo,&pi)) goto cleanup;
  created=1;
  if (!AssignProcessToJobObject(job,pi.hProcess)) goto cleanup;
  assigned=1;
  if (ResumeThread(pi.hThread)==(DWORD)-1) goto cleanup;
  if (WaitForSingleObject(entered,5000)!=WAIT_OBJECT_0) { status=3; goto cleanup; }
  if (WaitForSingleObject(returned,5000)!=WAIT_OBJECT_0) { status=4; goto cleanup; }
  observed=1;
  if (WaitForSingleObject(pi.hProcess,5000)!=WAIT_OBJECT_0) { status=5; goto cleanup; }
  DWORD code=0;
  if (!GetExitCodeProcess(pi.hProcess,&code) || code!=0) { status=6; goto cleanup; }
  status=0;
cleanup:
  if (created && WaitForSingleObject(pi.hProcess,0)!=WAIT_OBJECT_0) {
    if (assigned) { if (!TerminateJobObject(job,47)) status=8; }
    else if (!TerminateProcess(pi.hProcess,47)) status=8;
    if (WaitForSingleObject(pi.hProcess,5000)!=WAIT_OBJECT_0) status=8;
  }
  if (assigned) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting={0};
    ULONGLONG deadline=GetTickCount64()+5000;
    do {
      if (!QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),NULL)) {
        fprintf(stderr,"FAIL supervisor query-job error=%lu\n",GetLastError()); status=8; break;
      }
      if (!accounting.ActiveProcesses) break;
      Sleep(5);
    } while (GetTickCount64()<deadline);
    if (accounting.ActiveProcesses) { fprintf(stderr,"FAIL supervisor job active=%lu\n",accounting.ActiveProcesses); status=8; }
  }
  if (attrs_ready) DeleteProcThreadAttributeList(attrs);
  if (attrs && !HeapFree(GetProcessHeap(),0,attrs)) { fprintf(stderr,"FAIL supervisor heapfree error=%lu\n",GetLastError()); status=8; }
  if (created) {
    if (!CloseHandle(pi.hThread)) { fprintf(stderr,"FAIL supervisor thread-handle error=%lu\n",GetLastError()); status=8; }
    if (!CloseHandle(pi.hProcess)) { fprintf(stderr,"FAIL supervisor process-handle error=%lu\n",GetLastError()); status=8; }
  }
  if (job && !CloseHandle(job)) { fprintf(stderr,"FAIL supervisor job-handle error=%lu\n",GetLastError()); status=8; }
  if (entered && !CloseHandle(entered)) { fprintf(stderr,"FAIL supervisor entered-handle error=%lu\n",GetLastError()); status=8; }
  if (returned && !CloseHandle(returned)) { fprintf(stderr,"FAIL supervisor returned-handle error=%lu\n",GetLastError()); status=8; }
  if (!status && observed) printf("PASS destructor deletes its own key and returns; child pid=%lu exit=0 job-empty\n",pi.dwProcessId);
  return status;
}
