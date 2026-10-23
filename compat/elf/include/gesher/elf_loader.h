// gesher: an ELF loader for x86-64 Android shared objects on Windows.
//
// Android apps ship their native code as ELF .so files built by the NDK for
// Linux. Windows cannot load them; LoadLibrary wants PE. This loader maps an
// ELF64 x86-64 shared object into memory itself (through gesher-base's mmap /
// mprotect), applies its relocations, resolves its DT_NEEDED dependencies and
// undefined symbols against the bionic shim (compat/bionic) and other loaded
// objects, runs its init arrays, and looks up its exported symbols. The result
// is a handle usable like dlopen's: JNI_OnLoad and Java_* entry points are
// found through Lookup().
//
// See docs/native-libs.md and docs/decisions/0014-native-libs-*.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

namespace gesher::elf {

// A loaded ELF object. Opaque; created by Load(), owned by the loader.
struct Library;

// Resolves a symbol the loaded object imports but none of the ELF objects on
// its dependency graph define: the bionic shim's libc/libm/libdl/liblog/...
// symbols. Returns the address, or nullptr if unknown.
using SymbolResolver = std::function<void*(const char* name)>;

// Registers the process-wide resolver for symbols not found among loaded ELF
// objects (compat/bionic installs it). Without one, only self-contained
// objects load.
void SetBionicResolver(SymbolResolver resolver);

// Options for a load.
struct LoadOptions {
  // Directories searched for a DT_NEEDED soname, in order, before the resolver.
  // The APK's extracted lib/x86_64 goes here.
  std::vector<std::string> search_paths;
  // When true, missing DT_NEEDED libraries that the bionic resolver can satisfy
  // symbol-by-symbol (libc.so, libm.so, libdl.so, liblog.so, libandroid.so,
  // libc++_shared.so provided by the shim) are not an error.
  bool allow_shimmed_needed = true;
};

// Loads the ELF object at `path`. On success returns a handle and runs its
// init functions; on failure returns nullptr and sets *error. The file must be
// an ELF64 little-endian x86-64 ET_DYN object.
Library* Load(const char* path, std::string* error, const LoadOptions& options = {});

// Loads from a buffer already in memory (e.g. a segment read from the APK zip).
// `name` names it for diagnostics and soname matching.
Library* LoadFromMemory(const char* name, const void* data, size_t size, std::string* error,
                        const LoadOptions& options = {});

// The address of exported symbol `name` in `library` (and its dependencies),
// or nullptr. STT_GNU_IFUNC symbols are resolved to their target.
void* Lookup(Library* library, const char* name);

// Looks up `name` across every loaded ELF object (for RTLD_DEFAULT-style
// lookup, and for the bionic resolver to find symbols one object exports to
// another). nullptr if not found.
void* LookupGlobal(const char* name);

// Returns the already-loaded object whose soname or file base name is `name`,
// or nullptr. Load() uses this to share dependencies.
Library* Find(const char* name);

// The soname (DT_SONAME, else the file base name) of a loaded object.
const char* Soname(Library* library);

// Runs the object's DT_FINI_ARRAY / DT_FINI and marks it closed. The mapping is
// kept (handles may still be held); full unmap is not implemented.
void Close(Library* library);

// Per-thread setup: makes %fs point at this thread's bionic TLS block, so the
// loaded code's thread-local and stack-protector accesses work. Call on every
// thread that will enter loaded native code. Safe to call more than once.
// Returns false if %fs cannot be set up on this host (see the fsbase probe).
bool SetupThreadTls();

}  // namespace gesher::elf
