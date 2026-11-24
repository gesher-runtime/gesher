// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Disposable-test-only observer: no production dependency or inline patches.
// Native ABIs: Microsoft's NtOpenThreadToken[Ex], NtOpenProcessToken and
// ZwOpenProcessTokenEx, NtOpenDirectoryObject, ZwCreateDirectoryObject and
// NtCreateLowBoxToken documentation. BOOLEAN is deliberately not Win32 BOOL.
// The additional Ex directory declaration and RTL last-status declarations are
// independently checked against the maintained primary phnt headers at:
// https://github.com/winsiderss/phnt/blob/53fbbdc5b5d2b08761db1c7b26bfa8c820924356/ntobapi.h
// https://github.com/winsiderss/phnt/blob/53fbbdc5b5d2b08761db1c7b26bfa8c820924356/ntrtl.h
// Those declarations are ABI evidence, not a Microsoft support guarantee.
#include "usb_lowbox_iat_trace.h"
#include <winternl.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#if defined(_MSC_VER)
#include <intrin.h>
#define TRACE_NOINLINE __declspec(noinline)
#define TRACE_CALLER() _ReturnAddress()
#else
#define TRACE_NOINLINE __attribute__((noinline))
#define TRACE_CALLER() __builtin_return_address(0)
#endif

namespace gesher::usb_storage::test {
namespace {
using OpenThread = NTSTATUS(NTAPI*)(HANDLE, ACCESS_MASK, BOOLEAN, PHANDLE);
using OpenThreadEx = NTSTATUS(NTAPI*)(HANDLE, ACCESS_MASK, BOOLEAN, ULONG, PHANDLE);
using OpenProcess = NTSTATUS(NTAPI*)(HANDLE, ACCESS_MASK, PHANDLE);
using OpenProcessEx = NTSTATUS(NTAPI*)(HANDLE, ACCESS_MASK, ULONG, PHANDLE);
using Directory = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
using DirectoryEx = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, ULONG);
using Lowbox = NTSTATUS(NTAPI*)(PHANDLE, HANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
    PSID, ULONG, PSID_AND_ATTRIBUTES, ULONG, PHANDLE);
using LastNtStatus = NTSTATUS(NTAPI*)();
using SetNtStatus = VOID(NTAPI*)(NTSTATUS);

constexpr DWORD kSlots = 32, kPages = 16;
constexpr const char* kNames[kLowboxTraceSymbolCount] = {
    "NtOpenThreadToken", "NtOpenThreadTokenEx", "NtOpenProcessToken", "NtOpenProcessTokenEx",
    "NtOpenDirectoryObject", "NtCreateDirectoryObject", "NtCreateDirectoryObjectEx", "NtCreateLowBoxToken"};
struct Slot { void* volatile* address = nullptr; void* original = nullptr; void* hook = nullptr; DWORD api = 0; bool installed = false; };
struct Page { void* address = nullptr; DWORD original = 0; bool writable = false; };
struct State {
  LowboxIatTraceEvidence* evidence = nullptr;
  ULONG_PTR base = 0;
  DWORD image_size = 0, thread = 0, page_size = 0, slot_count = 0, page_count = 0;
  Slot slots[kSlots]{};
  Page pages[kPages]{};
  void* originals[kLowboxTraceSymbolCount]{};
  LastNtStatus last_status = nullptr;
  SetNtStatus set_status = nullptr;
  // Keep the first restoration failure even if a later rollback can recover.
  // Recovery cannot turn the fixture's unsafe-restoration observation green.
  DWORD restore_failure = ERROR_SUCCESS, page_restore_failure = ERROR_SUCCESS;
  volatile LONG armed = 0, used = 0;
  bool ended = false;
};
// Hook destinations and all forwarding pointers remain alive until process
// exit, including an unsuccessful restoration. Other threads never dereference
// the caller-owned evidence; they forward through these immutable originals.
State state;
thread_local bool observing = false;

struct Failure { DWORD error; };
void Need(bool ok, DWORD error = ERROR_SUCCESS) {
  if (!ok) { if (!error) error = GetLastError(); throw Failure{error ? error : ERROR_GEN_FAILURE}; }
}
DWORD Error() { const DWORD error = GetLastError(); return error ? error : ERROR_GEN_FAILURE; }
bool Range(DWORD rva, SIZE_T size) {
  return rva < state.image_size && size <= state.image_size - rva;
}
bool Readable(DWORD protection) {
  if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
  const DWORD access = protection & 0xff;
  return access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
      access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}
// All snapshots happen BEFORE forwarding. Guarded, bounded copying accepts no
// guard/no-access pages and catches a concurrent unmap. This POD-only function
// uses Windows SEH (MinGW Clang requires -fms-extensions for this test unit).
bool Snapshot(void* destination, const void* source, SIZE_T size) noexcept {
  if (!size) return true;
  if (!source || size > 4096) return false;
  const ULONG_PTR begin = reinterpret_cast<ULONG_PTR>(source);
  if (begin > (std::numeric_limits<ULONG_PTR>::max)() - size) return false;
  const ULONG_PTR end = begin + size;
  ULONG_PTR current = begin;
  for (DWORD regions = 0; current < end && regions < 3; ++regions) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || !Readable(info.Protect)) return false;
    const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(info.BaseAddress);
    if (base > current || info.RegionSize > (std::numeric_limits<ULONG_PTR>::max)() - base ||
        base + info.RegionSize <= current) return false;
    current = (std::min)(end, base + info.RegionSize);
  }
  if (current != end) return false;
  __try { std::memcpy(destination, source, size); return true; }
  __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <class T> T ImageValue(DWORD rva) {
  T value{};
  Need(Range(rva, sizeof(value)) && Snapshot(&value, reinterpret_cast<const void*>(state.base + rva), sizeof(value)),
      ERROR_BAD_EXE_FORMAT);
  return value;
}
bool ImageText(DWORD rva, char* output, SIZE_T capacity) {
  for (SIZE_T i = 0; i < capacity; ++i) {
    if (i > MAXDWORD - rva || !Range(rva + static_cast<DWORD>(i), 1) ||
        !Snapshot(output + i, reinterpret_cast<const void*>(state.base + rva + i), 1)) return false;
    if (!output[i]) return true;
  }
  return false;
}
bool NtdllName(const char* name) {
  constexpr char expected[] = "ntdll.dll";
  for (SIZE_T i = 0; i < sizeof(expected); ++i) {
    char c = name[i]; if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (c != expected[i]) return false;
    if (!c) return true;
  }
  return false;
}
DWORD Symbol(const char* name) {
  for (DWORD i = 0; i < kLowboxTraceSymbolCount; ++i) if (std::strcmp(name, kNames[i]) == 0) return i;
  return kLowboxTraceSymbolCount;
}
void ObjectSnapshot(LowboxTraceCall* call, POBJECT_ATTRIBUTES source) noexcept {
  if (!source) return;
  OBJECT_ATTRIBUTES object{};
  if (!Snapshot(&object, source, sizeof(object)) || object.Length != sizeof(object)) {
    call->input_snapshot_status = ERROR_INVALID_DATA; return;
  }
  call->object_attributes = object.Attributes;
  call->root = reinterpret_cast<ULONG_PTR>(object.RootDirectory);
  if (!object.ObjectName) return;
  UNICODE_STRING name{};
  if (!Snapshot(&name, object.ObjectName, sizeof(name)) || (name.Length % sizeof(wchar_t)) ||
      name.Length > name.MaximumLength || (name.Length && !name.Buffer)) {
    call->input_snapshot_status = ERROR_INVALID_DATA; return;
  }
  call->name_bytes = name.Length;
  const DWORD characters = name.Length / sizeof(wchar_t);
  const DWORD copied = (std::min)(characters, static_cast<DWORD>(std::size(call->name) - 1));
  if (!Snapshot(call->name, name.Buffer, copied * sizeof(wchar_t))) {
    call->input_snapshot_status = ERROR_NOACCESS; call->name[0] = 0; return;
  }
  call->name[copied] = 0;
  call->name_characters = copied;
  call->name_truncated = copied != characters;
}

template <class Invoke>
NTSTATUS Forward(LowboxTraceApi api, LowboxTraceCall input, POBJECT_ATTRIBUTES object,
    PHANDLE output, void* caller, Invoke invoke) noexcept {
  const DWORD entry_error = GetLastError();
  const NTSTATUS entry_status = state.last_status();
  const bool record = GetCurrentThreadId() == state.thread &&
      InterlockedCompareExchange(&state.armed, 0, 0) != 0 && !observing;
  if (!record) {
    // Even the cheap thread filter is outside the native call's semantics.
    state.set_status(entry_status); SetLastError(entry_error);
    return invoke();
  }
  observing = true;
  auto& evidence = *state.evidence;
  const DWORD sequence = evidence.total_calls;
  LowboxTraceCall* call = nullptr;
  if (sequence != MAXDWORD) {
    ++evidence.total_calls;
    evidence.record_count = (std::min)(evidence.total_calls, kLowboxTraceRecordCount);
    evidence.overflow = evidence.total_calls - evidence.record_count;
    call = &evidence.records[sequence % kLowboxTraceRecordCount];
    *call = input; call->sequence = sequence; call->api = api;
    const ULONG_PTR address = reinterpret_cast<ULONG_PTR>(caller);
    if (address >= state.base && address - state.base < state.image_size)
      call->caller_rva = static_cast<DWORD>(address - state.base);
    ObjectSnapshot(call, object);
  }
  // Snapshot helpers may change both error slots. Restore both before passing
  // the exact original arguments, token context and access mask to ntdll.
  state.set_status(entry_status); SetLastError(entry_error);
  const NTSTATUS result = invoke();
  const DWORD returned_error = GetLastError();
  if (call) {
    call->status = static_cast<DWORD>(result);
    // On NT_SUCCESS the native contract has just written this caller-owned
    // output slot. KernelBase retains its arguments until this return. No API,
    // token query, allocation or name resolution runs after forwarding.
    if (result >= 0 && output) {
      HANDLE value = nullptr; std::memcpy(&value, output, sizeof(value));
      call->result = reinterpret_cast<ULONG_PTR>(value);
      call->returned_handle = value && value != INVALID_HANDLE_VALUE;
    }
  }
  observing = false;
  SetLastError(returned_error);
  return result;
}
template <class T> T Original(LowboxTraceApi api) noexcept {
  return reinterpret_cast<T>(state.originals[static_cast<DWORD>(api)]);
}
TRACE_NOINLINE NTSTATUS NTAPI HookOpenThread(HANDLE thread, ACCESS_MASK access, BOOLEAN self, PHANDLE out) noexcept {
  LowboxTraceCall in{}; in.source = reinterpret_cast<ULONG_PTR>(thread); in.access = access; in.open_as_self = self;
  return Forward(LowboxTraceApi::open_thread_token, in, nullptr, out, TRACE_CALLER(),
      [&] { return Original<OpenThread>(LowboxTraceApi::open_thread_token)(thread, access, self, out); });
}
TRACE_NOINLINE NTSTATUS NTAPI HookOpenThreadEx(HANDLE thread, ACCESS_MASK access, BOOLEAN self, ULONG attributes, PHANDLE out) noexcept {
  LowboxTraceCall in{}; in.source = reinterpret_cast<ULONG_PTR>(thread); in.access = access; in.open_as_self = self; in.handle_attributes = attributes;
  return Forward(LowboxTraceApi::open_thread_token_ex, in, nullptr, out, TRACE_CALLER(),
      [&] { return Original<OpenThreadEx>(LowboxTraceApi::open_thread_token_ex)(thread, access, self, attributes, out); });
}
TRACE_NOINLINE NTSTATUS NTAPI HookOpenProcess(HANDLE process, ACCESS_MASK access, PHANDLE out) noexcept {
  LowboxTraceCall in{}; in.source = reinterpret_cast<ULONG_PTR>(process); in.access = access;
  return Forward(LowboxTraceApi::open_process_token, in, nullptr, out, TRACE_CALLER(),
      [&] { return Original<OpenProcess>(LowboxTraceApi::open_process_token)(process, access, out); });
}
TRACE_NOINLINE NTSTATUS NTAPI HookOpenProcessEx(HANDLE process, ACCESS_MASK access, ULONG attributes, PHANDLE out) noexcept {
  LowboxTraceCall in{}; in.source = reinterpret_cast<ULONG_PTR>(process); in.access = access; in.handle_attributes = attributes;
  return Forward(LowboxTraceApi::open_process_token_ex, in, nullptr, out, TRACE_CALLER(),
      [&] { return Original<OpenProcessEx>(LowboxTraceApi::open_process_token_ex)(process, access, attributes, out); });
}
TRACE_NOINLINE NTSTATUS NTAPI HookOpenDirectory(PHANDLE out, ACCESS_MASK access, POBJECT_ATTRIBUTES object) noexcept {
  LowboxTraceCall in{}; in.access = access;
  return Forward(LowboxTraceApi::open_directory, in, object, out, TRACE_CALLER(),
      [&] { return Original<Directory>(LowboxTraceApi::open_directory)(out, access, object); });
}
TRACE_NOINLINE NTSTATUS NTAPI HookCreateDirectory(PHANDLE out, ACCESS_MASK access, POBJECT_ATTRIBUTES object) noexcept {
  LowboxTraceCall in{}; in.access = access;
  return Forward(LowboxTraceApi::create_directory, in, object, out, TRACE_CALLER(),
      [&] { return Original<Directory>(LowboxTraceApi::create_directory)(out, access, object); });
}
TRACE_NOINLINE NTSTATUS NTAPI HookCreateDirectoryEx(PHANDLE out, ACCESS_MASK access, POBJECT_ATTRIBUTES object, HANDLE shadow, ULONG flags) noexcept {
  LowboxTraceCall in{}; in.access = access; in.shadow = reinterpret_cast<ULONG_PTR>(shadow); in.flags = flags;
  return Forward(LowboxTraceApi::create_directory_ex, in, object, out, TRACE_CALLER(),
      [&] { return Original<DirectoryEx>(LowboxTraceApi::create_directory_ex)(out, access, object, shadow, flags); });
}
TRACE_NOINLINE NTSTATUS NTAPI HookCreateLowbox(PHANDLE out, HANDLE source, ACCESS_MASK access, POBJECT_ATTRIBUTES object,
    PSID package, ULONG count, PSID_AND_ATTRIBUTES capabilities, ULONG handles, PHANDLE values) noexcept {
  LowboxTraceCall in{}; in.source = reinterpret_cast<ULONG_PTR>(source); in.access = access; in.capability_count = count; in.handle_count = handles;
  return Forward(LowboxTraceApi::create_lowbox_token, in, object, out, TRACE_CALLER(),
      [&] { return Original<Lowbox>(LowboxTraceApi::create_lowbox_token)(out, source, access, object, package, count, capabilities, handles, values); });
}
void* Hook(DWORD api) {
  void* hooks[kLowboxTraceSymbolCount] = {
      reinterpret_cast<void*>(&HookOpenThread), reinterpret_cast<void*>(&HookOpenThreadEx),
      reinterpret_cast<void*>(&HookOpenProcess), reinterpret_cast<void*>(&HookOpenProcessEx),
      reinterpret_cast<void*>(&HookOpenDirectory), reinterpret_cast<void*>(&HookCreateDirectory),
      reinterpret_cast<void*>(&HookCreateDirectoryEx), reinterpret_cast<void*>(&HookCreateLowbox)};
  return hooks[api];
}

void InspectImage(HMODULE module, LowboxIatTraceEvidence* evidence) {
  MEMORY_BASIC_INFORMATION region{};
  Need(VirtualQuery(module, &region, sizeof(region)) == sizeof(region) && region.AllocationBase == module &&
      region.Type == MEM_IMAGE && region.State == MEM_COMMIT && Readable(region.Protect), ERROR_BAD_EXE_FORMAT);
  IMAGE_DOS_HEADER dos{};
  Need(Snapshot(&dos, module, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
      dos.e_lfanew < 4096, ERROR_BAD_EXE_FORMAT);
  IMAGE_NT_HEADERS64 nt{};
  state.base = reinterpret_cast<ULONG_PTR>(module);
  Need(Snapshot(&nt, reinterpret_cast<void*>(state.base + static_cast<DWORD>(dos.e_lfanew)), sizeof(nt)) &&
      nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
      nt.FileHeader.SizeOfOptionalHeader == sizeof(IMAGE_OPTIONAL_HEADER64) && nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
      nt.OptionalHeader.SizeOfImage >= 4096 && nt.OptionalHeader.SizeOfImage <= 128 * 1024 * 1024 &&
      nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT, ERROR_BAD_EXE_FORMAT);
  state.image_size = nt.OptionalHeader.SizeOfImage; evidence->image_size = state.image_size;
  if (nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT)
    evidence->delay_imports_present = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].Size != 0;
  const auto table = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  Need(table.VirtualAddress && table.Size >= sizeof(IMAGE_IMPORT_DESCRIPTOR) && Range(table.VirtualAddress, table.Size), ERROR_BAD_EXE_FORMAT);
  bool terminated = false;
  const DWORD descriptor_count = (std::min)(table.Size / static_cast<DWORD>(sizeof(IMAGE_IMPORT_DESCRIPTOR)), DWORD{512});
  for (DWORD n = 0; n < descriptor_count; ++n) {
    const auto descriptor = ImageValue<IMAGE_IMPORT_DESCRIPTOR>(
        table.VirtualAddress + n * static_cast<DWORD>(sizeof(IMAGE_IMPORT_DESCRIPTOR)));
    if (!descriptor.Name && !descriptor.FirstThunk && !descriptor.OriginalFirstThunk) { terminated = true; break; }
    char library[128]{};
    Need(ImageText(descriptor.Name, library, sizeof(library)), ERROR_BAD_EXE_FORMAT);
    if (!NtdllName(library)) continue;
    if (!descriptor.OriginalFirstThunk || !descriptor.FirstThunk) { ++evidence->uninspectable_imports; continue; }
    bool thunks_end = false;
    for (DWORD i = 0; i < 8192; ++i) {
      const ULONGLONG lookup_rva = static_cast<ULONGLONG>(descriptor.OriginalFirstThunk) + i * sizeof(IMAGE_THUNK_DATA64);
      const ULONGLONG slot_rva = static_cast<ULONGLONG>(descriptor.FirstThunk) + i * sizeof(IMAGE_THUNK_DATA64);
      Need(lookup_rva <= MAXDWORD && slot_rva <= MAXDWORD, ERROR_BAD_EXE_FORMAT);
      const auto lookup = ImageValue<IMAGE_THUNK_DATA64>(static_cast<DWORD>(lookup_rva));
      if (!lookup.u1.AddressOfData) { thunks_end = true; break; }
      if (IMAGE_SNAP_BY_ORDINAL64(lookup.u1.Ordinal)) { ++evidence->uninspectable_imports; continue; }
      Need(lookup.u1.AddressOfData <= MAXDWORD - sizeof(WORD), ERROR_BAD_EXE_FORMAT);
      char name[128]{};
      Need(ImageText(static_cast<DWORD>(lookup.u1.AddressOfData) + static_cast<DWORD>(sizeof(WORD)), name, sizeof(name)), ERROR_BAD_EXE_FORMAT);
      const DWORD api = Symbol(name); if (api == kLowboxTraceSymbolCount) continue;
      ++evidence->symbols[api].imported;
      Need(Range(static_cast<DWORD>(slot_rva), sizeof(void*)) && ((state.base + slot_rva) % alignof(void*)) == 0,
          ERROR_BAD_EXE_FORMAT);
      const auto value = ImageValue<IMAGE_THUNK_DATA64>(static_cast<DWORD>(slot_rva));
      if (value.u1.Function != reinterpret_cast<ULONG_PTR>(state.originals[api])) {
        evidence->symbols[api].status = ERROR_INVALID_ADDRESS; continue;
      }
      Need(state.slot_count < kSlots, ERROR_MORE_DATA);
      auto* slot = reinterpret_cast<void* volatile*>(state.base + slot_rva);
      for (DWORD prior = 0; prior < state.slot_count; ++prior) Need(state.slots[prior].address != slot, ERROR_INVALID_DATA);
      state.slots[state.slot_count++] = {slot, state.originals[api], Hook(api), api, false};
      evidence->symbols[api].status = ERROR_SUCCESS;
    }
    Need(thunks_end, ERROR_BAD_EXE_FORMAT);
  }
  Need(terminated, ERROR_BAD_EXE_FORMAT);
  evidence->slot_count = state.slot_count;
  for (const auto& symbol : evidence->symbols) if (!symbol.imported) ++evidence->missing_symbols;
  Need(state.slot_count != 0, ERROR_PROC_NOT_FOUND);
}
Page& PageFor(void* address) {
  const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(address) & ~(static_cast<ULONG_PTR>(state.page_size) - 1);
  for (DWORD i = 0; i < state.page_count; ++i) if (state.pages[i].address == reinterpret_cast<void*>(base)) return state.pages[i];
  Need(state.page_count < kPages, ERROR_MORE_DATA);
  MEMORY_BASIC_INFORMATION info{};
  Need(VirtualQuery(address, &info, sizeof(info)) == sizeof(info) && info.Type == MEM_IMAGE &&
      reinterpret_cast<ULONG_PTR>(info.AllocationBase) == state.base && info.State == MEM_COMMIT &&
      (info.Protect == PAGE_READONLY || info.Protect == PAGE_READWRITE || info.Protect == PAGE_WRITECOPY),
      ERROR_INVALID_ACCESS);
  auto& page = state.pages[state.page_count++];
  page.address = reinterpret_cast<void*>(base); page.original = info.Protect;
  return page;
}
DWORD RestorePages(LowboxIatTraceEvidence* evidence) noexcept {
  DWORD result = ERROR_SUCCESS;
  for (DWORD i = 0; i < state.page_count; ++i) {
    auto& page = state.pages[i]; if (!page.writable) continue;
    DWORD discarded = 0;
    if (!VirtualProtect(page.address, state.page_size, page.original, &discarded)) {
      if (!result) result = Error();
    } else {
      // The restoration API already removed our write permission. A failed
      // readback must not let later rollback write without reacquiring it.
      page.writable = false;
      MEMORY_BASIC_INFORMATION info{};
      if (VirtualQuery(page.address, &info, sizeof(info)) != sizeof(info) ||
          reinterpret_cast<ULONG_PTR>(info.AllocationBase) != state.base ||
          info.Type != MEM_IMAGE || info.State != MEM_COMMIT || info.Protect != page.original) {
        if (!result) result = ERROR_INVALID_ACCESS;
      }
    }
  }
  if (result && !state.page_restore_failure) state.page_restore_failure = result;
  if (result && !state.restore_failure) state.restore_failure = result;
  evidence->page_restore_status = state.page_restore_failure;
  return result;
}
void Install(LowboxIatTraceEvidence* evidence) {
  SYSTEM_INFO info{}; GetSystemInfo(&info);
  state.page_size = info.dwPageSize;
  Need(state.page_size >= sizeof(void*) && !(state.page_size & (state.page_size - 1)), ERROR_INVALID_DATA);
  for (DWORD i = 0; i < state.slot_count; ++i) PageFor(const_cast<void**>(state.slots[i].address));
  evidence->page_count = state.page_count;
  for (DWORD i = 0; i < state.page_count; ++i) {
    auto& page = state.pages[i]; DWORD old = 0;
    Need(VirtualProtect(page.address, state.page_size, PAGE_READWRITE, &old)); page.writable = true;
    if (old != page.original) {
      // A concurrent protection change is not ours to overwrite. Roll back to
      // the actual value returned by VirtualProtect and abandon installation.
      page.original = old;
      if (!state.restore_failure) state.restore_failure = ERROR_INVALID_ACCESS;
      Need(false, ERROR_INVALID_ACCESS);
    }
  }
  for (DWORD i = 0; i < state.slot_count; ++i) {
    auto& slot = state.slots[i];
    Need(InterlockedCompareExchangePointer(slot.address, slot.hook, slot.original) == slot.original, ERROR_BUSY);
    slot.installed = true; ++evidence->symbols[slot.api].patched;
  }
  const DWORD restored = RestorePages(evidence);
  Need(restored == ERROR_SUCCESS, restored);
  // Only data pointers changed; executable code/cache/CFG policy is untouched.
  InterlockedExchange(&state.armed, 1);
}
DWORD Restore(LowboxIatTraceEvidence* evidence) noexcept {
  InterlockedExchange(&state.armed, 0);
  DWORD result = ERROR_SUCCESS;
  for (DWORD p = 0; p < state.page_count; ++p) {
    auto& page = state.pages[p]; bool needed = false;
    const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(page.address);
    for (DWORD i = 0; i < state.slot_count; ++i) {
      const ULONG_PTR address = reinterpret_cast<ULONG_PTR>(state.slots[i].address);
      if (state.slots[i].installed && address >= base && address - base < state.page_size) needed = true;
    }
    if (!needed) continue;
    if (!page.writable) {
      DWORD old = 0;
      if (!VirtualProtect(page.address, state.page_size, PAGE_READWRITE, &old)) { if (!result) result = Error(); continue; }
      page.writable = true;
      if (old != page.original) {
        // Preserve a foreign protection change, but never report that the
        // original instrumentation state was restored without interference.
        page.original = old;
        if (!result) result = ERROR_INVALID_ACCESS;
      }
    }
    for (DWORD i = 0; i < state.slot_count; ++i) {
      auto& slot = state.slots[i]; const ULONG_PTR address = reinterpret_cast<ULONG_PTR>(slot.address);
      if (!slot.installed || address < base || address - base >= state.page_size) continue;
      if (InterlockedCompareExchangePointer(slot.address, slot.original, slot.hook) != slot.hook) {
        if (!result) result = ERROR_BUSY;
      } else { slot.installed = false; ++evidence->symbols[slot.api].restored; }
    }
  }
  const DWORD pages = RestorePages(evidence);
  if (!result) result = pages;
  for (DWORD i = 0; i < state.slot_count; ++i) if (state.slots[i].installed && !result) result = ERROR_BUSY;
  if (result && !state.restore_failure) state.restore_failure = result;
  return state.restore_failure;
}
}  // namespace

DWORD BeginLowboxIatTrace(LowboxIatTraceEvidence* evidence) noexcept {
  const DWORD last_error = GetLastError();
  if (!evidence) return ERROR_INVALID_PARAMETER;
  const DWORD expected = evidence->expected_target_session;
  *evidence = {}; evidence->expected_target_session = expected; evidence->attempted = 1;
  if (InterlockedCompareExchange(&state.used, 1, 0) != 0) {
    evidence->setup_status = ERROR_ALREADY_EXISTS; evidence->restore_status = ERROR_BUSY;
    SetLastError(last_error); return evidence->setup_status;
  }
  state.evidence = evidence; state.thread = GetCurrentThreadId(); evidence->worker_thread = state.thread;
  evidence->worker_session_status = ProcessIdToSessionId(GetCurrentProcessId(), &evidence->worker_session) ? ERROR_SUCCESS : Error();
  try {
    const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll"), ntdll = GetModuleHandleW(L"ntdll.dll");
    Need(kernelbase && ntdll, ERROR_MOD_NOT_FOUND);
    state.last_status = reinterpret_cast<LastNtStatus>(GetProcAddress(ntdll, "RtlGetLastNtStatus"));
    state.set_status = reinterpret_cast<SetNtStatus>(GetProcAddress(ntdll, "RtlSetLastWin32ErrorAndNtStatusFromNtStatus"));
    Need(state.last_status && state.set_status, ERROR_PROC_NOT_FOUND);
    // Verify preservation helpers before any slot can dispatch through us.
    const NTSTATUS original_status = state.last_status(); const DWORD original_error = GetLastError();
    state.set_status(original_status); SetLastError(original_error);
    Need(state.last_status() == original_status && GetLastError() == original_error, ERROR_INVALID_DATA);
    for (DWORD i = 0; i < kLowboxTraceSymbolCount; ++i) {
      state.originals[i] = reinterpret_cast<void*>(GetProcAddress(ntdll, kNames[i]));
      Need(state.originals[i] != nullptr, ERROR_PROC_NOT_FOUND);
    }
    InspectImage(kernelbase, evidence); Install(evidence);
    evidence->setup_status = ERROR_SUCCESS;
  } catch (const Failure& failure) { evidence->setup_status = failure.error; }
  catch (...) { evidence->setup_status = ERROR_NOT_ENOUGH_MEMORY; }
  if (evidence->setup_status != ERROR_SUCCESS) evidence->restore_status = Restore(evidence);
  SetLastError(last_error);
  return evidence->setup_status;
}

DWORD EndLowboxIatTrace(LowboxIatTraceEvidence* evidence) noexcept {
  const DWORD last_error = GetLastError();
  if (!evidence) return ERROR_INVALID_PARAMETER;
  if (state.evidence != evidence) {
    evidence->restore_status = ERROR_INVALID_PARAMETER;
  } else if (!state.ended) {
    evidence->restore_status = Restore(evidence);
    state.ended = true;
  }
  const DWORD result = evidence->restore_status;
  SetLastError(last_error); return result;
}

void PrintLowboxIatTrace(const LowboxIatTraceEvidence& e) noexcept {
  bool valid = e.version == 1 && e.attempted <= 1 && e.delay_imports_present <= 1 &&
      e.record_count <= kLowboxTraceRecordCount && e.slot_count <= kSlots &&
      e.page_count <= kPages && e.missing_symbols <= kLowboxTraceSymbolCount && e.record_count <= e.total_calls &&
      e.overflow == e.total_calls - e.record_count;
  for (const auto& symbol : e.symbols) valid = valid && symbol.imported <= kSlots && symbol.patched <= symbol.imported && symbol.restored <= symbol.patched;
  const DWORD first = valid ? e.total_calls - e.record_count : 0;
  for (DWORD i = 0; valid && i < e.record_count; ++i) {
    const auto& r = e.records[(first + i) % kLowboxTraceRecordCount];
    valid = r.sequence == first + i && static_cast<DWORD>(r.api) < kLowboxTraceSymbolCount &&
        r.name_characters < std::size(r.name) && r.name[r.name_characters] == 0 && r.name_truncated <= 1 &&
        r.returned_handle <= 1 && r.caller_rva < e.image_size;
  }
  std::printf("Protected lowbox IAT: attempted=%lu setup=%lu restore=%lu page-restore=%lu thread=%lu worker-session=%lu session-query=%lu expected-target-session=%lu slots=%lu pages=%lu calls=%lu records=%lu overflow=%lu missing=%lu uninspectable=%lu delay-imports=%lu\n",
      static_cast<unsigned long>(e.attempted), static_cast<unsigned long>(e.setup_status), static_cast<unsigned long>(e.restore_status),
      static_cast<unsigned long>(e.page_restore_status), static_cast<unsigned long>(e.worker_thread), static_cast<unsigned long>(e.worker_session),
      static_cast<unsigned long>(e.worker_session_status), static_cast<unsigned long>(e.expected_target_session), static_cast<unsigned long>(e.slot_count),
      static_cast<unsigned long>(e.page_count), static_cast<unsigned long>(e.total_calls), static_cast<unsigned long>(e.record_count),
      static_cast<unsigned long>(e.overflow), static_cast<unsigned long>(e.missing_symbols), static_cast<unsigned long>(e.uninspectable_imports),
      static_cast<unsigned long>(e.delay_imports_present));
  if (!valid) { std::puts("Protected lowbox IAT: invalid bounded evidence; coverage inconclusive"); return; }
  std::puts("Protected lowbox IAT: scope=one-complete-AuthorityLaunch::Create ordinary-KernelBase-imports-only, including intentional token-right denials; relative names retain raw root handles; missing records do not prove missing calls");
  for (DWORD i = 0; i < kLowboxTraceSymbolCount; ++i) {
    const auto& s = e.symbols[i];
    std::printf("Protected lowbox IAT: api=%s imported=%lu patched=%lu restored=%lu coverage-win32=%lu\n", kNames[i],
        static_cast<unsigned long>(s.imported), static_cast<unsigned long>(s.patched), static_cast<unsigned long>(s.restored), static_cast<unsigned long>(s.status));
  }
  for (DWORD i = 0; i < e.record_count; ++i) {
    const auto& r = e.records[(first + i) % kLowboxTraceRecordCount];
    std::printf("Protected lowbox IAT: seq=%lu api=%s ntstatus=0x%08lx access=0x%08lx open-as-self=%lu handle-attributes=0x%08lx object-attributes=0x%08lx flags=0x%08lx source=0x%llx root=0x%llx shadow=0x%llx result=0x%llx returned=%lu caller-rva=0x%08lx capabilities=%lu handles=%lu snapshot=%lu name-bytes=%lu copied=%lu truncated=%lu name=%ls\n",
        static_cast<unsigned long>(r.sequence), kNames[static_cast<DWORD>(r.api)], static_cast<unsigned long>(r.status),
        static_cast<unsigned long>(r.access), static_cast<unsigned long>(r.open_as_self), static_cast<unsigned long>(r.handle_attributes),
        static_cast<unsigned long>(r.object_attributes), static_cast<unsigned long>(r.flags), static_cast<unsigned long long>(r.source),
        static_cast<unsigned long long>(r.root), static_cast<unsigned long long>(r.shadow), static_cast<unsigned long long>(r.result),
        static_cast<unsigned long>(r.returned_handle), static_cast<unsigned long>(r.caller_rva), static_cast<unsigned long>(r.capability_count),
        static_cast<unsigned long>(r.handle_count), static_cast<unsigned long>(r.input_snapshot_status), static_cast<unsigned long>(r.name_bytes),
        static_cast<unsigned long>(r.name_characters), static_cast<unsigned long>(r.name_truncated), r.name);
  }
}
}  // namespace gesher::usb_storage::test
