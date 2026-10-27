// gesher: the bionic shim, the libc/libm/libdl/liblog/pthread symbols an
// Android x86-64 .so imports, as System V entry points over gesher-base and
// the CRT (compat/bionic). See docs/native-libs.md.
#pragma once

namespace gesher::bionic {

// Registers the shim with the ELF loader (gesher/elf_loader.h) as the
// resolver for symbols no loaded ELF object defines, and as the reporter for
// calls to imports nothing defines. Call once before loading a .so.
void InstallBionicShim();

// The shim's System V entry point (or data object) for `name`, or nullptr.
void* LookupBionicSymbol(const char* name);

// Adds (or replaces) a symbol the shim provides, e.g. JNI_GetCreatedJavaVMs
// from the JNI bridge (compat/bionic/jni_bridge.cc). Call before the .so that
// imports it is loaded.
void AddBionicSymbol(const char* name, void* address);

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
