// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Exercise the production serializer/parser without a transport or authority.
#include "../../compat/usb_storage/protected_runtime_wire.h"

#include <array>
#include <cstdio>

namespace wire = gesher::usb_storage::runtime_wire;
namespace {
int checks = 0, failures = 0;
void Check(bool condition, const char* description) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", description);
  }
}
std::vector<std::byte> Bytes(std::initializer_list<unsigned char> values) {
  std::vector<std::byte> result;
  for (const auto value : values) result.push_back(static_cast<std::byte>(value));
  return result;
}
}  // namespace

int main() {
  std::vector<std::byte> payload;
  Check(wire::Append(&payload, {}), "default empty view serializes without a null source copy");
  Check(payload == Bytes({0, 0, 0, 0}), "empty string has a four-byte zero length");
  Check(wire::Append(&payload, u"A\u05d0\U0001f600"), "non-ASCII UTF-16 field appends after an existing field");
  const auto golden = Bytes({0, 0, 0, 0, 4, 0, 0, 0, 0x41, 0, 0xd0, 5, 0x3d, 0xd8, 0, 0xde});
  Check(payload == golden, "wire layout matches independent little-endian UTF-16 golden bytes");
  wire::Reader reader(golden);
  std::u16string text = u"previous";
  Check(reader.Read(&text) && text.empty() && !reader.empty(), "reader accepts the empty field and preserves its next boundary");
  Check(reader.Read(&text) && text == u"A\u05d0\U0001f600" && reader.empty(), "reader consumes the exact remaining UTF-16 field");
  Check(!reader.Read(&text), "reader refuses a missing field after the final boundary");

  const auto before = payload;
  const std::array<char16_t, 3> embedded{u'a', u'\0', u'b'};
  Check(!wire::Append(&payload, {embedded.data(), embedded.size()}) && payload == before,
      "embedded NUL is rejected without changing the output");
  Check(!wire::Append(&payload, std::u16string(8193, u'x')) && payload == before,
      "oversized field is rejected without changing prior fields");
  payload.clear();
  const std::u16string maximum(8192, u'x');
  Check(wire::Append(&payload, maximum), "maximum field length is accepted");
  wire::Reader maximum_reader(payload);
  Check(maximum_reader.Read(&text) && text == maximum && maximum_reader.empty(), "maximum field round-trips at the exact boundary");
  Check(wire::Append(&payload, maximum) && wire::Append(&payload, maximum) &&
      wire::Append(&payload, std::u16string(8184, u'y')) && payload.size() == wire::kMaximumPayload,
      "multiple fields can fill the exact 65536-byte transport payload");
  const auto full = payload;
  Check(!wire::Append(&payload, {}) && payload == full, "even an empty field cannot exceed the total payload bound");

  // Independent malformed frames: short header, short UTF-16 code unit,
  // oversized count, arithmetic-wrap count and a prohibited embedded NUL.
  for (const auto& frame : {
      Bytes({}), Bytes({1}), Bytes({1, 0}), Bytes({1, 0, 0}),
      Bytes({1, 0, 0, 0}), Bytes({1, 0, 0, 0, 0x41}),
      Bytes({1, 0x20, 0, 0}), Bytes({0xff, 0xff, 0xff, 0xff}),
      Bytes({3, 0, 0, 0, 0x61, 0, 0, 0, 0x62, 0})}) {
    wire::Reader malformed(frame);
    Check(!malformed.Read(&text), "malformed frame is rejected without an out-of-bounds read");
  }
  std::printf("Protected runtime wire: %d checks, %d failures (no authority or hardware execution)\n", checks, failures);
  return failures ? 1 : 0;
}
