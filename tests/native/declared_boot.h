// Test-only: run a test as an app process of a boot that declared an OpenGL ES
// version (ro.opengles.version, set by the system server's probe;
// docs/system-features.md, compat/bionic/egl_gles_version.h). With
// `--declared <version>` the test runs again as a child process, in a property
// session and data directory of its own under %TEMP%, where it sets the
// property before anything reads it; the parent waits for it, returns its exit
// code and removes the directory (the child's property area stays mapped until
// the child exits, so only the parent can remove it).
#pragma once

#include <windows.h>

#include <gesher/system_properties.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace gesher::test {

// The declared version of `--declared <version>`, 0 without it.
inline int DeclaredArgument(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--declared") == 0) return static_cast<int>(std::strtol(argv[i + 1], nullptr, 0));
  }
  return 0;
}

// With --declared, in the parent: runs the child and returns its exit code
// (the caller returns it). In the child, or without --declared: -1, and in the
// child the property is set (*declared is the version; 0 without --declared).
inline int RunAsDeclaredBoot(int argc, char** argv, const char* name, int* declared) {
  *declared = DeclaredArgument(argc, argv);
  if (*declared == 0) return -1;
  const std::string value = std::to_string(*declared);
  if (std::getenv("GESHER_TEST_DECLARED_CHILD") != nullptr) {
    std::string read;
    const bool ok = gesher::properties::Set("ro.opengles.version", value) &&
                    gesher::properties::Get("ro.opengles.version", &read) && read == value;
    std::printf("this process's boot declared ro.opengles.version=%s: %s\n", value.c_str(),
                ok ? "set" : "FAILED to set");
    std::fflush(stdout);
    return ok ? -1 : 1;
  }
  char temp[MAX_PATH];
  GetTempPathA(sizeof temp, temp);
  const std::string session = std::string(name) + "-" + std::to_string(GetCurrentProcessId());
  const std::string data = std::string(temp) + session;
  CreateDirectoryA(data.c_str(), nullptr);
  SetEnvironmentVariableA("GESHER_BINDER", nullptr);
  SetEnvironmentVariableA("GESHER_PROPERTY_SESSION", session.c_str());
  SetEnvironmentVariableA("ANDROID_DATA", data.c_str());
  SetEnvironmentVariableA("GESHER_TEST_DECLARED_CHILD", "1");
  STARTUPINFOW si = {};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi = {};
  std::wstring command = GetCommandLineW();
  DWORD code = 1;
  if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  } else {
    std::printf("FAILED: the --declared child did not start (error %lu)\n", GetLastError());
  }
  std::error_code ec;
  std::filesystem::remove_all(data, ec);
  std::printf("%s: the --declared run's data directory %s\n", name,
              ec || std::filesystem::exists(data) ? "was NOT removed" : "removed");
  std::fflush(stdout);
  return ec ? 1 : static_cast<int>(code);
}

}  // namespace gesher::test
