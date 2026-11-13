// Bounded sensor data and flush completions. Callers serialize access.
#pragma once

#include <algorithm>
#include <deque>
#include <stddef.h>

#include "sensors.h"

namespace gesher::devices::sensors {

struct Event {
  int handle;
  bool flush;
  Reading reading;
  int64_t timestampNs;
};

class EventBuffer {
 public:
  static constexpr size_t kCapacity = 256;

  bool PushData(const Event& event) {
    if (events_.size() == kCapacity) {
      auto data = std::find_if(events_.begin(), events_.end(),
                               [](const Event& e) { return !e.flush; });
      if (data == events_.end()) return false;
      events_.erase(data);
    }
    events_.push_back(event);
    return true;
  }

  bool Flush(const std::vector<int>& handles) {
    const size_t controls = static_cast<size_t>(std::count_if(
        events_.begin(), events_.end(), [](const Event& e) { return e.flush; }));
    // Admission is atomic for every enabled sensor: never return success
    // for a completion that a later sample can evict, or accept a partial set.
    if (handles.size() > kCapacity - controls) return false;
    while (events_.size() + handles.size() > kCapacity) {
      auto data = std::find_if(events_.begin(), events_.end(),
                               [](const Event& e) { return !e.flush; });
      events_.erase(data);  // the admission check guarantees one exists
    }
    for (int handle : handles) events_.push_back({handle, true, Reading(), 0});
    return true;
  }

  std::deque<Event> Take() {
    std::deque<Event> result;
    result.swap(events_);
    return result;
  }

 private:
  std::deque<Event> events_;
};

}  // namespace gesher::devices::sensors
