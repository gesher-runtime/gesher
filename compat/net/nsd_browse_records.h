// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string_view>

namespace gesher::nsd {
inline bool same_dns_name(std::wstring_view left, std::wstring_view right) {
  if (!left.empty() && left.back() == L'.') left.remove_suffix(1);
  if (!right.empty() && right.back() == L'.') right.remove_suffix(1);
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    wchar_t x = left[i], y = right[i];
    if (x >= L'A' && x <= L'Z') x += L'a' - L'A';
    if (y >= L'A' && y <= L'Z') y += L'a' - L'A';
    if (x != y) return false;
  }
  return true;
}

enum class PtrObservation { Ignore, Membership, Goodbye };

// A companion base query may receive additional subtype records, but a positive
// base PTR never establishes membership of the requested subtype. For subtype
// browsing the companion owns base withdrawal; an ordinary browse owns its own
// base withdrawal. Subtype-only withdrawals are not whole-service goodbyes.
inline PtrObservation classify_ptr(std::wstring_view owner,
                                  std::wstring_view membership_query,
                                  std::wstring_view base_query, bool withdrawn,
                                  bool companion) {
  if (withdrawn && same_dns_name(owner, base_query)) {
    return companion || same_dns_name(membership_query, base_query)
        ? PtrObservation::Goodbye : PtrObservation::Ignore;
  }
  if (same_dns_name(owner, membership_query)) return PtrObservation::Membership;
  return PtrObservation::Ignore;
}
}  // namespace gesher::nsd
