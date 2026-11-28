// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Real Windows BCrypt checks. Does not create a profile, token or ACL and does
// not claim to exercise admission, installation or physical hardware.
#include "../../compat/usb_storage/protected_profile_name.h"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace {
int checks = 0;
int failures = 0;
void Check(bool condition, const char* description) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", description);
  }
}

std::string FramedDigest(std::wstring_view first, std::wstring_view second) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
  std::array<UCHAR, 32> digest{};
  if (ok) ok = gesher::usb_storage::protected_profile_name_detail::HashField(hash, first) &&
               gesher::usb_storage::protected_profile_name_detail::HashField(hash, second) &&
               BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (!ok) return {};
  std::string text;
  constexpr char hex[] = "0123456789abcdef";
  for (const UCHAR value : digest) {
    text.push_back(hex[value >> 4]);
    text.push_back(hex[value & 15]);
  }
  return text;
}

}  // namespace

int main() {
  using gesher::usb_storage::ProtectedProfileName;
  constexpr std::wstring_view sid = L"S-1-5-21-111-222-333-1001";
  constexpr std::wstring_view package = L"com.cardwake.otgroot";
  constexpr std::wstring_view signer = L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  constexpr std::wstring_view installation = L"11111111-2222-3333-4444-555555555555";
  // Independent reference generated with Python's hashlib and base64:
  // application fields = ('Gesher.ProtectedProfile.v2', sid, package, signer, installation)
  // data = b''.join(struct.pack('<I',len(s.encode('utf-16le'))//2)
  //                 + s.encode('utf-16le') for s in fields)
  // base64.b32encode(hashlib.sha256(data).digest()).rstrip(b'=').lower()
  constexpr std::wstring_view expected_server =
      L"Gesher.S.l7cazcavodqulw2n3k4dx456ycftgxom5ab4fjmjcwqzenthl6kq";
  constexpr std::wstring_view expected_app =
      L"Gesher.A.hymmqhami2rdpbwzazwnieek55lfskq267eeunausppuxc3pilga";
  std::wstring server, app;
  Check(ProtectedProfileName(sid, {}, {}, &server) == ERROR_SUCCESS && server == expected_server,
        "server matches independent framed UTF-16 SHA256/base32 reference");
  Check(ProtectedProfileName(sid, package, signer, installation, &app) == ERROR_SUCCESS && app == expected_app,
        "application matches independent framed UTF-16 SHA256/base32 reference");
  Check(app.size() == 61 && server.size() == 61 && app != server &&
            app.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-") == std::wstring::npos,
        "full 256-bit identity and role fit permitted profile-name characters and length");
  bool stable = true;
  for (unsigned i = 0; i < 128; ++i) {
    std::wstring repeated;
    stable = stable && ProtectedProfileName(sid, package, signer, installation, &repeated) == ERROR_SUCCESS && repeated == app;
  }
  Check(stable, "same desktop package and verified signer reuse the same identity across calls");
  std::wstring changed;
  Check(ProtectedProfileName(L"S-1-5-21-111-222-333-1002", package, signer, installation, &changed) == ERROR_SUCCESS && changed != app,
        "different desktop accounts cannot share an application profile");
  Check(ProtectedProfileName(sid, L"com.example.other", signer, installation, &changed) == ERROR_SUCCESS && changed != app,
        "different packages cannot share an application profile");
  std::wstring other_signer(signer);
  other_signer[0] = L'f';
  Check(ProtectedProfileName(sid, package, other_signer, installation, &changed) == ERROR_SUCCESS && changed != app,
        "different verified signers cannot inherit the old application's profile");
  Check(ProtectedProfileName(L"S-1-5-21-111-222-333-1002", {}, {}, &changed) == ERROR_SUCCESS && changed != server,
        "server profile is also isolated by desktop account");

  const auto invalid = [&](std::wstring_view desktop, std::wstring_view name,
                           std::wstring_view certificate, const char* description) {
    std::wstring output = L"stale-profile";
    Check(ProtectedProfileName(desktop, name, certificate, installation, &output) == ERROR_INVALID_PARAMETER && output.empty(),
          description);
  };
  invalid({}, package, signer, "empty desktop identity is rejected");
  invalid(L"S-1-5-21-111-222-333-4294967296", package, signer, "overflowing SID component is rejected");
  invalid(L"S-1-5-21-111-222-333-01001", package, signer, "noncanonical SID component is rejected");
  invalid(std::wstring(185, L'1'), package, signer, "oversized desktop identity is rejected");
  std::wstring embedded_sid(sid);
  embedded_sid.push_back(L'\0'); embedded_sid += L"suffix";
  invalid(embedded_sid, package, signer, "embedded SID NUL cannot truncate identity");
  invalid(sid, {}, signer, "empty package with signer is not a server identity");
  invalid(sid, package, {}, "application requires a verified signer");
  invalid(sid, L"com..example", signer, "empty package component is rejected");
  invalid(sid, L"1com.example", signer, "package component must start with a letter");
  invalid(sid, L"com.example/other", signer, "package path punctuation is rejected");
  invalid(sid, std::wstring(256, L'a'), signer, "oversized package is rejected");
  std::wstring embedded_package(package);
  embedded_package.push_back(L'\0'); embedded_package += L"suffix";
  invalid(sid, embedded_package, signer, "embedded package NUL cannot truncate identity");
  invalid(sid, package, signer.substr(1), "short signer digest is rejected");
  other_signer.assign(signer); other_signer[0] = L'A';
  invalid(sid, package, other_signer, "signer must be canonical lowercase hex");
  other_signer.assign(signer); other_signer[1] = L'\0';
  invalid(sid, package, other_signer, "embedded signer NUL is rejected");
  Check(ProtectedProfileName(sid, package, signer, installation, nullptr) == ERROR_INVALID_PARAMETER,
        "null output is rejected");

  Check(ProtectedProfileName(sid, package, signer, L"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", &changed) == ERROR_SUCCESS && changed != app,
        "fresh installation owner cannot inherit the prior profile");
  for (const std::wstring_view owner : {std::wstring_view{}, std::wstring_view(L"00000000-0000-0000-0000-000000000000"),
       std::wstring_view(L"11111111-2222-3333-4444-55555555555A"), std::wstring_view(L"11111111222233334444555555555555")}) {
    changed = L"stale";
    Check(ProtectedProfileName(sid, package, signer, owner, &changed) == ERROR_INVALID_PARAMETER && changed.empty(),
        "missing, zero or noncanonical installation owner fails closed");
  }
  Check(ProtectedProfileName(sid, package, signer, &changed) == ERROR_INVALID_PARAMETER && changed.empty(),
        "old application signature cannot silently reuse legacy identity");

  // Independent hashlib vectors for two field sequences with identical raw
  // concatenation. Length framing must prevent a boundary from being moved.
  Check(FramedDigest(L"ab", L"c") == "f1eabf021bf4a371a25e17515e29d00915698a38bcec197f6b767b349d19817b",
        "first ambiguous-concatenation vector retains its field boundary");
  Check(FramedDigest(L"a", L"bc") == "602413528388a0c46e6494ea36a3c4e8df4d7bebac70fedc3f325987f890be45",
        "second ambiguous-concatenation vector retains its field boundary");
  std::printf("Windows protected profile identity: %d checks, %d failures (no profile or ACL creation)\n", checks, failures);
  return failures ? 1 : 0;
}
