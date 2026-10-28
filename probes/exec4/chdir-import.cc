// Match the bionic wrapper's header order and inspect the actual relocation.
#include <direct.h>
#include <unistd.h>
extern "C" int exec4_chdir_import(const char* path) { return chdir(path); }
