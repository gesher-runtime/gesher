// CI-only temporal diagnostic. This uses the actual common duplication helper
// and raw CreateProcess inheritance, not a claim about ordered POSIX spawn.
#include "../../compat/posix/src/record_lock_duplicate.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
static_assert(sizeof(void*) == 8, "Win64-only raw inheritance diagnostic");
bool ApiFailure(const char* operation) {
  const DWORD error = GetLastError();
  std::fprintf(stderr, "CLOEXEC_PROBE_API_FAILURE operation=%s error=%lu\n", operation, error);
  return false;
}
struct Handle {
  HANDLE value = nullptr;
  Handle() = default;
  explicit Handle(HANDLE h) : value(h) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  bool Close() {
    if (!value || value == INVALID_HANDLE_VALUE) { value = nullptr; return true; }
    if (!CloseHandle(value)) return false;
    value = nullptr; return true;
  }
};
struct State {
  std::array<unsigned char, 16> nonce{};
  FILE_ID_INFO file{};
  HANDLE positive = nullptr, negative = nullptr;
  ULONGLONG deadline = 0;
  bool observed = false, leaked = false;
  DWORD before_flags = 0;
};
State* state = nullptr;  // Direct internal unit dependency; never guest/env selected.

std::string Hex(const unsigned char* bytes, size_t count) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < count; ++i) {
    out += digits[bytes[i] >> 4]; out += digits[bytes[i] & 15];
  }
  return out;
}
bool Unhex(const char* text, unsigned char* bytes, size_t count) {
  if (std::strlen(text) != 2 * count) return false;
  auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  for (size_t i = 0; i < count; ++i) {
    int high = digit(text[2 * i]), low = digit(text[2 * i + 1]);
    if (high < 0 || low < 0) return false;
    bytes[i] = static_cast<unsigned char>((high << 4) | low);
  }
  return true;
}
bool Number(const char* text, uint64_t* value) {
  if (!*text || *text == '-') return false;
  char* end = nullptr; errno = 0;
  auto result = std::strtoull(text, &end, 10);
  if (errno || !end || *end) return false;
  *value = result; return true;
}
bool SameFileBytes(HANDLE handle, const State& expected) {
  FILE_ID_INFO actual{};
  if (!GetFileInformationByHandleEx(handle, FileIdInfo, &actual, sizeof(actual)) ||
      actual.VolumeSerialNumber != expected.file.VolumeSerialNumber ||
      std::memcmp(actual.FileId.Identifier, expected.file.FileId.Identifier, 16)) return false;
  LARGE_INTEGER zero{};
  std::array<unsigned char, 16> bytes{};
  DWORD read = 0;
  return SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN) &&
      ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
      read == bytes.size() && bytes == expected.nonce;
}
int Child(int argc, char** argv) {
  if (argc != 8) return 92;
  if (_setmode(_fileno(stdout), _O_BINARY) < 0) return 94;
  uint64_t candidate = 0, positive = 0, negative = 0, volume = 0;
  State expected;
  if (!Number(argv[2], &candidate) || !Number(argv[3], &positive) || !Number(argv[4], &negative) ||
      !Unhex(argv[5], expected.nonce.data(), expected.nonce.size()) || !Number(argv[6], &volume) ||
      !Unhex(argv[7], expected.file.FileId.Identifier, 16)) return 92;
  expected.file.VolumeSerialNumber = volume;
  bool inherited = SameFileBytes(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(positive)), expected);
  bool excluded = !SameFileBytes(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(negative)), expected);
  bool leaked = SameFileBytes(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(candidate)), expected);
  std::printf("CLOEXEC_CHILD intended-inherited=%d intended-noninherited-excluded=%d candidate-visible=%d exact-file-id-and-nonce=true\n",
      inherited, excluded, leaked);
  std::fflush(stdout);
  if (!inherited || !excluded) return 94;
  return leaked ? 23 : 0;  // An actual accessible candidate is a diagnostic failure.
}

bool ObserveChild(HANDLE candidate) {
  Handle job(CreateJobObjectW(nullptr, nullptr));
  if (!job.value) return ApiFailure("CreateJobObject");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return ApiFailure("SetJobKillOnClose");
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  Handle reader, writer;
  if (!CreatePipe(&reader.value, &writer.value, &inherit, 4096) ||
      !SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0)) return ApiFailure("CreatePrivateReadPipe");
  Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &inherit, OPEN_EXISTING, 0, nullptr));
  if (input.value == INVALID_HANDLE_VALUE) return ApiFailure("OpenChildInput");
  wchar_t executable[32768]{};
  DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
  if (!length || length >= 32768) return ApiFailure("GetOwnExecutable");
  std::wstring command = L"\"" + std::wstring(executable, length) + L"\" --child";
  for (HANDLE h : {candidate, state->positive, state->negative})
    command += L" " + std::to_wstring(reinterpret_cast<uintptr_t>(h));
  const auto nonce = Hex(state->nonce.data(), state->nonce.size());
  const auto identity = Hex(state->file.FileId.Identifier, 16);
  command += L" " + std::wstring(nonce.begin(), nonce.end()) + L" " +
      std::to_wstring(state->file.VolumeSerialNumber) + L" " + std::wstring(identity.begin(), identity.end());
  SIZE_T attribute_bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
  if (!attribute_bytes) return ApiFailure("SizeJobAttributes");
  std::vector<char> attribute_storage(attribute_bytes);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes)) return ApiFailure("InitializeJobAttributes");
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job.value, sizeof(job.value), nullptr, nullptr)) {
    ApiFailure("SetAtomicJobAttribute"); DeleteProcThreadAttributeList(attributes); return false;
  }
  STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.value;
  startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = writer.value;
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION process{};
  // Deliberate raw broad-inheritance boundary. No HANDLE_LIST can hide a
  // transient inheritable alias. The child runs only after owned-job assignment.
  const bool created = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
      CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
      nullptr, nullptr, &startup.StartupInfo, &process) != FALSE;
  const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
  DeleteProcThreadAttributeList(attributes);
  if (!created) { SetLastError(create_error); return ApiFailure("CreateOwnedSuspendedChild"); }
  Handle child(process.hProcess), thread(process.hThread);
  BOOL in_job = FALSE;
  bool owned = IsProcessInJob(child.value, job.value, &in_job) && in_job;
  bool closed_writer = writer.Close();
  bool resumed = owned && closed_writer && ResumeThread(thread.value) == 1;
  ULONGLONG now = GetTickCount64();
  DWORD remaining = now < state->deadline ? static_cast<DWORD>(state->deadline - now) : 0;
  if (remaining > 5000) remaining = 5000;
  bool exited = resumed && remaining && WaitForSingleObject(child.value, remaining) == WAIT_OBJECT_0;
  if (!exited) {
    // Cleanup never converts a failed launch, resume or semantic deadline into
    // acceptance. Exact handles, not a PID lookup or global runner process kill.
    bool stopped = TerminateJobObject(job.value, 95) != FALSE;
    if (!stopped) stopped = job.Close();  // Atomic assignment plus KILL_ON_CLOSE fallback.
    bool reaped = stopped && WaitForSingleObject(child.value, 5000) == WAIT_OBJECT_0;
    std::fprintf(stderr, "CLOEXEC_CHILD_FAILURE owned=%d resumed=%d stopped=%d reaped=%d\n", owned, resumed, stopped, reaped);
    return false;
  }
  DWORD code = 0;
  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
  if (!GetExitCodeProcess(child.value, &code) ||
      !QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr))
    return ApiFailure("CompletedJobAudit");
  if (accounting.TotalProcesses != 1 || accounting.ActiveProcesses != 0) {
    std::fprintf(stderr, "CLOEXEC_CHILD_FAILURE processes=%lu active=%lu expected-one-exited-child=true\n",
        accounting.TotalProcesses, accounting.ActiveProcesses);
    return false;
  }
  std::string output;
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr)) {
      if (GetLastError() == ERROR_BROKEN_PIPE) break;
      return ApiFailure("PeekChildOutput");
    }
    if (!available) break;  // Exact child has exited; no other owned writer exists.
    char bytes[512]; DWORD read = 0;
    if (available > sizeof(bytes)) available = sizeof(bytes);
    if (!ReadFile(reader.value, bytes, available, &read, nullptr) || !read) return ApiFailure("ReadChildOutput");
    output.append(bytes, read);
    if (output.size() > 2048 || GetTickCount64() >= state->deadline) return false;
  }
  std::fputs(output.c_str(), stdout);
  const std::string prefix = "CLOEXEC_CHILD intended-inherited=1 intended-noninherited-excluded=1 candidate-visible=";
  const std::string expected = prefix + (code == 23 ? "1" : "0") + " exact-file-id-and-nonce=true\n";
  bool resources_closed = thread.Close(); resources_closed = child.Close() && resources_closed;
  resources_closed = reader.Close() && resources_closed;
  resources_closed = input.Close() && resources_closed; resources_closed = job.Close() && resources_closed;
  if ((code != 0 && code != 23) || output != expected || !resources_closed || GetTickCount64() >= state->deadline) return false;
  if ((code == 23) != ((state->before_flags & HANDLE_FLAG_INHERIT) != 0)) {
    std::fprintf(stderr, "CLOEXEC_CHILD_FAILURE native-flag-and-accessibility-disagree=true\n");
    return false;
  }
  state->leaked = code == 23; state->observed = true;
  std::printf("CLOEXEC_CHILD_AUDIT actual-exit=%lu processes=1 active=0 atomic-job-assignment=true checked-suspended-resume=true reaped=true\n", code);
  return true;
}

int AtFinalFlags(int, void* native, bool inherit) {
  HANDLE candidate = static_cast<HANDLE>(native);
  if (inherit) return EIO;
  if (!GetHandleInformation(candidate, &state->before_flags)) { ApiFailure("GetCandidateFlags"); return EIO; }
  bool observed = ObserveChild(candidate);
  bool cleared = SetHandleInformation(candidate, HANDLE_FLAG_INHERIT, 0) != FALSE;
  if (!cleared) ApiFailure("ClearFinalInheritFlag");
  return observed && cleared ? 0 : EIO;
}
}  // namespace

int main(int argc, char** argv) {
  if (!std::getenv("GITHUB_ACTIONS") || std::strcmp(std::getenv("GITHUB_ACTIONS"), "true")) return 90;
  if (argc > 1 && !std::strcmp(argv[1], "--child")) return Child(argc, argv);
  if (argc != 1) return 92;
  State observation; state = &observation;
  observation.deadline = GetTickCount64() + 10000;
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, observation.nonce.data(),
      static_cast<ULONG>(observation.nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return 95;
  wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
  DWORD size = GetTempPathW(MAX_PATH, directory);
  if (!size || size >= MAX_PATH || !GetTempFileNameW(directory, L"gcl", 0, path)) return 95;
  Handle file(CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
  if (file.value == INVALID_HANDLE_VALUE) { DeleteFileW(path); return 95; }
  DWORD written = 0;
  if (!WriteFile(file.value, observation.nonce.data(), static_cast<DWORD>(observation.nonce.size()), &written, nullptr) ||
      written != observation.nonce.size() || !GetFileInformationByHandleEx(file.value, FileIdInfo, &observation.file, sizeof(observation.file))) return 95;
  Handle positive, negative;
  if (!DuplicateHandle(GetCurrentProcess(), file.value, GetCurrentProcess(), &positive.value, 0, TRUE, DUPLICATE_SAME_ACCESS) ||
      !DuplicateHandle(GetCurrentProcess(), file.value, GetCurrentProcess(), &negative.value, 0, FALSE, DUPLICATE_SAME_ACCESS)) return 95;
  observation.positive = positive.value; observation.negative = negative.value;
  int source = _open_osfhandle(reinterpret_cast<intptr_t>(file.value), _O_RDWR | _O_BINARY | _O_NOINHERIT);
  if (source < 0) return 95;
  file.value = nullptr;  // Actual CRT now owns exactly this native handle.
  gesher::record_locks::DuplicateOperations operations{};
  operations.set_inherit = AtFinalFlags;
  int duplicate = gesher::record_locks::DuplicateAtLeast(source, 64, true, &operations);
  DWORD final_flags = HANDLE_FLAG_INHERIT;
  bool final_excluded = duplicate >= 64 && GetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(duplicate)), &final_flags) &&
      (final_flags & HANDLE_FLAG_INHERIT) == 0;
  bool duplicate_closed = duplicate < 0 || close(duplicate) == 0;
  bool source_closed = close(source) == 0;
  bool controls_closed = positive.Close(); controls_closed = negative.Close() && controls_closed;
  SetLastError(ERROR_SUCCESS);
  bool file_removed = GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
  auto private_status = gesher::record_locks::InspectPrivateReservations();
  bool clean = duplicate_closed && source_closed && controls_closed && file_removed &&
      private_status.nul == 0 && private_status.native_nul == 0 && private_status.uncertain == 0;
  std::printf("CLOEXEC_WINDOW_PROBE observed=%d transient-native-inherit=%d candidate-leaked=%d final-cloexec=%d cleanup=%d scope=raw-CreateProcess-internal-duplicate-helper ordered-spawn-unqualified\n",
      observation.observed, (observation.before_flags & HANDLE_FLAG_INHERIT) != 0, observation.leaked, final_excluded, clean);
  if (!observation.observed || !final_excluded || !clean || GetTickCount64() >= observation.deadline) return 95;
  return observation.leaked ? 1 : 0;
}
