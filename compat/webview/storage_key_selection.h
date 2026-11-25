// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Read the browser's observed serialized keys without reconstructing a key or PSL.
#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gesher::webview::storage_key_selection {

enum class Kind { kFirstParty, kExplicitSite, kImplicitSite, kOpaqueSite };

struct Key {
  std::wstring serialized;
  std::wstring origin;
  std::wstring site;
  Kind kind = Kind::kFirstParty;
};

inline bool NoncePart(std::wstring_view value, uint64_t* result) {
  if (value.empty() || value.size() > 20 || (value.size() > 1 && value.front() == L'0'))
    return false;
  uint64_t number = 0;
  for (wchar_t c : value) {
    if (c < L'0' || c > L'9' ||
        number > (std::numeric_limits<uint64_t>::max() - uint64_t(c - L'0')) / 10)
      return false;
    number = number * 10 + uint64_t(c - L'0');
  }
  *result = number;
  return true;
}

// These are the serialized forms emitted by Chromium StorageKey::Serialize.
// The inventory is browser-owned. Reject an unfamiliar form before deleting
// anything; do not reinterpret delimiters or silently omit an unknown key.
inline bool Parse(const std::wstring& value, Key* key) {
  if (value.empty() || value.size() > 4096) return false;
  for (wchar_t c : value) if (c <= 0x20 || c >= 0x7f) return false;
  const size_t caret = value.find(L'^');
  const std::wstring origin = value.substr(0, caret);
  if (origin.empty() || origin.back() != L'/' || origin.find(L"://") == std::wstring::npos)
    return false;
  Key parsed{value, origin, {}, Kind::kFirstParty};
  if (caret == std::wstring::npos) {
    *key = std::move(parsed);
    return true;
  }
  const std::wstring_view suffix(value.data() + caret, value.size() - caret);
  if (suffix.starts_with(L"^0")) {
    const auto site = suffix.substr(2);
    if (site.empty() || site.find(L'^') != std::wstring_view::npos ||
        site.find(L"://") == std::wstring_view::npos || site.back() == L'/') return false;
    parsed.kind = Kind::kExplicitSite;
    parsed.site = site;
  } else if (suffix == L"^31") {
    parsed.kind = Kind::kImplicitSite;
  } else if (suffix.starts_with(L"^1") || suffix.starts_with(L"^4")) {
    const bool opaque = suffix[1] == L'4';
    const std::wstring_view second = opaque ? L"^5" : L"^2";
    const size_t middle = suffix.find(second, 2);
    const size_t last = opaque ? suffix.find(L"^6", middle == std::wstring_view::npos ? 2 : middle + 2)
                               : suffix.size();
    uint64_t high = 0, low = 0;
    if (middle == std::wstring_view::npos || last == std::wstring_view::npos ||
        !NoncePart(suffix.substr(2, middle - 2), &high) ||
        !NoncePart(suffix.substr(middle + 2, last - middle - 2), &low) ||
        (high == 0 && low == 0)) return false;
    // An opaque top-level site may have no precursor tuple at all (^6 ends
    // the key). A precursor, when present, still does not make that site equal
    // to the nonopaque origin used for trusted deletion.
    if (opaque && suffix.find(L'^', last + 2) != std::wstring_view::npos)
      return false;
    parsed.kind = opaque ? Kind::kOpaqueSite : Kind::kImplicitSite;
  } else {
    return false;
  }
  *key = std::move(parsed);
  return true;
}

inline std::wstring_view Scheme(const std::wstring& origin) {
  const size_t colon = origin.find(L':');
  return colon == std::wstring::npos ? std::wstring_view() : std::wstring_view(origin.data(), colon);
}

// Only an implicit-site key with the requested scheme can match. The runtime
// supplies all entries in `sites`; this function never implements a suffix rule.
inline bool Select(const std::vector<Key>& observed, const std::wstring& origin,
                   const std::map<std::wstring, std::wstring>& sites,
                   std::set<std::wstring>* selected) {
  auto requested = sites.find(origin);
  if (requested == sites.end() || requested->second.empty()) return false;
  std::set<std::wstring> result;
  for (const Key& key : observed) {
    bool match = false;
    switch (key.kind) {
      case Kind::kFirstParty:
        match = key.origin == origin;
        break;
      case Kind::kExplicitSite:
        match = key.site == requested->second;
        break;
      case Kind::kImplicitSite:
        if (Scheme(key.origin) == Scheme(origin)) {
          auto found = sites.find(key.origin);
          if (found == sites.end() || found->second.empty()) return false;
          match = found->second == requested->second;
        }
        break;
      case Kind::kOpaqueSite:
        break;
    }
    if (match) result.insert(key.serialized);
  }
  *selected = std::move(result);
  return true;
}

}  // namespace gesher::webview::storage_key_selection
