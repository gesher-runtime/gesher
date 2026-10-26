// Portable editor snapshots and TSF edit transactions (UTF-16 indices).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
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
  enum Kind { kReplace, kSelect, kCompositionRange, kCompositionEnd, kCompositionText };
  Kind kind = kReplace;
  std::u16string text;
  int32_t start = 0, end = 0;
  int32_t epoch = 0, revision = 0;
};
struct TextCharacterBounds {
  int32_t index = -1, left = 0, top = 0, right = 0, bottom = 0;
  bool clipped = false;
  bool operator==(const TextCharacterBounds& other) const {
    return index == other.index && left == other.left && top == other.top
        && right == other.right && bottom == other.bottom && clipped == other.clipped;
  }
};
struct TextGeometry {
  int32_t epoch = 0, start = 0;
  std::u16string text;
  std::vector<TextCharacterBounds> characters; // absolute UTF-16 indices, client pixels
};
}  // namespace gesher::input
