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
assert body.count("HRESULT status = state->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));") == 1
body = body.replace(
    "HRESULT status = state->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));",
    'std::printf("DOC_CLOSE begin tid=%lu hwnd=%p timer=%llu\\n", GetCurrentThreadId(), '
    'static_cast<void*>(window), static_cast<unsigned long long>(id)); std::fflush(stdout);\n'
    '    HRESULT status = state->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));\n'
    '    std::printf("DOC_CLOSE end hr=0x%08lx\\n", static_cast<unsigned long>(status)); '
    'std::fflush(stdout);')
header = r'''#define WIN32_LEAN_AND_MEAN
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
    gesher::devsvc::DocumentSelection result;
    ULONGLONG start = GetTickCount64();
    std::thread worker([&] {
      result = gesher::devsvc::PickDocuments(mode, u"Gesher exact-source dialog audit",
          u"new-fixture-file.txt", {}, u"*.txt", u"txt", multiple, local,
          GetCurrentProcessId(), Active, &lifetime);
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
    if (result.error || !result.paths.empty() || lifetime.polls < 4
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
    "change": "Only Close begin/end tracing; complete production selection/lifetime bodies retained"}))
