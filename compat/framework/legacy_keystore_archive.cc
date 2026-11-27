// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#include "legacy_keystore_archive.h"

#include <windows.h>
#include <winternl.h>
#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <set>
#include <string_view>
#include <vector>
#include <utility>

namespace gesher::keystore {
namespace {

constexpr size_t kMaximum = 16 * 1024 * 1024;
constexpr wchar_t kStore[] = L".gesher-keystore";
constexpr wchar_t kLock[] = L".gesher-keystore.lock";
constexpr DWORD kSharing = FILE_SHARE_READ | FILE_SHARE_DELETE;
struct Failure { DWORD code; };
void Require(bool ok, DWORD error = ERROR_INVALID_DATA) {
  if (!ok) throw Failure{error};
}
void Win(bool ok) { if (!ok) throw Failure{GetLastError()}; }
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  Handle() = default;
  explicit Handle(HANDLE h) : value(h) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
      value = other.value; other.value = INVALID_HANDLE_VALUE;
    }
    return *this;
  }
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Local {
  void* value = nullptr;
  ~Local() { if (value) LocalFree(value); }
};
DWORD NativeError(NTSTATUS status) {
  using Convert = ULONG(WINAPI*)(NTSTATUS);
  const auto convert = reinterpret_cast<Convert>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError")));
  return convert ? convert(status) : ERROR_GEN_FAILURE;
}
std::wstring FullPath(const std::u16string& input) {
  Require(!input.empty() && input.size() < 32700 && input.find(u'\0') == input.npos,
          ERROR_INVALID_NAME);
  std::wstring path(input.begin(), input.end());
  if (path.starts_with(L"/mnt/") && path.size() >= 6 &&
      ((path[5] >= L'a' && path[5] <= L'z') || (path[5] >= L'A' && path[5] <= L'Z')) &&
      (path.size() == 6 || path[6] == L'/')) {
    path = std::wstring(1, path[5]) + L":" + (path.size() == 6 ? L"\\" : path.substr(6));
  }
  std::replace(path.begin(), path.end(), L'/', L'\\');
  const DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  Require(size > 0 && size < 32760, ERROR_INVALID_NAME);
  std::wstring result(size, L'\0');
  const DWORD used = GetFullPathNameW(path.c_str(), size, result.data(), nullptr);
  Require(used > 0 && used < size, ERROR_INVALID_NAME);
  result.resize(used);
  while (result.size() > 3 && result.back() == L'\\') result.pop_back();
  return result;
}
HANDLE OpenRelative(HANDLE parent, std::wstring_view name, DWORD access, DWORD sharing,
                    ULONG disposition, bool directory, void* descriptor = nullptr,
                    bool* created = nullptr) {
  using Create = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
      PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
  const auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile")));
  Require(create != nullptr, ERROR_NOT_SUPPORTED);
  const auto bytes = static_cast<USHORT>(name.size() * sizeof(wchar_t));
  UNICODE_STRING text{bytes, bytes, const_cast<wchar_t*>(name.data())};
  OBJECT_ATTRIBUTES attributes{};
  InitializeObjectAttributes(&attributes, &text, OBJ_CASE_INSENSITIVE, parent, descriptor);
  IO_STATUS_BLOCK io{};
  HANDLE result = INVALID_HANDLE_VALUE;
  const ULONG options = FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT |
      (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
  const NTSTATUS status = create(&result, access | SYNCHRONIZE, &attributes, &io, nullptr,
      FILE_ATTRIBUTE_NORMAL, sharing, disposition, options, nullptr, 0);
  if (status < 0) throw Failure{NativeError(status)};
  if (created) *created = io.Information == FILE_CREATED;
  return result;
}
BY_HANDLE_FILE_INFORMATION Info(HANDLE file, bool directory) {
  Require(GetFileType(file) == FILE_TYPE_DISK, ERROR_INVALID_HANDLE);
  BY_HANDLE_FILE_INFORMATION info{};
  Win(GetFileInformationByHandle(file, &info));
  Require(!(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), ERROR_CANT_ACCESS_FILE);
  Require(bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory, ERROR_DIRECTORY);
  if (!directory) Require(info.nNumberOfLinks == 1, ERROR_TOO_MANY_LINKS);
  return info;
}
FILE_ID_INFO Identity(HANDLE file) {
  FILE_ID_INFO id{};
  if (GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id))) return id;
  const DWORD error = GetLastError();
  Require(error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER ||
      error == ERROR_INVALID_FUNCTION, error);
  const auto info = Info(file, false);
  id.VolumeSerialNumber = info.dwVolumeSerialNumber;
  const uint64_t index = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  std::memcpy(id.FileId.Identifier, &index, sizeof(index));
  return id;
}
bool Same(HANDLE first, HANDLE second) {
  const auto a = Identity(first), b = Identity(second);
  return a.VolumeSerialNumber == b.VolumeSerialNumber &&
      std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0;
}
std::vector<BYTE> Read(HANDLE file) {
  const auto info = Info(file, false);
  const uint64_t length = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  Require(length <= kMaximum, ERROR_FILE_TOO_LARGE);
  LARGE_INTEGER zero{};
  Win(SetFilePointerEx(file, zero, nullptr, FILE_BEGIN));
  std::vector<BYTE> result(static_cast<size_t>(length));
  size_t offset = 0;
  while (offset < result.size()) {
    DWORD got = 0;
    Win(ReadFile(file, result.data() + offset, static_cast<DWORD>(result.size() - offset), &got, nullptr));
    Require(got != 0, ERROR_HANDLE_EOF);
    offset += got;
  }
  return result;
}
void Write(HANDLE file, const std::vector<BYTE>& data) {
  size_t offset = 0;
  while (offset < data.size()) {
    DWORD written = 0;
    Win(WriteFile(file, data.data() + offset, static_cast<DWORD>(data.size() - offset), &written, nullptr));
    Require(written != 0, ERROR_WRITE_FAULT);
    offset += written;
  }
  Win(FlushFileBuffers(file));
}
std::wstring Hex(const BYTE* data, size_t size) {
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring value;
  value.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    value += digits[data[i] >> 4];
    value += digits[data[i] & 15];
  }
  return value;
}
std::wstring Hash(const std::vector<BYTE>& data) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
          ERROR_GEN_FAILURE);
  struct Algorithm { BCRYPT_ALG_HANDLE value; ~Algorithm() { BCryptCloseAlgorithmProvider(value, 0); } } a{algorithm};
  BCRYPT_HASH_HANDLE hash = nullptr;
  Require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0, ERROR_GEN_FAILURE);
  struct Digest { BCRYPT_HASH_HANDLE value; ~Digest() { BCryptDestroyHash(value); } } d{hash};
  Require(BCryptHashData(hash, const_cast<BYTE*>(data.data()), static_cast<ULONG>(data.size()), 0) >= 0,
          ERROR_GEN_FAILURE);
  std::array<BYTE, 32> digest{};
  Require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0, ERROR_GEN_FAILURE);
  return Hex(digest.data(), digest.size());
}
std::vector<BYTE> Security(HANDLE file, SECURITY_INFORMATION flags) {
  DWORD size = 0;
  GetKernelObjectSecurity(file, flags, nullptr, 0, &size);
  Require(GetLastError() == ERROR_INSUFFICIENT_BUFFER && size > 0 && size <= 65536,
          ERROR_INVALID_SECURITY_DESCR);
  std::vector<BYTE> result(size);
  Win(GetKernelObjectSecurity(file, flags, result.data(), size, &size));
  return result;
}
std::wstring SidString(PSID sid) {
  Local text;
  Win(ConvertSidToStringSidW(sid, reinterpret_cast<LPWSTR*>(&text.value)));
  return static_cast<const wchar_t*>(text.value);
}
std::vector<BYTE> TokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS type) {
  DWORD size = 0;
  GetTokenInformation(token, type, nullptr, 0, &size);
  Require(GetLastError() == ERROR_INSUFFICIENT_BUFFER && size > 0 && size <= 65536);
  std::vector<BYTE> result(size);
  Win(GetTokenInformation(token, type, result.data(), size, &size));
  return result;
}
struct ArchiveSecurity {
  Local descriptor;
  std::vector<BYTE> user, container;
  ArchiveSecurity() {
    Handle token;
    Win(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value));
    user = TokenInfo(token.value, TokenUser);
    std::wstring trustees = L"(A;OICI;FA;;;SY)(A;OICI;FA;;;" +
        SidString(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid) + L")";
    DWORD contained = 0, size = 0;
    Win(GetTokenInformation(token.value, TokenIsAppContainer, &contained, sizeof(contained), &size));
    if (contained) {
      container = TokenInfo(token.value, TokenAppContainerSid);
      const PSID sid = reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(container.data())->TokenAppContainer;
      Require(IsValidSid(sid) != FALSE, ERROR_INVALID_SID);
      trustees += L"(A;OICI;FA;;;" + SidString(sid) + L")";
    }
    // OWNER RIGHTS suppresses implicit owner WRITE_DAC, including siblings
    // whose normal TokenUser is the same private service/desktop account.
    const std::wstring sddl = L"O:" + SidString(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid) + L"D:P" + trustees + L"(A;OICI;RC;;;OW)S:(ML;OICI;NW;;;LW)";
    Win(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
        reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr));
  }
  void Verify(HANDLE file) const {
    const auto actual = Security(file, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION);
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    Win(GetSecurityDescriptorControl(const_cast<BYTE*>(actual.data()), &control, &revision));
    Require((control & SE_DACL_PROTECTED) != 0, ERROR_ACCESS_DENIED);
    PSID owner = nullptr;
    BOOL defaulted = FALSE;
    Win(GetSecurityDescriptorOwner(const_cast<BYTE*>(actual.data()), &owner, &defaulted));
    const PSID expected_owner = reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
    Require(owner && EqualSid(owner, expected_owner), ERROR_ACCESS_DENIED);
    PACL wanted = nullptr, found = nullptr;
    BOOL present = FALSE;
    Win(GetSecurityDescriptorDacl(descriptor.value, &present, &wanted, &defaulted));
    Require(present && wanted, ERROR_INVALID_ACL);
    Win(GetSecurityDescriptorDacl(const_cast<BYTE*>(actual.data()), &present, &found, &defaulted));
    Require(present && found && IsValidAcl(found) && found->AceCount == wanted->AceCount, ERROR_ACCESS_DENIED);
    for (DWORD i = 0; i < found->AceCount; ++i) {
      void* a = nullptr;
      void* b = nullptr;
      Win(GetAce(found, i, &a));
      Win(GetAce(wanted, i, &b));
      const auto* first = static_cast<const ACE_HEADER*>(a);
      const auto* second = static_cast<const ACE_HEADER*>(b);
      Require(first->AceSize == second->AceSize && std::memcmp(a, b, first->AceSize) == 0, ERROR_ACCESS_DENIED);
    }
  }
};
struct Cursor {
  const std::vector<BYTE>& input;
  size_t position = 0;
  size_t Take(size_t count) {
    Require(count <= input.size() - position);
    const size_t at = position;
    position += count;
    return at;
  }
  uint32_t U32() {
    const size_t at = Take(4);
    return uint32_t(input[at]) << 24 | uint32_t(input[at + 1]) << 16 |
        uint32_t(input[at + 2]) << 8 | input[at + 3];
  }
  void Utf() {
    const size_t at = Take(2);
    Take(size_t(input[at]) << 8 | input[at + 1]);
  }
  void Blob() { Take(U32()); }
};
void U32(std::vector<BYTE>* output, uint32_t value) {
  for (int shift : {24, 16, 8, 0}) output->push_back(static_cast<BYTE>(value >> shift));
}
std::vector<BYTE> Scrub(const std::vector<BYTE>& input, bool* changed) {
  Cursor cursor{input};
  const uint32_t magic = cursor.U32();
  Require(magic == 0x474b5331 || magic == 0x474b5332);
  const uint32_t count = cursor.U32();
  Require(count <= input.size() / 21);
  *changed = magic == 0x474b5331;
  std::vector<BYTE> output;
  output.reserve(input.size());
  U32(&output, 0x474b5332);
  U32(&output, count);
  std::set<std::string> aliases;
  auto copy = [&](size_t first, size_t last) {
    output.insert(output.end(), input.begin() + first, input.begin() + last);
  };
  for (uint32_t i = 0; i < count; ++i) {
    const size_t start = cursor.position;
    cursor.Utf();
    Require(aliases.emplace(reinterpret_cast<const char*>(input.data() + start), cursor.position - start).second);
    const BYTE kind = input[cursor.Take(1)];
    Require(kind >= 1 && kind <= 3);
    cursor.Take(8);
    cursor.Utf();
    const size_t metadata_end = cursor.position;
    const BYTE flag = magic == 0x474b5331 ? BYTE(kind == 3 ? 0 : 1) : input[cursor.Take(1)];
    Require(flag <= 2 && (kind != 3 || flag == 0));
    const size_t key_start = cursor.position;
    const uint32_t key_size = cursor.U32();
    cursor.Take(key_size);
    Require(kind != 3 || key_size == 0);
    const size_t chain_start = cursor.position;
    const uint32_t certificates = cursor.U32();
    Require(certificates <= (input.size() - cursor.position) / 4);
    for (uint32_t c = 0; c < certificates; ++c) cursor.Blob();
    copy(start, metadata_end);
    const bool legacy = kind != 3 && flag == 1;
    output.push_back(legacy ? 2 : flag);
    if (legacy) {
      U32(&output, 0);
      *changed = true;
    } else {
      Require(flag != 2 || key_size == 0);
      copy(key_start, chain_start);
    }
    copy(chain_start, cursor.position);
    Require(output.size() <= kMaximum, ERROR_FILE_TOO_LARGE);
  }
  Require(cursor.position == input.size());
  return output;
}
void Rename(HANDLE file, HANDLE directory, const std::wstring& name, bool replace) {
  using Set = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
  const auto set = reinterpret_cast<Set>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile")));
  Require(set != nullptr, ERROR_NOT_SUPPORTED);
  struct Header { ULONG flags; HANDLE root; ULONG bytes; WCHAR name[1]; };
  const size_t size = offsetof(Header, name) + name.size() * sizeof(wchar_t);
  std::vector<BYTE> memory(size);
  auto* info = reinterpret_cast<Header*>(memory.data());
  info->flags = replace ? 3 : 0;  // REPLACE_IF_EXISTS | POSIX_SEMANTICS.
  info->root = directory;
  info->bytes = static_cast<ULONG>(name.size() * sizeof(wchar_t));
  std::memcpy(info->name, name.data(), info->bytes);
  IO_STATUS_BLOCK io{};
  const NTSTATUS status = set(file, &io, info, static_cast<ULONG>(size), static_cast<FILE_INFORMATION_CLASS>(65));
  if (status < 0) throw Failure{NativeError(status)};
}
struct Temporary {
  Handle file;
  bool committed = false;
  Temporary(HANDLE directory, void* descriptor) {
    std::array<BYTE, 16> random{};
    Require(BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, ERROR_GEN_FAILURE);
    file.value = OpenRelative(directory, L".gesher-key-" + Hex(random.data(), random.size()) + L".tmp",
        GENERIC_READ | GENERIC_WRITE | DELETE | READ_CONTROL, kSharing, FILE_CREATE, false, descriptor);
    Info(file.value, false);
  }
  ~Temporary() {
    if (!committed && file.value != INVALID_HANDLE_VALUE) {
      FILE_DISPOSITION_INFO erase{TRUE};
      SetFileInformationByHandle(file.value, FileDispositionInfo, &erase, sizeof(erase));
    }
  }
};
// Keep every ancestor pinned until the operation finishes; no component may
// redirect migration through a junction/symlink, including the trusted system
// parent of an archive whose final directory does not exist yet.
struct PinnedDirectory {
  std::vector<Handle> chain;
  explicit PinnedDirectory(const std::wstring& path) {
    size_t first;
    if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') first = 3;
    else if (path.starts_with(L"\\\\")) {
      const size_t server = path.find(L'\\', 2);
      Require(server != path.npos, ERROR_INVALID_NAME);
      const size_t share = path.find(L'\\', server + 1);
      first = share == path.npos ? path.size() : share + 1;
    } else throw Failure{ERROR_INVALID_NAME};
    HANDLE root = CreateFileW(path.substr(0, first).c_str(),
        FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Win(root != INVALID_HANDLE_VALUE);
    chain.emplace_back(root);
    Info(root, true);
    while (first < path.size()) {
      const size_t slash = path.find(L'\\', first);
      const size_t end = slash == path.npos ? path.size() : slash;
      Require(end > first, ERROR_INVALID_NAME);
      Handle child(OpenRelative(chain.back().value,
          std::wstring_view(path).substr(first, end - first),
          FILE_READ_ATTRIBUTES | FILE_TRAVERSE, FILE_SHARE_READ | FILE_SHARE_WRITE,
          FILE_OPEN, true));
      Info(child.value, true);
      chain.push_back(std::move(child));
      first = end + 1;
    }
  }
  HANDLE get() const { return chain.back().value; }
};

void CommitArchive(HANDLE directory, const std::wstring& name, const std::vector<BYTE>& original,
                   const ArchiveSecurity& policy) {
  Temporary pending(directory, policy.descriptor.value);
  policy.Verify(pending.file.value);
  Write(pending.file.value, original);
  Require(Read(pending.file.value) == original, ERROR_CRC);
  try {
    Rename(pending.file.value, directory, name, false);
    pending.committed = true;
  } catch (const Failure& failure) {
    if (failure.code != ERROR_ALREADY_EXISTS && failure.code != ERROR_FILE_EXISTS) throw;
    Handle existing(OpenRelative(directory, name, GENERIC_READ | READ_CONTROL,
        FILE_SHARE_READ, FILE_OPEN, false));
    Info(existing.value, false);
    policy.Verify(existing.value);
    Require(Read(existing.value) == original, ERROR_CRC);
  }
}
}  // namespace

uint32_t ArchiveLegacy(const std::u16string& data_directory, const std::u16string& archive_root,
                       const std::u16string& binding) try {
  Require(binding.size() == 64 && std::all_of(binding.begin(), binding.end(), [](char16_t c) {
    return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f');
  }), ERROR_INVALID_NAME);
  const auto source_path = FullPath(data_directory), archive_path = FullPath(archive_root);
  Require(archive_path.size() > 3 && !archive_path.ends_with(L"\\"), ERROR_INVALID_NAME);
  const size_t split = archive_path.rfind(L'\\');
  Require(split != archive_path.npos && split + 1 < archive_path.size(), ERROR_INVALID_NAME);
  std::unique_ptr<PinnedDirectory> source_chain;
  try { source_chain = std::make_unique<PinnedDirectory>(source_path); }
  catch (const Failure& failure) {
    if (failure.code == ERROR_PATH_NOT_FOUND || failure.code == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    throw;
  }
  const HANDLE source = source_chain->get();
  // Avoid creating lock files or needing write permission when no migration is
  // required. Reopen and parse under the exclusive sidecar before any change.
  try {
    Handle probe(OpenRelative(source, kStore, GENERIC_READ, kSharing, FILE_OPEN, false));
    bool changed = false;
    Scrub(Read(probe.value), &changed);
    if (!changed) return ERROR_SUCCESS;
  } catch (const Failure& failure) {
    if (failure.code == ERROR_FILE_NOT_FOUND || failure.code == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
    throw;
  }
  Handle lock;
  const ULONGLONG deadline = GetTickCount64() + 5000;
  for (;;) {
    try {
      lock.value = OpenRelative(source, kLock, GENERIC_WRITE | FILE_READ_ATTRIBUTES,
          0, FILE_OPEN_IF, false);
      break;
    } catch (const Failure& failure) {
      if (failure.code != ERROR_SHARING_VIOLATION && failure.code != ERROR_LOCK_VIOLATION) throw;
      if (GetTickCount64() >= deadline) return ERROR_TIMEOUT;
      Sleep(10);
    }
  }
  Info(lock.value, false);
  Handle original;
  try { original.value = OpenRelative(source, kStore, GENERIC_READ | READ_CONTROL,
      kSharing, FILE_OPEN, false); }
  catch (const Failure& failure) {
    if (failure.code == ERROR_FILE_NOT_FOUND || failure.code == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
    throw;
  }
  const auto source_info = Info(original.value, false);
  const auto raw = Read(original.value);
  bool changed = false;
  const auto scrubbed = Scrub(raw, &changed);
  if (!changed) return ERROR_SUCCESS;
  Require(!(source_info.dwFileAttributes & FILE_ATTRIBUTE_READONLY), ERROR_ACCESS_DENIED);
  ArchiveSecurity policy;
  PinnedDirectory archive_parent(archive_path.substr(0, split == 2 ? 3 : split));
  Handle archive(OpenRelative(archive_parent.get(), archive_path.substr(split + 1),
      FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF, true, policy.descriptor.value));
  Info(archive.value, true);
  policy.Verify(archive.value);
  const std::wstring name(binding.begin(), binding.end());
  Handle bucket(OpenRelative(archive.value, name,
      FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_READ_ATTRIBUTES | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF, true, policy.descriptor.value));
  Info(bucket.value, true);
  policy.Verify(bucket.value);
  CommitArchive(bucket.value, Hash(raw) + L".gks", raw, policy);
  auto descriptor = Security(original.value, DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION);
  Win(SetSecurityDescriptorControl(descriptor.data(), SE_DACL_PROTECTED, SE_DACL_PROTECTED));
  Temporary replacement(source, descriptor.data());
  Write(replacement.file.value, scrubbed);
  Require(Read(replacement.file.value) == scrubbed, ERROR_CRC);
  // Both parent handles remain pinned against rename. The per-store share0
  // sidecar excludes provider transactions throughout this read/modify/write.
  // Recheck the fixed no-follow leaf before publishing, without resolving a
  // caller-controlled target path or writing through a hardlink/reparse point.
  Handle current(OpenRelative(source, kStore, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN, false));
  Info(current.value, false);
  Require(Same(original.value, current.value) && Read(original.value) == raw, ERROR_RETRY);
  Rename(replacement.file.value, source, kStore, true);
  replacement.committed = true;
  return ERROR_SUCCESS;
} catch (const Failure& failure) {
  return failure.code;
} catch (const std::bad_alloc&) {
  return ERROR_NOT_ENOUGH_MEMORY;
} catch (...) {
  return ERROR_GEN_FAILURE;
}

}  // namespace gesher::keystore
