// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Disposable-test-only observer: no production dependency or inline patches.
// Native ABIs: Microsoft's NtOpenThreadToken[Ex], NtOpenProcessToken and
// ZwOpenProcessTokenEx, NtOpenDirectoryObject, ZwCreateDirectoryObject,
// NtCreateLowBoxToken, ZwOpenSymbolicLinkObject, NtQuery/SetSecurityObject,
// ZwDuplicateObject, NtDuplicateToken, NtQuery/SetInformationToken and
// RtlNtStatusToDosError documentation. BOOLEAN is deliberately not Win32 BOOL;
// the status mapper returns ULONG (Win32), not NTSTATUS.
// The symbolic-link creation, Ex directory and RTL last-status declarations,
// and all new token/object declarations, are independently checked against
// the maintained primary phnt headers at:
// https://github.com/winsiderss/phnt/blob/53fbbdc5b5d2b08761db1c7b26bfa8c820924356/ntobapi.h
// https://github.com/winsiderss/phnt/blob/53fbbdc5b5d2b08761db1c7b26bfa8c820924356/ntrtl.h
// https://github.com/winsiderss/phnt/blob/53fbbdc5b5d2b08761db1c7b26bfa8c820924356/ntseapi.h
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
using CreateSymbolicLink = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PCUNICODE_STRING);
using QuerySecurity = NTSTATUS(NTAPI*)(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, ULONG, PULONG);
using SetSecurity = NTSTATUS(NTAPI*)(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR);
using DuplicateObject = NTSTATUS(NTAPI*)(HANDLE, HANDLE, HANDLE, PHANDLE, ACCESS_MASK, ULONG, ULONG);
using DuplicateToken = NTSTATUS(NTAPI*)(HANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, BOOLEAN, TOKEN_TYPE, PHANDLE);
using QueryTokenInformation = NTSTATUS(NTAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, PVOID, ULONG, PULONG);
using SetTokenInformation = NTSTATUS(NTAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, PVOID, ULONG);
using StatusToWin32 = ULONG(NTAPI*)(NTSTATUS);
using CreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using LastNtStatus = NTSTATUS(NTAPI*)();
using SetNtStatus = VOID(NTAPI*)(NTSTATUS);

constexpr DWORD kSlots = 32, kPages = 16;
constexpr const char* kNames[kLowboxTraceSymbolCount] = {
    "NtOpenThreadToken", "NtOpenThreadTokenEx", "NtOpenProcessToken", "NtOpenProcessTokenEx",
    "NtOpenDirectoryObject", "NtCreateDirectoryObject", "NtCreateDirectoryObjectEx", "NtCreateLowBoxToken",
    "NtCreateSymbolicLinkObject", "NtOpenSymbolicLinkObject", "NtQuerySecurityObject", "NtSetSecurityObject",
    "NtDuplicateObject", "NtDuplicateToken", "NtQueryInformationToken", "NtSetInformationToken",
    "RtlNtStatusToDosError", "NtCreateFile"};
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
// This POD-only copy makes no Windows call. Successful native query output is
// copied only within its supplied and returned sizes. SEH catches a concurrent
// unmap; Forward restores both captured error slots even after an exception.
bool RawSnapshot(void* destination, const void* source, SIZE_T size) noexcept {
  if (!size) return true;
  if (!source || size > 4096 || reinterpret_cast<ULONG_PTR>(source) >
      (std::numeric_limits<ULONG_PTR>::max)() - size) return false;
  __try { std::memcpy(destination, source, size); return true; }
  __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Input snapshots happen BEFORE forwarding. Check page accessibility as well
// as guarding the bounded copy. MinGW Clang requires -fms-extensions for SEH.
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
  return RawSnapshot(destination, source, size);
}
using SnapshotState = LowboxSnapshotState;

template <SIZE_T Capacity>
void ComponentSnapshot(const void* source, DWORD bytes, LowboxTraceBytes<Capacity>* output) noexcept {
  output->bytes = bytes;
  const DWORD copied = (std::min)(bytes, static_cast<DWORD>(Capacity));
  if (!Snapshot(output->data, source, copied)) { output->state = SnapshotState::unreadable; return; }
  output->copied = copied;
  output->state = copied == bytes ? SnapshotState::complete : SnapshotState::truncated;
}
void SidSnapshot(const void* source, LowboxTraceBytes<SECURITY_MAX_SID_SIZE>* output) noexcept {
  if (!source) { output->state = SnapshotState::absent; return; }
  struct Header { BYTE revision, count; SID_IDENTIFIER_AUTHORITY authority; } header{};
  static_assert(sizeof(Header) == 8);
  if (!Snapshot(&header, source, sizeof(header))) { output->state = SnapshotState::unreadable; return; }
  if (header.revision != SID_REVISION || header.count > SID_MAX_SUB_AUTHORITIES) {
    output->state = SnapshotState::invalid; return;
  }
  ComponentSnapshot(source, static_cast<DWORD>(sizeof(header)) + header.count * sizeof(DWORD), output);
}
template <SIZE_T Capacity>
void AclSnapshot(const void* source, bool present, LowboxTraceBytes<Capacity>* output) noexcept {
  if (!present) { output->state = SnapshotState::absent; return; }
  if (!source) { output->state = SnapshotState::null_acl; return; }
  ACL header{};
  if (!Snapshot(&header, source, sizeof(header))) { output->state = SnapshotState::unreadable; return; }
  if (header.AclSize < sizeof(header) ||
      (header.AclRevision != ACL_REVISION && header.AclRevision != ACL_REVISION_DS)) {
    output->state = SnapshotState::invalid; return;
  }
  ComponentSnapshot(source, header.AclSize, output);
}
bool RelativeComponent(const void* descriptor, DWORD offset, const void** output) noexcept {
  if (!offset) { *output = nullptr; return true; }
  const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(descriptor);
  if (offset < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || base > (std::numeric_limits<ULONG_PTR>::max)() - offset) return false;
  *output = reinterpret_cast<const void*>(base + offset);
  return true;
}
bool CompleteComponent(SnapshotState component_state) noexcept {
  return component_state == SnapshotState::complete || component_state == SnapshotState::absent || component_state == SnapshotState::null_acl;
}
void DescriptorSnapshot(const void* source, LowboxDirectorySecurity* output) noexcept {
  if (!source) { output->state = SnapshotState::absent; return; }
  output->descriptor_present = 1;
  struct Header { BYTE revision, reserved; SECURITY_DESCRIPTOR_CONTROL control; } header{};
  static_assert(sizeof(Header) == 4);
  if (!Snapshot(&header, source, sizeof(header))) { output->state = SnapshotState::unreadable; return; }
  output->revision = header.revision; output->control = header.control;
  if (header.revision != SECURITY_DESCRIPTOR_REVISION) { output->state = SnapshotState::invalid; return; }
  const void *owner = nullptr, *group = nullptr, *dacl = nullptr, *sacl = nullptr;
  if (header.control & SE_SELF_RELATIVE) {
    SECURITY_DESCRIPTOR_RELATIVE relative{};
    if (!Snapshot(&relative, source, sizeof(relative))) { output->state = SnapshotState::unreadable; return; }
    if (relative.Revision != header.revision || relative.Control != header.control) {
      output->state = SnapshotState::invalid; return;
    }
    if (!RelativeComponent(source, relative.Owner, &owner) || !RelativeComponent(source, relative.Group, &group) ||
        ((header.control & SE_DACL_PRESENT) && !RelativeComponent(source, relative.Dacl, &dacl)) ||
        ((header.control & SE_SACL_PRESENT) && !RelativeComponent(source, relative.Sacl, &sacl))) {
      output->state = SnapshotState::invalid; return;
    }
  } else {
    SECURITY_DESCRIPTOR absolute{};
    if (!Snapshot(&absolute, source, sizeof(absolute))) { output->state = SnapshotState::unreadable; return; }
    if (absolute.Revision != header.revision || absolute.Control != header.control) {
      output->state = SnapshotState::invalid; return;
    }
    owner = absolute.Owner; group = absolute.Group; dacl = absolute.Dacl; sacl = absolute.Sacl;
  }
  SidSnapshot(owner, &output->owner); SidSnapshot(group, &output->group);
  AclSnapshot(dacl, (header.control & SE_DACL_PRESENT) != 0, &output->dacl);
  AclSnapshot(sacl, (header.control & SE_SACL_PRESENT) != 0, &output->sacl);
  output->state = CompleteComponent(output->owner.state) && CompleteComponent(output->group.state) &&
      CompleteComponent(output->dacl.state) && CompleteComponent(output->sacl.state)
      ? SnapshotState::complete : SnapshotState::incomplete;
}
struct SecurityQueryOutput {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  ULONG length = 0;
  PULONG returned = nullptr;
};
void QuerySnapshot(const LowboxTraceCall& call, SecurityQueryOutput output, LowboxSecurityQuery* evidence) noexcept {
  const DWORD count = evidence->observations;
  *evidence = {};
  evidence->observations = count == MAXDWORD ? count : count + 1;
  evidence->sequence = call.sequence; evidence->source = call.source;
  evidence->information = call.security_information; evidence->native_status = call.status;
  evidence->buffer_length = output.length;
  auto& bytes = evidence->descriptor;
  // The native success contract writes returned length and descriptor bytes.
  // No API, token operation, handle duplication or name lookup runs here.
  if (!RawSnapshot(&evidence->returned_length, output.returned, sizeof(ULONG))) {
    bytes.state = SnapshotState::unreadable; return;
  }
  bytes.bytes = evidence->returned_length;
  if (bytes.bytes < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || bytes.bytes > output.length) {
    bytes.state = SnapshotState::invalid; return;
  }
  const DWORD copied = (std::min)(bytes.bytes, static_cast<DWORD>(sizeof(bytes.data)));
  if (!RawSnapshot(bytes.data, output.descriptor, copied)) { bytes.state = SnapshotState::unreadable; return; }
  bytes.copied = copied;
  bytes.state = copied == bytes.bytes ? SnapshotState::complete : SnapshotState::truncated;
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
template <SIZE_T Capacity>
void UnicodeSnapshot(PCUNICODE_STRING source, wchar_t (&text)[Capacity], DWORD* bytes,
    DWORD* copied_characters, DWORD* truncated, DWORD* status) noexcept {
  UNICODE_STRING name{};
  if (!Snapshot(&name, source, sizeof(name)) || (name.Length % sizeof(wchar_t)) ||
      name.Length > name.MaximumLength || (name.Length && !name.Buffer)) {
    *status = ERROR_INVALID_DATA; return;
  }
  *bytes = name.Length;
  const DWORD characters = name.Length / sizeof(wchar_t);
  const DWORD copied = (std::min)(characters, static_cast<DWORD>(Capacity - 1));
  if (!Snapshot(text, name.Buffer, copied * sizeof(wchar_t))) {
    *status = ERROR_NOACCESS; text[0] = 0; return;
  }
  text[copied] = 0;
  *copied_characters = copied;
  *truncated = copied != characters;
}
void ObjectSnapshot(LowboxTraceCall* call, POBJECT_ATTRIBUTES source,
    LowboxDirectorySecurity* security) noexcept {
  if (!source) {
    if (security) {
      security->state = SnapshotState::invalid;
      call->input_snapshot_status = ERROR_INVALID_PARAMETER;
    }
    return;
  }
  OBJECT_ATTRIBUTES object{};
  if (!Snapshot(&object, source, sizeof(object)) || object.Length != sizeof(object)) {
    if (security) security->state = SnapshotState::invalid;
    call->input_snapshot_status = ERROR_INVALID_DATA; return;
  }
  call->object_attributes = object.Attributes;
  call->root = reinterpret_cast<ULONG_PTR>(object.RootDirectory);
  if (security) {
    security->root = call->root;
    DescriptorSnapshot(object.SecurityDescriptor, security);
  }
  if (!object.ObjectName) return;
  UnicodeSnapshot(object.ObjectName, call->name, &call->name_bytes, &call->name_characters,
      &call->name_truncated, &call->input_snapshot_status);
}

template <class Result = NTSTATUS, class Invoke>
Result Forward(LowboxTraceApi api, LowboxTraceCall input, POBJECT_ATTRIBUTES object,
    PHANDLE output, void* caller, Invoke invoke, PCUNICODE_STRING target = nullptr,
    SecurityQueryOutput query = {}) noexcept {
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
  LowboxDirectorySecurity* security = nullptr;
  if (sequence != MAXDWORD) {
    ++evidence.total_calls;
    evidence.record_count = (std::min)(evidence.total_calls, kLowboxTraceRecordCount);
    evidence.overflow = evidence.total_calls - evidence.record_count;
    call = &evidence.records[sequence % kLowboxTraceRecordCount];
    *call = input; call->sequence = sequence; call->api = api;
    const ULONG_PTR address = reinterpret_cast<ULONG_PTR>(caller);
    if (address >= state.base && address - state.base < state.image_size)
      call->caller_rva = static_cast<DWORD>(address - state.base);
    if (api == LowboxTraceApi::create_directory_ex) {
      auto& selected = evidence.directory_security;
      if (selected.observations != MAXDWORD) ++selected.observations;
      if (selected.observations == 1) {
        security = &selected; security->sequence = sequence; security->shadow = input.shadow;
      }
    }
    ObjectSnapshot(call, object, security);
    if (api == LowboxTraceApi::create_symbolic_link) {
      UnicodeSnapshot(target, call->target, &call->target_bytes, &call->target_characters,
          &call->target_truncated, &call->target_snapshot_status);
    }
  }
  // Snapshot helpers may change both error slots. Restore both before passing
  // the exact original arguments, token context and access mask to ntdll.
  state.set_status(entry_status); SetLastError(entry_error);
  const Result result = invoke();
  const DWORD returned_error = GetLastError();
  // The TEB slot is independent of the API's NTSTATUS return. Preserve the
  // actual post-call slot, including when a guarded output copy takes SEH.
  const NTSTATUS returned_slot = state.last_status();
  if (call) {
    call->status = static_cast<DWORD>(result);
    // On NT_SUCCESS the native contract has just written this caller-owned
    // output slot. KernelBase retains its arguments until this return. Apart
    // from error-slot preservation, output observation makes no Windows call.
    if constexpr (std::is_same_v<Result, NTSTATUS>) {
      if (result >= 0 && output) {
        HANDLE value = nullptr; std::memcpy(&value, output, sizeof(value));
        call->result = reinterpret_cast<ULONG_PTR>(value);
        call->returned_handle = value && value != INVALID_HANDLE_VALUE;
      }
      if (security) {
        security->native_status = call->status;
        security->result = call->result; security->returned_handle = call->returned_handle;
      }
      if (result >= 0 && api == LowboxTraceApi::query_security &&
          call->security_information == DACL_SECURITY_INFORMATION && !evidence.directory_security.observations) {
        QuerySnapshot(*call, query, &evidence.security_query);
      }
    }
  }
  state.set_status(returned_slot); SetLastError(returned_error);
  observing = false;
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
TRACE_NOINLINE NTSTATUS NTAPI HookCreateSymbolicLink(PHANDLE out, ACCESS_MASK access,
    POBJECT_ATTRIBUTES object, PCUNICODE_STRING target) noexcept {
  LowboxTraceCall in{}; in.access = access;
  return Forward(LowboxTraceApi::create_symbolic_link, in, object, out, TRACE_CALLER(),
      [&] { return Original<CreateSymbolicLink>(LowboxTraceApi::create_symbolic_link)(out, access, object, target); }, target);
}
TRACE_NOINLINE NTSTATUS NTAPI HookOpenSymbolicLink(PHANDLE out, ACCESS_MASK access, POBJECT_ATTRIBUTES object) noexcept {
  LowboxTraceCall in{}; in.access = access;
  return Forward(LowboxTraceApi::open_symbolic_link, in, object, out, TRACE_CALLER(),
      [&] { return Original<Directory>(LowboxTraceApi::open_symbolic_link)(out, access, object); });
}
TRACE_NOINLINE NTSTATUS NTAPI HookQuerySecurity(HANDLE object, SECURITY_INFORMATION information,
    PSECURITY_DESCRIPTOR descriptor, ULONG length, PULONG needed) noexcept {
  LowboxTraceCall in{}; in.source = reinterpret_cast<ULONG_PTR>(object);
  in.security_information = information; in.buffer_length = length;
  return Forward(LowboxTraceApi::query_security, in, nullptr, nullptr, TRACE_CALLER(),
      [&] { return Original<QuerySecurity>(LowboxTraceApi::query_security)(object, information, descriptor, length, needed); },
      nullptr, {descriptor, length, needed});
}
TRACE_NOINLINE NTSTATUS NTAPI HookSetSecurity(HANDLE object, SECURITY_INFORMATION information,
    PSECURITY_DESCRIPTOR descriptor) noexcept {
  LowboxTraceCall in{}; in.source = reinterpret_cast<ULONG_PTR>(object); in.security_information = information;
  return Forward(LowboxTraceApi::set_security, in, nullptr, nullptr, TRACE_CALLER(),
      [&] { return Original<SetSecurity>(LowboxTraceApi::set_security)(object, information, descriptor); });
}
TRACE_NOINLINE NTSTATUS NTAPI HookDuplicateObject(HANDLE source_process, HANDLE source, HANDLE target_process,
    PHANDLE out, ACCESS_MASK access, ULONG attributes, ULONG options) noexcept {
  LowboxTraceCall in{}; in.source = reinterpret_cast<ULONG_PTR>(source); in.access = access;
  in.source_process = reinterpret_cast<ULONG_PTR>(source_process);
  in.target_process = reinterpret_cast<ULONG_PTR>(target_process);
  in.handle_attributes = attributes; in.flags = options;
  // With DUPLICATE_CLOSE_SOURCE and no target process, no target handle is
  // produced. Forward the caller's exact out pointer but do not read it.
  return Forward(LowboxTraceApi::duplicate_object, in, nullptr, target_process ? out : nullptr, TRACE_CALLER(),
      [&] { return Original<DuplicateObject>(LowboxTraceApi::duplicate_object)(source_process, source,
          target_process, out, access, attributes, options); });
}
TRACE_NOINLINE NTSTATUS NTAPI HookDuplicateToken(HANDLE source, ACCESS_MASK access,
    POBJECT_ATTRIBUTES object, BOOLEAN effective_only, TOKEN_TYPE type, PHANDLE out) noexcept {
  LowboxTraceCall in{}; in.source = reinterpret_cast<ULONG_PTR>(source); in.access = access;
  in.effective_only = effective_only; in.token_type = static_cast<DWORD>(type);
  return Forward(LowboxTraceApi::duplicate_token, in, object, out, TRACE_CALLER(),
      [&] { return Original<DuplicateToken>(LowboxTraceApi::duplicate_token)(source, access, object, effective_only, type, out); });
}
TRACE_NOINLINE NTSTATUS NTAPI HookQueryTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS information,
    PVOID buffer, ULONG length, PULONG returned) noexcept {
  LowboxTraceCall in{}; in.source = reinterpret_cast<ULONG_PTR>(token);
  in.information_class = static_cast<DWORD>(information); in.buffer_length = length;
  return Forward(LowboxTraceApi::query_token_information, in, nullptr, nullptr, TRACE_CALLER(),
      [&] { return Original<QueryTokenInformation>(LowboxTraceApi::query_token_information)(token, information, buffer, length, returned); });
}
TRACE_NOINLINE NTSTATUS NTAPI HookSetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS information,
    PVOID buffer, ULONG length) noexcept {
  LowboxTraceCall in{}; in.source = reinterpret_cast<ULONG_PTR>(token);
  in.information_class = static_cast<DWORD>(information); in.buffer_length = length;
  return Forward(LowboxTraceApi::set_token_information, in, nullptr, nullptr, TRACE_CALLER(),
      [&] { return Original<SetTokenInformation>(LowboxTraceApi::set_token_information)(token, information, buffer, length); });
}
TRACE_NOINLINE ULONG NTAPI HookStatusToWin32(NTSTATUS status) noexcept {
  LowboxTraceCall in{}; in.mapped_ntstatus = static_cast<DWORD>(status);
  // This routine's return is a Win32 error number, not an NTSTATUS and not a
  // native-success indicator. Preserve any last-NTSTATUS change it makes.
  return Forward<ULONG>(LowboxTraceApi::status_to_win32, in, nullptr, nullptr, TRACE_CALLER(),
      [&] { return Original<StatusToWin32>(LowboxTraceApi::status_to_win32)(status); });
}
TRACE_NOINLINE NTSTATUS NTAPI HookCreateFile(PHANDLE file, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes,
    PIO_STATUS_BLOCK iosb, PLARGE_INTEGER allocation, ULONG file_attributes, ULONG share, ULONG disposition,
    ULONG options, PVOID ea, ULONG ea_length) noexcept {
  LowboxTraceCall in{}; in.access = access;
  in.flags = disposition;
  in.file_attributes = file_attributes; in.file_share = share;
  in.file_options = options; in.file_ea_length = ea_length;
  return Forward(LowboxTraceApi::create_file, in, attributes, file, TRACE_CALLER(), [&] {
    return Original<CreateFileFn>(LowboxTraceApi::create_file)(file, access, attributes, iosb,
        allocation, file_attributes, share, disposition, options, ea, ea_length);
  });
}
static_assert(std::is_convertible_v<decltype(&HookCreateSymbolicLink), CreateSymbolicLink> &&
              std::is_convertible_v<decltype(&HookOpenSymbolicLink), Directory> &&
              std::is_convertible_v<decltype(&HookQuerySecurity), QuerySecurity> &&
              std::is_convertible_v<decltype(&HookSetSecurity), SetSecurity> &&
              std::is_convertible_v<decltype(&HookDuplicateObject), DuplicateObject> &&
              std::is_convertible_v<decltype(&HookDuplicateToken), DuplicateToken> &&
              std::is_convertible_v<decltype(&HookQueryTokenInformation), QueryTokenInformation> &&
              std::is_convertible_v<decltype(&HookSetTokenInformation), SetTokenInformation> &&
              std::is_convertible_v<decltype(&HookStatusToWin32), StatusToWin32>);
void* Hook(DWORD api) {
  void* hooks[kLowboxTraceSymbolCount] = {
      reinterpret_cast<void*>(&HookOpenThread), reinterpret_cast<void*>(&HookOpenThreadEx),
      reinterpret_cast<void*>(&HookOpenProcess), reinterpret_cast<void*>(&HookOpenProcessEx),
      reinterpret_cast<void*>(&HookOpenDirectory), reinterpret_cast<void*>(&HookCreateDirectory),
      reinterpret_cast<void*>(&HookCreateDirectoryEx), reinterpret_cast<void*>(&HookCreateLowbox),
      reinterpret_cast<void*>(&HookCreateSymbolicLink), reinterpret_cast<void*>(&HookOpenSymbolicLink),
      reinterpret_cast<void*>(&HookQuerySecurity), reinterpret_cast<void*>(&HookSetSecurity),
      reinterpret_cast<void*>(&HookDuplicateObject), reinterpret_cast<void*>(&HookDuplicateToken),
      reinterpret_cast<void*>(&HookQueryTokenInformation), reinterpret_cast<void*>(&HookSetTokenInformation),
      reinterpret_cast<void*>(&HookStatusToWin32), reinterpret_cast<void*>(&HookCreateFile)};
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
  evidence->image_timestamp = nt.FileHeader.TimeDateStamp;
  evidence->image_checksum = nt.OptionalHeader.CheckSum;
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
const char* SnapshotName(SnapshotState value) noexcept {
  switch (value) {
    case SnapshotState::not_observed: return "not-observed";
    case SnapshotState::not_requested: return "not-requested";
    case SnapshotState::absent: return "absent";
    case SnapshotState::null_acl: return "present-null-acl";
    case SnapshotState::complete: return "complete";
    case SnapshotState::unreadable: return "unreadable";
    case SnapshotState::invalid: return "invalid";
    case SnapshotState::truncated: return "truncated";
    case SnapshotState::incomplete: return "incomplete";
  }
  return "invalid-state";
}
template <SIZE_T Capacity>
bool ValidBytes(const LowboxTraceBytes<Capacity>& value) noexcept {
  if (value.copied > Capacity || value.copied > value.bytes) return false;
  switch (value.state) {
    case SnapshotState::not_observed:
    case SnapshotState::not_requested:
    case SnapshotState::absent:
    case SnapshotState::null_acl: return !value.bytes && !value.copied;
    case SnapshotState::complete: return value.bytes && value.copied == value.bytes;
    case SnapshotState::unreadable:
    case SnapshotState::invalid: return !value.copied;
    case SnapshotState::truncated: return value.bytes > Capacity && value.copied == Capacity;
    case SnapshotState::incomplete: return false;
  }
  return false;
}
bool ValidSecurity(const LowboxIatTraceEvidence& evidence) noexcept {
  const auto& input = evidence.directory_security;
  const auto& query = evidence.security_query;
  return static_cast<DWORD>(input.state) <= static_cast<DWORD>(SnapshotState::incomplete) &&
      input.descriptor_present <= 1 && input.returned_handle <= 1 && input.observations <= evidence.total_calls &&
      (!input.observations || input.sequence < evidence.total_calls) &&
      ValidBytes(input.owner) && ValidBytes(input.group) && ValidBytes(input.dacl) && ValidBytes(input.sacl) &&
      query.observations <= evidence.total_calls && ValidBytes(query.descriptor) &&
      (!query.observations || (query.sequence < evidence.total_calls && query.information == DACL_SECURITY_INFORMATION &&
          static_cast<NTSTATUS>(query.native_status) >= 0 && (!input.observations || query.sequence < input.sequence)));
}
template <SIZE_T Capacity>
void PrintBytes(const char* component, const LowboxTraceBytes<Capacity>& value) noexcept {
  std::printf("Protected lowbox security: component=%s state=%s bytes=%lu copied=%lu capacity=%zu hex=",
      component, SnapshotName(value.state), static_cast<unsigned long>(value.bytes),
      static_cast<unsigned long>(value.copied), Capacity);
  for (DWORD i = 0; i < value.copied; ++i) std::printf("%02x", static_cast<unsigned>(value.data[i]));
  std::putchar('\n');
}
void PrintSid(const char* component, const BYTE* bytes, DWORD size) noexcept {
  if (size < 8 || bytes[0] != SID_REVISION || bytes[1] > SID_MAX_SUB_AUTHORITIES ||
      size != 8 + static_cast<DWORD>(bytes[1]) * sizeof(DWORD)) return;
  unsigned long long authority = 0;
  for (DWORD i = 2; i < 8; ++i) authority = (authority << 8) | bytes[i];
  std::printf("Protected lowbox security: component=%s sid=S-%u-%llu", component,
      static_cast<unsigned>(bytes[0]), authority);
  for (DWORD i = 0; i < bytes[1]; ++i) {
    DWORD sub = 0; std::memcpy(&sub, bytes + 8 + i * sizeof(DWORD), sizeof(sub));
    std::printf("-%lu", static_cast<unsigned long>(sub));
  }
  std::putchar('\n');
}
void PrintAcl(const char* component, const BYTE* bytes, DWORD size) noexcept {
  ACL acl{};
  if (size < sizeof(acl)) return;
  std::memcpy(&acl, bytes, sizeof(acl));
  bool valid = acl.AclSize == size && (acl.AclRevision == ACL_REVISION || acl.AclRevision == ACL_REVISION_DS);
  DWORD cursor = sizeof(acl);
  for (DWORD i = 0; valid && i < acl.AceCount; ++i) {
    ACE_HEADER ace{};
    if (cursor > size || size - cursor < sizeof(ace)) { valid = false; break; }
    std::memcpy(&ace, bytes + cursor, sizeof(ace));
    if (ace.AceSize < sizeof(ace) || ace.AceSize > size - cursor) { valid = false; break; }
    std::printf("Protected lowbox security: component=%s ace=%lu type=%u flags=0x%02x bytes=%u",
        component, static_cast<unsigned long>(i), static_cast<unsigned>(ace.AceType),
        static_cast<unsigned>(ace.AceFlags), static_cast<unsigned>(ace.AceSize));
    const bool simple = ace.AceType == ACCESS_ALLOWED_ACE_TYPE || ace.AceType == ACCESS_DENIED_ACE_TYPE ||
        ace.AceType == SYSTEM_AUDIT_ACE_TYPE || ace.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE;
    if (simple && ace.AceSize >= 8) {
      DWORD mask = 0; std::memcpy(&mask, bytes + cursor + sizeof(ace), sizeof(mask));
      std::printf(" mask=0x%08lx\n", static_cast<unsigned long>(mask));
      PrintSid(component, bytes + cursor + 8, ace.AceSize - 8);
    } else std::putchar('\n');
    cursor += ace.AceSize;
  }
  std::printf("Protected lowbox security: component=%s acl-structural-validation=%s revision=%u ace-count=%u\n",
      component, valid ? "complete" : "invalid", static_cast<unsigned>(acl.AclRevision), static_cast<unsigned>(acl.AceCount));
}
void PrintSecurity(const LowboxIatTraceEvidence& evidence) noexcept {
  const auto& input = evidence.directory_security;
  const auto& query = evidence.security_query;
  std::puts("Protected lowbox security: scope=first-DirectoryEx-input-and-last-prior-successful-DACL-only-query; input is not final inherited/object security; no handle is retained");
  std::printf("Protected lowbox security: DirectoryEx-calls=%lu input-seq=%lu state=%s descriptor-present=%lu revision=%lu control=0x%04lx root=0x%llx shadow=0x%llx native-ntstatus=0x%08lx returned=%lu result=0x%llx\n",
      static_cast<unsigned long>(input.observations), static_cast<unsigned long>(input.sequence), SnapshotName(input.state),
      static_cast<unsigned long>(input.descriptor_present), static_cast<unsigned long>(input.revision),
      static_cast<unsigned long>(input.control), static_cast<unsigned long long>(input.root),
      static_cast<unsigned long long>(input.shadow), static_cast<unsigned long>(input.native_status),
      static_cast<unsigned long>(input.returned_handle), static_cast<unsigned long long>(input.result));
  PrintBytes("input-owner", input.owner); PrintBytes("input-group", input.group);
  PrintBytes("input-dacl", input.dacl); PrintBytes("input-sacl", input.sacl);
  if (input.owner.state == SnapshotState::complete) PrintSid("input-owner", input.owner.data, input.owner.copied);
  if (input.group.state == SnapshotState::complete) PrintSid("input-group", input.group.data, input.group.copied);
  if (input.dacl.state == SnapshotState::complete) PrintAcl("input-dacl", input.dacl.data, input.dacl.copied);
  if (input.sacl.state == SnapshotState::complete) PrintAcl("input-sacl", input.sacl.data, input.sacl.copied);
  const bool correlation = input.observations && query.observations && query.source == input.shadow;
  std::printf("Protected lowbox security: query-observations=%lu query-seq=%lu source=0x%llx information=0x%08lx buffer-length=%lu returned-length=%lu native-ntstatus=0x%08lx shadow-numeric-match=%u owner=not-requested group=not-requested sacl-label=not-requested\n",
      static_cast<unsigned long>(query.observations), static_cast<unsigned long>(query.sequence),
      static_cast<unsigned long long>(query.source), static_cast<unsigned long>(query.information),
      static_cast<unsigned long>(query.buffer_length), static_cast<unsigned long>(query.returned_length),
      static_cast<unsigned long>(query.native_status), static_cast<unsigned>(correlation));
  PrintBytes("queried-descriptor", query.descriptor);
  // Decode only the requested DACL, from the captured bounded self-relative
  // buffer, after all import slots have been restored. Other components were
  // not requested; zero offsets do not establish absent owner/group/label.
  if (query.descriptor.state != SnapshotState::complete) return;
  SECURITY_DESCRIPTOR_RELATIVE descriptor{};
  if (query.descriptor.copied < sizeof(descriptor)) return;
  std::memcpy(&descriptor, query.descriptor.data, sizeof(descriptor));
  if (descriptor.Revision != SECURITY_DESCRIPTOR_REVISION || !(descriptor.Control & SE_SELF_RELATIVE)) {
    std::puts("Protected lowbox security: queried-dacl state=invalid-descriptor"); return;
  }
  if (!(descriptor.Control & SE_DACL_PRESENT) || !descriptor.Dacl) {
    std::printf("Protected lowbox security: queried-dacl state=%s control=0x%04x\n",
        descriptor.Control & SE_DACL_PRESENT ? "present-null-acl" : "absent", static_cast<unsigned>(descriptor.Control));
    return;
  }
  const DWORD size = query.descriptor.copied;
  const DWORD offset = descriptor.Dacl;
  ACL acl{};
  if (offset < sizeof(descriptor) || offset > size || size - offset < sizeof(acl)) {
    std::puts("Protected lowbox security: queried-dacl state=invalid-offset"); return;
  }
  std::memcpy(&acl, query.descriptor.data + offset, sizeof(acl));
  if (acl.AclSize < sizeof(acl) || acl.AclSize > size - offset) {
    std::puts("Protected lowbox security: queried-dacl state=invalid-size"); return;
  }
  PrintAcl("queried-dacl", query.descriptor.data + offset, acl.AclSize);
  if (correlation && input.dacl.state == SnapshotState::complete) {
    const bool same = input.dacl.copied == acl.AclSize &&
        std::memcmp(input.dacl.data, query.descriptor.data + offset, acl.AclSize) == 0;
    std::printf("Protected lowbox security: input-versus-queried-dacl-byte-equal=%u source-causality=not-established\n", static_cast<unsigned>(same));
  }
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
  bool valid = e.version == 3 && e.attempted <= 1 && e.delay_imports_present <= 1 &&
      e.record_count <= kLowboxTraceRecordCount && e.slot_count <= kSlots &&
      e.page_count <= kPages && e.missing_symbols <= kLowboxTraceSymbolCount && e.record_count <= e.total_calls &&
      e.overflow == e.total_calls - e.record_count && ValidSecurity(e);
  for (const auto& symbol : e.symbols) valid = valid && symbol.imported <= kSlots && symbol.patched <= symbol.imported && symbol.restored <= symbol.patched;
  const DWORD first = valid ? e.total_calls - e.record_count : 0;
  for (DWORD i = 0; valid && i < e.record_count; ++i) {
    const auto& r = e.records[(first + i) % kLowboxTraceRecordCount];
    valid = r.sequence == first + i && static_cast<DWORD>(r.api) < kLowboxTraceSymbolCount &&
        r.name_characters < std::size(r.name) && r.name[r.name_characters] == 0 && r.name_truncated <= 1 &&
        r.target_characters < std::size(r.target) && r.target[r.target_characters] == 0 && r.target_truncated <= 1 &&
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
  std::printf("Protected lowbox IAT: version=%lu module=KernelBase.dll mapped-pe-timestamp=0x%08lx mapped-pe-checksum=0x%08lx mapped-image-size=0x%08lx\n",
      static_cast<unsigned long>(e.version), static_cast<unsigned long>(e.image_timestamp),
      static_cast<unsigned long>(e.image_checksum), static_cast<unsigned long>(e.image_size));
  std::puts("Protected lowbox IAT: scope=one-complete-AuthorityLaunch::Create ordinary-KernelBase-imports-only, including intentional token-right denials; relative names retain raw root handles; missing records do not prove missing calls");
  std::puts("Protected lowbox IAT: names and link targets have separate byte lengths/truncation flags; truncated text is not exact identity; numeric handle values can be reused after close");
  std::printf("Protected lowbox IAT: name-capacity=%zu link-target-capacity=%zu records-capacity=%lu evidence-bytes=%zu\n",
      std::size(e.records[0].name), std::size(e.records[0].target),
      static_cast<unsigned long>(kLowboxTraceRecordCount), sizeof(e));
  for (DWORD i = 0; i < kLowboxTraceSymbolCount; ++i) {
    const auto& s = e.symbols[i];
    std::printf("Protected lowbox IAT: api=%s imported=%lu patched=%lu restored=%lu coverage-win32=%lu\n", kNames[i],
        static_cast<unsigned long>(s.imported), static_cast<unsigned long>(s.patched), static_cast<unsigned long>(s.restored), static_cast<unsigned long>(s.status));
  }
  for (DWORD i = 0; i < e.record_count; ++i) {
    const auto& r = e.records[(first + i) % kLowboxTraceRecordCount];
    const bool win32 = r.api == LowboxTraceApi::status_to_win32;
    std::printf("Protected lowbox IAT: seq=%lu api=%s return-domain=%s %s=0x%08lx mapped-input-ntstatus=0x%08lx access=0x%08lx open-as-self=%lu handle-attributes=0x%08lx object-attributes=0x%08lx flags=0x%08lx source=0x%llx source-process=0x%llx target-process=0x%llx root=0x%llx shadow=0x%llx result=0x%llx returned=%lu caller-rva=0x%08lx capabilities=%lu handles=%lu information-class=%lu buffer-length=%lu security-information=0x%08lx token-type=%lu effective-only=%lu snapshot=%lu name-bytes=%lu copied=%lu truncated=%lu name=%ls target-snapshot=%lu target-bytes=%lu target-copied=%lu target-truncated=%lu target=%ls\n",
        static_cast<unsigned long>(r.sequence), kNames[static_cast<DWORD>(r.api)], win32 ? "win32" : "ntstatus",
        win32 ? "win32" : "ntstatus", static_cast<unsigned long>(r.status), static_cast<unsigned long>(r.mapped_ntstatus),
        static_cast<unsigned long>(r.access), static_cast<unsigned long>(r.open_as_self), static_cast<unsigned long>(r.handle_attributes),
        static_cast<unsigned long>(r.object_attributes), static_cast<unsigned long>(r.flags), static_cast<unsigned long long>(r.source),
        static_cast<unsigned long long>(r.source_process), static_cast<unsigned long long>(r.target_process),
        static_cast<unsigned long long>(r.root), static_cast<unsigned long long>(r.shadow), static_cast<unsigned long long>(r.result),
        static_cast<unsigned long>(r.returned_handle), static_cast<unsigned long>(r.caller_rva), static_cast<unsigned long>(r.capability_count),
        static_cast<unsigned long>(r.handle_count), static_cast<unsigned long>(r.information_class), static_cast<unsigned long>(r.buffer_length),
        static_cast<unsigned long>(r.security_information), static_cast<unsigned long>(r.token_type), static_cast<unsigned long>(r.effective_only),
        static_cast<unsigned long>(r.input_snapshot_status), static_cast<unsigned long>(r.name_bytes),
        static_cast<unsigned long>(r.name_characters), static_cast<unsigned long>(r.name_truncated), r.name,
        static_cast<unsigned long>(r.target_snapshot_status), static_cast<unsigned long>(r.target_bytes),
        static_cast<unsigned long>(r.target_characters), static_cast<unsigned long>(r.target_truncated), r.target);
    if (r.api == LowboxTraceApi::create_file) {
      std::printf("Protected lowbox IAT file: seq=%lu attributes=0x%08lx share=0x%08lx disposition=%lu options=0x%08lx ea-length=%lu\n",
          static_cast<unsigned long>(r.sequence), static_cast<unsigned long>(r.file_attributes),
          static_cast<unsigned long>(r.file_share), static_cast<unsigned long>(r.flags),
          static_cast<unsigned long>(r.file_options), static_cast<unsigned long>(r.file_ea_length));
    }
  }
  PrintSecurity(e);
}
}  // namespace gesher::usb_storage::test
