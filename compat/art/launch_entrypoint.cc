// The real dalvikvm parser records its entrypoint after validating that the
// Windows command-line vector is identical to the CRT argv it parsed.
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
// 0 means unset, -1 means the two argument domains did not match.
std::atomic<int> entrypoint_index{0};
std::wstring launch_prefix;

void AppendQuoted(std::wstring& out, const wchar_t* argument) {
  if (!out.empty()) out.push_back(L' ');
  out.push_back(L'"');
  unsigned slashes = 0;
  for (const wchar_t* p = argument; *p; ++p) {
    if (*p == L'\\') { ++slashes; continue; }
    if (*p == L'"') {
      out.append(slashes * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(slashes, L'\\');
      out.push_back(*p);
    }
    slashes = 0;
  }
  out.append(slashes * 2, L'\\');
  out.push_back(L'"');
}
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
    if (equal) {
      for (int i = 0; i < index; ++i) AppendQuoted(launch_prefix, wide[i]);
      result = index;
    }
  }
  if (wide) LocalFree(wide);
  int expected = 0;
  if (!entrypoint_index.compare_exchange_strong(expected, result)) std::abort();
}

extern "C" const wchar_t* gesher_vm_launch_prefix() {
  return entrypoint_index.load() > 0 ? launch_prefix.c_str() : nullptr;
}
