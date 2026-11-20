// Diagnostic-only owner of one unchanged historical libbinder probe and its
// descendants. Sample only this owned root's stacks if it remains alive after
// all transaction checks; retain the original 120-second termination deadline.
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {
void Frame(HANDLE process, DWORD thread, unsigned index, DWORD64 pc) {
  IMAGEHLP_MODULE64 module{}; module.SizeOfStruct = sizeof(module);
  const BOOL found = SymGetModuleInfo64(process, pc, &module);
  std::printf("QA73 STACK thread=%lu frame=%u pc=0x%llx module=%s base=0x%llx rva=0x%llx\n",
    thread, index, static_cast<unsigned long long>(pc), found ? module.ModuleName : "?",
    static_cast<unsigned long long>(found ? module.BaseOfImage : 0),
    static_cast<unsigned long long>(found ? pc - module.BaseOfImage : pc));
}

bool Snapshot(HANDLE process, DWORD pid, DWORD main_thread) {
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
  if (!SymInitialize(process, nullptr, TRUE)) {
    std::printf("QA73 STACK initialize_error=%lu\n", GetLastError()); return false;
  }
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) { SymCleanup(process); return false; }
  THREADENTRY32 entry{}; entry.dwSize = sizeof(entry); unsigned sampled = 0; bool main_sampled = false;
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid) continue;
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                               FALSE, entry.th32ThreadID);
    if (!thread) continue;
    if (GetProcessIdOfThread(thread) != pid) { CloseHandle(thread); continue; }
    DWORD previous = SuspendThread(thread);
    if (previous == static_cast<DWORD>(-1)) { CloseHandle(thread); continue; }
    CONTEXT context{}; context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &context)) {
      if (entry.th32ThreadID == main_thread) main_sampled = true;
      ++sampled; STACKFRAME64 frame{};
      frame.AddrPC.Offset = context.Rip; frame.AddrPC.Mode = AddrModeFlat;
      frame.AddrStack.Offset = context.Rsp; frame.AddrStack.Mode = AddrModeFlat;
      frame.AddrFrame.Offset = context.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
      Frame(process, entry.th32ThreadID, 0, context.Rip);
      for (unsigned i = 1; i <= 40; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context,
              nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset) break;
        Frame(process, entry.th32ThreadID, i, frame.AddrPC.Offset);
      }
    }
    const DWORD resumed = ResumeThread(thread);
    std::printf("QA73 STACK thread=%lu prior_suspend=%lu resume_previous=%lu\n",
                entry.th32ThreadID, previous, resumed);
    CloseHandle(thread);
  }
  CloseHandle(snapshot); SymCleanup(process);
  std::printf("QA73 STACK owned_pid=%lu sampled_threads=%u main_sampled=%d\n", pid, sampled, main_sampled);
  return sampled != 0 && main_sampled;
}

bool Drain(HANDLE pipe, const char* tag, bool* eof) {
  if (*eof) return true;
  DWORD available = 0;
  if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
    if (GetLastError() == ERROR_BROKEN_PIPE) { *eof = true; return true; }
    return false;
  }
  while (available) {
    char bytes[4096]; DWORD read = 0, count = available < sizeof(bytes) ? available : sizeof(bytes);
    if (!ReadFile(pipe, bytes, count, &read, nullptr) || !read) return false;
    std::printf("QA73 BINDER_%s %.*s", tag, static_cast<int>(read), bytes); available -= read;
  }
  std::fflush(stdout); return true;
}

int Run(const wchar_t* path) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE readers[2]{}, writers[2]{};
  for (int i = 0; i < 2; ++i) {
    if (!CreatePipe(&readers[i], &writers[i], &sa, 256 * 1024)) return 125;
    if (!SetHandleInformation(readers[i], HANDLE_FLAG_INHERIT, 0)) return 125;
  }
  STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = writers[0]; si.hStdError = writers[1];
  PROCESS_INFORMATION pi{}; std::wstring command = L"\"" + std::wstring(path) + L"\"";
  if (!CreateProcessW(path, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) return 125;
  CloseHandle(pi.hThread); for (HANDLE h : writers) CloseHandle(h);
  const ULONGLONG start = GetTickCount64(); bool sampled = false, sample_ok = false;
  bool out_eof = false, err_eof = false, healthy = true; DWORD wait = WAIT_TIMEOUT, code = STILL_ACTIVE;
  for (;;) {
    if (!Drain(readers[0], "STDOUT", &out_eof) || !Drain(readers[1], "STDERR", &err_eof)) { healthy = false; break; }
    wait = WaitForSingleObject(pi.hProcess, 0);
    if (wait == WAIT_OBJECT_0) { if (!GetExitCodeProcess(pi.hProcess, &code)) healthy = false; break; }
    if (wait != WAIT_TIMEOUT) { healthy = false; break; }
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (!sampled && elapsed >= 20000) {
      std::printf("QA73 BINDER root=%lu main_thread=%lu elapsed_ms=%llu sampling_owned_root\n", pi.dwProcessId, pi.dwThreadId, static_cast<unsigned long long>(elapsed));
      sampled = true; sample_ok = Snapshot(pi.hProcess, pi.dwProcessId, pi.dwThreadId);
    }
    if (elapsed >= 120000) break;
    Sleep(10);
  }
  const bool timed_out = wait != WAIT_OBJECT_0;
  if (timed_out) { TerminateProcess(pi.hProcess, 124); WaitForSingleObject(pi.hProcess, 5000); }
  Drain(readers[0], "STDOUT", &out_eof); Drain(readers[1], "STDERR", &err_eof);
  CloseHandle(pi.hProcess); for (HANDLE h : readers) CloseHandle(h);
  std::printf("QA73 BINDER_RESULT pid=%lu wait=%lu code=%lu healthy=%d timeout=%d sampled=%d sample_ok=%d elapsed_ms=%llu\n",
    pi.dwProcessId, wait, code, healthy, timed_out, sampled, sample_ok,
    static_cast<unsigned long long>(GetTickCount64() - start));
  if (!healthy || (sampled && !sample_ok)) return 125;
  return timed_out ? 124 : static_cast<int>(code);
}
}  // namespace
int wmain(int argc, wchar_t** argv) { return argc == 2 ? Run(argv[1]) : 2; }
