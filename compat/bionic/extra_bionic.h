// gesher bionic shim: what the native2 additions (extra_bionic.cc,
// extra_sys_bionic.cc, zlib_bionic.cc) share with the rest of the shim
// (docs/native-libs.md, ADR 0057).
#pragma once

#include "shim_internal.h"

#include <string>
#include <vector>

namespace gesher::bionic {

// Registered before every other part of the shim (shim.cc's Table()), so a
// name the core files define keeps their definition: these fill gaps only.
void RegisterExtra(SymbolTable& table);     // extra_bionic.cc
void RegisterExtraSys(SymbolTable& table);  // extra_sys_bionic.cc
void RegisterZlib(SymbolTable& table);      // zlib_bionic.cc

// --- dlopen of the platform's libraries ---------------------------------------
// The NDK libraries the shim implements symbol by symbol (libc.so, libEGL.so,
// libz.so, ...) have no file: dlopen() of one of them by name or by its
// /system/lib64 path gives a handle of its own, valid for dlsym() and dlclose().
// nullptr if `path` names no such library, or the shim lacks it in this
// process (libEGL.so outside dalvikvm, libvulkan.so).
void* PlatformLibraryHandle(const char* path);
// Is `handle` one PlatformLibraryHandle returned?
bool IsPlatformLibraryHandle(const void* handle);
// The soname of such a handle ("libEGL.so").
const char* PlatformLibraryName(const void* handle);

// Directories app libraries were loaded from (the APK's extracted lib/<abi>,
// as the JNI bridge and dlopen() with a path saw them) and $LD_LIBRARY_PATH:
// where dlopen() of a bare name not loaded yet looks, as the app's linker
// namespace's search path does on Android. Most recent first.
void NoteLibraryDirectory(const std::string& dir);
std::vector<std::string> LibraryDirectories();

// --- system properties --------------------------------------------------------
// Where __system_property_* read and write. dalvikvm's runtime has the
// properties gesher-app loads from build.prop (android::base); libandroid.cc
// installs those accessors. Without them (elf_load_test, gesher-exec) the
// shim reads $ANDROID_ROOT/build.prop itself.
using PropertyGetter = bool (*)(const char* name, std::string* value);
using PropertySetter = bool (*)(const char* name, const char* value);
void SetPropertyAccessors(PropertyGetter get, PropertySetter set);
// __system_property_get's work: the value (at most 91 bytes + NUL) and its length.
int SystemPropertyGetValue(const char* name, char* value);

}  // namespace gesher::bionic
