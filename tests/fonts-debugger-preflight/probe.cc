// Harmless CI-only normal-start child for debugger detach/completion proof.
// This imports the real CRT getenv/stdio provider; it contains no font owner,
// ART/JVM, mutation, thread, debugger API or callback implementation.
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc,char**) {
  const char* ci=std::getenv("GITHUB_ACTIONS");
  const char* canary=std::getenv("FONT_DEBUGGER_PREFLIGHT_CANARY");
  if(argc!=1||!ci||std::strcmp(ci,"true")||!canary||std::strcmp(canary,"probe-v1"))return 2;
  std::puts("FONT_DEBUGGER_PREFLIGHT_READY");
  if(std::fflush(stdout)!=0)return 3;
  if(std::getchar()!='\n')return 4;
  std::puts("FONT_DEBUGGER_PREFLIGHT_DONE");
  return std::fflush(stdout)==0?0:5;
}
