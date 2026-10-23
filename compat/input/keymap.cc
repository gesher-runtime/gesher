// Gesher: the virtual keyboard's key character map. See keymap.h.

#define LOG_TAG "GesherInput"

#include "keymap.h"

#include <stdlib.h>
#include <unistd.h>

#include <string>

#include <log/log.h>

namespace gesher {
namespace input {

std::unique_ptr<android::KeyCharacterMap> LoadGenericKeyCharacterMap() {
  using android::KeyCharacterMap;
  const char* root = getenv("ANDROID_ROOT");
  if (root != nullptr && *root != '\0') {
    std::string path = std::string(root) + "/usr/keychars/Generic.kcm";
    if (access(path.c_str(), R_OK) == 0) {
      auto map = KeyCharacterMap::load(path, KeyCharacterMap::Format::BASE);
      if (map.ok()) return std::move(*map);
      ALOGW("%s: %s; using the built-in Generic.kcm", path.c_str(),
            map.error().message().c_str());
    }
  }
  auto map = KeyCharacterMap::loadContents("Generic.kcm (built in)", gesher_generic_kcm,
                                           KeyCharacterMap::Format::BASE);
  if (!map.ok()) {
    ALOGE("the built-in Generic.kcm does not load: %s", map.error().message().c_str());
    return nullptr;
  }
  return std::make_unique<KeyCharacterMap>(**map);
}

}  // namespace input
}  // namespace gesher
