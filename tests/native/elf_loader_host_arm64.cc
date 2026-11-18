// The full-loader Linux fixture executes x86 ELF code. ARM translation is
// deliberately unavailable here and remains a real Windows translator gate.
#include <gesher/arm64_runtime.h>
#include <cstdlib>
namespace gesher::arm64 {
bool EnsureLoaded() { return false; }
bool Available() { return false; }
void AddCode(const void*, size_t, const char*) { std::abort(); }
void RemoveCode(const void*, size_t) { std::abort(); }
void* BindHostFunction(void*) { std::abort(); }
void UnbindHostFunction(void*) { std::abort(); }
void* HostEntry(const void*) { std::abort(); }
uint64_t Call(const void*, const uint64_t*, size_t) { std::abort(); }
int64_t TlsRegister(size_t, size_t) { std::abort(); }
bool TlsPrepare(int64_t, const void*, size_t, size_t) { std::abort(); }
void TlsPublish(int64_t, const void*, size_t, size_t) { std::abort(); }
bool TlsRollback(int64_t, size_t) { std::abort(); }
void* TlsAddress(int64_t, size_t) { std::abort(); }
void AddVaListFunction(const void*, int) { std::abort(); }
bool IsGuestCode(const void*) { return false; }
uint64_t RunProgram(const void*, const uint64_t*, size_t) { std::abort(); }
void* GuestSymbol(const char*) { std::abort(); }
void* TlsDescStatic() { std::abort(); }
void* TlsDescUnresolvedWeak() { std::abort(); }
uint64_t CallIfuncResolver(const void*) { std::abort(); }
}
