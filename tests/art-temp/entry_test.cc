// Exercise the actual production wmain and option/path/bootstrap policy, with
// explicit Win32/CRT boundary replies. This is not Windows execution evidence.
#include <codecvt>
#include <cstdio>
#include <cwchar>
#include <locale>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "exec_spawn.h"
#include "../../compat/art/dalvikvm_main.cc"

namespace {
int checks = 0;
DWORD last_error = 0;
std::map<std::wstring, std::wstring> wide_environment;
std::map<std::string, std::string> narrow_environment;
std::wstring temporary_path;
std::wstring attribute_path;
DWORD attributes;
bool environment_failure;
bool temp_failure;
bool publish_failure;
int crt_failure;
int temp_reads;
int attribute_reads;
int crt_writes;
int vm_calls;
std::vector<std::string> vm_arguments;

void Require(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

void Reset() {
  last_error = 0;
  wide_environment.clear();
  narrow_environment.clear();
  temporary_path = L"C:\\Users\\runner\\AppData\\Local\\Temp\\";
  attribute_path.clear();
  attributes = FILE_ATTRIBUTE_DIRECTORY;
  environment_failure = temp_failure = publish_failure = false;
  crt_failure = temp_reads = attribute_reads = crt_writes = vm_calls = 0;
  vm_arguments.clear();
}

int Run(std::vector<std::wstring> arguments = {L"dalvikvm.exe", L"Main"}) {
  std::vector<wchar_t*> pointers;
  for (auto& value : arguments) pointers.push_back(value.data());
  return wmain(static_cast<int>(pointers.size()), pointers.data());
}

void Default(const std::wstring& windows, const std::string& guest) {
  Reset();
  temporary_path = windows;
  Require(Run() == 77 && vm_calls == 1, "Default must enter the real ART entry");
  Require(narrow_environment["TMPDIR"] == guest,
          "Default TMPDIR must name the Windows temp directory on its actual drive");
  Require(wide_environment[L"TMPDIR"] == gesher::exec::Widen(guest),
          "Windows environment must retain the same Unicode directory");
  Require(attribute_reads == 0,
          "Bootstrap must preserve missing/denied default paths for real I/O and app-cache setup");
  Require(vm_arguments == std::vector<std::string>({"dalvikvm.exe", "Main"}),
          "Initialization must not inject an immutable Java property or alter argv");
}
}  // namespace

void SetLastError(DWORD error) { last_error = error; }
DWORD GetLastError() { return last_error; }
DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t* output, DWORD size) {
  if (environment_failure) { last_error = ERROR_ACCESS_DENIED; return 0; }
  auto found = wide_environment.find(name);
  if (found == wide_environment.end()) { last_error = ERROR_ENVVAR_NOT_FOUND; return 0; }
  last_error = ERROR_SUCCESS;
  const auto& value = found->second;
  if (value.empty()) return 0;
  DWORD needed = static_cast<DWORD>(value.size() + 1);
  if (size < needed) return needed;
  std::wmemcpy(output, value.c_str(), needed);
  return needed - 1;
}
DWORD GetTempPathW(DWORD size, wchar_t* output) {
  ++temp_reads;
  if (temp_failure) { last_error = ERROR_ACCESS_DENIED; return 0; }
  DWORD needed = static_cast<DWORD>(temporary_path.size() + 1);
  if (size < needed) return needed;
  std::wmemcpy(output, temporary_path.c_str(), needed);
  return needed - 1;
}
DWORD GetFileAttributesW(const wchar_t* path) {
  ++attribute_reads;
  attribute_path = path;
  return attributes;
}
int SetEnvironmentVariableW(const wchar_t* name, const wchar_t* value) {
  if (publish_failure) return 0;
  wide_environment[name] = value;
  return 1;
}
int _putenv_s(const char* name, const char* value) {
  ++crt_writes;
  if (crt_failure) return crt_failure;
  narrow_environment[name] = value;
  // Model the CRT's active-code-page conversion independently of UTF-8.
  // The production entry must subsequently publish the real UTF-16 value.
  wide_environment[L"TMPDIR"] = L"incorrect active-code-page bytes";
  return 0;
}
namespace gesher::exec {
std::wstring Widen(const std::string& value) {
  return std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(value);
}
std::string Narrow(const std::wstring& value) {
  return std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(value);
}
}
int gesher_dalvikvm_main(int argc, char** argv) {
  ++vm_calls;
  for (int i = 0; i < argc; ++i) vm_arguments.emplace_back(argv[i]);
  Require(argv[argc] == nullptr, "ART argv must retain its null sentinel");
  return 77;
}

int main() {
  try {
    Default(L"C:\\Users\\runner\\AppData\\Local\\Temp\\", "/mnt/c/Users/runner/AppData/Local/Temp");
    Default(L"D:/runner/temp/", "/mnt/d/runner/temp");
    Default(L"Z:\\space directory\\קבצים 😀\\", "/mnt/z/space directory/קבצים 😀");
    Default(L"C:\\", "/mnt/c/");
    Default(L"\\\\?\\D:\\long\\temp\\", "/mnt/d/long/temp");
    Default(L"\\\\Server\\Share\\Temp קבצים 😀\\", "/mnt/unc/Server/Share/Temp קבצים 😀");
    Default(L"\\\\?\\UNC\\Server\\Share\\temp\\", "/mnt/unc/Server/Share/temp");
    Default(L"\\\\?\\unc\\Server\\Share\\temp\\", "/mnt/unc/Server/Share/temp");
    Default(L"\\\\Server\\Share\\", "/mnt/unc/Server/Share");
    const std::wstring long_path = L"E:\\" + std::wstring(300, L'a') + L"\\";
    Default(long_path, "/mnt/e/" + std::string(300, 'a'));
    Require(temp_reads == 2, "Long default must retry with the required buffer size");

    for (DWORD failure : {INVALID_FILE_ATTRIBUTES, DWORD{0}}) {
      Reset(); attributes = failure;
      Require(Run() == 77 && vm_calls == 1 && attribute_reads == 0,
              "Unusable default must not prevent an app selecting its own valid cache");
    }
    Reset(); temp_failure = true;
    Require(Run() == EXIT_FAILURE && vm_calls == 0, "Windows temp lookup failure must remain a failure");
    for (const auto& path : {L"\\\\server", L"\\\\server\\", L"\\\\server\\..\\temp",
                             L"\\\\.\\C:\\device", L"\\\\?\\GLOBALROOT\\Device\\Volume",
                             L"\\\\server\\bad:share\\temp", L"C:relative",
                             L"\\rooted-without-drive", L"relative"}) {
      Reset(); temporary_path = path;
      Require(Run() == EXIT_FAILURE && vm_calls == 0 && crt_writes == 0,
              "Unrepresentable default must not silently redirect to the current drive");
    }
    for (const auto& value : {L"/mnt/z/explicit קבצים 😀", L"/tmp/intentionally-missing",
                              L"relative directory", L"C:\\explicit-windows-value"}) {
      Reset(); wide_environment[L"TMPDIR"] = value; attributes = INVALID_FILE_ATTRIBUTES;
      Require(Run() == 77 && temp_reads == 0 && attribute_reads == 0,
              "Explicit TMPDIR must not be validated or replaced by host fallback");
      Require(narrow_environment["TMPDIR"] == gesher::exec::Narrow(value) &&
                  wide_environment[L"TMPDIR"] == value, "Explicit TMPDIR text must be preserved");
    }
    Reset(); wide_environment[L"TMPDIR"] = L""; narrow_environment["TMPDIR"] = "";
    Require(Run() == 77 && temp_reads == 0 && crt_writes == 0,
            "An explicitly empty environment value must not be deleted by the CRT writer");
    Reset(); environment_failure = true;
    Require(Run() == EXIT_FAILURE && vm_calls == 0 && temp_reads == 0,
            "Environment lookup error must not be treated as an absent override");

    for (const auto& option : {L"-Djava.io.tmpdir=/explicit", L"-Djava.io.tmpdir="}) {
      Reset(); temp_failure = true;
      Require(Run({L"dalvikvm.exe", L"-cp", L"classes.dex", option, L"Main"}) == 77 &&
                  temp_reads == 0 && crt_writes == 0, "Explicit VM property must win before host lookup");
    }
    Reset();
    Require(Run({L"dalvikvm.exe", L"Main", L"-Djava.io.tmpdir=/app-argument"}) == 77 && temp_reads == 1,
            "Application arguments must not be mistaken for VM properties");
    Reset();
    Require(Run({L"dalvikvm.exe", L"-classpath", L"-Djava.io.tmpdir=/cp-operand", L"Main"}) == 77 && temp_reads == 1,
            "A classpath operand must not be mistaken for a VM property");
    Reset(); crt_failure = 12;
    Require(Run() == EXIT_FAILURE && vm_calls == 0, "CRT publication failure must not start ART");
    Reset(); publish_failure = true;
    Require(Run() == EXIT_FAILURE && vm_calls == 0, "Wide publication failure must not start ART");
    std::printf("TempDirectory entry host tests passed: %d checks; Windows APIs modeled\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "TempDirectory entry host test failed: %s\n", error.what());
    return 1;
  }
}
