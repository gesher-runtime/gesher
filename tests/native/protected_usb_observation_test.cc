// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Isolated metadata/codec fixture. The real query and codec are linked below;
// only the OS-facing inventory functions are replaced. No device is opened.
#include "../../compat/usb_storage/protected_usb_observation.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace fixture {
bool available = true, inventory_failed = false, inspect_failed = false, replace_during_inspect = false;
int64_t epoch = 7;
unsigned version_calls = 0, inspect_calls = 0;
std::vector<std::wstring> paths{L"\\\\?\\usb#test-reader"};
gesher::usb::StorageObservation observation{
    {18, 1, 0, 2, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0, 1, 1, 2, 3, 1,
     9, 2, 18, 0, 1, 1, 0, 0x80, 50,
     9, 4, 0, 0, 0, 8, 6, 0x50, 0},
    {{uint8_t{1}, L"Reader"}, {uint8_t{2}, L"Card"}, {uint8_t{3}, L"serial"}}};
unsigned checks = 0;
void Check(bool value, const char* expression, int line) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "protected USB observation fixture: line %d: %s\n", line, expression);
    std::exit(1);
  }
}
}  // namespace fixture

namespace gesher::usb {
bool AttachmentMonitoringAvailable() { return fixture::available; }
int64_t AttachmentVersion(const wchar_t*) { ++fixture::version_calls; return fixture::epoch; }
std::optional<std::vector<std::wstring>> StorageInterfacePaths() {
  return fixture::inventory_failed ? std::nullopt : std::optional(fixture::paths);
}
std::optional<StorageObservation> ObserveStorage(const std::wstring&) {
  ++fixture::inspect_calls;
  if (fixture::replace_during_inspect) ++fixture::epoch;
  return fixture::inspect_failed ? std::nullopt : std::optional(fixture::observation);
}
}  // namespace gesher::usb

#define CHECK(expression) fixture::Check((expression), #expression, __LINE__)
using namespace gesher::usb_storage;

void Put(std::vector<std::byte>* bytes, size_t at, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) (*bytes)[at + i] = std::byte(value >> (i * 8));
}

void Malformed(UsbObservationKind kind, const std::vector<std::byte>& original) {
  UsbObservationData output;
  output.version = 41;
  output.paths = {L"untouched"};
  for (size_t end = 0; end < original.size(); ++end) {
    CHECK(!DecodeUsbObservation(kind, std::span(original).first(end), &output));
    CHECK(output.version == 41 && output.paths == std::vector<std::wstring>{L"untouched"});
  }
  auto extra = original;
  extra.push_back(std::byte{0});
  CHECK(!DecodeUsbObservation(kind, extra, &output));
  Put(&extra, 12, static_cast<uint32_t>(extra.size() - 20));
  CHECK(!DecodeUsbObservation(kind, extra, &output));
  for (size_t offset : {size_t{0}, size_t{4}, size_t{8}, size_t{12}, size_t{16}}) {
    auto corrupted = original;
    Put(&corrupted, offset, UINT32_MAX);
    CHECK(!DecodeUsbObservation(kind, corrupted, &output));
  }
  CHECK(!DecodeUsbObservation(static_cast<UsbObservationKind>(99), original, &output));
  CHECK(!DecodeUsbObservation(kind, original, nullptr));
}

void Codec() {
  UsbObservationData value, output;
  std::vector<std::byte> bytes;
  CHECK(EncodeUsbObservation(UsbObservationKind::Paths, value, &bytes) == ERROR_SUCCESS);
  CHECK(!bytes.empty() && DecodeUsbObservation(UsbObservationKind::Paths, bytes, &output) && output.paths.empty());
  value.paths = fixture::paths;
  CHECK(EncodeUsbObservation(UsbObservationKind::Paths, value, &bytes) == ERROR_SUCCESS);
  CHECK(DecodeUsbObservation(UsbObservationKind::Paths, bytes, &output) && output.paths == value.paths);
  Malformed(UsbObservationKind::Paths, bytes);
  auto malformed = bytes;
  Put(&malformed, 20, 129);
  CHECK(!DecodeUsbObservation(UsbObservationKind::Paths, malformed, &output));
  Put(&malformed, 20, 1);
  Put(&malformed, 24, UINT32_MAX);
  CHECK(!DecodeUsbObservation(UsbObservationKind::Paths, malformed, &output));
  value.paths.push_back(L"\\\\?\\USB#TEST-READER");
  CHECK(EncodeUsbObservation(UsbObservationKind::Paths, value, &bytes) == ERROR_INVALID_DATA && bytes.empty());
  value.paths = {std::wstring(L"reader") + std::wstring(1, L'\0')};
  CHECK(EncodeUsbObservation(UsbObservationKind::Paths, value, &bytes) == ERROR_INVALID_DATA && bytes.empty());
  value.paths.assign(129, L"reader");
  CHECK(EncodeUsbObservation(UsbObservationKind::Paths, value, &bytes) == ERROR_BUFFER_OVERFLOW && bytes.empty());
  value.paths.clear();
  for (unsigned i = 0; i < 128; ++i) value.paths.push_back(std::wstring(32760, L'a') + std::to_wstring(i));
  CHECK(EncodeUsbObservation(UsbObservationKind::Paths, value, &bytes) == ERROR_BUFFER_OVERFLOW && bytes.empty());

  value = {};
  value.storage = fixture::observation;
  CHECK(EncodeUsbObservation(UsbObservationKind::Inspect, value, &bytes) == ERROR_SUCCESS);
  CHECK(DecodeUsbObservation(UsbObservationKind::Inspect, bytes, &output) &&
        output.storage.descriptors == value.storage.descriptors && output.storage.strings == value.storage.strings);
  Malformed(UsbObservationKind::Inspect, bytes);
  malformed = bytes;
  Put(&malformed, 20, UINT32_MAX);
  CHECK(!DecodeUsbObservation(UsbObservationKind::Inspect, malformed, &output));
  const size_t string_count = 24 + value.storage.descriptors.size();
  malformed = bytes;
  Put(&malformed, string_count, 256);
  CHECK(!DecodeUsbObservation(UsbObservationKind::Inspect, malformed, &output));
  malformed = bytes;
  Put(&malformed, string_count + 4, 0);
  CHECK(!DecodeUsbObservation(UsbObservationKind::Inspect, malformed, &output));
  malformed = bytes;
  Put(&malformed, string_count + 8, 127);
  CHECK(!DecodeUsbObservation(UsbObservationKind::Inspect, malformed, &output));
  malformed = bytes;
  Put(&malformed, string_count + 4 + 8 + 2 * value.storage.strings.at(1).size(), 1);
  CHECK(!DecodeUsbObservation(UsbObservationKind::Inspect, malformed, &output));
  value.storage.descriptors[20] = 19;
  CHECK(EncodeUsbObservation(UsbObservationKind::Inspect, value, &bytes) == ERROR_INVALID_DATA && bytes.empty());
  value.storage = fixture::observation;
  value.storage.strings[1] = std::wstring(127, L'x');
  CHECK(EncodeUsbObservation(UsbObservationKind::Inspect, value, &bytes) == ERROR_INVALID_DATA && bytes.empty());

  value = {};
  for (int64_t epoch : {int64_t{0}, int64_t{1}, int64_t{0x100000001}, std::numeric_limits<int64_t>::max()}) {
    value.version = epoch;
    CHECK(EncodeUsbObservation(UsbObservationKind::Version, value, &bytes) == ERROR_SUCCESS);
    CHECK(DecodeUsbObservation(UsbObservationKind::Version, bytes, &output) && output.version == epoch);
  }
  Malformed(UsbObservationKind::Version, bytes);
  malformed = bytes;
  Put(&malformed, 24, UINT32_MAX);
  CHECK(!DecodeUsbObservation(UsbObservationKind::Version, malformed, &output));
  value.version = -1;
  CHECK(EncodeUsbObservation(UsbObservationKind::Version, value, &bytes) == ERROR_INVALID_DATA && bytes.empty());
  for (bool available : {false, true}) {
    value.available = available;
    CHECK(EncodeUsbObservation(UsbObservationKind::Available, value, &bytes) == ERROR_SUCCESS);
    CHECK(DecodeUsbObservation(UsbObservationKind::Available, bytes, &output) && output.available == available);
  }
  Malformed(UsbObservationKind::Available, bytes);
  Put(&bytes, 20, 2);
  CHECK(!DecodeUsbObservation(UsbObservationKind::Available, bytes, &output));
  CHECK(EncodeUsbObservation(static_cast<UsbObservationKind>(99), value, &bytes) == ERROR_INVALID_PARAMETER && bytes.empty());
  CHECK(EncodeUsbObservation(UsbObservationKind::Available, value, nullptr) == ERROR_INVALID_PARAMETER);
}

void Query() {
  UsbObservationData output;
  std::vector<std::byte> bytes;
  CHECK(QueryProtectedUsbObservation(UsbObservationKind::Available, {}, &bytes) == ERROR_SUCCESS);
  CHECK(DecodeUsbObservation(UsbObservationKind::Available, bytes, &output) && output.available);
  CHECK(QueryProtectedUsbObservation(UsbObservationKind::Paths, {}, &bytes) == ERROR_SUCCESS);
  CHECK(DecodeUsbObservation(UsbObservationKind::Paths, bytes, &output) && output.paths == fixture::paths);
  CHECK(QueryProtectedUsbObservation(UsbObservationKind::Inspect, fixture::paths[0], &bytes) == ERROR_SUCCESS);
  CHECK(DecodeUsbObservation(UsbObservationKind::Inspect, bytes, &output) &&
        output.storage.descriptors == fixture::observation.descriptors);
  CHECK(QueryProtectedUsbObservation(UsbObservationKind::Version, L"\\\\?\\USB#TEST-READER", &bytes) == ERROR_SUCCESS);
  CHECK(DecodeUsbObservation(UsbObservationKind::Version, bytes, &output) && output.version == fixture::epoch);
  const unsigned versions = fixture::version_calls, inspections = fixture::inspect_calls;
  for (auto kind : {UsbObservationKind::Inspect, UsbObservationKind::Version}) {
    for (const wchar_t* path : {L"\\\\.\\PhysicalDrive0", L"C:\\secret", L"\\\\?\\usb#not-present"}) {
      CHECK(QueryProtectedUsbObservation(kind, path, &bytes) == ERROR_NOT_FOUND && bytes.empty());
    }
    CHECK(QueryProtectedUsbObservation(kind, {}, &bytes) == ERROR_INVALID_PARAMETER && bytes.empty());
  }
  CHECK(fixture::version_calls == versions && fixture::inspect_calls == inspections);
  CHECK(QueryProtectedUsbObservation(UsbObservationKind::Paths, fixture::paths[0], &bytes) == ERROR_INVALID_PARAMETER);
  CHECK(QueryProtectedUsbObservation(UsbObservationKind::Available, fixture::paths[0], &bytes) == ERROR_INVALID_PARAMETER);
  fixture::replace_during_inspect = true;
  CHECK(QueryProtectedUsbObservation(UsbObservationKind::Inspect, fixture::paths[0], &bytes) == ERROR_DEVICE_NOT_CONNECTED && bytes.empty());
  fixture::replace_during_inspect = false;
  fixture::inspect_failed = true;
  CHECK(QueryProtectedUsbObservation(UsbObservationKind::Inspect, fixture::paths[0], &bytes) == ERROR_GEN_FAILURE && bytes.empty());
  fixture::inspect_failed = false;
  fixture::inventory_failed = true;
  CHECK(QueryProtectedUsbObservation(UsbObservationKind::Paths, {}, &bytes) == ERROR_GEN_FAILURE && bytes.empty());
  fixture::inventory_failed = false;
  fixture::paths.assign(129, L"reader");
  CHECK(QueryProtectedUsbObservation(UsbObservationKind::Paths, {}, &bytes) == ERROR_BUFFER_OVERFLOW && bytes.empty());
  fixture::paths.clear();
  CHECK(QueryProtectedUsbObservation(UsbObservationKind::Paths, {}, &bytes) == ERROR_SUCCESS);
  CHECK(DecodeUsbObservation(UsbObservationKind::Paths, bytes, &output) && output.paths.empty());
  fixture::available = false;
  CHECK(QueryProtectedUsbObservation(UsbObservationKind::Paths, {}, &bytes) == ERROR_NOT_SUPPORTED && bytes.empty());
  CHECK(QueryProtectedUsbObservation(UsbObservationKind::Available, {}, &bytes) == ERROR_SUCCESS);
  CHECK(DecodeUsbObservation(UsbObservationKind::Available, bytes, &output) && !output.available);
}

int main() {
  Codec();
  Query();
  std::printf("Protected USB observation codec and query fixture: %u checks passed\n", fixture::checks);
}
