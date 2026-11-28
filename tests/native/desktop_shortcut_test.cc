// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
#include <windows.h>
#include <cstdio>
#include "../../compat/shell/shell_win32.h"

namespace shell = gesher::shell;

int main() {
  wchar_t parent[MAX_PATH], directory[MAX_PATH], target[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, parent) ||
      !GetTempFileNameW(parent, L"gsh", 0, directory) ||
      !DeleteFileW(directory) || !CreateDirectoryW(directory, nullptr) ||
      !GetModuleFileNameW(nullptr, target, MAX_PATH)) return 2;
  const auto utf16 = [](const wchar_t* text) {
    return std::u16string(text, text + wcslen(text));
  };
  const std::wstring path = std::wstring(directory) + L"\\example.lnk";
  const std::wstring copy = std::wstring(directory) + L"\\copy.lnk";
  const std::u16string link = utf16(path.c_str()), executable = utf16(target);
  const auto create = [&](const char16_t* arguments) {
    return shell::CreateDesktopShortcut(link, executable, arguments, u"", u"", u"Test", u"Gesher.Shortcut.Test");
  };
  const auto receipt = create(u"original");
  const bool created = receipt.size() == 112 && shell::CheckDesktopShortcut(link, receipt, false);
  const bool refused_replace = create(u"different").empty() &&
      shell::CheckDesktopShortcut(link, receipt, false);
  const bool copied = CopyFileW(path.c_str(), copy.c_str(), TRUE) != 0;
  const bool removed = shell::CheckDesktopShortcut(link, receipt, true) && !shell::Exists(link);
  const bool preserved_replacement = copied && MoveFileW(copy.c_str(), path.c_str()) &&
      !shell::CheckDesktopShortcut(link, receipt, true) && shell::Exists(link);
  DeleteFileW(copy.c_str());
  DeleteFileW(path.c_str());
  const bool cleaned = RemoveDirectoryW(directory) != 0;
  std::printf("desktop_shortcut_test: created=%d no_replace=%d removed=%d preserved_replacement=%d cleaned=%d\n",
              created, refused_replace, removed, preserved_replacement, cleaned);
  return !(created && refused_replace && removed && preserved_replacement && cleaned);
}
