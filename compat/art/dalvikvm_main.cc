// Windows' CRT supplies wide argv before any lossy active-code-page conversion.
// Initialize process defaults after CRT startup and before the real ART entry,
// so both standalone Java tools and system services see the same host resources.
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <gesher/unc_paths.h>
#include "exec_spawn.h"

// Only upstream dalvikvm.cc is compiled with this main rename.
int gesher_dalvikvm_main(int argc, char** argv);

namespace {

// Match dalvikvm.cc's option boundary: a main-class argument that looks like a
// VM option, or a -cp operand, must not suppress initialization for this VM.
bool HasTemporaryDirectoryProperty(const std::vector<std::string>& arguments) {
  bool operand = false;
  for (std::size_t i = 1; i < arguments.size(); ++i) {
    const std::string& argument = arguments[i];
    if (!operand && (argument.empty() || argument[0] != '-')) break;
    if (argument.rfind("-XXlib:", 0) == 0) continue;
    if (!operand && argument.rfind("-Djava.io.tmpdir=", 0) == 0) return true;
    operand = argument == "-cp" || argument == "-classpath";
  }
  return false;
}

bool ReadTemporaryDirectory(std::wstring* value, bool* provided) {
  SetLastError(ERROR_SUCCESS);
  DWORD size = GetEnvironmentVariableW(L"TMPDIR", nullptr, 0);
  if (size == 0) {
    DWORD error = GetLastError();
    *provided = error == ERROR_SUCCESS;
    if (error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND) return true;
    std::fprintf(stderr, "dalvikvm: cannot read TMPDIR (Windows error %lu)\n",
                 static_cast<unsigned long>(error));
    return false;
  }
  *provided = true;
  for (int attempt = 0; attempt < 8 && size <= 32768; ++attempt) {
    value->assign(size, L'\0');
    SetLastError(ERROR_SUCCESS);
    DWORD length = GetEnvironmentVariableW(L"TMPDIR", value->data(), size);
    if (length > 0 && length < size) {
      value->resize(length);
      return true;
    }
    if (length == 0) {
      DWORD error = GetLastError();
      if (error == ERROR_SUCCESS) { value->clear(); return true; }
      break;  // A failed or disappearing explicit override is not a default.
    }
    size = length + 1;
  }
  std::fprintf(stderr, "dalvikvm: TMPDIR changed or could not be read\n");
  return false;
}

bool WindowsTemporaryDirectory(std::wstring* value) {
  DWORD size = MAX_PATH + 1;
  for (int attempt = 0; attempt < 8 && size <= 32768; ++attempt) {
    value->assign(size, L'\0');
    DWORD length = GetTempPathW(size, value->data());
    if (length == 0) break;
    if (length >= size) { size = length + 1; continue; }
    value->resize(length);
    // Match the guest path namespace, including UNC paths without a leading
    // double slash that java.io.File and java.nio.file.Path would collapse.
    std::wstring unc;
    if (gesher::unc_paths::FromWindows(*value, &unc)) {
      *value = std::move(unc);
      while (value->back() == L'/') value->pop_back();
      return true;
    }
    for (wchar_t& character : *value) if (character == L'\\') character = L'/';
    if (value->rfind(L"//?/", 0) == 0) value->erase(0, 4);
    if (value->size() < 3 || (*value)[1] != L':' ||
        !(((*value)[0] >= L'A' && (*value)[0] <= L'Z') ||
          ((*value)[0] >= L'a' && (*value)[0] <= L'z')) ||
        (*value)[2] != L'/') {
      std::fprintf(stderr, "dalvikvm: Windows temporary path needs an absolute drive or UNC path; "
                           "set a supported TMPDIR or -Djava.io.tmpdir path\n");
      return false;
    }
    wchar_t drive = (*value)[0];
    if (drive >= L'A' && drive <= L'Z') drive += L'a' - L'A';
    *value = std::wstring(L"/mnt/") + drive + value->substr(2);
    while (value->size() > 7 && value->back() == L'/') value->pop_back();
    // Do not validate permissions/existence here or create a replacement.
    // Android reports actual File/Files I/O errors on use; ActivityThread can
    // first replace an unavailable host default with the app's private cache.
    return true;
  }
  std::fprintf(stderr, "dalvikvm: cannot obtain the Windows temporary directory\n");
  return false;
}

bool InitializeTemporaryDirectory(const std::vector<std::string>& arguments) {
  // Android treats -D properties as immutable. Preserve explicit options, but
  // never inject one: ActivityThread must still select each app's cache later.
  if (HasTemporaryDirectoryProperty(arguments)) return true;
  std::wstring directory;
  bool provided = false;
  if (!ReadTemporaryDirectory(&directory, &provided)) return false;
  if (!provided && !WindowsTemporaryDirectory(&directory)) return false;
  // An explicitly empty Windows environment entry retains its existing CRT
  // representation; _putenv_s(name, "") would instead delete that entry.
  if (provided && directory.empty()) return true;
  std::string utf8 = gesher::exec::Narrow(directory);
  if (gesher::exec::Widen(utf8) != directory) {
    std::fprintf(stderr, "dalvikvm: temporary directory is not valid Unicode\n");
    return false;
  }
  // Libcore and bionic read UTF-8 from the narrow CRT environment; Windows
  // child processes inherit UTF-16. Publish both before ART starts threads.
  int error = _putenv_s("TMPDIR", utf8.c_str());
  if (error != 0 || !SetEnvironmentVariableW(L"TMPDIR", directory.c_str())) {
    std::fprintf(stderr, "dalvikvm: cannot initialize the temporary-directory environment\n");
    return false;
  }
  return true;
}

}  // namespace

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
  if (!InitializeTemporaryDirectory(values)) return EXIT_FAILURE;
  std::vector<char*> argv;
  argv.reserve(values.size() + 1);
  for (auto& value : values) argv.push_back(value.data());
  argv.push_back(nullptr);
  // ART retains its original FastExit behavior; argument storage remains live
  // for the whole call, with no VM/font initialization in this adapter.
  return gesher_dalvikvm_main(argc, argv.data());
}
