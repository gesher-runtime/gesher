// Gesher: bounded diagnostic of actual WebView2 navigation event ordering.
// Own-load pairs, same-document history, and bounded Stop/history interruption
// observations. Fixed synthetic documents only; no Java/provider model.
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
constexpr wchar_t kOldDocument[] = L"https://gesher-file.example/diagnostic/before/";
constexpr wchar_t kNewDocument[] = L"https://gesher-file.example/diagnostic/after/";
constexpr char kHtml[] = "<!doctype html><html><head><title>gesher-overlap</title>"
                         "<link rel='icon' href='data:,'></head><body>fixture</body></html>";
std::mutex g_log_mutex;
unsigned g_sequence = 0;

void Log(const char* name, const char* event, int64_t id = 0, int64_t handed = 0,
         int arg = 0, bool fixture = true, const char* value_name = "handed", int fixture_id = -1) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  fprintf(stderr, "OVERLAP case=%s seq=%u tick=%llu event=%s id=%lld %s=%lld arg=%d fixture=%d fixture_id=%d\n",
          name, ++g_sequence, static_cast<unsigned long long>(GetTickCount64()), event,
          static_cast<long long>(id), value_name, static_cast<long long>(handed), arg, fixture, fixture_id);
  fflush(stderr);
}

int FixtureId(const std::wstring& url) {
  if (url == kDocument) return 0;
  if (url == std::wstring(kDocument) + L"#one") return 1;
  if (url == std::wstring(kDocument) + L"#control") return 2;
  if (url == kOldDocument) return 3;
  if (url == kNewDocument) return 4;
  return -1;
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
  std::atomic<bool> can_go_back{false};
  std::atomic<int64_t> held_request{0}, latest_started{0};
  std::atomic<int64_t> completed_number{-1};
  std::atomic<int64_t> expected_script{0};
  std::atomic<ULONGLONG> last_navigation_event{0};
  std::atomic<int> starts{0}, requests{0}, finishes{0};
  std::atomic<int> all_starts{0}, all_finishes{0}, source_changes{0}, history_changes{0};
  std::atomic<int> source_fixture{-1};

  Probe(const char* label, int64_t base) : name(label), first(base + 1), second(base + 2), warmup(base) {}

  bool Issue(int64_t number, const std::wstring& url = kDocument) {
    std::lock_guard<std::mutex> lock(host_mutex);
    const bool was_ready = ready.load();
    Log(name, "issue", number, 0, was_ready ? 1 : 0, true, "handed", FixtureId(url));
    host.load()->Navigate(url, L"", L"GET", {}, number);
    return was_ready;
  }

  void IssueCommand(Host::Command command, const char* event) {
    std::lock_guard<std::mutex> lock(host_mutex);
    // This records enqueue only. The diagnostic-only native Stop trace records
    // the actual call/return; these caller-side counts are not a Stop barrier.
    Log(name, event, command, all_starts, 0, true, "starts_seen_at_enqueue");
    host.load()->Run(command);
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
    const int fixture_id = FixtureId(a);
    const bool fixture = fixture_id >= 0;
    if (type == gesher::webview::kReady) {
      Log(name, "ready");
      ready = true;
    } else if (type == gesher::webview::kFailed || type == gesher::webview::kProcessFailed) {
      Log(name, "failure", id, 0, arg);
      failed = true;
    } else if (type == gesher::webview::kPageStarted) {
      ++all_starts;
      last_navigation_event = GetTickCount64();
      Log(name, "start", id, handed, arg, fixture, "handed", fixture_id);
      if (fixture) {
        latest_started = handed;
        if (handed >= first) ++starts;
      }
    } else if (type == gesher::webview::kPageFinished) {
      ++all_finishes;
      last_navigation_event = GetTickCount64();
      // id is HTTP status here. The adjacent existing native trace contains
      // the actual NavigationId; matching by handed alone would be ambiguous.
      // The finish URL is Host's current-source snapshot, not a request owner.
      Log(name, "finish-http", id, handed, arg, fixture, "handed", fixture_id);
      if (fixture) {
        if (arg == -1 && id == 200) completed_number = handed;
        if (handed == warmup && arg == -1 && id == 200) warmup_done = true;
        if (handed >= first) ++finishes;
        if (handed == first) first_finished = true;
        // This is an observed handoff watermark, not proof that a completion
        // belongs to the second issue. The native NavigationId trace decides.
        if (handed == second && arg == -1 && id == 200) final_done = true;
      }
    } else if (type == gesher::webview::kSourceChanged) {
      last_navigation_event = GetTickCount64();
      const int known = fixture_id;
      source_fixture = known;
      ++source_changes;
      Log(name, "source", known, id, arg, known >= 0, "handed_snapshot");
    } else if (type == gesher::webview::kHistory) {
      last_navigation_event = GetTickCount64();
      can_go_back = (arg & 1) != 0;
      ++history_changes;
      Log(name, "history", history_changes, 0, arg);
    } else if (type == gesher::webview::kScriptResult) {
      if (id != expected_script) {
        Log(name, "script-callback", id, 0, arg);
        return;
      }
      script_matches = arg >= 0 && a == (id >= 900 ? L"true" : L"\"gesher-overlap\"");
      Log(name, "document-check", id, 0, script_matches ? 1 : 0);
      script_done = true;
    }
  }

  bool OnNavigationStarting(const std::wstring& url, bool user, bool redirect, int64_t handed) override {
    Log(name, "client-start", user ? 1 : 0, handed, redirect ? 1 : 0,
        FixtureId(url) >= 0, "handed", FixtureId(url));
    return false;
  }

  void OnResourceRequest(int64_t id, const std::wstring& url, const std::wstring&,
                         const std::wstring&, int32_t context, bool main_frame) override {
    last_navigation_event = GetTickCount64();
    const int fixture_id = FixtureId(url);
    const bool document = fixture_id >= 0 && context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT;
    Log(name, "resource", id, latest_started, context, fixture_id >= 0, "latest_started", fixture_id);
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

enum class Mode { BeforeReady, ReadyPair, HeldResource, SameDocumentHistory,
                  StopDistinct, StopSame, BackThenNavigate };

// Observe public callbacks around real history commands. A matching fixture
// URL is a diagnostic endpoint check, never a proposed production ownership
// rule. In particular, log page pushState separately: it may produce the same
// SourceChanged/HistoryChanged event types as an application's history command.
bool ObserveHistory(Probe* probe, Host* host) {
  auto step = [&](const char* label, int expected, const std::function<void()>& issue) {
    const int starts = probe->all_starts, finishes = probe->all_finishes;
    const int sources = probe->source_changes, histories = probe->history_changes;
    Log(probe->name, label, expected);
    issue();
    bool ok = Wait(5000, [&] { return probe->failed || probe->source_fixture == expected; })
        && !probe->failed && probe->source_fixture == expected;
    if (ok) ok = Wait(2000, [&] {
      return probe->failed || GetTickCount64() - probe->last_navigation_event >= 500;
    }) && !probe->failed;
    probe->script_done = false;
    probe->script_matches = false;
    probe->expected_script = 900;
    const std::wstring suffix = expected == 0 ? L"" : expected == 1 ? L"#one" : L"#control";
    if (ok) {
      host->ExecuteScript(L"location.href === '" + std::wstring(kDocument) + suffix + L"'", 900);
      ok = Wait(5000, [&] { return probe->script_done || probe->failed; })
          && probe->script_matches && !probe->failed;
    }
    Log(probe->name, "history-step-navigation-delta", probe->all_starts - starts,
        probe->all_finishes - finishes, ok ? 1 : 0, true, "finishes");
    Log(probe->name, "history-step-source-delta", probe->source_changes - sources,
        probe->history_changes - histories, expected, true, "history_events");
    return ok;
  };
  if (!step("issue-fragment", 1, [&] { probe->Issue(probe->first, std::wstring(kDocument) + L"#one"); }))
    return false;
  if (!step("issue-back", 0, [&] { host->Run(Host::kGoBack); })) return false;
  if (!step("issue-forward", 1, [&] { host->Run(Host::kGoForward); })) return false;
  if (!step("issue-page-push-state", 2, [&] {
    host->ExecuteScript(L"history.pushState(null, '', '#control'); true", 901);
  })) return false;
  return step("issue-back-from-push-state", 1, [&] { host->Run(Host::kGoBack); });
}

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
  const bool interruption = mode == Mode::StopDistinct || mode == Mode::StopSame
      || mode == Mode::BackThenNavigate;
  const wchar_t* final_url = mode == Mode::StopSame ? kDocument : kNewDocument;
  bool ok = true;
  if (mode == Mode::BeforeReady) {
    // The first case uses a fresh process/environment. Require the intended
    // precondition instead of silently relabelling already-ready dispatch.
    const bool first_ready = probe->Issue(probe->first);
    const bool second_ready = probe->Issue(probe->second);
    ok = !first_ready && !second_ready;
  } else {
    ok = Wait(30000, [&] { return probe->ready || probe->failed; }) && !probe->failed;
    if (ok && mode == Mode::BackThenNavigate) {
      // Establish two real cross-document history entries before the batch.
      probe->Issue(probe->warmup - 1);
      ok = Wait(15000, [&] { return probe->completed_number == probe->warmup - 1 || probe->failed; })
          && probe->completed_number == probe->warmup - 1 && !probe->failed;
    }
    if (ok) {
      probe->Issue(probe->warmup, mode == Mode::BackThenNavigate ? kOldDocument : kDocument);
      ok = Wait(15000, [&] { return probe->warmup_done || probe->failed; }) && probe->warmup_done;
    }
    if (ok && interruption) {
      ok = Wait(2000, [&] { return GetTickCount64() - probe->last_navigation_event >= 500 || probe->failed; })
          && !probe->failed;
      if (ok && mode == Mode::BackThenNavigate) {
        ok = probe->can_go_back;
        Log(name, "back-precondition", 0, 0, ok ? 1 : 0);
      }
      if (ok) {
        Log(name, "batch-begin", probe->all_starts, probe->all_finishes, 0, true, "finishes_seen");
        if (mode == Mode::BackThenNavigate) {
          probe->IssueCommand(Host::kGoBack, "issue-back-enqueued");
        } else {
          probe->Issue(probe->first, mode == Mode::StopDistinct ? kOldDocument : kDocument);
          probe->IssueCommand(Host::kStop, "issue-stop-enqueued");
        }
        probe->Issue(probe->second, final_url);
        Log(name, "batch-enqueued", probe->all_starts, probe->all_finishes, 0, true, "finishes_seen");
      }
    }
    if (ok && mode == Mode::SameDocumentHistory) ok = ObserveHistory(probe, host);
    if (ok && mode == Mode::HeldResource) probe->hold_first = true;
    if (ok && mode != Mode::SameDocumentHistory && !interruption) probe->Issue(probe->first);
    if (ok && mode == Mode::HeldResource) {
      ok = Wait(15000, [&] { return probe->held_request != 0 || probe->failed; }) && probe->held_request != 0;
    }
    if (ok && mode != Mode::SameDocumentHistory && !interruption) probe->Issue(probe->second);
    if (ok && mode == Mode::HeldResource) {
      // Observe without blocking the WebView STA. Release on a second start,
      // the first completion, or two seconds, whichever arrives first.
      const bool event = Wait(2000, [&] { return probe->starts >= 2 || probe->first_finished || probe->failed; });
      Log(name, "hold-observation", probe->starts, 0, event ? 1 : 0);
      probe->ReleaseHeld();
    }
  }
  if (ok && mode != Mode::SameDocumentHistory) ok = Wait(mode == Mode::BeforeReady ? 60000 : 15000,
                    [&] { return probe->final_done || probe->failed; }) && probe->final_done && !probe->failed;
  if (ok && mode != Mode::SameDocumentHistory) {
    // Observe a finite quiet interval after every start seen so far completed.
    // This deliberately does not assert an ownership mapping for equal URLs.
    ok = Wait(10000, [&] {
      // Stop need not produce a completion for every observed start. Record it
      // without treating a finite quiet interval as callback-queue drainage.
      return probe->failed || (probe->starts > 0 && (interruption || probe->finishes >= probe->starts) &&
                               GetTickCount64() - probe->last_navigation_event >= 500);
    }) && !probe->failed;
    Log(name, "quiet-window-ms", 500, 0, ok ? 1 : 0);
  }
  if (ok) {
    probe->script_done = false;
    probe->script_matches = false;
    probe->expected_script = interruption ? base + 1000 : base + 99;
    const std::wstring script = interruption
        ? L"location.href === '" + std::wstring(final_url) + L"' && document.title === 'gesher-overlap'"
        : L"document.title";
    host->ExecuteScript(script, probe->expected_script);
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

int wmain(int argc, wchar_t** argv) {
  const bool history_only = argc == 2 && std::wstring(argv[1]) == L"--history-only";
  const bool interruption_only = argc == 2 && std::wstring(argv[1]) == L"--interruptions-only";
  if (argc != 1 && !history_only && !interruption_only) return 2;
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
  bool ok = true;
  if (!history_only && !interruption_only) ok = RunCase(window, "before-ready-pair", 100, Mode::BeforeReady);
  if (ok && !history_only && !interruption_only) ok = RunCase(window, "ready-pair", 200, Mode::ReadyPair);
  if (ok && !history_only && !interruption_only) ok = RunCase(window, "held-first-resource", 300, Mode::HeldResource);
  if (ok && !interruption_only) ok = RunCase(window, "same-document-history", 400, Mode::SameDocumentHistory);
  if (ok && interruption_only) ok = RunCase(window, "ready-stop-distinct", 500, Mode::StopDistinct);
  if (ok && interruption_only) ok = RunCase(window, "ready-stop-same", 600, Mode::StopSame);
  if (ok && interruption_only) ok = RunCase(window, "ready-back-navigate", 700, Mode::BackThenNavigate);
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
