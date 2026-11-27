// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// The file `gesher-app apk-info <apk> <out>` writes (docs/installer.md), as
// gesher-launch's install confirmation reads it: UTF-8, one key=value per line.
// Pure parsing (no file or process access), shared with
// tests/native/apk_info_file_test.cc.
#pragma once

#include <windows.h>

#include <string>

struct GesherApkInfo {
  std::wstring package;
  long long version_code = 0;
  std::wstring inputs_sha256;
  std::wstring version_name;
  std::wstring label;  // never empty after a successful parse (the package name)
  std::wstring icon;   // a Windows path, or empty
  bool installed = false;               // installed_version_code was written
  bool installed_version_valid = false;  // ... and is a number
  long long installed_version_code = 0;
  std::wstring installed_version_name;
  bool desktop_shortcut = false;  // the package's recorded desktop shortcut exists
};

// A decimal long (Java's range), nothing else: no sign but '-', no spaces.
static inline bool GesherParseLong(const std::wstring& text, long long* out) {
  size_t i = 0;
  bool negative = false;
  if (i < text.size() && text[i] == L'-') {
    negative = true;
    ++i;
  }
  if (i == text.size() || text.size() - i > 19) return false;
  unsigned long long value = 0;
  for (; i < text.size(); ++i) {
    if (text[i] < L'0' || text[i] > L'9') return false;
    value = value * 10 + static_cast<unsigned long long>(text[i] - L'0');
  }
  const unsigned long long limit = negative ? 9223372036854775808ULL : 9223372036854775807ULL;
  if (value > limit) return false;
  *out = negative ? (value == limit ? static_cast<long long>(-9223372036854775807LL - 1)
                                    : -static_cast<long long>(value))
                  : static_cast<long long>(value);
  return true;
}

// An Android package name as gesher-launch passes it on a command line
// (`launch <package>`): dot-separated segments of letters, digits and '_'.
static inline bool GesherValidPackage(const std::wstring& name) {
  if (name.empty() || name.size() > 255 || name.front() == L'.' || name.back() == L'.') {
    return false;
  }
  wchar_t previous = 0;
  for (wchar_t c : name) {
    const bool word = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                      (c >= L'0' && c <= L'9') || c == L'_';
    if (!word && c != L'.') return false;
    if (c == L'.' && previous == L'.') return false;
    previous = c;
  }
  return true;
}

// Text for a dialog: control characters (a label's tabs, stray CR/LF) become
// spaces, surrounding spaces go.
static inline std::wstring GesherDisplayText(const std::wstring& text) {
  std::wstring out;
  for (wchar_t c : text) out.push_back(c < 0x20 || c == 0x7f ? L' ' : c);
  const size_t first = out.find_first_not_of(L' ');
  if (first == std::wstring::npos) return std::wstring();
  return out.substr(first, out.find_last_not_of(L' ') - first + 1);
}

static inline std::wstring GesherApkInfoWiden(const std::string& utf8) {
  if (utf8.empty() || utf8.size() > 0x7fffffff) return std::wstring();
  const int length = static_cast<int>(utf8.size());
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, &out[0], n);
  return out;
}

// Reads the file's text. Lines end with LF or CRLF; a UTF-8 BOM, blank lines,
// '#' comments and unknown keys are ignored; a key's last line wins. Returns
// false (and leaves *info unspecified) without a valid package and version_code.
static inline bool GesherParseApkInfo(const std::string& bytes, GesherApkInfo* info) {
  *info = GesherApkInfo();
  size_t p = bytes.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
  bool have_code = false;
  while (p < bytes.size()) {
    size_t end = bytes.find('\n', p);
    if (end == std::string::npos) end = bytes.size();
    std::string line = bytes.substr(p, end - p);
    p = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t equals = line.find('=');
    if (equals == std::string::npos || equals == 0) continue;
    const std::string key = line.substr(0, equals);
    const std::wstring value = GesherApkInfoWiden(line.substr(equals + 1));
    if (key == "package") {
      info->package = value;
    } else if (key == "version_code") {
      have_code = GesherParseLong(value, &info->version_code);
    } else if (key == "inputs_sha256") {
      if (value.size() != 64) return false;
      for (wchar_t c : value) {
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
      }
      info->inputs_sha256 = value;
    } else if (key == "version_name") {
      info->version_name = GesherDisplayText(value);
    } else if (key == "label") {
      info->label = GesherDisplayText(value);
    } else if (key == "icon") {
      info->icon = value;
    } else if (key == "installed_version_code") {
      info->installed = true;
      info->installed_version_valid = GesherParseLong(value, &info->installed_version_code);
    } else if (key == "installed_version_name") {
      info->installed_version_name = GesherDisplayText(value);
    } else if (key == "desktop_shortcut") {
      info->desktop_shortcut = value == L"1";
    }
  }
  if (!have_code || !GesherValidPackage(info->package)) return false;
  if (info->label.empty()) info->label = info->package;
  return true;
}

// The same versionCode is installed: opening the file opens the app.
static inline bool GesherApkInfoSameVersion(const GesherApkInfo& info) {
  return info.installed && info.installed_version_valid &&
         info.installed_version_code == info.version_code;
}
