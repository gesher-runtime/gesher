// State belongs to a physical key, not the last key in a chord.
#pragma once

#include <array>
#include <limits>
#include <stdint.h>

namespace gesher::input {

class KeyHistory {
 public:
  struct Press {
    int64_t down_time = 0;
    int32_t key_code = 0;
    int32_t repeat_count = 0;
    bool held = false;
  };

  Press Update(int32_t vk, int32_t scan, int32_t code, bool down, int64_t t) {
    // Scanless synthetic keys have a separate, bounded virtual-key namespace.
    int index = scan > 0 && scan <= 0x1ff ? scan
                : vk >= 0 && vk <= 0xff ? 512 + vk : -1;
    Press fresh{t, code, 0, down};
    if (index < 0) return fresh;
    Press& stored = keys_[index];
    if (down) {
      if (!stored.held) {
        stored = fresh;
      } else if (stored.repeat_count < std::numeric_limits<int32_t>::max()) {
        ++stored.repeat_count;
      }
      return stored;
    }
    Press result = stored.held ? stored : fresh;
    result.repeat_count = 0;
    result.held = false;
    stored = {};
    return result;
  }

  void Reset() { keys_.fill({}); }

 private:
  std::array<Press, 768> keys_{};
};

}  // namespace gesher::input
