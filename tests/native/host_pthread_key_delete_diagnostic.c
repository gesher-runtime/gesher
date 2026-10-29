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
  if (WaitForSingleObject(returned,1500)!=WAIT_TIMEOUT || WaitForSingleObject(pi.hProcess,0)!=WAIT_TIMEOUT) { status=4; goto cleanup; }
  observed=1;
  if (!TerminateJobObject(job,47) || WaitForSingleObject(pi.hProcess,5000)!=WAIT_OBJECT_0) { status=5; goto cleanup; }
  DWORD code=0;
  if (!GetExitCodeProcess(pi.hProcess,&code) || code!=47) { status=6; goto cleanup; }
  status=0;
cleanup:
  if (created && WaitForSingleObject(pi.hProcess,0)!=WAIT_OBJECT_0) {
    if (assigned) { if (!TerminateJobObject(job,47)) status=8; }
    else if (!TerminateProcess(pi.hProcess,47)) status=8;
    if (WaitForSingleObject(pi.hProcess,5000)!=WAIT_OBJECT_0) status=8;
  }
  if (assigned) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting={0};
    if (!QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),NULL) || accounting.ActiveProcesses!=0) status=8;
  }
  if (attrs_ready) DeleteProcThreadAttributeList(attrs);
  if (attrs && !HeapFree(GetProcessHeap(),0,attrs)) status=8;
  if (created) { if (!CloseHandle(pi.hThread)) status=8; if (!CloseHandle(pi.hProcess)) status=8; }
  if (job && !CloseHandle(job)) status=8;
  if (entered && !CloseHandle(entered)) status=8;
  if (returned && !CloseHandle(returned)) status=8;
  if (!status && observed) printf("CONTROL destructor entered, same-key deletion did not return within 1500ms, owned child pid=%lu exit=47 job-empty\n",pi.dwProcessId);
  return status;
}
