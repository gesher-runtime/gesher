/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gesher::windows {

// Read an OLE source's exact file list. The data block is source-provided;
// validate its layout before passing the HDROP handle to Shell32.
inline std::optional<std::vector<std::wstring>> ReadDropPaths(IDataObject* source, size_t maximum = UINT_MAX) {
  FORMATETC request{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (!source || FAILED(source->GetData(&request, &medium))) return std::nullopt;
  std::vector<std::wstring> paths;
  if (medium.tymed == TYMED_HGLOBAL) {
    const size_t bytes = GlobalSize(medium.hGlobal);
    const auto* header = static_cast<const DROPFILES*>(GlobalLock(medium.hGlobal));
    bool valid = false;
    if (header && bytes >= sizeof(DROPFILES) && header->pFiles >= sizeof(DROPFILES) &&
        header->pFiles <= bytes) {
      const size_t units = bytes - header->pFiles;
      const auto* first = reinterpret_cast<const char*>(header) + header->pFiles;
      if (header->fWide && header->pFiles % alignof(wchar_t) == 0) {
        const auto* chars = reinterpret_cast<const wchar_t*>(first);
        for (size_t i = 1; i < units / sizeof(wchar_t); ++i) {
          if (chars[i - 1] != L'\0' || chars[i] != L'\0') continue;
          valid = true;
          break;
        }
      } else if (!header->fWide) {
        for (size_t i = 1; i < units; ++i) {
          if (first[i - 1] != '\0' || first[i] != '\0') continue;
          valid = true;
          break;
        }
      }
    }
    if (header) GlobalUnlock(medium.hGlobal);
    if (valid) {
      const HDROP drop = static_cast<HDROP>(medium.hGlobal);
      const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      if (count > maximum) { ReleaseStgMedium(&medium); return paths; }
      paths.reserve(count);
      for (UINT i = 0; i < count; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        if (length == 0 || length >= 32768) break;
        std::wstring path(length + 1, L'\0');
        if (DragQueryFileW(drop, i, path.data(), length + 1) != length) break;
        path.resize(length);
        paths.push_back(std::move(path));
      }
      if (paths.size() != count) paths.clear();
    }
  }
  ReleaseStgMedium(&medium);
  return paths;
}


inline std::wstring ReadDropText(IDataObject* source, size_t maximum) {
  for (const auto format : {CF_UNICODETEXT, CF_TEXT}) {
    FORMATETC request{static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (source->QueryGetData(&request) != S_OK || FAILED(source->GetData(&request, &medium))) continue;
    std::wstring result;
    if (medium.tymed == TYMED_HGLOBAL) {
      const auto* data = static_cast<const char*>(GlobalLock(medium.hGlobal));
      const auto size = GlobalSize(medium.hGlobal);
      if (data != nullptr) {
        if (format == CF_UNICODETEXT) {
          auto first = reinterpret_cast<const wchar_t*>(data);
          auto count = std::min(size / sizeof(wchar_t), maximum + 1);
          const auto* end = std::find(first, first + count, L'\0');
          if (end != first + count) result.assign(first, end);
        } else {
          const auto count = std::min(size, maximum + 1);
          const auto* end = std::find(data, data + count, '\0');
          if (end != data + count && end != data) {
            const int length = static_cast<int>(end - data);
            const auto wide = MultiByteToWideChar(CP_ACP, 0, data, length, nullptr, 0);
            if (wide > 0 && static_cast<size_t>(wide) <= maximum) {
              result.resize(wide);
              if (!MultiByteToWideChar(CP_ACP, 0, data, length, result.data(), wide)) result.clear();
            }
          }
        }
        GlobalUnlock(medium.hGlobal);
      }
    }
    ReleaseStgMedium(&medium);
    if (!result.empty()) return result;
  }
  return {};
}

}  // namespace gesher::windows
