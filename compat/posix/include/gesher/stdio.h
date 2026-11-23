// Shared host/ELF stream state; FILE itself is an opaque host allocation.
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <memory>
#include <array>
#include <gesher/cancellation.h>
namespace gesher::stdio {
// Android wchar_t/mbstate_t differ from the host CRT. Keep their conversion
// state with the shared stream owner so host and guest lifecycle cannot split.
struct BionicState {
  bool conversion_error = false;
  uint32_t pushback = 0xffffffffu;
  std::array<uint8_t, 8> input_state{};
};
class StreamGuard {
 public:
  explicit StreamGuard(FILE* stream);
  ~StreamGuard();
  StreamGuard(const StreamGuard&) = delete;
  StreamGuard& operator=(const StreamGuard&) = delete;
  explicit operator bool() const { return token_ != nullptr; }
  // The acquired token pins the state for the duration of this guard.
  BionicState* Metadata() const;
 private:
  void Release();
  std::shared_ptr<void> token_;
  CancellationCleanup cleanup_;
};
void Lock(FILE* stream);
void Unlock(FILE* stream);
int TryLock(FILE* stream);
size_t Read(void* bytes, size_t size, size_t count, FILE* stream, bool unlocked = false);
size_t Write(const void* bytes, size_t size, size_t count, FILE* stream, bool unlocked = false);
char* ReadLine(char* bytes, int count, FILE* stream, bool unlocked = false);
int Get(FILE* stream, bool unlocked = false);
int Put(int value, FILE* stream, bool unlocked = false);
int Flush(FILE* stream, bool unlocked = false);
int Error(FILE* stream, bool unlocked = false);
int Eof(FILE* stream, bool unlocked = false);
void Clear(FILE* stream, bool unlocked = false);
void SetError(FILE* stream, int error);
int Orient(FILE* stream, int mode, bool unlocked = false);
int64_t Seek(FILE* stream, int64_t offset, int whence);
int64_t Tell(FILE* stream);
}
