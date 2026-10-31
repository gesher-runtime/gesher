/* CI-only owned failure backend test. Mode3 is currently an EXPECTED RED
 * correctness blocker, not a passed negative and never production acceptance. */
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
  int fd, observed, backend_cleanup, source_cleanup, close_result, unlink_result, error;
  void *source, *alias;
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
  observed=FileAliasFailureOutstanding();
  /* Failure source must still be a valid view with its actual data. */
  int source_intact=((unsigned char*)source)[0]==0x5a;
  if(alias!=MAP_FAILED)munmap(alias,(size_t)page);
  backend_cleanup=FileAliasFailureCleanup();
  source_cleanup=munmap(source,(size_t)page);
  close_result=close(fd);
  unlink_result=unlink(path);
  printf("{\"mode\":%d,\"failed\":%d,\"errno\":%d,\"source_intact\":%d,"
         "\"outstanding_before_controller_cleanup\":%d,\"backend_cleanup\":%d,"
         "\"source_cleanup\":%d,\"close\":%d,\"unlink\":%d}\n",mode,alias==MAP_FAILED,error,
         source_intact,observed,backend_cleanup,source_cleanup,close_result,unlink_result);
  return !(alias==MAP_FAILED&&error!=0&&source_intact&&observed==0&&backend_cleanup==0&&
           source_cleanup==0&&close_result==0&&unlink_result==0);
}
