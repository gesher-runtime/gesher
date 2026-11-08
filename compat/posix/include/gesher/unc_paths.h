// UNC file paths in a namespace that Java's Unix path normalization preserves.
#pragma once

#include <string>
#include <string_view>

namespace gesher::unc_paths {

enum class Match { kUnrelated, kMapped, kInvalid };

inline bool RootComponent(std::wstring_view component) {
  if (component.empty() || component == L"." || component == L"..") return false;
  for (wchar_t c : component) {
    if (c < 32 || c == 127 || std::wstring_view(L"/\\:*?\"<>|").find(c)
        != std::wstring_view::npos) return false;
  }
  return true;
}

// The server and share form the UNC root. Device prefixes are not UNC roots.
inline bool SharePath(std::wstring_view path) {
  size_t server_end = path.find(L'/');
  if (server_end == std::wstring_view::npos || !RootComponent(path.substr(0, server_end)))
    return false;
  size_t share_end = path.find(L'/', server_end + 1);
  auto share = path.substr(server_end + 1, share_end == std::wstring_view::npos
      ? std::wstring_view::npos : share_end - server_end - 1);
  return RootComponent(share) && path.find(L'\0') == std::wstring_view::npos;
}

inline Match ToWindows(std::wstring_view guest, std::wstring* windows) {
  constexpr std::wstring_view prefix = L"/mnt/unc";
  if (guest.substr(0, prefix.size()) != prefix || (guest.size() > prefix.size()
      && guest[prefix.size()] != L'/' && guest[prefix.size()] != L'\\'
      && guest[prefix.size()] != L':')) return Match::kUnrelated;
  if (guest.size() <= prefix.size() || guest[prefix.size()] != L'/'
      || !SharePath(guest.substr(prefix.size() + 1))) return Match::kInvalid;
  *windows = L"\\\\" + std::wstring(guest.substr(prefix.size() + 1));
  for (wchar_t& c : *windows) if (c == L'/') c = L'\\';
  return Match::kMapped;
}

// Only ordinary and extended UNC file roots are converted. On false, the
// caller retains its existing handling of drives and unsupported namespaces.
inline bool FromWindows(std::wstring_view native, std::wstring* guest) {
  std::wstring path(native);
  for (wchar_t& c : path) if (c == L'\\') c = L'/';
  std::wstring_view body(path);
  if (body.size() >= 8 && body.substr(0, 4) == L"//?/"
      && (body[4] == L'U' || body[4] == L'u')
      && (body[5] == L'N' || body[5] == L'n')
      && (body[6] == L'C' || body[6] == L'c') && body[7] == L'/') {
    body.remove_prefix(8);
  } else if (body.substr(0, 2) == L"//") {
    body.remove_prefix(2);
  } else {
    return false;
  }
  if (!SharePath(body)) return false;
  *guest = L"/mnt/unc/" + std::wstring(body);
  return true;
}

}  // namespace gesher::unc_paths
