// libutils' Windows Threads.cpp leaves out androidSetThreadName, which
// libbinder's ProcessState names its pooled threads with. Processes with
// libandroid_runtime get it from compat/framework/android_runtime.cc; the
// native binder programs (servicemanager, libbinder_probe) from here.
#include <pthread.h>

extern "C" void androidSetThreadName(const char* name) {
  pthread_setname_np(pthread_self(), name);
}
