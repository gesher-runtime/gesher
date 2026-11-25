// Windows environment transport accepts WTF8 as well as UTF8 so an inherited
// unpaired UTF16 surrogate survives a POSIX environment snapshot unchanged.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace gesher {
inline bool DecodeEnvironmentUtf8(std::string_view bytes, std::wstring* output) {
  std::wstring wide;
  wide.reserve(bytes.size());
  for (size_t index = 0; index < bytes.size();) {
    uint32_t value = static_cast<unsigned char>(bytes[index++]);
    unsigned following = 0;
    uint32_t minimum = 0;
    if (value < 0x80) {}
    else if (value >= 0xc2 && value <= 0xdf) { value &= 31; following = 1; minimum = 0x80; }
    else if (value >= 0xe0 && value <= 0xef) { value &= 15; following = 2; minimum = 0x800; }
    else if (value >= 0xf0 && value <= 0xf4) { value &= 7; following = 3; minimum = 0x10000; }
    else return false;
    if (following > bytes.size() - index) return false;
    while (following--) {
      const auto next = static_cast<unsigned char>(bytes[index++]);
      if ((next & 0xc0) != 0x80) return false;
      value = (value << 6) | (next & 63);
    }
    if (value < minimum || value > 0x10ffff) return false;
    if (value < 0x10000) wide.push_back(static_cast<wchar_t>(value));
    else {
      value -= 0x10000;
      wide.push_back(static_cast<wchar_t>(0xd800 + (value >> 10)));
      wide.push_back(static_cast<wchar_t>(0xdc00 + (value & 1023)));
    }
  }
  output->swap(wide);
  return true;
}
}  // namespace gesher
