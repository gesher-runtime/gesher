// Portable editor snapshots and TSF edit transactions (UTF-16 indices).
#pragma once
#include <cstdint>
#include <string>
namespace gesher::input {
struct TextDocument {
  std::u16string text;
  int32_t start = 0, end = 0;
  int32_t offset = 0;
  int32_t epoch = 0, applied_revision = 0;
  bool available = false;
  bool reversed = false;
};
struct DocumentEdit {
  enum Kind { kReplace, kSelect, kCompositionRange, kCompositionEnd };
  Kind kind = kReplace;
  std::u16string text;
  int32_t start = 0, end = 0;
  int32_t epoch = 0, revision = 0;
};
}  // namespace gesher::input
