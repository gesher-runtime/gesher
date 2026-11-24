// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "protected_usb_observation.h"

#include <windows.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

#include "../devsvc/usb_winusb.h"

namespace gesher::usb_storage {
namespace {

constexpr uint32_t kMagic = 0x314f4255;  // UBO1
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderBytes = 20;
constexpr size_t kDescriptorLimit = 18 + 65535;
constexpr size_t kStringUnits = 126;  // One real USB string descriptor.

bool ValidKind(UsbObservationKind kind) {
  return kind == UsbObservationKind::Paths || kind == UsbObservationKind::Inspect ||
      kind == UsbObservationKind::Version || kind == UsbObservationKind::Available;
}

bool ValidPath(std::wstring_view path) {
  return !path.empty() && path.size() <= kUsbObservationPathUnits &&
      path.find(L'\0') == std::wstring_view::npos;
}

bool Same(std::wstring_view left, std::wstring_view right) {
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                              static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool UniquePaths(const std::vector<std::wstring>& paths) {
  for (size_t i = 0; i < paths.size(); ++i) {
    if (!ValidPath(paths[i])) return false;
    for (size_t j = 0; j < i; ++j) if (Same(paths[i], paths[j])) return false;
  }
  return true;
}

bool Descriptors(const std::vector<unsigned char>& bytes) {
  // ObserveStorage returns one complete device descriptor and its one actual
  // configuration. Keep the bytes verbatim; do not invent missing descriptors.
  if (bytes.size() < 27 || bytes.size() > kDescriptorLimit || bytes[0] != 18 || bytes[1] != 1 ||
      bytes[17] != 1 || bytes[18] != 9 || bytes[19] != 2) return false;
  const size_t configuration = bytes[20] | (static_cast<size_t>(bytes[21]) << 8);
  if (configuration != bytes.size() - 18) return false;
  for (size_t at = 18; at < bytes.size();) {
    const size_t count = bytes[at];
    if (count < 2 || count > bytes.size() - at) return false;
    at += count;
  }
  return true;
}

class Writer {
 public:
  bool Number(uint32_t number) {
    if (!Space(4)) return false;
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(std::byte(number >> shift));
    return true;
  }
  bool Text(std::wstring_view text) {
    static_assert(sizeof(wchar_t) == 2);
    if (text.size() > kUsbObservationPathUnits || !Number(static_cast<uint32_t>(text.size())) ||
        !Space(text.size() * 2)) return false;
    for (wchar_t value : text) {
      bytes.push_back(std::byte(value & 255));
      bytes.push_back(std::byte((static_cast<uint16_t>(value) >> 8) & 255));
    }
    return true;
  }
  bool Raw(const std::vector<unsigned char>& input) {
    if (input.size() > kDescriptorLimit || !Number(static_cast<uint32_t>(input.size())) ||
        !Space(input.size())) return false;
    for (unsigned char value : input) bytes.push_back(std::byte(value));
    return true;
  }
  std::vector<std::byte> bytes;
 private:
  bool Space(size_t size) const { return size <= kUsbObservationLimit - bytes.size(); }
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
  bool Number(uint32_t* number) {
    if (bytes_.size() < 4) return false;
    *number = 0;
    for (unsigned i = 0; i < 4; ++i) *number |= std::to_integer<uint32_t>(bytes_[i]) << (i * 8);
    bytes_ = bytes_.subspan(4);
    return true;
  }
  bool Text(std::wstring* text, size_t limit) {
    uint32_t count = 0;
    if (!Number(&count) || count > limit || count > bytes_.size() / 2) return false;
    text->clear();
    text->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t value = std::to_integer<uint32_t>(bytes_[2 * i]) |
          (std::to_integer<uint32_t>(bytes_[2 * i + 1]) << 8);
      text->push_back(static_cast<wchar_t>(value));
    }
    bytes_ = bytes_.subspan(static_cast<size_t>(count) * 2);
    return true;
  }
  bool Raw(std::vector<unsigned char>* output) {
    uint32_t count = 0;
    if (!Number(&count) || count > kDescriptorLimit || count > bytes_.size()) return false;
    output->clear();
    output->reserve(count);
    for (std::byte value : bytes_.first(count)) output->push_back(std::to_integer<unsigned char>(value));
    bytes_ = bytes_.subspan(count);
    return true;
  }
  bool empty() const { return bytes_.empty(); }
 private:
  std::span<const std::byte> bytes_;
};

}  // namespace

uint32_t EncodeUsbObservation(UsbObservationKind kind, const UsbObservationData& value,
                              std::vector<std::byte>* output) try {
  if (!output) return ERROR_INVALID_PARAMETER;
  output->clear();
  if (!ValidKind(kind)) return ERROR_INVALID_PARAMETER;
  Writer writer;
  if (!writer.Number(kMagic) || !writer.Number(kVersion) ||
      !writer.Number(static_cast<uint32_t>(kind)) || !writer.Number(0) || !writer.Number(0))
    return ERROR_BUFFER_OVERFLOW;
  switch (kind) {
    case UsbObservationKind::Paths:
      if (value.paths.size() > kUsbObservationMaxPaths) return ERROR_BUFFER_OVERFLOW;
      if (!UniquePaths(value.paths)) return ERROR_INVALID_DATA;
      if (!writer.Number(static_cast<uint32_t>(value.paths.size()))) return ERROR_BUFFER_OVERFLOW;
      for (const auto& path : value.paths) if (!writer.Text(path)) return ERROR_BUFFER_OVERFLOW;
      break;
    case UsbObservationKind::Inspect:
      if (!Descriptors(value.storage.descriptors) || value.storage.strings.size() > 255)
        return ERROR_INVALID_DATA;
      if (!writer.Raw(value.storage.descriptors) ||
          !writer.Number(static_cast<uint32_t>(value.storage.strings.size()))) return ERROR_BUFFER_OVERFLOW;
      for (const auto& [index, text] : value.storage.strings) {
        if (!index || text.size() > kStringUnits) return ERROR_INVALID_DATA;
        if (!writer.Number(index) || !writer.Text(text)) return ERROR_BUFFER_OVERFLOW;
      }
      break;
    case UsbObservationKind::Version:
      if (value.version < 0) return ERROR_INVALID_DATA;
      if (!writer.Number(static_cast<uint32_t>(value.version)) ||
          !writer.Number(static_cast<uint32_t>(static_cast<uint64_t>(value.version) >> 32)))
        return ERROR_BUFFER_OVERFLOW;
      break;
    case UsbObservationKind::Available:
      if (!writer.Number(value.available ? 1 : 0)) return ERROR_BUFFER_OVERFLOW;
      break;
  }
  const uint32_t payload = static_cast<uint32_t>(writer.bytes.size() - kHeaderBytes);
  for (unsigned i = 0; i < 4; ++i) writer.bytes[12 + i] = std::byte(payload >> (i * 8));
  *output = std::move(writer.bytes);
  return ERROR_SUCCESS;
} catch (const std::bad_alloc&) {
  if (output) output->clear();
  return ERROR_NOT_ENOUGH_MEMORY;
}

bool DecodeUsbObservation(UsbObservationKind kind, std::span<const std::byte> bytes,
                          UsbObservationData* value) try {
  if (!value || !ValidKind(kind) || bytes.size() < kHeaderBytes || bytes.size() > kUsbObservationLimit)
    return false;
  Reader reader(bytes);
  uint32_t magic = 0, version = 0, encoded_kind = 0, payload = 0, reserved = 0;
  if (!reader.Number(&magic) || !reader.Number(&version) || !reader.Number(&encoded_kind) ||
      !reader.Number(&payload) || !reader.Number(&reserved) || magic != kMagic || version != kVersion ||
      encoded_kind != static_cast<uint32_t>(kind) || reserved || payload != bytes.size() - kHeaderBytes)
    return false;
  UsbObservationData decoded;
  uint32_t count = 0;
  switch (kind) {
    case UsbObservationKind::Paths:
      if (!reader.Number(&count) || count > kUsbObservationMaxPaths) return false;
      decoded.paths.resize(count);
      for (auto& path : decoded.paths) if (!reader.Text(&path, kUsbObservationPathUnits)) return false;
      if (!UniquePaths(decoded.paths)) return false;
      break;
    case UsbObservationKind::Inspect:
      if (!reader.Raw(&decoded.storage.descriptors) || !Descriptors(decoded.storage.descriptors) ||
          !reader.Number(&count) || count > 255) return false;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t index = 0;
        std::wstring text;
        if (!reader.Number(&index) || !index || index > 255 || !reader.Text(&text, kStringUnits) ||
            !decoded.storage.strings.emplace(static_cast<unsigned char>(index), std::move(text)).second)
          return false;
      }
      break;
    case UsbObservationKind::Version: {
      uint32_t low = 0, high = 0;
      if (!reader.Number(&low) || !reader.Number(&high) || high > INT32_MAX) return false;
      decoded.version = static_cast<int64_t>((uint64_t{high} << 32) | low);
      break;
    }
    case UsbObservationKind::Available:
      if (!reader.Number(&count) || count > 1) return false;
      decoded.available = count != 0;
      break;
  }
  if (!reader.empty()) return false;
  *value = std::move(decoded);
  return true;
} catch (const std::bad_alloc&) {
  return false;
}

uint32_t QueryProtectedUsbObservation(UsbObservationKind kind, std::wstring_view path,
                                      std::vector<std::byte>* output) try {
  if (!output) return ERROR_INVALID_PARAMETER;
  output->clear();
  if (!ValidKind(kind)) return ERROR_INVALID_PARAMETER;
  const bool needs_path = kind == UsbObservationKind::Inspect || kind == UsbObservationKind::Version;
  if (needs_path ? !ValidPath(path) : !path.empty()) return ERROR_INVALID_PARAMETER;
  UsbObservationData value;
  value.available = usb::AttachmentMonitoringAvailable();
  if (kind == UsbObservationKind::Available) return EncodeUsbObservation(kind, value, output);
  if (!value.available) return ERROR_NOT_SUPPORTED;
  auto paths = usb::StorageInterfacePaths();
  if (!paths) return ERROR_GEN_FAILURE;
  if (paths->size() > kUsbObservationMaxPaths) return ERROR_BUFFER_OVERFLOW;
  if (!UniquePaths(*paths)) return ERROR_INVALID_DATA;
  if (kind == UsbObservationKind::Paths) {
    value.paths = std::move(*paths);
    return EncodeUsbObservation(kind, value, output);
  }
  const auto selected = std::find_if(paths->begin(), paths->end(), [&](const auto& candidate) {
    return Same(path, candidate);
  });
  if (selected == paths->end()) return ERROR_NOT_FOUND;
  value.version = usb::AttachmentVersion(selected->c_str());
  if (value.version < 0) return ERROR_NOT_SUPPORTED;
  if (kind == UsbObservationKind::Inspect) {
    auto observation = usb::ObserveStorage(*selected);
    if (!observation) return ERROR_GEN_FAILURE;
    if (usb::AttachmentVersion(selected->c_str()) != value.version) return ERROR_DEVICE_NOT_CONNECTED;
    value.storage = std::move(*observation);
  }
  return EncodeUsbObservation(kind, value, output);
} catch (const std::bad_alloc&) {
  if (output) output->clear();
  return ERROR_NOT_ENOUGH_MEMORY;
}

}  // namespace gesher::usb_storage
