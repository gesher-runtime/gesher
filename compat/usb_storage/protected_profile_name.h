// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "protected_runtime_wire.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace gesher::usb_storage {
namespace protected_profile_name_detail {

inline bool Decimal(std::wstring_view text, uint64_t maximum) {
  if (text.empty() || (text.size() > 1 && text.front() == L'0')) return false;
  uint64_t value = 0;
  for (const wchar_t c : text) {
    if (c < L'0' || c > L'9') return false;
    const auto digit = static_cast<uint64_t>(c - L'0');
    if (value > (maximum - digit) / 10) return false;
    value = value * 10 + digit;
  }
  return true;
}

// The caller supplies the canonical TokenUser SID returned by Windows, never
// an Android environment variable or a caller-selected realm/UID. Bounds and
// syntax validation here prevent ambiguous or truncated identity material.
inline bool DesktopSid(std::wstring_view text) {
  if (text.size() > 184 || !text.starts_with(L"S-1-") ||
      text.find(L'\0') != std::wstring_view::npos) return false;
  text.remove_prefix(4);
  size_t end = text.find(L'-');
  if (end == std::wstring_view::npos) return false;
  const auto authority = text.substr(0, end);
  if (authority.starts_with(L"0x")) {
    if (authority.size() != 14 || authority.substr(2).find_first_not_of(
            L"0123456789abcdefABCDEF") != std::wstring_view::npos) return false;
  } else if (!Decimal(authority, 0xffffffffffffULL)) {
    return false;
  }
  text.remove_prefix(end + 1);
  unsigned components = 0;
  for (;;) {
    end = text.find(L'-');
    const auto component = text.substr(0, end);
    if (++components > 15 || !Decimal(component, UINT32_MAX)) return false;
    if (end == std::wstring_view::npos) return true;
    text.remove_prefix(end + 1);
  }
}

inline bool Package(std::wstring_view text) {
  if (text.empty() || text.size() > 255) return false;
  bool first = true;
  for (const wchar_t c : text) {
    if (c == L'.') {
      if (first) return false;
      first = true;
    } else {
      const bool letter = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
      if (!letter && (first || (c != L'_' && (c < L'0' || c > L'9')))) return false;
      first = false;
    }
  }
  return !first;
}

inline bool HashField(BCRYPT_HASH_HANDLE hash, std::wstring_view field) {
  // Stable, explicit framing: a uint32 little-endian count of UTF-16 code
  // units followed by those units in little-endian order, without a NUL.
  // The largest accepted field is the 255-character package name.
  if (field.size() > 255) return false;
  std::array<UCHAR, 4 + 255 * 2> encoded{};
  const auto length = static_cast<uint32_t>(field.size());
  for (unsigned i = 0; i < 4; ++i) encoded[i] = static_cast<UCHAR>(length >> (i * 8));
  for (size_t i = 0; i < field.size(); ++i) {
    const auto code = static_cast<uint16_t>(field[i]);
    encoded[4 + i * 2] = static_cast<UCHAR>(code);
    encoded[5 + i * 2] = static_cast<UCHAR>(code >> 8);
  }
  return BCryptHashData(hash, encoded.data(), static_cast<ULONG>(4 + field.size() * 2), 0) >= 0;
}

}  // namespace protected_profile_name_detail

inline bool ValidInstallationOwner(std::wstring_view owner) {
  return runtime_wire::ValidInstallationOwner(owner);
}

// Applications retain their profile across an update of the same installation,
// never across uninstall/reinstall. The owner is trusted installer metadata;
// neither a launch/session ID nor a recyclable Android UID is an identity.
// Both empty package and signer select the system-server profile; applications
// require a valid package and the verified 64-character lowercase signer hash.
// Success writes 61 ASCII characters; failure clears a non-null result.
inline uint32_t ProtectedProfileNameHash(std::wstring_view desktop_sid, std::wstring_view package,
    std::wstring_view signer, std::wstring_view installation, bool legacy, std::wstring* result) {
  if (!result) return ERROR_INVALID_PARAMETER;
  const auto fail = [result](uint32_t error) { result->clear(); return error; };
  const bool server = package.empty() && signer.empty();
  if (!protected_profile_name_detail::DesktopSid(desktop_sid) ||
      (server && !installation.empty()) ||
      (!server && (!protected_profile_name_detail::Package(package) || signer.size() != 64 ||
                   signer.find_first_not_of(L"0123456789abcdef") != std::wstring_view::npos ||
                   (!legacy && !ValidInstallationOwner(installation))))) {
    return fail(ERROR_INVALID_PARAMETER);
  }
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
    return fail(ERROR_GEN_FAILURE);
  }
  struct AlgorithmOwner {
    BCRYPT_ALG_HANDLE value;
    ~AlgorithmOwner() { BCryptCloseAlgorithmProvider(value, 0); }
  } algorithm_owner{algorithm};
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) return fail(ERROR_GEN_FAILURE);
  struct HashOwner {
    BCRYPT_HASH_HANDLE value;
    ~HashOwner() { BCryptDestroyHash(value); }
  } hash_owner{hash};
  const std::wstring_view domain = server || legacy ? L"Gesher.ProtectedProfile.v1" : L"Gesher.ProtectedProfile.v2";
  for (const auto field : {domain, desktop_sid, package, signer}) {
    if (!protected_profile_name_detail::HashField(hash, field)) return fail(ERROR_GEN_FAILURE);
  }
  if (!server && !legacy && !protected_profile_name_detail::HashField(hash, installation))
    return fail(ERROR_GEN_FAILURE);
  std::array<UCHAR, 32> digest{};
  if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
    return fail(ERROR_GEN_FAILURE);
  }
  try {
    std::wstring name = server ? L"Gesher.S." : L"Gesher.A.";
    name.reserve(61);
    constexpr wchar_t alphabet[] = L"abcdefghijklmnopqrstuvwxyz234567";
    uint32_t pending = 0;
    unsigned bits = 0;
    for (const UCHAR value : digest) {
      pending = (pending << 8) | value;
      bits += 8;
      while (bits >= 5) {
        bits -= 5;
        name.push_back(alphabet[(pending >> bits) & 31]);
      }
    }
    if (bits) name.push_back(alphabet[(pending << (5 - bits)) & 31]);
    *result = std::move(name);
  } catch (const std::bad_alloc&) {
    return fail(ERROR_NOT_ENOUGH_MEMORY);
  }
  return ERROR_SUCCESS;
}

inline uint32_t ProtectedProfileName(std::wstring_view desktop, std::wstring_view package,
    std::wstring_view signer, std::wstring_view installation, std::wstring* result) {
  return ProtectedProfileNameHash(desktop, package, signer, installation, false, result);
}

// The old signature remains valid only for the stable system-server identity.
inline uint32_t ProtectedProfileName(std::wstring_view desktop, std::wstring_view package,
    std::wstring_view signer, std::wstring* result) {
  return ProtectedProfileName(desktop, package, signer, {}, result);
}

// Migration only: identify obsolete ACEs. Never use this identity to launch an app.
inline uint32_t LegacyProtectedApplicationProfileName(std::wstring_view desktop,
    std::wstring_view package, std::wstring_view signer, std::wstring* result) {
  if (package.empty()) {
    if (result) result->clear();
    return ERROR_INVALID_PARAMETER;
  }
  return ProtectedProfileNameHash(desktop, package, signer, {}, true, result);
}

}  // namespace gesher::usb_storage
