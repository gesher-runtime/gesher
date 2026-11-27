// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Actual production identity decoder; no claim of process/pipe authentication.
#include <cstdio>
#include <cstdlib>

#include "../../compat/usb_storage/protected_runtime_wire.h"
namespace wire = gesher::usb_storage::runtime_wire;
void Check(bool ok, const char* name) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", name);
    std::exit(1);
  }
}
std::vector<std::byte> Prefix() {
  std::vector<std::byte> bytes;
  for (const auto& field : {u"fixture.owner", u"fixture.owner",
                            u"C:/sealed/base.apk", u"digest", u"signer"})
    Check(wire::Append(&bytes, field), "encode fixture identity field");
  return bytes;
}
bool Decode(const std::vector<std::byte>& bytes) {
  wire::Reader reader(bytes);
  wire::SpawnIdentity identity;
  return wire::ReadSpawnIdentity(reader, &identity);
}
int main() {
  Check(wire::kVersion == 4, "new owner shape has explicit wire version");
  auto absent = Prefix();
  Check(!Decode(absent), "old five-field spawn cannot omit owner");
  for (const auto owner :
       {u"", u"00000000-0000-0000-0000-000000000000",
        u"11111111-2222-3333-4444-55555555555A",
        u"11111111222233334444555555555555", u"GESHER_INET=1"}) {
    auto bytes = Prefix();
    Check(wire::Append(&bytes, owner), "encode malformed owner");
    Check(!Decode(bytes), "missing malformed or environment-as-owner rejected");
  }
  auto valid = Prefix();
  Check(wire::Append(&valid, u"11111111-2222-3333-4444-555555555555"),
        "encode canonical owner");
  Check(Decode(valid), "canonical installed owner decodes");
  valid.pop_back();
  Check(!Decode(valid), "truncated owner payload fails before admission");
  auto embedded = Prefix();
  std::u16string owner = u"11111111-2222-3333-4444-555555555555";
  owner[3] = 0;
  Check(!wire::Append(&embedded, owner),
        "embedded owner NUL cannot enter wire");
  std::puts(
      "PASS actual production v4 owner decoder: "
      "old/missing/zero/noncanonical/truncated/env-spoof controls and "
      "canonical identity");
  return 0;
}
