// gesher bionic shim: the libc/libm/libdl/liblog symbols an app's .so imports,
// as System V entry points over gesher-base and the CRT.
//
// A non-variadic CRT or gesher-base function is exposed through a generated
// sysv_abi wrapper (Sysv<F>); functions with callbacks, data symbols (the std
// streams, the stack cookie) and the bionic-specific entry points are written
// out. Variadic entry points (the printf family, __android_log_print) cannot
// be forwarded generally — va_start is not allowed in a sysv_abi function on
// Windows (docs/decisions/0011) — so the shim offers the non-variadic stdio
// and logging the test libraries use, and a best-effort fixed-argument form of
// the variadic ones; a general vararg bridge is future work.
//
// install_bionic_shim() registers the resolver with the ELF loader.

#include "abi_types.h"

#include <windows.h>

#include <fcntl.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <gesher/elf_loader.h>

namespace gesher::bionic {

// The pthread entry points (pthread_bionic.cc).
extern "C" {
int BIONIC_SYSV gesher_bionic_pthread_create(bionic_pthread_t*, const bionic_pthread_attr_t*,
                                            void* (BIONIC_SYSV*)(void*), void*);
int BIONIC_SYSV gesher_bionic_pthread_join(bionic_pthread_t, void**);
int BIONIC_SYSV gesher_bionic_pthread_detach(bionic_pthread_t);
bionic_pthread_t BIONIC_SYSV gesher_bionic_pthread_self(void);
int BIONIC_SYSV gesher_bionic_pthread_equal(bionic_pthread_t, bionic_pthread_t);
void BIONIC_SYSV gesher_bionic_pthread_exit(void*);
int BIONIC_SYSV gesher_bionic_pthread_mutex_init(bionic_pthread_mutex_t*,
                                                const bionic_pthread_mutexattr_t*);
int BIONIC_SYSV gesher_bionic_pthread_mutex_destroy(bionic_pthread_mutex_t*);
int BIONIC_SYSV gesher_bionic_pthread_mutex_lock(bionic_pthread_mutex_t*);
int BIONIC_SYSV gesher_bionic_pthread_mutex_trylock(bionic_pthread_mutex_t*);
int BIONIC_SYSV gesher_bionic_pthread_mutex_unlock(bionic_pthread_mutex_t*);
int BIONIC_SYSV gesher_bionic_pthread_cond_init(bionic_pthread_cond_t*,
                                               const bionic_pthread_condattr_t*);
int BIONIC_SYSV gesher_bionic_pthread_cond_destroy(bionic_pthread_cond_t*);
int BIONIC_SYSV gesher_bionic_pthread_cond_wait(bionic_pthread_cond_t*, bionic_pthread_mutex_t*);
int BIONIC_SYSV gesher_bionic_pthread_cond_signal(bionic_pthread_cond_t*);
int BIONIC_SYSV gesher_bionic_pthread_cond_broadcast(bionic_pthread_cond_t*);
int BIONIC_SYSV gesher_bionic_pthread_once(bionic_pthread_once_t*, void (BIONIC_SYSV*)(void));
int BIONIC_SYSV gesher_bionic_pthread_key_create(bionic_pthread_key_t*, void (BIONIC_SYSV*)(void*));
int BIONIC_SYSV gesher_bionic_pthread_key_delete(bionic_pthread_key_t);
void* BIONIC_SYSV gesher_bionic_pthread_getspecific(bionic_pthread_key_t);
int BIONIC_SYSV gesher_bionic_pthread_setspecific(bionic_pthread_key_t, const void*);
}

namespace {

// A sysv_abi wrapper around a non-variadic function F (CRT or gesher-base).
template <auto F>
struct Sysv;
template <class R, class... A, R (*F)(A...)>
struct Sysv<F> {
  static R BIONIC_SYSV Call(A... args) { return F(args...); }
};
#define WRAP(fn) reinterpret_cast<void*>(&Sysv<&fn>::Call)
// For an overloaded function (the math ones): select the overload by signature.
#define WRAPS(sig, fn) reinterpret_cast<void*>(&Sysv<static_cast<sig>(&fn)>::Call)
#define ENTRY(name, ptr) ((*table)[name] = reinterpret_cast<void*>(ptr))

// Small named entry points (lambdas with an ABI attribute are awkward).
int BIONIC_SYSV ShimUsleep(unsigned us) {
  Sleep(us / 1000);
  return 0;
}
int BIONIC_SYSV ShimAtexit(void (BIONIC_SYSV*)(void)) { return 0; }

// --- data symbols ----------------------------------------------------------

// The standard streams, so the .so's stdio (fwrite/fputs) reaches the CRT's.
FILE* g_stdin = stdin;
FILE* g_stdout = stdout;
FILE* g_stderr = stderr;

// The stack-protector cookie, for a .so compiled with the global guard model
// (the TLS model reads the same value from %fs:0x28; see tls.cc).
uintptr_t g_stack_chk_guard = 0x0123456789abcdefULL;

// --- bionic-specific entry points -----------------------------------------

void BIONIC_SYSV StackChkFail() {
  std::fprintf(stderr, "gesher bionic: stack smashing detected\n");
  std::abort();
}

// __tls_get_addr(TlsIndex*): general-dynamic TLS.
struct TlsIndex {
  size_t module_id;
  size_t offset;
};
void* BIONIC_SYSV TlsGetAddr(TlsIndex* ti) {
  return gesher::elf::TlsGetAddr(ti->module_id, ti->offset);
}

// C++ static destructor registration. The test libraries do not rely on
// destructors running; they are recorded and run on __cxa_finalize best effort.
struct AtExitEntry {
  void (BIONIC_SYSV* func)(void*);
  void* arg;
};
std::mutex g_atexit_mutex;
std::vector<AtExitEntry> g_atexit;

int BIONIC_SYSV CxaAtexit(void (BIONIC_SYSV* func)(void*), void* arg, void* /*dso*/) {
  std::lock_guard<std::mutex> lock(g_atexit_mutex);
  g_atexit.push_back({func, arg});
  return 0;
}
void BIONIC_SYSV CxaFinalize(void* /*dso*/) {
  std::lock_guard<std::mutex> lock(g_atexit_mutex);
  for (auto it = g_atexit.rbegin(); it != g_atexit.rend(); ++it) {
    if (it->func) it->func(it->arg);
  }
  g_atexit.clear();
}
void BIONIC_SYSV CxaPureVirtual() {
  std::fprintf(stderr, "gesher bionic: pure virtual function called\n");
  std::abort();
}

// --- liblog (non-variadic, and a fixed-argument print) ---------------------

int BIONIC_SYSV AndroidLogWrite(int prio, const char* tag, const char* text) {
  std::fprintf(stderr, "[%d] %s: %s\n", prio, tag ? tag : "", text ? text : "");
  return 0;
}
// Variadic __android_log_print cannot be forwarded (no sysv va_start on
// Windows); this prints the format literally, which is enough for the test
// libraries (which log fixed strings). A general bridge is future work.
int BIONIC_SYSV AndroidLogPrint(int prio, const char* tag, const char* fmt) {
  return AndroidLogWrite(prio, tag, fmt);
}

// --- libdl (through the ELF loader) ----------------------------------------

void* BIONIC_SYSV DlOpen(const char* filename, int /*flags*/) {
  if (filename == nullptr) return nullptr;
  std::string error;
  return gesher::elf::Load(filename, &error);
}
void* BIONIC_SYSV DlSym(void* handle, const char* symbol) {
  if (handle == nullptr || handle == reinterpret_cast<void*>(-1)) {
    return gesher::elf::LookupGlobal(symbol);
  }
  return gesher::elf::Lookup(static_cast<gesher::elf::Library*>(handle), symbol);
}
int BIONIC_SYSV DlClose(void* /*handle*/) { return 0; }
const char* BIONIC_SYSV DlError() { return nullptr; }

// A 3-argument open: a .so that calls open(path, flags) leaves the mode
// register undefined, which open ignores without O_CREAT.
int BIONIC_SYSV Open3(const char* path, int flags, int mode) { return open(path, flags, mode); }

// --- the symbol table ------------------------------------------------------

std::unordered_map<std::string, void*>& Table() {
  static std::unordered_map<std::string, void*>* t = [] {
    auto* table = new std::unordered_map<std::string, void*>();
    // Memory.
    ENTRY("malloc", WRAP(::malloc));
    ENTRY("free", WRAP(::free));
    ENTRY("calloc", WRAP(::calloc));
    ENTRY("realloc", WRAP(::realloc));
    ENTRY("posix_memalign", WRAP(::posix_memalign));
    // mem/str.
    ENTRY("memcpy", WRAP(::memcpy));
    ENTRY("memmove", WRAP(::memmove));
    ENTRY("memset", WRAP(::memset));
    ENTRY("memcmp", WRAP(::memcmp));
    ENTRY("memchr", WRAP(::memchr));
    ENTRY("strlen", WRAP(::strlen));
    ENTRY("strnlen", WRAP(::strnlen));
    ENTRY("strcmp", WRAP(::strcmp));
    ENTRY("strncmp", WRAP(::strncmp));
    ENTRY("strcpy", WRAP(::strcpy));
    ENTRY("strncpy", WRAP(::strncpy));
    ENTRY("strcat", WRAP(::strcat));
    ENTRY("strncat", WRAP(::strncat));
    ENTRY("strchr", WRAP(::strchr));
    ENTRY("strrchr", WRAP(::strrchr));
    ENTRY("strstr", WRAP(::strstr));
    ENTRY("strdup", WRAP(::strdup));
    ENTRY("strtol", WRAP(::strtol));
    ENTRY("strtoul", WRAP(::strtoul));
    ENTRY("strtoll", WRAP(::strtoll));
    ENTRY("strtod", WRAP(::strtod));
    ENTRY("atoi", WRAP(::atoi));
    // stdio (non-variadic).
    ENTRY("fwrite", WRAP(::fwrite));
    ENTRY("fread", WRAP(::fread));
    ENTRY("fputs", WRAP(::fputs));
    ENTRY("fputc", WRAP(::fputc));
    ENTRY("putchar", WRAP(::putchar));
    ENTRY("puts", WRAP(::puts));
    ENTRY("fflush", WRAP(::fflush));
    ENTRY("fopen", WRAP(::fopen));
    ENTRY("fclose", WRAP(::fclose));
    // stdio data symbols (bionic names).
    ENTRY("stdin", &g_stdin);
    ENTRY("stdout", &g_stdout);
    ENTRY("stderr", &g_stderr);
    // Process.
    ENTRY("abort", WRAP(::abort));
    ENTRY("exit", WRAP(::exit));
    ENTRY("atexit", ShimAtexit);
    // libm (non-variadic; forwarded to the CRT, overloads selected by signature).
    ENTRY("sin", WRAPS(double (*)(double), ::sin));
    ENTRY("cos", WRAPS(double (*)(double), ::cos));
    ENTRY("tan", WRAPS(double (*)(double), ::tan));
    ENTRY("pow", WRAPS(double (*)(double, double), ::pow));
    ENTRY("sqrt", WRAPS(double (*)(double), ::sqrt));
    ENTRY("exp", WRAPS(double (*)(double), ::exp));
    ENTRY("log", WRAPS(double (*)(double), ::log));
    ENTRY("floor", WRAPS(double (*)(double), ::floor));
    ENTRY("ceil", WRAPS(double (*)(double), ::ceil));
    ENTRY("fabs", WRAPS(double (*)(double), ::fabs));
    ENTRY("fmod", WRAPS(double (*)(double, double), ::fmod));
    // unistd / fd (non-variadic).
    ENTRY("open", Open3);
    ENTRY("close", WRAP(::close));
    ENTRY("read", WRAP(::read));
    ENTRY("write", WRAP(::write));
    ENTRY("lseek", WRAP(::lseek));
    ENTRY("usleep", ShimUsleep);
    // pthread.
    ENTRY("pthread_create", gesher_bionic_pthread_create);
    ENTRY("pthread_join", gesher_bionic_pthread_join);
    ENTRY("pthread_detach", gesher_bionic_pthread_detach);
    ENTRY("pthread_self", gesher_bionic_pthread_self);
    ENTRY("pthread_equal", gesher_bionic_pthread_equal);
    ENTRY("pthread_exit", gesher_bionic_pthread_exit);
    ENTRY("pthread_mutex_init", gesher_bionic_pthread_mutex_init);
    ENTRY("pthread_mutex_destroy", gesher_bionic_pthread_mutex_destroy);
    ENTRY("pthread_mutex_lock", gesher_bionic_pthread_mutex_lock);
    ENTRY("pthread_mutex_trylock", gesher_bionic_pthread_mutex_trylock);
    ENTRY("pthread_mutex_unlock", gesher_bionic_pthread_mutex_unlock);
    ENTRY("pthread_cond_init", gesher_bionic_pthread_cond_init);
    ENTRY("pthread_cond_destroy", gesher_bionic_pthread_cond_destroy);
    ENTRY("pthread_cond_wait", gesher_bionic_pthread_cond_wait);
    ENTRY("pthread_cond_signal", gesher_bionic_pthread_cond_signal);
    ENTRY("pthread_cond_broadcast", gesher_bionic_pthread_cond_broadcast);
    ENTRY("pthread_once", gesher_bionic_pthread_once);
    ENTRY("pthread_key_create", gesher_bionic_pthread_key_create);
    ENTRY("pthread_key_delete", gesher_bionic_pthread_key_delete);
    ENTRY("pthread_getspecific", gesher_bionic_pthread_getspecific);
    ENTRY("pthread_setspecific", gesher_bionic_pthread_setspecific);
    // liblog.
    ENTRY("__android_log_write", AndroidLogWrite);
    ENTRY("__android_log_print", AndroidLogPrint);
    // libdl.
    ENTRY("dlopen", DlOpen);
    ENTRY("dlsym", DlSym);
    ENTRY("dlclose", DlClose);
    ENTRY("dlerror", DlError);
    // C runtime support.
    ENTRY("__stack_chk_fail", StackChkFail);
    ENTRY("__stack_chk_guard", &g_stack_chk_guard);
    ENTRY("__cxa_atexit", CxaAtexit);
    ENTRY("__cxa_finalize", CxaFinalize);
    ENTRY("__cxa_pure_virtual", CxaPureVirtual);
    ENTRY("__tls_get_addr", TlsGetAddr);
    return table;
  }();
  return *t;
}

}  // namespace

// Registers the resolver with the ELF loader.
void InstallBionicShim() {
  gesher::elf::SetBionicResolver([](const char* name) -> void* {
    auto& table = Table();
    auto it = table.find(name);
    return it != table.end() ? it->second : nullptr;
  });
}

}  // namespace gesher::bionic
