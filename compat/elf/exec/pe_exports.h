// Read PE export names without loading code or resolving the target's imports.
// Layout: Microsoft's PE/COFF specification, "Export Tables" and "Section Table".
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gesher::exec {
inline bool HasFileDescriptionImporter(const void* file, size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(file);
  auto contains = [&](size_t offset, size_t length) {
    return offset <= size && length <= size - offset;
  };
  auto word = [&](size_t offset) -> uint16_t {
    uint16_t value = 0;
    if (contains(offset, sizeof(value))) std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
  };
  auto dword = [&](size_t offset) -> uint32_t {
    uint32_t value = 0;
    if (contains(offset, sizeof(value))) std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
  };
  if (!contains(0, 64) || word(0) != 0x5a4d) return false;
  const size_t pe = dword(60);
  if (!contains(pe, 24) || dword(pe) != 0x00004550) return false;
  const size_t optional = pe + 24;
  const size_t optional_size = word(pe + 20);
  const size_t section_count = word(pe + 6);
  if (!contains(optional, optional_size)) return false;
  const uint16_t magic = word(optional);
  const size_t directory = magic == 0x20b ? 112 : magic == 0x10b ? 96 : 0;
  if (!directory || optional_size < directory + 8 || dword(optional + directory - 4) == 0) return false;
  const uint32_t export_rva = dword(optional + directory);
  const uint32_t export_size = dword(optional + directory + 4);
  if (!export_rva || export_size < 40) return false;
  const size_t sections = optional + optional_size;
  if (section_count > (size - sections) / 40) return false;
  const uint32_t headers_size = dword(optional + 60);
  auto rva = [&](uint32_t address, size_t length) -> const unsigned char* {
    if (address < headers_size && length <= headers_size - address && contains(address, length))
      return bytes + address;
    for (size_t i = 0; i < section_count; ++i) {
      const size_t section = sections + i * 40;
      const uint32_t base = dword(section + 12);
      const uint32_t raw_size = dword(section + 16);
      if (address < base || address - base > raw_size || length > raw_size - (address - base)) continue;
      const uint64_t offset = static_cast<uint64_t>(dword(section + 20)) + address - base;
      if (offset <= size && contains(static_cast<size_t>(offset), length))
        return bytes + static_cast<size_t>(offset);
    }
    return nullptr;
  };
  const unsigned char* exports = rva(export_rva, 40);
  if (!exports) return false;
  auto field = [](const unsigned char* at) {
    uint32_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
  };
  const uint32_t function_count = field(exports + 20);
  const uint32_t name_count = field(exports + 24);
  if (static_cast<uint64_t>(function_count) * 4 > size || static_cast<uint64_t>(name_count) * 4 > size)
    return false;
  const unsigned char* functions = rva(field(exports + 28), static_cast<size_t>(function_count) * 4);
  const unsigned char* names = rva(field(exports + 32), static_cast<size_t>(name_count) * 4);
  const unsigned char* ordinals = rva(field(exports + 36), static_cast<size_t>(name_count) * 2);
  if (!functions || !names || !ordinals) return false;
  constexpr char marker[] = "gesher_ofd_owner_v1";
  for (uint32_t i = 0; i < name_count; ++i) {
    const auto* name = rva(field(names + static_cast<size_t>(i) * 4), sizeof(marker));
    if (!name || std::memcmp(name, marker, sizeof(marker)) != 0) continue;
    uint16_t ordinal;
    std::memcpy(&ordinal, ordinals + static_cast<size_t>(i) * 2, sizeof(ordinal));
    if (ordinal >= function_count) return false;
    const uint32_t function = field(functions + static_cast<size_t>(ordinal) * 4);
    // A forwarding export does not install this executable's startup importer.
    return function && !(function >= export_rva && function - export_rva < export_size) && rva(function, 1);
  }
  return false;
}
}  // namespace gesher::exec
