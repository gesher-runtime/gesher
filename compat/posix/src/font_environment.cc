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
  bool pending = false;
  FontEnvironmentValue::State pending_state = FontEnvironmentValue::State::kError;
  FontEnvironmentBytes* pending_value = nullptr;
};
struct View {
  FontEnvironmentValue::State state = FontEnvironmentValue::State::kError;
  FontEnvironmentBytes* value = nullptr;
  bool authoritative = false;
};
SRWLOCK g_font_environment_writer = SRWLOCK_INIT;
SRWLOCK g_font_environment_snapshot = SRWLOCK_INIT;
Slot g_font_environment[2];  // One selected gesher_posix object per runtime EXE.
constexpr const char* kNames[] = {"ANDROID_ROOT", "GESHER_WINDOWS_FONTS"};
constexpr const wchar_t* kWideNames[] = {L"ANDROID_ROOT", L"GESHER_WINDOWS_FONTS"};
class Lock {
 public:
  explicit Lock(SRWLOCK& lock, bool exclusive) : lock_(&lock), exclusive_(exclusive) {
    if (exclusive_) AcquireSRWLockExclusive(lock_);
    else AcquireSRWLockShared(lock_);
  }
  ~Lock() {
    if (exclusive_) ReleaseSRWLockExclusive(lock_);
    else ReleaseSRWLockShared(lock_);
  }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
 private:
  SRWLOCK* lock_;
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
View Snapshot(int key) {
  Lock lock(g_font_environment_snapshot, false);
  const Slot& slot = g_font_environment[key];
  View copy;
  if (slot.owned) copy = {slot.value ? FontEnvironmentValue::State::kValue :
                         FontEnvironmentValue::State::kAbsent, slot.value, true};
  else if (slot.pending) copy = {slot.pending_state, slot.pending_value, true};
  Retain(copy.value);  // Refcount only; no allocation/release or CRT call here.
  return copy;
}
FontEnvironmentValue Adopt(View view) {
  // Validate known UTF8 bytes; never guess an alternative encoding/path.
  if (view.state == FontEnvironmentValue::State::kValue && view.value &&
      view.value->size && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
          view.value->data, static_cast<int>(view.value->size), nullptr, 0) == 0) {
    Release(view.value);
    return {};
  }
  return {view.state, view.value};
}
View EmptyOrError(DWORD error) {
  if (error == ERROR_ENVVAR_NOT_FOUND)
    return {FontEnvironmentValue::State::kAbsent, nullptr};
  if (error == ERROR_SUCCESS)
    return {FontEnvironmentValue::State::kValue, nullptr};
  return {};
}
View ReadWide(int key) {
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
  View before = Snapshot(index);
  if (before.authoritative) return Adopt(before);
  auto os = ReadWide(index);  // All allocation/conversion outside snapshot SRW.
  View after = Snapshot(index);
  // A committed owner or an in-flight prior view wins over transient OS bytes.
  if (after.authoritative) { Release(os.value); return Adopt(after); }
  return Adopt(os);
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
    Lock lock(g_font_environment_writer, true);
    if (Skip(name, overwrite)) return 0;  // Before preparatory allocation.
  }
  FontEnvironmentBytes* prepared = length ? Allocate(length) : nullptr;
  if (length && !prepared) {
    Lock lock(g_font_environment_writer, true);
    if (Skip(name, overwrite)) return 0;
    errno = ENOMEM;
    return ENOMEM;  // No CRT mutation or publication.
  }
  if (prepared) std::memcpy(prepared->data, value, length);
  int result = 0;
  FontEnvironmentBytes* retired_prior = nullptr;
  {
    // Serializes tracked writers only. Readers never acquire this lock; read
    // callbacks under UCRT's environment lock can inspect snapshot state.
    Lock writer(g_font_environment_writer, true);
    if (!Skip(name, overwrite)) {
      bool unrecorded;
      { Lock snapshot(g_font_environment_snapshot, false);
        unrecorded = !g_font_environment[key].owned; }
      if (unrecorded) {
        View prior = ReadWide(key);
        Lock snapshot(g_font_environment_snapshot, true);
        g_font_environment[key].pending = true;
        g_font_environment[key].pending_state = prior.state;
        g_font_environment[key].pending_value = prior.value;
      }
      // No snapshot lock is held during CRT locks/callbacks. Arbitrary
      // reentrant handler WRITES remain unqualified/open; no guarantee added.
      result = _putenv_s(name, prepared ? prepared->data : "");
      {
        Lock snapshot(g_font_environment_snapshot, true);
        if (result == 0) {
          std::swap(g_font_environment[key].value, prepared);
          g_font_environment[key].owned = true;
        }
        retired_prior = std::exchange(g_font_environment[key].pending_value, nullptr);
        g_font_environment[key].pending = false;
      }
      // Failure publishes no owner. UCRT may itself partially update paired
      // tables; an Unrecorded key subsequently observes that actual OS state.
    }
  }
  Release(prepared);
  Release(retired_prior);
  return result;
}
}  // namespace gesher
