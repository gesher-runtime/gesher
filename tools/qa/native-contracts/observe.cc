// Standalone Windows observations for PR73/PR100 design work.
// This executable does not link Gesher and cannot qualify its production backend.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
  ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  bool valid() const { return value != INVALID_HANDLE_VALUE && value != nullptr; }
  HANDLE Release() { HANDLE h = value; value = INVALID_HANDLE_VALUE; return h; }
  void Reset(HANDLE h) { if (valid()) CloseHandle(value); value = h; }
};

using NtOpen = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                              PIO_STATUS_BLOCK, ULONG, ULONG);

bool PipeObservation(int topology, int method, int endpoint, DWORD data_access, unsigned serial) {
  wchar_t name[160];
  std::swprintf(name, 160, L"\\\\.\\pipe\\gesher-qa-reopen-%lu-%u", GetCurrentProcessId(), serial);
  Handle server(CreateNamedPipeW(name,
      (topology ? PIPE_ACCESS_DUPLEX : PIPE_ACCESS_INBOUND) | FILE_FLAG_FIRST_PIPE_INSTANCE,
      (topology ? PIPE_TYPE_MESSAGE : PIPE_TYPE_BYTE) | PIPE_READMODE_BYTE |
      PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr));
  if (!server.valid()) return false;
  Handle client(CreateFileW(name,
      (topology ? GENERIC_READ | GENERIC_WRITE : GENERIC_WRITE) | FILE_WRITE_ATTRIBUTES,
      0, nullptr, OPEN_EXISTING, 0, nullptr));
  if (!client.valid()) return false;
  if (!ConnectNamedPipe(server.value, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return false;
  DWORD nowait = PIPE_NOWAIT;
  if (!SetNamedPipeHandleState(server.value, &nowait, nullptr, nullptr) ||
      !SetNamedPipeHandleState(client.value, &nowait, nullptr, nullptr)) return false;
  if (topology) {
    // Exact PR73 publication boundary: directional duplicates of the duplex
    // message pipe, with both full-access creator handles retired.
    Handle read_end, write_end;
    const DWORD metadata = FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;
    if (!DuplicateHandle(GetCurrentProcess(), server.value, GetCurrentProcess(), &read_end.value,
                         FILE_READ_DATA | metadata, FALSE, 0) ||
        !DuplicateHandle(GetCurrentProcess(), client.value, GetCurrentProcess(), &write_end.value,
                         FILE_WRITE_DATA | metadata, FALSE, 0)) return false;
    server.Reset(read_end.Release());
    client.Reset(write_end.Release());
  }
  HANDLE source = endpoint == 0 ? server.value : client.value;
  HANDLE result = INVALID_HANDLE_VALUE;
  DWORD error = 0;
  NTSTATUS status = 0;
  const DWORD desired = data_access | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;
  if (method == 0) {
    result = ReOpenFile(source, desired, FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
    if (result == INVALID_HANDLE_VALUE) error = GetLastError();
  } else {
    auto open = reinterpret_cast<NtOpen>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtOpenFile")));
    if (!open) return false;
    wchar_t empty[] = L"";
    UNICODE_STRING relative{};
    relative.Buffer = empty;
    relative.MaximumLength = sizeof(empty);
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = source;
    attributes.ObjectName = &relative;
    attributes.Attributes = 0x40;  // OBJ_CASE_INSENSITIVE
    IO_STATUS_BLOCK io{};
    // FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE; existing object only.
    status = open(&result, desired, &attributes, &io,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, 0x20 | 0x40);
    if (status < 0) result = INVALID_HANDLE_VALUE;
  }
  Handle reopened(result);
  DWORD wait_mode = PIPE_WAIT, original_mode = 0;
  bool changed = reopened.valid() &&
      SetNamedPipeHandleState(reopened.value, &wait_mode, nullptr, nullptr);
  bool queried = GetNamedPipeHandleStateW(source, &original_mode, nullptr, nullptr,
                                         nullptr, nullptr, 0) != FALSE;
  bool changed_nowait = reopened.valid() &&
      SetNamedPipeHandleState(reopened.value, &nowait, nullptr, nullptr);
  bool reads_queue = false, writes_queue = false;
  if (changed_nowait) {
    DWORD count = 0;
    char byte = 0;
    if ((data_access & FILE_READ_DATA) != 0) {
      reads_queue = WriteFile(client.value, "r", 1, &count, nullptr) && count == 1 &&
          ReadFile(reopened.value, &byte, 1, &count, nullptr) && count == 1 && byte == 'r';
      if (!reads_queue) ReadFile(server.value, &byte, 1, &count, nullptr);
    }
    if ((data_access & FILE_WRITE_DATA) != 0) {
      writes_queue = WriteFile(reopened.value, "w", 1, &count, nullptr) && count == 1 &&
          ReadFile(server.value, &byte, 1, &count, nullptr) && count == 1 && byte == 'w';
    }
  }
  std::printf("PIPE_REOPEN topology=%s method=%s endpoint=%d access=%lu opened=%d win32_error=%lu "
      "ntstatus=%08lx set_wait=%d original_query=%d original_nowait=%d "
      "set_nowait=%d reads_original_queue=%d writes_original_queue=%d\n",
      topology ? "pr73-directional-message-duplex" : "inbound-byte-server",
      method == 0 ? "ReOpenFile" : "NtOpenFile-relative-empty", endpoint, data_access,
      reopened.valid(), error, static_cast<unsigned long>(status), changed, queried,
      (original_mode & PIPE_NOWAIT) != 0, changed_nowait, reads_queue, writes_queue);
  return true;
}

struct ExtendedNotification {
  DWORD next, action;
  LARGE_INTEGER creation_time, modification_time, change_time, access_time;
  LARGE_INTEGER allocated_length, file_size;
  DWORD attributes, reparse_tag;
  LARGE_INTEGER file_id, parent_file_id;
  DWORD name_length;
  WCHAR name[1];
};
using ExtendedRead = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, BOOL, DWORD, LPDWORD,
                                  LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE, int);

struct DirectoryWatch {
  Handle directory, event;
  OVERLAPPED operation{};
  std::vector<DWORD> buffer = std::vector<DWORD>(16384);
  ExtendedRead read = nullptr;
  bool pending = false;
  explicit DirectoryWatch(const std::wstring& path)
      : directory(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr)),
        event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    read = reinterpret_cast<ExtendedRead>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ReadDirectoryChangesExW")));
  }
  ~DirectoryWatch() {
    if (pending) {
      CancelIoEx(directory.value, &operation);
      DWORD count = 0;
      GetOverlappedResult(directory.value, &operation, &count, TRUE);
    }
  }
  bool Issue() {
    if (!read || !directory.valid() || !event.valid() || pending) return false;
    operation = {};
    operation.hEvent = event.value;
    ResetEvent(event.value);
    DWORD unused = 0;
    pending = read(directory.value, buffer.data(), 65536, FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_ATTRIBUTES, &unused, &operation, nullptr, 2) != FALSE;
    return pending;
  }
};

void Append(std::string& result, char event) {
  if (result.empty() || result.back() != event) result.push_back(event);
}

bool ObserveHistory(unsigned serial, int history, int stamp_mode) {
  wchar_t temporary[MAX_PATH];
  DWORD length = GetTempPathW(MAX_PATH, temporary);
  if (!length || length >= MAX_PATH) return false;
  wchar_t leaf[120];
  std::swprintf(leaf, 120, L"gesher-qa-notify-%lu-%llu-%u", GetCurrentProcessId(),
                static_cast<unsigned long long>(GetTickCount64()), serial);
  const std::wstring directory = std::wstring(temporary) + leaf;
  if (!CreateDirectoryW(directory.c_str(), nullptr)) return false;
  bool okay = true;
  std::string events;
  unsigned batches = 0, records = 0;
  {
    const std::wstring first = directory + L"\\A";
    Handle writer(CreateFileW(first.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!writer.valid()) okay = false;
    char original[4096]{};
    DWORD count = 0;
    if (okay) okay = WriteFile(writer.value, original, sizeof(original), &count, nullptr) &&
                       count == sizeof(original);
    FILETIME fixed_time{};
    if (okay) okay = GetFileTime(writer.value, nullptr, nullptr, &fixed_time) != FALSE;
    DirectoryWatch watch(directory);
    if (okay) okay = watch.Issue();
    std::wstring name = L"A", tracked = L"A";
    bool renaming = false;
    auto rename = [&](const wchar_t* next) {
      if (!okay) return;
      const std::wstring from = directory + L"\\" + name;
      const std::wstring to = directory + L"\\" + next;
      okay = MoveFileExW(from.c_str(), to.c_str(), 0) != FALSE;
      if (okay) name = next;
    };
    auto write = [&] {
      if (!okay) return;
      LARGE_INTEGER start{};
      okay = SetFilePointerEx(writer.value, start, nullptr, FILE_BEGIN) &&
          WriteFile(writer.value, "x", 1, &count, nullptr) && count == 1;
      if (okay && stamp_mode != 0) {
        FILETIME value = fixed_time;
        if (stamp_mode == 1) GetSystemTimePreciseAsFileTime(&value);
        okay = SetFileTime(writer.value, nullptr, nullptr, &value) != FALSE;
      }
    };
    if (history == 0) { rename(L"B"); write(); rename(L"A"); write(); }
    if (history == 1) { rename(L"B"); write(); rename(L"C"); write(); rename(L"B"); write(); }
    if (history == 2) { rename(L"B"); rename(L"A"); write(); }
    if (history == 3) { write(); write(); }
    // Intentionally collect only after the whole sequence. Later changes must
    // cross the native buffer between successive reads, not just the first IRP.
    const ULONGLONG deadline = GetTickCount64() + 4000;
    while (okay && GetTickCount64() < deadline) {
      DWORD waited = WaitForSingleObject(watch.event.value, 200);
      if (waited == WAIT_TIMEOUT) break;
      if (waited != WAIT_OBJECT_0) { okay = false; break; }
      DWORD bytes = 0;
      if (!GetOverlappedResult(watch.directory.value, &watch.operation, &bytes, FALSE)) {
        okay = false; break;
      }
      watch.pending = false;
      ++batches;
      if (!bytes) { std::printf("NOTIFY_OVERFLOW serial=%u\n", serial); okay = false; break; }
      size_t offset = 0;
      const auto* data = reinterpret_cast<const unsigned char*>(watch.buffer.data());
      while (offset < bytes) {
        constexpr size_t header = offsetof(ExtendedNotification, name);
        if (bytes - offset < header) { okay = false; break; }
        const auto* entry = reinterpret_cast<const ExtendedNotification*>(data + offset);
        if (entry->name_length % sizeof(wchar_t) || entry->name_length > bytes - offset - header) {
          okay = false; break;
        }
        const std::wstring current(entry->name, entry->name_length / sizeof(wchar_t));
        std::printf("NOTIFY_RECORD serial=%u batch=%u action=%lu name=%ls "
            "file_id=%lld modification=%lld change=%lld size=%lld\n", serial, batches,
            entry->action, current.c_str(), entry->file_id.QuadPart,
            entry->modification_time.QuadPart, entry->change_time.QuadPart,
            entry->file_size.QuadPart);
        ++records;
        if (entry->action == FILE_ACTION_RENAMED_OLD_NAME && current == tracked) {
          Append(events, 'M'); renaming = true;
        } else if (entry->action == FILE_ACTION_RENAMED_NEW_NAME && renaming) {
          tracked = current; renaming = false;
        } else if (entry->action == FILE_ACTION_MODIFIED && current == tracked) {
          Append(events, 'W');
        }
        if (!entry->next) break;
        if (entry->next < header || entry->next > bytes - offset || entry->next % 4) {
          okay = false; break;
        }
        offset += entry->next;
      }
      if (okay) okay = watch.Issue();
    }
    const char* expected[] = {"MWMW", "MWMWMW", "MW", "W"};
    const char* modes[] = {"native-write-only", "explicit-current-write-time", "explicit-unchanged-time-control"};
    std::printf("NOTIFY_HISTORY serial=%u history=%d mode=%s writer_still_open=1 "
        "expected=%s observed=%s batches=%u records=%u setup_ok=%d "
        "raw_action_order_matches=%d production_attribute_classification_checked=0\n",
        serial, history, modes[stamp_mode], expected[history], events.c_str(), batches,
        records, okay, events == expected[history]);
  }
  // These are the only names created by this isolated fixture.
  for (const wchar_t* name : {L"A", L"B", L"C"}) {
    const std::wstring file = directory + L"\\" + name;
    if (!DeleteFileW(file.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) okay = false;
  }
  if (!RemoveDirectoryW(directory.c_str())) okay = false;
  return okay;
}
}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::puts("DIAGNOSTIC_ONLY production_backend_validated=0 android_contract_approved=0");
  bool setup_ok = true;
  unsigned serial = 0;
  for (int topology = 0; topology != 2; ++topology)
    for (int method = 0; method != 2; ++method)
      for (int endpoint = 0; endpoint != 2; ++endpoint)
        for (DWORD access : {DWORD(FILE_READ_DATA), DWORD(FILE_WRITE_DATA), DWORD(FILE_READ_DATA | FILE_WRITE_DATA)})
          setup_ok = PipeObservation(topology, method, endpoint, access, ++serial) && setup_ok;
  for (int mode = 0; mode != 3; ++mode)
    for (int history = 0; history != 4; ++history)
      setup_ok = ObserveHistory(++serial, history, mode) && setup_ok;
  std::printf("DIAGNOSTIC_COMPLETED setup_ok=%d observations=%u production_backend_validated=0\n",
              setup_ok, serial);
  return setup_ok ? 0 : 1;
}
