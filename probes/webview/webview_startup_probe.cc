// Fault-injected startup lifecycle probe. Compiled only into this separate executable.
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <atomic>
#include "webview_host.h"

class StartupListener final : public gesher::webview::Listener {
 public:
  std::atomic<int> events{0};
  std::atomic<int> destroyed{0};
  std::atomic<bool> ready{false};
  std::atomic<bool> session_ok{false};
  std::atomic<gesher::webview::Host*> host{nullptr};
  std::atomic<bool> close_ok{false};
  std::atomic<bool> in_callback{false};
  std::atomic<bool> destroyed_in_callback{false};
  std::atomic<bool> registered{false}, removed{false}, evaluation{false}, bad_event{false};
  std::atomic<bool> event_armed{false}, capacity_released{false}, saturated{false}, busy{false};
  std::atomic<bool> retry_replied{false};
  std::atomic<int> session_events{0};
  void OnEvent(int32_t type, int64_t id, int32_t arg, const std::wstring& result,
               const std::wstring& session, const std::wstring& detail,
               const std::vector<uint8_t>*) override {
    ++events;
    wchar_t fault[32] = {};
    GetEnvironmentVariableW(L"GESHER_WEB4_STARTUP_FAULT", fault, 32);
    if (type == gesher::webview::kReady) ready = true;
    if (type == gesher::webview::kSessionResult && id == 103 && arg == 0) evaluation = true;
    if (type == gesher::webview::kSessionResult && id == 101 && arg == 0) event_armed = true;
    if (type == gesher::webview::kSessionResult && id == 1000 &&
        arg == static_cast<int32_t>(HRESULT_FROM_WIN32(ERROR_TIMEOUT))) capacity_released = true;
    if (type == gesher::webview::kSessionSubscription) {
      if (id == 100 && result == L"registered" && arg == 0) registered = true;
      else if ((id == 102 || (id >= 104 && id <= 113)) && result == L"removed" && arg == 0) {
        removed = true;
        if (id >= 104) retry_replied = true;
      } else if ((id == 102 || (id >= 104 && id <= 113)) && result == L"removed" &&
                 arg == static_cast<int32_t>(HRESULT_FROM_WIN32(ERROR_BUSY))) {
        busy = true;
        if (id >= 104) retry_replied = true;
      }
      else if (id == 100 && result == L"event" && arg == 0) {
        if (!registered || !saturated || !session.empty() ||
            detail.find(L"WEB4_EVENT_TEST") == std::wstring::npos)
          bad_event = true;
        ++session_events;
        if (wcscmp(fault, L"subscription-busy") == 0) {
          auto* current = host.load();
          if (current == nullptr || gesher::webview::Host::RemoveSessionSubscription(
                  current->SessionHandle(), 102, 100, 5000) != 0) {
            bad_event = true;
          } else {
            ULONGLONG until = GetTickCount64() + 3000;
            while (!busy && GetTickCount64() < until) {
              MSG msg;
              while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg); DispatchMessageW(&msg);
                if (busy) break;
              }
              Sleep(1);
            }
            if (!busy) bad_event = true;
          }
        }
      } else { bad_event = true; }
    }
    if (type == gesher::webview::kFailed) {
      fwprintf(stderr, L"STARTUP_FAILED hr=0x%08x detail=%ls\n", static_cast<unsigned>(arg),
               result.c_str());
      fflush(stderr);
    }
    if (type == gesher::webview::kSessionResult && id == 1 && arg == 0 &&
        result.find(L"\"value\":42") != std::wstring::npos) session_ok = true;
    if (type == gesher::webview::kSessionResult && wcscmp(fault, L"session-close") == 0) {
      in_callback = true;
      auto* current = host.exchange(nullptr);
      close_ok = current != nullptr && current->Destroy();
      in_callback = false;
    }
    if (type == gesher::webview::kSessionResult && wcscmp(fault, L"callback-stall") == 0) {
      fputs("WEB4_CALLBACK_BLOCKED\n", stdout); fflush(stdout);
      Sleep(12000);
    }
  }
  bool OnNavigationStarting(const std::wstring&, bool, bool) override { return false; }
  void OnResourceRequest(int64_t, const std::wstring&, const std::wstring&, const std::wstring&,
                         int32_t, bool) override { ++events; }
  std::wstring OnBridgeCall(const std::wstring&, const std::wstring&,
                            const std::wstring&) override { ++events; return L"{}"; }
  void OnDestroyed() override {
    if (in_callback) destroyed_in_callback = true;
    ++destroyed;
  }
};

bool OwnWindow(HWND parent, const wchar_t* window_class) {
  HWND window = nullptr;
  while ((window = FindWindowExW(parent, window, window_class, nullptr)) != nullptr) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId()) return true;
  }
  return false;
}

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) return 2;
  bool watchdog = wcscmp(argv[1], L"watchdog") == 0 || wcscmp(argv[1], L"callback-stall") == 0;
  const wchar_t* fault = wcscmp(argv[1], L"watchdog") == 0 ? L"stall" : argv[1];
  if (!SetEnvironmentVariableW(L"GESHER_WEB4_STARTUP_FAULT", fault)) return 2;
  StartupListener listener;
  ULONGLONG begin = GetTickCount64();
  auto* host = gesher::webview::Host::Create(&listener, {});
  ULONGLONG elapsed = GetTickCount64() - begin;
  if (watchdog) {
    if (host == nullptr) return 10;
    ULONGLONG deadline = GetTickCount64() + 20000;
    while (!listener.ready && GetTickCount64() < deadline) Sleep(20);
    if (!listener.ready) return 11;
    int status = gesher::webview::Host::SubmitSession(host->SessionHandle(), 1, L"",
        L"Runtime.evaluate", L"{\"expression\":\"6*7\",\"returnByValue\":true}",
        wcscmp(argv[1], L"watchdog") == 0 ? 100 : 5000);
    if (status != 0) return 12;
    fputs("WEB4_WATCHDOG_ARMED\n", stdout); fflush(stdout);
    Sleep(15000);
    return 13;  // watchdog failed to terminate the stalled generation
  }
  if (wcscmp(argv[1], L"subscription") == 0 || wcscmp(argv[1], L"subscription-busy") == 0) {
    if (host == nullptr) return 20;
    bool reentrant = wcscmp(argv[1], L"subscription-busy") == 0;
    listener.host = host;
    ULONGLONG deadline = GetTickCount64() + 20000;
    while (!listener.ready && GetTickCount64() < deadline) Sleep(20);
    if (!listener.ready || gesher::webview::Host::SubscribeSession(host->SessionHandle(), 100,
        L"Runtime.consoleAPICalled", 5000) != 0) return 21;
    if (gesher::webview::Host::SubscribeSession(host->SessionHandle(), 100,
        L"Runtime.consoleAPICalled", 5000) != -3) return 27;
    deadline = GetTickCount64() + 10000;
    while (!listener.registered && GetTickCount64() < deadline) Sleep(20);
    if (!listener.registered || gesher::webview::Host::SubmitSession(host->SessionHandle(), 101,
        L"", L"Runtime.evaluate",
        L"{\"expression\":\"(()=>{let t=setInterval(()=>{if(window.__web4Go){clearInterval(t);console.log('WEB4_EVENT_TEST')}},20)})()\"}",
        5000) != 0) return 22;
    deadline = GetTickCount64() + 10000;
    while (!listener.event_armed && GetTickCount64() < deadline) Sleep(20);
    if (!listener.event_armed) return 28;
    if (!reentrant) for (int64_t id = 1000; id < 1128; ++id) {
      uint32_t timeout = id == 1000 ? 15000 : 60000;
      if (gesher::webview::Host::SubmitSession(host->SessionHandle(), id, L"", L"Runtime.evaluate",
          L"{\"expression\":\"new Promise(()=>{})\",\"awaitPromise\":true}", timeout) != 0)
        return 29;
    }
    if (!reentrant && gesher::webview::Host::SubmitSession(host->SessionHandle(), 1128, L"",
        L"Runtime.enable", L"{}", 60000) != -5) return 30;
    listener.saturated = true;
    host->ExecuteScript(L"window.__web4Go=true", 0); // release only after saturation is observed
    deadline = GetTickCount64() + 10000;
    while (listener.session_events == 0 && GetTickCount64() < deadline) Sleep(20);
    if (listener.session_events != 1 || listener.bad_event) return 31;
    deadline = GetTickCount64() + 20000;
    while (!(reentrant ? listener.busy.load() : listener.capacity_released.load()) &&
           GetTickCount64() < deadline) Sleep(20);
    if (!(reentrant ? listener.busy.load() : listener.capacity_released.load())) return 23;
    if (reentrant) {
      for (int64_t op = 104; op <= 113 && !listener.removed; ++op) {
        listener.retry_replied = false;
        if (gesher::webview::Host::RemoveSessionSubscription(host->SessionHandle(), op, 100, 5000) != 0)
          return 23;
        deadline = GetTickCount64() + 5000;
        while (!listener.retry_replied && GetTickCount64() < deadline) Sleep(20);
        if (!listener.retry_replied) return 23;
      }
    } else if (gesher::webview::Host::RemoveSessionSubscription(host->SessionHandle(),
                                                                 102, 100, 5000) != 0) return 23;
    deadline = GetTickCount64() + 10000;
    while (!listener.removed && GetTickCount64() < deadline) Sleep(20);
    if (!listener.removed || gesher::webview::Host::SubmitSession(host->SessionHandle(), 103,
        L"", L"Runtime.evaluate", L"{\"expression\":\"console.log('WEB4_AFTER_REMOVE')\"}",
        5000) != 0) return 24;
    deadline = GetTickCount64() + 10000;
    while (!listener.evaluation && GetTickCount64() < deadline) Sleep(20);
    Sleep(1000); // bounded observation after removal
    if (!listener.evaluation || listener.session_events != 1 || listener.bad_event || !host->Destroy())
      return 25;
    deadline = GetTickCount64() + 10000;
    while (listener.destroyed != 1 && GetTickCount64() < deadline) Sleep(20);
    if (listener.destroyed != 1) return 26;
    wprintf(L"PASS startup fault=%ls registered=1 event=1 saturated=%d busy=%d removed=1 destroyed=1\n",
            argv[1], reentrant ? 0 : 128, reentrant ? 1 : 0);
    return 0;
  }
  if (wcscmp(argv[1], L"session-normal") == 0 || wcscmp(argv[1], L"session-close") == 0) {
    if (host == nullptr) return 14;
    bool reentrant = wcscmp(argv[1], L"session-close") == 0;
    listener.host = host;
    ULONGLONG deadline = GetTickCount64() + 20000;
    while (!listener.ready && GetTickCount64() < deadline) Sleep(20);
    if (!listener.ready || gesher::webview::Host::SubmitSession(host->SessionHandle(), 1, L"",
        L"Runtime.evaluate", L"{\"expression\":\"6*7\",\"returnByValue\":true}", 5000) != 0)
      return 15;
    deadline = GetTickCount64() + 10000;
    while (!listener.session_ok && GetTickCount64() < deadline) Sleep(20);
    if (!listener.session_ok) return 16;
    Sleep(6000); // longer than dispatch watchdog grace; completion must remain nonfatal
    if (reentrant) {
      if (!listener.close_ok || listener.host != nullptr) return 17;
    } else if (!host->Destroy()) return 17;
    deadline = GetTickCount64() + 10000;
    while (listener.destroyed != 1 && GetTickCount64() < deadline) Sleep(20);
    if (listener.destroyed != 1 || listener.destroyed_in_callback) return 18;
    wprintf(L"PASS startup fault=%ls result=42 destroyed=1\n", argv[1]);
    return 0;
  }
  if (wcscmp(argv[1], L"normal") == 0) {
    if (host == nullptr) return 7;
    ULONGLONG deadline = GetTickCount64() + 20000;
    while (!listener.ready && GetTickCount64() < deadline) Sleep(20);
    if (!listener.ready) {
      fwprintf(stderr, L"STARTUP_READY_TIMEOUT events=%d destroyed=%d\n",
               listener.events.load(), listener.destroyed.load());
      fflush(stderr);
      return 8;
    }
    if (!host->Destroy()) {
      fwprintf(stderr, L"STARTUP_DESTROY_ENQUEUE_FAILED\n"); fflush(stderr); return 8;
    }
    deadline = GetTickCount64() + 10000;
    while (listener.destroyed != 1 && GetTickCount64() < deadline) Sleep(20);
    if (listener.destroyed != 1) return 9;
    wprintf(L"PASS startup fault=normal ready=1 destroyed=1\n");
    return 0;
  }
  if (host != nullptr) { host->Destroy(); return 3; }
  if (elapsed > 15000 || listener.destroyed != 1 || listener.events != 0) return 4;
  Sleep(3000); // observe late startup after the 10-second timeout
  host = gesher::webview::Host::Create(&listener, {});
  if (host != nullptr) { host->Destroy(); return 5; }
  if (listener.destroyed != 2 || listener.events != 0 ||
      OwnWindow(HWND_MESSAGE, L"GesherWebViewTasks") ||
      OwnWindow(nullptr, L"GesherWebViewParking")) return 6;
  wprintf(L"PASS startup fault=%ls elapsed=%llu destroyed=2 events=0\n", argv[1], elapsed);
  return 0;
}
