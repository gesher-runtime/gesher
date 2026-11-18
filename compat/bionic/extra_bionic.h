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
void RegisterExtra(SymbolTable& table);     // extra_bionic.cc: libc
void RegisterExtraLibm(SymbolTable& table); // extra_bionic.cc: libm
void RegisterExtraLibdl(SymbolTable& table); // extra_bionic.cc: libdl
void RegisterExtraLibdlAndroid(SymbolTable& table); // private libdl_android
void RegisterExtraSys(SymbolTable& table);  // extra_sys_bionic.cc
void RegisterZlib(SymbolTable& table);      // zlib_bionic.cc

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
