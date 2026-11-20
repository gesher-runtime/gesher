#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <windows.h>

#include <string>

static void MetadataXattrProcesses(const char* root);

#include "filesystem_metadata_contract_test.inc"

namespace {

constexpr char kRaceFile[] = "xattr-process-race";
constexpr unsigned kWrites = 40;

std::string ChildKey(unsigned child, unsigned index) {
  return "user.child-" + std::to_string(child) + "-" + std::to_string(index);
}

int XattrWorker(unsigned child) {
  int result = setxattr(kRaceFile, "user.contended", &child, sizeof(child), XATTR_CREATE);
  if (result != 0 && errno != EEXIST) return 3;
  for (unsigned i = 0; i < kWrites; ++i) {
    if (setxattr(kRaceFile, ChildKey(child, i).c_str(), &i, sizeof(i), XATTR_CREATE) != 0) return 4;
  }
  return result == 0 ? 0 : 1;
}

DWORD WaitChild(HANDLE process) {
  if (!process) return 0xff;
  const DWORD waited = WaitForSingleObject(process, 10000);
  DWORD result = 0xff;
  if (waited == WAIT_OBJECT_0) {
    if (!GetExitCodeProcess(process, &result)) result = 0xff;
  } else {
    TerminateProcess(process, result);
    WaitForSingleObject(process, 5000);
  }
  CloseHandle(process);
  return result;
}

HANDLE StartXattrWorker(const wchar_t* executable, const wchar_t* cwd, unsigned child) {
  std::wstring line = L"\"" + std::wstring(executable) + L"\" --xattr-worker " + std::to_wstring(child);
  STARTUPINFOW start{};
  start.cb = sizeof(start);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable, line.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, cwd, &start, &process)) return nullptr;
  CloseHandle(process.hThread);
  return process.hProcess;
}

}  // namespace

static void MetadataXattrProcesses(const char* root) {
  char file[4096];
  MetadataJoin(file, sizeof(file), root, kRaceFile);
  MetadataCheck(MetadataWrite(file, "persistent"), "create cross-process xattr fixture");
  wchar_t executable[4096], cwd[4096];
  const DWORD executable_size = GetModuleFileNameW(nullptr, executable, 4096);
  const int cwd_size = MultiByteToWideChar(CP_UTF8, 0, root, -1, cwd, 4096);
  MetadataCheck(executable_size > 0 && executable_size < 4096 && cwd_size > 0,
                "resolve the xattr process fixture");
  if (executable_size > 0 && executable_size < 4096 && cwd_size > 0) {
    // Both writers share an on-disk inode and race the same XATTR_CREATE.
    HANDLE first = StartXattrWorker(executable, cwd, 1);
    HANDLE second = StartXattrWorker(executable, cwd, 2);
    const DWORD first_code = WaitChild(first);
    const DWORD second_code = WaitChild(second);
    MetadataCheck((first_code == 0 && second_code == 1) || (first_code == 1 && second_code == 0),
                  "exactly one Windows process creates the contended attribute");
    unsigned winner = 0;
    MetadataCheck(getxattr(file, "user.contended", &winner, sizeof(winner)) == static_cast<ssize_t>(sizeof(winner)) &&
                  winner == (first_code == 0 ? 1u : 2u),
                  "published xattr survives the creating process's exit");
    for (unsigned child = 1; child <= 2; ++child) {
      for (unsigned i = 0; i < kWrites; ++i) {
        unsigned value = ~0u;
        MetadataCheck(getxattr(file, ChildKey(child, i).c_str(), &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value)) &&
                      value == i, "cross-process updates preserve every other live attribute");
      }
    }
  }
  unlink(file);
}

int main(int argc, char** argv) {
  if (argc == 3 && strcmp(argv[1], "--xattr-worker") == 0)
    return XattrWorker(static_cast<unsigned>(strtoul(argv[2], nullptr, 10)));
  return filesystem_metadata_contracts() ? 1 : 0;
}
