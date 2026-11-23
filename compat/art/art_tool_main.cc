// Gesher: ART's command-line tools (dex2oat, profman) receive Windows argv as
// UTF-16 and pass it on as UTF-8, as dalvikvm does (dalvikvm_main.cc).
//
// The CRT's narrow main() gets the command line in the active code page, but
// gesher-base reads every path as UTF-8 (docs/posix.md). The system server
// runs these tools with an installed app's code path, which is in the profile
// directory under its long name: for a Windows user with a Hebrew name and the
// Hebrew code page, "--dex-file=/Users/<name>/..." arrived as code page 1255
// bytes, the file was "non-existent", and dex2oat failed for every app.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// The tool's own main(), renamed in its source (cmake/aosp/dex2oat.cmake).
int gesher_art_tool_main(int argc, char** argv);

namespace {

// UTF-8 for a UTF-16 argument; false for one that is not valid UTF-16 (a lone
// surrogate), which no Unicode path or option can be.
bool Utf8(const wchar_t* wide, std::string* utf8) {
  utf8->clear();
  if (*wide == L'\0') return true;
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, nullptr, 0, nullptr,
                              nullptr);
  if (n <= 0) return false;
  utf8->resize(static_cast<size_t>(n));
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, utf8->data(), n, nullptr,
                          nullptr) != n) {
    return false;
  }
  utf8->resize(static_cast<size_t>(n) - 1);  // the terminating NUL
  return true;
}

}  // namespace

extern "C" int wmain(int argc, wchar_t** wide_argv) {
  if (argc < 1 || wide_argv == nullptr) return EXIT_FAILURE;
  std::vector<std::string> values(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    if (wide_argv[i] == nullptr || !Utf8(wide_argv[i], &values[static_cast<size_t>(i)])) {
      std::fprintf(stderr, "argument %d is not valid Unicode\n", i);
      return EXIT_FAILURE;
    }
  }
  std::vector<char*> argv;
  argv.reserve(values.size() + 1);
  for (auto& value : values) argv.push_back(value.data());
  argv.push_back(nullptr);
  // The arguments stay alive for the whole call, also across the tool's FastExit.
  return gesher_art_tool_main(argc, argv.data());
}
