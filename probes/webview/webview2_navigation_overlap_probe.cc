// Gesher: bounded diagnostic of actual WebView2 navigation event ordering.
// Three pairs of own loads, a fixed synthetic document, no Java/provider model.
// Native trace supplies the real navigation IDs and latest handed API number.
// Resource callbacks do not supply a per-request navigation/API ID; do not
// interpret their latest-started snapshot as ownership of the request.
// A successful diagnostic means that all cases completed, not that a particular
// overlap was observed or that the Notes/ART failure has been repaired.
#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "webview_host.h"

using gesher::webview::Host;
using gesher::webview::HostOptions;

namespace {
constexpr wchar_t kDocument[] = L"https://gesher-file.example/diagnostic/overlap/";
constexpr char kHtml[] = "<!doctype html><html><head><title>gesher-overlap</title>"
                         "<link rel='icon' href='data:,'></head><body>fixture</body></html>";
std::mutex g_log_mutex;
unsigned g_sequence = 0;

void Log(const char* name, const char* event, int64_t id = 0, int64_t handed = 0,
         int arg = 0, bool fixture = true, const char* value_name = "handed") {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  fprintf(stderr, "OVERLAP case=%s seq=%u tick=%llu event=%s id=%lld %s=%lld arg=%d fixture=%d\n",
          name, ++g_sequence, static_cast<unsigned long long>(GetTickCount64()), event,
          static_cast<long long>(id), value_name, static_cast<long long>(handed), arg, fixture);
  fflush(stderr);
}

bool Wait(DWORD milliseconds, const std::function<bool()>& done) {
  const ULONGLONG end = GetTickCount64() + milliseconds;
  while (!done()) {
    if (GetTickCount64() >= end) return false;
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return true;
}

class Probe final : public gesher::webview::Listener {
 public:
  const char* const name;
  const int64_t first, second, warmup;
  std::mutex host_mutex;
  std::mutex request_mutex;
  std::atomic<Host*> host{nullptr};
  std::atomic<bool> ready{false}, failed{false}, destroyed{false};
  std::atomic<bool> warmup_done{false}, final_done{false}, script_done{false};
  std::atomic<bool> script_matches{false}, hold_first{false}, first_finished{false};
  std::atomic<int64_t> held_request{0}, latest_started{0};
  std::atomic<ULONGLONG> last_navigation_event{0};
  std::atomic<int> starts{0}, requests{0}, finishes{0};

  Probe(const char* label, int64_t base) : name(label), first(base + 1), second(base + 2), warmup(base) {}

  bool Issue(int64_t number) {
    std::lock_guard<std::mutex> lock(host_mutex);
    const bool was_ready = ready.load();
    Log(name, "issue", number, 0, was_ready ? 1 : 0);
    host.load()->Navigate(kDocument, L"", L"GET", {}, number);
    return was_ready;
  }

  void Reply(int64_t request, bool document) {
    std::lock_guard<std::mutex> lock(host_mutex);
    if (Host* value = host.load()) {
      Log(name, "respond-issued", request, latest_started, document ? 200 : 204, true, "latest_started");
      value->RespondToRequest(request, document ? 200 : 204, document ? L"OK" : L"No Content",
          L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n",
          document ? std::vector<uint8_t>(kHtml, kHtml + sizeof(kHtml) - 1) : std::vector<uint8_t>{});
    }
  }

  void ReleaseHeld() {
    int64_t request;
    {
      std::lock_guard<std::mutex> lock(request_mutex);
      hold_first = false;
      request = held_request.exchange(0);
    }
    if (request != 0) {
      Log(name, "release-held", request, latest_started, 0, true, "latest_started");
      Reply(request, true);
    }
  }

  void OnEvent(int32_t type, int64_t id, int32_t arg, const std::wstring& a,
               const std::wstring& b, const std::wstring&, const std::vector<uint8_t>*) override {
    const int64_t handed = b.empty() ? 0 : _wcstoi64(b.c_str(), nullptr, 10);
    const bool fixture = a == kDocument;
    if (type == gesher::webview::kReady) {
      Log(name, "ready");
      ready = true;
    } else if (type == gesher::webview::kFailed || type == gesher::webview::kProcessFailed) {
      Log(name, "failure", id, 0, arg);
      failed = true;
    } else if (type == gesher::webview::kPageStarted) {
      last_navigation_event = GetTickCount64();
      Log(name, "start", id, handed, arg, fixture);
      if (fixture) {
        latest_started = handed;
        if (handed >= first) ++starts;
      }
    } else if (type == gesher::webview::kPageFinished) {
      last_navigation_event = GetTickCount64();
      // id is HTTP status here. The adjacent existing native trace contains
      // the actual NavigationId; matching by handed alone would be ambiguous.
      Log(name, "finish-http", id, handed, arg, fixture);
      if (fixture) {
        if (handed == warmup && arg == -1 && id == 200) warmup_done = true;
        if (handed >= first) ++finishes;
        if (handed == first) first_finished = true;
        // This is an observed handoff watermark, not proof that a completion
        // belongs to the second issue. The native NavigationId trace decides.
        if (handed == second && arg == -1 && id == 200) final_done = true;
      }
    } else if (type == gesher::webview::kScriptResult) {
      script_matches = arg >= 0 && a == L"\"gesher-overlap\"";
      Log(name, "document-check", id, 0, script_matches ? 1 : 0);
      script_done = true;
    }
  }

  bool OnNavigationStarting(const std::wstring& url, bool user, bool redirect, int64_t handed) override {
    Log(name, "client-start", user ? 1 : 0, handed, redirect ? 1 : 0, url == kDocument);
    return false;
  }

  void OnResourceRequest(int64_t id, const std::wstring& url, const std::wstring&,
                         const std::wstring&, int32_t context, bool main_frame) override {
    last_navigation_event = GetTickCount64();
    const bool document = url == kDocument && context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT;
    Log(name, "resource", id, latest_started, context, url == kDocument, "latest_started");
    if (document) {
      if (latest_started >= first) ++requests;
      Log(name, "resource-main", id, latest_started, main_frame ? 1 : 0, true, "latest_started");
      std::lock_guard<std::mutex> lock(request_mutex);
      int64_t empty = 0;
      if (hold_first && held_request.compare_exchange_strong(empty, id)) {
        Log(name, "hold-resource", id, latest_started, 0, true, "latest_started");
        return;  // The real Host already holds the real WebView2 deferral.
      }
    }
    // Never let the synthetic document or incidental resources reach a network.
    Reply(id, document);
  }

  std::wstring OnBridgeCall(const std::wstring&, const std::wstring&, const std::wstring&) override {
    return L"null";
  }
  void OnDestroyed() override {
    Log(name, "destroyed");
    destroyed = true;
  }
};

enum class Mode { BeforeReady, ReadyPair, HeldResource };

bool RunCase(HWND window, const char* name, int64_t base, Mode mode) {
  Log(name, "begin", base + 1, base + 2, 0, true, "second_api");
  auto* probe = new Probe(name, base);
  HostOptions options;
  options.parent = window;
  Host* host = Host::Create(probe, options);
  if (host == nullptr) { delete probe; Log(name, "create-failed"); return false; }
  probe->host = host;
  Host::Settings settings;
  settings.intercept_all = true;
  host->SetSettings(settings);
  host->SetBounds(window, 0, 0, 480, 320, 0, 0, 480, 320, true);
  host->SetDocumentScript(L"window.gesherOverlapDiagnostic = true;");
  bool ok = true;
  if (mode == Mode::BeforeReady) {
    // The first case uses a fresh process/environment. Require the intended
    // precondition instead of silently relabelling already-ready dispatch.
    const bool first_ready = probe->Issue(probe->first);
    const bool second_ready = probe->Issue(probe->second);
    ok = !first_ready && !second_ready;
  } else {
    ok = Wait(30000, [&] { return probe->ready || probe->failed; }) && !probe->failed;
    if (ok) {
      probe->Issue(probe->warmup);
      ok = Wait(15000, [&] { return probe->warmup_done || probe->failed; }) && probe->warmup_done;
    }
    if (ok && mode == Mode::HeldResource) probe->hold_first = true;
    if (ok) probe->Issue(probe->first);
    if (ok && mode == Mode::HeldResource) {
      ok = Wait(15000, [&] { return probe->held_request != 0 || probe->failed; }) && probe->held_request != 0;
    }
    if (ok) probe->Issue(probe->second);
    if (ok && mode == Mode::HeldResource) {
      // Observe without blocking the WebView STA. Release on a second start,
      // the first completion, or two seconds, whichever arrives first.
      const bool event = Wait(2000, [&] { return probe->starts >= 2 || probe->first_finished || probe->failed; });
      Log(name, "hold-observation", probe->starts, 0, event ? 1 : 0);
      probe->ReleaseHeld();
    }
  }
  if (ok) ok = Wait(mode == Mode::BeforeReady ? 60000 : 15000,
                    [&] { return probe->final_done || probe->failed; }) && probe->final_done && !probe->failed;
  if (ok) {
    // Observe a finite quiet interval after every start seen so far completed.
    // This deliberately does not assert an ownership mapping for equal URLs.
    ok = Wait(10000, [&] {
      return probe->failed || (probe->starts > 0 && probe->finishes >= probe->starts &&
                               GetTickCount64() - probe->last_navigation_event >= 500);
    }) && !probe->failed;
    Log(name, "quiet-window-ms", 500, 0, ok ? 1 : 0);
  }
  if (ok) {
    host->ExecuteScript(L"document.title", base + 99);
    ok = Wait(5000, [&] { return probe->script_done || probe->failed; }) && probe->script_matches && !probe->failed;
  }
  probe->ReleaseHeld();
  Log(name, "counts", probe->starts, probe->requests, probe->finishes, true, "requests");
  {
    // Serialize destruction with callback-side RespondToRequest calls. An
    // atomic pointer alone cannot keep Host alive after a concurrent load.
    std::lock_guard<std::mutex> lock(probe->host_mutex);
    probe->host = nullptr;
    host->Destroy();
  }
  if (!Wait(10000, [&] { return probe->destroyed.load(); })) {
    // The listener must outlive callbacks. Do not free it after a failed drain.
    Log(name, "destroy-timeout");
    return false;
  }
  delete probe;
  Log(name, "end", 0, 0, ok ? 1 : 0);
  return ok;
}
}  // namespace

int wmain() {
  _putenv("GESHER_TRACE_WEBVIEW_NAVIGATION=1");
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) { Log("all", "com-failed", 0, 0, initialized); return 1; }
  HRESULT hr = S_OK;
  const std::wstring version = Host::RuntimeVersion(&hr);
  if (version.empty()) { Log("all", "runtime-missing", 0, 0, hr); CoUninitialize(); return 2; }
  char version_utf8[256]{};
  if (WideCharToMultiByte(CP_UTF8, 0, version.c_str(), -1, version_utf8,
                           sizeof(version_utf8), nullptr, nullptr) == 0) {
    CoUninitialize();
    return 1;
  }
  fprintf(stderr, "OVERLAP_RUNTIME version=%s\n", version_utf8);
  wchar_t temporary[MAX_PATH]{};
  const DWORD temporary_length = GetTempPathW(MAX_PATH, temporary);
  if (temporary_length == 0 || temporary_length >= MAX_PATH) { CoUninitialize(); return 1; }
  Host::Configure(std::wstring(temporary) + L"gesher-overlap-" + std::to_wstring(GetCurrentProcessId()) +
                      L"-" + std::to_wstring(GetTickCount64()), L"");
  HWND window = CreateWindowExW(0, L"STATIC", L"Gesher navigation overlap diagnostic",
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 30, 30, 500, 380,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (window == nullptr) { CoUninitialize(); return 1; }
  ShowWindow(window, SW_SHOWNOACTIVATE);
  bool ok = RunCase(window, "before-ready-pair", 100, Mode::BeforeReady);
  if (ok) ok = RunCase(window, "ready-pair", 200, Mode::ReadyPair);
  if (ok) ok = RunCase(window, "held-first-resource", 300, Mode::HeldResource);
  const int shutdown = Host::ShutdownIdle(10000);
  Log("all", "shutdown", shutdown);
  DestroyWindow(window);
  CoUninitialize();
  Log("all", "diagnostic-complete", 0, 0, ok && shutdown == 1 ? 1 : 0);
  // A failed owner-thread shutdown must not race static destruction of logging
  // state. The external runner retains the failure log and process exit code.
  if (shutdown != 1) ExitProcess(1);
  return ok && shutdown == 1 ? 0 : 1;
}
