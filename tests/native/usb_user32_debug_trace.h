// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace gesher::usb_storage::test {
// Only the disposable late-loader diagnostic process is observed. Hardware
// breakpoints preserve the actual call ABI and record raw returns, without
// calling undocumented APIs, replacing their results or patching DLL code.
class User32NativeDebugTrace {
 public:
  User32NativeDebugTrace(HANDLE process, DWORD thread, std::span<char> tail, DWORD* bytes)
      : process_(process), thread_(thread), tail_(tail), bytes_(bytes) {}
  ~User32NativeDebugTrace() {
    const DWORD cleanup = Clear();
    char line[256]{};
    int count = std::snprintf(line, sizeof(line), "USER32 native trace: setup=%lu error=%lu cleanup=%lu\n", setup_, error_, cleanup);
    Append(std::string_view(line, static_cast<size_t>(count)));
    for (size_t i = 0; i < names_.size(); ++i) {
      count = std::snprintf(line, sizeof(line), "USER32 native trace: api=%s entries=%lu returns=%lu pending=%lu",
          names_[i], calls_[i].entries, calls_[i].returns, calls_[i].pending ? 1ul : 0ul);
      std::string row(line, static_cast<size_t>(count));
      for (DWORD n = 0; n < calls_[i].returns; ++n) {
        count = std::snprintf(line, sizeof(line), " raw%lu=0x%08lx", n, calls_[i].result[n]);
        row.append(line, static_cast<size_t>(count));
      }
      row += '\n'; Append(row);
    }
  }
  void Loaded(const DEBUG_EVENT& event) {
    if (setup_ != ERROR_NOT_READY || event.dwDebugEventCode != LOAD_DLL_DEBUG_EVENT ||
        !event.u.LoadDll.hFile || event.u.LoadDll.hFile == INVALID_HANDLE_VALUE) return;
    std::array<wchar_t, 1024> path{}, local_path{}, system{};
    DWORD used = GetFinalPathNameByHandleW(event.u.LoadDll.hFile, path.data(), static_cast<DWORD>(path.size()), 0);
    if (!used || used >= path.size()) return;
    std::wstring name(path.data(), used);
    if (name.starts_with(L"\\\\?\\")) name.erase(0, 4);
    const UINT system_size = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (!system_size || system_size >= system.size()) return;
    const std::wstring expected = std::wstring(system.data(), system_size) + L"\\ntdll.dll";
    if (CompareStringOrdinal(name.c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL) return;
    setup_ = ERROR_INVALID_DATA;
    const HMODULE local = GetModuleHandleW(L"ntdll.dll");
    used = local ? GetModuleFileNameW(local, local_path.data(), static_cast<DWORD>(local_path.size())) : 0;
    if (!used || used >= local_path.size() ||
        CompareStringOrdinal(local_path.data(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL) return;
    const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, thread_);
    if (!thread) { setup_ = GetLastError(); return; }
    CONTEXT context{}; context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(thread, &context) || (context.Dr7 & 0xff)) {
      setup_ = ERROR_BUSY; CloseHandle(thread); return;
    }
    std::array<DWORD64*, 4> registers{&context.Dr0, &context.Dr1, &context.Dr2, &context.Dr3};
    bool complete = true;
    for (size_t i = 0; i < names_.size(); ++i) {
      const FARPROC target = GetProcAddress(local, names_[i]);
      MEMORY_BASIC_INFORMATION info{};
      const void* local_address = reinterpret_cast<const void*>(target);
      if (!target || !VirtualQuery(local_address, &info, sizeof(info)) || info.AllocationBase != local) {
        complete = false; break;
      }
      const uintptr_t offset = reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(local);
      const uintptr_t remote = reinterpret_cast<uintptr_t>(event.u.LoadDll.lpBaseOfDll) + offset;
      std::array<std::byte, 16> remote_code{}, local_code{};
      std::memcpy(local_code.data(), local_address, local_code.size());
      if (!Read(remote, remote_code) || remote_code != local_code) { complete = false; break; }
      calls_[i].entry = remote;
      *registers[i] = remote;
      context.Dr7 |= DWORD64{1} << (i * 2);
    }
    if (complete) {
      context.Dr6 = 0; armed_ = SetThreadContext(thread, &context) != FALSE;
      setup_ = armed_ ? ERROR_SUCCESS : GetLastError();
      CONTEXT observed{}; observed.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (setup_ == ERROR_SUCCESS && (!GetThreadContext(thread, &observed) ||
          observed.Dr0 != context.Dr0 || observed.Dr1 != context.Dr1 ||
          observed.Dr2 != context.Dr2 || observed.Dr3 != context.Dr3 ||
          (observed.Dr7 & 0xff) != (context.Dr7 & 0xff))) setup_ = ERROR_INVALID_STATE;
    }
    CloseHandle(thread);
  }
  void InitialBreakpoint() {
    if (setup_ != ERROR_SUCCESS) return;
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, thread_);
    if (!thread) { error_ = GetLastError(); return; }
    CONTEXT context{}; context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(thread, &context)) {
      std::array<DWORD64*, 4> registers{&context.Dr0, &context.Dr1, &context.Dr2, &context.Dr3};
      context.Dr7 = 0; context.Dr6 = 0;
      for (size_t i = 0; i < calls_.size(); ++i) {
        *registers[i] = calls_[i].entry;
        context.Dr7 |= DWORD64{1} << (i * 2);
      }
      if (!SetThreadContext(thread, &context)) error_ = GetLastError();
      CONTEXT observed{}; observed.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (!GetThreadContext(thread, &observed) || observed.Dr0 != context.Dr0 ||
          observed.Dr1 != context.Dr1 || observed.Dr2 != context.Dr2 ||
          observed.Dr3 != context.Dr3 || (observed.Dr7 & 0xff) != (context.Dr7 & 0xff))
        error_ = ERROR_INVALID_STATE;
    } else error_ = GetLastError();
    CloseHandle(thread);
  }
  bool EntryMarker(const OUTPUT_DEBUG_STRING_INFO& info) {
    constexpr char marker[] = "GESHER_LOADER_ENTRY";
    if (info.fUnicode || info.nDebugStringLength != sizeof(marker)) return false;
    std::array<char, sizeof(marker)> actual{};
    if (!Read(reinterpret_cast<uintptr_t>(info.lpDebugStringData),
        std::as_writable_bytes(std::span(actual))) || std::memcmp(actual.data(), marker, actual.size()) != 0)
      return false;
    for (auto& call : calls_) {
      call.entries = 0; call.returns = 0; call.pending = false; call.result.fill(0);
    }
    InitialBreakpoint();
    return true;
  }
  bool Exception(const DEBUG_EVENT& event) {
    if (setup_ != ERROR_SUCCESS || event.dwThreadId != thread_ ||
        event.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_SINGLE_STEP) return false;
    const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, thread_);
    if (!thread) { error_ = GetLastError(); return false; }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_DEBUG_REGISTERS;
    bool handled = false;
    if (GetThreadContext(thread, &context)) {
      std::array<DWORD64*, 4> registers{&context.Dr0, &context.Dr1, &context.Dr2, &context.Dr3};
      for (size_t i = 0; i < calls_.size(); ++i) {
        if (!(context.Dr6 & (DWORD64{1} << i))) continue;
        auto& call = calls_[i];
        if (!*registers[i] || context.Rip != *registers[i]) continue;
        handled = true;
        if (call.pending) {
          call.result[call.returns++] = static_cast<DWORD>(context.Rax);
          call.pending = false;
          if (call.returns == call.result.size()) {
            *registers[i] = 0; context.Dr7 &= ~(DWORD64{1} << (i * 2));
          } else *registers[i] = call.entry;
        } else {
          ++call.entries;
          uintptr_t address = 0;
          if (Read(context.Rsp, std::as_writable_bytes(std::span(&address, 1))) && address) {
            *registers[i] = address; call.pending = true;
          } else {
            error_ = ERROR_PARTIAL_COPY;
            *registers[i] = 0; context.Dr7 &= ~(DWORD64{1} << (i * 2));
          }
        }
      }
      if (handled) {
        context.Dr6 = 0; context.EFlags |= 0x10000;  // RF: execute this exact breakpoint instruction once.
        if (!SetThreadContext(thread, &context)) { error_ = GetLastError(); handled = false; }
      }
    } else error_ = GetLastError();
    CloseHandle(thread); return handled;
  }
 private:
  DWORD Clear() {
    DWORD exit = STILL_ACTIVE;
    if (GetExitCodeProcess(process_, &exit) && exit != STILL_ACTIVE) return ERROR_SUCCESS;
    if (!armed_) return ERROR_SUCCESS;
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, thread_);
    if (!thread) return GetLastError();
    DWORD status = ERROR_SUCCESS;
    if (SuspendThread(thread) == MAXDWORD) status = GetLastError();
    else {
      CONTEXT context{}; context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (!GetThreadContext(thread, &context)) status = GetLastError();
      else {
        context.Dr0 = 0; context.Dr1 = 0; context.Dr2 = 0; context.Dr3 = 0;
        context.Dr6 = 0; context.Dr7 &= ~DWORD64{0xff};
        if (!SetThreadContext(thread, &context)) status = GetLastError();
      }
      if (ResumeThread(thread) == MAXDWORD && status == ERROR_SUCCESS) status = GetLastError();
    }
    CloseHandle(thread); return status;
  }
  bool Read(uintptr_t address, std::span<std::byte> output) const {
    SIZE_T copied = 0;
    return ReadProcessMemory(process_, reinterpret_cast<const void*>(address), output.data(), output.size(), &copied) &&
        copied == output.size();
  }
  void Append(std::string_view line) {
    if (*bytes_ > tail_.size() || line.size() > tail_.size()) return;
    if (*bytes_ + line.size() > tail_.size()) {
      size_t discard = *bytes_ + line.size() - tail_.size();
      while (discard < *bytes_ && tail_[discard - 1] != '\n') ++discard;
      std::memmove(tail_.data(), tail_.data() + discard, *bytes_ - discard);
      *bytes_ -= static_cast<DWORD>(discard);
    }
    std::memcpy(tail_.data() + *bytes_, line.data(), line.size()); *bytes_ += static_cast<DWORD>(line.size());
  }
  struct Call { uintptr_t entry = 0; DWORD entries = 0, returns = 0; bool pending = false; std::array<DWORD, 4> result{}; };
  static constexpr std::array<const char*, 4> names_{
      "CsrClientConnectToServer", "CsrClientCallServer", "NtAlpcConnectPort", "NtAlpcSendWaitReceivePort"};
  HANDLE process_; DWORD thread_; std::span<char> tail_; DWORD* bytes_;
  DWORD setup_ = ERROR_NOT_READY, error_ = ERROR_SUCCESS;
  bool armed_ = false;
  std::array<Call, 4> calls_{};
};
}  // namespace gesher::usb_storage::test
