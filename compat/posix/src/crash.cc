// gesher-base: report crashes that nothing else handles.
//
// Until gesher-base delivers signals from Windows exceptions, a fault in
// native code ends the process silently. This prints the exception and a
// stack trace to stderr, with addresses as <module>+<offset>, which
// tools/symbolize.py resolves against the linker map.
//
// Frames are unwound with the Windows unwind tables (.pdata), which clang
// emits for C and C++; ART's assembly stubs have none, so a trace stops at
// the first stub frame.
//
// GESHER_TRACE_FAULTS=1 also reports every access violation when it is
// raised, before any handler sees it: for faults that something handles by
// ending the process, which would otherwise leave no report.

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

namespace {

void PrintAddress(const char* prefix, DWORD64 pc) {
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(pc), &module) &&
      GetModuleFileNameA(module, name, sizeof(name)) != 0) {
    const char* base = strrchr(name, '\\');
    fprintf(stderr, "%s%s+0x%llx\n", prefix, base != nullptr ? base + 1 : name,
            static_cast<unsigned long long>(pc - reinterpret_cast<DWORD64>(module)));
  } else {
    fprintf(stderr, "%s0x%llx\n", prefix, static_cast<unsigned long long>(pc));
  }
}

void Report(const char* what, EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  CONTEXT ctx = *info->ContextRecord;
  fprintf(stderr, "\ngesher: %s 0x%08lx", what, rec->ExceptionCode);
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    const char* op = rec->ExceptionInformation[0] == 0   ? "reading"
                     : rec->ExceptionInformation[0] == 1 ? "writing"
                                                         : "executing";
    fprintf(stderr, " (%s 0x%llx)", op,
            static_cast<unsigned long long>(rec->ExceptionInformation[1]));
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void*>(rec->ExceptionInformation[1]), &mbi,
                     sizeof(mbi)) != 0) {
      const char* state = mbi.State == MEM_COMMIT    ? "committed"
                          : mbi.State == MEM_RESERVE ? "reserved"
                                                     : "free";
      fprintf(stderr, "\n  that page: %s, protect 0x%lx, region %p+0x%zx (allocation %p)",
              state, mbi.Protect, mbi.BaseAddress, mbi.RegionSize, mbi.AllocationBase);
    }
  }
  fprintf(stderr, "\n");
  fprintf(stderr, "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n", ctx.Rax, ctx.Rbx,
          ctx.Rcx, ctx.Rdx);
  fprintf(stderr, "  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n", ctx.Rsi, ctx.Rdi,
          ctx.Rbp, ctx.Rsp);
  fprintf(stderr, "  r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx\n", ctx.R8, ctx.R9,
          ctx.R10, ctx.R11);
  fprintf(stderr, "backtrace:\n");
  for (int frame = 0; frame < 64 && ctx.Rip != 0; ++frame) {
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "  #%02d ", frame);
    PrintAddress(prefix, ctx.Rip);
    DWORD64 image_base = 0;
    RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (fn == nullptr) break;  // no unwind info (e.g. an assembly stub)
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data,
                     &establisher, nullptr);
  }
  fflush(stderr);
}

LONG WINAPI ReportCrash(EXCEPTION_POINTERS* info) {
  Report("unhandled exception", info);
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI TraceFault(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    Report("fault (GESHER_TRACE_FAULTS)", info);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

struct Installer {
  Installer() {
    SetUnhandledExceptionFilter(ReportCrash);
    if (getenv("GESHER_TRACE_FAULTS") != nullptr) AddVectoredExceptionHandler(1, TraceFault);
  }
} installer;

}  // namespace

// Referenced from the link line (posix.cmake) so this object, and with it
// the installer above, is linked into every executable.
extern "C" int gesher_crash_reporter_anchor = 0;
