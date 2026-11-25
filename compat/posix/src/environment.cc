// POSIX environment storage is independent of UCRT's freeing/copying table.
#include <gesher/environment.h>
#include "environment_internal.h"
#include <windows.h>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <new>
#include <system_error>

namespace gesher {
int RawPutAssignment(const char* assignment);
namespace {
struct EnvironmentOwner {
  std::recursive_mutex writer;
  std::mutex snapshot;
  environment::Table table;
  bool writing = false;
};
INIT_ONCE once = INIT_ONCE_STATIC_INIT;
EnvironmentOwner* local = nullptr;

char* Utf8(const wchar_t* wide) {
  if (!wide) return errno = EINVAL, nullptr;
  // Preserve an unpaired Windows surrogate as invalid UTF8 (WTF8), rather
  // than replacing it or preventing unrelated environment reads/writes.
  // Consumers that require Unicode, including the font bridge, reject it.
  size_t size = 0;
  for (size_t i = 0; wide[i]; ++i) {
    uint32_t value = wide[i];
    if (value >= 0xd800 && value <= 0xdbff && wide[i + 1] >= 0xdc00 && wide[i + 1] <= 0xdfff) {
      ++i; size += 4;
    } else size += value < 0x80 ? 1 : value < 0x800 ? 2 : 3;
  }
  char* bytes = static_cast<char*>(std::malloc(size + 1));
  if (!bytes) return errno = ENOMEM, nullptr;
  char* output = bytes;
  for (size_t i = 0; wide[i]; ++i) {
    uint32_t value = wide[i];
    if (value >= 0xd800 && value <= 0xdbff && wide[i + 1] >= 0xdc00 && wide[i + 1] <= 0xdfff)
      value = 0x10000 + ((value - 0xd800) << 10) + (wide[++i] - 0xdc00);
    if (value < 0x80) *output++ = static_cast<char>(value);
    else {
      if (value >= 0x10000) { *output++ = static_cast<char>(0xf0 | (value >> 18)); *output++ = static_cast<char>(0x80 | ((value >> 12) & 63)); }
      else if (value >= 0x800) *output++ = static_cast<char>(0xe0 | (value >> 12));
      if (value >= 0x800) *output++ = static_cast<char>(0x80 | ((value >> 6) & 63));
      else *output++ = static_cast<char>(0xc0 | (value >> 6));
      *output++ = static_cast<char>(0x80 | (value & 63));
    }
  }
  *output = '\0';
  return bytes;
}
BOOL CALLBACK Initialize(PINIT_ONCE, void*, void**) {
  wchar_t* block = GetEnvironmentStringsW();
  if (!block) return errno = ENOMEM, FALSE;
  size_t count = 0;
  for (const wchar_t* item = block; *item; item += std::wcslen(item) + 1)
    if (*item != L'=') ++count;  // Windows per-drive entries are not POSIX names.
  auto** entries = static_cast<char**>(std::calloc(count + 1, sizeof(char*)));
  auto* owner = static_cast<EnvironmentOwner*>(std::malloc(sizeof(EnvironmentOwner)));
  size_t used = 0;
  if (entries && owner) {
    for (const wchar_t* item = block; *item; item += std::wcslen(item) + 1) {
      if (*item == L'=') continue;
      entries[used] = Utf8(item);
      if (!entries[used]) break;
      // Windows normally spells the inherited search path "Path". The
      // shell already exposes it as POSIX PATH; use that spelling throughout.
      if (_strnicmp(entries[used], "PATH=", 5) == 0) std::memcpy(entries[used], "PATH", 4);
      ++used;
    }
  } else errno = ENOMEM;
  FreeEnvironmentStringsW(block);
  if (!entries || !owner || used != count) {
    for (size_t index = 0; index < used; ++index) std::free(entries[index]);
    std::free(entries); std::free(owner); return FALSE;
  }
  bool constructed = false;
  try {
    new (owner) EnvironmentOwner;
    constructed = true;
  } catch (const std::bad_alloc&) {
    errno = ENOMEM;
  } catch (const std::system_error& error) {
    errno = error.code().value();
  }
  if (!constructed) {
    for (size_t index = 0; index < used; ++index) std::free(entries[index]);
    std::free(entries); std::free(owner); return FALSE;
  }
  owner->table.Initialize(entries, count + 1);
  local = owner;  // The mutex, table slot and retained strings outlive atexit.
  return TRUE;
}
extern "C" __declspec(dllexport) EnvironmentOwner* gesher_environment_owner_v1() {
  return InitOnceExecuteOnce(&once, Initialize, nullptr, nullptr) ? local : nullptr;
}
EnvironmentOwner* Owner() {
  using Resolve = EnvironmentOwner* (*)();
  static const auto resolve = [] {
    auto shared = reinterpret_cast<Resolve>(GetProcAddress(GetModuleHandleW(nullptr),
                                                            "gesher_environment_owner_v1"));
    return shared ? shared : gesher_environment_owner_v1;
  }();
  return resolve();
}
int NativeWrite(const char* name, const char* value, void*) {
  return WriteNativeEnvironment(name, value);
}
struct WideWrite { const wchar_t* name; const wchar_t* value; };
int NativeWideWrite(const char* name, const char* value, void* context) {
  auto* wide = static_cast<WideWrite*>(context);
  return WriteWideNativeEnvironment(name, value, wide->name, wide->value);
}
template<class Character> bool RejectedLength(const Character* value) {
  // UCRT rejects each name/value length before allocation or table changes.
  size_t length = 0;
  while (length != 32767 && value[length]) ++length;
  return length == 32767;
}
int NativeResult(int error) { return error ? (errno = error, -1) : 0; }
struct NativeTransaction {
  std::unique_lock<std::mutex>* snapshot;
  environment::Table::Writer writer;
  void* context;
};
int UnlockedNativeWrite(const char* name, const char* value, void* context) {
  auto* transaction = static_cast<NativeTransaction*>(context);
  transaction->snapshot->unlock();
  const int result = transaction->writer(name, value, transaction->context);
  transaction->snapshot->lock();
  return result;
}
// Reads in a CRT invalid-parameter callback may inspect the prior table.
// Recursive writes cannot invalidate an outer transaction's prepared storage.
class WriteGuard {
 public:
  explicit WriteGuard(EnvironmentOwner& owner) : owner_(owner), entered_(!owner.writing) {
    if (entered_) owner_.writing = true;
  }
  ~WriteGuard() { if (entered_) owner_.writing = false; }
  explicit operator bool() const { return entered_; }
 private:
  EnvironmentOwner& owner_;
  bool entered_;
};
}  // namespace

extern "C" void gesher_environment_anchor_v1() {}
char*** EnvironmentSlot() {
  auto* owner = Owner();
  return owner ? owner->table.Slot() : nullptr;
}
char* GetEnvironment(const char* name) {
  auto* owner = Owner();
  if (!owner) return nullptr;
  std::lock_guard<std::mutex> lock(owner->snapshot);
  return owner->table.Get(name);
}
char* GetWindowsEnvironment(const char* name) {
  auto* owner = Owner();
  if (!owner) return nullptr;
  std::lock_guard<std::mutex> lock(owner->snapshot);
  return owner->table.GetNative(name);
}
int SetEnvironment(const char* name, const char* value, int overwrite) {
  if (!name || !*name || !value || std::strchr(name, '=')) return errno = EINVAL, -1;
  if (RejectedLength(name) || RejectedLength(value)) {
    // Skipped POSIX writes must not invoke the invalid handler. Proven
    // pre-mutation UCRT rejection holds neither owner lock and performs no
    // retained-string swap, preserving the original writer/CRT wait chain.
    if (!overwrite && GetEnvironment(name)) return 0;
    return NativeResult(RawPutEnvironment(name, value));
  }
  auto* owner = Owner();
  if (!owner) return -1;
  std::lock_guard<std::recursive_mutex> lock(owner->writer);
  WriteGuard writing(*owner);
  if (!writing) return errno = EBUSY, -1;
  std::unique_lock<std::mutex> snapshot(owner->snapshot);
  NativeTransaction native{&snapshot, NativeWrite, nullptr};
  return owner->table.Set(name, value, overwrite, UnlockedNativeWrite, &native);
}
int PutEnvironment(char* assignment) {
  if (!assignment) return errno = EINVAL, -1;
  const char* equals = std::strchr(assignment, '=');
  if (!equals) return UnsetEnvironment(assignment);
  if (equals == assignment) return errno = EINVAL, -1;
  if (equals - assignment >= 32767 || RejectedLength(equals + 1))
    return RawPutAssignment(assignment);
  auto* owner = Owner();
  if (!owner) return -1;
  std::lock_guard<std::recursive_mutex> lock(owner->writer);
  WriteGuard writing(*owner);
  if (!writing) return errno = EBUSY, -1;
  std::unique_lock<std::mutex> snapshot(owner->snapshot);
  NativeTransaction native{&snapshot, NativeWrite, nullptr};
  return owner->table.Put(assignment, UnlockedNativeWrite, &native);
}
int UnsetEnvironment(const char* name) {
  if (!name || !*name || std::strchr(name, '=')) return errno = EINVAL, -1;
  if (RejectedLength(name)) return NativeResult(RawPutEnvironment(name, ""));
  auto* owner = Owner();
  if (!owner) return -1;
  std::lock_guard<std::recursive_mutex> lock(owner->writer);
  WriteGuard writing(*owner);
  if (!writing) return errno = EBUSY, -1;
  std::unique_lock<std::mutex> snapshot(owner->snapshot);
  NativeTransaction native{&snapshot, NativeWrite, nullptr};
  return owner->table.Unset(name, UnlockedNativeWrite, &native);
}
int SetNativeEnvironment(const char* name, const char* value) {
  if (!name || !*name || std::strchr(name, '=')) return errno = EINVAL, -1;
  if (RejectedLength(name) || (value && RejectedLength(value)))
    return NativeResult(RawPutEnvironment(name, value ? value : ""));
  auto* owner = Owner();
  if (!owner) return -1;
  std::lock_guard<std::recursive_mutex> lock(owner->writer);
  WriteGuard writing(*owner);
  if (!writing) return errno = EBUSY, -1;
  std::unique_lock<std::mutex> snapshot(owner->snapshot);
  NativeTransaction native{&snapshot, NativeWrite, nullptr};
  return owner->table.SetNative(name, value, UnlockedNativeWrite, &native);
}
int SetWideEnvironment(const wchar_t* name, const wchar_t* value) {
  if (!name || !*name || !value || std::wcschr(name, L'='))
    return NativeResult(RawPutWideEnvironment(name, value));
  if (RejectedLength(name) || RejectedLength(value))
    return NativeResult(RawPutWideEnvironment(name, value));
  auto* owner = Owner();
  if (!owner) return -1;
  char* narrow_name = Utf8(name);
  if (!narrow_name) return -1;
  char* narrow_value = Utf8(value);
  if (!narrow_value) { std::free(narrow_name); return -1; }
  int result;
  {
    std::lock_guard<std::recursive_mutex> lock(owner->writer);
    WriteGuard writing(*owner);
    WideWrite wide{name, value};
    if (!writing) result = (errno = EBUSY, -1);
    else {
      std::unique_lock<std::mutex> snapshot(owner->snapshot);
      NativeTransaction native{&snapshot, NativeWideWrite, &wide};
      result = owner->table.SetNative(narrow_name, *value ? narrow_value : nullptr, UnlockedNativeWrite, &native);
    }
  }
  const int error = errno;
  std::free(narrow_name); std::free(narrow_value);
  errno = error;
  return result;
}
int VisitBorrowedFontEnvironment(const char* name, void (*visit)(const char*, void*), void* context) {
  auto* owner = Owner();
  if (!owner) return -1;
  std::lock_guard<std::mutex> lock(owner->snapshot);
  const char* value = nullptr;
  if (!owner->table.BorrowedProjection(name, &value)) return 0;
  visit(value, context);
  return 1;
}
bool VisitEnvironment(void (*visit)(const char*, void*), void* context) {
  auto* owner = Owner();
  if (!owner) return false;
  std::lock_guard<std::mutex> lock(owner->snapshot);
  char** entries = *owner->table.Slot();
  for (char** entry = entries; entry && *entry; ++entry) visit(*entry, context);
  return true;
}
int RawPutEnvironment(const char* name, const char* value) {
  using Write = int (*)(const char*, const char*);
  static const auto write = reinterpret_cast<Write>(GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"), "_putenv_s"));
  if (!write) return errno = ENOSYS, ENOSYS;
  const int error = write(name, value);
  return error > 34 ? gesher_errno_from_crt(error) : error;
}
int RawPutWideEnvironment(const wchar_t* name, const wchar_t* value) {
  using Write = int (*)(const wchar_t*, const wchar_t*);
  static const auto write = reinterpret_cast<Write>(GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"), "_wputenv_s"));
  if (!write) return errno = ENOSYS, ENOSYS;
  const int error = write(name, value);
  return error > 34 ? gesher_errno_from_crt(error) : error;
}
int RawPutAssignment(const char* assignment) {
  using Write = int (*)(const char*);
  static const auto write = reinterpret_cast<Write>(GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"), "_putenv"));
  return write ? write(assignment) : (errno = ENOSYS, -1);
}
}  // namespace gesher

extern "C" char* getenv(const char* name) { return gesher::GetEnvironment(name); }
extern "C" int putenv(const char* assignment) {
  return gesher::PutEnvironment(const_cast<char*>(assignment));
}
extern "C" char*** gesher_environment_slot() { return gesher::EnvironmentSlot(); }

// Native CRT callers can mutate after Android ELF environ relocations have
// bound their slot. Route both ordinary and dllimport calls through the owner;
// only RawPut* above may enter the original UCRT primitive.
extern "C" int gesher_crt_putenv_s(const char*, const char*) __asm__("_putenv_s");
extern "C" int gesher_crt_putenv_s(const char* name, const char* value) {
  if (!name || !*name || !value || std::strchr(name, '=')) return gesher::RawPutEnvironment(name, value);
  const int result = gesher::SetNativeEnvironment(name, *value ? value : nullptr);
  return result == 0 ? 0 : errno;
}
extern "C" int gesher_crt_wputenv_s(const wchar_t*, const wchar_t*) __asm__("_wputenv_s");
extern "C" int gesher_crt_wputenv_s(const wchar_t* name, const wchar_t* value) {
  if (!name || !*name || !value || std::wcschr(name, L'=')) return gesher::RawPutWideEnvironment(name, value);
  return gesher::SetWideEnvironment(name, value) == 0 ? 0 : errno;
}
extern "C" int gesher_crt_putenv(const char*) __asm__("_putenv");
extern "C" int gesher_crt_putenv(const char* assignment) {
  const char* equals = assignment ? std::strchr(assignment, '=') : nullptr;
  if (!equals || equals == assignment) return gesher::RawPutAssignment(assignment);
  const size_t length = static_cast<size_t>(equals - assignment);
  char* name = static_cast<char*>(std::malloc(length + 1));
  if (!name) return errno = ENOMEM, -1;
  std::memcpy(name, assignment, length); name[length] = '\0';
  const int error = gesher_crt_putenv_s(name, equals + 1);
  std::free(name);
  return error ? (errno = error, -1) : 0;
}
extern "C" {
char* (*__imp_getenv)(const char*) = getenv;
int (*__imp_putenv)(const char*) = putenv;
int (*__imp__putenv_s)(const char*, const char*) = gesher_crt_putenv_s;
int (*__imp__wputenv_s)(const wchar_t*, const wchar_t*) = gesher_crt_wputenv_s;
int (*__imp__putenv)(const char*) = gesher_crt_putenv;
}
