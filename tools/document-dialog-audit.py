#!/usr/bin/env python3
"""Diagnostic branch only: retain production dialog bodies and add trace output."""
from pathlib import Path
import hashlib
import json

root = Path(__file__).resolve().parents[1]
source = root / "compat/devsvc/devsvc_win32.cc"
text = source.read_text(encoding="utf-8")
helpers = text[text.index("const wchar_t* W("):text.index("std::string Utf8(")]
body = text[text.index("bool IsLocalDocument("):text.index("}  // namespace devsvc")]
assert body.count("if (!PostMessageW(window, WM_CLOSE, 0, 0)) {") == 1
body = body.replace(
    "if (!PostMessageW(window, WM_CLOSE, 0, 0)) {",
    'BOOL posted = PostMessageW(window, WM_CLOSE, 0, 0);\n'
    '    std::printf("DOC_CANCEL posted=%u tid=%lu hwnd=%p timer=%llu\\n", '
    'static_cast<unsigned>(posted), GetCurrentThreadId(), static_cast<void*>(window), '
    'static_cast<unsigned long long>(id)); std::fflush(stdout);\n'
    '    if (!posted) {')
body = body.replace(
    'if (FAILED(status)) return status;\n  if (!state->window',
    'if (FAILED(status)) return status;\n'
    '  audit_window = state->window;\n'
    '  audit_thread = GetWindowThreadProcessId(audit_window, &audit_process);\n'
    '  if (!state->window')
body = body.replace('status = dialog->Show(owner.window);', 'status = dialog->Show(owner.window); audit_show_status = status;')
old_constructor = 'multiple_(multiple), custom_(custom), lifetime_(lifetime) {}'
assert body.count(old_constructor) == 1
body = body.replace(old_constructor,
    'multiple_(multiple), custom_(custom), lifetime_(lifetime) { ++audit_constructed; }\n'
    '  ~DocumentEvents() { ++audit_destroyed; }')
header = r'''#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <atomic>
#include <cstdio>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include "compat/devsvc/devsvc_win32.h"
namespace gesher { namespace devsvc {
thread_local HWND audit_window = nullptr;
thread_local DWORD audit_thread = 0, audit_process = 0;
thread_local unsigned audit_constructed = 0, audit_destroyed = 0;
thread_local HRESULT audit_show_status = E_UNEXPECTED;
namespace {
'''
footer = r'''
} }
struct Lifetime {
  std::atomic<unsigned> polls{0};
  std::atomic<unsigned> retired{0};
  ULONGLONG pumping = 0;
};
bool Active(void* context) {
  auto* state = static_cast<Lifetime*>(context);
  unsigned count = ++state->polls;
  if (count >= 3 && state->pumping == 0) state->pumping = GetTickCount64();
  bool active = state->pumping == 0 || GetTickCount64() - state->pumping < 800;
  if (!active) ++state->retired;
  std::printf("DOC_ACTIVE poll=%u active=%u elapsed=%llu tid=%lu\n", count,
      static_cast<unsigned>(active), static_cast<unsigned long long>(state->pumping
          ? GetTickCount64() - state->pumping : 0), GetCurrentThreadId());
  std::fflush(stdout);
  return active;
}
int main() {
  for (int index = 0; index != 9; ++index) {
    int mode = index % 3;
    bool local = index < 6;
    bool multiple = mode == 0 && index >= 3;
    Lifetime lifetime;
    std::atomic<bool> done{false};
    std::atomic<bool> retired_resources{false};
    gesher::devsvc::DocumentSelection result;
    ULONGLONG start = GetTickCount64();
    std::thread worker([&] {
      result = gesher::devsvc::PickDocuments(mode, u"Gesher exact-source dialog audit",
          u"new-fixture-file.txt", {}, u"*.txt", u"txt", multiple, local,
          GetCurrentProcessId(), Active, &lifetime);
      retired_resources = gesher::devsvc::audit_show_status == HRESULT_FROM_WIN32(ERROR_CANCELLED)
          && gesher::devsvc::audit_window != nullptr
          && gesher::devsvc::audit_thread == GetCurrentThreadId()
          && gesher::devsvc::audit_process == GetCurrentProcessId()
          && !IsWindow(gesher::devsvc::audit_window)
          && gesher::devsvc::document_timers.empty()
          && gesher::devsvc::audit_constructed == 1
          && gesher::devsvc::audit_destroyed == 1;
      std::printf("DOC_RESOURCES show=0x%08lx ownThread=%u ownProcess=%u windowRetired=%u timers=%zu events=%u/%u\n",
          static_cast<unsigned long>(gesher::devsvc::audit_show_status),
          static_cast<unsigned>(gesher::devsvc::audit_thread == GetCurrentThreadId()),
          static_cast<unsigned>(gesher::devsvc::audit_process == GetCurrentProcessId()),
          static_cast<unsigned>(!IsWindow(gesher::devsvc::audit_window)),
          gesher::devsvc::document_timers.size(), gesher::devsvc::audit_constructed,
          gesher::devsvc::audit_destroyed);
      std::fflush(stdout);
      done = true;
    });
    while (!done && GetTickCount64() - start < 10000) Sleep(10);
    std::printf("DOC_RESULT case=%d mode=%d local=%u done=%u polls=%u retired=%u elapsed=%llu\n",
        index, mode, static_cast<unsigned>(local), static_cast<unsigned>(done.load()),
        lifetime.polls.load(), lifetime.retired.load(),
        static_cast<unsigned long long>(GetTickCount64() - start));
    std::fflush(stdout);
    if (!done) ExitProcess(1);
    worker.join();
    if (!retired_resources || result.error || !result.paths.empty() || lifetime.polls < 4
        || lifetime.retired == 0 || GetTickCount64() - start < 800) return 2;
  }
  std::puts("DOC_AUDIT all9 exact-source native cancellation cases passed");
}
'''
generated = header + helpers + "}\n" + body + footer
Path("document-dialog-audit.cc").write_text(generated, encoding="utf-8")
print(json.dumps({"production": str(source.relative_to(root)),
    "production_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    "generated_sha256": hashlib.sha256(generated.encode()).hexdigest(),
    "change": "Only cancellation tracing; complete candidate selection/lifetime bodies retained"}))
