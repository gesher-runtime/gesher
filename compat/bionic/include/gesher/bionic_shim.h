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

}  // namespace gesher::bionic
