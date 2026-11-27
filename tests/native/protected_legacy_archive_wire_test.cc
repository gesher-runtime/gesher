// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Pure production request syntax/root derivation, not peer authentication.
#include "../../compat/usb_storage/protected_legacy_archive.h"
#include <cstdio>
#include <cstdlib>
using namespace gesher::usb_storage;
int main() {
  const std::u16string root = u"C:\\sealed\\data", archive = root + u"\\system\\legacy-key-archive";
  const std::u16string binding(64, u'a');
  size_t checks = 0;
  auto check = [&](bool value) { ++checks; if (!value) std::abort(); };
  auto encode = [&](const std::u16string& data, const std::u16string& path, const std::u16string& id) {
    std::vector<std::byte> payload;
    check(runtime_wire::Append(&payload, data)); check(runtime_wire::Append(&payload, path));
    check(runtime_wire::Append(&payload, id)); return payload;
  };
  auto resolve = [&](const std::vector<std::byte>& payload) {
    std::u16string data, output_archive, id;
    return ResolveLegacyArchivePaths(std::wstring(root.begin(), root.end()), payload,
        &data, &output_archive, &id);
  };
  for (const auto& suffix : {u"\\data\\org.example", u"\\user_de\\0\\org.example"}) {
    auto payload = encode(root + suffix, archive, binding);
    std::u16string data, output_archive, id;
    check(ResolveLegacyArchivePaths(std::wstring(root.begin(), root.end()), payload, &data, &output_archive, &id));
    check(data == root + suffix && output_archive == archive && id == binding);
    for (size_t n = 0; n < payload.size(); ++n)
      check(!resolve(std::vector<std::byte>(payload.begin(), payload.begin() + n)));
    payload.push_back(std::byte{}); check(!resolve(payload));
  }
  for (const auto& data : {u"C:\\sealed\\data\\data\\..", u"C:\\sealed\\data\\data\\.bad",
      u"C:\\sealed\\data\\data\\bad.", u"C:\\sealed\\data\\data\\a..b",
      u"C:\\sealed\\data\\data\\app\\other", u"C:\\sealed\\data\\data\\app:stream",
      u"C:\\sealed\\data\\data\\app/other", u"C:\\sealed\\data\\user_de\\1\\app",
      u"C:\\other\\data\\app", u"C:\\sealed\\data\\system\\app", u"C:\\sealed\\data\\data\\"})
    check(!resolve(encode(data, archive, binding)));
  check(!resolve(encode(root + u"\\data\\app", archive + u"\\..", binding)));
  check(!resolve(encode(root + u"\\data\\app", root + u"\\system\\other", binding)));
  check(!resolve(encode(root + u"\\data\\app", archive, std::u16string(63, u'a'))));
  check(!resolve(encode(root + u"\\data\\app", archive, std::u16string(64, u'G'))));
  check(!resolve(encode(root + u"\\data\\" + std::u16string(256, u'a'), archive, binding)));
  std::printf("PROTECTED_LEGACY_ARCHIVE_WIRE_PASS checks=%zu authentication=not-tested\n", checks);
}
