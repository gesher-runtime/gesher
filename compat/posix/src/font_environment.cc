// Two font keys only. Fallible buffers preserve the global -fno-exceptions policy.
#include <gesher/font_environment.h>
#include <windows.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace gesher {
struct FontEnvironmentBytes {
  explicit FontEnvironmentBytes(size_t length) noexcept : size(length) {}
  std::atomic<size_t> refs{1};
  size_t size;
  char* data = nullptr;
};
namespace {
FontEnvironmentBytes* Allocate(size_t size) {
  // All callers have bounded OS/POSIX input lengths.
  void* memory = std::malloc(sizeof(FontEnvironmentBytes));
  if (!memory) return nullptr;
  auto* bytes = new (memory) FontEnvironmentBytes(size);
  bytes->data = static_cast<char*>(std::malloc(size + 1));
  if (!bytes->data) { bytes->~FontEnvironmentBytes(); std::free(bytes); return nullptr; }
  bytes->data[size] = '\0';
  return bytes;
}
void Retain(FontEnvironmentBytes* bytes) {
  if (bytes) bytes->refs.fetch_add(1, std::memory_order_relaxed);
}
void Release(FontEnvironmentBytes* bytes) {
  if (bytes && bytes->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::free(bytes->data);
    bytes->~FontEnvironmentBytes();
    std::free(bytes);
  }
}
struct Slot {
  bool owned = false;
  FontEnvironmentBytes* value = nullptr;  // null + owned is an absent tombstone.
};
SRWLOCK g_font_environment_lock = SRWLOCK_INIT;
Slot g_font_environment[2];  // One selected gesher_posix object per runtime EXE.
constexpr const char* kNames[] = {"ANDROID_ROOT", "GESHER_WINDOWS_FONTS"};
constexpr const wchar_t* kWideNames[] = {L"ANDROID_ROOT", L"GESHER_WINDOWS_FONTS"};
class Lock {
 public:
  explicit Lock(bool exclusive) : exclusive_(exclusive) {
    if (exclusive_) AcquireSRWLockExclusive(&g_font_environment_lock);
    else AcquireSRWLockShared(&g_font_environment_lock);
  }
  ~Lock() {
    if (exclusive_) ReleaseSRWLockExclusive(&g_font_environment_lock);
    else ReleaseSRWLockShared(&g_font_environment_lock);
  }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
 private:
  bool exclusive_;
};
int KeyIndex(const char* name) {
  if (!name) return -1;
  for (int key = 0; key < 2; ++key) {
    size_t i = 0;
    for (; name[i] && kNames[key][i]; ++i) {
      unsigned char c = static_cast<unsigned char>(name[i]);
      if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
      if (c != static_cast<unsigned char>(kNames[key][i])) break;
    }
    if (name[i] == '\0' && kNames[key][i] == '\0') return key;
  }
  return -1;
}
Slot Snapshot(int key) {
  Lock lock(false);
  Slot copy = g_font_environment[key];
  Retain(copy.value);  // Refcount only; no allocation or release under SRW.
  return copy;
}
FontEnvironmentValue Owned(FontEnvironmentBytes* value) {
  if (!value) return {FontEnvironmentValue::State::kAbsent, nullptr};
  // Validate a known POSIX UTF8 owner, never guess another encoding/path.
  if (value->size && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
          value->data, static_cast<int>(value->size), nullptr, 0) == 0) {
    Release(value);
    return {};
  }
  return {FontEnvironmentValue::State::kValue, value};
}
FontEnvironmentValue EmptyOrError(DWORD error) {
  if (error == ERROR_ENVVAR_NOT_FOUND)
    return {FontEnvironmentValue::State::kAbsent, nullptr};
  if (error == ERROR_SUCCESS)
    return {FontEnvironmentValue::State::kValue, nullptr};
  return {};
}
FontEnvironmentValue ReadWide(int key) {
  SetLastError(ERROR_SUCCESS);
  DWORD needed = GetEnvironmentVariableW(kWideNames[key], nullptr, 0);
  if (!needed) return EmptyOrError(GetLastError());
  for (int retry = 0; retry < 8; ++retry) {
    if (needed > 32768) return {};
    using Wide = std::unique_ptr<wchar_t, decltype(&std::free)>;
    Wide wide(static_cast<wchar_t*>(std::malloc(needed * sizeof(wchar_t))), &std::free);
    if (!wide) return {};
    SetLastError(ERROR_SUCCESS);
    DWORD length = GetEnvironmentVariableW(kWideNames[key], wide.get(), needed);
    if (!length) return EmptyOrError(GetLastError());
    if (length >= needed) { needed = length; continue; }
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.get(),
                                   static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (!size) return {};
    auto* value = Allocate(static_cast<size_t>(size));
    if (!value) return {};
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.get(),
          static_cast<int>(length), value->data, size, nullptr, nullptr) != size) {
      Release(value);
      return {};
    }
    return {FontEnvironmentValue::State::kValue, value};
  }
  return {};  // Bounded external OS resize race, never an absent/default root.
}
bool Skip(const char* name, int overwrite) {
  return !overwrite && std::getenv(name) != nullptr;
}
}  // namespace

FontEnvironmentValue::FontEnvironmentValue(State s, FontEnvironmentBytes* bytes) noexcept
    : state(s), bytes_(bytes) {}
FontEnvironmentValue::FontEnvironmentValue(const FontEnvironmentValue& other) noexcept
    : state(other.state), bytes_(other.bytes_) { Retain(bytes_); }
FontEnvironmentValue::FontEnvironmentValue(FontEnvironmentValue&& other) noexcept
    : state(other.state), bytes_(std::exchange(other.bytes_, nullptr)) {}
FontEnvironmentValue& FontEnvironmentValue::operator=(const FontEnvironmentValue& other) noexcept {
  Retain(other.bytes_);
  Release(std::exchange(bytes_, other.bytes_));
  state = other.state;
  return *this;
}
FontEnvironmentValue& FontEnvironmentValue::operator=(FontEnvironmentValue&& other) noexcept {
  if (this != &other) {
    Release(std::exchange(bytes_, std::exchange(other.bytes_, nullptr)));
    state = other.state;
  }
  return *this;
}
FontEnvironmentValue::~FontEnvironmentValue() { Release(bytes_); }
std::string_view FontEnvironmentValue::value() const noexcept {
  return bytes_ ? std::string_view(bytes_->data, bytes_->size) : std::string_view{};
}
FontEnvironmentValue ReadFontEnvironment(FontEnvironmentKey key) noexcept {
  int index = static_cast<int>(key);
  if (index < 0 || index >= 2) return {};
  Slot before = Snapshot(index);
  if (before.owned) return Owned(before.value);
  auto os = ReadWide(index);  // Fallible allocation/conversion outside SRW.
  Slot after = Snapshot(index);
  // Ownership never returns to Unrecorded. A tracked writer racing the wide
  // read wins at this recheck; a snapshot holds its bytes after unlocking.
  return after.owned ? Owned(after.value) : std::move(os);
}
std::optional<int> WriteFontEnvironment(const char* name, const char* value,
                                      int overwrite) noexcept {
  int key = KeyIndex(name);
  if (key < 0 || !value) return std::nullopt;
  // Preserve UCRT's invalid-parameter callback path outside our owner lock.
  size_t length = 0;
  while (length < 32767 && value[length]) ++length;
  if (length == 32767) return std::nullopt;
  {
    Lock lock(true);
    if (Skip(name, overwrite)) return 0;  // Before preparatory allocation.
  }
  FontEnvironmentBytes* prepared = length ? Allocate(length) : nullptr;
  if (length && !prepared) {
    Lock lock(true);
    if (Skip(name, overwrite)) return 0;
    errno = ENOMEM;
    return ENOMEM;  // No CRT mutation or publication.
  }
  if (prepared) std::memcpy(prepared->data, value, length);
  int result;
  bool skipped;
  {
    Lock lock(true);
    skipped = Skip(name, overwrite);
    result = skipped ? 0 : _putenv_s(name, prepared ? prepared->data : "");
    if (!skipped && result == 0) {
      // Pointer exchange cannot fail. Release the retired owner after SRW.
      std::swap(g_font_environment[key].value, prepared);
      g_font_environment[key].owned = true;
    }
    // Failure publishes nothing; UCRT may itself partially update paired
    // tables before failing. This layer promises no compensating rollback.
  }
  Release(prepared);
  return result;
}
}  // namespace gesher
