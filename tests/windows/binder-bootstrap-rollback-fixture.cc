// CI-only actual broker rollback controls. Linked with the production source
// under a compile-time test seam; no test setter exists in production builds.
#include "../../compat/framework/services_zygote.h"
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace z = gesher::zygote;
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
std::u16string U(const wchar_t* value) {
  auto end = value + wcslen(value);
  return {reinterpret_cast<const char16_t*>(value), reinterpret_cast<const char16_t*>(end)};
}
int wmain(int argc, wchar_t** argv) {
  try {
    Check(argc == 4, "broker, directory and fault mode required");
    std::wstring mode(argv[3]);
    int extra = mode == L"wait" ? 2 : mode == L"close" ? 4 : 0;
    Check(mode == L"duplicate" || mode == L"wait" || mode == L"close", "unknown fault mode");
    std::u16string dir = U(argv[2]);
    std::wstring device = L"rollback-" + std::to_wstring(GetCurrentProcessId());
    std::u16string broker = u"\"" + U(argv[1]) + u"\" " + U(device.c_str());
    z::FixtureRollbackFaults(1 | 8 | extra);
    std::vector<int> children;
    std::thread owner([&] {
      children = z::StartDevice({broker, u"unused-manager", u"unused-writer"},
          {dir + u"\\broker.log", dir + u"\\manager.log", dir + u"\\writer.log"},
          {u"GESHER_BINDER=" + U(device.c_str())});
    });
    uint64_t deadline = GetTickCount64() + 4000;
    int pid;
    while (!(pid = z::FixtureRollbackPid()) && GetTickCount64() < deadline) Sleep(1);
    HANDLE observer = reinterpret_cast<HANDLE>(z::FixtureObserveRollback(pid));
    bool observed = pid > 0 && z::FixtureRollbackState(pid) == 1 &&
        observer && z::ExitStatus(pid) == z::kExitUnknown && !z::Kill(pid);
    z::FixtureRollbackFaults(1 | extra);
    owner.join();
    Check(observed, "concurrent observers must not consume or kill reserved rollback");
    Check(WaitForSingleObject(observer, 5000) == WAIT_OBJECT_0, "observed exact broker must terminate");
    DWORD code = 0;
    Check(GetExitCodeProcess(observer, &code) && (code == 137 || code == 1),
          "exact broker terminated by rollback or private-channel loss");
    Check(CloseHandle(observer), "close independent observer");
    Check(children.empty(), "injected duplication must fail bootstrap");
    if (!extra) {
      Check(z::FixtureRollbackState(pid) == 0, "signaled broker must be closed and retired");
    } else {
      Check(z::FixtureRollbackState(pid) == 2, "unconfirmed handle remains quarantined");
      Check(z::ExitStatus(pid) == z::kExitUnknown && !z::Kill(pid), "quarantine rejects public observers");
      deadline = GetTickCount64() + 5000;
      while (!z::FixtureReapRollback(pid)) {
        Check(GetTickCount64() < deadline, "exact quarantined broker did not signal and close");
        Sleep(5);
      }
      Check(z::FixtureRollbackState(pid) == 0, "confirmed retry retired exact handle");
    }
    std::printf("ROLLBACK_FIXTURE mode=%ls duplicate_failed=1 observer_rejected=1 exact_retired=1\n", mode.c_str());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "ROLLBACK_FIXTURE failed: %s\n", error.what());
    return 1;
  }
}
