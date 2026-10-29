// The real dalvikvm parser records its entrypoint after validating that the
// Windows command-line vector is identical to the CRT argv it parsed.
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
// 0 means unset, -1 means the two argument domains did not match.
std::atomic<int> entrypoint_index{0};
}

extern "C" void gesher_record_vm_entrypoint_index(int index, int argc, char** argv) {
  int result = -1;
  int count = 0;
  wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
  if (wide && count == argc && index > 0 && index < count) {
    bool equal = true;
    for (int i = 0; i < count && equal; ++i) {
      int size = WideCharToMultiByte(CP_ACP, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
      if (size <= 0) { equal = false; break; }
      std::vector<char> value(static_cast<size_t>(size));
      equal = WideCharToMultiByte(CP_ACP, 0, wide[i], -1, value.data(), size, nullptr, nullptr) == size &&
          std::strcmp(value.data(), argv[i]) == 0;
    }
    if (equal) result = index;
  }
  if (wide) LocalFree(wide);
  int expected = 0;
  if (!entrypoint_index.compare_exchange_strong(expected, result)) std::abort();
}

extern "C" int gesher_vm_entrypoint_index() {
  return entrypoint_index.load();
}
