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
  // When true, an undefined function import that nothing defines is bound to
  // a stub that reports it when called (SetUnresolvedHandler), and an
  // undefined data import to a placeholder in an inaccessible region whose
  // use names the symbol and aborts, instead of failing the load. TLS imports
  // must always resolve.
  bool stub_unresolved_functions = true;
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

// Resolves `name` through the bionic resolver (SetBionicResolver) alone: the
// host (shim) definition, or nullptr. The arm64 translator wraps some of them.
void* ResolveBionic(const char* name);

// Is `library` an arm64-v8a object (run by the arm64 translator, ADR 0047)?
bool IsAarch64(Library* library);

// Returns the already-loaded object whose soname or file base name is `name`,
// or nullptr. Load() uses this to share dependencies.
Library* Find(const char* name);

// The soname (DT_SONAME, else the file base name) of a loaded object.
const char* Soname(Library* library);

// Runs the object's DT_FINI_ARRAY / DT_FINI and marks it closed. The mapping is
// kept (handles may still be held); full unmap is not implemented.
void Close(Library* library);

// What dl_iterate_phdr reports for one loaded object.
struct PhdrInfo {
  uintptr_t addr = 0;            // load bias
  const char* name = "";         // soname
  const void* phdr = nullptr;    // Elf64_Phdr[phnum], inside the mapping
  uint16_t phnum = 0;
  size_t tls_modid = 0;          // 0 when the object has no PT_TLS
  uint64_t adds = 0;             // objects loaded so far (dlpi_adds)
};

// Calls `fn` for each loaded (not closed) object, in load order, until it
// returns a non-zero value, which is returned. 0 if every call returned 0.
int ForEachLoaded(const std::function<int(const PhdrInfo&)>& fn);

// For dladdr: the object containing `addr` and the closest exported symbol at
// or below it. Returns false if `addr` is in no loaded object.
struct AddressInfo {
  const char* soname = "";
  void* base = nullptr;          // the mapping's start
  const char* symbol = nullptr;  // nullptr if no symbol covers it
  void* symbol_addr = nullptr;
};
bool FindAddress(const void* addr, AddressInfo* info);

// Called (System V ABI, from loaded code) when the object calls a function
// that no loaded object or the shim defines. The loader binds such imports to
// a per-symbol stub that passes the name here, so an object whose unused
// imports are missing still loads. Without a handler the stub aborts.
using UnresolvedHandler = void (*)(const char* name);
void SetUnresolvedHandler(UnresolvedHandler handler);

// Per-thread setup: builds this thread's bionic TLS block and stores its
// thread pointer in the TEB slot that the loaded code's rewritten %fs:0
// accesses read (ADR 0014). Call on every thread that will enter loaded native
// code with initial-exec TLS; the stack guard (%fs:0x28, rewritten to the TEB)
// and __tls_get_addr work without it. Safe to call more than once. Returns
// false only if no inline TLS slot or memory is available.
bool SetupThreadTls();

// What the loader's %fs rewrite did to an object (fs_rewrite.h).
struct FsRewriteCounts {
  size_t stack_guard = 0;      // %fs:0x28 -> %gs:0x30
  size_t thread_pointer = 0;   // %fs:0 -> %gs:<TLS slot>
  size_t stubbed = 0;          // %fs:(%reg) replayed out of line
  size_t trapped = 0;          // not rewritable: int3 + diagnostic if reached
  size_t decode_errors = 0;    // bytes skipped while decoding code
  size_t bytes_decoded = 0;
  bool used_unwind_info = false;  // code found through .eh_frame_hdr FDEs
};
FsRewriteCounts FsRewrites(Library* library);

// __tls_get_addr's worker: the address of loaded module `module_id`'s TLS
// variable at `offset` in the current thread. The bionic shim wraps this as the
// System V __tls_get_addr the loaded code calls for general-dynamic TLS.
void* TlsGetAddr(size_t module_id, size_t offset);

// --- Executables (docs/exec.md) ----------------------------------------------
// An Android executable is a PIE: an ET_DYN object with PT_INTERP
// (/system/bin/linker64) whose entry point is the NDK's _start. Load() maps it
// like a shared object; the launcher (compat/elf/exec) then enters it.

// What a launcher needs to start a loaded executable at its entry point.
struct ExecutableInfo {
  void* entry = nullptr;        // e_entry + load bias (_start); null if e_entry is 0
  const void* phdr = nullptr;   // Elf64_Phdr[phnum] inside the mapping (AT_PHDR)
  uint16_t phnum = 0;           // AT_PHNUM
  void* base = nullptr;         // the mapping's start
  bool has_interp = false;      // PT_INTERP present: built as an executable
  bool aarch64 = false;         // an arm64-v8a program: entered through the translator
};
// Fills `info` for `library`; false if `library` is null.
bool GetExecutableInfo(Library* library, ExecutableInfo* info);

// The (argc, argv, envp) that init functions (DT_PREINIT_ARRAY, DT_INIT,
// DT_INIT_ARRAY) are called with from now on, as bionic's linker calls them.
// Until this is called they get argc 0 and null argv/envp. The arrays must
// outlive every later load.
void SetInitArguments(int argc, char** argv, char** envp);

}  // namespace gesher::elf
