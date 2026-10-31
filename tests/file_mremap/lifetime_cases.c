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
#ifdef GESHER_FILE_MREMAP_FAILURE_TEST
static int ledger(const char* phase,unsigned char* address,size_t size,size_t page) {
  FileAliasPage rows[5]; int bad=0;
  if(size!=5*page||FileAliasRegistrySnapshot(address,size,rows,5)!=5)return 1;
  for(int i=0;i<5;++i) {
    FileAliasPage* r=&rows[i];
    if(!r->query_ok)bad=1;
    if(r->present && r->kind==1) {
      if(r->live && (r->query_state!=MEM_COMMIT || r->query_type!=MEM_MAPPED || r->query_protection==PAGE_NOACCESS))bad=1;
      if(!r->live && r->query_state==MEM_COMMIT && r->query_protection!=PAGE_NOACCESS)bad=1;
      if(r->ledger_protection && r->query_protection!=r->ledger_protection)bad=1;
      if(!r->owner_refs || r->owner_state!=1 || r->owner_refs!=r->owner_views+r->owner_backing_pins)bad=1;
      size_t counted=0;
      for(int j=0;j<5;++j)if(rows[j].present && rows[j].kind==1 &&
          (size_t)j-rows[j].region_page==(size_t)i-r->region_page && rows[j].live)++counted;
      if(counted!=r->live_pages)bad=1;
    }
    if(r->present && r->kind==2 && (r->live || r->live_pages || r->owner_refs || r->query_state!=MEM_RESERVE))bad=1;
    printf("{\"event\":\"ledger\",\"phase\":\"%s\",\"page\":%d,\"present\":%d,\"region_page\":%zu,\"region_size\":%zu,\"kind\":%d,\"live\":%d,\"live_pages\":%zu,\"owner_refs\":%zu,\"owner_views\":%zu,\"owner_backing_pins\":%zu,\"owner_state\":%d,\"owner_failure\":%d,\"failed_owners\":%d,\"failed_owner_state\":%d,\"failed_owner_error\":%lu,\"failed_owner_refs\":%zu,\"rollback_view\":%d,\"failed_handle_valid\":%d,\"backing_refs\":%zu,\"rollback_query_ok\":%d,\"rollback_state\":%lu,\"rollback_protection\":%lu,\"rollback_type\":%lu,\"ledger_protection\":%lu,\"operation_error\":%lu,\"query_ok\":%d,\"state\":%lu,\"protection\":%lu,\"type\":%lu,\"group\":%d}\n",
      phase,i,r->present,r->region_page,r->region_size,r->kind,r->live,r->live_pages,r->owner_refs,r->owner_views,r->owner_backing_pins,r->owner_state,r->owner_failure,r->failed_owners,r->failed_owner_state,r->failed_owner_error,r->failed_owner_refs,r->rollback_view,r->failed_handle_valid,r->backing_refs,r->rollback_query_ok,r->rollback_state,r->rollback_protection,r->rollback_type,r->ledger_protection,r->operation_error,r->query_ok,r->query_state,r->query_protection,r->query_type,r->group);
  }
  return bad;
}
static int failure_state(unsigned char* source,unsigned char* alias,size_t size,size_t page,int fd) {
  FileAliasPage rows[5],after[5]; int bad=0;
  int metrics[20];for(int i=0;i<20;++i)metrics[i]=FileAliasFailureMetric(i);
  long handles=resources();
  if(FileAliasRegistrySnapshot(source,size,rows,5)!=5 || !rows[0].owner_failure)return 1;
  for(int route=0;route<11;++route) {
    errno=0;void* result=MAP_FAILED;
    switch(route) {
      case 0:result=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);break;
      case 1:result=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);break;
      case 2:result=mmap(source,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);break;
      case 3:result=mmap64(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);break;
      case 4:result=mremap(alias,0,size,MREMAP_MAYMOVE);break;
      case 5:result=mremap(alias,size,size,MREMAP_MAYMOVE);break;
      case 6:result=mremap(source,page,page*2,MREMAP_MAYMOVE);break;
    }
    int error=errno;
    if(route>=7) {
      errno=0;int status=-2;
      if(route==7)status=mprotect(source,page,PROT_READ|PROT_WRITE);
      if(route==8)status=madvise(source,page,MADV_DONTNEED);
      if(route==9)status=madvise(source,page,MADV_FREE);
      if(route==10)status=madvise(source,page,MADV_REMOVE);
      error=errno;result=status==-1?MAP_FAILED:source;
    }
    printf("{\"event\":\"failedstate_guard\",\"route\":%d,\"rejected\":%d,\"errno\":%d}\n",route,result==MAP_FAILED,error);
    if(result!=MAP_FAILED||error!=EIO)bad=1;
  }
  int unchanged=FileAliasRegistrySnapshot(source,size,after,5)==5;
  for(int i=0;i<5 && unchanged;++i) {
    FileAliasPage* x=&rows[i];FileAliasPage* y=&after[i];
    if(x->present!=y->present||x->region_size!=y->region_size||x->kind!=y->kind||
       x->live!=y->live||x->live_pages!=y->live_pages||x->owner_refs!=y->owner_refs||
       x->owner_views!=y->owner_views||x->owner_backing_pins!=y->owner_backing_pins||
       x->owner_state!=y->owner_state||x->owner_failure!=y->owner_failure||
       x->ledger_protection!=y->ledger_protection||x->operation_error!=y->operation_error||
       x->query_ok!=y->query_ok||x->query_state!=y->query_state||
       x->query_protection!=y->query_protection||x->query_type!=y->query_type||
       x->failed_owners!=y->failed_owners||x->region_page!=y->region_page||x->group!=y->group||
       x->failed_owner_state!=y->failed_owner_state||x->failed_owner_error!=y->failed_owner_error||
       x->failed_owner_refs!=y->failed_owner_refs||x->failed_handle_valid!=y->failed_handle_valid||
       x->rollback_view!=y->rollback_view||x->backing_refs!=y->backing_refs||
       x->rollback_query_ok!=y->rollback_query_ok||x->rollback_state!=y->rollback_state||
       x->rollback_protection!=y->rollback_protection||x->rollback_type!=y->rollback_type)unchanged=0;
  }
  for(int i=0;i<20;++i)if(metrics[i]!=FileAliasFailureMetric(i))unchanged=0;
  long handles_after=resources();if(handles<0||handles!=handles_after)unchanged=0;
  printf("{\"event\":\"failedstate_unchanged\",\"ledger_and_backend_unchanged\":%d,\"resources_before\":%ld,\"resources_after\":%ld}\n",unchanged,handles,handles_after);
  return bad||!unchanged;
}
static int protect_result(const char* step,unsigned char* at,size_t length,int prot,int wanted,int wanted_errno) {
  int before=FileAliasFailureMetric(5),injected=FileAliasFailureMetric(12);
  errno=0;int status=mprotect(at,length,prot),error=errno;
  int after=FileAliasFailureMetric(5),injected_after=FileAliasFailureMetric(12);
  printf("{\"event\":\"protection_result\",\"step\":\"%s\",\"result\":%d,\"errno\":%d,\"protect_before\":%d,\"protect_after\":%d,\"injected_before\":%d,\"injected_after\":%d}\n",step,status,error,before,after,injected,injected_after);
  return status!=wanted || (wanted && error!=wanted_errno);
}
static int punch_equal(const FileAliasPage* a,const FileAliasPage* b) {
  return a->present==b->present&&a->region_page==b->region_page&&a->region_size==b->region_size&&
    a->kind==b->kind&&a->live==b->live&&a->live_pages==b->live_pages&&a->live_capacity==b->live_capacity&&
    a->owner_refs==b->owner_refs&&a->owner_views==b->owner_views&&a->owner_backing_pins==b->owner_backing_pins&&
    a->owner_failure==b->owner_failure&&a->ledger_protection==b->ledger_protection&&
    a->query_state==b->query_state&&a->query_protection==b->query_protection&&a->query_type==b->query_type&&
    a->mapping_handle_valid==b->mapping_handle_valid&&a->query_ok==b->query_ok&&a->query_base==b->query_base&&a->allocation_base==b->allocation_base&&a->query_region_size==b->query_region_size;
}
static void punch_rows(const char* phase,unsigned char* origin,const FileAliasPage* rows,size_t count) {
  for(size_t i=0;i<count;++i) {
    const FileAliasPage* q=&rows[i];
    printf("{\"event\":\"punch_page\",\"phase\":\"%s\",\"page\":%zu,\"present\":%d,\"region_page\":%zu,\"region_size\":%zu,\"capacity\":%zu,\"kind\":%d,\"live\":%d,\"live_pages\":%zu,\"refs\":%zu,\"views\":%zu,\"pins\":%zu,\"failed\":%d,\"error\":%lu,\"state\":%lu,\"protection\":%lu,\"type\":%lu,\"ledger\":%lu,\"query_ok\":%d,\"query_base_offset\":%lld,\"query_size\":%zu,\"allocation_base_offset\":%lld}\n",
      phase,i,q->present,q->region_page,q->region_size,q->live_capacity,q->kind,q->live,q->live_pages,q->owner_refs,q->owner_views,q->owner_backing_pins,q->owner_failure,q->operation_error,q->query_state,q->query_protection,q->query_type,q->ledger_protection,q->query_ok,(long long)((intptr_t)q->query_base-(intptr_t)origin),q->query_region_size,(long long)((intptr_t)q->allocation_base-(intptr_t)origin));
  }
}
static int readable(unsigned char* at) {
  MEMORY_BASIC_INFORMATION q;return VirtualQuery(at,&q,sizeof(q))==sizeof(q)&&q.State==MEM_COMMIT&&q.Protect!=PAGE_NOACCESS&&!(q.Protect&PAGE_GUARD);
}
#endif
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
#ifdef GESHER_FILE_MREMAP_FAILURE_TEST
  int punch_case=!strncmp(mode,"punch_",6);
  size_t punch_span=0;
  if(punch_case)size=512*page;
#else
  int punch_case=0;
#endif
  /* Owned relative filename/nonce grammar; no arbitrary filesystem requests. */
  if(strcmp(name,"backing.dat")||strlen(nonce)!=32||strspn(nonce,"0123456789abcdef")!=32)return 2;
  if(warm(page)){printf("{\"event\":\"prerequisite\",\"warm_failed\":true}\n");return 2;}
  long before=resources();int fd=-1,failed=0,cleanup=0;unsigned char *a=MAP_FAILED,*b=MAP_FAILED,*c=MAP_FAILED,*observed_source=MAP_FAILED;
  if(!strcmp(mode,"peer")) {
    fd=open(name,O_RDWR);if(fd<0)return 2;
    struct stat peer_stat;
    if(fstat(fd,&peer_stat)||peer_stat.st_size<=0 || (peer_stat.st_size!=(off_t)(5*page)&&peer_stat.st_size!=(off_t)(512*page))){close(fd);return 2;}
    size=(size_t)peer_stat.st_size;
    a=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(a==MAP_FAILED)failed=1;
    else {if(a[37]!=0x5a||a[43]!=(unsigned char)((43*13+7)&255))failed=1;a[43]=0x6b;}
    if(a!=MAP_FAILED&&munmap(a,size))++cleanup;
    if(close(fd))++cleanup;
    long after=resources();
    printf("{\"event\":\"peer_complete\",\"nonce\":\"%s\",\"pid\":%ld,\"failed\":%d,\"cleanup_failures\":%d,\"resources_before\":%ld,\"resources_after\":%ld}\n",nonce,(long)getpid(),failed,cleanup,before,after);
    return failed||cleanup||before<0||after!=before;
  }
  int adverse=!strncmp(mode,"fault_",6);
#ifdef GESHER_FILE_MREMAP_FAILURE_TEST
  int protection_case=!strncmp(mode,"protect_",8);
#else
  int protection_case=0;
#endif
  if(!adverse && !protection_case && !punch_case && strcmp(mode,"source_first")&&strcmp(mode,"alias_first")&&strcmp(mode,"partial")&&strcmp(mode,"middle"))return 2;
  fd=open(name,O_CREAT|O_EXCL|O_RDWR,0600);if(fd<0)return 2;
  if(ftruncate(fd,(off_t)size))failed=1;
  if(!failed)a=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(a==MAP_FAILED){failed=1;goto done;}
  observed_source=a;
  for(size_t i=0;i<size;++i)a[i]=(unsigned char)((i*13+7)&255);
  a[37]=0x5a;
  b=mremap(a,0,size,MREMAP_MAYMOVE);if(b==MAP_FAILED){failed=1;goto done;}
  c=mremap(b,0,size,MREMAP_MAYMOVE);if(c==MAP_FAILED){failed=1;goto done;}
  printf("{\"event\":\"ready\",\"nonce\":\"%s\",\"pid\":%ld,\"page\":%zu,\"file_size\":%zu}\n",nonce,(long)getpid(),page,size);fflush(stdout);
  char control[40];if(!fgets(control,sizeof(control),stdin)||strcmp(control,"PEER_DONE\n")){failed=1;goto done;}
  if(!all_bytes(a,size)||!all_bytes(b,size)||!all_bytes(c,size))failed=1;
  query("before",a,punch_case?page:size,page);
#ifdef GESHER_FILE_MREMAP_FAILURE_TEST
  if(punch_case) {
    FileAliasPage before_rows[512],after_rows[512];
    const int private_case=!strcmp(mode,"punch_private_success");
    void* middle=mmap(a+16*page,(private_case?128:16)*page,PROT_READ|PROT_WRITE,
      private_case?(MAP_PRIVATE|MAP_FIXED):(MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED),private_case?fd:-1,private_case?(off_t)(16*page):0);
    if(middle==MAP_FAILED){failed=1;goto done;}
    if(private_case) {
      a[100*page]=0x9d;
      if(mprotect(a+101*page,page,PROT_NONE)){failed=1;goto done;}
    }
    if(FileAliasRegistrySnapshot(a,size,before_rows,512)!=512){failed=1;goto done;}
    const size_t capacity=before_rows[0].live_capacity;
    size_t combined=capacity+1;if(combined<33)combined=33;
    if(combined<=capacity||combined>128) {
      printf("{\"event\":\"prerequisite\",\"production_capacity_unavailable\":true,\"capacity\":%zu}\n",capacity);
      failed=1;goto done;
    }
    punch_span=combined;
    punch_rows("before",a,before_rows,combined);
    int injected_mode=!strcmp(mode,"punch_prepare_failure")||!strcmp(mode,"punch_file_prepare_failure")?12:!strcmp(mode,"punch_coalesce_failure")?13:(!strcmp(mode,"punch_success")||private_case)?0:-1;
    if(injected_mode<0){failed=1;goto done;}
    FileAliasFailureReset(injected_mode);FileAliasFailureNth(1);
    long handles_before=resources();errno=0;
    void* result=!strcmp(mode,"punch_file_prepare_failure")
      ?mmap(a,combined*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_FIXED,fd,0)
      :mmap(a,combined*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    int error=errno;
    FileAliasAllocationDisable();
    int metrics[28];for(int n=0;n<28;++n)metrics[n]=FileAliasFailureMetric(n);
    long handles_after=resources();
    if(FileAliasRegistrySnapshot(a,size,after_rows,512)!=512){failed=1;goto done;}
    int unchanged=1;for(size_t n=0;n<512;++n)if(!punch_equal(&before_rows[n],&after_rows[n]))unchanged=0;
    punch_rows("after",a,after_rows,combined);
    if(private_case) {
      const FileAliasPage* q=&after_rows[101];
      int value=readable(a+100*page)?a[100*page]:-1;
      printf("{\"event\":\"punch_private\",\"value100\":%d,\"live100\":%d,\"live101\":%d,\"protection101\":%lu,\"ledger101\":%lu,\"section_handle_valid\":%d,\"type101\":%lu}\n",value,after_rows[100].live,q->live,q->query_protection,q->ledger_protection,q->mapping_handle_valid,q->query_type);
      if(value!=0x9d||!after_rows[100].live||!q->live||q->query_protection!=PAGE_NOACCESS||q->ledger_protection!=PAGE_NOACCESS||!q->mapping_handle_valid)failed=1;
    }
    printf("{\"event\":\"punch_result\",\"case\":\"%s\",\"capacity\":%zu,\"combined_pages\":%zu,\"failed\":%d,\"errno\":%d,\"source_unchanged\":%d,\"resources_before\":%ld,\"resources_after\":%ld,\"metrics\":[",mode,capacity,combined,result==MAP_FAILED,error,unchanged,handles_before,handles_after);
    for(int n=0;n<28;++n)printf("%s%d",n?",":"",metrics[n]);
    printf("]}\n");
    if(injected_mode==12) {
      if(result!=MAP_FAILED||error!=ENOMEM||!unchanged||metrics[20]<1||metrics[21]!=1||handles_before!=handles_after)failed=1;
      for(int n=0;n<20;++n)if(metrics[n])failed=1;
      for(int n=22;n<28;++n)if(metrics[n])failed=1;
    } else if(injected_mode==13) {
      if(result!=MAP_FAILED||!after_rows[0].owner_failure||metrics[25]!=1||metrics[26]!=1)failed=1;
      for(size_t n=0;n<combined;++n)if(after_rows[n].kind!=2||after_rows[n].live||after_rows[n].query_state!=MEM_RESERVE)failed=1;
      /* Persistent partial outcome is FAILED_CONTAINMENT, never passed negative. */
      failed=1;
    } else {
      if(result==MAP_FAILED||after_rows[0].owner_failure)failed=1;
      for(size_t n=0;n<combined;++n)if(after_rows[n].kind!=0||!after_rows[n].live||after_rows[n].query_state!=MEM_COMMIT)failed=1;
    }
    if(!strcmp(mode,"punch_success")) {
      memcpy(before_rows,after_rows,sizeof(before_rows));
      FileAliasFailureReset(12);FileAliasFailureNth(1);errno=0;
      int cleanup_result=munmap(a+page,page),cleanup_errno=errno;
      FileAliasAllocationDisable();
      if(FileAliasRegistrySnapshot(a,size,after_rows,512)!=512){failed=1;goto done;}
      int same=1;for(size_t n=0;n<512;++n)if(!punch_equal(&before_rows[n],&after_rows[n]))same=0;
      printf("{\"event\":\"punch_cleanup_prepare\",\"result\":%d,\"errno\":%d,\"source_unchanged\":%d,\"metrics\":[",cleanup_result,cleanup_errno,same);
      for(int n=0;n<28;++n)printf("%s%d",n?",":"",FileAliasFailureMetric(n));
      printf("]}\n");
      if(cleanup_result!=-1||cleanup_errno!=ENOMEM||!same||FileAliasFailureMetric(20)<1||FileAliasFailureMetric(21)!=1)failed=1;
      for(int n=0;n<20;++n)if(FileAliasFailureMetric(n))failed=1;
      for(int n=22;n<28;++n)if(FileAliasFailureMetric(n))failed=1;
    }
    if(!all_bytes(b,size)||!all_bytes(c,size))failed=1;
    goto done;
  }
  if(protection_case) {
    if(!strcmp(mode,"protect_dead")||!strcmp(mode,"protect_mixed")) {
      if(munmap(a+page,page))failed=1;
      failed|=ledger("protect_before",a,size,page);
      FileAliasFailureReset(0);FileAliasFailureNth(1);
      if(!strcmp(mode,"protect_dead")) {
        failed|=protect_result("unaligned",a+1,page,PROT_READ,-1,EINVAL);
        failed|=protect_result("length_overflow",a,SIZE_MAX,PROT_READ,-1,EINVAL);
        failed|=protect_result("dead",a+page,page,PROT_READ|PROT_WRITE,-1,ENOMEM);
      }
      else failed|=protect_result("mixed",a,page*3,PROT_READ,-1,ENOMEM);
      if(FileAliasFailureMetric(5)!=0)failed=1;
      failed|=ledger("protect_after",a,size,page);
      if(readable(a+page)||!readable(a)||!readable(a+2*page))failed=1;
      /* A dead page elsewhere in the SAME view must not reject a live-only range. */
      failed|=protect_result("live_survivor",a+2*page,page,PROT_READ,0,0);
      failed|=ledger("protect_survivor",a,size,page);
    } else if(!strcmp(mode,"protect_partial")) {
      FileAliasFailureReset(5);FileAliasFailureNth(2);
      failed|=protect_result("partial_failure",a,page*3,PROT_READ,-1,EACCES);
      if(FileAliasFailureMetric(5)!=2||FileAliasFailureMetric(12)!=1)failed=1;
      failed|=ledger("protect_partial",a,size,page);
      /* Harmless one-shot protection failure does not poison a sound owner;
         record it before disabling injection for explicit native cleanup. */
      FileAliasPage observed[5];
      if(FileAliasRegistrySnapshot(a,size,observed,5)!=5||observed[0].owner_failure)failed=1;
      if(!readable(a)||!readable(a+page)||a[0]!=expected(0)||a[page]!=expected(page))failed=1;
      FileAliasFailureReset(0);
    } else if(!strcmp(mode,"protect_surgery")) {
      FileAliasFailureReset(0);FileAliasFailureNth(1);
      failed|=protect_result("whole_read",a,size-1,PROT_READ,0,0);
      failed|=protect_result("suffix_write",a+3*page,page*2,PROT_READ|PROT_WRITE,0,0);
      failed|=ledger("protect_changed",a,size,page);
      void* middle=mmap(a+2*page,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
      if(middle==MAP_FAILED){failed=1;goto done;}
      if(!readable(a)||!readable(a+page)||!readable(a+3*page)||a[0]!=expected(0)||a[page]!=expected(page)||a[3*page]!=expected(3*page))failed=1;
      if(((unsigned char*)middle)[0]!=0||!all_bytes(b,size)||!all_bytes(c,size))failed=1;
      failed|=ledger("protect_surgery",a,size,page);
      failed|=protect_result("post_surgery_live",a+page,page*3,PROT_READ,0,0);
      failed|=ledger("protect_surgery_updated",a,size,page);
      if(munmap(a,page))failed=1;
      /* The now-dead prefix cannot become accessible through mixed mprotect. */
      failed|=protect_result("post_surgery_mixed",a,page*2,PROT_READ,-1,ENOMEM);
      if(readable(a)||!readable(a+page)||a[page]!=expected(page))failed=1;
      failed|=ledger("protect_post_surgery_dead",a,size,page);
    } else {failed=1;goto done;}
    if(!all_bytes(b,size)||!all_bytes(c,size))failed=1;
    if(munmap(a,size))++cleanup;
    failed|=ledger("protect_retired",a,size,page);
    a=MAP_FAILED;
    goto done;
  }
  if(adverse && argc!=6){failed=1;goto done;}
  if(argc==6){FileAliasFailureReset(atoi(argv[4]));FileAliasFailureNth(atoi(argv[5]));}
  if(adverse) {
    unsigned char* original_address=a;
    int operation=0; errno=0;
    if(!strcmp(mode,"fault_alias")) {
      void* extra=mremap(a,0,size,MREMAP_MAYMOVE);
      operation=extra==MAP_FAILED?-1:0;
      if(extra!=MAP_FAILED&&munmap(extra,size))++cleanup;
    } else if(!strcmp(mode,"fault_middle")) {
      operation=mmap(a+2*page,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==MAP_FAILED?-1:0;
    } else if(!strcmp(mode,"fault_prefix"))operation=munmap(a,page);
    else if(!strcmp(mode,"fault_suffix"))operation=munmap(a+4*page,page);
    else if(!strcmp(mode,"fault_partial"))operation=munmap(a+page,page*2);
    else if(!strcmp(mode,"fault_sourcefirst")){operation=munmap(a,size);if(!operation)a=MAP_FAILED;}
    else if(!strcmp(mode,"fault_aliasfirst")){operation=munmap(c,size);if(!operation)c=MAP_FAILED;}
    else if(!strcmp(mode,"fault_finalclose")) {
      operation=munmap(a,size);a=MAP_FAILED;
      if(munmap(b,size))operation=-1;
      b=MAP_FAILED;
      if(munmap(c,size))operation=-1;
      c=MAP_FAILED;
    } else {failed=1;goto done;}
    int operation_errno=errno;
    printf("{\"event\":\"fault_result\",\"case\":\"%s\",\"operation\":%d,\"errno\":%d}\n",mode,operation,operation_errno);
    /* Always inspect original source address, including released placeholders.
       The independent snapshots are not failure-injected. */
    unsigned char* original=original_address;
    if(original)failed|=ledger("after_fault",original,size,page);
    if(!strcmp(mode,"fault_middle")&&atoi(argv[4])==5&&atoi(argv[5])==2) {
      /* Failed second prefix restoration must retain BOTH real prefix pages.
         Observe first, then clean first page; never dereference an absent page. */
      if(!readable(a)||!readable(a+page))failed=1;
      else if(a[0]!=expected(0)||a[page]!=expected(page))failed=1;
      int first=munmap(a,page); /* EIO is expected while invariant is failed. */
      printf("{\"event\":\"prefix_cleanup\",\"result\":%d,\"errno\":%d,\"second_readable\":%d}\n",first,errno,readable(a+page));
      if(!readable(a+page)||a[page]!=expected(page))failed=1;
      failed|=ledger("after_first_prefix_cleanup",a,size,page);
    }
    FileAliasPage state_rows[5];
    if(FileAliasRegistrySnapshot(original,size,state_rows,5)!=5)failed=1;
    if(state_rows[0].owner_failure) {
      failed|=failure_state(original,b==MAP_FAILED?original:b,size,page,fd);
      printf("{\"event\":\"containment\",\"owner_failure\":true,\"classification\":\"FAILED_CONTAINMENT\"}\n");
      failed=1; /* persistent failed invariant never a positive negative */
    }
    goto done;
  }
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
#ifdef GESHER_FILE_MREMAP_FAILURE_TEST
  if(punch_case && punch_span && observed_source!=MAP_FAILED) {
    FileAliasPage final_rows[128];
    if(FileAliasRegistrySnapshot(observed_source,punch_span*page,final_rows,128)!=(int)punch_span)failed=1;
    else punch_rows("cleanup",observed_source,final_rows,punch_span);
  }
  if((adverse||protection_case) && observed_source!=MAP_FAILED)failed|=ledger("after_cleanup",observed_source,size,page);
#else
  (void)observed_source;
#endif
  struct stat st;long long bytes=-1;unsigned long long hash=0;
  if(fd>=0){if(!fstat(fd,&st))bytes=(long long)st.st_size;else failed=1;hash=checksum(fd,size);if(close(fd))++cleanup;}
  long after=resources();
#ifdef GESHER_FILE_MREMAP_FAILURE_TEST
  printf("{\"event\":\"injection\",\"reached\":[");
  for(int i=0;i<20;++i)printf("%s%d",i?",":"",FileAliasFailureMetric(i));
  printf("],\"outstanding_before_controller_cleanup\":%d}\n",FileAliasFailureOutstanding());
#endif
  printf("{\"event\":\"complete\",\"nonce\":\"%s\",\"case\":\"%s\",\"failed\":%d,\"cleanup_failures\":%d,\"file_size\":%lld,\"file_fnv1a64\":\"%016llx\",\"resources_before\":%ld,\"resources_after\":%ld}\n",nonce,mode,failed,cleanup,bytes,hash,before,after);
  return failed||cleanup||before<0||after!=before;
}
