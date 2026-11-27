// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>
#include <cstdlib>
#include <string>
#include <string_view>

namespace gesher::shared_storage_policy {

enum class StorageMode {
  kScoped,
  kMediaRo,
  kLegacyRo,
  kLegacyRw,
  kManager,
};

inline bool ParsePolicyContent(std::string_view content, std::string_view expected_pkg,
                        std::string_view expected_uid, StorageMode* out_mode, int* out_media_mask,
                        std::string_view expected_installation = {}) {
  std::string_view version;
  std::string_view pkg;
  std::string_view uid;
  std::string_view mode_str;
  std::string_view installation;
  int media_mask = 0;
  bool has_media_mask = false;

  size_t pos = 0;
  while (pos < content.size()) {
    size_t nl = content.find('\n', pos);
    std::string_view line =
        nl == std::string_view::npos ? content.substr(pos) : content.substr(pos, nl - pos);
    pos = nl == std::string_view::npos ? content.size() : nl + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    std::string_view key = line.substr(0, eq);
    std::string_view val = line.substr(eq + 1);
    if (key == "v") {
      version = val;
    } else if (key == "pkg") {
      pkg = val;
    } else if (key == "uid") {
      uid = val;
    } else if (key == "installation") {
      if (!installation.empty()) return false;
      installation = val;
    } else if (key == "mode") {
      mode_str = val;
    } else if (key == "media_mask") {
      if (has_media_mask) return false;
      has_media_mask = true;
      std::string tmp(val);
      char* end = nullptr;
      long parsed = strtol(tmp.c_str(), &end, 10);
      if (end != tmp.c_str() && *end == '\0' && parsed >= 0 && parsed <= 7) {
        media_mask = static_cast<int>(parsed);
      } else return false;
    }
  }

  if (version != "1" || pkg != expected_pkg || expected_uid.empty() || uid != expected_uid ||
      !has_media_mask || (!expected_installation.empty() && installation != expected_installation))
    return false;

  StorageMode mode;
  if (mode_str == "MANAGER") {
    mode = StorageMode::kManager;
  } else if (mode_str == "LEGACY_RW") {
    mode = StorageMode::kLegacyRw;
  } else if (mode_str == "LEGACY_RO") {
    mode = StorageMode::kLegacyRo;
  } else if (mode_str == "MEDIA_RO") {
    mode = StorageMode::kMediaRo;
  } else if (mode_str == "SCOPED") {
    mode = StorageMode::kScoped;
  } else {
    return false;
  }

  *out_mode = mode;
  *out_media_mask = media_mask;
  return true;
}

inline bool MatchesExtensionList(std::wstring_view ext, const wchar_t* const* list, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const std::wstring_view candidate(list[i]);
    if (CompareStringOrdinal(ext.data(), static_cast<int>(ext.size()), candidate.data(),
                             static_cast<int>(candidate.size()), TRUE) == CSTR_EQUAL) return true;
  }
  return false;
}

inline bool HasMediaExtension(std::wstring_view name, int media_mask) {
  const size_t dot = name.find_last_of(L'.');
  if (dot == std::wstring_view::npos) return false;
  const std::wstring_view ext = name.substr(dot);
  static constexpr const wchar_t* kImages[] = {
      L".jpg", L".jpeg", L".png", L".gif", L".webp", L".bmp", L".heic", L".heif", L".dng", L".avif",
  };
  static constexpr const wchar_t* kVideo[] = {
      L".mp4", L".m4v", L".mkv", L".webm", L".3gp", L".avi", L".mov", L".ts",
  };
  static constexpr const wchar_t* kAudio[] = {
      L".mp3", L".m4a", L".aac", L".ogg", L".oga", L".opus", L".wav", L".flac", L".mid", L".midi",
  };

  if ((media_mask & 1) != 0 && MatchesExtensionList(ext, kImages, sizeof(kImages) / sizeof(kImages[0]))) {
    return true;
  }
  if ((media_mask & 2) != 0 && MatchesExtensionList(ext, kVideo, sizeof(kVideo) / sizeof(kVideo[0]))) {
    return true;
  }
  if ((media_mask & 4) != 0 && MatchesExtensionList(ext, kAudio, sizeof(kAudio) / sizeof(kAudio[0]))) {
    return true;
  }
  return false;
}

}  // namespace gesher::shared_storage_policy
