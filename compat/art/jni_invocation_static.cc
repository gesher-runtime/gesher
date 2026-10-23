// Gesher: JniInvocation for a runtime linked into the same executable.
//
// libnativehelper's JniInvocation dlopen()s libart.so and forwards
// JNI_CreateJavaVM and friends to it. Gesher links ART statically, so the
// runtime's own JNI_CreateJavaVM is already there and there is nothing to load.

#include "nativehelper/JniInvocation.h"

#include <stdlib.h>

#include <string>

#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/strings.h>

struct JniInvocationImpl {};

namespace {
JniInvocationImpl the_instance;

// On a device, init loads /system/build.prop into the system properties before
// any process starts, and ART reads some of them while it is constructed:
// Runtime's sdk_version_ is ro.build.version.sdk, and with it unset ART treats
// every static final field as unmodifiable, so the framework's
// Typeface.setSystemFontMap (nativeForceSetStaticFinalField) aborts. gesher-app
// loads build.prop from Java, after the runtime is up; dalvikvm calls
// JniInvocationInit just before JNI_CreateJavaVM, so it is loaded here too,
// from $ANDROID_ROOT/build.prop, with the same syntax (BuildProps.java).
void LoadBuildProp() {
  const char* root = getenv("ANDROID_ROOT");
  if (root == nullptr) return;
  std::string text;
  if (!android::base::ReadFileToString(std::string(root) + "/build.prop", &text)) return;
  for (const std::string& raw : android::base::Split(text, "
")) {
    std::string line = android::base::Trim(raw);
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == 0 || eq == std::string::npos) continue;
    std::string key = android::base::Trim(line.substr(0, eq));
    // init's "?=" (set if unset) is taken as a plain assignment, as in BuildProps.
    if (!key.empty() && key.back() == '?') key = android::base::Trim(key.substr(0, key.size() - 1));
    android::base::SetProperty(key, android::base::Trim(line.substr(eq + 1)));
  }
}
}  // namespace

extern "C" {

JniInvocationImpl* JniInvocationCreate() { return &the_instance; }

bool JniInvocationInit(JniInvocationImpl*, const char*) {
  LoadBuildProp();
  return true;
}

void JniInvocationDestroy(JniInvocationImpl*) {}

const char* JniInvocationGetLibrary(const char* library, char*) {
  return library != nullptr ? library : "(linked in)";
}

}  // extern "C"
