// Shared host/guest C stdio. UCRT allocates and closes the opaque FILE token;
// all stream I/O, buffering and locking belong here and reach read/write/lseek.
// CRT read-ahead and its private append flag must not bypass OFD status.
#ifdef __USE_MINGW_ANSI_STDIO
#undef __USE_MINGW_ANSI_STDIO
#endif
#define __USE_MINGW_ANSI_STDIO 0
// This translation unit supplies the public stream symbols. MinGW's C++
// header inlines must not also define getwchar/putwchar in this owner.
#define __CRT__NO_INLINE 1

#include "stdio_internal.h"
#include "stdio_stream_buffer.h"
#include "internal.h"
#include "record_lock_descriptors.h"
#include <gesher/file_description.h>

#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <unistd.h>

#include <cerrno>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#undef getwc
#undef putwc
#undef getwchar
#undef putwchar

// These formatter cores are part of the pinned MinGW runtime already used for
// C99 printf. They emit through public fputc/fputwc, routed below. Formatting
// once also avoids double evaluation of a %n conversion.
extern "C" int __mingw_pformat(int, void*, int, const char*, va_list);
extern "C" int __mingw_wpformat(int, void*, int, const wchar_t*, va_list);
extern "C" int __real___mingw_vfscanf(FILE*, const char*, va_list);
extern "C" int __real___mingw_vfwscanf(FILE*, const wchar_t*, va_list);

namespace gesher::stdio {
namespace {
using Access = StreamBuffer::Access;
struct State {
  std::recursive_mutex mutex;
  StreamBuffer buffer;
  FILE* stream = nullptr;
  int fd = -1;
  bool live = true;
  std::atomic<bool> line_output{false};
  BionicState bionic;
  mbstate_t input_conversion{};
  mbstate_t output_conversion{};
};
using StatePtr = std::shared_ptr<State>;
// MinGW's formatter emits one character at a time. The complete formatting
// call already pins and locks its stream, so its callbacks need no registry
// lookup or recursive lock per byte. Nested formatting restores the prior
// context, and other threads never see this pointer.
thread_local State* formatting_stream = nullptr;
class FormatScope {
 public:
  explicit FormatScope(State* state) : previous_(formatting_stream), cleanup_(Restore, this) {
    formatting_stream = state;
  }
 private:
  static void Restore(void* raw) { formatting_stream = static_cast<FormatScope*>(raw)->previous_; }
  State* previous_;
  CancellationCleanup cleanup_;
};
using Streams = std::map<FILE*, StatePtr>;
struct Registry {
  std::mutex mutex;
  Streams streams;
};
INIT_ONCE once = INIT_ONCE_STATIC_INIT;
Registry* registry = nullptr;

auto Writer(int fd) { return [fd](const char* bytes, unsigned count) { return ::write(fd, bytes, count); }; }
auto Seeker(int fd) { return [fd](int64_t offset, int whence) { return static_cast<int64_t>(::lseek64(fd, offset, whence)); }; }

int WalkFlush(bool line_only, State* skip = nullptr);
auto Reader(State* state) {
  return [state](char* bytes, unsigned count) {
    if (state->buffer.Buffering() != StreamBuffer::Mode::kFull) WalkFlush(true, state);
    return ::read(state->fd, bytes, count);
  };
}
int FlushAllLocal();
void FlushAtExit() { FlushAllLocal(); }
BOOL CALLBACK CreateRegistry(PINIT_ONCE, void*, void**) {
  try {
    std::unique_ptr<Registry> candidate(new Registry);
    if (std::atexit(FlushAtExit) != 0) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    registry = candidate.release();
    return TRUE;
  } catch (const std::bad_alloc&) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
  }
}
bool Initialize() {
  if (InitOnceExecuteOnce(&once, CreateRegistry, nullptr, nullptr)) return true;
  FailWithLastError();
  return false;
}
Access StreamAccess(int flags) {
  return (flags & O_ACCMODE) == O_RDONLY ? Access::kRead :
      (flags & O_ACCMODE) == O_WRONLY ? Access::kWrite : Access::kUpdate;
}
int ConfigureDefaults(State& state, FILE* stream, int fd, int flags, bool standard = false) {
  state.buffer = StreamBuffer(StreamAccess(flags));
  state.stream = stream;
  state.fd = fd;
  state.live = true;
  state.line_output.store(false, std::memory_order_relaxed);
  state.bionic = {};
  state.input_conversion = {};
  state.output_conversion = {};
  const int saved = errno;
  auto writer = Writer(fd);
  if (standard && stream == stderr) {
    if (state.buffer.Configure(StreamBuffer::Mode::kNone, nullptr, 0, writer) < 0) return -1;
  } else if (isatty(fd)) {
    if (state.buffer.Configure(StreamBuffer::Mode::kLine, nullptr,
                               OutputBuffer::kDefaultCapacity, writer) < 0) return -1;
  }
  errno = saved;
  return 0;
}
StatePtr Find(FILE* stream, bool create) {
  if (!Initialize()) return {};
  {
    std::lock_guard<std::mutex> lock(registry->mutex);
    auto found = registry->streams.find(stream);
    if (found != registry->streams.end()) return found->second;
  }
  if (!create) return {};
  // Query the descriptor outside the registry mutex. Descriptor replacement
  // takes its transaction guard after a stream lock; the registry never waits
  // on a stream or descriptor while holding its mutex.
  int fd = _fileno(stream);
  // Buffer direction needs the actual access mode, not an observable wait
  // policy. An inherited native pipe can perform I/O even when F_GETFL cannot
  // truthfully report its externally controlled blocking mode.
  auto snapshot = CaptureDescriptor(fd);
  int flags = FileDescriptionIoFlags(snapshot);
  if (flags < 0) return {};
  try {
    auto candidate = std::make_shared<State>();
    if (ConfigureDefaults(*candidate, stream, fd, flags, true) < 0) return {};
    std::lock_guard<std::mutex> lock(registry->mutex);
    return registry->streams.emplace(stream, candidate).first->second;
  } catch (const std::bad_alloc&) {
    errno = ENOMEM;
    return {};
  }
}
class AccessStream {
 public:
  AccessStream(FILE* stream, bool unlocked, bool create = true)
      : state_(Find(stream, create)), cleanup_(Release, this) {
    if (state_ && !unlocked) lock_ = std::unique_lock<std::recursive_mutex>(state_->mutex);
    live_ = state_ && state_->live;
    if (state_ && !live_) errno = EBADF;
  }
  explicit operator bool() const { return live_; }
  State* Get() const { return state_.get(); }
  State* operator->() const { return state_.get(); }
 private:
  static void Release(void* raw) {
    auto& access = *static_cast<AccessStream*>(raw);
    if (access.live_) access.state_->line_output.store(access.state_->buffer.Writing() &&
        access.state_->buffer.Buffering() == StreamBuffer::Mode::kLine, std::memory_order_release);
    access.live_ = false;
    if (access.lock_.owns_lock()) access.lock_.unlock();
    access.state_.reset();
  }
  StatePtr state_;
  std::unique_lock<std::recursive_mutex> lock_;
  bool live_ = false;
  CancellationCleanup cleanup_;
};
std::shared_ptr<void> AcquireLocal(FILE* stream) {
  auto state = Find(stream, true);
  if (!state) return {};
  state->mutex.lock();
  if (!state->live) { state->mutex.unlock(); errno = EBADF; return {}; }
  return state;
}
void ReleaseLocal(const std::shared_ptr<void>& token) {
  if (token) static_cast<State*>(token.get())->mutex.unlock();
}
void LockLocal(FILE* stream) {
  auto state = Find(stream, true);
  if (!state) return;
  state->mutex.lock();
  if (!state->live) { state->mutex.unlock(); errno = EBADF; }
}
void UnlockLocal(FILE* stream) {
  auto state = Find(stream, false);
  if (state) state->mutex.unlock();
}
int TryLockLocal(FILE* stream) {
  auto state = Find(stream, true);
  if (!state || !state->mutex.try_lock()) return EBUSY;
  if (!state->live) { state->mutex.unlock(); errno = EBADF; return EBUSY; }
  return 0;
}
void ForgetLocal(FILE* stream) {
  auto state = Find(stream, false);
  if (!state) return;
  // Caller owns the stream lock. Flush-all pins the state, takes that same
  // lock, then checks live before touching FILE/fd. Retirement therefore does
  // not leave a stale FILE pointer in a concurrent fflush(NULL) traversal.
  state->live = false;
  std::lock_guard<std::mutex> lock(registry->mutex);
  auto found = registry->streams.find(stream);
  if (found != registry->streams.end() && found->second == state) registry->streams.erase(found);
}
int RegisterLocal(FILE* stream, int flags) {
  auto state = Find(stream, true);
  if (!state) return -1;
  std::lock_guard<std::recursive_mutex> lock(state->mutex);
  return ConfigureDefaults(*state, stream, _fileno(stream), flags);
}
FILE* OpenDescriptorLocal(int fd, const char* mode, int flags) {
  if (!Initialize()) return nullptr;
  // Allocate the state AND map node before _fdopen takes ownership. Insertion
  // of the prepared node cannot allocate, so fdopen failure never closes fd.
  Streams pending;
  try {
    auto state = std::make_shared<State>();
    if (ConfigureDefaults(*state, nullptr, fd, flags) < 0) return nullptr;
    pending.emplace(nullptr, std::move(state));
  } catch (const std::bad_alloc&) {
    return errno = ENOMEM, nullptr;
  }
  FILE* stream = _fdopen(fd, mode);
  if (!stream) return errno = gesher_errno_from_crt(errno), nullptr;
  auto node = pending.extract(pending.begin());
  node.key() = stream;
  node.mapped()->stream = stream;
  {
    std::lock_guard<std::mutex> lock(registry->mutex);
    // A raw external CRT close can leave stale metadata, but no participating
    // live stream can have the token _fdopen has just allocated.
    registry->streams.erase(stream);
    registry->streams.insert(std::move(node));
  }
  return stream;
}
int FlushLocal(FILE* stream) {
  AccessStream state(stream, true, false);
  if (!state) return 0;
  return state->buffer.FlushOutput(Writer(state->fd));
}
size_t WriteLocal(const void* bytes, size_t size, size_t count, FILE* stream, bool unlocked) {
  if (size == 0 || count == 0) return 0;
  if (formatting_stream && formatting_stream->stream == stream) {
    return formatting_stream->buffer.Write(bytes, size, count, Writer(formatting_stream->fd));
  }
  AccessStream state(stream, unlocked);
  if (!state) return 0;
  return state->buffer.Write(bytes, size, count, Writer(state->fd));
}
size_t ReadLocal(void* bytes, size_t size, size_t count, FILE* stream, bool unlocked) {
  if (size == 0 || count == 0) return 0;
  AccessStream state(stream, unlocked);
  if (!state) return 0;
  state->line_output.store(false, std::memory_order_release);
  return state->buffer.Read(bytes, size, count, Reader(state.Get()), Writer(state->fd));
}
char* LineLocal(char* bytes, int count, FILE* stream, bool unlocked) {
  if (count <= 0) std::abort();  // Bionic fgets rejects a nonpositive bound fatally.
  AccessStream state(stream, unlocked);
  if (!state) return nullptr;
  state->buffer.Orient(-1);
  state->line_output.store(false, std::memory_order_release);
  int used = 0;
  while (used < count - 1) {
    int value = state->buffer.Get(Reader(state.Get()), Writer(state->fd));
    if (value == EOF) { if (!used) return nullptr; break; }
    bytes[used++] = static_cast<char>(value);
    if (value == '\n') break;
  }
  bytes[used] = '\0';
  return bytes;
}
int UngetLocal(int character, FILE* stream) {
  AccessStream state(stream, false);
  if (!state) return EOF;
  state->line_output.store(false, std::memory_order_release);
  return state->buffer.Unget(character, Writer(state->fd));
}
int FflushLocal(FILE* stream, bool unlocked) {
  if (!stream) return FlushAllLocal();
  AccessStream state(stream, unlocked);
  return state ? state->buffer.Flush(Writer(state->fd)) : EOF;
}
int WalkFlush(bool line_only, State* skip) {
  if (!Initialize()) return EOF;
  std::vector<StatePtr> streams;
  try {
    std::lock_guard<std::mutex> lock(registry->mutex);
    streams.reserve(registry->streams.size());
    for (const auto& entry : registry->streams) streams.push_back(entry.second);
  } catch (const std::bad_alloc&) {
    return errno = ENOMEM, EOF;
  }
  CancellationCleanup pins([](void* raw) {
    std::vector<StatePtr> released;
    released.swap(*static_cast<std::vector<StatePtr>*>(raw));
  }, &streams);
  int result = 0;
  for (const auto& state : streams) {
    if (state.get() == skip || (line_only && !state->line_output.load(std::memory_order_acquire))) continue;
    std::unique_lock<std::recursive_mutex> lock(state->mutex);
    CancellationCleanup unlock([](void* raw) {
      auto& held = *static_cast<std::unique_lock<std::recursive_mutex>*>(raw);
      if (held.owns_lock()) held.unlock();
    }, &lock);
    if (state->live && (!line_only || (state->buffer.Writing() &&
        state->buffer.Buffering() == StreamBuffer::Mode::kLine)) &&
        state->buffer.FlushOutput(Writer(state->fd)) < 0) result = EOF;
  }
  return result;
}
int FlushAllLocal() { return WalkFlush(false); }
int FlagLocal(FILE* stream, bool error, bool unlocked) {
  AccessStream state(stream, unlocked, false);
  return state ? (error ? state->buffer.Error() || state->bionic.conversion_error : state->buffer.Eof()) : 0;
}
void ClearLocal(FILE* stream, bool unlocked) {
  AccessStream state(stream, unlocked, false);
  if (state) { state->buffer.Clear(); state->bionic.conversion_error = false; }
}
void ErrorLocal(FILE* stream, int error) {
  AccessStream state(stream, false);
  if (state) state->buffer.SetError(error);
}
int BufferLocal(FILE* stream, char* buffer, int mode, size_t size) {
  if ((mode != _IOFBF && mode != _IOLBF && mode != _IONBF) ||
      (mode != _IONBF && size > INT_MAX)) return -1;
  AccessStream state(stream, false);
  if (!state) return -1;
  auto buffering = mode == _IONBF ? StreamBuffer::Mode::kNone :
      mode == _IOLBF ? StreamBuffer::Mode::kLine : StreamBuffer::Mode::kFull;
  int result = state->buffer.Configure(buffering, buffer, size, Writer(state->fd));
  if (result == 0) {
    state->input_conversion = {};
    state->output_conversion = {};
    state->bionic.input_state = {};
    state->bionic.pushback = 0xffffffffu;
  }
  return result;
}
int64_t SeekLocal(FILE* stream, int64_t offset, int whence) {
  AccessStream state(stream, false);
  if (!state) return -1;
  int result = state->buffer.Seek(offset, whence, Writer(state->fd), Seeker(state->fd));
  if (result == 0) {
    state->input_conversion = {};
    state->output_conversion = {};
    state->bionic.input_state = {};
    state->bionic.pushback = 0xffffffffu;
  }
  return result;
}
int64_t TellLocal(FILE* stream) {
  AccessStream state(stream, false);
  return state ? state->buffer.Tell(Writer(state->fd), Seeker(state->fd)) : -1;
}
int OrientLocal(FILE* stream, int mode) {
  AccessStream state(stream, false, mode != 0);
  return state ? state->buffer.Orient(mode) : 0;
}
wint_t PutWideLocal(wchar_t value, FILE* stream) {
  AccessStream state(stream, false);
  if (!state) return WEOF;
  state->buffer.Orient(1);
  char encoded[MB_LEN_MAX];
  size_t count = std::wcrtomb(encoded, value, &state->output_conversion);
  if (count == static_cast<size_t>(-1)) { state->buffer.SetError(gesher_errno_from_crt(errno)); return WEOF; }
  const size_t written = state->buffer.Write(encoded, 1, count, Writer(state->fd));
  return written == count ? static_cast<wint_t>(value) : WEOF;
}
wint_t GetWideLocal(FILE* stream) {
  AccessStream state(stream, false);
  if (!state) return WEOF;
  state->buffer.Orient(1);
  state->line_output.store(false, std::memory_order_release);
  bool partial = false;
  for (;;) {
    int value = state->buffer.Get(Reader(state.Get()), Writer(state->fd));
    if (value == EOF) { if (partial) state->buffer.SetError(EILSEQ); return WEOF; }
    const char byte = static_cast<char>(value);
    wchar_t decoded = 0;
    size_t count = std::mbrtowc(&decoded, &byte, 1, &state->input_conversion);
    if (count == static_cast<size_t>(-2)) { partial = true; continue; }
    if (count == static_cast<size_t>(-1)) { state->buffer.SetError(gesher_errno_from_crt(errno)); return WEOF; }
    return static_cast<wint_t>(decoded);
  }
}
wint_t UngetWideLocal(wint_t value, FILE* stream) {
  if (value == WEOF) return WEOF;
  AccessStream state(stream, false);
  if (!state) return WEOF;
  state->buffer.Orient(1);
  state->line_output.store(false, std::memory_order_release);
  char encoded[MB_LEN_MAX];
  mbstate_t conversion{};
  size_t count = std::wcrtomb(encoded, static_cast<wchar_t>(value), &conversion);
  if (count == static_cast<size_t>(-1)) return errno = gesher_errno_from_crt(errno), WEOF;
  while (count) {
    if (state->buffer.Unget(static_cast<unsigned char>(encoded[--count]), Writer(state->fd)) == EOF) return WEOF;
  }
  state->input_conversion = {};
  return value;
}
int PrintLocal(FILE* stream, const char* format, va_list arguments) {
  AccessStream state(stream, false);
  if (!state) return -1;
  if (!state->buffer.CanWrite()) { state->buffer.SetError(EBADF); return -1; }
  state->buffer.Orient(-1);
  const uint64_t failures = state->buffer.Failures();
  FormatScope callbacks(state.Get());
  int result = __mingw_pformat(0x6000 /* TO_FILE | NOLIMIT */, stream, 0, format, arguments);
  return state->buffer.Failures() != failures ? -1 : result;
}
int PrintWideLocal(FILE* stream, const wchar_t* format, va_list arguments) {
  AccessStream state(stream, false);
  if (!state) return -1;
  if (!state->buffer.CanWrite()) { state->buffer.SetError(EBADF); return -1; }
  state->buffer.Orient(1);
  const uint64_t failures = state->buffer.Failures();
  FormatScope callbacks(state.Get());
  // MinGW's wide NOLIMIT shortcut sends literal/wide-string spans through
  // private __ms_fwprintf, bypassing this stream's buffer and OFD status.
  // A representable printf result fits INT_MAX; the bounded TO_FILE path
  // emits every character through our public fputwc callback exactly once.
  int result = __mingw_wpformat(0x2000 /* TO_FILE */, stream, INT_MAX, format, arguments);
  return state->buffer.Failures() != failures ? -1 : result;
}
int ScanLocal(FILE* stream, const char* format, va_list arguments) {
  AccessStream state(stream, false);
  if (!state) return EOF;
  state->buffer.Orient(-1);
  // The runtime parser uses public getc/ungetc. Wrapping its va_list entry
  // retains this same stream lock for the whole conversion sequence.
  return __real___mingw_vfscanf(stream, format, arguments);
}
int ScanWideLocal(FILE* stream, const wchar_t* format, va_list arguments) {
  AccessStream state(stream, false);
  if (!state) return EOF;
  state->buffer.Orient(1);
  return __real___mingw_vfwscanf(stream, format, arguments);
}
BionicState* BionicMetadataLocal(FILE* stream, bool create) {
  auto state = Find(stream, create);
  if (!state || !state->live) return nullptr;
  return &state->bionic;
}
struct Api {
  std::shared_ptr<void> (*acquire)(FILE*);
  void (*release)(const std::shared_ptr<void>&);
  void (*lock)(FILE*);
  void (*unlock)(FILE*);
  int (*try_lock)(FILE*);
  FILE* (*open_descriptor)(int, const char*, int);
  int (*register_stream)(FILE*, int);
  void (*forget)(FILE*);
  int (*flush)(FILE*);
  size_t (*write)(const void*, size_t, size_t, FILE*, bool);
  size_t (*read)(void*, size_t, size_t, FILE*, bool);
  char* (*line)(char*, int, FILE*, bool);
  int (*unget)(int, FILE*);
  int (*fflush)(FILE*, bool);
  int (*flag)(FILE*, bool, bool);
  void (*clear)(FILE*, bool);
  void (*error)(FILE*, int);
  int (*buffer)(FILE*, char*, int, size_t);
  int64_t (*seek)(FILE*, int64_t, int);
  int64_t (*tell)(FILE*);
  int (*orient)(FILE*, int);
  wint_t (*put_wide)(wchar_t, FILE*);
  wint_t (*get_wide)(FILE*);
  wint_t (*unget_wide)(wint_t, FILE*);
  int (*print)(FILE*, const char*, va_list);
  int (*print_wide)(FILE*, const wchar_t*, va_list);
  int (*scan)(FILE*, const char*, va_list);
  int (*scan_wide)(FILE*, const wchar_t*, va_list);
  BionicState* (*bionic_metadata)(FILE*, bool);
};
const Api* Owner() {
  using Factory = const Api* (*)();
  static auto factory = reinterpret_cast<Factory>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(nullptr), "gesher_stdio_output_owner_v1")));
  if (!factory) std::abort();
  return factory();
}
}  // namespace

extern "C" void gesher_stdio_output_anchor_v1() {}
extern "C" __declspec(dllexport) const Api* gesher_stdio_output_owner_v1() {
  static const Api api{AcquireLocal, ReleaseLocal, LockLocal, UnlockLocal, TryLockLocal,
      OpenDescriptorLocal, RegisterLocal, ForgetLocal, FlushLocal, WriteLocal, ReadLocal,
      LineLocal, UngetLocal, FflushLocal, FlagLocal, ClearLocal, ErrorLocal, BufferLocal,
      SeekLocal, TellLocal, OrientLocal, PutWideLocal, GetWideLocal, UngetWideLocal,
      PrintLocal, PrintWideLocal, ScanLocal, ScanWideLocal, BionicMetadataLocal};
  return &api;
}
namespace {
// Register before ordinary application constructors register their destructors
// or atexit callbacks. A lazy registration during the first fopen would flush
// too early, losing output written by an older exit handler afterwards.
__attribute__((constructor(101))) void RegisterFinalFlush() {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&gesher_stdio_output_anchor_v1), &self) ||
      self != GetModuleHandleW(nullptr)) return;
  if (!Initialize()) std::abort();
}
}
StreamGuard::StreamGuard(FILE* stream)
    : token_(Owner()->acquire(stream)), cleanup_([](void* raw) { static_cast<StreamGuard*>(raw)->Release(); }, this) {}
StreamGuard::~StreamGuard() = default;
void StreamGuard::Release() { Owner()->release(token_); token_.reset(); }
BionicState* BionicMetadata(FILE* stream, bool create) { return Owner()->bionic_metadata(stream, create); }
void Lock(FILE* stream) { Owner()->lock(stream); }
void Unlock(FILE* stream) { Owner()->unlock(stream); }
int TryLock(FILE* stream) { return Owner()->try_lock(stream); }
FILE* OpenDescriptor(int fd, const char* mode, int flags) { return Owner()->open_descriptor(fd, mode, flags); }
int Register(FILE* stream, int access) { return Owner()->register_stream(stream, access); }
void Forget(FILE* stream) { Owner()->forget(stream); }
int FlushOutput(FILE* stream) { return Owner()->flush(stream); }
int64_t Seek(FILE* stream, int64_t offset, int whence) { return Owner()->seek(stream, offset, whence); }
int64_t Tell(FILE* stream) { return Owner()->tell(stream); }
int Orient(FILE* stream, int mode) { return Owner()->orient(stream, mode); }
void SetError(FILE* stream, int error) { Owner()->error(stream, error); }
size_t Read(void* bytes, size_t size, size_t count, FILE* stream, bool unlocked) {
  return Owner()->read(bytes, size, count, stream, unlocked);
}
size_t Write(const void* bytes, size_t size, size_t count, FILE* stream, bool unlocked) {
  return Owner()->write(bytes, size, count, stream, unlocked);
}
char* ReadLine(char* bytes, int count, FILE* stream, bool unlocked) { return Owner()->line(bytes, count, stream, unlocked); }
int Get(FILE* stream, bool unlocked) {
  unsigned char value;
  return Read(&value, 1, 1, stream, unlocked) == 1 ? value : EOF;
}
int Put(int value, FILE* stream, bool unlocked) {
  const unsigned char byte = static_cast<unsigned char>(value);
  return Write(&byte, 1, 1, stream, unlocked) == 1 ? byte : EOF;
}
int Flush(FILE* stream, bool unlocked) { return Owner()->fflush(stream, unlocked); }
int Error(FILE* stream, bool unlocked) { return Owner()->flag(stream, true, unlocked); }
int Eof(FILE* stream, bool unlocked) { return Owner()->flag(stream, false, unlocked); }
void Clear(FILE* stream, bool unlocked) { Owner()->clear(stream, unlocked); }

int PrepareReopenedStream(FILE* stream, int flags) {
  const int fd = _fileno(stream);
  if ((flags & O_APPEND) != 0) {
    record_locks::DescriptorGuard descriptors;
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), HandleFromFd(fd), GetCurrentProcess(), &duplicate,
                         0, (flags & O_CLOEXEC) == 0, DUPLICATE_SAME_ACCESS)) return FailWithLastError();
    int temporary = _open_osfhandle(reinterpret_cast<intptr_t>(duplicate), _O_BINARY |
                                     ((flags & O_CLOEXEC) != 0 ? _O_NOINHERIT : 0));
    if (temporary < 0) {
      int error = gesher_errno_from_crt(errno);
      CloseHandle(duplicate);
      return errno = error, -1;
    }
    int result = _dup2(temporary, fd);
    int error = result < 0 ? gesher_errno_from_crt(errno) : 0;
    _close(temporary);
    if (error) return errno = error, -1;
    if (!SetHandleInformation(HandleFromFd(fd), HANDLE_FLAG_INHERIT,
                              (flags & O_CLOEXEC) != 0 ? 0 : HANDLE_FLAG_INHERIT)) return FailWithLastError();
  }
  return Register(stream, flags);
}
}  // namespace gesher::stdio

size_t fwrite(const void* data, size_t size, size_t count, FILE* stream) { return gesher::stdio::Write(data, size, count, stream); }
size_t fread(void* data, size_t size, size_t count, FILE* stream) { return gesher::stdio::Read(data, size, count, stream); }
int fputc(int value, FILE* stream) { return gesher::stdio::Put(value, stream); }
int putc(int value, FILE* stream) { return fputc(value, stream); }
int putchar(int value) { return fputc(value, stdout); }
int fgetc(FILE* stream) { return gesher::stdio::Get(stream); }
int getc(FILE* stream) { return fgetc(stream); }
int getchar() { return fgetc(stdin); }
char* fgets(char* bytes, int count, FILE* stream) { return gesher::stdio::ReadLine(bytes, count, stream); }
int ungetc(int value, FILE* stream) { return gesher::stdio::Owner()->unget(value, stream); }
int fputs(const char* text, FILE* stream) {
  size_t length = std::strlen(text);
  return fwrite(text, 1, length, stream) == length ? 0 : EOF;
}
int puts(const char* text) {
  gesher::stdio::StreamGuard lock(stdout);
  if (!lock) return EOF;
  return fputs(text, stdout) != EOF && fputc('\n', stdout) != EOF ? 0 : EOF;
}
int fflush(FILE* stream) { return gesher::stdio::Flush(stream); }
int ferror(FILE* stream) { return gesher::stdio::Error(stream); }
int feof(FILE* stream) { return gesher::stdio::Eof(stream); }
void clearerr(FILE* stream) { gesher::stdio::Clear(stream); }
int setvbuf(FILE* stream, char* buffer, int mode, size_t size) { return gesher::stdio::Owner()->buffer(stream, buffer, mode, size); }
void setbuf(FILE* stream, char* buffer) { setvbuf(stream, buffer, buffer ? _IOFBF : _IONBF, BUFSIZ); }
int fseek(FILE* stream, long offset, int whence) { return static_cast<int>(gesher::stdio::Seek(stream, offset, whence)); }
long ftell(FILE* stream) {
  int64_t position = gesher::stdio::Tell(stream);
  if (position > LONG_MAX) return errno = EOVERFLOW, -1;
  return static_cast<long>(position);
}
int fgetpos(FILE* stream, fpos_t* position) {
  int64_t value = gesher::stdio::Tell(stream);
  if (value == -1) return -1;
  *position = static_cast<fpos_t>(value);
  return 0;
}
int fsetpos(FILE* stream, const fpos_t* position) { return static_cast<int>(gesher::stdio::Seek(stream, *position, SEEK_SET)); }
int fgetpos64(FILE* stream, fpos_t* position) { return fgetpos(stream, position); }
int fsetpos64(FILE* stream, const fpos_t* position) { return fsetpos(stream, position); }
void rewind(FILE* stream) { gesher::stdio::Seek(stream, 0, SEEK_SET); clearerr(stream); }
void perror(const char* message) {
  const int saved = errno;
  if (message && *message) fprintf(stderr, "%s: %s\n", message, strerror(saved));
  else fprintf(stderr, "%s\n", strerror(saved));
}
int vfprintf(FILE* stream, const char* format, va_list arguments) { return gesher::stdio::Owner()->print(stream, format, arguments); }
int fprintf(FILE* stream, const char* format, ...) {
  va_list arguments; va_start(arguments, format);
  int result = vfprintf(stream, format, arguments);
  va_end(arguments); return result;
}
int vprintf(const char* format, va_list arguments) { return vfprintf(stdout, format, arguments); }
int printf(const char* format, ...) {
  va_list arguments; va_start(arguments, format);
  int result = vfprintf(stdout, format, arguments);
  va_end(arguments); return result;
}
wint_t fputwc(wchar_t value, FILE* stream) { return gesher::stdio::Owner()->put_wide(value, stream); }
wint_t putwc(wchar_t value, FILE* stream) { return fputwc(value, stream); }
wint_t putwchar(wchar_t value) { return fputwc(value, stdout); }
wint_t fgetwc(FILE* stream) { return gesher::stdio::Owner()->get_wide(stream); }
wint_t getwc(FILE* stream) { return fgetwc(stream); }
wint_t getwchar() { return fgetwc(stdin); }
wint_t ungetwc(wint_t value, FILE* stream) { return gesher::stdio::Owner()->unget_wide(value, stream); }
int fputws(const wchar_t* text, FILE* stream) {
  gesher::stdio::StreamGuard lock(stream);
  if (!lock) return -1;
  gesher::stdio::Orient(stream, 1);
  while (*text) if (fputwc(*text++, stream) == WEOF) return -1;
  return 0;
}
wchar_t* fgetws(wchar_t* text, int count, FILE* stream) {
  if (count <= 0) return errno = EINVAL, nullptr;
  gesher::stdio::StreamGuard lock(stream);
  if (!lock) return nullptr;
  int used = 0;
  while (used < count - 1) {
    wint_t value = fgetwc(stream);
    if (value == WEOF) { if (!used) return nullptr; break; }
    text[used++] = static_cast<wchar_t>(value);
    if (value == L'\n') break;
  }
  text[used] = 0;
  return text;
}
int fwide(FILE* stream, int mode) { return gesher::stdio::Orient(stream, mode); }
int vfwprintf(FILE* stream, const wchar_t* format, va_list arguments) { return gesher::stdio::Owner()->print_wide(stream, format, arguments); }
int fwprintf(FILE* stream, const wchar_t* format, ...) {
  va_list arguments; va_start(arguments, format);
  int result = vfwprintf(stream, format, arguments);
  va_end(arguments); return result;
}
int vwprintf(const wchar_t* format, va_list arguments) { return vfwprintf(stdout, format, arguments); }
int wprintf(const wchar_t* format, ...) {
  va_list arguments; va_start(arguments, format);
  int result = vfwprintf(stdout, format, arguments);
  va_end(arguments); return result;
}

// GNU-mode call sites use these assembly aliases instead of the public names.
extern "C" int __mingw_vfprintf(FILE* stream, const char* format, va_list ap) { return vfprintf(stream, format, ap); }
extern "C" int __mingw_fprintf(FILE* stream, const char* format, ...) {
  va_list ap; va_start(ap, format); int result = vfprintf(stream, format, ap); va_end(ap); return result;
}
extern "C" int __mingw_vprintf(const char* format, va_list ap) { return vfprintf(stdout, format, ap); }
extern "C" int __mingw_printf(const char* format, ...) {
  va_list ap; va_start(ap, format); int result = vfprintf(stdout, format, ap); va_end(ap); return result;
}
extern "C" int __mingw_vfwprintf(FILE* stream, const wchar_t* format, va_list ap) { return vfwprintf(stream, format, ap); }
extern "C" int __mingw_fwprintf(FILE* stream, const wchar_t* format, ...) {
  va_list ap; va_start(ap, format); int result = vfwprintf(stream, format, ap); va_end(ap); return result;
}
extern "C" int __mingw_vwprintf(const wchar_t* format, va_list ap) { return vfwprintf(stdout, format, ap); }
extern "C" int __mingw_wprintf(const wchar_t* format, ...) {
  va_list ap; va_start(ap, format); int result = vfwprintf(stdout, format, ap); va_end(ap); return result;
}
int vfscanf(FILE* stream, const char* format, va_list ap) { return gesher::stdio::Owner()->scan(stream, format, ap); }
int fscanf(FILE* stream, const char* format, ...) {
  va_list ap; va_start(ap, format); int result = vfscanf(stream, format, ap); va_end(ap); return result;
}
int vscanf(const char* format, va_list ap) { return vfscanf(stdin, format, ap); }
int scanf(const char* format, ...) {
  va_list ap; va_start(ap, format); int result = vfscanf(stdin, format, ap); va_end(ap); return result;
}
void flockfile(FILE* stream) { gesher::stdio::Lock(stream); }
void funlockfile(FILE* stream) { gesher::stdio::Unlock(stream); }
int ftrylockfile(FILE* stream) { return gesher::stdio::TryLock(stream); }
size_t fread_unlocked(void* bytes, size_t size, size_t count, FILE* stream) { return gesher::stdio::Read(bytes, size, count, stream, true); }
size_t fwrite_unlocked(const void* bytes, size_t size, size_t count, FILE* stream) { return gesher::stdio::Write(bytes, size, count, stream, true); }
int fgetc_unlocked(FILE* stream) { return gesher::stdio::Get(stream, true); }
int getc_unlocked(FILE* stream) { return gesher::stdio::Get(stream, true); }
int getchar_unlocked() { return gesher::stdio::Get(stdin, true); }
int fputc_unlocked(int value, FILE* stream) { return gesher::stdio::Put(value, stream, true); }
int putc_unlocked(int value, FILE* stream) { return gesher::stdio::Put(value, stream, true); }
int putchar_unlocked(int value) { return gesher::stdio::Put(value, stdout, true); }
char* fgets_unlocked(char* bytes, int count, FILE* stream) { return gesher::stdio::ReadLine(bytes, count, stream, true); }
int fputs_unlocked(const char* text, FILE* stream) {
  size_t length = std::strlen(text);
  return gesher::stdio::Write(text, 1, length, stream, true) == length ? 0 : EOF;
}
int fflush_unlocked(FILE* stream) { return gesher::stdio::Flush(stream, true); }
int ferror_unlocked(FILE* stream) { return gesher::stdio::Error(stream, true); }
int feof_unlocked(FILE* stream) { return gesher::stdio::Eof(stream, true); }
void clearerr_unlocked(FILE* stream) { gesher::stdio::Clear(stream, true); }
int vfwscanf(FILE* stream, const wchar_t* format, va_list ap) { return gesher::stdio::Owner()->scan_wide(stream, format, ap); }
int fwscanf(FILE* stream, const wchar_t* format, ...) {
  va_list ap; va_start(ap, format); int result = vfwscanf(stream, format, ap); va_end(ap); return result;
}
int vwscanf(const wchar_t* format, va_list ap) { return vfwscanf(stdin, format, ap); }
int wscanf(const wchar_t* format, ...) {
  va_list ap; va_start(ap, format); int result = vfwscanf(stdin, format, ap); va_end(ap); return result;
}
extern "C" int __wrap___mingw_vfscanf(FILE* stream, const char* format, va_list ap) { return vfscanf(stream, format, ap); }
extern "C" int __wrap___mingw_vfwscanf(FILE* stream, const wchar_t* format, va_list ap) { return vfwscanf(stream, format, ap); }

// MinGW's 64-bit aliases are dllimport declarations. Provide both forms so
// host callers and the ELF adapter use the same positioning barrier.
extern "C" int gesher_fseeko64(FILE* stream, long long offset, int whence) __asm__("fseeko64");
extern "C" int gesher_fseeko64(FILE* stream, long long offset, int whence) { return static_cast<int>(gesher::stdio::Seek(stream, offset, whence)); }
extern "C" long long gesher_ftello64(FILE* stream) __asm__("ftello64");
extern "C" long long gesher_ftello64(FILE* stream) { return gesher::stdio::Tell(stream); }
extern "C" {
int (*__imp_fseeko64)(FILE*, long long, int) = gesher_fseeko64;
long long (*__imp_ftello64)(FILE*) = gesher_ftello64;
}
