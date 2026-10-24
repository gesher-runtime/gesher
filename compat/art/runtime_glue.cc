// Gesher: small pieces libart-runtime expects from modules Gesher does not
// build (yet).

#include "base/flags.h"
#include "com_android_art_rw_flags.h"

namespace art {

// libartbase/base/flags.cc leaves this out on Windows, where AOSP has no
// aconfig; Gesher generates the aconfig headers itself.
bool is_test_rw_flag_enabled() { return COM_ANDROID_ART_RW_FLAGS_TEST_RW_FLAG; }

}  // namespace art
