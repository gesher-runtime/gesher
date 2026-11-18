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

// An ELF object or actual virtual provider. Opaque; owned by the loader.
struct Library;
struct Namespace;

// Acquire before an embedding registry's mutex when configuring namespaces.
// The loader already holds this recursive lock while calling constructors;
// the shared order prevents registry -> loader / loader -> registry deadlocks.
// Release the embedding registry's mutex before entering application code.
class ScopedLoaderOperation {
 public:
  ScopedLoaderOperation();
  ~ScopedLoaderOperation();
  ScopedLoaderOperation(const ScopedLoaderOperation&) = delete;
  ScopedLoaderOperation& operator=(const ScopedLoaderOperation&) = delete;
};

enum class Architecture { kAuto = 0, kX86_64 = 1, kAarch64 = 2 };
enum class ProviderVisibility { kPrivate = 0, kNdkPublic = 1, kDevicePublic = 2 };
enum class LookupKind { kHandle, kDefault, kNext, kMain };

struct LoaderContext {
  Namespace* name_space = nullptr;
  const void* caller_address = nullptr;
  Architecture architecture = Architecture::kAuto;
};

struct NamespaceOptions {
  std::string name;
  std::vector<std::string> ld_library_paths;
  std::vector<std::string> default_library_paths;
  std::vector<std::string> permitted_paths;
  Namespace* parent = nullptr;
  int target_sdk_version = 36;
  bool isolated = true;
  bool shared = false;
  bool exempt_list_enabled = false;
};

// Namespace identities remain stable while their loaded objects are retained.
Namespace* DefaultNamespace();
Namespace* CreateNamespace(const NamespaceOptions& options, std::string* error);
bool LinkNamespaces(Namespace* from, Namespace* to,
                    const std::vector<std::string>& sonames, std::string* error);
bool SetAnonymousNamespace(Namespace* name_space, std::string* error);
void SetDefaultLibraryPath(const std::vector<std::string>& paths);
std::vector<std::string> DefaultLibraryPath();
void SetDefaultTargetSdkVersion(int target_sdk_version);
int DefaultTargetSdkVersion();

// Resolves a symbol the loaded object imports but none of the ELF objects on
// its dependency graph define: the bionic shim's libc/libm/libdl/liblog/...
// symbols. Returns the address, or nullptr if unknown.
using SymbolResolver = std::function<void*(const char* name)>;

// An actual virtual library. The callback searches owned exports only; the
// loader walks its declared dependencies using the same scope rules as ELF.
struct ProviderOptions {
  std::string soname;
  std::vector<std::string> path_aliases;
  std::vector<std::string> dependencies;
  // A port may add a compiled provider after initial registration. This
  // snapshot describes the actual implementation subset, never a filter that
  // hides an unavailable required dependency. Empty uses the static list.
  std::function<std::vector<std::string>()> dependency_snapshot;
  ProviderVisibility visibility = ProviderVisibility::kPrivate;
  unsigned architecture_mask = 3;
  SymbolResolver lookup;
  std::function<bool()> available;
};
struct ProviderInfo {
  std::string soname;
  ProviderVisibility visibility;
  unsigned architecture_mask;
};
Library* RegisterProvider(const ProviderOptions& options, std::string* error);
std::vector<ProviderInfo> AvailableProviders();
bool IsProvider(const Library* library);
Namespace* LibraryNamespace(const Library* library);
Library* FindCaller(const void* address);
// Legacy embedder callback retained for source compatibility. It cannot
// establish provider identity or satisfy a scoped dependency; use RegisterProvider.
using LibraryProvider = std::function<bool(const char* soname)>;

// Registers internal host/translator plumbing and explicit diagnostic fallback.
// Ordinary guest loads resolve only through their ELF/provider graph.
void SetBionicResolver(SymbolResolver resolver, LibraryProvider library_provider = {});

// Options for a load.
struct LoadOptions {
  LoaderContext context;
  int flags = 2;  // Android LP64 RTLD_NOW; RTLD_LOCAL is zero.
  // Explicit embedder search paths for this operation, after requester RUNPATH
  // and before namespace defaults. Ordinary classloader paths belong to its
  // namespace, so one request never modifies another loader's search state.
  std::vector<std::string> search_paths;
  // Allows actual registered virtual providers to satisfy DT_NEEDED.
  // An unrecognized or unavailable dependency is still a load failure.
  bool allow_shimmed_needed = true;
  // When true, an undefined function import that nothing defines is bound to
  // a stub that reports it when called (SetUnresolvedHandler), and an
  // undefined data import to a placeholder in an inaccessible region whose
  // use names the symbol and aborts, instead of failing the load. TLS imports
  // must always resolve.
  bool stub_unresolved_functions = false;
  // Explicit embedder/diagnostic support. Ordinary guest and JNI loads never
  // resolve an import through the aggregate host table outside its scope.
  bool allow_unscoped_resolver = false;
  // Optional stable identity for a memory source. Null means a fresh source;
  // the diagnostic name alone never establishes file identity.
  const void* memory_identity = nullptr;
  // Only the initial dynamic executable, before its dependencies. Enables
  // link-time local-exec TLS offsets when its PT_TLS layout is compatible.
  bool main_executable = false;
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
// or nullptr. STT_GNU_IFUNC symbols are resolved to their target; STT_TLS
// symbols identify the calling thread's instance of the selected object.
void* Lookup(Library* library, const char* name);

// Explicit version lookup (dlvsym). nullptr version is ordinary lookup;
// non-default/hidden definitions can be selected by their exact version name.
// Selection and address availability are separate: an IFUNC may successfully
// resolve to nullptr, while missing current-thread TLS storage is unavailable.
// A selected-but-unavailable symbol must not fall back to another provider.
enum class LookupStatus { kNotFound, kResolved, kUnavailable };
void* LookupVersion(Library* library, const char* name, const char* version,
                    LookupStatus* status = nullptr);

// Caller-aware libdl lookup. An explicit handle uses its own namespace; NEXT
// needs an identified ELF caller. Errors own their text for the caller to copy.
void* LookupScoped(Library* handle, const char* name, const char* version,
                   LookupKind kind, const LoaderContext& context, std::string* error,
                   LookupStatus* status = nullptr);

// RTLD_DEFAULT lookup for an unidentified host caller in the configured
// anonymous/default namespace. Use LookupScoped for actual guest caller scope.
void* LookupGlobal(const char* name);
void* LookupGlobalVersion(const char* name, const char* version, LookupStatus* status = nullptr);

// Resolves `name` through the bionic resolver (SetBionicResolver) alone: the
// host (shim) definition, or nullptr. The arm64 translator wraps some of them.
void* ResolveBionic(const char* name);

// Is `library` an arm64-v8a object (run by the arm64 translator, ADR 0047)?
bool IsAarch64(Library* library);

// Finds a visible bare SONAME in the default namespace and its allowed links.
// Paths and missing SONAMEs require Load's opened-file identity check.
Library* Find(const char* name);

// Actual DT_SONAME; absent stays empty for target SDK >= 23.
const char* Soname(Library* library);

// Runs the object's DT_FINI_ARRAY / DT_FINI and marks it closed. The mapping is
// kept (handles may still be held); full unmap is not implemented.
void Close(Library* library);

// What dl_iterate_phdr reports for one loaded object.
struct PhdrInfo {
  uintptr_t addr = 0;            // load bias
  const char* name = "";         // resolved source path, or memory diagnostic name
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
  const char* soname = "";       // dli_fname: resolved source path (legacy field name)
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
  bool used_unwind_info = false;  // .eh_frame_hdr supplied ranges or boundary anchors
  size_t syscalls = 0;         // raw `syscall` instructions turned into emulated traps
  bool self_unpacking = false; // code unpacked at run time: rewritten on first execution
};
FsRewriteCounts FsRewrites(Library* library);

// --- Raw system calls and code made at run time (ADR 0084) --------------------
// A raw Linux x86-64 system call made by loaded code (a `syscall` instruction
// the loader turned into a trap): `nr` and the six arguments as the kernel
// takes them; returns the kernel's result, or -errno. The bionic shim installs
// it (it maps the call to its own functions); without one every raw system
// call returns -ENOSYS.
using RawSyscallHandler = int64_t (*)(int64_t nr, const int64_t args[6]);
void SetRawSyscallHandler(RawSyscallHandler handler);

// mprotect/mmap/munmap for loaded code (the bionic shim's): as gesher-base's,
// except that pages of a loaded x86-64 object that are made executable are
// kept non-executable until their first execution, when the loader rewrites
// the code then in them (%fs accesses, raw system calls) and gives them the
// requested protection. Same results and errno as mprotect/mmap/munmap.
// `raw_syscall`: the call is a raw system call of loaded code (a packer's
// unpacker); executable memory it maps or protects outside every loaded
// object waits for its first execution too (UPX 5 maps its decompressor from
// a memfd). libc's mmap/mprotect (a JIT's) leave such memory as asked.
int ProtectLoadedMemory(void* addr, size_t len, int prot, bool raw_syscall = false);
void* MapLoadedMemory(void* addr, size_t len, int prot, int flags, int fd, int64_t offset,
                      bool raw_syscall = false);
int UnmapLoadedMemory(void* addr, size_t len);
// msync for loaded code: also writes back the private copies made of shared
// file mappings gesher-base cannot place (MapLoadedMemory), as munmap does.
int SyncLoadedMemory(void* addr, size_t len, int flags);

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
