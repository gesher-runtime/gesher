// Windows' CRT supplies wide argv before any lossy active-code-page conversion.
// Keep one normal entry adapter; a future approved process-resource bootstrap
// belongs here after CRT startup and before the real ART entry, not in DllMain.
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "exec_spawn.h"

// Only upstream dalvikvm.cc is compiled with this main rename.
int gesher_dalvikvm_main(int argc, char** argv);

extern "C" int wmain(int argc, wchar_t** wide_argv) {
  if (argc < 1 || wide_argv == nullptr) return EXIT_FAILURE;
  std::vector<std::string> values;
  values.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    if (wide_argv[i] == nullptr) return EXIT_FAILURE;
    std::wstring wide(wide_argv[i]);
    std::string utf8 = gesher::exec::Narrow(wide);
    if (gesher::exec::Widen(utf8) != wide) {
      std::fprintf(stderr, "dalvikvm: argument %d is not valid Unicode\n", i);
      return EXIT_FAILURE;
    }
    values.push_back(std::move(utf8));
  }
  std::vector<char*> argv;
  argv.reserve(values.size() + 1);
  for (auto& value : values) argv.push_back(value.data());
  argv.push_back(nullptr);
  // ART retains its original FastExit behavior; argument storage remains live
  // for the whole call, with no VM/font initialization in this adapter.
  return gesher_dalvikvm_main(argc, argv.data());
}
