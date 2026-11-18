// gesher: the bionic shim, the libc/libm/libdl/liblog/pthread symbols an
// Android x86-64 .so imports, as System V entry points over gesher-base and
// the CRT (compat/bionic). See docs/native-libs.md.
#pragma once

namespace gesher::bionic {

// Registers the actual library providers with the ELF loader. Ordinary guest
// lookups reach their owned exports through the selected dependency scope.
// Also installs the separate resolver used by internal host/translator code.
void InstallBionicShim();

// Initializes the compiled ART JNI bridge and its owned invocation exports.
// Call before taking a classloader's provider snapshot. This does not create
// a VM, enter guest code, allocate guest TLS, or build guest proxy tables.
void InstallJniBridge();

// Internal host/translator lookup in the combined shim table. This is not an
// Android guest visibility rule and must not be an ordinary libdl fallback.
void* LookupBionicSymbol(const char* name);

// Adds an internal host/translator symbol. It is not exported by any Android
// library. Existing provider ownership is unchanged by this overload.
void AddBionicSymbol(const char* name, void* address);

// Adds (or replaces) a real export owned by `soname` and also updates the
// internal combined table. Register a shared export once for each actual
// owning library; provider callbacks observe subsequent replacements.
void AddBionicSymbol(const char* soname, const char* name, void* address);

// Sets up this thread's real ELF TLS once before entering loaded code.
// Also used by native media callbacks, without a Java VM dependency.
void EnterLoadedCode();

// Lets signals other processes send (gesher-base's signal mailbox) reach the
// .so's handlers. `authoritative`: the shim owns every signal, so SIG_DFL
// acts as Linux's default (gesher-exec, whose program has the shim as its
// libc); otherwise only signals the .so set an action for (dalvikvm, where
// the rest are ART's). InstallBionicShim installs it non-authoritative.
void InstallSignalHook(bool authoritative);

// exit()/_exit() on a thread that is a vfork-style child (fork() returned 0
// and exec*() has not run yet; docs/exec.md): fork() returns again in the
// parent, the child reported as exited with `status`. Returns only when the
// calling thread is not such a child.
void ExitIfVforkChild(int status);

}  // namespace gesher::bionic
