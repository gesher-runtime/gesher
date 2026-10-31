/* CI-only backend. Mode3 is a FAILED final-release invariant, not a pass.
 * Map failure now acquires no HANDLE and does not exercise close recovery. */
#define _GNU_SOURCE
#include "alias_failure_adapter.h"
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
int main(int argc,char** argv) {
  char path[]="file-mremap-failure-XXXXXX";
  long page=sysconf(_SC_PAGESIZE);
  int mode=argc==2?atoi(argv[1]):0;
  int fd, before_cleanup, backend_cleanup, source_cleanup, alias_cleanup=-2;
  int close_result, unlink_result, error, source_intact;
  void *source,*alias;
  if(mode<1||mode>3||page<=0)return 2;
  fd=mkstemp(path);
  if(fd<0)return 2;
  if(ftruncate(fd,page)!=0){close(fd);unlink(path);return 2;}
  source=mmap(NULL,(size_t)page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(source==MAP_FAILED){close(fd);unlink(path);return 2;}
  ((unsigned char*)source)[0]=0x5a;
  FileAliasFailureReset(mode);
  errno=0;
  alias=mremap(source,0,(size_t)page,MREMAP_MAYMOVE);
  error=errno;
  source_intact=((unsigned char*)source)[0]==0x5a;
  source_cleanup=munmap(source,(size_t)page); // source-first, section must survive
  if(alias!=MAP_FAILED) {
    source_intact=source_intact&&((unsigned char*)alias)[0]==0x5a;
    alias_cleanup=munmap(alias,(size_t)page); // final promoted release, mode3 here
  }
  before_cleanup=FileAliasFailureOutstanding();
  backend_cleanup=FileAliasFailureCleanup();
  close_result=close(fd);
  unlink_result=unlink(path);
  printf("{\"mode\":%d,\"map_failed\":%d,\"errno\":%d,\"source_intact\":%d,"
         "\"map_calls\":%d,\"close_calls\":%d,\"map_injected\":%d,\"close_injected\":%d,"
         "\"outstanding_before_controller_cleanup\":%d,\"backend_cleanup\":%d,"
         "\"source_cleanup\":%d,\"alias_cleanup\":%d,\"close\":%d,\"unlink\":%d}\n",
         mode,alias==MAP_FAILED,error,source_intact,FileAliasFailureMetric(0),FileAliasFailureMetric(1),
         FileAliasFailureMetric(2),FileAliasFailureMetric(3),before_cleanup,backend_cleanup,
         source_cleanup,alias_cleanup,close_result,unlink_result);
  if(mode==3)return 1; // permanent failure is honest failed containment
  if(mode==1)return !(alias==MAP_FAILED&&error!=0&&FileAliasFailureMetric(2)==1&&
                    FileAliasFailureMetric(1)==0&&source_intact&&source_cleanup==0&&
                    before_cleanup==0&&backend_cleanup==0&&close_result==0&&unlink_result==0);
  return !(alias!=MAP_FAILED&&source_intact&&source_cleanup==0&&alias_cleanup==0&&
           FileAliasFailureMetric(1)==1&&FileAliasFailureMetric(3)==0&&before_cleanup==0&&
           backend_cleanup==0&&close_result==0&&unlink_result==0);
}
