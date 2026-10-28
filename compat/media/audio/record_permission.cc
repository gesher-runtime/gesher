#include "record_permission.h"

#include <atomic>

namespace gesher::media {
namespace {
std::atomic<RecordingPermissionProvider> g_provider{nullptr};
}

void SetRecordingPermissionProvider(RecordingPermissionProvider provider) {
  g_provider.store(provider, std::memory_order_release);
}

bool RecordingAllowed() {
  const auto provider = g_provider.load(std::memory_order_acquire);
  return provider != nullptr && provider();
}

}  // namespace gesher::media
