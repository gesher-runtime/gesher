/* Real Slice A lifetime/shared-backing actor. CI ONLY, one owned case/process.
 * Controller owns peer launch/deadlines/pipes/kernel reap and hashes file bytes.
 * Failure-backend builds are separate targets; persistent failures never PASS. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif
#ifdef GESHER_FILE_MREMAP_FAILURE_TEST
#include "alias_failure_adapter.h"
#endif
static unsigned char expected(size_t at) {
  if(at==37)return 0x5a;
  if(at==43)return 0x6b;
  return (unsigned char)((at*13+7)&255);
}
static long resources(void) {
#ifdef _WIN32
  DWORD count=0;
  return GetProcessHandleCount(GetCurrentProcess(),&count)?(long)count:-1;
#else
  DIR* d=opendir("/proc/self/fd"); long count=0; struct dirent* e;
  if(!d)return -1;
  while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,".."))++count;
  if(closedir(d))return -1;
  return count;
#endif
}
static int all_bytes(const unsigned char* map,size_t size) {
  for(size_t i=0;i<size;++i)if(map[i]!=expected(i))return 0;
  return 1;
}
static unsigned long long checksum(int fd,size_t size) {
  unsigned char bytes[4096]; unsigned long long hash=14695981039346656037ULL;
  if(lseek(fd,0,SEEK_SET)<0)return 0;
  size_t done=0;
  while(done<size) {
    ssize_t n=read(fd,bytes,size-done<sizeof(bytes)?size-done:sizeof(bytes));
    if(n<=0)return 0;
    for(ssize_t i=0;i<n;++i){hash^=bytes[i];hash*=1099511628211ULL;}
    done+=(size_t)n;
  }
  return hash;
}
static void query(const char* phase,unsigned char* start,size_t size,size_t page) {
#ifdef _WIN32
  for(size_t i=0;i<size;i+=page) {
    MEMORY_BASIC_INFORMATION info; SIZE_T n=VirtualQuery(start+i,&info,sizeof(info));
    printf("{\"event\":\"query\",\"phase\":\"%s\",\"page\":%zu,\"ok\":%d,\"state\":%lu,\"protection\":%lu,\"type\":%lu}\n",
      phase,i/page,n==sizeof(info),n?info.State:0,n?info.Protect:0,n?info.Type:0);
  }
#else
  /* Linux data/access checks remain independent; no simulated Windows rows. */
  (void)phase;(void)start;(void)size;(void)page;
#endif
}
static int warm(size_t page) {
  char name[]="warm-XXXXXX"; int fd=mkstemp(name),failed=0;
  void* a=MAP_FAILED;void* b=MAP_FAILED;
  if(fd<0)return 1;
  if(ftruncate(fd,(off_t)page))failed=1;
  if(!failed)a=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(a!=MAP_FAILED)b=mremap(a,0,page,MREMAP_MAYMOVE);
  if(a==MAP_FAILED||b==MAP_FAILED)failed=1;
  if(a!=MAP_FAILED&&munmap(a,page))failed=1;
  if(b!=MAP_FAILED&&munmap(b,page))failed=1;
  if(close(fd)||unlink(name))failed=1;
  return failed;
}
int main(int argc,char** argv) {
  long rawpage=sysconf(_SC_PAGESIZE);if(rawpage<=0)return 2;
  size_t page=(size_t)rawpage,size=page*5;
  if(argc<4)return 2;
  const char* mode=argv[1];const char* name=argv[2];const char* nonce=argv[3];
  /* Owned relative filename/nonce grammar; no arbitrary filesystem requests. */
  if(strcmp(name,"backing.dat")||strlen(nonce)!=32||strspn(nonce,"0123456789abcdef")!=32)return 2;
  if(warm(page)){printf("{\"event\":\"prerequisite\",\"warm_failed\":true}\n");return 2;}
  long before=resources();int fd=-1,failed=0,cleanup=0;unsigned char *a=MAP_FAILED,*b=MAP_FAILED,*c=MAP_FAILED;
  if(!strcmp(mode,"peer")) {
    fd=open(name,O_RDWR);if(fd<0)return 2;
    a=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(a==MAP_FAILED)failed=1;
    else {if(a[37]!=0x5a||a[43]!=(unsigned char)((43*13+7)&255))failed=1;a[43]=0x6b;}
    if(a!=MAP_FAILED&&munmap(a,size))++cleanup;
    if(close(fd))++cleanup;
    long after=resources();
    printf("{\"event\":\"peer_complete\",\"nonce\":\"%s\",\"pid\":%ld,\"failed\":%d,\"cleanup_failures\":%d,\"resources_before\":%ld,\"resources_after\":%ld}\n",nonce,(long)getpid(),failed,cleanup,before,after);
    return failed||cleanup||before<0||after!=before;
  }
  if(strcmp(mode,"source_first")&&strcmp(mode,"alias_first")&&strcmp(mode,"partial")&&strcmp(mode,"middle"))return 2;
  fd=open(name,O_CREAT|O_EXCL|O_RDWR,0600);if(fd<0)return 2;
  if(ftruncate(fd,(off_t)size))failed=1;
  if(!failed)a=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(a==MAP_FAILED){failed=1;goto done;}
  for(size_t i=0;i<size;++i)a[i]=(unsigned char)((i*13+7)&255);
  a[37]=0x5a;
  b=mremap(a,0,size,MREMAP_MAYMOVE);if(b==MAP_FAILED){failed=1;goto done;}
  c=mremap(b,0,size,MREMAP_MAYMOVE);if(c==MAP_FAILED){failed=1;goto done;}
  printf("{\"event\":\"ready\",\"nonce\":\"%s\",\"pid\":%ld,\"page\":%zu,\"file_size\":%zu}\n",nonce,(long)getpid(),page,size);fflush(stdout);
  char control[40];if(!fgets(control,sizeof(control),stdin)||strcmp(control,"PEER_DONE\n")){failed=1;goto done;}
  if(!all_bytes(a,size)||!all_bytes(b,size)||!all_bytes(c,size))failed=1;
  query("before",a,size,page);
#ifdef GESHER_FILE_MREMAP_FAILURE_TEST
  if(argc==6){FileAliasFailureReset(atoi(argv[4]));FileAliasFailureNth(atoi(argv[5]));}
#endif
  if(!strcmp(mode,"partial")) {
    if(munmap(a+page,page))failed=1;
    if(a[0]!=expected(0)||a[2*page]!=expected(2*page))failed=1;
    if(munmap(a,page))failed=1;
    /* Second surviving interval must remain accessible after prefix cleanup. */
    if(a[2*page]!=expected(2*page)||a[size-1]!=expected(size-1))failed=1;
    query("partial_survivors",a+2*page,page*3,page);
    if(munmap(a+2*page,page*3))++cleanup;
    a=MAP_FAILED;
  } else if(!strcmp(mode,"middle")) {
    errno=0;
    void* middle=mmap(a+2*page,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    int operation_error=errno;
    printf("{\"event\":\"middle_result\",\"failed\":%d,\"errno\":%d}\n",middle==MAP_FAILED,operation_error);
    if(middle==MAP_FAILED)failed=1;
    else {((unsigned char*)middle)[0]=0xee;if(b[2*page]!=expected(2*page))failed=1;}
    query("after_middle",a,size,page);
    /* Regression: second-prefix VirtualProtect failure MUST NOT undercount
       the complete mapped prefix and destroy its second page here. */
    if(munmap(a,page))failed=1;
    if(a[page]!=expected(page))failed=1;
    query("second_prefix_survives",a+page,page,page);
    if(munmap(a+page,page*4))++cleanup;
    a=MAP_FAILED;
  }
  if(!strcmp(mode,"alias_first")) {
    if(munmap(c,size))++cleanup;
    c=MAP_FAILED;
    if(munmap(b,size))++cleanup;
    b=MAP_FAILED;
    if(!all_bytes(a,size))failed=1;
  } else {
    if(a!=MAP_FAILED){if(munmap(a,size))++cleanup;a=MAP_FAILED;}
    if(!all_bytes(b,size)||!all_bytes(c,size))failed=1;
    if(munmap(b,size))++cleanup;
    b=MAP_FAILED;
    if(!all_bytes(c,size))failed=1;
  }
done:
  if(a!=MAP_FAILED&&munmap(a,size))++cleanup;
  if(b!=MAP_FAILED&&munmap(b,size))++cleanup;
  if(c!=MAP_FAILED&&munmap(c,size))++cleanup;
  struct stat st;long long bytes=-1;unsigned long long hash=0;
  if(fd>=0){if(!fstat(fd,&st))bytes=(long long)st.st_size;else failed=1;hash=checksum(fd,size);if(close(fd))++cleanup;}
  long after=resources();
#ifdef GESHER_FILE_MREMAP_FAILURE_TEST
  printf("{\"event\":\"injection\",\"reached\":[");
  for(int i=0;i<18;++i)printf("%s%d",i?",":"",FileAliasFailureMetric(i));
  printf("],\"outstanding_before_controller_cleanup\":%d}\n",FileAliasFailureOutstanding());
#endif
  printf("{\"event\":\"complete\",\"nonce\":\"%s\",\"case\":\"%s\",\"failed\":%d,\"cleanup_failures\":%d,\"file_size\":%lld,\"file_fnv1a64\":\"%016llx\",\"resources_before\":%ld,\"resources_after\":%ld}\n",nonce,mode,failed,cleanup,bytes,hash,before,after);
  return failed||cleanup||before<0||after!=before;
}
