// Narrow font-key ownership. This does not replace getenv or either environ.
#pragma once
#include <optional>
#include <string_view>
namespace gesher {
struct FontEnvironmentBytes;
struct FontEnvironmentValue {
  enum class State { kAbsent, kValue, kError };
  State state = State::kError;
  FontEnvironmentValue() noexcept = default;
  // Internal adoption of one immutable-byte reference; no allocation.
  FontEnvironmentValue(State, FontEnvironmentBytes*) noexcept;
  FontEnvironmentValue(const FontEnvironmentValue&) noexcept;
  FontEnvironmentValue(FontEnvironmentValue&&) noexcept;
  FontEnvironmentValue& operator=(const FontEnvironmentValue&) noexcept;
  FontEnvironmentValue& operator=(FontEnvironmentValue&&) noexcept;
  ~FontEnvironmentValue();
  std::string_view value() const noexcept;
 private:
  FontEnvironmentBytes* bytes_ = nullptr;
};
enum class FontEnvironmentKey { kAndroidRoot, kWindowsFonts };
// Unrecorded keys read OS UTF16 explicitly; tracked successful POSIX writes
// subsequently own UTF8 bytes (including an absent tombstone after removal).
// Untracked OS/raw CRT changes cannot override a recorded owner. No generic
// environment synchronization or executable initial-envp mutation is promised.
FontEnvironmentValue ReadFontEnvironment(FontEnvironmentKey key) noexcept;
// nullopt retains the caller's original branch (unrelated/invalid input).
// Otherwise returns the raw CRT code; callers preserve their existing errno
// mapping. A skipped overwrite=0 or failed primitive never changes ownership.
// Arbitrary CRT-handler reentrant writes remain an open contract; this does
// not promise general callback/environment synchronization.
std::optional<int> WriteFontEnvironment(const char* name, const char* value,
                                      int overwrite) noexcept;
}  // namespace gesher
