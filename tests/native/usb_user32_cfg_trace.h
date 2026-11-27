// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gesher::usb_storage::test {
// Disposable diagnostic only. Observe actual basic-block decisions and call
// returns; never change RIP, input registers, results or production permissions.
class User32CfgDebugTrace {
 public:
  User32CfgDebugTrace(HANDLE process, DWORD thread, std::span<char> tail, DWORD* bytes,
                      const std::wstring& metadata, ULONGLONG deadline = 0, bool gdi = false)
      : process_(process), thread_(thread), tail_(tail), bytes_(bytes),
        deadline_(deadline ? deadline : GetTickCount64() + 5000), gdi_(gdi) { Load(metadata); }
  ~User32CfgDebugTrace() {
    const DWORD cleanup = Clear();
    char line[256]{};
    int length = std::snprintf(line, sizeof(line),
        "%s CFG: setup=%lu error=%lu cleanup=%lu events=%lu finished=%lu raw-return=0x%08lx\n",
        gdi_ ? "GDI32FULL" : "USER32", setup_, error_, cleanup, events_, finished_ ? 1ul : 0ul, final_);
    Append(std::string_view(line, static_cast<size_t>(length)));
    const size_t first_printed = gdi_ && used_ > 8 ? used_ - 8 : 0;
    for (size_t n = first_printed; n < used_; ++n) {
      const auto& r = records_[(first_ + n) % records_.size()];
      length = gdi_ ? std::snprintf(line, sizeof(line),
          "GDI32FULL CFG: kind=%lu rva=0x%08lx target=0x%08lx rax=0x%016llx eflags=0x%08lx taken=%lu\n",
          r.kind, r.rva, r.target, r.rax, r.flags, r.taken)
          : std::snprintf(line, sizeof(line),
          "USER32 CFG: kind=%lu rva=0x%08lx target=0x%08lx eax=0x%08lx eflags=0x%08lx taken=%lu\n",
          r.kind, r.rva, r.target, static_cast<DWORD>(r.rax), r.flags, r.taken);
      Append(std::string_view(line, static_cast<size_t>(length)));
    }
  }
  void InitialBreakpoint() {}
  bool EntryMarker(const OUTPUT_DEBUG_STRING_INFO& info) {
    constexpr char marker[] = "GESHER_LOADER_ENTRY";
    if (info.fUnicode || info.nDebugStringLength != sizeof(marker)) return false;
    std::array<char, sizeof(marker)> actual{};
    if (!Read(reinterpret_cast<uintptr_t>(info.lpDebugStringData), std::as_writable_bytes(std::span(actual))) ||
        std::memcmp(actual.data(), marker, actual.size())) return false;
    ready_ = true; return true;
  }
  void Loaded(const DEBUG_EVENT& event) {
    if (setup_ != ERROR_NOT_READY || event.dwDebugEventCode != LOAD_DLL_DEBUG_EVENT ||
        !event.u.LoadDll.hFile || event.u.LoadDll.hFile == INVALID_HANDLE_VALUE) return;
    std::array<wchar_t, 1024> name{}, system{};
    DWORD size = GetFinalPathNameByHandleW(event.u.LoadDll.hFile, name.data(), static_cast<DWORD>(name.size()), 0);
    if (!size || size >= name.size()) return;
    std::wstring path(name.data(), size);
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    size = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (!size || size >= system.size()) return;
    const std::wstring expected = std::wstring(system.data(), size) +
        (gdi_ ? L"\\gdi32full.dll" : L"\\user32.dll");
    if (CompareStringOrdinal(path.c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL) return;
    setup_ = ERROR_INVALID_DATA;
    if (GetTickCount64() >= deadline_) { setup_ = ERROR_TIMEOUT; return; }
    if (!ready_) { setup_ = ERROR_NOT_READY; return; }
    HANDLE file = CreateFileW(expected.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { setup_ = GetLastError(); return; }
    BY_HANDLE_FILE_INFORMATION a{}, b{};
    bool same = GetFileInformationByHandle(file, &a) && GetFileInformationByHandle(event.u.LoadDll.hFile, &b) &&
        a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
        a.nFileIndexLow == b.nFileIndexLow && !(a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    std::array<BYTE, 32> digest{};
    bool hashed = same && VerifyImageHeader(file) && Hash(file, &digest, deadline_); CloseHandle(file);
    if (GetTickCount64() >= deadline_) { setup_ = ERROR_TIMEOUT; return; }
    if (!hashed || digest != header_.digest) { setup_ = ERROR_CRC; return; }
    base_ = reinterpret_cast<uintptr_t>(event.u.LoadDll.lpBaseOfDll);
    if (gdi_ && !VerifyGdiRoot()) { setup_ = ERROR_INVALID_DATA; return; }
    for (const auto& row : rows_) {
      if (GetTickCount64() >= deadline_) { setup_ = ERROR_TIMEOUT; return; }
      std::array<std::byte, 16> bytes{};
      if (!Read(base_ + row.rva, std::span(bytes).first(row.length)) ||
          std::memcmp(bytes.data(), row.code.data(), row.length)) { setup_ = ERROR_CRC; return; }
    }
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, thread_);
    if (!thread) { setup_ = GetLastError(); return; }
    CONTEXT context{}; context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(thread, &context) || (context.Dr7 & 0xff)) setup_ = ERROR_BUSY;
    else {
      context.Dr0 = base_ + header_.entry; context.Dr1 = 0; context.Dr6 = 0;
      context.Dr7 = 1; action_ = Action::kEntry;
      armed_ = SetThreadContext(thread, &context) != FALSE;
      setup_ = armed_ ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(thread);
  }
  bool Exception(const DEBUG_EVENT& event) {
    if (setup_ != ERROR_SUCCESS || finished_ || event.dwThreadId != thread_ ||
        event.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_SINGLE_STEP) return false;
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, thread_);
    if (!thread) { error_ = GetLastError(); return false; }
    CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_DEBUG_REGISTERS;
    bool handled = false;
    if (GetThreadContext(thread, &context)) {
      if ((context.Dr6 & 2) && context.Rip == root_return_) {
        // Every nested DLL initializer returns through this same ntdll caller.
        // Match the original stack frame before treating a return as USER32's.
        handled = true;
        if (++events_ > header_.budget || GetTickCount64() >= deadline_) { error_ = ERROR_TIMEOUT; Finish(context); }
        else if (context.Rsp == root_stack_ + sizeof(uintptr_t)) {
          final_ = static_cast<DWORD>(context.Rax); finished_ = true;
          context.Dr0 = 0; context.Dr1 = 0; context.Dr7 &= ~DWORD64{0xf};
        }
      } else if ((context.Dr6 & 1) && context.Rip == context.Dr0) {
        handled = true;
        if (++events_ > header_.budget || GetTickCount64() >= deadline_) { error_ = ERROR_TIMEOUT; Finish(context); }
        else if (action_ == Action::kEntry) {
          if (context.Rdx != DLL_PROCESS_ATTACH || !Read(context.Rsp,
              std::as_writable_bytes(std::span(&root_return_, 1))) || !root_return_) {
            error_ = ERROR_INVALID_DATA; Finish(context);
          } else {
            root_stack_ = context.Rsp;
            context.Dr1 = root_return_; context.Dr7 |= 4;
            Arm(context, header_.entry, true);
          }
        } else {
          if (action_ == Action::kCallReturn) Record(1, pending_.rva, pending_.target, context, 0);
          const uint64_t offset = context.Rip - base_;
          if (offset >= header_.image_size) { error_ = ERROR_INVALID_ADDRESS; Finish(context); }
          else Arm(context, static_cast<DWORD>(offset), true);
        }
      }
      if (handled) {
        context.Dr6 = 0; context.EFlags |= 0x10000;
        if (!SetThreadContext(thread, &context)) { error_ = GetLastError(); handled = false; }
      }
    } else error_ = GetLastError();
    CloseHandle(thread); return handled;
  }
 private:
  struct Header { DWORD magic, version, image_size, entry, count, reserved; std::array<BYTE, 32> digest; DWORD row_size, budget; std::array<BYTE, 32> table_digest; } header_{};
  struct Row { DWORD rva, target; BYTE length, kind, condition, reserved; std::array<BYTE, 16> code; DWORD end; };
  static_assert(sizeof(Header) == 96 && sizeof(Row) == 32);
  struct Observation { DWORD kind, rva, target; unsigned long long rax; DWORD flags, taken; };
  enum class Action { kEntry, kTerminator, kCallReturn } action_ = Action::kEntry;
  template<class T> static std::span<std::byte> Bytes(T& item) { return std::as_writable_bytes(std::span(&item, 1)); }
  bool VerifyImageHeader(HANDLE file) const {
    const auto at = [&](DWORD offset, std::span<std::byte> output) {
      LARGE_INTEGER position{}; position.QuadPart = offset;
      DWORD got = 0;
      return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
          ReadFile(file, output.data(), static_cast<DWORD>(output.size()), &got, nullptr) && got == output.size();
    };
    std::array<WORD, 1> magic{}; DWORD nt = 0, signature = 0, entry = 0, image_size = 0;
    WORD machine = 0, optional = 0;
    bool ok = at(0, std::as_writable_bytes(std::span(magic))) && magic[0] == IMAGE_DOS_SIGNATURE &&
        at(60, Bytes(nt)) && nt <= 32 * 1024 * 1024 - 128 &&
        at(nt, Bytes(signature)) && signature == IMAGE_NT_SIGNATURE &&
        at(nt + 4, Bytes(machine)) && machine == IMAGE_FILE_MACHINE_AMD64 &&
        at(nt + 24, Bytes(optional)) && optional == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
        at(nt + 24 + 16, Bytes(entry)) && entry == (gdi_ ? header_.reserved : header_.entry) &&
        at(nt + 24 + 56, Bytes(image_size)) && image_size == header_.image_size;
    LARGE_INTEGER zero{};
    return SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) && ok;
  }
  bool ReadImage(DWORD rva, std::span<std::byte> bytes) const {
    return rva < header_.image_size && bytes.size() <= header_.image_size - rva &&
        GetTickCount64() < deadline_ && Read(base_ + rva, bytes);
  }
  bool VerifyGdiRoot() const {
    DWORD nt = 0;
    IMAGE_NT_HEADERS64 pe{};
    if (!ReadImage(60, Bytes(nt)) || !ReadImage(nt, Bytes(pe)) ||
        pe.Signature != IMAGE_NT_SIGNATURE || pe.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        pe.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        pe.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION ||
        pe.OptionalHeader.SizeOfImage != header_.image_size ||
        pe.OptionalHeader.AddressOfEntryPoint != header_.reserved) return false;
    const auto& exports = pe.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const auto& exceptions = pe.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    IMAGE_EXPORT_DIRECTORY table{};
    if (exports.VirtualAddress >= header_.image_size ||
        exports.Size > header_.image_size - exports.VirtualAddress || exports.Size < sizeof(table) ||
        !ReadImage(exports.VirtualAddress, Bytes(table)) ||
        !table.NumberOfFunctions || table.NumberOfFunctions > 8192 ||
        table.NumberOfNames > table.NumberOfFunctions) return false;
    std::vector<DWORD> names(table.NumberOfNames), functions(table.NumberOfFunctions);
    std::vector<WORD> ordinals(table.NumberOfNames);
    if (!ReadImage(table.AddressOfNames, std::as_writable_bytes(std::span(names))) ||
        !ReadImage(table.AddressOfFunctions, std::as_writable_bytes(std::span(functions))) ||
        !ReadImage(table.AddressOfNameOrdinals, std::as_writable_bytes(std::span(ordinals)))) return false;
    DWORD exported = 0;
    constexpr char name[] = "GdiDllInitialize";
    for (size_t i = 0; i < names.size(); ++i) {
      std::array<char, sizeof(name)> actual{};
      if (!ReadImage(names[i], std::as_writable_bytes(std::span(actual)))) return false;
      if (std::memcmp(actual.data(), name, sizeof(name)) != 0) continue;
      if (exported || ordinals[i] >= functions.size()) return false;
      exported = functions[ordinals[i]];
    }
    if (!exported || exported >= header_.image_size ||
        (exports.VirtualAddress <= exported && exported - exports.VirtualAddress < exports.Size) ||
        !exceptions.Size || exceptions.Size % sizeof(RUNTIME_FUNCTION) ||
        exceptions.Size / sizeof(RUNTIME_FUNCTION) > 65536) return false;
    std::vector<RUNTIME_FUNCTION> unwind(exceptions.Size / sizeof(RUNTIME_FUNCTION));
    if (!ReadImage(exceptions.VirtualAddress, std::as_writable_bytes(std::span(unwind)))) return false;
    const auto entry = [&](DWORD rva) {
      return std::lower_bound(unwind.begin(), unwind.end(), rva,
          [](const RUNTIME_FUNCTION& record, DWORD target) { return record.BeginAddress < target; });
    };
    DWORD root = exported;
    const auto direct = entry(exported);
    if (direct == unwind.end() || direct->BeginAddress != exported) {
      if (direct != unwind.begin() && (direct - 1)->EndAddress > exported) return false;
      std::array<std::byte, 5> thunk{};
      int32_t relative = 0;
      if (!ReadImage(exported, thunk) || thunk[0] != std::byte{0xe9}) return false;
      std::memcpy(&relative, thunk.data() + 1, sizeof(relative));
      const int64_t target = static_cast<int64_t>(exported) + 5 + relative;
      if (target <= 0 || target >= header_.image_size) return false;
      root = static_cast<DWORD>(target);
    }
    if (root != header_.entry) return false;
    const auto start = entry(root);
    if (start == unwind.end() || start->BeginAddress != root) return false;
    for (const Row& row : rows_) {
      if (GetTickCount64() >= deadline_) return false;
      const auto after = std::upper_bound(unwind.begin(), unwind.end(), row.rva,
          [](DWORD rva, const RUNTIME_FUNCTION& record) { return rva < record.BeginAddress; });
      if (after == unwind.begin() || (after - 1)->EndAddress != row.end) return false;
    }
    return true;
  }
  static bool HashBytes(std::span<const std::byte> input, std::array<BYTE, 32>* digest) {
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE;
    if (ok) ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    if (ok) ok = CryptHashData(hash, reinterpret_cast<const BYTE*>(input.data()), static_cast<DWORD>(input.size()), 0) != FALSE;
    DWORD bytes = static_cast<DWORD>(digest->size());
    if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest->data(), &bytes, 0) && bytes == digest->size();
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    return ok;
  }
  static bool Hash(HANDLE file, std::array<BYTE, 32>* digest, ULONGLONG deadline) {
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE;
    if (ok) ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    std::array<BYTE, 65536> buffer{}; DWORD total = 0;
    while (ok) {
      if (GetTickCount64() >= deadline) { ok = false; break; }
      DWORD got = 0;
      ok = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) != FALSE;
      if (!ok || !got) break;
      total += got;
      if (total > 32 * 1024 * 1024) { ok = false; break; }
      ok = CryptHashData(hash, buffer.data(), got, 0) != FALSE;
    }
    DWORD bytes = static_cast<DWORD>(digest->size());
    if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest->data(), &bytes, 0) && bytes == digest->size();
    if (hash) CryptDestroyHash(hash); if (provider) CryptReleaseContext(provider, 0);
    return ok;
  }
  void Load(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { setup_ = GetLastError(); return; }
    BY_HANDLE_FILE_INFORMATION info{}; DWORD read = 0;
    bool ok = GetFileInformationByHandle(file, &info) && info.nNumberOfLinks == 1 &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && !info.nFileSizeHigh &&
        info.nFileSizeLow <= sizeof(Header) + 4096 * 32 && info.nFileSizeLow >= sizeof(Header);
    std::vector<std::byte> data(ok ? info.nFileSizeLow : 0);
    if (ok) ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) && read == data.size();
    CloseHandle(file);
    if (ok) {
      std::memcpy(Bytes(header_).data(), data.data(), Bytes(header_).size());
      ok = header_.magic == 0x31434655 && header_.version == 1 &&
          (gdi_ ? header_.reserved && header_.reserved < header_.image_size : !header_.reserved) &&
          header_.row_size == 32 && header_.budget == 128 && header_.count && header_.count <= 4096 &&
          header_.image_size <= 32 * 1024 * 1024 && header_.entry < header_.image_size &&
          data.size() == sizeof(Header) + header_.count * 32;
      if (ok) {
        std::array<BYTE, 32> digest{};
        if (!HashBytes(std::span(data).subspan(sizeof(Header)), &digest) || digest != header_.table_digest) {
          setup_ = ERROR_CRC; return;
        }
        rows_.resize(header_.count);
        std::memcpy(std::as_writable_bytes(std::span(rows_)).data(), data.data() + sizeof(Header),
            std::as_writable_bytes(std::span(rows_)).size());
        DWORD previous = 0;
        for (const auto& row : rows_) {
          if (!(previous < row.rva && row.rva < row.end && row.end <= header_.image_size &&
              row.length && row.length <= 15 && row.length <= row.end - row.rva && row.kind <= 5 &&
              row.condition <= 15 && !row.reserved && row.target < header_.image_size)) { ok = false; break; }
          previous = row.rva;
        }
        if (!Find(header_.entry)) ok = false;
      }
    }
    if (!ok) setup_ = ERROR_INVALID_DATA;
    if (GetTickCount64() >= deadline_) setup_ = ERROR_TIMEOUT;
  }
  const Row* Find(DWORD rva) const {
    auto found = std::lower_bound(rows_.begin(), rows_.end(), rva, [](const Row& row, DWORD at) { return row.rva < at; });
    return found != rows_.end() && found->rva == rva ? &*found : nullptr;
  }
  static bool Taken(BYTE condition, DWORD flags) {
    const bool c = flags & 1, p = flags & 4, z = flags & 0x40, s = flags & 0x80, o = flags & 0x800;
    const std::array<bool, 16> values{o, !o, c, !c, z, !z, c || z, !c && !z,
        s, !s, p, !p, s != o, s == o, z || s != o, !z && s == o};
    return values[condition];
  }
  void Arm(CONTEXT& context, DWORD rva, bool at_current) {
    const Row* row = Find(rva);
    while (row && row->kind == 0) {
      if (row->rva + row->length >= row->end) { row = nullptr; break; }
      row = Find(row->rva + row->length);
    }
    if (!row || row->kind == 5) { error_ = ERROR_NOT_SUPPORTED; Finish(context); return; }
    if (row->kind == 1) {
      // No branch lies between the current block start and this call. Observe
      // its real return directly, without another event at the call itself.
      pending_ = *row; context.Dr0 = base_ + row->rva + row->length;
      context.Dr7 |= 1; action_ = Action::kCallReturn;
      return;
    }
    if (!at_current || base_ + row->rva != context.Rip) {
      context.Dr0 = base_ + row->rva; context.Dr7 |= 1; action_ = Action::kTerminator; return;
    }
    if (row->kind == 2 || row->kind == 3) {
      const bool taken = row->kind == 3 || Taken(row->condition, context.EFlags);
      Record(row->kind, row->rva, row->target, context, taken ? 1 : 0);
      Arm(context, taken ? row->target : row->rva + row->length, false);
    } else {
      Record(4, row->rva, 0, context, 0);
      context.Dr0 = 0; context.Dr7 &= ~DWORD64{1};
    }
  }
  void Finish(CONTEXT& context) { context.Dr0 = 0; context.Dr1 = 0; context.Dr7 &= ~DWORD64{0xf}; }
  void Record(DWORD kind, DWORD rva, DWORD target, const CONTEXT& c, DWORD taken) {
    size_t index = (first_ + used_) % records_.size();
    if (used_ == records_.size()) first_ = (first_ + 1) % records_.size(); else ++used_;
    records_[index] = {kind, rva, target, c.Rax, c.EFlags, taken};
  }
  bool Read(uintptr_t address, std::span<std::byte> output) const {
    SIZE_T copied = 0;
    return ReadProcessMemory(process_, reinterpret_cast<const void*>(address), output.data(), output.size(), &copied) && copied == output.size();
  }
  DWORD Clear() {
    DWORD exit = STILL_ACTIVE;
    if (!armed_ || (GetExitCodeProcess(process_, &exit) && exit != STILL_ACTIVE)) return ERROR_SUCCESS;
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, thread_);
    if (!thread) return GetLastError();
    DWORD status = ERROR_SUCCESS;
    if (SuspendThread(thread) == MAXDWORD) status = GetLastError();
    else {
      CONTEXT context{}; context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (!GetThreadContext(thread, &context)) status = GetLastError();
      else { Finish(context); context.Dr6 = 0; if (!SetThreadContext(thread, &context)) status = GetLastError(); }
      if (ResumeThread(thread) == MAXDWORD && !status) status = GetLastError();
    }
    CloseHandle(thread); return status;
  }
  void Append(std::string_view text) {
    if (*bytes_ > tail_.size() || text.size() > tail_.size()) return;
    if (*bytes_ + text.size() > tail_.size()) {
      size_t discard = *bytes_ + text.size() - tail_.size();
      while (discard < *bytes_ && tail_[discard - 1] != '\n') ++discard;
      std::memmove(tail_.data(), tail_.data() + discard, *bytes_ - discard); *bytes_ -= static_cast<DWORD>(discard);
    }
    std::memcpy(tail_.data() + *bytes_, text.data(), text.size()); *bytes_ += static_cast<DWORD>(text.size());
  }
  HANDLE process_; DWORD thread_; std::span<char> tail_; DWORD* bytes_; ULONGLONG deadline_;
  bool gdi_;
  std::vector<Row> rows_; uintptr_t base_ = 0, root_return_ = 0, root_stack_ = 0;
  DWORD setup_ = ERROR_NOT_READY, error_ = 0, events_ = 0, final_ = 0;
  bool ready_ = false, armed_ = false, finished_ = false; Row pending_{};
  std::array<Observation, 12> records_{}; size_t first_ = 0, used_ = 0;
};
}  // namespace gesher::usb_storage::test
