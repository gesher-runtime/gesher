// Gesher: hwui's SharedLib (utils/SharedLib.h). On a device and on Linux hosts
// it dlopens libandroid(_runtime).so for ANativeWindow_fromSurface; in Gesher
// hwui and libandroid_runtime are one executable, so the symbol is at hand.
#include "utils/SharedLib.h"

#include <string.h>

#include <android/native_window_jni.h>

namespace android {
namespace uirenderer {

void* SharedLib::openSharedLib(std::string) {
  static char self;
  return &self;
}

void* SharedLib::getSymbol(void*, const char* symbol) {
  if (strcmp(symbol, "ANativeWindow_fromSurface") == 0) {
    return reinterpret_cast<void*>(&ANativeWindow_fromSurface);
  }
  return nullptr;
}

}  // namespace uirenderer
}  // namespace android
