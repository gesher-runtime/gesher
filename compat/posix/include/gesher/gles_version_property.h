// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// ro.opengles.version from a build.prop: kept only as a version Gesher serves,
// 0x30000 or 0x20000 (compat/bionic/egl_gles_version.h; docs/system-features.md).
// Another value would be declared while EGL gives something else, so it is left
// unset and the system server's probe declares the version. One rule for every
// build.prop loader: the property area's initial values
// (compat/posix/src/system_properties.cc), dalvikvm's LoadBuildProp
// (compat/art/jni_invocation_static.cc) and BuildProps.java (the same forms,
// written out there).
//
// Accepted forms: decimal digits ("196608"), or 0x/0X and hexadecimal digits
// ("0x30000"); no sign, spaces, other prefixes or suffixes.
#pragma once

#include <string_view>

namespace gesher::properties {

inline constexpr std::string_view kGlEsVersionProperty = "ro.opengles.version";

// `text` as an OpenGL ES version number in an accepted form; false otherwise.
inline bool ParseGlEsVersion(std::string_view text, long long* value) {
  int base = 10;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
    text.remove_prefix(2);
  }
  if (text.empty() || text.size() > 10) return false;
  long long v = 0;
  for (char c : text) {
    int digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return false;
    v = v * base + digit;
  }
  *value = v;
  return true;
}

// Whether a build.prop line `key`=`value` may be loaded: anything but
// ro.opengles.version, and that only as a served version.
inline bool ServedBuildProperty(std::string_view key, std::string_view value) {
  if (key != kGlEsVersionProperty) return true;
  long long v = 0;
  return ParseGlEsVersion(value, &v) && (v == 0x30000 || v == 0x20000);
}

}  // namespace gesher::properties
