#include "record_lock_operation.h"
#include "internal.h"
#include <cstring>

namespace gesher::record_locks {
namespace {
int WinError() { return gesher::ErrnoFromWin32(GetLastError()); }
int ReadUser(const void* source, void* destination, size_t bytes) {
  SIZE_T copied = 0;
  return source && ReadProcessMemory(GetCurrentProcess(), source, destination, bytes, &copied) && copied == bytes
    ? 0 : EFAULT;
}
int WriteUser(void* destination, const void* source, size_t bytes) {
  SIZE_T copied = 0;
  return destination && WriteProcessMemory(GetCurrentProcess(), destination, source, bytes, &copied) && copied == bytes
    ? 0 : EFAULT;
}
}  // namespace

int PrepareOperation(HANDLE anchor, int command, void* user_flock, PreparedOperation* prepared) {
  if (!prepared) return EINVAL;
  const bool query = command == 5 || command == 36;  // F_GETLK/F_OFD_GETLK
  const bool ofd = command == 36 || command == 37;
  if (!query && command != 6 && command != 37) return EINVAL;  // F_SETLK/F_OFD_SETLK
  if (!anchor || anchor == INVALID_HANDLE_VALUE) return EBADF;
  SetLastError(ERROR_SUCCESS);
  DWORD file_type = GetFileType(anchor);
  if (file_type == FILE_TYPE_UNKNOWN && GetLastError() != ERROR_SUCCESS) return WinError();
  if (file_type != FILE_TYPE_DISK) return EINVAL;  // ADR0080 regular-file scope.
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(anchor, &information)) return WinError();
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return EINVAL;

  LinuxFlock input{};
  int error = ReadUser(user_flock, &input, sizeof(input));
  if (error) return error;
  if (input.type < 0 || input.type > 2 || (query && input.type == 2) || (ofd && input.pid != 0)) return EINVAL;
  LARGE_INTEGER zero{}, current{}, size{};
  if (input.whence == 1 && !SetFilePointerEx(anchor, zero, &current, FILE_CURRENT)) return WinError();
  if (input.whence == 2 && !GetFileSizeEx(anchor, &size)) return WinError();
  File file{};
  Range range{};
  error = FileIdentity(anchor, &file);
  if (error) return error;
  error = Normalize(input.whence, input.start, input.length, current.QuadPart, size.QuadPart, &range);
  if (error) return error;
  if (!query && input.type != 2) {
    uint32_t access = 0;
    error = HandleAccess(anchor, &access);
    if (error) return error;
    if (input.type == 0 && !(access & FILE_READ_DATA)) return EBADF;
    if (input.type == 1 && !(access & (FILE_WRITE_DATA | FILE_APPEND_DATA))) return EBADF;
  }
  std::memcpy(&prepared->input, &input, sizeof(input));
  prepared->file = file;
  prepared->range = range;
  prepared->user_flock = user_flock;
  prepared->query = query;
  prepared->ofd = ofd;
  return 0;
}

int ApplyOperation(RegistryState* state, const Owner& owner,
                    const PreparedOperation& prepared, bool* mutated) {
  if (!state || !mutated || state->locks.count > kRecordCapacity) return EINVAL;
  *mutated = false;
  if (static_cast<unsigned>(owner.kind) > 1 || (owner.low == 0 && owner.high == 0) ||
      prepared.ofd != (owner.kind == Ownership::kDescription) ||
      (!prepared.ofd && (owner.low == 0 || owner.low > INT32_MAX || owner.high == 0))) return EINVAL;
  Record requested{prepared.file, owner, prepared.range, static_cast<Kind>(prepared.input.type),
                   prepared.ofd ? -1 : static_cast<int32_t>(owner.low)};

  if (prepared.query) {
    LinuxFlock output;
    std::memcpy(&output, &prepared.input, sizeof(output));  // Preserve all nonconflicting input fields/padding.
    const Record* conflict = FindConflict(state->locks, requested);
    if (!conflict) output.type = 2;
    else {
      output.type = static_cast<int16_t>(conflict->kind);
      output.whence = 0;
      output.start = static_cast<int64_t>(conflict->range.start);
      output.length = conflict->range.end == kThroughEof ? 0 :
                      static_cast<int64_t>(conflict->range.end - conflict->range.start);
      output.pid = conflict->pid;
    }
    return WriteUser(prepared.user_flock, &output, sizeof(output));
  }

  int error = Replace(&state->locks, requested, kRecordCapacity);
  if (error) return error;
  *mutated = true;
  return 0;
}

int NonblockingOperation(RegistryState* state, HANDLE anchor, const Owner& owner,
                         int command, void* user_flock, bool* mutated) {
  if (!mutated) return EINVAL;
  *mutated = false;
  PreparedOperation prepared{};
  int error = PrepareOperation(anchor, command, user_flock, &prepared);
  return error ? error : ApplyOperation(state, owner, prepared, mutated);
}

}  // namespace gesher::record_locks
