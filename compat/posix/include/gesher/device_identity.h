// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// The PC's own identity, as android.os.Build reports it (docs/device-identity.md):
// ro.product.manufacturer and ro.product.model come from the PC's SMBIOS System
// Information (type 1), or its baseboard's (type 2) when the firmware left the
// system strings as placeholders, else Gesher's documented fallback. A virtual
// machine's SMBIOS names its hypervisor; that is the real identity and is kept.
//
// Header-only: the property area's Initial() (system_properties.cc) and the
// host fixture (tests/native/device_identity_test.cc) compile the same code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace gesher::device_identity {

// Used when neither the system nor the baseboard names a manufacturer or model:
// Build.MANUFACTURER "Gesher", Build.MODEL "PC" (never another vendor's name).
inline constexpr char kFallbackManufacturer[] = "Gesher";
inline constexpr char kFallbackModel[] = "PC";
// PROP_VALUE_MAX - 1: what __system_property_get can return for any value.
inline constexpr size_t kMaxValueBytes = 91;

struct Smbios {
  std::string system_manufacturer, system_product, system_version, system_family;
  std::string board_manufacturer, board_product;
};

struct Identity {
  std::string manufacturer, model;
};

inline char LowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

inline bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
  }
  return true;
}

// An SMBIOS string as a property value: whitespace runs collapsed to one space
// and trimmed, other control characters and bytes that are not well-formed
// UTF-8 dropped, cut at a whole character within kMaxValueBytes.
inline std::string Clean(std::string_view raw) {
  std::string out;
  bool pending_space = false;
  for (size_t i = 0; i < raw.size();) {
    const auto c = static_cast<unsigned char>(raw[i]);
    size_t n = c < 0x80 ? 1 : (c >= 0xc2 && c <= 0xdf) ? 2 : (c >= 0xe0 && c <= 0xef) ? 3
             : (c >= 0xf0 && c <= 0xf4) ? 4 : 0;
    bool valid = n != 0 && n <= raw.size() - i;
    for (size_t k = 1; valid && k < n; ++k) {
      valid = (static_cast<unsigned char>(raw[i + k]) & 0xc0) == 0x80;
    }
    if (valid && n >= 3) {
      const auto second = static_cast<unsigned char>(raw[i + 1]);
      // No overlong forms, surrogates or values past U+10FFFF.
      if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0) ||
          (c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90)) valid = false;
    }
    if (!valid) { ++i; continue; }
    if (n == 1 && (c <= 0x20 || c == 0x7f)) {
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') pending_space = true;
      ++i;
      continue;
    }
    size_t extra = pending_space && !out.empty() ? 1 : 0;
    if (out.size() + extra + n > kMaxValueBytes) break;
    if (extra) out += ' ';
    pending_space = false;
    out.append(raw.data() + i, n);
    i += n;
  }
  return out;
}

// Whether a cleaned SMBIOS string is a firmware placeholder rather than a name:
// empty, no letter or digit, one repeated character ("0000", "xxxx"), or one of
// the template strings BIOS vendors ship unfilled (AMI's "To Be Filled By
// O.E.M." and "Default string", ASUS's "System manufacturer"/"System Product
// Name", Insyde's "Type1..."/"Type2 - ..." names), compared case-insensitively.
inline bool Placeholder(std::string_view value) {
  if (value.empty()) return true;
  bool alphanumeric = false, repeated = true;
  for (char c : value) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        static_cast<unsigned char>(c) >= 0x80) alphanumeric = true;
    if (c != value[0]) repeated = false;
  }
  if (!alphanumeric || repeated) return true;
  static constexpr const char* kPlaceholders[] = {
      "to be filled by o.e.m.", "to be filled by o.e.m", "to be filled by oem",
      "system manufacturer", "system manufacture", "system product name", "system product",
      "system name", "system version", "system family", "system sku", "system serial number",
      "default string", "default", "not applicable", "not specified", "not available",
      "not defined", "none", "n/a", "na", "unknown", "undefined", "invalid", "empty", "null",
      "oem", "o.e.m.", "oem name", "oem product", "manufacturer", "product name", "product",
      "board manufacturer", "board product name", "base board manufacturer",
      "base board product name", "baseboard manufacturer", "baseboard product name",
      "motherboard", "mainboard", "family", "sku", "123456789", "1234567890", "0123456789",
      "chassis manufacturer", "x.x", "insyde", "insyde corp."};
  for (const char* placeholder : kPlaceholders) {
    if (EqualsIgnoreCase(value, placeholder)) return true;
  }
  static constexpr const char* kPrefixes[] = {"type1", "type2 - ", "type2-"};
  for (const char* prefix : kPrefixes) {
    size_t n = std::strlen(prefix);
    if (value.size() >= n && EqualsIgnoreCase(value.substr(0, n), prefix)) return true;
  }
  return false;
}

// The structure table, as SMBIOS (DSP0134) lays it out: each structure's type,
// formatted length and handle, its formatted area, then its strings, each
// NUL-terminated, ending with a second NUL. Only the first type 1 and type 2
// structures are read. Malformed or truncated data ends the walk.
inline Smbios ParseSmbiosTable(const uint8_t* table, size_t size) {
  Smbios result;
  bool system = false, board = false;
  size_t at = 0;
  while (table && size >= 4 && at <= size - 4) {
    const uint8_t type = table[at];
    const uint8_t length = table[at + 1];
    if (length < 4 || length > size - at) break;
    size_t end = at + length;  // the first NUL of the closing pair
    while (end + 1 < size && !(table[end] == 0 && table[end + 1] == 0)) ++end;
    if (end + 1 >= size) break;
    std::vector<std::string_view> strings;
    for (size_t s = at + length; s < end;) {
      size_t e = s;
      while (e < end && table[e]) ++e;
      strings.emplace_back(reinterpret_cast<const char*>(table + s), e - s);
      s = e + 1;
    }
    auto text = [&](size_t offset) -> std::string {
      if (offset >= length) return {};
      uint8_t index = table[at + offset];
      if (index == 0 || index > strings.size()) return {};
      return Clean(strings[index - 1]);
    };
    if (type == 1 && !system) {
      system = true;
      result.system_manufacturer = text(0x04);
      result.system_product = text(0x05);
      result.system_version = text(0x06);
      result.system_family = text(0x1a);  // SMBIOS 2.4 and later
    } else if (type == 2 && !board) {
      board = true;
      result.board_manufacturer = text(0x04);
      result.board_product = text(0x05);
    }
    if (type == 127 || (system && board)) break;  // end of table
    at = end + 2;
  }
  return result;
}

// What GetSystemFirmwareTable('RSMB', 0) returns (RawSMBIOSData): the calling
// method, major and minor version and DMI revision bytes, the table's 32-bit
// length, then the table.
inline Smbios ParseRawSmbios(const uint8_t* data, size_t size) {
  if (!data || size < 8) return {};
  uint32_t length;
  std::memcpy(&length, data + 4, sizeof(length));
  return ParseSmbiosTable(data + 8, length < size - 8 ? length : size - 8);
}

// Build.MANUFACTURER: the system manufacturer, else the baseboard's, else
// kFallbackManufacturer. Build.MODEL: the system product name, else the system
// family, else the baseboard product, else kFallbackModel. Lenovo puts the
// machine-type code in the product name (for example "20HRCTO1WW") and the
// model's name in the system version ("ThinkPad X1 Carbon 5th"), so a usable
// Lenovo system version is the model.
inline Identity Resolve(const Smbios& s) {
  Identity id;
  const bool system_manufacturer = !Placeholder(s.system_manufacturer);
  id.manufacturer = system_manufacturer ? s.system_manufacturer
                    : !Placeholder(s.board_manufacturer) ? s.board_manufacturer
                                                         : std::string(kFallbackManufacturer);
  if (system_manufacturer && EqualsIgnoreCase(s.system_manufacturer, "LENOVO") &&
      !Placeholder(s.system_version) && !EqualsIgnoreCase(s.system_version, s.system_manufacturer)) {
    id.model = s.system_version;
  } else if (!Placeholder(s.system_product)) {
    id.model = s.system_product;
  } else if (!Placeholder(s.system_family)) {
    id.model = s.system_family;
  } else if (!Placeholder(s.board_product)) {
    id.model = s.board_product;
  } else {
    id.model = kFallbackModel;
  }
  return id;
}

inline std::vector<std::pair<std::string, std::string>> Properties(const Identity& id) {
  return {{"ro.product.manufacturer", id.manufacturer}, {"ro.product.model", id.model}};
}

#ifdef _WIN32
// The firmware's SMBIOS table through the documented kernel32 call (no WMI).
// An empty Smbios when Windows exposes none; Resolve then gives the fallback.
inline Smbios ReadSmbios() {
  constexpr DWORD kRsmb = (DWORD{'R'} << 24) | (DWORD{'S'} << 16) | (DWORD{'M'} << 8) | DWORD{'B'};
  UINT size = GetSystemFirmwareTable(kRsmb, 0, nullptr, 0);
  if (size == 0) return {};
  std::vector<uint8_t> data(size);
  UINT read = GetSystemFirmwareTable(kRsmb, 0, data.data(), size);
  if (read == 0 || read > size) return {};
  return ParseRawSmbios(data.data(), read);
}

inline Identity Read() { return Resolve(ReadSmbios()); }

// Windows' computer name as Settings > System > About shows it: the physical
// DNS host name (case kept, up to 63 characters; not the 15-character
// upper-case NetBIOS name). Empty when Windows does not give one.
inline std::string ComputerName() {
  wchar_t name[MAX_COMPUTERNAME_LENGTH + 256];
  DWORD count = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
  if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &count) || count == 0) return {};
  int bytes = WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(count), nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) return {};
  std::string utf8(static_cast<size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(count), utf8.data(), bytes, nullptr, nullptr);
  return Clean(utf8);
}
#endif

}  // namespace gesher::device_identity
